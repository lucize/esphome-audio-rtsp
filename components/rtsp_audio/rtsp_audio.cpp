#include "rtsp_audio.h"

#include <algorithm>
#include <cinttypes>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <vector>

#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/tcp.h"

namespace esphome {
namespace rtsp_audio {

static const char *const TAG = "rtsp_audio";

static uint32_t now_ms() { return (uint32_t) (esp_timer_get_time() / 1000ULL); }

static std::string header_value(const std::string &request, const char *name) {
  std::string needle = std::string(name) + ":";
  std::string lower_req = request;
  std::string lower_needle = needle;
  std::transform(lower_req.begin(), lower_req.end(), lower_req.begin(), ::tolower);
  std::transform(lower_needle.begin(), lower_needle.end(), lower_needle.begin(), ::tolower);
  size_t pos = lower_req.find(lower_needle);
  if (pos == std::string::npos) return "";
  pos += needle.size();
  while (pos < request.size() && (request[pos] == ' ' || request[pos] == '\t')) pos++;
  size_t end = request.find("\r\n", pos);
  if (end == std::string::npos) end = request.find('\n', pos);
  if (end == std::string::npos) end = request.size();
  return request.substr(pos, end - pos);
}

void RTSPAudioComponent::setup() {
  this->setup_attempts_++;
  this->running_ = true;
  ESP_LOGI(TAG, "Native RTSP audio (microphone-source) setup attempt %" PRIu32, this->setup_attempts_);

  if (this->auth_enabled_) {
    this->auth_token_ = base64_encode_(this->auth_username_ + ":" + this->auth_password_);
    ESP_LOGI(TAG, "RTSP Basic authentication enabled (realm: %s)", this->auth_realm_.c_str());
  }

  if (this->mic_ == nullptr) {
    this->last_error_ = "no microphone: entity configured";
    ESP_LOGE(TAG, "%s", this->last_error_.c_str());
    return;
  }

  // Always request 16-bit from MicrophoneSource: that's exactly what RTP L16
  // needs, and it means MicrophoneSource does the 24/32-bit -> 16-bit
  // conversion for us instead of us hand-rolling a bit-shift (which is what
  // the old direct-I2S version of this component had to do itself).
  this->mic_source_ = new microphone::MicrophoneSource(this->mic_, /*bits_per_sample=*/16, this->gain_factor_,
                                                         /*passive=*/false);
  this->mic_source_->add_channel(this->channel_);

  audio::AudioStreamInfo info = this->mic_source_->get_audio_stream_info();
  this->sample_rate_ = info.get_sample_rate();

  // Small buffer between the microphone callback and RTP sender. Keeping this
  // low avoids building up noticeable latency; the trigger level is one RTP
  // packet so the sender wakes for whole packets instead of tiny partial chunks.
  const size_t packet_bytes = std::max<size_t>(160, ((size_t) this->sample_rate_ * (size_t) this->packet_ms_ / 1000U) * 2U);
  size_t buffer_bytes = ((size_t) this->sample_rate_ * (size_t) this->buffer_ms_ / 1000U) * 2U;
  if (buffer_bytes < packet_bytes * 3U) buffer_bytes = packet_bytes * 3U;
  if (buffer_bytes < 4096) buffer_bytes = 4096;
  this->audio_buffer_ = xStreamBufferCreate(buffer_bytes, packet_bytes);
  if (this->audio_buffer_ == nullptr) {
    this->last_error_ = "failed to allocate audio stream buffer";
    ESP_LOGE(TAG, "%s", this->last_error_.c_str());
    return;
  }

  this->mic_source_->add_data_callback([this](const std::vector<uint8_t> &data) {
    if (this->audio_buffer_ != nullptr) {
      // Zero timeout: never block the microphone's own task. If the RTP
      // sender has fallen behind and the buffer is full, this just sends as
      // many bytes as currently fit and silently drops the rest.
      size_t written = xStreamBufferSend(this->audio_buffer_, data.data(), data.size(), 0);
      if (written < data.size()) this->dropped_bytes_ += (uint32_t) (data.size() - written);
    }
  });

  if (!this->start_server_task_()) {
    ESP_LOGE(TAG, "RTSP server startup failed: %s", this->last_error_.c_str());
    return;
  }
  this->started_ = true;
  this->last_error_ = "none";
  ESP_LOGI(TAG, "Native RTSP audio server task started on port %d (microphone: %u Hz)", this->port_,
           (unsigned) this->sample_rate_);
  ESP_LOGI(TAG, "RTSP URL: rtsp://%s:%d/", this->local_ip_().c_str(), this->port_);
}

bool RTSPAudioComponent::start_server_task_() {
  if (this->server_task_handle_ != nullptr) return true;
  BaseType_t ok = xTaskCreatePinnedToCore(&RTSPAudioComponent::server_task_trampoline_, "rtsp_audio_srv", 8192, this, 5,
                                          &this->server_task_handle_, 0);
  if (ok != pdPASS) {
    this->last_error_ = "xTaskCreate server failed";
    return false;
  }
  return true;
}

void RTSPAudioComponent::server_task_trampoline_(void *arg) {
  auto *self = static_cast<RTSPAudioComponent *>(arg);
  self->server_task_();
  self->server_task_handle_ = nullptr;
  vTaskDelete(nullptr);
}

void RTSPAudioComponent::rtp_task_trampoline_(void *arg) {
  auto *self = static_cast<RTSPAudioComponent *>(arg);
  self->rtp_task_();
  self->rtp_task_handle_ = nullptr;
  vTaskDelete(nullptr);
}

int RTSPAudioComponent::create_tcp_server_() {
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (fd < 0) {
    this->last_error_ = std::string("socket failed: ") + strerror(errno);
    return -1;
  }
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  ::sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(this->port_);
  if (bind(fd, (::sockaddr *) &addr, sizeof(addr)) < 0) {
    this->last_error_ = std::string("bind failed: ") + strerror(errno);
    close(fd);
    return -1;
  }
  if (listen(fd, 1) < 0) {
    this->last_error_ = std::string("listen failed: ") + strerror(errno);
    close(fd);
    return -1;
  }
  return fd;
}

void RTSPAudioComponent::server_task_() {
  while (this->running_) {
    this->server_fd_ = this->create_tcp_server_();
    if (this->server_fd_ < 0) {
      ESP_LOGE(TAG, "RTSP listen setup failed: %s; retrying", this->last_error_.c_str());
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    ESP_LOGI(TAG, "RTSP TCP control listening on 0.0.0.0:%d", this->port_);

    while (this->running_) {
      ::sockaddr_in peer = {};
      socklen_t len = sizeof(peer);
      int cfd = accept(this->server_fd_, (::sockaddr *) &peer, &len);
      if (cfd < 0) {
        if (this->running_) ESP_LOGW(TAG, "accept failed: %s", strerror(errno));
        continue;
      }
      char peer_ip[INET_ADDRSTRLEN] = {0};
      inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
      int nodelay = 1;
      setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
      ESP_LOGI(TAG, "RTSP client connected from %s:%u", peer_ip, ntohs(peer.sin_port));
      this->client_fd_ = cfd;
      this->client_connected_ = true;
      this->handle_rtsp_client_(cfd);
      this->streaming_ = false;
      this->client_connected_ = false;
      this->client_fd_ = -1;
      this->close_rtp_sockets_();
      close(cfd);
      ESP_LOGI(TAG, "RTSP client disconnected");
    }

    close(this->server_fd_);
    this->server_fd_ = -1;
  }
}

bool RTSPAudioComponent::read_rtsp_request_(int fd, std::string &request) {
  request.clear();
  char buf[512];
  while (this->running_) {
    int n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) return false;
    request.append(buf, n);
    if (request.find("\r\n\r\n") != std::string::npos || request.find("\n\n") != std::string::npos) return true;
    if (request.size() > 4096) return false;
  }
  return false;
}

int RTSPAudioComponent::parse_cseq_(const std::string &request) const {
  std::string v = header_value(request, "CSeq");
  if (v.empty()) return 1;
  return atoi(v.c_str());
}

bool RTSPAudioComponent::parse_client_ports_(const std::string &request, int *rtp_port, int *rtcp_port) const {
  std::string transport = header_value(request, "Transport");
  size_t p = transport.find("client_port=");
  if (p == std::string::npos) return false;
  p += strlen("client_port=");
  int a = atoi(transport.c_str() + p);
  size_t dash = transport.find('-', p);
  int b = (dash == std::string::npos) ? (a + 1) : atoi(transport.c_str() + dash + 1);
  if (a <= 0 || b <= 0) return false;
  *rtp_port = a;
  *rtcp_port = b;
  return true;
}


std::string RTSPAudioComponent::base64_encode_(const std::string &input) {
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((input.size() + 2) / 3) * 4);

