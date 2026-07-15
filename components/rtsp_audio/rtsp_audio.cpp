#include "rtsp_audio.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <vector>

#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

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
  ESP_LOGI(TAG, "Native ESP-IDF RTSP audio setup attempt %u", this->setup_attempts_);

  if (!this->start_i2s_()) {
    ESP_LOGE(TAG, "I2S startup failed: %s", this->last_error_.c_str());
    return;
  }
  if (!this->start_server_task_()) {
    ESP_LOGE(TAG, "RTSP server startup failed: %s", this->last_error_.c_str());
    return;
  }
  this->started_ = true;
  this->last_error_ = "none";
  ESP_LOGI(TAG, "Native RTSP audio server task started on port %d", this->port_);
  ESP_LOGI(TAG, "RTSP URL: rtsp://%s:%d/", this->local_ip_().c_str(), this->port_);
}

bool RTSPAudioComponent::start_i2s_() {
  if (this->i2s_started_) return true;

  i2s_port_t port = (this->i2s_port_num_ == 1) ? I2S_NUM_1 : I2S_NUM_0;

  i2s_data_bit_width_t bits = I2S_DATA_BIT_WIDTH_32BIT;
  if (this->i2s_bits_per_sample_ == 16) bits = I2S_DATA_BIT_WIDTH_16BIT;
#if defined(I2S_DATA_BIT_WIDTH_24BIT)
  if (this->i2s_bits_per_sample_ == 24) bits = I2S_DATA_BIT_WIDTH_24BIT;
#endif

  ESP_LOGI(TAG, "Starting ESP-IDF I2S STD: port=%d bclk=GPIO%d lrclk=GPIO%d din=GPIO%d rate=%d i2s_bits=%d channel=%s shift=%d gain=%.2fx apll=%s",
           this->i2s_port_num_, this->bclk_pin_, this->lrclk_pin_, this->din_pin_, this->sample_rate_,
           this->i2s_bits_per_sample_, this->right_channel_ ? "right" : "left", this->sample_shift_, this->gain_, YESNO(this->use_apll_));

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(port, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 8;
  chan_cfg.dma_frame_num = 256;

  esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &this->rx_chan_);
  if (err != ESP_OK) {
    this->last_error_ = std::string("i2s_new_channel failed: ") + esp_err_to_name(err);
    this->rx_chan_ = nullptr;
    return false;
  }

  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(static_cast<uint32_t>(this->sample_rate_));
#if defined(I2S_CLK_SRC_APLL)
  if (this->use_apll_) {
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
  }
#endif
  std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, this->use_stereo_slot_ ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO);
  std_cfg.slot_cfg.slot_mask = this->right_channel_ ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;
  std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(this->bclk_pin_);
  std_cfg.gpio_cfg.ws = static_cast<gpio_num_t>(this->lrclk_pin_);
  std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.din = static_cast<gpio_num_t>(this->din_pin_);
  std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.ws_inv = false;

  err = i2s_channel_init_std_mode(this->rx_chan_, &std_cfg);
  if (err != ESP_OK) {
    this->last_error_ = std::string("i2s_channel_init_std_mode failed: ") + esp_err_to_name(err);
    i2s_del_channel(this->rx_chan_);
    this->rx_chan_ = nullptr;
    return false;
  }

  err = i2s_channel_enable(this->rx_chan_);
  if (err != ESP_OK) {
    this->last_error_ = std::string("i2s_channel_enable failed: ") + esp_err_to_name(err);
    i2s_del_channel(this->rx_chan_);
    this->rx_chan_ = nullptr;
    return false;
  }

  this->i2s_started_ = true;
  return true;
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
  const int input_bytes_per_sample = this->i2s_bits_per_sample_ <= 16 ? 2 : 4;
  std::vector<uint8_t> input(samples_per_packet * input_bytes_per_sample);
  std::vector<uint8_t> packet(12 + samples_per_packet * 2);
  ESP_LOGI(TAG, "RTP task started: %d samples/packet, %d ms packets", samples_per_packet, this->packet_ms_);
  while (this->running_) {
    if (!this->streaming_ || this->rtp_fd_ < 0 || this->client_rtp_addr_ == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    size_t bytes_read = 0;
    esp_err_t err = this->rx_chan_ == nullptr ? ESP_ERR_INVALID_STATE : i2s_channel_read(this->rx_chan_, input.data(), input.size(), &bytes_read, 1000);
    this->last_bytes_read_ = (uint32_t) bytes_read;
    if (err != ESP_OK || bytes_read == 0) {
      this->i2s_empty_reads_++;
      if (this->debug_) ESP_LOGW(TAG, "i2s_read failed/empty: %s bytes=%u", esp_err_to_name(err), (unsigned) bytes_read);
      continue;
    }
    this->i2s_reads_++;

    int samples = bytes_read / input_bytes_per_sample;
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

    for (int i = 0; i < samples; i++) {
      int32_t raw = 0;
      if (input_bytes_per_sample == 2) {
        int16_t v;
        memcpy(&v, &input[i * 2], 2);
        raw = v;
      } else {
        int32_t v;
        memcpy(&v, &input[i * 4], 4);
        raw = v >> this->sample_shift_;
      }
      int32_t amplified = (int32_t) lrintf((float) raw * this->gain_);
      if (amplified > 32767) { amplified = 32767; this->clipped_samples_++; }
      if (amplified < -32768) { amplified = -32768; this->clipped_samples_++; }
      if (amplified < min_sample) min_sample = amplified;
      if (amplified > max_sample) max_sample = amplified;
      int32_t abs_sample = amplified < 0 ? -amplified : amplified;
      if (abs_sample > peak_sample) peak_sample = abs_sample;
      uint16_t be = htons((uint16_t) ((int16_t) amplified));
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
  if (this->debug_ && this->status_interval_ms_ > 0 && now - this->last_status_ms_ >= this->status_interval_ms_) {
    this->last_status_ms_ = now;
    this->log_status_("periodic");
  }
}

void RTSPAudioComponent::log_status_(const char *reason) {
  ESP_LOGI(TAG,
           "RTSP native status [%s]: started=%s i2s=%s client=%s streaming=%s ip=%s port=%d free_heap=%u "
           "packets=%u send_err=%u clip=%u i2s_reads=%u empty=%u bytes=%u peak=%d min=%d max=%d last_error=%s",
           reason, YESNO(this->started_.load()), YESNO(this->i2s_started_.load()), YESNO(this->client_connected_.load()),
           YESNO(this->streaming_.load()), this->local_ip_().c_str(), this->port_, (unsigned) esp_get_free_heap_size(),
           (unsigned) this->rtp_packets_.load(), (unsigned) this->rtp_send_errors_.load(),
           (unsigned) this->clipped_samples_.load(), (unsigned) this->i2s_reads_.load(), (unsigned) this->i2s_empty_reads_.load(), (unsigned) this->last_bytes_read_.load(),
           (int) this->last_peak_.load(), (int) this->last_min_.load(), (int) this->last_max_.load(), this->last_error_.c_str());
}

void RTSPAudioComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Native ESP-IDF RTSP Audio v9");
  ESP_LOGCONFIG(TAG, "  Port: %d", this->port_);
  ESP_LOGCONFIG(TAG, "  BCLK pin: GPIO%d", this->bclk_pin_);
  ESP_LOGCONFIG(TAG, "  LRCLK/WS pin: GPIO%d", this->lrclk_pin_);
  ESP_LOGCONFIG(TAG, "  DIN/SD pin: GPIO%d", this->din_pin_);
  ESP_LOGCONFIG(TAG, "  Sample rate: %d", this->sample_rate_);
  ESP_LOGCONFIG(TAG, "  RTP codec: L16/%d/1 payload_type=%d", this->sample_rate_, this->rtp_payload_type_);
  ESP_LOGCONFIG(TAG, "  I2S port: %d", this->i2s_port_num_);
  ESP_LOGCONFIG(TAG, "  I2S bits/sample: %d", this->i2s_bits_per_sample_);
  ESP_LOGCONFIG(TAG, "  Channel: %s", this->right_channel_ ? "right" : "left");
  ESP_LOGCONFIG(TAG, "  Sample shift: %d", this->sample_shift_);
  ESP_LOGCONFIG(TAG, "  Gain: %.2fx", this->gain_);
  ESP_LOGCONFIG(TAG, "  APLL: %s", YESNO(this->use_apll_));
  ESP_LOGCONFIG(TAG, "  I2S slot mode: %s", this->use_stereo_slot_ ? "stereo clocks / selected slot" : "mono");
  ESP_LOGCONFIG(TAG, "  Packet duration: %d ms", this->packet_ms_);
  ESP_LOGCONFIG(TAG, "  Debug: %s", YESNO(this->debug_));
  ESP_LOGCONFIG(TAG, "  Status interval: %u ms", this->status_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Runtime started=%s i2s=%s client=%s streaming=%s ip=%s last_error=%s", YESNO(this->started_.load()),
                YESNO(this->i2s_started_.load()), YESNO(this->client_connected_.load()), YESNO(this->streaming_.load()),
                this->local_ip_().c_str(), this->last_error_.c_str());
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
  if (this->i2s_started_ && this->rx_chan_ != nullptr) {
    i2s_channel_disable(this->rx_chan_);
    i2s_del_channel(this->rx_chan_);
    this->rx_chan_ = nullptr;
    this->i2s_started_ = false;
  }
}

}  // namespace rtsp_audio
}  // namespace esphome
