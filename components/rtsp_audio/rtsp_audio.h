#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/microphone/microphone_source.h"

#include <atomic>
#include <cstdint>
#include <string>

#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "lwip/sockets.h"

namespace esphome {
namespace rtsp_audio {

class RTSPAudioComponent : public Component {
 public:
  void set_microphone(microphone::Microphone *mic) { this->mic_ = mic; }
  void set_port(int port) { this->port_ = port; }
  void set_channel(uint8_t channel) { this->channel_ = channel; }
  void set_gain_factor(int32_t gain_factor) { this->gain_factor_ = gain_factor; }
  void set_rtp_config(int payload_type, int packet_ms) {
    this->rtp_payload_type_ = payload_type;
    this->packet_ms_ = packet_ms;
  }
  void set_debug(bool debug) { this->debug_ = debug; }
  void set_buffer_ms(int buffer_ms) { this->buffer_ms_ = buffer_ms; }
  void set_status_interval(uint32_t interval_ms) { this->status_interval_ms_ = interval_ms; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  bool start_server_task_();
  void stop_server_();
  void log_status_(const char *reason);
  std::string local_ip_() const;

  static void server_task_trampoline_(void *arg);
  static void rtp_task_trampoline_(void *arg);
  void server_task_();
  void rtp_task_();

  int create_tcp_server_();
  void handle_rtsp_client_(int client_fd);
  bool read_rtsp_request_(int fd, std::string &request);
  void send_rtsp_response_(int fd, int code, const char *reason, int cseq, const std::string &headers, const std::string &body);
  int parse_cseq_(const std::string &request) const;
  bool parse_client_ports_(const std::string &request, int *rtp_port, int *rtcp_port) const;
  std::string make_sdp_() const;
  void close_rtp_sockets_();

  microphone::Microphone *mic_{nullptr};
  microphone::MicrophoneSource *mic_source_{nullptr};
  StreamBufferHandle_t audio_buffer_{nullptr};
  uint8_t channel_{0};
  int32_t gain_factor_{4};

  int port_{8554};
  int sample_rate_{16000};  // populated from the microphone at setup() time
  int rtp_payload_type_{96};
  int packet_ms_{20};
  int buffer_ms_{200};
  bool debug_{false};
  uint32_t status_interval_ms_{10000};

  std::atomic<bool> running_{false};
  std::atomic<bool> started_{false};
  std::atomic<bool> client_connected_{false};
  std::atomic<bool> streaming_{false};

  TaskHandle_t server_task_handle_{nullptr};
  TaskHandle_t rtp_task_handle_{nullptr};
  int server_fd_{-1};
  int client_fd_{-1};
  int rtp_fd_{-1};
  int rtcp_fd_{-1};
  int server_rtp_port_{0};
  int server_rtcp_port_{0};
  uint32_t ssrc_{0x45535048};
  uint16_t rtp_sequence_{0};
  uint32_t rtp_timestamp_{0};

  ::sockaddr_in *client_rtp_addr_{nullptr};
  ::sockaddr_in *client_rtcp_addr_{nullptr};

  uint32_t setup_attempts_{0};
  std::atomic<uint32_t> rtp_packets_{0};
  std::atomic<uint32_t> rtp_send_errors_{0};
  std::atomic<uint32_t> clipped_samples_{0};
  std::atomic<uint32_t> i2s_reads_{0};
  std::atomic<uint32_t> i2s_empty_reads_{0};
  std::atomic<uint32_t> dropped_bytes_{0};
  std::atomic<int32_t> last_peak_{0};
  std::atomic<int32_t> last_min_{0};
  std::atomic<int32_t> last_max_{0};
  std::atomic<uint32_t> last_bytes_read_{0};
  uint32_t last_status_ms_{0};
  uint32_t last_send_error_log_ms_{0};
  std::string last_error_{"not started yet"};
};

}  // namespace rtsp_audio
}  // namespace esphome
