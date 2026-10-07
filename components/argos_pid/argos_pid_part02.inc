  ESP_LOGCONFIG(TAG, "  DNS: UDP/53");
  ESP_LOGCONFIG(TAG, "  HTTP: TCP/80 /sz/ster7.php");
  if (this->proxy_mode_) {
    ESP_LOGCONFIG(TAG, "  Upstream host: %s", this->upstream_host_.c_str());
    ESP_LOGCONFIG(TAG, "  Upstream IP: %s:%u", this->upstream_ip_.c_str(), this->upstream_port_);
    ESP_LOGCONFIG(TAG, "  Proxy timeout: %lu ms", static_cast<unsigned long>(this->proxy_timeout_ms_));
    ESP_LOGCONFIG(TAG, "  Local fallback: %s", YESNO(this->proxy_fallback_local_));
  }
  ESP_LOGCONFIG(TAG, "  LOCAL/fallback response: A35=%s A36=%s A37=%s", this->response_a35_.c_str(),
                this->response_a36_.c_str(), this->response_a37_.c_str());
}

void ArgosPidComponent::loop() {
  const uint32_t loop_start_ms = millis();

  if (!this->servers_started_ &&
      static_cast<int32_t>(loop_start_ms - this->next_server_start_attempt_ms_) >= 0) {
    if (!this->start_servers_())
      this->next_server_start_attempt_ms_ = loop_start_ms + 5000;
  }

  if (this->servers_started_) {
    this->handle_dns_();
    this->handle_http_accept_();
    this->handle_http_client_();
  }

  // Use a fresh millis() after HTTP processing. This avoids unsigned underflow
  // when process_argos_body_() updates last_seen_ms_ inside the same loop pass.
  const uint32_t timeout_now_ms = millis();
  if (this->online_state_ && this->last_seen_ms_ != 0) {
    const uint32_t age_ms = timeout_now_ms - this->last_seen_ms_;
    if (age_ms > this->online_timeout_ms_) {
      this->online_state_ = false;
      if (this->online_binary_sensor_ != nullptr)
        this->online_binary_sensor_->publish_state(false);
      ESP_LOGW(TAG, "Argos offline: no POST for %lu ms (age=%lu ms)", static_cast<unsigned long>(this->online_timeout_ms_), static_cast<unsigned long>(age_ms));
    }
  }
}

