#include "teslawbc_web.h"

#include <cmath>
#include <cstring>
#include <strings.h>

#include <lwip/sockets.h>

#include "esphome/components/json/json_util.h"
#include "esphome/components/sha256/sha256.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

#ifdef TESLAWBC_USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#endif

namespace esphome::teslawbc_web {

static const char *const TAG = "teslawbc_web";
static const uint32_t CONFIG_VERSION = 1;
static const size_t MAX_BODY = 2048;
static const size_t MIN_PASSWORD_LEN = 8;

// Defined by the Python code generator: the gzipped web page.
}  // namespace esphome::teslawbc_web
extern const uint8_t TESLAWBC_INDEX_HTML[];
extern const size_t TESLAWBC_INDEX_HTML_SIZE;
namespace esphome::teslawbc_web {

static void copy_str(char *dst, size_t size, const std::string &src) {
  size_t n = std::min(src.size(), size - 1);
  memcpy(dst, src.data(), n);
  dst[n] = '\0';
}

static std::string object_id(EntityBase *obj) {
  char buf[OBJECT_ID_MAX_LEN];
  return std::string(obj->get_object_id_to(buf).c_str());
}

void TeslaWbcWeb::setup() {
  this->pref_ = global_preferences->make_preference<StoredConfig>(fnv1_hash("teslawbc_web_config"), true);
  if (!this->pref_.load(&this->cfg_) || this->cfg_.version != CONFIG_VERSION) {
    memset(&this->cfg_, 0, sizeof(this->cfg_));
    this->cfg_.version = CONFIG_VERSION;
    this->cfg_.mqtt_port = this->default_mqtt_port_;
  }

  auto *base = web_server_base::global_web_server_base;
  base->init();
  base->add_handler(this);

  // MQTT starts disabled in the YAML; connect once the stored broker is known.
  this->defer([this]() { this->apply_mqtt_(); });
}

void TeslaWbcWeb::dump_config() {
  ESP_LOGCONFIG(TAG,
                "TeslaWBC web:\n"
                "  Setup done: %s\n"
                "  Device password set: %s\n"
                "  MQTT broker: %s:%u\n"
                "  Wall Connector: %s",
                YESNO(this->cfg_.setup_done), YESNO(this->cfg_.password_hash[0] != '\0'),
                this->cfg_.mqtt_host[0] ? this->cfg_.mqtt_host : "(not set)", this->cfg_.mqtt_port,
                this->cfg_.wc_host[0] ? this->cfg_.wc_host : "(not set)");
}

void TeslaWbcWeb::save_() {
  this->pref_.save(&this->cfg_);
  global_preferences->sync();
}

void TeslaWbcWeb::factory_reset() {
  ESP_LOGW(TAG, "Factory reset: clearing device password and connection settings");
  memset(&this->cfg_, 0, sizeof(this->cfg_));
  this->cfg_.version = CONFIG_VERSION;
  this->cfg_.mqtt_port = this->default_mqtt_port_;
  this->save_();
  this->apply_mqtt_();
}

void TeslaWbcWeb::apply_mqtt_() {
#ifdef TESLAWBC_USE_MQTT
  auto *m = mqtt::global_mqtt_client;
  if (m == nullptr)
    return;
  m->disable();
  if (!this->mqtt_configured()) {
    ESP_LOGI(TAG, "MQTT off: no broker set");
    return;
  }
  m->set_broker_address(this->cfg_.mqtt_host);
  m->set_broker_port(this->cfg_.mqtt_port);
  m->set_username(this->cfg_.mqtt_user);
  m->set_password(this->cfg_.mqtt_password);
  m->enable();
  ESP_LOGI(TAG, "MQTT broker %s:%u", this->cfg_.mqtt_host, this->cfg_.mqtt_port);
#endif
}

// ---------------------------------------------------------------------------
// Password: salted SHA-256, checked with HTTP Basic auth (any user name)
// ---------------------------------------------------------------------------

void TeslaWbcWeb::hash_password_(const char *salt, const std::string &password, char *out_hex) const {
  sha256::SHA256 h;
  h.init();
  h.add(reinterpret_cast<const uint8_t *>(salt), strlen(salt));
  h.add(reinterpret_cast<const uint8_t *>(password.data()), password.size());
  h.calculate();
  h.get_hex(out_hex);
}

void TeslaWbcWeb::set_password_(const std::string &password) {
  uint32_t a = random_uint32(), b = random_uint32();
  snprintf(this->cfg_.password_salt, sizeof(this->cfg_.password_salt), "%08x%08x", (unsigned) a, (unsigned) b);
  this->hash_password_(this->cfg_.password_salt, password, this->cfg_.password_hash);
}

uint32_t TeslaWbcWeb::client_key_(AsyncWebServerRequest *request) {
  // The client's IP address, hashed into a key for the login lockout.
  int fd = httpd_req_to_sockfd(*request);
  struct sockaddr_storage addr {};
  socklen_t len = sizeof(addr);
  if (fd < 0 || lwip_getpeername(fd, reinterpret_cast<struct sockaddr *>(&addr), &len) != 0)
    return 0;
  uint32_t h = 2166136261u;
  const uint8_t *p = nullptr;
  size_t n = 0;
  if (addr.ss_family == AF_INET) {
    auto *a = reinterpret_cast<struct sockaddr_in *>(&addr);
    p = reinterpret_cast<const uint8_t *>(&a->sin_addr);
    n = sizeof(a->sin_addr);
#if LWIP_IPV6
  } else if (addr.ss_family == AF_INET6) {
    auto *a = reinterpret_cast<struct sockaddr_in6 *>(&addr);
    p = reinterpret_cast<const uint8_t *>(&a->sin6_addr);
    n = sizeof(a->sin6_addr);
#endif
  }
  for (size_t i = 0; i < n; i++)
    h = (h ^ p[i]) * 16777619u;
  return h;
}

TeslaWbcWeb::Auth TeslaWbcWeb::authorize_(AsyncWebServerRequest *request, uint32_t *retry_s) {
  *retry_s = 0;
  // Until the setup wizard has set a password, the device is open.
  if (!this->cfg_.setup_done || this->cfg_.password_hash[0] == '\0')
    return Auth::OK;
  auto header = request->get_header("Authorization");
  if (!header.has_value())
    return Auth::MISSING;  // the browser asks for the password: not an attempt
  uint32_t client = client_key_(request);
  uint32_t now = millis();
  uint32_t wait = this->guard_.check(client, now);
  if (wait > 0) {
    *retry_s = (wait + 999) / 1000;
    return Auth::LOCKED;
  }
  bool ok = false;
  const std::string &h = header.value();
  if (h.size() >= 7 && strncasecmp(h.c_str(), "Basic ", 6) == 0) {
    uint8_t decoded[160];
    size_t n = base64_decode(h.substr(6), decoded, sizeof(decoded) - 1);
    decoded[n] = '\0';
    const char *colon = strchr(reinterpret_cast<const char *>(decoded), ':');
    if (colon != nullptr) {
      char hex[65];
      this->hash_password_(this->cfg_.password_salt, std::string(colon + 1), hex);
      // Constant-time comparison
      uint8_t diff = 0;
      for (size_t i = 0; i < 64; i++)
        diff |= hex[i] ^ this->cfg_.password_hash[i];
      ok = diff == 0;
    }
  }
  if (ok) {
    this->guard_.success(client);
    return Auth::OK;
  }
  uint32_t w = this->guard_.fail(client, now);
  *retry_s = (w + 999) / 1000;
  ESP_LOGW(TAG, "Wrong device password (%u in a row from this client), next try in %u s",
           (unsigned) this->guard_.fails(client), (unsigned) *retry_s);
  return Auth::WRONG;
}

void TeslaWbcWeb::deny_(AsyncWebServerRequest *request, Auth auth, uint32_t retry_s) {
  char retry[12];
  snprintf(retry, sizeof(retry), "%u", (unsigned) retry_s);
  if (auth == Auth::LOCKED) {
    std::string json = "{\"error\":\"too many wrong passwords, try again in " + std::string(retry) +
                       " s\",\"retry_after_s\":" + retry + "}";
    auto *res = request->beginResponse(429, "application/json", json);
    res->addHeader("Retry-After", retry);
    request->send(res);
    return;
  }
  std::string json = auth == Auth::WRONG ? "{\"error\":\"wrong password\",\"retry_after_s\":" + std::string(retry) + "}"
                                         : std::string("{\"error\":\"password required\"}");
  auto *res = request->beginResponse(401, "application/json", json);
  res->addHeader("WWW-Authenticate", "Basic realm=\"TeslaWBC\", charset=\"UTF-8\"");
  if (auth == Auth::WRONG)
    res->addHeader("Retry-After", retry);
  request->send(res);
}

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

bool TeslaWbcWeb::canHandle(AsyncWebServerRequest *request) const {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(buf);
  return url == "/" || url == "/index.html" || strncmp(url.c_str(), "/api/", 5) == 0;
}

void TeslaWbcWeb::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                             size_t total) {
  if (index == 0)
    this->body_.clear();
  if (this->body_.size() + len <= MAX_BODY)
    this->body_.append(reinterpret_cast<const char *>(data), len);
}

