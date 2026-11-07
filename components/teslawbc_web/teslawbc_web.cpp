#include "teslawbc_web.h"

#include <cmath>
#include <cstring>
#include <strings.h>

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

bool TeslaWbcWeb::authorized_(AsyncWebServerRequest *request) const {
  // Until the setup wizard has set a password, the device is open.
  if (!this->cfg_.setup_done || this->cfg_.password_hash[0] == '\0')
    return true;
  auto header = request->get_header("Authorization");
  if (!header.has_value())
    return false;
  const std::string &h = header.value();
  if (h.size() < 7 || strncasecmp(h.c_str(), "Basic ", 6) != 0)
    return false;
  uint8_t decoded[160];
  size_t n = base64_decode(h.substr(6), decoded, sizeof(decoded) - 1);
  decoded[n] = '\0';
  const char *colon = strchr(reinterpret_cast<const char *>(decoded), ':');
  if (colon == nullptr)
    return false;
  char hex[65];
  this->hash_password_(this->cfg_.password_salt, std::string(colon + 1), hex);
  // Constant-time comparison
  uint8_t diff = 0;
  for (size_t i = 0; i < 64; i++)
    diff |= hex[i] ^ this->cfg_.password_hash[i];
  return diff == 0;
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

  if (!this->authorized_(request)) {
    auto *res = request->beginResponse(401, "application/json", std::string("{\"error\":\"password required\"}"));
    res->addHeader("WWW-Authenticate", "Basic realm=\"TeslaWBC\", charset=\"UTF-8\"");
    request->send(res);
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
  this->defer([this, next, number_changes, connection_changed, mqtt_changed]() {
    for (auto &c : number_changes) {
      auto call = c.first->make_call();
      call.set_value(c.second);
      call.perform();
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

}  // namespace esphome::teslawbc_web
