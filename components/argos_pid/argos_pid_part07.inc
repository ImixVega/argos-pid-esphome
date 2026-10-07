  // Called immediately after parsing the current Argos POST, therefore
  // live_fields_ is fresh. Revalidate the complete staged form against it.
  std::map<std::string, std::string> to_send;
  for (const auto &kv : this->pending_writes_) {
    auto live = this->live_fields_.find(kv.first);
    if (live == this->live_fields_.end() || live->second.empty()) {
      std::ostringstream ss;
      ss << "Zapis wstrzymany - pole " << kv.first << " nieobecne w aktualnym POST Argosa";
      this->set_write_status_(ss.str());
      return false;
    }

    if (lower_copy_(live->second) == lower_copy_(kv.second)) {
      ESP_LOGI(TAG, "SAVE no-op on fresh POST: %s already equals %s", kv.first.c_str(), kv.second.c_str());
      continue;
    }
    to_send[kv.first] = kv.second;
  }

  if (to_send.empty()) {
    this->pending_writes_.clear();
    this->save_requested_ = false;
    this->set_write_status_("Brak zmian do zapisania - sterownik ma juz wartosci z formularza");
    return false;
  }

  // Validate the effective configuration before sending. This catches
  // out-of-range values and, importantly, prevents v0.5c from preserving
  // collateral corruption caused by uppercase hex in v0.5b.
  auto effective_raw = [&](const char *key) -> std::string {
    auto staged = to_send.find(key);
    if (staged != to_send.end())
      return staged->second;
    auto live = this->live_fields_.find(key);
    if (live != this->live_fields_.end())
      return live->second;
    return {};
  };

  std::vector<std::string> invalid_fields;
  auto check_range = [&](const char *key, uint32_t min_v, uint32_t max_v) {
    const std::string raw = effective_raw(key);
    if (raw.empty()) {
      invalid_fields.emplace_back(std::string(key) + "=missing");
      return;
    }
    char *end = nullptr;
    const unsigned long v = std::strtoul(raw.c_str(), &end, 16);
    if (end == raw.c_str() || *end != '\0' || v < min_v || v > max_v) {
      std::ostringstream item;
      item << key << "=" << raw;
      invalid_fields.push_back(item.str());
    }
  };

  check_range("B2", 20, 79);  check_range("B3", 20, 79);
  check_range("C1", 5, 20);   check_range("C2", 3, 220);
  check_range("C3", 1, 98);   check_range("C4", 2, 80);
  check_range("C5", 3, 40);
  check_range("E1", 10, 240); check_range("E2", 1, 240);
  check_range("E3", 5, 80);   check_range("E4", 1, 10);
  check_range("E5", 1, 10);
  check_range("F2", 0, 2);    check_range("F3", 10, 60);
  check_range("F4", 2, 9);
  check_range("G2", 0, 2);    check_range("G3", 41, 80);
  check_range("G4", 2, 40);   check_range("G5", 0, 9);
  check_range("H2", 0, 2);    check_range("H3", 10, 70);
  check_range("H4", 10, 70);  check_range("H5", 2, 9);
  check_range("H6", 1, 99);   check_range("H7", 1, 99);
  check_range("I2", 0, 1);    check_range("I3", 20, 70);
  check_range("I4", 20, 70);
  check_range("K3", 0, 2);    check_range("K4", 2, 20);
  check_range("K5", 0, 1);

  if (!invalid_fields.empty()) {
    this->save_requested_ = false;
    std::ostringstream ss;
    ss << "Zapis anulowany - popraw nieprawidlowe pola: ";
    for (size_t i = 0; i < invalid_fields.size(); i++) {
      if (i != 0)
        ss << ", ";
      ss << invalid_fields[i];
    }
    this->set_write_status_(ss.str());
    ESP_LOGE(TAG, "FULL CONFIG validation failed: %s", ss.str().c_str());
    return false;
  }

  ESP_LOGI(TAG, "FULL CONFIG: lowercase hex + schedule masks ffffff + fixed 0 fallback for L2,L3,J1C,J1D,J1E");
  body = this->build_config_response_(to_send);
  if (body.empty()) {
    this->set_write_status_("Blad budowy FULL CONFIG - nic nie wyslano");
    return false;
  }

  this->expected_writes_ = to_send;
  this->pending_writes_.clear();
  this->save_requested_ = false;
  this->write_in_flight_ = true;
  this->write_confirm_packets_ = 0;

  std::ostringstream ss;
  ss << "Wyslano " << this->expected_writes_.size() << " zmiana/y - czekam na potwierdzenie Argosa";
  this->set_write_status_(ss.str());
  return true;
}

