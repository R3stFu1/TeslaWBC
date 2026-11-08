// TeslaWBC control logic.
//
// Pure C++ with no ESPHome dependency, so it can be unit-tested on a PC
// (see tests/test_control.cpp). The ESPHome config calls compute() after
// every meter reading and on a short interval, and the Neurio emulation
// serves the values it returns.
//
// How the Wall Connector is steered: it lowers the car's current whenever
// the current reported by its (emulated) Neurio meter exceeds its Max
// Conductor Limit L. We report
//
//     R = max(W1, W2, W3) + (L - T)
//
// on all three CTs, where W is the wallbox's own phase current measured by
// the DRS-100 and T the target. R rises above L when the car draws more
// than T and falls below L when it draws less, so the car settles at T.
// R always follows W 1:1 and is never below W, which keeps the charger's
// plausibility checks happy.
#pragma once

#include <cmath>
#include <cstdint>

namespace teslawbc {

enum class Mode : uint8_t {
  BYPASS = 0,   // report the honest wallbox current: no control at all
  MANUAL = 1,  // hold the target current T
  PAUSE = 2,   // report more than L: the charger opens its contactor
};

enum class State : uint8_t {
  BYPASS = 0,
  CHARGING = 1,           // Manual, holding the target
  PAUSED = 2,             // Pause mode, or Manual with a target below the minimum
  FALLBACK_CHARGING = 3,  // Aperta silent: holding the fallback target
  FALLBACK_PAUSED = 4,    // Aperta silent: fallback target is "pause"
  METER_FAULT = 5,        // no valid meter reading: pausing for safety
};

inline const char *state_name(State s) {
  switch (s) {
    case State::BYPASS: return "Bypass";
    case State::CHARGING: return "Charging";
    case State::PAUSED: return "Paused";
    case State::FALLBACK_CHARGING: return "Fallback charging";
    case State::FALLBACK_PAUSED: return "Fallback paused";
    case State::METER_FAULT: return "Meter fault";
  }
  return "Unknown";
}

struct Settings {
  float conductor_limit = 18.0f;  // L, must match "Max Conductor Limit" in the Tesla app
  float max_output = 16.0f;       // I_max, must match "Max Output Current" in the Tesla app
  float min_current = 6.0f;       // lowest current a car accepts; below this we pause
  float pause_margin = 2.0f;      // how far above L we report to pause
  float fallback_target = 6.0f;   // used when Aperta is silent; < min_current = pause
  uint32_t fallback_timeout_ms = 5u * 60u * 1000u;
  uint32_t meter_timeout_ms = 3000u;
};

struct Inputs {
  uint32_t now_ms = 0;
  // Meter: last successful reading and the phase currents it gave.
  bool meter_seen = false;
  uint32_t last_meter_ms = 0;
  float current[3] = {NAN, NAN, NAN};
  // Aperta: requested mode and target, and when it last sent a command or heartbeat.
  Mode mode = Mode::BYPASS;
  float target = 6.0f;
  uint32_t last_cmd_ms = 0;
};

struct Result {
  float reported = NAN;          // R, served on CT1 to CT3
  float effective_target = NAN;  // T actually applied; NAN when not regulating
  float max_current = NAN;       // max(W1, W2, W3), NAN on meter fault
  State state = State::METER_FAULT;
  bool meter_ok = false;
  bool fallback = false;         // true while Aperta is considered silent
};

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline Result compute(const Settings &cfg, const Inputs &in) {
  Result r;
  const float limit = cfg.conductor_limit;
  // Target ceiling: the charger never gives more than I_max, and T above L
  // would make L - T negative, which could report less than the car draws.
  const float t_max = cfg.max_output < limit ? cfg.max_output : limit;

  // Meter health: a reading must be recent and every phase must be a number.
  bool meter_ok = in.meter_seen && (uint32_t) (in.now_ms - in.last_meter_ms) <= cfg.meter_timeout_ms;
  float w_max = 0.0f;
  for (float w : in.current) {
    if (std::isnan(w) || std::isinf(w)) {
      meter_ok = false;
      break;
    }
    float a = std::fabs(w);
    if (a > w_max) w_max = a;
  }
  r.meter_ok = meter_ok;
  r.fallback = (uint32_t) (in.now_ms - in.last_cmd_ms) > cfg.fallback_timeout_ms;

  if (!meter_ok) {
    // Without a measurement we cannot echo the car's current, so we stop it.
    r.state = State::METER_FAULT;
    r.reported = limit + cfg.pause_margin;
    return r;
  }
  r.max_current = w_max;

  // Bypass is the kill switch: honest current, independent of Aperta.
  if (in.mode == Mode::BYPASS) {
    r.state = State::BYPASS;
    r.reported = w_max;
    return r;
  }

  bool pause;
  float target;
  if (r.fallback) {
    target = cfg.fallback_target;
    pause = !(target >= cfg.min_current);
  } else if (in.mode == Mode::PAUSE) {
    target = 0.0f;
    pause = true;
  } else {
    target = in.target;
    pause = !(target >= cfg.min_current);  // also catches NAN
  }

  if (pause) {
    r.state = r.fallback ? State::FALLBACK_PAUSED : State::PAUSED;
    // Above L by the margin, and never below what the wallbox draws.
    r.reported = (w_max > limit ? w_max : limit) + cfg.pause_margin;
    return r;
  }

  target = clampf(target, cfg.min_current, t_max);
  r.state = r.fallback ? State::FALLBACK_CHARGING : State::CHARGING;
  r.effective_target = target;
  r.reported = w_max + (limit - target);
  return r;
}

// Shapes R before it is served, so the charger always sees a live, smooth
// signal. Field reports (tesla-loadpilot, PVi1, Klangen82) show the Wall
// Connector distrusts a meter whose value is flat for minutes or jumps in
// steps that its own current does not explain, and then ignores it, often
// until it is power-cycled. So:
//
//  * The part of R that TeslaWBC adds on top of the car's current, the
//    offset R - max(W), moves towards its new value at ramp_rate A/s. This
//    turns every target change, a pause and a return to Bypass into a
//    smooth ramp. Changes in the car's own current still pass 1:1.
//  * A small dither in [0, 2 x dither] A keeps the value from ever being
//    flat, also in a pause and on a meter fault. It only ever adds, so R
//    stays at or above what the wallbox draws.
//  * A meter fault is served at once (no ramp): without a measurement
//    there is nothing to echo, and stopping the car is the safe side.
//    When the meter comes back, the offset starts from the fault value and
//    ramps down from there.
struct ShapeSettings {
  float ramp_rate = 0.25f;  // A/s for the offset; 0 or less: no ramp
  float dither = 0.05f;     // A
};

class Shaper {
 public:
  float update(const ShapeSettings &ss, uint32_t now_ms, const Result &r) {
    float dt = have_time_ ? (uint32_t) (now_ms - last_ms_) / 1000.0f : 0.0f;
    have_time_ = true;
    last_ms_ = now_ms;
    float out;
    if (r.state == State::METER_FAULT || std::isnan(r.max_current)) {
      out = r.reported;
      have_offset_ = false;
    } else {
      float w = r.max_current;
      float goal = r.reported - w;
      if (goal < 0.0f) goal = 0.0f;
      if (!have_offset_) {
        // Start where the served value was (after a fault: L + margin).
        offset_ = have_last_ ? last_out_ - w : goal;
        if (offset_ < 0.0f) offset_ = 0.0f;
        have_offset_ = true;
      }
      if (ss.ramp_rate > 0.0f) {
        float step = ss.ramp_rate * dt;
        if (offset_ < goal) {
          offset_ = offset_ + step > goal ? goal : offset_ + step;
        } else if (offset_ > goal) {
          offset_ = offset_ - step < goal ? goal : offset_ - step;
        }
      } else {
        offset_ = goal;
      }
      out = w + offset_;
    }
    if (std::isnan(out)) return out;
    last_out_ = out;
    have_last_ = true;
    return out + ss.dither * next_noise_();
  }

