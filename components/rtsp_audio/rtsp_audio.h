#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"

#include <atomic>
#include <cstdint>
#include <string>

#include "driver/i2s_std.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace esphome {
namespace rtsp_audio {

class RTSPAudioComponent : public Component {
 public:
  void set_i2s_pins(int bclk_pin, int lrclk_pin, int din_pin) {
    this->bclk_pin_ = bclk_pin;
    this->lrclk_pin_ = lrclk_pin;
    this->din_pin_ = din_pin;
  }

  void set_port(int port) { this->port_ = port; }
  void set_audio_config(int sample_rate, bool right_channel) {
    this->sample_rate_ = sample_rate;
    this->right_channel_ = right_channel;
  }
  void set_i2s_config(int i2s_port, int i2s_bits_per_sample, int sample_shift, bool use_apll) {
    this->i2s_port_num_ = i2s_port;
    this->i2s_bits_per_sample_ = i2s_bits_per_sample;
    this->sample_shift_ = sample_shift;
    this->use_apll_ = use_apll;
  }
  void set_rtp_config(int payload_type, int packet_ms) {
    this->rtp_payload_type_ = payload_type;
    this->packet_ms_ = packet_ms;
  }
  void set_gain(float gain) { this->gain_ = gain; }
  void set_debug(bool debug) { this->debug_ = debug; }
  void set_status_interval(uint32_t interval_ms) { this->status_interval_ms_ = interval_ms; }
  void set_use_stereo_slot(bool use_stereo_slot) { this->use_stereo_slot_ = use_stereo_slot; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  bool start_i2s_();
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

  int bclk_pin_{-1};
  int lrclk_pin_{-1};
  int din_pin_{-1};
  int port_{8554};
  int sample_rate_{16000};
  bool right_channel_{false};
  int i2s_port_num_{0};
  i2s_chan_handle_t rx_chan_{nullptr};
  int i2s_bits_per_sample_{32};
  int sample_shift_{14};
  bool use_apll_{false};
  bool use_stereo_slot_{false};
  int rtp_payload_type_{96};
  int packet_ms_{20};
  float gain_{1.0f};
  bool debug_{false};
  uint32_t status_interval_ms_{10000};

  std::atomic<bool> running_{false};
  std::atomic<bool> started_{false};
  std::atomic<bool> i2s_started_{false};
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
