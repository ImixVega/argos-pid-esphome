#include "argos_pid.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <vector>

#include <fcntl.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <unistd.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace argos_pid {

static const char *const TAG = "argos_pid";

static bool argos_full_config_zero_fallback_(const std::string &key) {
  // These five fields were present in every captured real-cloud FULL CONFIG
  // response, always as 0, but are not reported in the regular 103-field
  // Argos POST. They belong to the currently unused room-regulator/schedule
  // block. v1.1 supplies 0 only for these exact fields.
  return key == "L2" || key == "L3" || key == "J1C" || key == "J1D" || key == "J1E";
}

static bool argos_schedule_mask_key_(const std::string &key) {
  // User intentionally uses Home Assistant for scheduling. Keep every internal
  // Argos schedule mask enabled 24/7. This also repairs masks accidentally
  // zeroed by the uppercase-hex issue discovered in v0.5b.
  return key == "J1" || key == "J2" || key == "J3" ||
         key == "J6" || key == "J7" || key == "J8" ||
         key == "JC" || key == "JD" || key == "JE" ||
         key == "J11" || key == "J12" || key == "J13" ||
         key == "J17" || key == "J18" || key == "J19";
}

static const char *const ARGOS_FULL_CONFIG_KEYS[] = {
    "B2", "B3", "C1", "C2", "C3", "C4", "C5", "C7", "C9",
    "E1", "E2", "E3", "E4", "E5",
    "F2", "F3", "F4",
    "G2", "G3", "G4", "G5",
    "H2", "H3", "H4", "H5", "H6", "H7",
    "I2", "I3", "I4",
    "J1", "J2", "J3", "J6", "J7", "J8", "JC", "JD", "JE",
    "J11", "J12", "J13", "J17", "J18", "J19",
    "L2", "L3", "J1C", "J1D", "J1E",
    "K3", "K4", "K5",
};

static const char *const ARGOS_WRITABLE_KEYS[] = {
    "B2", "B3",
    "C1", "C2", "C3", "C4", "C5",
    "E1", "E2", "E3", "E4", "E5",
    "F2", "F3", "F4",
    "G2", "G3", "G4", "G5",
    "H2", "H3", "H4", "H5", "H6", "H7",
    "I2", "I3", "I4",
    "K3", "K4", "K5",
};

bool ArgosPidComponent::is_known_post_key_(const std::string &key) {
  // Pola telemetryczne już rozpoznane.
  static const char *const mapped_telemetry[] = {
      "A0",  // identyfikator urządzenia
      "A2", "A3", "A4", "A5", "A6",
      "AF", "A10", "A18",
      "AB",  // numer bledu / alarm, ASCII HEX
  };

  for (const char *known : mapped_telemetry) {
    if (key == known)
      return true;
  }

  // Wszystkie pola należące do znanej konfiguracji traktujemy jako znane,
  // nawet jeśli część ich semantyki nie jest jeszcze opisana w HA.
  for (const char *known : ARGOS_FULL_CONFIG_KEYS) {
    if (key == known)
      return true;
  }

  return false;
}

