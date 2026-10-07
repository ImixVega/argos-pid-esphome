    ESP_LOGD(TAG, "DNS %s from %s -> %s", qname.c_str(), src_ip, this->bind_ip_.c_str());
  else {
    ESP_LOGV(TAG, "DNS %s type=%u from %s -> NXDOMAIN", qname.c_str(), qtype, src_ip);
  }
}

void ArgosPidComponent::handle_http_accept_() {
  if (this->client_fd_ >= 0)
    return;

  sockaddr_in src{};
  socklen_t src_len = sizeof(src);
  const int fd = accept(this->http_fd_, reinterpret_cast<sockaddr *>(&src), &src_len);
  if (fd < 0) {
    if (errno != EWOULDBLOCK && errno != EAGAIN)
      ESP_LOGW(TAG, "HTTP accept failed: errno=%d", errno);
    return;
  }

  char src_ip[INET_ADDRSTRLEN]{};
  inet_ntop(AF_INET, &src.sin_addr, src_ip, sizeof(src_ip));
  if (!this->expected_device_ip_.empty() && this->expected_device_ip_ != src_ip) {
    ESP_LOGW(TAG, "Rejecting HTTP client from unexpected IP %s", src_ip);
    close(fd);
    return;
  }

  this->client_fd_ = fd;
  fcntl(this->client_fd_, F_SETFL, O_NONBLOCK);
  this->client_buffer_.clear();
  this->client_started_ms_ = millis();
}

size_t ArgosPidComponent::parse_content_length_(const std::string &headers) {
  std::string lower = lower_(headers);
  const std::string needle = "content-length:";
  const size_t pos = lower.find(needle);
  if (pos == std::string::npos)
    return 0;
  size_t start = pos + needle.size();
  while (start < headers.size() && std::isspace(static_cast<unsigned char>(headers[start])))
    start++;
  size_t end = start;
  while (end < headers.size() && std::isdigit(static_cast<unsigned char>(headers[end])))
    end++;
  if (end == start)
    return 0;
  return static_cast<size_t>(std::strtoul(headers.substr(start, end - start).c_str(), nullptr, 10));
}

int ArgosPidComponent::parse_http_status_(const std::string &headers) {
  const size_t line_end = headers.find("
");
  const std::string line = headers.substr(0, line_end);
  const size_t first_space = line.find(' ');
  if (first_space == std::string::npos)
    return 0;
  return std::atoi(line.c_str() + first_space + 1);
}

void ArgosPidComponent::handle_http_client_() {
  if (this->client_fd_ < 0)
    return;

  char buf[768];
  while (true) {
    const int n = recv(this->client_fd_, buf, sizeof(buf), 0);
    if (n > 0) {
      this->client_buffer_.append(buf, n);
      if (this->client_buffer_.size() > 8192) {
        ESP_LOGW(TAG, "HTTP request too large, closing");
        this->close_http_client_();
        return;
      }
      continue;
    }
    if (n == 0) {
      this->close_http_client_();
      return;
    }
    if (errno != EWOULDBLOCK && errno != EAGAIN) {
      ESP_LOGW(TAG, "HTTP recv failed: errno=%d", errno);
      this->close_http_client_();
      return;
    }
    break;
  }

  const size_t header_end = this->client_buffer_.find("

");
  if (header_end != std::string::npos) {
    const size_t content_length = parse_content_length_(this->client_buffer_.substr(0, header_end + 4));
    const size_t total_needed = header_end + 4 + content_length;
    if (this->client_buffer_.size() >= total_needed) {
      this->process_http_request_(this->client_buffer_.substr(0, total_needed));
      this->close_http_client_();
      return;
    }
  }

  if ((millis() - this->client_started_ms_) > 3000) {
    ESP_LOGW(TAG, "HTTP client timeout");
    this->close_http_client_();
  }
}

void ArgosPidComponent::close_http_client_() {
  if (this->client_fd_ >= 0) {
    close(this->client_fd_);
    this->client_fd_ = -1;
  }
  this->client_buffer_.clear();
}

void ArgosPidComponent::process_http_request_(const std::string &request) {
  const size_t first_line_end = request.find("
");
  const std::string first_line = first_line_end == std::string::npos ? request : request.substr(0, first_line_end);

  if (first_line.rfind("POST /sz/ster7.php ", 0) != 0) {
    ESP_LOGW(TAG, "Unexpected HTTP request: %s", first_line.c_str());
    this->send_http_response_("NOT FOUND
");
    return;
  }

  const size_t body_pos = request.find("

");
  if (body_pos == std::string::npos) {
    this->send_http_response_("BAD REQUEST
");
    return;
  }

  const std::string body = request.substr(body_pos + 4);
  this->process_argos_body_(body);

  // PROXY is intentionally a separate diagnostic mode in v0.5.
  // It is transparent: cloud response wins and local staged edits cannot be
  // committed while proxy is enabled.
  if (this->proxy_mode_) {
    std::string cloud_body;
    int status_code = 0;
    uint32_t latency_ms = 0;
    this->proxy_request_count_++;
    publish_(this->proxy_request_count_sensor_, static_cast<float>(this->proxy_request_count_));

    if (this->proxy_to_cloud_(request, cloud_body, status_code, latency_ms)) {
      this->set_cloud_online_(true);
      this->process_cloud_response_(cloud_body, status_code, latency_ms);
      this->send_http_response_(cloud_body);
      return;
    }

    this->set_cloud_online_(false);
    if (!this->proxy_fallback_local_) {
      ESP_LOGE(TAG, "Cloud proxy failed and local fallback is disabled");
      this->send_http_response_("ERROR
");
      return;
    }

    ESP_LOGW(TAG, "Cloud proxy failed -> LOCAL fallback A35=%s A36=%s A37=%s",
             this->response_a35_.c_str(), this->response_a36_.c_str(), this->response_a37_.c_str());
  } else {
    // LOCAL mode. A press of "Zapisz nastawy" only sets save_requested_.
    // The actual transaction is generated against this fresh Argos POST.
    std::string local_write_body;
    if (this->prepare_pending_write_response_(local_write_body)) {
      ESP_LOGW(TAG, "HA -> ARGOS SAVE: FULL CONFIG with %u changed field(s)",
               static_cast<unsigned>(this->expected_writes_.size()));
      ESP_LOGW(TAG, "HA FULL CONFIG frame: 55 configuration fields (A35=8 A36=0 + 53 current/overridden)");
      this->log_body_chunks_("HA FULL CONFIG:", local_write_body);
      this->send_http_response_(local_write_body);
      return;
    }
  }

  std::string response = "OK
A35=" + this->response_a35_ + "
A36=" + this->response_a36_ + "
A37=" +
                         this->response_a37_ + "
";
  this->send_http_response_(response);
}

bool ArgosPidComponent::proxy_to_cloud_(const std::string &request, std::string &response_body, int &status_code,
                                        uint32_t &latency_ms) {
  response_body.clear();
  status_code = 0;
  latency_ms = 0;

  in_addr upstream{};
  if (inet_aton(this->upstream_ip_.c_str(), &upstream) == 0) {
    ESP_LOGE(TAG, "Invalid upstream_ip: %s", this->upstream_ip_.c_str());
    return false;
  }

  const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    ESP_LOGE(TAG, "CLOUD socket() failed: errno=%d", errno);
    return false;
  }

  // send/recv safety net; connect timeout is handled with non-blocking select().
  timeval tv{};
  tv.tv_sec = this->proxy_timeout_ms_ / 1000;
  tv.tv_usec = (this->proxy_timeout_ms_ % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