  uint32_t val = 0;
  int valb = -6;
  for (uint8_t c : input) {
    val = (val << 8) | c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(table[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

bool RTSPAudioComponent::request_authorized_(const std::string &request) const {
  if (!this->auth_enabled_) return true;
  std::string auth = header_value(request, "Authorization");
  if (auth.empty()) return false;

  const std::string prefix = "Basic ";
  if (auth.size() < prefix.size()) return false;
  if (auth.compare(0, prefix.size(), prefix) != 0) return false;

  size_t pos = prefix.size();
  while (pos < auth.size() && (auth[pos] == ' ' || auth[pos] == '\t')) pos++;
  size_t end = auth.size();
  while (end > pos && (auth[end - 1] == ' ' || auth[end - 1] == '\t' || auth[end - 1] == '\r' || auth[end - 1] == '\n')) end--;
  return auth.substr(pos, end - pos) == this->auth_token_;
}

void RTSPAudioComponent::send_auth_required_(int fd, int cseq) {
  std::string h = "WWW-Authenticate: Basic realm=\"" + this->auth_realm_ + "\"\r\n";
  this->send_rtsp_response_(fd, 401, "Unauthorized", cseq, h, "");
}

std::string RTSPAudioComponent::make_sdp_() const {
  char sdp[512];
  snprintf(sdp, sizeof(sdp),
           "v=0\r\n"
           "o=- 0 0 IN IP4 %s\r\n"
           "s=ESPHome RTSP Audio\r\n"
           "c=IN IP4 %s\r\n"
           "t=0 0\r\n"
           "m=audio 0 RTP/AVP %d\r\n"
           "a=rtpmap:%d L16/%d/1\r\n"
           "a=control:trackID=0\r\n",
           this->local_ip_().c_str(), this->local_ip_().c_str(), this->rtp_payload_type_, this->rtp_payload_type_, this->sample_rate_);
  return std::string(sdp);
}

void RTSPAudioComponent::send_rtsp_response_(int fd, int code, const char *reason, int cseq, const std::string &headers, const std::string &body) {
  char head[768];
  int len = snprintf(head, sizeof(head),
                     "RTSP/1.0 %d %s\r\n"
                     "CSeq: %d\r\n"
                     "%s"
                     "Content-Length: %u\r\n"
                     "\r\n",
                     code, reason, cseq, headers.c_str(), (unsigned) body.size());
  send(fd, head, len, 0);
  if (!body.empty()) send(fd, body.data(), body.size(), 0);
}

void RTSPAudioComponent::handle_rtsp_client_(int client_fd) {
  std::string session = "12345678";
  while (this->running_) {
    std::string req;
    if (!this->read_rtsp_request_(client_fd, req)) break;

    int cseq = this->parse_cseq_(req);
    std::string first = req.substr(0, req.find('\n'));
    if (!first.empty() && first.back() == '\r') first.pop_back();
    if (this->debug_) ESP_LOGI(TAG, "RTSP request: %s", first.c_str());

    if (this->auth_enabled_ && req.rfind("OPTIONS", 0) != 0 && !this->request_authorized_(req)) {
      if (this->debug_) ESP_LOGI(TAG, "RTSP request rejected: missing or invalid Basic auth");
      this->send_auth_required_(client_fd, cseq);
      continue;
    }

    if (req.rfind("OPTIONS", 0) == 0) {
      this->send_rtsp_response_(client_fd, 200, "OK", cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n", "");
    } else if (req.rfind("DESCRIBE", 0) == 0) {
      std::string sdp = this->make_sdp_();
      this->send_rtsp_response_(client_fd, 200, "OK", cseq, "Content-Type: application/sdp\r\n", sdp);
    } else if (req.rfind("SETUP", 0) == 0) {
      if (req.find("RTP/AVP/TCP") != std::string::npos || req.find("interleaved") != std::string::npos) {
        this->send_rtsp_response_(client_fd, 461, "Unsupported Transport", cseq, "", "");
        continue;
      }
      int client_rtp = 0, client_rtcp = 0;
      if (!this->parse_client_ports_(req, &client_rtp, &client_rtcp)) {
        this->send_rtsp_response_(client_fd, 400, "Bad Request", cseq, "", "");
        continue;
      }

      ::sockaddr_in peer = {};
      socklen_t peer_len = sizeof(peer);
      getpeername(client_fd, (::sockaddr *) &peer, &peer_len);
      this->close_rtp_sockets_();
      this->rtp_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
      this->rtcp_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
      if (this->rtp_fd_ < 0 || this->rtcp_fd_ < 0) {
        this->send_rtsp_response_(client_fd, 500, "Internal Server Error", cseq, "", "");
        continue;
      }

      // A modest UDP send buffer reduces short Wi-Fi/lwIP backpressure bursts.
      // Keep it conservative for non-PSRAM ESP32 boards.
      int sndbuf = 16 * 1024;
      setsockopt(this->rtp_fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

      ::sockaddr_in local = {};
      local.sin_family = AF_INET;
      local.sin_addr.s_addr = htonl(INADDR_ANY);
      local.sin_port = 0;
      bind(this->rtp_fd_, (::sockaddr *) &local, sizeof(local));
      bind(this->rtcp_fd_, (::sockaddr *) &local, sizeof(local));
      socklen_t llen = sizeof(local);
      getsockname(this->rtp_fd_, (::sockaddr *) &local, &llen);
      this->server_rtp_port_ = ntohs(local.sin_port);
      getsockname(this->rtcp_fd_, (::sockaddr *) &local, &llen);
      this->server_rtcp_port_ = ntohs(local.sin_port);

      this->client_rtp_addr_ = new ::sockaddr_in();
      this->client_rtcp_addr_ = new ::sockaddr_in();
      *this->client_rtp_addr_ = peer;
      *this->client_rtcp_addr_ = peer;
      this->client_rtp_addr_->sin_port = htons(client_rtp);
      this->client_rtcp_addr_->sin_port = htons(client_rtcp);

      char h[256];
      snprintf(h, sizeof(h),
               "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
               "Session: %s\r\n",
               client_rtp, client_rtcp, this->server_rtp_port_, this->server_rtcp_port_, session.c_str());
      this->send_rtsp_response_(client_fd, 200, "OK", cseq, h, "");
      ESP_LOGI(TAG, "RTSP SETUP complete: client RTP/RTCP %d/%d, server RTP/RTCP %d/%d", client_rtp, client_rtcp,
               this->server_rtp_port_, this->server_rtcp_port_);
    } else if (req.rfind("PLAY", 0) == 0) {
      if (this->rtp_fd_ < 0 || this->client_rtp_addr_ == nullptr) {
        this->send_rtsp_response_(client_fd, 454, "Session Not Found", cseq, "", "");
        continue;
      }
      if (this->audio_buffer_ != nullptr) xStreamBufferReset(this->audio_buffer_);
      this->streaming_ = true;
      if (this->rtp_task_handle_ == nullptr) {
        xTaskCreatePinnedToCore(&RTSPAudioComponent::rtp_task_trampoline_, "rtsp_audio_rtp", 8192, this, 6,
                                &this->rtp_task_handle_, 1);
      }
      std::string h = "Session: " + session + "\r\nRTP-Info: url=trackID=0;seq=0;rtptime=0\r\n";
      this->send_rtsp_response_(client_fd, 200, "OK", cseq, h, "");
      ESP_LOGI(TAG, "RTSP PLAY; streaming started");
    } else if (req.rfind("TEARDOWN", 0) == 0) {
      this->streaming_ = false;
      this->send_rtsp_response_(client_fd, 200, "OK", cseq, "", "");
      break;
    } else {
      this->send_rtsp_response_(client_fd, 405, "Method Not Allowed", cseq, "", "");
    }
  }
}

void RTSPAudioComponent::rtp_task_() {
  const int samples_per_packet = std::max(80, (this->sample_rate_ * this->packet_ms_) / 1000);
  // MicrophoneSource always hands us 16-bit signed PCM (we requested it that
  // way in setup()), so each sample is always 2 bytes here -- unlike the old
  // direct-I2S version, there's no separate 16-bit-vs-32-bit-input branch.
  std::vector<uint8_t> input(samples_per_packet * 2);
  std::vector<uint8_t> packet(12 + samples_per_packet * 2);
  ESP_LOGI(TAG, "RTP task started: %d samples/packet, %d ms packets", samples_per_packet, this->packet_ms_);
  while (this->running_) {
    if (!this->streaming_ || this->rtp_fd_ < 0 || this->client_rtp_addr_ == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    size_t bytes_read = 0;
    if (this->audio_buffer_ != nullptr) {
      const uint32_t deadline = now_ms() + (uint32_t) std::max(50, this->packet_ms_ * 4);
      while (bytes_read < input.size() && this->running_ && this->streaming_) {
        TickType_t wait_ticks = bytes_read == 0 ? pdMS_TO_TICKS(std::max(50, this->packet_ms_ * 4)) : pdMS_TO_TICKS(2);
        size_t n = xStreamBufferReceive(this->audio_buffer_, input.data() + bytes_read, input.size() - bytes_read, wait_ticks);
        bytes_read += n;
        if (n == 0 || now_ms() >= deadline) break;
      }
    }
    this->last_bytes_read_ = (uint32_t) bytes_read;
    if (bytes_read == 0) {
      this->i2s_empty_reads_++;
      continue;
    }
    this->i2s_reads_++;

    int samples = bytes_read / 2;
    if (samples <= 0) continue;
    if (samples > samples_per_packet) samples = samples_per_packet;
    int32_t min_sample = 32767;
    int32_t max_sample = -32768;
    int32_t peak_sample = 0;

    packet[0] = 0x80;
    packet[1] = (uint8_t) (this->rtp_payload_type_ & 0x7F);
    packet[2] = (uint8_t) (this->rtp_sequence_ >> 8);
    packet[3] = (uint8_t) (this->rtp_sequence_ & 0xFF);
    packet[4] = (uint8_t) (this->rtp_timestamp_ >> 24);
    packet[5] = (uint8_t) (this->rtp_timestamp_ >> 16);
    packet[6] = (uint8_t) (this->rtp_timestamp_ >> 8);
    packet[7] = (uint8_t) (this->rtp_timestamp_ & 0xFF);
    packet[8] = (uint8_t) (this->ssrc_ >> 24);
    packet[9] = (uint8_t) (this->ssrc_ >> 16);
    packet[10] = (uint8_t) (this->ssrc_ >> 8);
    packet[11] = (uint8_t) (this->ssrc_ & 0xFF);

    // Gain is now applied upstream by MicrophoneSource (gain_factor_), so this
    // loop just repackages already-processed samples into RTP L16 instead of
    // also shifting/scaling/clipping them itself. Peak/min/max are still
    // tracked here (from the post-gain samples) since they're a useful debug
    // signal regardless of where the gain was applied; a sample pinned at
    // +/-32767 still means "something upstream clipped".
    for (int i = 0; i < samples; i++) {
      int16_t v;
      memcpy(&v, &input[i * 2], 2);
      if (v < min_sample) min_sample = v;
      if (v > max_sample) max_sample = v;
      int32_t abs_sample = v < 0 ? -(int32_t) v : (int32_t) v;
      if (abs_sample > peak_sample) peak_sample = abs_sample;
      if (abs_sample >= 32767) this->clipped_samples_++;
      uint16_t be = htons((uint16_t) v);
      memcpy(&packet[12 + i * 2], &be, 2);
    }
    this->last_min_ = min_sample;
    this->last_max_ = max_sample;
    this->last_peak_ = peak_sample;

    int sent = sendto(this->rtp_fd_, packet.data(), 12 + samples * 2, 0, (::sockaddr *) this->client_rtp_addr_, sizeof(::sockaddr_in));
    if (sent < 0) {
      this->rtp_send_errors_++;
      // Do not pause for 100 ms on ENOBUFS/backpressure; that creates audible gaps.
      // Throttle the warning so logging itself does not make audio worse.
      uint32_t t = now_ms();
      if (t - this->last_send_error_log_ms_ > 2000) {
        this->last_send_error_log_ms_ = t;
        ESP_LOGW(TAG, "RTP sendto failed: %s", strerror(errno));
      }
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    this->rtp_packets_++;
    this->rtp_sequence_++;
    this->rtp_timestamp_ += samples;
  }
  ESP_LOGI(TAG, "RTP task stopped");
}

void RTSPAudioComponent::close_rtp_sockets_() {
  this->streaming_ = false;
  if (this->rtp_fd_ >= 0) {
    close(this->rtp_fd_);
    this->rtp_fd_ = -1;
  }
  if (this->rtcp_fd_ >= 0) {
    close(this->rtcp_fd_);
    this->rtcp_fd_ = -1;
  }
  delete this->client_rtp_addr_;
  delete this->client_rtcp_addr_;
  this->client_rtp_addr_ = nullptr;
  this->client_rtcp_addr_ = nullptr;
}

std::string RTSPAudioComponent::local_ip_() const {
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif == nullptr) return "0.0.0.0";
  esp_netif_ip_info_t ip = {};
  if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) return "0.0.0.0";
  char buf[16];
  snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
  return std::string(buf);
}

void RTSPAudioComponent::loop() {
  uint32_t now = now_ms();

  // Only run the microphone while an RTSP client is actually in PLAY state,
  // so an idle server doesn't keep the mic (and whatever it owns) running for
  // nothing. mic_source_->start()/stop() are only ever called from here (the
  // main ESPHome loop task), never from the RTSP/RTP tasks directly -- the
  // RTSP task just flips the streaming_ atomic and this polls it.
  if (this->mic_source_ != nullptr) {
    bool want_running = this->streaming_.load();
    bool mic_running = this->mic_source_->is_running();
    if (want_running && !mic_running) {
      ESP_LOGD(TAG, "RTSP playback active, starting microphone capture.");
      this->mic_source_->start();
    } else if (!want_running && mic_running) {
      ESP_LOGD(TAG, "RTSP playback stopped, stopping microphone capture.");
      this->mic_source_->stop();
    }
  }

  if (this->debug_ && this->status_interval_ms_ > 0 && now - this->last_status_ms_ >= this->status_interval_ms_) {
    this->last_status_ms_ = now;
    this->log_status_("periodic");
  }
}

void RTSPAudioComponent::log_status_(const char *reason) {
  ESP_LOGI(TAG,
           "RTSP native status [%s]: started=%s mic_running=%s client=%s streaming=%s ip=%s port=%d free_heap=%u "
           "packets=%u send_err=%u clip=%u reads=%u empty=%u drop=%u bytes=%u peak=%d min=%d max=%d last_error=%s",
           reason, YESNO(this->started_.load()),
           YESNO(this->mic_source_ != nullptr && this->mic_source_->is_running()), YESNO(this->client_connected_.load()),
           YESNO(this->streaming_.load()), this->local_ip_().c_str(), this->port_, (unsigned) esp_get_free_heap_size(),
           (unsigned) this->rtp_packets_.load(), (unsigned) this->rtp_send_errors_.load(),
           (unsigned) this->clipped_samples_.load(), (unsigned) this->i2s_reads_.load(), (unsigned) this->i2s_empty_reads_.load(), (unsigned) this->dropped_bytes_.load(), (unsigned) this->last_bytes_read_.load(),
           (int) this->last_peak_.load(), (int) this->last_min_.load(), (int) this->last_max_.load(), this->last_error_.c_str());
}

void RTSPAudioComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Native RTSP Audio (microphone-source) v2");
  ESP_LOGCONFIG(TAG, "  Port: %d", this->port_);
  ESP_LOGCONFIG(TAG, "  Microphone sample rate: %d Hz", this->sample_rate_);
  ESP_LOGCONFIG(TAG, "  Microphone channel index: %u", (unsigned) this->channel_);
  ESP_LOGCONFIG(TAG, "  Gain factor: %d", (int) this->gain_factor_);
  ESP_LOGCONFIG(TAG, "  RTP codec: L16/%d/1 payload_type=%d", this->sample_rate_, this->rtp_payload_type_);
  ESP_LOGCONFIG(TAG, "  Packet duration: %d ms", this->packet_ms_);
  ESP_LOGCONFIG(TAG, "  Audio buffer: %d ms", this->buffer_ms_);
  ESP_LOGCONFIG(TAG, "  Authentication: %s", YESNO(this->auth_enabled_));
  if (this->auth_enabled_) {
    ESP_LOGCONFIG(TAG, "  Auth realm: %s", this->auth_realm_.c_str());
    ESP_LOGCONFIG(TAG, "  Auth username: %s", this->auth_username_.c_str());
  }
  ESP_LOGCONFIG(TAG, "  Debug: %s", YESNO(this->debug_));
  ESP_LOGCONFIG(TAG, "  Status interval: %" PRIu32 " ms", this->status_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Runtime started=%s client=%s streaming=%s ip=%s last_error=%s", YESNO(this->started_.load()),
                YESNO(this->client_connected_.load()), YESNO(this->streaming_.load()), this->local_ip_().c_str(),
                this->last_error_.c_str());
}

void RTSPAudioComponent::stop_server_() {
  this->running_ = false;
  this->streaming_ = false;
  if (this->client_fd_ >= 0) {
    shutdown(this->client_fd_, SHUT_RDWR);
    close(this->client_fd_);
    this->client_fd_ = -1;
  }
  if (this->server_fd_ >= 0) {
    shutdown(this->server_fd_, SHUT_RDWR);
    close(this->server_fd_);
    this->server_fd_ = -1;
  }
  this->close_rtp_sockets_();
}

void RTSPAudioComponent::on_shutdown() {
  ESP_LOGI(TAG, "Shutting down native RTSP audio");
  this->stop_server_();
  if (this->mic_source_ != nullptr && this->mic_source_->is_running()) {
    this->mic_source_->stop();
  }
  if (this->audio_buffer_ != nullptr) {
    vStreamBufferDelete(this->audio_buffer_);
    this->audio_buffer_ = nullptr;
  }
}

}  // namespace rtsp_audio
}  // namespace esphome
