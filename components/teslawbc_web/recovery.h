// Automatic recovery when the Wall Connector stops trusting the meter.
//
// Pure C++ with no ESPHome dependency, so it can be unit-tested on a PC
// (see tests/test_recovery.cpp).
//
// What restored trust for others: switch the Wall Connector off and on,
// then send it an honest signal (Bypass) for about two hours. So a
// recovery is:
//
//   POWER_OFF  the relay opens the normally closed contactor for
//              power_off_ms (default 1 min); TeslaWBC is in Bypass
//   BYPASS     power back on, TeslaWBC stays in Bypass for bypass_ms
//              (default 2 h)
//   IDLE       back to the mode Aperta set
//
// Automatic recovery starts when Control Lost is raised, at most once per
// min_interval_ms (default 24 h) counted from the last start. If trust is
// lost again within that time, TeslaWBC leaves the alarm on and waits for
// a person. A manual start (button, REST, MQTT) ignores that limit.
#pragma once

#include <cstdint>

namespace teslawbc {

enum class RecoveryPhase : uint8_t { IDLE = 0, POWER_OFF = 1, BYPASS = 2 };

inline const char *recovery_phase_name(RecoveryPhase p) {
  switch (p) {
    case RecoveryPhase::IDLE: return "Idle";
    case RecoveryPhase::POWER_OFF: return "Power off";
    case RecoveryPhase::BYPASS: return "Bypass";
  }
  return "Unknown";
}

struct RecoverySettings {
  bool auto_enabled = false;
  uint32_t power_off_ms = 60u * 1000u;
  uint32_t bypass_ms = 2u * 60u * 60u * 1000u;
  uint32_t min_interval_ms = 24u * 60u * 60u * 1000u;
};

class Recovery {
 public:
  // Call regularly (every 250 ms). Returns true when a recovery started now.
  bool update(const RecoverySettings &rs, uint32_t now_ms, bool control_lost) {
    // Forget the last start once the interval has passed, so millis()
    // wrapping after 49 days cannot block a later attempt.
    if (have_last_ && elapsed_(now_ms, last_start_ms_) >= rs.min_interval_ms)
      have_last_ = false;

    switch (phase_) {
      case RecoveryPhase::POWER_OFF:
        if (elapsed_(now_ms, phase_ms_) >= rs.power_off_ms) {
          phase_ = RecoveryPhase::BYPASS;
          phase_ms_ = now_ms;
        }
        return false;
      case RecoveryPhase::BYPASS:
        if (elapsed_(now_ms, phase_ms_) >= rs.bypass_ms)
          phase_ = RecoveryPhase::IDLE;
        return false;
      case RecoveryPhase::IDLE:
        break;
    }
    if (control_lost && rs.auto_enabled && !have_last_) {
      this->start(now_ms);
      return true;
    }
    return false;
  }

  void start(uint32_t now_ms) {
    phase_ = RecoveryPhase::POWER_OFF;
    phase_ms_ = now_ms;
    last_start_ms_ = now_ms;
    have_last_ = true;
    attempts_++;
  }

  void cancel() { phase_ = RecoveryPhase::IDLE; }

  RecoveryPhase phase() const { return phase_; }
  bool active() const { return phase_ != RecoveryPhase::IDLE; }
  // The relay energizes the contactor coil: the Wall Connector is off.
  bool power_cut() const { return phase_ == RecoveryPhase::POWER_OFF; }
  // TeslaWBC must report honestly while this is true.
  bool forces_bypass() const { return phase_ != RecoveryPhase::IDLE; }
  uint32_t attempts() const { return attempts_; }

  // Time left in the current phase, 0 when idle.
  uint32_t remaining_ms(const RecoverySettings &rs, uint32_t now_ms) const {
    uint32_t len = phase_ == RecoveryPhase::POWER_OFF ? rs.power_off_ms
                   : phase_ == RecoveryPhase::BYPASS  ? rs.bypass_ms
                                                      : 0;
    uint32_t e = elapsed_(now_ms, phase_ms_);
    return e >= len ? 0 : len - e;
  }

  // Time until an automatic recovery may start again, 0 if it may now.
  uint32_t blocked_ms(const RecoverySettings &rs, uint32_t now_ms) const {
    if (!have_last_)
      return 0;
    uint32_t e = elapsed_(now_ms, last_start_ms_);
    return e >= rs.min_interval_ms ? 0 : rs.min_interval_ms - e;
  }

 private:
  static uint32_t elapsed_(uint32_t now, uint32_t then) { return (uint32_t) (now - then); }

  RecoveryPhase phase_ = RecoveryPhase::IDLE;
  uint32_t phase_ms_ = 0;
  uint32_t last_start_ms_ = 0;
  bool have_last_ = false;
  uint32_t attempts_ = 0;
};

}  // namespace teslawbc