void TeslaWbcWeb::send_json_(AsyncWebServerRequest *request, int code, const std::string &json) {
  auto *res = request->beginResponse(code, "application/json", json);
  res->addHeader("Cache-Control", "no-store");
  request->send(res);
}

void TeslaWbcWeb::send_error_(AsyncWebServerRequest *request, int code, const char *message) {
  std::string json = json::build_json([message](JsonObject root) { root["error"] = message; });
  this->send_json_(request, code, json);
}

void TeslaWbcWeb::handleRequest(AsyncWebServerRequest *request) {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  std::string url = request->url_to(buf).c_str();
  bool post = request->method() == HTTP_POST;
  std::string body;
  if (post) {
    body.swap(this->body_);
  }

  // Firmware uploads were authorized when the upload started.
  if (url == "/api/firmware" && post) {
    this->handle_firmware_(request);
    return;
  }

  uint32_t retry_s;
  Auth auth = this->authorize_(request, &retry_s);
  if (auth != Auth::OK) {
    this->deny_(request, auth, retry_s);
    return;
  }

  if (url == "/" || url == "/index.html") {
    auto *res = request->beginResponse(200, "text/html", TESLAWBC_INDEX_HTML, TESLAWBC_INDEX_HTML_SIZE);
    res->addHeader("Content-Encoding", "gzip");
    res->addHeader("Cache-Control", "no-cache");
    request->send(res);
    return;
  }

  this->body_ = std::move(body);  // handlers below read it from body_
  if (url == "/api/status" && !post) {
    this->handle_status_(request);
  } else if (url == "/api/settings" && !post) {
    this->handle_settings_get_(request);
  } else if (url == "/api/settings" && post) {
    this->handle_settings_post_(request);
  } else if (url == "/api/control" && post) {
    this->handle_control_(request);
  } else if (url == "/api/heartbeat" && post) {
    this->defer([this]() { this->heartbeat_->press(); });
    this->send_json_(request, 200, "{\"ok\":true}");
  } else if (url == "/api/incidents" && !post) {
    this->handle_incidents_(request);
  } else if (!post && url.rfind("/api/incidents/", 0) == 0 && url.size() > 19 &&
             url.compare(url.size() - 4, 4, ".csv") == 0) {
    this->handle_incident_csv_(request, strtoul(url.c_str() + 15, nullptr, 10));
  } else if (url == "/api/recovery" && post) {
    this->handle_recovery_(request);
  } else if (url == "/api/password" && post) {
    this->handle_password_(request);
  } else if (url == "/api/setup/complete" && post) {
    this->handle_setup_complete_(request);
  } else if (url == "/api/restart" && post) {
    this->send_json_(request, 200, "{\"ok\":true}");
    this->defer([this]() { this->set_timeout(500, []() { App.safe_reboot(); }); });
  } else if (url == "/api/factory-reset" && post) {
    this->send_json_(request, 200, "{\"ok\":true}");
    this->defer([this]() { this->factory_reset(); });
  } else {
    this->send_error_(request, 404, "unknown endpoint");
  }
  this->body_.clear();
}