  float offset() const { return offset_; }

 private:
  // Deterministic pseudo-random value in [0, 2].
  float next_noise_() {
    seed_ = seed_ * 1664525u + 1013904223u;
    return (seed_ >> 8) * (2.0f / 16777216.0f);
  }

  bool have_time_ = false;
  bool have_offset_ = false;
  bool have_last_ = false;
  uint32_t last_ms_ = 0;
  float offset_ = 0.0f;
  float last_out_ = 0.0f;
  uint32_t seed_ = 12345u;
};

// Detects that the charger has stopped following the emulated meter.
//
// If the Wall Connector ever judges the meter implausible, it ignores it,
// sometimes for hours, and charges at its own maximum. Pause then has no
// effect either. This watch raises an alarm when, for longer than
// hold_ms, the car draws clearly more than the target (Charging states) or
// any real current keeps flowing while TeslaWBC asks for a pause. It clears
// as soon as the condition ends. Bypass and meter faults are never judged.
struct WatchSettings {
  float over_target_margin = 1.5f;  // A above the target that counts as "not following"
  float pause_current = 1.0f;       // A that counts as "still charging" in a pause
  uint32_t hold_ms = 3u * 60u * 1000u;
};

class ControlWatch {
 public:
  // Call after every compute(); returns true while control is lost.
  bool update(const WatchSettings &ws, uint32_t now_ms, const Result &r) {
    bool violating = false;
    switch (r.state) {
      case State::CHARGING:
      case State::FALLBACK_CHARGING:
        violating = r.max_current > r.effective_target + ws.over_target_margin;
        break;
      case State::PAUSED:
      case State::FALLBACK_PAUSED:
        violating = r.max_current > ws.pause_current;
        break;
      default:
        break;
    }
    if (!violating) {
      since_valid_ = false;
      return false;
    }
    if (!since_valid_) {
      since_valid_ = true;
      since_ms_ = now_ms;
    }
    return (uint32_t) (now_ms - since_ms_) >= ws.hold_ms;
  }

 private:
  bool since_valid_ = false;
  uint32_t since_ms_ = 0;
};

}  // namespace teslawbc
