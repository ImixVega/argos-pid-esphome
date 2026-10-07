    case 14: return "Otwarta klapa zasobnika";
    case 15: return "Blad czujnika zewnetrznego";
    case 16: return "Blad czujnika spalin";
    case 17: return "Trwa rozpalanie/wygaszanie";
    default: return nullptr;
  }
}

void ArgosPidComponent::publish_alarm_state_(const std::map<std::string, std::string> &fields) {
  uint32_t code = 0;
  if (!hex_value_(fields, "AB", code))
    return;

  if (this->alarm_initialized_ && code == this->last_alarm_code_)
    return;

  this->alarm_initialized_ = true;
  this->last_alarm_code_ = code;

  publish_(this->alarm_code_sensor_, static_cast<float>(code));
  if (this->alarm_active_binary_sensor_ != nullptr)
    this->alarm_active_binary_sensor_->publish_state(code != 0);

  std::string message;
  const char *known = alarm_message_for_code_(code);
  if (known != nullptr) {
    message = known;
  } else {
    std::ostringstream ss;
    ss << "Nieznany alarm Er" << code;
    auto it = fields.find("AB");
    if (it != fields.end())
      ss << " (RAW=" << it->second << ")";
    message = ss.str();
  }

  if (this->alarm_message_text_sensor_ != nullptr)
    this->alarm_message_text_sensor_->publish_state(message);

  auto it = fields.find("AB");
  if (code == 0) {
    ESP_LOGI(TAG, "ALARM AB=%s -> OK",
             it != fields.end() ? it->second.c_str() : "?");
  } else {
    ESP_LOGW(TAG, "ALARM AB=%s -> Er%lu: %s",
             it != fields.end() ? it->second.c_str() : "?",
             static_cast<unsigned long>(code), message.c_str());
  }
}

void ArgosPidComponent::publish_(sensor::Sensor *sensor, float value) {
  if (sensor != nullptr)
    sensor->publish_state(value);
}

void ArgosPidComponent::publish_fields_(const std::map<std::string, std::string> &f) {
  uint32_t v = 0;

  this->publish_alarm_state_(f);
#define HEX_PUBLISH(KEY, SENSOR, SCALE) \
  do {                                  \
    if (hex_value_(f, KEY, v))          \
      publish_(SENSOR, v / (SCALE));    \
  } while (0)

  HEX_PUBLISH("A2", this->temperature_boiler_sensor_, 10.0f);
  HEX_PUBLISH("A3", this->temperature_dhw_sensor_, 10.0f);
  HEX_PUBLISH("A4", this->temperature_feeder_sensor_, 10.0f);
  HEX_PUBLISH("A5", this->temperature_valve_sensor_, 10.0f);
  HEX_PUBLISH("A6", this->fuel_kg_sensor_, 1.0f);
  HEX_PUBLISH("AF", this->power_percent_sensor_, 1.0f);
  HEX_PUBLISH("A10", this->exhaust_temperature_provisional_sensor_, 10.0f);
  HEX_PUBLISH("A18", this->current_power_kw_provisional_sensor_, 10.0f);

  HEX_PUBLISH("B2", this->boiler_night_setpoint_sensor_, 1.0f);
  HEX_PUBLISH("B3", this->boiler_day_setpoint_sensor_, 1.0f);

  HEX_PUBLISH("C1", this->burner_feed_time_sensor_, 1.0f);
  HEX_PUBLISH("C2", this->burner_pause_time_sensor_, 1.0f);
  HEX_PUBLISH("C3", this->burner_fan_max_sensor_, 1.0f);
  HEX_PUBLISH("C4", this->burner_fan_min_sensor_, 1.0f);
  HEX_PUBLISH("C5", this->burner_power_min_sensor_, 1.0f);

  HEX_PUBLISH("E1", this->hold_work_time_sensor_, 1.0f);
  HEX_PUBLISH("E2", this->hold_pause_time_sensor_, 1.0f);
  HEX_PUBLISH("E3", this->hold_fan_sensor_, 1.0f);
  HEX_PUBLISH("E4", this->hold_feed_every_cycles_sensor_, 1.0f);
  HEX_PUBLISH("E5", this->hold_feed_multiplier_sensor_, 1.0f);

  HEX_PUBLISH("F2", this->co_pump_mode_sensor_, 1.0f);
  HEX_PUBLISH("F3", this->co_pump_start_temp_sensor_, 1.0f);
  HEX_PUBLISH("F4", this->co_pump_hysteresis_sensor_, 1.0f);

  HEX_PUBLISH("G2", this->dhw_mode_sensor_, 1.0f);
  HEX_PUBLISH("G3", this->dhw_setpoint_sensor_, 1.0f);
  HEX_PUBLISH("G4", this->dhw_hysteresis_sensor_, 1.0f);
  if (hex_value_(f, "G5", v) && this->dhw_priority_binary_sensor_ != nullptr)
    this->dhw_priority_binary_sensor_->publish_state(v != 0);

  HEX_PUBLISH("H2", this->circulation_mode_sensor_, 1.0f);
  HEX_PUBLISH("H3", this->circulation_co_temp_sensor_, 1.0f);
  HEX_PUBLISH("H4", this->circulation_dhw_temp_sensor_, 1.0f);
  HEX_PUBLISH("H5", this->circulation_hysteresis_sensor_, 1.0f);
  HEX_PUBLISH("H6", this->circulation_work_min_sensor_, 1.0f);
  HEX_PUBLISH("H7", this->circulation_pause_min_sensor_, 1.0f);

  HEX_PUBLISH("I2", this->valve_mode_sensor_, 1.0f);
  HEX_PUBLISH("I3", this->valve_day_setpoint_sensor_, 1.0f);
  HEX_PUBLISH("I4", this->valve_night_setpoint_sensor_, 1.0f);

#undef HEX_PUBLISH
}

void ArgosPidComponent::send_http_response_(const std::string &body) {
  if (this->client_fd_ < 0)
    return;
  std::ostringstream ss;
  ss << "HTTP/1.0 200 OK\r\n"
     << "Content-Type: text/plain\r\n"
     << "Content-Length: " << body.size() << "\r\n"
     << "Connection: close\r\n\r\n"
     << body;
  const std::string data = ss.str();
  size_t sent_total = 0;
  while (sent_total < data.size()) {
    const int n = send(this->client_fd_, data.data() + sent_total, data.size() - sent_total, 0);
    if (n > 0) {
      sent_total += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
      delay(1);
      continue;
    }
    break;
  }
}

}  // namespace argos_pid
}  // namespace esphome
