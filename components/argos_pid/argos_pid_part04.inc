  const int old_flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, old_flags | O_NONBLOCK);

  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(this->upstream_port_);
  dst.sin_addr = upstream;

  const uint32_t started = millis();
  int rc = connect(fd, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
  if (rc < 0 && errno != EINPROGRESS) {
    ESP_LOGW(TAG, "CLOUD connect(%s:%u) failed immediately: errno=%d", this->upstream_ip_.c_str(),
             this->upstream_port_, errno);
    close(fd);
    return false;
  }

  if (rc < 0) {
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    timeval ctv{};
    ctv.tv_sec = this->proxy_timeout_ms_ / 1000;
    ctv.tv_usec = (this->proxy_timeout_ms_ % 1000) * 1000;
    rc = select(fd + 1, nullptr, &wfds, nullptr, &ctv);
    if (rc <= 0) {
      ESP_LOGW(TAG, "CLOUD connect timeout after %lu ms", static_cast<unsigned long>(this->proxy_timeout_ms_));
      close(fd);
      return false;
    }
    int so_error = 0;
    socklen_t so_len = sizeof(so_error);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len);
    if (so_error != 0) {
      ESP_LOGW(TAG, "CLOUD connect failed: errno=%d", so_error);
      close(fd);
      return false;
    }
  }

  // Switch back to blocking I/O with the socket timeouts above.
  fcntl(fd, F_SETFL, old_flags & ~O_NONBLOCK);

  size_t sent = 0;
  while (sent < request.size()) {
    const int n = send(fd, request.data() + sent, request.size() - sent, 0);
    if (n <= 0) {
      ESP_LOGW(TAG, "CLOUD send failed: errno=%d", errno);
      close(fd);
      return false;
    }
    sent += static_cast<size_t>(n);
  }

  std::string raw;
  raw.reserve(2048);
  char buf[768];
  while (raw.size() < 16384) {
    const int n = recv(fd, buf, sizeof(buf), 0);
    if (n > 0) {
      raw.append(buf, n);
      continue;
    }
    if (n == 0)
      break;
    if (errno == EWOULDBLOCK || errno == EAGAIN)
      break;
    ESP_LOGW(TAG, "CLOUD recv failed: errno=%d", errno);
    close(fd);
    return false;
  }
  close(fd);
  latency_ms = millis() - started;

  const size_t header_end = raw.find("

");
  if (header_end == std::string::npos) {
    ESP_LOGW(TAG, "CLOUD invalid HTTP response: no header terminator (%u bytes)", static_cast<unsigned>(raw.size()));
    return false;
  }
  const std::string headers = raw.substr(0, header_end + 4);
  status_code = parse_http_status_(headers);
  response_body = raw.substr(header_end + 4);

  // If Content-Length exists, trim any accidental trailing bytes. The current
  // e-kotlownia response is plain HTTP/1.x, non-chunked.
  const size_t content_length = parse_content_length_(headers);
  if (content_length > 0 && response_body.size() >= content_length)
    response_body.resize(content_length);

  if (status_code <= 0) {
    ESP_LOGW(TAG, "CLOUD invalid status line");
    return false;
  }
  return true;
}

int ArgosPidComponent::count_key_value_lines_(const std::string &body) {
  int count = 0;
  size_t pos = 0;
  while (pos <= body.size()) {
    const size_t nl = body.find('
', pos);
    std::string line = body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    if (!line.empty() && line.back() == '')
      line.pop_back();
    if (line.find('=') != std::string::npos)
      count++;
    if (nl == std::string::npos)
      break;
    pos = nl + 1;
  }
  return count;
}

std::string ArgosPidComponent::response_value_(const std::string &body, const std::string &key) {
  const std::string prefix = key + "=";
  size_t pos = 0;
  while (pos <= body.size()) {
    const size_t nl = body.find('
', pos);
    std::string line = body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    if (!line.empty() && line.back() == '')
      line.pop_back();
    if (line.rfind(prefix, 0) == 0)
      return line.substr(prefix.size());
    if (nl == std::string::npos)
      break;
    pos = nl + 1;
  }
  return {};
}

void ArgosPidComponent::learn_normal_response_(const std::string &body) {
  const std::string a35 = response_value_(body, "A35");
  const std::string a36 = response_value_(body, "A36");
  const std::string a37 = response_value_(body, "A37");
  if (!a35.empty())
    this->response_a35_ = a35;
  if (!a36.empty())
    this->response_a36_ = a36;
  if (!a37.empty())
    this->response_a37_ = a37;
}

void ArgosPidComponent::log_body_chunks_(const char *prefix, const std::string &body) {
  // Keep each log line short enough for ESPHome's logger buffer.
  constexpr size_t CHUNK = 180;
  if (body.empty()) {
    ESP_LOGI(TAG, "%s <empty>", prefix);
    return;
  }
  for (size_t pos = 0; pos < body.size(); pos += CHUNK) {
    std::string part = body.substr(pos, CHUNK);
    std::replace(part.begin(), part.end(), '', ' ');
    std::replace(part.begin(), part.end(), '
', '|');
    ESP_LOGI(TAG, "%s %s", prefix, part.c_str());
  }
}

void ArgosPidComponent::process_cloud_response_(const std::string &body, int status_code, uint32_t latency_ms) {
  const int fields = count_key_value_lines_(body);
  const bool full_config = this->is_full_config_response_(body);
  const std::string type = full_config ? "full_config" : "normal";

  publish_(this->cloud_latency_ms_sensor_, static_cast<float>(latency_ms));
  publish_(this->cloud_status_code_sensor_, static_cast<float>(status_code));
  publish_(this->cloud_response_fields_sensor_, static_cast<float>(fields));

  if (this->cloud_response_type_text_sensor_ != nullptr && type != this->last_cloud_response_type_) {
    this->cloud_response_type_text_sensor_->publish_state(type);
    this->last_cloud_response_type_ = type;
  }

  std::ostringstream summary;
  summary << type << " HTTP " << status_code << " " << latency_ms << "ms " << fields << " fields";
  const std::string a35 = response_value_(body, "A35");
  const std::string a36 = response_value_(body, "A36");
  const std::string a37 = response_value_(body, "A37");
  if (!a35.empty()) summary << " A35=" << a35;
  if (!a36.empty()) summary << " A36=" << a36;
  if (!a37.empty()) summary << " A37=" << a37;
  if (this->cloud_last_summary_text_sensor_ != nullptr)
    this->cloud_last_summary_text_sensor_->publish_state(summary.str());

  if (full_config) {
    ESP_LOGW(TAG, "CLOUD -> ARGOS FULL CONFIG: HTTP=%d latency=%lums fields=%d bytes=%u", status_code, static_cast<unsigned long>(latency_ms), fields,
             static_cast<unsigned>(body.size()));
    ESP_LOGW(TAG, "A full cloud configuration response means a pending cloud-side configuration write may be applied by Argos.");
    this->log_body_chunks_("CLOUD CONFIG:", body);
  } else {
    this->learn_normal_response_(body);
    ESP_LOGI(TAG, "CLOUD -> ARGOS normal: HTTP=%d latency=%lums fields=%d A35=%s A36=%s A37=%s", status_code,
             static_cast<unsigned long>(latency_ms), fields, this->response_a35_.c_str(), this->response_a36_.c_str(), this->response_a37_.c_str());
    ESP_LOGV(TAG, "CLOUD RAW: %s", body.c_str());
  }
}