void ArgosPidComponent::check_write_confirmation_(const std::map<std::string, std::string> &fields) {
  if (!this->write_in_flight_ || this->expected_writes_.empty())
    return;

  bool all_match = true;
  for (const auto &kv : this->expected_writes_) {
    auto it = fields.find(kv.first);
    if (it == fields.end() || lower_copy_(it->second) != lower_copy_(kv.second)) {
      all_match = false;
      break;
    }
  }

  if (all_match) {
    const size_t count = this->expected_writes_.size();
    this->expected_writes_.clear();
    this->write_in_flight_ = false;
    this->write_confirm_packets_ = 0;
    std::ostringstream ss;
    ss << "ZAPISANO i potwierdzono przez Argos: " << count << " zmiana/y";
    this->set_write_status_(ss.str());
    ESP_LOGI(TAG, "LOCAL WRITE CONFIRMED by Argos (%u field(s))", static_cast<unsigned>(count));
    return;
  }

  this->write_confirm_packets_++;
  if (this->write_confirm_packets_ >= 3) {
    ESP_LOGE(TAG, "LOCAL WRITE confirmation FAILED after %lu Argos POSTs",
             static_cast<unsigned long>(this->write_confirm_packets_));
    // Keep failed values in the working form so the user can inspect and retry.
    for (const auto &kv : this->expected_writes_)
      this->pending_writes_[kv.first] = kv.second;
    const size_t count = this->pending_writes_.size();
    this->expected_writes_.clear();
    this->write_in_flight_ = false;
    this->write_confirm_packets_ = 0;
    std::ostringstream ss;
    ss << "BLAD: Argos nie potwierdzil zapisu; " << count << " zmiana/y pozostalo niezapisanych";
    this->set_write_status_(ss.str());
  }
}

std::string ArgosPidComponent::masked_device_id_() const {
  if (this->device_id_.empty())
    return "?";
  if (this->device_id_.size() <= 4)
    return "****";
  return "********" + this->device_id_.substr(this->device_id_.size() - 4);
}

bool ArgosPidComponent::hex_value_(const std::map<std::string, std::string> &fields, const char *key, uint32_t &value) {
  auto it = fields.find(key);
  if (it == fields.end() || it->second.empty())
    return false;
  char *end = nullptr;
  const unsigned long v = std::strtoul(it->second.c_str(), &end, 16);
  if (end == it->second.c_str() || *end != '\0')
    return false;
  value = static_cast<uint32_t>(v);
  return true;
}

const char *ArgosPidComponent::alarm_message_for_code_(uint32_t code) {
  switch (code) {
    case 0: return "OK";
    case 1: return "Blad czujnika kotla";
    case 2: return "Blad czujnika podajnika";
    case 3: return "Blad czujnika CWU";
    case 4: return "Blad czujnika zaworu mieszajacego";
    case 5: return "Blad zapisanych ustawien";
    case 6: return "Blad zegara RTC";
    case 7: return "Blad zapisanych ustawien";
    case 8: return "Piec przegrzany";
    case 9: return "Przegrzanie podajnika";
    case 10: return "STB - zadzialalo zabezpieczenie";
    case 11: return "Kociol wygaszony";
    case 12: return "Konczy sie opal w zasobniku";
    case 13: return "Blad synchronizacji z siecia";