// ---------------------------------------------------------------------------
// Endpoints
// ---------------------------------------------------------------------------

void TeslaWbcWeb::handle_status_(AsyncWebServerRequest *request) {
  std::string json = json::build_json([this](JsonObject root) {
    root["name"] = App.get_name().c_str();
    root["firmware"] = ESPHOME_PROJECT_VERSION;
    root["uptime_s"] = millis() / 1000;
    root["setup_done"] = this->cfg_.setup_done;
    root["mode"] = this->mode_->has_state() ? this->mode_->current_option().c_str() : "";
    root["target"] = this->target_->state;
#ifdef TESLAWBC_USE_MQTT
    root["mqtt_connected"] = mqtt::global_mqtt_client != nullptr && mqtt::global_mqtt_client->is_connected();
#endif
    JsonObject values = root["values"].to<JsonObject>();
    for (auto *s : this->sensors_) {
      std::string id = object_id(s);
      if (std::isnan(s->state)) {
        values[id] = nullptr;
      } else {
        values[id] = s->state;
      }
    }
    for (auto *s : this->binary_sensors_) {
      std::string id = object_id(s);
      if (s->has_state()) {
        values[id] = s->state;
      } else {
        values[id] = nullptr;
      }
    }
    for (auto *s : this->text_sensors_)
      values[object_id(s)] = s->state;
    LockGuard guard(this->lock_);
    uint32_t now = millis();
    JsonObject rec = root["recovery"].to<JsonObject>();
    rec["phase"] = teslawbc::recovery_phase_name(this->recovery_.phase());
    rec["active"] = this->recovery_.active();
    rec["power_cut"] = this->recovery_.power_cut();
    rec["remaining_s"] = this->recovery_.remaining_ms(this->recovery_settings_, now) / 1000;
    rec["auto"] = this->recovery_settings_.auto_enabled;
    rec["next_auto_in_s"] = this->recovery_.blocked_ms(this->recovery_settings_, now) / 1000;
    rec["attempts"] = this->recovery_.attempts();
    const auto &incs = this->recorder_.incidents();
    root["incidents"] = incs.size();
    root["last_incident"] = incs.empty() ? 0 : incs.back().id;
  });
  this->send_json_(request, 200, json);
}

