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
  STOCK = 0,   // report the honest wallbox current: no control at all
  MANUAL = 1,  // hold the target current T
  PAUSE = 2,   // report more than L: the charger opens its contactor
};

enum class State : uint8_t {
  STOCK = 0,
  CHARGING = 1,           // Manual, holding the target
  PAUSED = 2,             // Pause mode, or Manual with a target below the minimum
  FALLBACK_CHARGING = 3,  // Home Assistant silent: holding the fallback target
  FALLBACK_PAUSED = 4,    // Home Assistant silent: fallback target is "pause"
  METER_FAULT = 5,        // no valid meter reading: pausing for safety
};

inline const char *state_name(State s) {
  switch (s) {
    case State::STOCK: return "Stock";
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
  float fallback_target = 6.0f;   // used when Home Assistant is silent; < min_current = pause
  uint32_t fallback_timeout_ms = 5u * 60u * 1000u;
  uint32_t meter_timeout_ms = 3000u;
};

struct Inputs {
  uint32_t now_ms = 0;
  // Meter: last successful reading and the phase currents it gave.
  bool meter_seen = false;
  uint32_t last_meter_ms = 0;
  float current[3] = {NAN, NAN, NAN};
  // Home Assistant: requested mode and target, and when we last heard from it.
  Mode mode = Mode::STOCK;
  float target = 6.0f;
  uint32_t last_ha_ms = 0;
};

struct Result {
  float reported = NAN;          // R, served on CT1 to CT3
  float effective_target = NAN;  // T actually applied; NAN when not regulating
  float max_current = NAN;       // max(W1, W2, W3), NAN on meter fault
  State state = State::METER_FAULT;
  bool meter_ok = false;
  bool fallback = false;         // true while Home Assistant is considered silent
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
  r.fallback = (uint32_t) (in.now_ms - in.last_ha_ms) > cfg.fallback_timeout_ms;

  if (!meter_ok) {
    // Without a measurement we cannot echo the car's current, so we stop it.
    r.state = State::METER_FAULT;
    r.reported = limit + cfg.pause_margin;
    return r;
  }
  r.max_current = w_max;

  // Stock is the kill switch: honest current, independent of Home Assistant.
  if (in.mode == Mode::STOCK) {
    r.state = State::STOCK;
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

}  // namespace teslawbc