bool ArgosPidComponent::start_servers_() {
  this->stop_servers_();

  in_addr bind_addr{};
  if (inet_aton(this->bind_ip_.c_str(), &bind_addr) == 0) {
    ESP_LOGE(TAG, "Invalid bind_ip: %s", this->bind_ip_.c_str());
    return false;
  }

  this->dns_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (this->dns_fd_ < 0) {
    ESP_LOGE(TAG, "DNS socket() failed: errno=%d", errno);
    return false;
  }
  fcntl(this->dns_fd_, F_SETFL, O_NONBLOCK);

  sockaddr_in dns_addr{};
  dns_addr.sin_family = AF_INET;
  dns_addr.sin_port = htons(53);
  dns_addr.sin_addr = bind_addr;
  if (bind(this->dns_fd_, reinterpret_cast<sockaddr *>(&dns_addr), sizeof(dns_addr)) < 0) {
    ESP_LOGE(TAG, "DNS bind(%s:53) failed: errno=%d", this->bind_ip_.c_str(), errno);
    this->stop_servers_();
    return false;
  }

  this->http_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (this->http_fd_ < 0) {
    ESP_LOGE(TAG, "HTTP socket() failed: errno=%d", errno);
    this->stop_servers_();
    return false;
  }
  int reuse = 1;
  setsockopt(this->http_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  fcntl(this->http_fd_, F_SETFL, O_NONBLOCK);

  sockaddr_in http_addr{};
  http_addr.sin_family = AF_INET;
  http_addr.sin_port = htons(80);
  http_addr.sin_addr = bind_addr;
  if (bind(this->http_fd_, reinterpret_cast<sockaddr *>(&http_addr), sizeof(http_addr)) < 0) {
    ESP_LOGE(TAG, "HTTP bind(%s:80) failed: errno=%d", this->bind_ip_.c_str(), errno);
    this->stop_servers_();
    return false;
  }
  if (listen(this->http_fd_, 2) < 0) {
    ESP_LOGE(TAG, "HTTP listen() failed: errno=%d", errno);
    this->stop_servers_();
    return false;
  }

  this->servers_started_ = true;
  ESP_LOGI(TAG, "DNS listening on udp://%s:53", this->bind_ip_.c_str());
  ESP_LOGI(TAG, "HTTP listening on http://%s:80", this->bind_ip_.c_str());
  return true;
}

void ArgosPidComponent::stop_servers_() {
  this->close_http_client_();
  if (this->dns_fd_ >= 0) {
    close(this->dns_fd_);
    this->dns_fd_ = -1;
  }
  if (this->http_fd_ >= 0) {
    close(this->http_fd_);
    this->http_fd_ = -1;
  }
  this->servers_started_ = false;
}

bool ArgosPidComponent::parse_dns_name_(const uint8_t *buf, size_t len, size_t offset, std::string &name,
                                        size_t &after_name) {
  name.clear();
  while (offset < len) {
    const uint8_t label_len = buf[offset++];
    if (label_len == 0) {
      after_name = offset;
      return true;
    }
    if ((label_len & 0xC0) != 0 || label_len > 63 || offset + label_len > len)
      return false;
    if (!name.empty())
      name.push_back('.');
    name.append(reinterpret_cast<const char *>(buf + offset), label_len);
    offset += label_len;
  }
  return false;
}

std::string ArgosPidComponent::lower_(std::string v) {
  std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return v;
}

void ArgosPidComponent::handle_dns_() {
  uint8_t query[512];
  sockaddr_in src{};
  socklen_t src_len = sizeof(src);
  const int n = recvfrom(this->dns_fd_, query, sizeof(query), 0, reinterpret_cast<sockaddr *>(&src), &src_len);
  if (n < 0) {
    if (errno != EWOULDBLOCK && errno != EAGAIN)
      ESP_LOGW(TAG, "DNS recvfrom failed: errno=%d", errno);
    return;
  }
  if (n < 12)
    return;

  char src_ip[INET_ADDRSTRLEN]{};
  inet_ntop(AF_INET, &src.sin_addr, src_ip, sizeof(src_ip));
  if (!this->expected_device_ip_.empty() && this->expected_device_ip_ != src_ip) {
    ESP_LOGW(TAG, "Ignoring DNS from unexpected IP %s", src_ip);
    return;
  }

  const uint16_t qdcount = (static_cast<uint16_t>(query[4]) << 8) | query[5];
  if (qdcount < 1)
    return;

  std::string qname;
  size_t after_name = 0;
  if (!parse_dns_name_(query, static_cast<size_t>(n), 12, qname, after_name) ||
      after_name + 4 > static_cast<size_t>(n))
    return;

  const uint16_t qtype = (static_cast<uint16_t>(query[after_name]) << 8) | query[after_name + 1];
  qname = lower_(qname);

  const bool supported_name = qname == "www.ekotlownia.pl" || qname == "ekotlownia.pl";
  const bool answer_a = supported_name && qtype == 1;

  std::vector<uint8_t> response(query, query + n);
  response[2] = 0x81;
  response[3] = answer_a ? 0x80 : 0x83;
  response[4] = 0x00;
  response[5] = 0x01;
  response[6] = 0x00;
  response[7] = answer_a ? 0x01 : 0x00;
  response[8] = response[9] = response[10] = response[11] = 0x00;

  if (answer_a) {
    in_addr target{};
    inet_aton(this->bind_ip_.c_str(), &target);
    response.push_back(0xC0);
    response.push_back(0x0C);
    response.push_back(0x00);
    response.push_back(0x01);
    response.push_back(0x00);
    response.push_back(0x01);
    response.push_back(0x00);
    response.push_back(0x00);
    response.push_back(0x00);
    response.push_back(0x3C);
    response.push_back(0x00);
    response.push_back(0x04);
    const uint8_t *ip = reinterpret_cast<const uint8_t *>(&target.s_addr);
    response.insert(response.end(), ip, ip + 4);
  }

  sendto(this->dns_fd_, response.data(), response.size(), 0, reinterpret_cast<sockaddr *>(&src), src_len);
  if (answer_a)
