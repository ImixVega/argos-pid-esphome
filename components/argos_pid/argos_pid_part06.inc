void ArgosPidComponent::stage_write_decimal(const std::string &key, uint32_t value) {
  if (this->proxy_mode_) {
    this->set_write_status_("Edycja zablokowana - najpierw wylacz Argos Chmura / Proxy");
    ESP_LOGW(TAG, "STAGE rejected for %s: proxy is enabled", key.c_str());
    return;
  }
  if (!this->shadow_ready_) {
    this->set_write_status_("Edycja zablokowana - czekam na pierwszy poprawny POST Argosa");
    ESP_LOGW(TAG, "STAGE rejected for %s: controller state not initialized", key.c_str());
    return;
  }
  if (this->write_in_flight_ || this->save_requested_) {
    this->set_write_status_("Edycja chwilowo zablokowana - trwa zapis konfiguracji");
    ESP_LOGW(TAG, "STAGE rejected for %s: save transaction already requested/in flight", key.c_str());
    return;
  }
  if (!this->is_allowed_write_key_(key)) {
    this->set_write_status_("Odrzucono zmiane - pole poza biala lista v0.5");
    ESP_LOGE(TAG, "STAGE rejected: key %s is not on whitelist", key.c_str());
    return;
  }

  auto live = this->live_fields_.find(key);
  if (live == this->live_fields_.end() || live->second.empty()) {
    this->set_write_status_("Odrzucono zmiane - pola nie ma w aktualnym stanie Argosa");
    ESP_LOGW(TAG, "STAGE rejected for %s: key absent in live Argos state", key.c_str());
    return;
  }

  const std::string raw = hex_string_(value);
  if (lower_copy_(live->second) == lower_copy_(raw)) {
    this->pending_writes_.erase(key);
    ESP_LOGI(TAG, "STAGE reverted/no-op: %s=%s equals live controller value", key.c_str(), raw.c_str());
  } else {
    this->pending_writes_[key] = raw;
    ESP_LOGI(TAG, "STAGED: %s=%s (%lu dec)", key.c_str(), raw.c_str(), static_cast<unsigned long>(value));
  }

  if (this->pending_writes_.empty()) {
    this->set_write_status_("Brak niezapisanych zmian");
  } else {
    std::ostringstream ss;
    ss << "Niezapisane zmiany: " << this->pending_writes_.size() << " - nacisnij 'Argos Zapisz nastawy'";
    this->set_write_status_(ss.str());
  }
}

void ArgosPidComponent::request_save() {
  if (this->proxy_mode_) {
    this->set_write_status_("Nie zapisano - wylacz Argos Chmura / Proxy");
    ESP_LOGW(TAG, "SAVE rejected: proxy is enabled");
    return;
  }
  if (!this->shadow_ready_) {
    this->set_write_status_("Nie zapisano - czekam na pierwszy poprawny POST Argosa");
    return;
  }
  if (this->write_in_flight_) {
    this->set_write_status_("Poprzedni zapis nadal czeka na potwierdzenie Argosa");
    return;
  }
  if (this->save_requested_) {
    this->set_write_status_("Zapis jest juz zlecony - czekam na nastepny POST Argosa");
    return;
  }
  if (this->pending_writes_.empty()) {
    this->set_write_status_("Brak niezapisanych zmian");
    return;
  }

  this->save_requested_ = true;
  std::ostringstream ss;
  ss << "Zapis zlecony: " << this->pending_writes_.size() << " zmiana/y - czekam na nastepny POST Argosa";
  this->set_write_status_(ss.str());
  ESP_LOGW(TAG, "SAVE requested by user: %u staged field(s)", static_cast<unsigned>(this->pending_writes_.size()));
}

