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
void ArgosPidComponent::set_cloud_online_(bool state) {
  if (!this->cloud_online_initialized_ || state != this->cloud_online_state_) {
    this->cloud_online_initialized_ = true;
    this->cloud_online_state_ = state;
    if (this->cloud_online_binary_sensor_ != nullptr)
      this->cloud_online_binary_sensor_->publish_state(state);
  }
}

std::string ArgosPidComponent::url_decode_(const std::string &v) {
  std::string out;
  out.reserve(v.size());
  for (size_t i = 0; i < v.size(); i++) {
    if (v[i] == '+') {
      out.push_back(' ');
    } else if (v[i] == '%' && i + 2 < v.size() && std::isxdigit(static_cast<unsigned char>(v[i + 1])) &&
               std::isxdigit(static_cast<unsigned char>(v[i + 2]))) {
      const std::string hex = v.substr(i + 1, 2);
      out.push_back(static_cast<char>(std::strtoul(hex.c_str(), nullptr, 16)));
      i += 2;
    } else {
      out.push_back(v[i]);
    }
  }
  return out;
}

void ArgosPidComponent::process_argos_body_(const std::string &body) {
  std::map<std::string, std::string> fields;
  size_t pos = 0;
  while (pos <= body.size()) {
    const size_t amp = body.find('&', pos);
    const std::string token = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    const size_t eq = token.find('=');
    if (eq != std::string::npos && eq > 0)
      fields[url_decode_(token.substr(0, eq))] = url_decode_(token.substr(eq + 1));
    if (amp == std::string::npos)
      break;
    pos = amp + 1;
  }

  auto id_it = fields.find("A0");
  if (id_it != fields.end())
    this->device_id_ = id_it->second;

  this->last_seen_ms_ = millis();
  this->packet_count_++;
  if (!this->online_state_) {
    this->online_state_ = true;
    if (this->online_binary_sensor_ != nullptr)
      this->online_binary_sensor_->publish_state(true);
  }

  uint32_t a2 = 0, b3 = 0, g3 = 0;
  hex_value_(fields, "A2", a2);
  hex_value_(fields, "B3", b3);
  hex_value_(fields, "G3", g3);
  const std::string masked_id = this->masked_device_id_();
  ESP_LOGI(TAG, "ARGOS POST #%lu ID=%s fields=%u A2=%s(%.1fC) B3=%s(%luC) G3=%s(%luC)", static_cast<unsigned long>(this->packet_count_),
           masked_id.c_str(), static_cast<unsigned>(fields.size()),
           fields.count("A2") ? fields.at("A2").c_str() : "?", a2 / 10.0f,
           fields.count("B3") ? fields.at("B3").c_str() : "?", static_cast<unsigned long>(b3),
           fields.count("G3") ? fields.at("G3").c_str() : "?", static_cast<unsigned long>(g3));
  ESP_LOGVV(TAG, "RAW BODY: %s", body.c_str());

  // v1.1: AB jest potwierdzonym polem numeru bledu. Monitor RAW pozostaje
  // tylko dla nadal nieznanych pol protokolu.
  this->track_unknown_raw_changes_(fields);

  this->live_fields_ = fields;

  // A regular Argos POST carries all currently known writable settings.
  // We keep the complete POST in RAM as the authoritative controller state.
  bool ready_now = true;
  for (const char *key : ARGOS_WRITABLE_KEYS) {
    auto it = fields.find(key);
    if (it == fields.end() || it->second.empty()) {
      ready_now = false;
      break;
    }
  }
  if (ready_now && !this->shadow_ready_) {
    this->shadow_ready_ = true;
    ESP_LOGI(TAG, "Initial Argos configuration loaded (%u POST fields)", static_cast<unsigned>(fields.size()));
    this->set_write_status_("Wczytano konfiguracje ze sterownika - brak niezapisanych zmian");
  }

  this->check_write_confirmation_(fields);
  this->publish_fields_(fields);
}




std::string ArgosPidComponent::lower_copy_(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
  return value;
}

std::string ArgosPidComponent::hex_string_(uint32_t value) {
  std::ostringstream ss;
  ss << std::hex << std::nouppercase << value;
  return ss.str();
}

float ArgosPidComponent::get_live_value(const std::string &key) const {
  auto it = this->live_fields_.find(key);
  if (it == this->live_fields_.end() || it->second.empty())
    return NAN;
  char *end = nullptr;
  const unsigned long v = std::strtoul(it->second.c_str(), &end, 16);
  if (end == it->second.c_str() || *end != ' ')
    return NAN;
  return static_cast<float>(v);
}

int ArgosPidComponent::get_live_int(const std::string &key, int fallback) const {
  const float v = this->get_live_value(key);
  if (std::isnan(v))
    return fallback;
  return static_cast<int>(v);
}

float ArgosPidComponent::get_edit_value(const std::string &key) const {
  auto pending = this->pending_writes_.find(key);
  if (pending != this->pending_writes_.end() && !pending->second.empty()) {
    char *end = nullptr;
    const unsigned long v = std::strtoul(pending->second.c_str(), &end, 16);
    if (end != pending->second.c_str() && *end == ' ')
      return static_cast<float>(v);
  }

  // While a SAVE transaction waits for confirmation, keep the requested
  // values visible in Home Assistant instead of snapping back to live values.
  auto expected = this->expected_writes_.find(key);
  if (expected != this->expected_writes_.end() && !expected->second.empty()) {
    char *end = nullptr;
    const unsigned long v = std::strtoul(expected->second.c_str(), &end, 16);
    if (end != expected->second.c_str() && *end == ' ')
      return static_cast<float>(v);
  }

  return this->get_live_value(key);
}

int ArgosPidComponent::get_edit_int(const std::string &key, int fallback) const {
  const float v = this->get_edit_value(key);
  if (std::isnan(v))
    return fallback;
  return static_cast<int>(v);
}

void ArgosPidComponent::set_write_status_(const std::string &status) {
  if (status == this->write_status_)
    return;
  this->write_status_ = status;
  ESP_LOGI(TAG, "CONFIG STATUS: %s", this->write_status_.c_str());
}

bool ArgosPidComponent::set_proxy_enabled(bool enabled) {
  if (enabled == this->proxy_mode_)
    return true;

  if (enabled) {
    if (!this->pending_writes_.empty() || this->save_requested_ || this->write_in_flight_) {
      this->set_write_status_("Nie wlaczono proxy - najpierw Zapisz albo Anuluj lokalne zmiany");
      ESP_LOGW(TAG, "PROXY enable rejected: local configuration transaction is pending");
      return false;
    }
    this->proxy_mode_ = true;
    this->set_write_status_("Proxy CHMURA wlaczone - lokalna edycja/zapis zablokowane");
    ESP_LOGW(TAG, "PROXY ENABLED by user: Argos traffic is forwarded to real cloud");
    return true;
  }

  this->proxy_mode_ = false;
  this->set_cloud_online_(false);
  if (!this->shadow_ready_)
    this->set_write_status_("Proxy wylaczone - czekam na pierwszy poprawny POST Argosa");
  else
    this->set_write_status_("Proxy wylaczone - tryb lokalny, brak niezapisanych zmian");
  ESP_LOGW(TAG, "PROXY DISABLED by user: LOCAL mode active");
  return true;
}

bool ArgosPidComponent::is_allowed_write_key_(const std::string &key) const {
  for (const char *allowed : ARGOS_WRITABLE_KEYS) {
    if (key == allowed)
      return true;
  }
  return false;
}

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