void TeslaWbcWeb::handle_settings_get_(AsyncWebServerRequest *request) {
  std::string json = json::build_json([this](JsonObject root) {
    JsonObject nums = root["numbers"].to<JsonObject>();
    for (auto *n : this->settings_) {
      JsonObject o = nums[object_id(n)].to<JsonObject>();
      o["name"] = n->get_name().c_str();
      o["value"] = n->state;
      o["min"] = n->traits.get_min_value();
      o["max"] = n->traits.get_max_value();
      o["step"] = n->traits.get_step();
      o["unit"] = n->get_unit_of_measurement_ref().c_str();
    }
    JsonObject sws = root["switches"].to<JsonObject>();
    for (auto *sw : this->switches_) {
      JsonObject o = sws[object_id(sw)].to<JsonObject>();
      o["name"] = sw->get_name().c_str();
      o["value"] = sw->state;
    }
    root["mqtt_host"] = this->cfg_.mqtt_host;
    root["mqtt_port"] = this->cfg_.mqtt_port;
    root["mqtt_user"] = this->cfg_.mqtt_user;
    root["mqtt_password_set"] = this->cfg_.mqtt_password[0] != '\0';
    root["wc_host"] = this->cfg_.wc_host;
    root["password_set"] = this->cfg_.password_hash[0] != '\0';
    root["setup_done"] = this->cfg_.setup_done;
  });
  this->send_json_(request, 200, json);
}

