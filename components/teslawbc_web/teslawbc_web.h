#pragma once

#include <string>
#include <vector>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/ota/ota_backend_factory.h"
#include "esphome/components/button/button.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#include "blackbox.h"
#include "login_guard.h"
#include "recovery.h"

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
  void add_switch(switch_::Switch *s) { this->switches_.push_back(s); }
  void add_sensor(sensor::Sensor *s) { this->sensors_.push_back(s); }
  void add_binary_sensor(binary_sensor::BinarySensor *s) { this->binary_sensors_.push_back(s); }
  void add_text_sensor(text_sensor::TextSensor *s) { this->text_sensors_.push_back(s); }
  void set_default_mqtt_port(uint16_t port) { this->default_mqtt_port_ = port; }

  // Used by the YAML
  bool setup_done() const { return this->cfg_.setup_done; }
  std::string wc_host() const { return std::string(this->cfg_.wc_host); }
  bool mqtt_configured() const { return this->cfg_.mqtt_host[0] != '\0'; }
  void factory_reset();

  // Recovery: call every control cycle. Returns true while the Wall
  // Connector's power must be cut. Also returns, via started, whether an
  // automatic recovery began in this call.
  bool update_recovery(const teslawbc::RecoverySettings &rs, uint32_t now_ms, bool control_lost, bool *started);
  bool recovery_forces_bypass() const { return this->recovery_.forces_bypass(); }
  bool recovery_active() const { return this->recovery_.active(); }
  void start_recovery();
  void cancel_recovery();

  // Incident recorder: offer a sample every control cycle. Returns true when
  // an incident has just been completed (see last_incident_summary()).
  bool record(const teslawbc::Sample &sample, bool alarm_rising, uint32_t epoch_now,
              const teslawbc::AnalysisSettings &as);
  std::string last_incident_summary();

  // AsyncWebHandler
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;
  void handleUpload(AsyncWebServerRequest *request, const std::string &filename, size_t index, uint8_t *data,
                    size_t len, bool final) override;
  bool isRequestHandlerTrivial() const override { return false; }

 protected:
  enum class Auth { OK, MISSING, WRONG, LOCKED };
  // Checks the password and applies the login lockout.
  Auth authorize_(AsyncWebServerRequest *request, uint32_t *retry_s);
  void deny_(AsyncWebServerRequest *request, Auth auth, uint32_t retry_s);
  static uint32_t client_key_(AsyncWebServerRequest *request);
  void send_json_(AsyncWebServerRequest *request, int code, const std::string &json);
  void send_error_(AsyncWebServerRequest *request, int code, const char *message);

  void handle_status_(AsyncWebServerRequest *request);
  void handle_settings_get_(AsyncWebServerRequest *request);
  void handle_settings_post_(AsyncWebServerRequest *request);
  void handle_control_(AsyncWebServerRequest *request);
  void handle_password_(AsyncWebServerRequest *request);
  void handle_setup_complete_(AsyncWebServerRequest *request);
  void handle_firmware_(AsyncWebServerRequest *request);
  void handle_incidents_(AsyncWebServerRequest *request);
  void handle_incident_csv_(AsyncWebServerRequest *request, uint32_t id);
  void handle_recovery_(AsyncWebServerRequest *request);
  void publish_incident_(const teslawbc::Incident &inc);

  void hash_password_(const char *salt, const std::string &password, char *out_hex) const;
  void set_password_(const std::string &password);
  void save_();
  void apply_mqtt_();

  select::Select *mode_{nullptr};
  number::Number *target_{nullptr};
  button::Button *heartbeat_{nullptr};
  std::vector<number::Number *> settings_;
  std::vector<switch_::Switch *> switches_;
  std::vector<sensor::Sensor *> sensors_;
  std::vector<binary_sensor::BinarySensor *> binary_sensors_;
  std::vector<text_sensor::TextSensor *> text_sensors_;
  uint16_t default_mqtt_port_{1883};

  StoredConfig cfg_{};
  ESPPreferenceObject pref_;
  std::string body_;  // request body being received (one request at a time)

  teslawbc::LoginGuard guard_;

  // Main loop writes, web server task reads: guarded by lock_.
  Mutex lock_;
  teslawbc::Recovery recovery_;
  teslawbc::RecoverySettings recovery_settings_;
  teslawbc::Recorder recorder_;

  // Firmware upload in progress (one at a time)
  ota::OTABackendPtr ota_backend_{nullptr};
  bool upload_started_{false};
  Auth upload_auth_{Auth::MISSING};
  uint32_t upload_retry_s_{0};
  size_t upload_bytes_{0};
  bool upload_ok_{false};
  const char *upload_error_{nullptr};
};

}  // namespace esphome::teslawbc_web