void ArgosPidComponent::track_unknown_raw_changes_(
    const std::map<std::string, std::string> &fields) {
  std::map<std::string, std::string> current_unknown;

  for (const auto &kv : fields) {
    if (!is_known_post_key_(kv.first))
      current_unknown[kv.first] = kv.second;
  }

  // Pierwszy POST tylko ustala punkt odniesienia. Nie generujemy przy starcie
  // kilkudziesięciu fałszywych "zmian".
  if (!this->raw_baseline_ready_) {
    this->raw_unknown_fields_ = current_unknown;
    this->raw_baseline_ready_ = true;

    ESP_LOGI(TAG, "RAW monitor baseline: %u unknown POST field(s)",
             static_cast<unsigned>(current_unknown.size()));

    if (this->raw_last_change_text_sensor_ != nullptr) {
      std::ostringstream ss;
      ss << "Monitoring aktywny - " << current_unknown.size()
         << " nieznanych pol";
      this->raw_last_change_text_sensor_->publish_state(ss.str());
    }
    return;
  }

  std::vector<std::string> changes;

  // Nowe i zmienione pola.
  for (const auto &kv : current_unknown) {
    auto old = this->raw_unknown_fields_.find(kv.first);

    if (old == this->raw_unknown_fields_.end()) {
      std::ostringstream ss;
      ss << kv.first << ":<brak>->" << kv.second;
      changes.push_back(ss.str());
      ESP_LOGW(TAG, "RAW NEW FIELD: %s=%s", kv.first.c_str(), kv.second.c_str());
      continue;
    }

    if (lower_copy_(old->second) != lower_copy_(kv.second)) {
      std::ostringstream ss;
      ss << kv.first << ":" << old->second << "->" << kv.second;
      changes.push_back(ss.str());
      ESP_LOGW(TAG, "RAW CHANGE: %s %s -> %s", kv.first.c_str(),
               old->second.c_str(), kv.second.c_str());
    }
  }

  // Pola, które zniknęły z POST-a.
  for (const auto &kv : this->raw_unknown_fields_) {
    if (current_unknown.find(kv.first) == current_unknown.end()) {
      std::ostringstream ss;
      ss << kv.first << ":" << kv.second << "-><brak>";
      changes.push_back(ss.str());
      ESP_LOGW(TAG, "RAW FIELD REMOVED: %s old=%s", kv.first.c_str(),
               kv.second.c_str());
    }
  }

  if (!changes.empty() && this->raw_last_change_text_sensor_ != nullptr) {
    // Stan HA ma być krótki i czytelny. Pełne wszystkie zmiany są zawsze w logu.
    std::ostringstream summary;
    const size_t max_items = std::min<size_t>(changes.size(), 4);
    for (size_t i = 0; i < max_items; i++) {
      if (i != 0)
        summary << " | ";
      summary << changes[i];
    }
    if (changes.size() > max_items)
      summary << " | +" << (changes.size() - max_items) << " zmian";

    std::string text = summary.str();
    if (text.size() > 240)
      text = text.substr(0, 237) + "...";
    this->raw_last_change_text_sensor_->publish_state(text);
  }

  this->raw_unknown_fields_ = std::move(current_unknown);
}

void ArgosPidComponent::setup() {
  ESP_LOGI(TAG, "Argos PID v1.1 starting");
  ESP_LOGI(TAG, "Mode: %s", this->proxy_mode_ ? "PROXY / DIAGNOSTIC" : "LOCAL / HA CONFIG");
  ESP_LOGI(TAG, "DNS/HTTP bind IP: %s", this->bind_ip_.c_str());
  ESP_LOGI(TAG, "Expected Argos IP: %s", this->expected_device_ip_.c_str());
  if (this->proxy_mode_) {
    ESP_LOGW(TAG, "PROXY MODE: cloud responses are forwarded to Argos. Cloud configuration changes CAN reach the boiler.");
    ESP_LOGW(TAG, "v0.5: proxy is diagnostic; local HA configuration is disabled while proxy is ON.");
    ESP_LOGI(TAG, "Upstream: %s (%s):%u, timeout=%lu ms, local_fallback=%s", this->upstream_host_.c_str(),
             this->upstream_ip_.c_str(), this->upstream_port_, static_cast<unsigned long>(this->proxy_timeout_ms_),
             YESNO(this->proxy_fallback_local_));
  } else {
    ESP_LOGW(TAG, "LOCAL MODE: cloud forwarding OFF; HA edits are staged until explicit ZAPISZ");
  }
  this->next_server_start_attempt_ms_ = millis() + 1500;
}

void ArgosPidComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Argos PID:");
  ESP_LOGCONFIG(TAG, "  Version: 1.1");
  ESP_LOGCONFIG(TAG, "  Mode: %s", this->proxy_mode_ ? "PROXY / DIAGNOSTIC" : "LOCAL / HA CONFIG");
  ESP_LOGCONFIG(TAG, "  Bind IP: %s", this->bind_ip_.c_str());
  ESP_LOGCONFIG(TAG, "  Expected device IP: %s", this->expected_device_ip_.c_str());