void TeslaWbcWeb::handle_settings_post_(AsyncWebServerRequest *request) {
  std::string error;
  bool connection_changed = false;
  std::vector<std::pair<number::Number *, float>> number_changes;
  std::vector<std::pair<switch_::Switch *, bool>> switch_changes;
  StoredConfig next = this->cfg_;

  bool ok = json::parse_json(this->body_, [&](JsonObject root) -> bool {
    for (auto *n : this->settings_) {
      std::string id = object_id(n);
      if (!root[id].is<float>())
        continue;
      float v = root[id].as<float>();
      if (std::isnan(v) || v < n->traits.get_min_value() || v > n->traits.get_max_value()) {
        error = id + " out of range";
        return false;
      }
      number_changes.emplace_back(n, v);
    }
    for (auto *sw : this->switches_) {
      std::string id = object_id(sw);
      if (root[id].is<bool>())
        switch_changes.emplace_back(sw, root[id].as<bool>());
    }
    auto take_str = [&](const char *key, char *dst, size_t size) {
      if (!root[key].is<const char *>())
        return;
      std::string v = root[key].as<std::string>();
      if (v.size() >= size) {
        error = std::string(key) + " too long";
        return;
      }
      copy_str(dst, size, v);
      connection_changed = true;
    };
    take_str("mqtt_host", next.mqtt_host, sizeof(next.mqtt_host));
    take_str("mqtt_user", next.mqtt_user, sizeof(next.mqtt_user));
    take_str("mqtt_password", next.mqtt_password, sizeof(next.mqtt_password));
    take_str("wc_host", next.wc_host, sizeof(next.wc_host));
    if (root["mqtt_port"].is<int>()) {
      int p = root["mqtt_port"].as<int>();
      if (p < 1 || p > 65535) {
        error = "mqtt_port out of range";
        return false;
      }
      next.mqtt_port = p;
      connection_changed = true;
    }
    return error.empty();
  });
  if (!ok || !error.empty()) {
    this->send_error_(request, 400, error.empty() ? "invalid JSON" : error.c_str());
    return;
  }

  bool mqtt_changed = strcmp(next.mqtt_host, this->cfg_.mqtt_host) != 0 || next.mqtt_port != this->cfg_.mqtt_port ||
                      strcmp(next.mqtt_user, this->cfg_.mqtt_user) != 0 ||
                      strcmp(next.mqtt_password, this->cfg_.mqtt_password) != 0;
  this->defer([this, next, number_changes, switch_changes, connection_changed, mqtt_changed]() {
    for (auto &c : number_changes) {
      auto call = c.first->make_call();
      call.set_value(c.second);
      call.perform();
    }
    for (auto &c : switch_changes) {
      if (c.second) {
        c.first->turn_on();
      } else {
        c.first->turn_off();
      }
    }
    if (connection_changed) {
      this->cfg_ = next;
      this->save_();
    }
    if (mqtt_changed)
      this->apply_mqtt_();
  });
  this->send_json_(request, 200, "{\"ok\":true}");
}