void ArgosPidComponent::cancel_pending_writes() {
  if (this->write_in_flight_) {
    this->set_write_status_("Nie mozna anulowac - konfiguracja zostala juz wyslana do Argosa");
    return;
  }

  const size_t count = this->pending_writes_.size();
  this->pending_writes_.clear();
  this->save_requested_ = false;

  if (count == 0)
    this->set_write_status_(this->shadow_ready_ ? "Brak niezapisanych zmian" : "Czekam na pierwszy poprawny POST Argosa");
  else {
    std::ostringstream ss;
    ss << "Anulowano " << count << " niezapisanych zmian - przywrocono stan sterownika";
    this->set_write_status_(ss.str());
  }
  ESP_LOGW(TAG, "Cancelled %u staged configuration field(s)", static_cast<unsigned>(count));
}

bool ArgosPidComponent::all_config_fields_present_(const std::map<std::string, std::string> &fields) const {
  for (const char *key : ARGOS_FULL_CONFIG_KEYS) {
    auto it = fields.find(key);
    if (it == fields.end() || it->second.empty())
      return false;
  }
  return true;
}

bool ArgosPidComponent::is_full_config_response_(const std::string &body) const {
  const int fields = count_key_value_lines_(body);
  return fields >= 10 || (!response_value_(body, "B2").empty() && !response_value_(body, "G3").empty());
}

std::string ArgosPidComponent::build_config_response_(const std::map<std::string, std::string> &overrides) const {
  if (overrides.empty())
    return {};

  // Argos accepts FULL CONFIG only when hexadecimal digits a..f are lowercase.
  // Regular device POSTs use uppercase A..F, so every value copied from the
  // device must be normalized before it is echoed back.
  std::ostringstream ss;
  ss << "OK
";
  ss << "A35=8
";
  ss << "A36=0
";

  for (const char *key_c : ARGOS_FULL_CONFIG_KEYS) {
    const std::string key(key_c);

    if (argos_schedule_mask_key_(key)) {
      ss << key << "=ffffff
";
      continue;
    }

    if (argos_full_config_zero_fallback_(key)) {
      ss << key << "=0
";
      continue;
    }

    std::string raw;
    auto over = overrides.find(key);
    if (over != overrides.end()) {
      raw = over->second;
    } else {
      auto live = this->live_fields_.find(key);
      if (live == this->live_fields_.end() || live->second.empty())
        return {};
      raw = live->second;
    }

    ss << key << "=" << lower_copy_(raw) << "
";
  }

  return ss.str();
}

bool ArgosPidComponent::prepare_pending_write_response_(std::string &body) {
  body.clear();

  if (!this->save_requested_)
    return false;
  if (this->proxy_mode_) {
    this->save_requested_ = false;
    this->set_write_status_("Anulowano zapis - proxy zostalo wlaczone");
    return false;
  }
  if (this->pending_writes_.empty()) {
    this->save_requested_ = false;
    this->set_write_status_("Brak niezapisanych zmian");
    return false;
  }
  if (this->write_in_flight_)
    return false;

  // The controller ignored partial config responses in the v0.5 test.
  // Before sending a cloud-compatible FULL CONFIG, verify that every one of
  // the 53 device-side config fields is present in this fresh POST. If even
  // one is missing, refuse the write rather than replaying stale data.
  std::ostringstream missing;
  size_t missing_count = 0;
  for (const char *key : ARGOS_FULL_CONFIG_KEYS) {
    auto it = this->live_fields_.find(key);
    if (it == this->live_fields_.end() || it->second.empty()) {
      if (argos_full_config_zero_fallback_(key))
        continue;

      if (missing_count != 0)
        missing << ",";
      missing << key;
      missing_count++;
    }
  }
  if (missing_count != 0) {
    std::ostringstream ss;
    ss << "Zapis wstrzymany - aktualny POST nie zawiera " << missing_count
       << " pol FULL CONFIG: " << missing.str();
    this->set_write_status_(ss.str());
    ESP_LOGE(TAG, "FULL CONFIG refused: fresh Argos POST misses %u field(s): %s",
             static_cast<unsigned>(missing_count), missing.str().c_str());
    return false;
  }

