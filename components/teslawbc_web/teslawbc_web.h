#pragma once

#include <string>
#include <vector>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

namespace esphome::teslawbc_web {

// Settings that must change without a firmware build. Stored in flash.
struct StoredConfig {
  uint32_t version;
  bool setup_done;
  char password_salt[17];   // hex
  char password_hash[65];   // hex SHA-256 of salt + password
  char mqtt_host[64];
  uint16_t mqtt_port;
  char mqtt_user[33];
  char mqtt_password[65];
  char wc_host[64];
} __attribute__((packed));

class TeslaWbcWeb : public Component, public AsyncWebHandler {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::WIFI - 2.0f; }

  void set_mode(select::Select *s) { this->mode_ = s; }
  void set_target(number::Number *n) { this->target_ = n; }
  void set_heartbeat(button::Button *b) { this->heartbeat_ = b; }
  void add_setting(number::Number *n) { this->settings_.push_back(n); }
  void add_sensor(sensor::Sensor *s) { this->sensors_.push_back(s); }
  void add_binary_sensor(binary_sensor::BinarySensor *s) { this->binary_sensors_.push_back(s); }
  void add_text_sensor(text_sensor::TextSensor *s) { this->text_sensors_.push_back(s); }
  void set_default_mqtt_port(uint16_t port) { this->default_mqtt_port_ = port; }

  // Used by the YAML
  bool setup_done() const { return this->cfg_.setup_done; }
  std::string wc_host() const { return std::string(this->cfg_.wc_host); }
  bool mqtt_configured() const { return this->cfg_.mqtt_host[0] != '\0'; }
  void factory_reset();

  // AsyncWebHandler
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;
  bool isRequestHandlerTrivial() const override { return false; }

 protected:
  bool authorized_(AsyncWebServerRequest *request) const;
  void send_json_(AsyncWebServerRequest *request, int code, const std::string &json);
  void send_error_(AsyncWebServerRequest *request, int code, const char *message);

  void handle_status_(AsyncWebServerRequest *request);
  void handle_settings_get_(AsyncWebServerRequest *request);
  void handle_settings_post_(AsyncWebServerRequest *request);
  void handle_control_(AsyncWebServerRequest *request);
  void handle_password_(AsyncWebServerRequest *request);
  void handle_setup_complete_(AsyncWebServerRequest *request);

  void hash_password_(const char *salt, const std::string &password, char *out_hex) const;
  void set_password_(const std::string &password);
  void save_();
  void apply_mqtt_();

  select::Select *mode_{nullptr};
  number::Number *target_{nullptr};
  button::Button *heartbeat_{nullptr};
  std::vector<number::Number *> settings_;
  std::vector<sensor::Sensor *> sensors_;
  std::vector<binary_sensor::BinarySensor *> binary_sensors_;
  std::vector<text_sensor::TextSensor *> text_sensors_;
  uint16_t default_mqtt_port_{1883};

  StoredConfig cfg_{};
  ESPPreferenceObject pref_;
  std::string body_;  // request body being received (one request at a time)
};

}  // namespace esphome::teslawbc_web