void TeslaWbcWeb::handle_control_(AsyncWebServerRequest *request) {
  std::string mode;
  float target = NAN;
  std::string error;
  bool ok = json::parse_json(this->body_, [&](JsonObject root) -> bool {
    if (root["mode"].is<const char *>()) {
      mode = root["mode"].as<std::string>();
      bool known = false;
      for (const auto &opt : this->mode_->traits.get_options())
        known |= (mode == opt);
      if (!known) {
        error = "unknown mode";
        return false;
      }
    }
    if (root["target"].is<float>()) {
      target = root["target"].as<float>();
      if (std::isnan(target) || target < this->target_->traits.get_min_value() ||
          target > this->target_->traits.get_max_value()) {
        error = "target out of range";
        return false;
      }
    }
    return true;
  });
  if (!this->body_.empty() && (!ok || !error.empty())) {
    this->send_error_(request, 400, error.empty() ? "invalid JSON" : error.c_str());
    return;
  }
  this->defer([this, mode, target]() {
    if (!std::isnan(target)) {
      auto call = this->target_->make_call();
      call.set_value(target);
      call.perform();
    }
    if (!mode.empty()) {
      auto call = this->mode_->make_call();
      call.set_option(mode);
      call.perform();
    }
    this->heartbeat_->press();
  });
  this->send_json_(request, 200, "{\"ok\":true}");
}

void TeslaWbcWeb::handle_password_(AsyncWebServerRequest *request) {
  std::string password;
  json::parse_json(this->body_, [&](JsonObject root) -> bool {
    if (root["password"].is<const char *>())
      password = root["password"].as<std::string>();
    return true;
  });
  if (password.size() < MIN_PASSWORD_LEN || password.size() > 64) {
    this->send_error_(request, 400, "password must be 8 to 64 characters");
    return;
  }
  this->defer([this, password]() {
    this->set_password_(password);
    this->save_();
    ESP_LOGI(TAG, "Device password changed");
  });
  this->send_json_(request, 200, "{\"ok\":true}");
}

