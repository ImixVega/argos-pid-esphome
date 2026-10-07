#pragma once

#include <map>
#include <string>

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome {
namespace argos_pid {

class ArgosPidComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_bind_ip(const std::string &v) { bind_ip_ = v; }
  void set_expected_device_ip(const std::string &v) { expected_device_ip_ = v; }
  void set_online_timeout_ms(uint32_t v) { online_timeout_ms_ = v; }
  void set_response_a35(const std::string &v) { response_a35_ = v; }
  void set_response_a36(const std::string &v) { response_a36_ = v; }
  void set_response_a37(const std::string &v) { response_a37_ = v; }

  void set_mode(const std::string &v) { proxy_mode_ = (v == "proxy"); }
  void set_upstream_host(const std::string &v) { upstream_host_ = v; }
  void set_upstream_ip(const std::string &v) { upstream_ip_ = v; }
  void set_upstream_port(uint16_t v) { upstream_port_ = v; }
  void set_proxy_timeout_ms(uint32_t v) { proxy_timeout_ms_ = v; }
  void set_proxy_fallback_local(bool v) { proxy_fallback_local_ = v; }

  // v0.5: local-first configuration editor.
  // First valid Argos POST initializes live_fields_. HA edits are staged only
  // in RAM. Nothing reaches Argos until request_save() is explicitly called.
  float get_live_value(const std::string &key) const;
  int get_live_int(const std::string &key, int fallback = -1) const;
  float get_edit_value(const std::string &key) const;
  int get_edit_int(const std::string &key, int fallback = -1) const;
  std::string get_write_status() const { return write_status_; }
  size_t get_pending_count() const { return pending_writes_.size() + expected_writes_.size(); }
  bool has_pending_changes() const { return !pending_writes_.empty() || !expected_writes_.empty(); }

  void stage_write_decimal(const std::string &key, uint32_t value);
  void request_save();
  void cancel_pending_writes();

  bool get_proxy_enabled() const { return proxy_mode_; }
  bool set_proxy_enabled(bool enabled);

  void set_online_binary_sensor(binary_sensor::BinarySensor *s) { online_binary_sensor_ = s; }
  void set_dhw_priority_binary_sensor(binary_sensor::BinarySensor *s) { dhw_priority_binary_sensor_ = s; }
  void set_cloud_online_binary_sensor(binary_sensor::BinarySensor *s) { cloud_online_binary_sensor_ = s; }
  void set_alarm_active_binary_sensor(binary_sensor::BinarySensor *s) { alarm_active_binary_sensor_ = s; }

  void set_cloud_response_type_text_sensor(text_sensor::TextSensor *s) { cloud_response_type_text_sensor_ = s; }
  void set_cloud_last_summary_text_sensor(text_sensor::TextSensor *s) { cloud_last_summary_text_sensor_ = s; }
  void set_raw_last_change_text_sensor(text_sensor::TextSensor *s) { raw_last_change_text_sensor_ = s; }
  void set_alarm_message_text_sensor(text_sensor::TextSensor *s) { alarm_message_text_sensor_ = s; }

#define ARGOS_SENSOR_SETTER(name) \
  void set_##name##_sensor(sensor::Sensor *s) { name##_sensor_ = s; }

  ARGOS_SENSOR_SETTER(temperature_boiler)
  ARGOS_SENSOR_SETTER(temperature_dhw)
  ARGOS_SENSOR_SETTER(temperature_feeder)
  ARGOS_SENSOR_SETTER(temperature_valve)
  ARGOS_SENSOR_SETTER(fuel_kg)
  ARGOS_SENSOR_SETTER(power_percent)
  ARGOS_SENSOR_SETTER(exhaust_temperature_provisional)
  ARGOS_SENSOR_SETTER(current_power_kw_provisional)
  ARGOS_SENSOR_SETTER(boiler_night_setpoint)
  ARGOS_SENSOR_SETTER(boiler_day_setpoint)
  ARGOS_SENSOR_SETTER(burner_feed_time)
  ARGOS_SENSOR_SETTER(burner_pause_time)
  ARGOS_SENSOR_SETTER(burner_fan_max)
  ARGOS_SENSOR_SETTER(burner_fan_min)
  ARGOS_SENSOR_SETTER(burner_power_min)
  ARGOS_SENSOR_SETTER(hold_work_time)
  ARGOS_SENSOR_SETTER(hold_pause_time)
  ARGOS_SENSOR_SETTER(hold_fan)
  ARGOS_SENSOR_SETTER(hold_feed_every_cycles)
  ARGOS_SENSOR_SETTER(hold_feed_multiplier)
  ARGOS_SENSOR_SETTER(co_pump_mode)
  ARGOS_SENSOR_SETTER(co_pump_start_temp)
  ARGOS_SENSOR_SETTER(co_pump_hysteresis)
  ARGOS_SENSOR_SETTER(dhw_mode)
  ARGOS_SENSOR_SETTER(dhw_setpoint)
  ARGOS_SENSOR_SETTER(dhw_hysteresis)
  ARGOS_SENSOR_SETTER(circulation_mode)
  ARGOS_SENSOR_SETTER(circulation_co_temp)
  ARGOS_SENSOR_SETTER(circulation_dhw_temp)
  ARGOS_SENSOR_SETTER(circulation_hysteresis)
  ARGOS_SENSOR_SETTER(circulation_work_min)
  ARGOS_SENSOR_SETTER(circulation_pause_min)
  ARGOS_SENSOR_SETTER(valve_mode)
  ARGOS_SENSOR_SETTER(valve_day_setpoint)
  ARGOS_SENSOR_SETTER(valve_night_setpoint)

  ARGOS_SENSOR_SETTER(cloud_latency_ms)
  ARGOS_SENSOR_SETTER(cloud_status_code)
  ARGOS_SENSOR_SETTER(cloud_response_fields)
  ARGOS_SENSOR_SETTER(proxy_request_count)
  ARGOS_SENSOR_SETTER(alarm_code)

#undef ARGOS_SENSOR_SETTER

 protected:
  bool start_servers_();
  void stop_servers_();
  void handle_dns_();
  void handle_http_accept_();
  void handle_http_client_();
  void close_http_client_();
  void process_http_request_(const std::string &request);
  void process_argos_body_(const std::string &body);
  void publish_fields_(const std::map<std::string, std::string> &fields);
  void send_http_response_(const std::string &body);

  bool proxy_to_cloud_(const std::string &request, std::string &response_body, int &status_code,
                       uint32_t &latency_ms);
  void process_cloud_response_(const std::string &body, int status_code, uint32_t latency_ms);
  void set_cloud_online_(bool state);
  void learn_normal_response_(const std::string &body);
  void log_body_chunks_(const char *prefix, const std::string &body);

  // v0.5 staged configuration helpers
  bool is_full_config_response_(const std::string &body) const;
  bool is_allowed_write_key_(const std::string &key) const;
  bool all_config_fields_present_(const std::map<std::string, std::string> &fields) const;
  bool prepare_pending_write_response_(std::string &body);
  std::string build_config_response_(const std::map<std::string, std::string> &overrides) const;
  void check_write_confirmation_(const std::map<std::string, std::string> &fields);
  void set_write_status_(const std::string &status);
  void track_unknown_raw_changes_(const std::map<std::string, std::string> &fields);
  void publish_alarm_state_(const std::map<std::string, std::string> &fields);
  static const char *alarm_message_for_code_(uint32_t code);
  static bool is_known_post_key_(const std::string &key);
  static std::string hex_string_(uint32_t value);
  static std::string lower_copy_(std::string value);
  std::string masked_device_id_() const;

  static bool parse_dns_name_(const uint8_t *buf, size_t len, size_t offset, std::string &name, size_t &after_name);
  static std::string lower_(std::string v);
  static std::string url_decode_(const std::string &v);
  static bool hex_value_(const std::map<std::string, std::string> &fields, const char *key, uint32_t &value);
  static size_t parse_content_length_(const std::string &headers);
  static int parse_http_status_(const std::string &headers);
  static int count_key_value_lines_(const std::string &body);
  static std::string response_value_(const std::string &body, const std::string &key);
  static void publish_(sensor::Sensor *sensor, float value);

  std::string bind_ip_{"10.10.15.1"};
  std::string expected_device_ip_{"10.10.15.11"};
  uint32_t online_timeout_ms_{60000};

  std::string response_a35_{"8"};
  std::string response_a36_{"0"};
  std::string response_a37_{"48"};

  bool proxy_mode_{false};
  std::string upstream_host_{"www.ekotlownia.pl"};
  std::string upstream_ip_{"109.95.148.70"};
  uint16_t upstream_port_{80};
  uint32_t proxy_timeout_ms_{2500};
  bool proxy_fallback_local_{true};

  int dns_fd_{-1};
  int http_fd_{-1};
  int client_fd_{-1};
  bool servers_started_{false};
  uint32_t next_server_start_attempt_ms_{0};

  std::string client_buffer_{};
  uint32_t client_started_ms_{0};

  uint32_t last_seen_ms_{0};
  bool online_state_{false};
  uint32_t packet_count_{0};
  std::string device_id_{};

  bool cloud_online_state_{false};
  bool cloud_online_initialized_{false};
  uint32_t proxy_request_count_{0};
  std::string last_cloud_response_type_{"none"};

  // Safe live configuration shadow. Never persisted to flash.
  std::map<std::string, std::string> live_fields_{};
  bool shadow_ready_{false};
  std::map<std::string, std::string> pending_writes_{};  // RAM-only working copy / overrides
  bool save_requested_{false};
  bool write_in_flight_{false};
  std::map<std::string, std::string> expected_writes_{};
  uint8_t write_confirm_packets_{0};
  std::string write_status_{"Czekam na pierwszy poprawny POST Argosa"};

  // v1.1: monitor nieznanych pol pozostaje diagnostycznie. AB jest zmapowane jako alarm.
  std::map<std::string, std::string> raw_unknown_fields_{};
  bool raw_baseline_ready_{false};
  bool alarm_initialized_{false};
  uint32_t last_alarm_code_{0};

  binary_sensor::BinarySensor *online_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *dhw_priority_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *cloud_online_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *alarm_active_binary_sensor_{nullptr};
  text_sensor::TextSensor *cloud_response_type_text_sensor_{nullptr};
  text_sensor::TextSensor *cloud_last_summary_text_sensor_{nullptr};
  text_sensor::TextSensor *raw_last_change_text_sensor_{nullptr};
  text_sensor::TextSensor *alarm_message_text_sensor_{nullptr};

#define ARGOS_SENSOR_MEMBER(name) sensor::Sensor *name##_sensor_{nullptr};
  ARGOS_SENSOR_MEMBER(temperature_boiler)
  ARGOS_SENSOR_MEMBER(temperature_dhw)
  ARGOS_SENSOR_MEMBER(temperature_feeder)
  ARGOS_SENSOR_MEMBER(temperature_valve)
  ARGOS_SENSOR_MEMBER(fuel_kg)
  ARGOS_SENSOR_MEMBER(power_percent)
  ARGOS_SENSOR_MEMBER(exhaust_temperature_provisional)
  ARGOS_SENSOR_MEMBER(current_power_kw_provisional)
  ARGOS_SENSOR_MEMBER(boiler_night_setpoint)
  ARGOS_SENSOR_MEMBER(boiler_day_setpoint)
  ARGOS_SENSOR_MEMBER(burner_feed_time)
  ARGOS_SENSOR_MEMBER(burner_pause_time)
  ARGOS_SENSOR_MEMBER(burner_fan_max)
  ARGOS_SENSOR_MEMBER(burner_fan_min)
  ARGOS_SENSOR_MEMBER(burner_power_min)
  ARGOS_SENSOR_MEMBER(hold_work_time)
  ARGOS_SENSOR_MEMBER(hold_pause_time)
  ARGOS_SENSOR_MEMBER(hold_fan)
  ARGOS_SENSOR_MEMBER(hold_feed_every_cycles)
  ARGOS_SENSOR_MEMBER(hold_feed_multiplier)
  ARGOS_SENSOR_MEMBER(co_pump_mode)
  ARGOS_SENSOR_MEMBER(co_pump_start_temp)
  ARGOS_SENSOR_MEMBER(co_pump_hysteresis)
  ARGOS_SENSOR_MEMBER(dhw_mode)
  ARGOS_SENSOR_MEMBER(dhw_setpoint)
  ARGOS_SENSOR_MEMBER(dhw_hysteresis)
  ARGOS_SENSOR_MEMBER(circulation_mode)
  ARGOS_SENSOR_MEMBER(circulation_co_temp)
  ARGOS_SENSOR_MEMBER(circulation_dhw_temp)
  ARGOS_SENSOR_MEMBER(circulation_hysteresis)
  ARGOS_SENSOR_MEMBER(circulation_work_min)
  ARGOS_SENSOR_MEMBER(circulation_pause_min)
  ARGOS_SENSOR_MEMBER(valve_mode)
  ARGOS_SENSOR_MEMBER(valve_day_setpoint)
  ARGOS_SENSOR_MEMBER(valve_night_setpoint)

  ARGOS_SENSOR_MEMBER(cloud_latency_ms)
  ARGOS_SENSOR_MEMBER(cloud_status_code)
  ARGOS_SENSOR_MEMBER(cloud_response_fields)
  ARGOS_SENSOR_MEMBER(proxy_request_count)
  ARGOS_SENSOR_MEMBER(alarm_code)
#undef ARGOS_SENSOR_MEMBER
};

}  // namespace argos_pid
}  // namespace esphome