void TeslaWbcWeb::handle_setup_complete_(AsyncWebServerRequest *request) {
  if (this->cfg_.password_hash[0] == '\0') {
    this->send_error_(request, 409, "set the device password first");
    return;
  }
  this->defer([this]() {
    this->cfg_.setup_done = true;
    this->save_();
    ESP_LOGI(TAG, "Setup complete; the device password is now required");
  });
  this->send_json_(request, 200, "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// Recovery and incidents
// ---------------------------------------------------------------------------

bool TeslaWbcWeb::update_recovery(const teslawbc::RecoverySettings &rs, uint32_t now_ms, bool control_lost,
                                  bool *started) {
  LockGuard guard(this->lock_);
  this->recovery_settings_ = rs;
  bool was = this->recovery_.active();
  *started = this->recovery_.update(rs, now_ms, control_lost);
  if (*started)
    ESP_LOGW(TAG, "Control lost: recovery started (power off, then Bypass)");
  if (was && !this->recovery_.active())
    ESP_LOGI(TAG, "Recovery finished: back to normal operation");
  return this->recovery_.power_cut();
}

void TeslaWbcWeb::start_recovery() {
  LockGuard guard(this->lock_);
  this->recovery_.start(millis());
  ESP_LOGW(TAG, "Recovery started by hand");
}

void TeslaWbcWeb::cancel_recovery() {
  LockGuard guard(this->lock_);
  if (this->recovery_.active())
    ESP_LOGI(TAG, "Recovery cancelled");
  this->recovery_.cancel();
}

bool TeslaWbcWeb::record(const teslawbc::Sample &sample, bool alarm_rising, uint32_t epoch_now,
                         const teslawbc::AnalysisSettings &as) {
  const teslawbc::Incident *done;
  {
    LockGuard guard(this->lock_);
    done = this->recorder_.add(sample, alarm_rising, epoch_now, as);
    if (alarm_rising)
      ESP_LOGW(TAG, "Recording incident %u", (unsigned) this->recorder_.incidents().back().id);
    if (done == nullptr)
      return false;
  }
  ESP_LOGW(TAG, "%s", teslawbc::incident_summary(*done).c_str());
  this->publish_incident_(*done);
  return true;
}

std::string TeslaWbcWeb::last_incident_summary() {
  LockGuard guard(this->lock_);
  const auto &incs = this->recorder_.incidents();
  return incs.empty() ? std::string() : teslawbc::incident_summary(incs.back());
}

static void incident_json(JsonObject o, const teslawbc::Incident &inc) {
  o["id"] = inc.id;
  if (inc.alarm_epoch != 0) {
    o["time"] = inc.alarm_epoch;
  } else {
    o["time"] = nullptr;
  }
  o["uptime_s"] = inc.alarm_ms / 1000;
  o["complete"] = inc.complete;
  o["summary"] = teslawbc::incident_summary(inc);
  o["samples"] = inc.samples.size();
  o["seconds_before"] = inc.alarm_index;
  char url[40];
  snprintf(url, sizeof(url), "/api/incidents/%u.csv", (unsigned) inc.id);
  o["csv"] = url;
  JsonArray causes = o["causes"].to<JsonArray>();
  for (const auto &f : inc.findings) {
    JsonObject c = causes.add<JsonObject>();
    c["cause"] = teslawbc::cause_key(f.cause);
    c["text"] = teslawbc::cause_text(f.cause);
    c["first_s"] = f.first_s;
    c["count"] = f.count;
    c["worst"] = f.worst;
  }
}

void TeslaWbcWeb::publish_incident_(const teslawbc::Incident &inc) {
#ifdef TESLAWBC_USE_MQTT
  auto *m = mqtt::global_mqtt_client;
  if (m == nullptr || !m->is_connected())
    return;
  std::string json;
  {
    LockGuard guard(this->lock_);
    json = json::build_json([&inc](JsonObject root) { incident_json(root, inc); });
  }
  m->publish(m->get_topic_prefix() + "/incident", json, 1, true);
#endif
}

void TeslaWbcWeb::handle_incidents_(AsyncWebServerRequest *request) {
  std::string json;
  {
    LockGuard guard(this->lock_);
    json = json::build_json([this](JsonObject root) {
      JsonArray list = root["incidents"].to<JsonArray>();
      for (const auto &inc : this->recorder_.incidents())
        incident_json(list.add<JsonObject>(), inc);
    });
  }
  this->send_json_(request, 200, json);
}

void TeslaWbcWeb::handle_incident_csv_(AsyncWebServerRequest *request, uint32_t id) {
  // Copy under the lock, stream without it, so a slow client cannot stall control.
  std::vector<teslawbc::Sample> samples;
  uint32_t alarm_ms = 0;
  {
    LockGuard guard(this->lock_);
    const teslawbc::Incident *inc = this->recorder_.find(id);
    if (inc != nullptr) {
      samples = inc->samples;
      alarm_ms = inc->alarm_ms;
    }
  }
  if (samples.empty()) {
    this->send_error_(request, 404, "no such incident");
    return;
  }
  httpd_req_t *r = *request;
  char disp[64];
  snprintf(disp, sizeof(disp), "attachment; filename=\"teslawbc-incident-%u.csv\"", (unsigned) id);
  httpd_resp_set_type(r, "text/csv");
  httpd_resp_set_hdr(r, "Content-Disposition", disp);
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  std::string chunk = teslawbc::csv_header();
  char row[160];
  for (const auto &smp : samples) {
    int n = teslawbc::csv_row(row, sizeof(row), smp, alarm_ms);
    if (n > 0)
      chunk.append(row, std::min<size_t>(n, sizeof(row) - 1));
    if (chunk.size() > 3000) {
      if (httpd_resp_send_chunk(r, chunk.data(), chunk.size()) != ESP_OK)
        return;
      chunk.clear();
    }
  }
  if (!chunk.empty())
    httpd_resp_send_chunk(r, chunk.data(), chunk.size());
  httpd_resp_send_chunk(r, nullptr, 0);
}

void TeslaWbcWeb::handle_recovery_(AsyncWebServerRequest *request) {
  std::string action;
  json::parse_json(this->body_, [&](JsonObject root) -> bool {
    if (root["action"].is<const char *>())
      action = root["action"].as<std::string>();
    return true;
  });
  if (action == "start") {
    this->defer([this]() { this->start_recovery(); });
  } else if (action == "cancel") {
    this->defer([this]() { this->cancel_recovery(); });
  } else {
    this->send_error_(request, 400, "action must be \"start\" or \"cancel\"");
    return;
  }
  this->send_json_(request, 200, "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// Firmware update: POST /api/firmware, multipart/form-data with the .bin file
// (firmware.ota.bin from the ESPHome build). Runs in the web server task.
// ---------------------------------------------------------------------------

void TeslaWbcWeb::handleUpload(AsyncWebServerRequest *request, const std::string &filename, size_t index,
                               uint8_t *data, size_t len, bool final) {
  if (index == 0 && len == 0 && !final) {
    // Start of a new upload: check the password once, before any flash write.
    if (this->ota_backend_) {
      ESP_LOGW(TAG, "New firmware upload while the previous one was open: aborting the previous one");
      this->ota_backend_->abort();
      this->ota_backend_.reset();
    }
    this->upload_started_ = true;
    this->upload_ok_ = false;
    this->upload_error_ = nullptr;
    this->upload_bytes_ = 0;
    this->upload_auth_ = this->authorize_(request, &this->upload_retry_s_);
    if (this->upload_auth_ != Auth::OK)
      return;
    ESP_LOGI(TAG, "Firmware upload started: %s", filename.c_str());
    this->ota_backend_ = ota::make_ota_backend();
    if (!this->ota_backend_ || this->ota_backend_->begin(0) != ota::OTA_RESPONSE_OK) {
      this->ota_backend_.reset();
      this->upload_error_ = "could not start the update";
    }
    return;
  }
  if (!this->ota_backend_)
    return;
  if (len > 0) {
    if (this->ota_backend_->write(data, len) != ota::OTA_RESPONSE_OK) {
      ESP_LOGE(TAG, "Firmware write failed after %u bytes", (unsigned) this->upload_bytes_);
      this->ota_backend_->abort();
      this->ota_backend_.reset();
      this->upload_error_ = "writing the firmware failed: is this a TeslaWBC .bin file?";
      return;
    }
    this->upload_bytes_ += len;
  }
  if (final) {
    if (this->ota_backend_->end() == ota::OTA_RESPONSE_OK) {
      this->upload_ok_ = true;
      ESP_LOGI(TAG, "Firmware update written (%u bytes)", (unsigned) this->upload_bytes_);
    } else {
      this->upload_error_ = "the firmware file is incomplete or invalid";
    }
    this->ota_backend_.reset();
  }
}

void TeslaWbcWeb::handle_firmware_(AsyncWebServerRequest *request) {
  if (!this->upload_started_) {
    // No file part arrived (or not a multipart request).
    uint32_t retry_s;
    Auth auth = this->authorize_(request, &retry_s);
    if (auth != Auth::OK) {
      this->deny_(request, auth, retry_s);
    } else {
      this->send_error_(request, 400, "no firmware file received");
    }
    return;
  }
  this->upload_started_ = false;
  if (this->ota_backend_) {
    // The request ended without a final part.
    this->ota_backend_->abort();
    this->ota_backend_.reset();
    if (this->upload_error_ == nullptr)
      this->upload_error_ = "the upload was cut off";
  }
  if (this->upload_auth_ != Auth::OK) {
    Auth auth = this->upload_auth_;
    this->upload_auth_ = Auth::MISSING;
    this->deny_(request, auth, this->upload_retry_s_);
    return;
  }
  this->upload_auth_ = Auth::MISSING;  // the next upload checks again
  if (!this->upload_ok_) {
    this->send_error_(request, 400, this->upload_error_ != nullptr ? this->upload_error_ : "no firmware file received");
    return;
  }
  this->upload_ok_ = false;
  this->send_json_(request, 200, "{\"ok\":true,\"restarting\":true}");
  this->defer([this]() { this->set_timeout(500, []() { App.safe_reboot(); }); });
}

}  // namespace esphome::teslawbc_web
