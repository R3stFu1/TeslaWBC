// Unit tests for teslawbc/control.h. Build and run with: make -C tests
#include "../teslawbc/control.h"

#include <cstdio>

using namespace teslawbc;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    ++checks;                                                          \
    if (!(cond)) {                                                     \
      ++failures;                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                  \
  } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// A healthy meter reading at t = 10 s, Aperta heard from just now.
static Inputs base(float w1, float w2, float w3) {
  Inputs in;
  in.now_ms = 10000;
  in.meter_seen = true;
  in.last_meter_ms = 9800;
  in.current[0] = w1;
  in.current[1] = w2;
  in.current[2] = w3;
  in.last_cmd_ms = 9000;
  return in;
}

static void test_bypass_reports_honest_current() {
  Settings cfg;
  Inputs in = base(15.2f, 15.9f, 15.4f);
  in.mode = Mode::BYPASS;
  Result r = compute(cfg, in);
  CHECK(r.state == State::BYPASS);
  CHECK(near(r.reported, 15.9f));
  CHECK(std::isnan(r.effective_target));
}

static void test_manual_formula() {
  Settings cfg;  // L = 18
  Inputs in = base(10.0f, 10.5f, 9.8f);
  in.mode = Mode::MANUAL;
  in.target = 10.0f;
  Result r = compute(cfg, in);
  CHECK(r.state == State::CHARGING);
  CHECK(near(r.effective_target, 10.0f));
  CHECK(near(r.reported, 10.5f + 8.0f));  // max(W) + (L - T)
  // Car draws exactly T: R equals L, the charger holds.
  in.current[0] = in.current[1] = in.current[2] = 10.0f;
  CHECK(near(compute(cfg, in).reported, 18.0f));
  // Car draws less than T: R below L, the charger lets it climb.
  in.current[0] = in.current[1] = in.current[2] = 8.0f;
  CHECK(compute(cfg, in).reported < 18.0f);
}

static void test_reported_follows_car_one_to_one() {
  Settings cfg;
  Inputs in = base(6.0f, 6.0f, 6.0f);
  in.mode = Mode::MANUAL;
  in.target = 12.0f;
  float r1 = compute(cfg, in).reported;
  in.current[0] = in.current[1] = in.current[2] = 9.5f;
  float r2 = compute(cfg, in).reported;
  CHECK(near(r2 - r1, 3.5f));
}

static void test_reported_never_below_wallbox_current() {
  Settings cfg;
  for (int mode = 0; mode < 3; ++mode) {
    for (float t = 0.0f; t <= 20.0f; t += 0.5f) {
      for (float w = 0.0f; w <= 17.0f; w += 0.5f) {
        Inputs in = base(w, w * 0.9f, w * 0.95f);
        in.mode = static_cast<Mode>(mode);
        in.target = t;
        CHECK(compute(cfg, in).reported >= w);
      }
    }
  }
}

static void test_target_is_clamped() {
  Settings cfg;
  Inputs in = base(0.0f, 0.0f, 0.0f);
  in.mode = Mode::MANUAL;
  in.target = 25.0f;
  Result r = compute(cfg, in);
  CHECK(near(r.effective_target, 16.0f));  // I_max
  CHECK(near(r.reported, 2.0f));
  // I_max above L is capped at L.
  cfg.max_output = 20.0f;
  CHECK(near(compute(cfg, in).effective_target, 18.0f));
}

static void test_target_below_minimum_pauses() {
  Settings cfg;
  Inputs in = base(6.0f, 6.0f, 6.0f);
  in.mode = Mode::MANUAL;
  in.target = 5.0f;
  Result r = compute(cfg, in);
  CHECK(r.state == State::PAUSED);
  CHECK(near(r.reported, 20.0f));
  in.target = NAN;
  CHECK(compute(cfg, in).state == State::PAUSED);
}

static void test_min_current_configurable() {
  Settings cfg;
  cfg.min_current = 5.0f;
  Inputs in = base(5.0f, 5.0f, 5.0f);
  in.mode = Mode::MANUAL;
  in.target = 5.0f;
  Result r = compute(cfg, in);
  CHECK(r.state == State::CHARGING);
  CHECK(near(r.effective_target, 5.0f));
  in.target = 4.0f;
  CHECK(compute(cfg, in).state == State::PAUSED);
}

static void test_pause_mode() {
  Settings cfg;
  Inputs in = base(0.0f, 0.0f, 0.0f);
  in.mode = Mode::PAUSE;
  in.target = 16.0f;
  Result r = compute(cfg, in);
  CHECK(r.state == State::PAUSED);
  CHECK(near(r.reported, 20.0f));  // L + 2
  // Start gate: a charger refuses to start above L - 5. Pause must stay above it.
  CHECK(r.reported > cfg.conductor_limit - 5.0f);
}

static void test_manual_with_idle_car_allows_start() {
  // With no car drawing, R must stay below L - 5 so the charger starts a session.
  Settings cfg;
  Inputs in = base(0.0f, 0.0f, 0.0f);
  in.mode = Mode::MANUAL;
  for (float t = 6.0f; t <= 16.0f; t += 1.0f) {
    in.target = t;
    CHECK(compute(cfg, in).reported < cfg.conductor_limit - 5.0f);
  }
}

static void test_meter_timeout_pauses() {
  Settings cfg;
  Inputs in = base(10.0f, 10.0f, 10.0f);
  in.mode = Mode::MANUAL;
  in.target = 16.0f;
  in.now_ms = in.last_meter_ms + cfg.meter_timeout_ms + 1;
  in.last_cmd_ms = in.now_ms;
  Result r = compute(cfg, in);
  CHECK(r.state == State::METER_FAULT);
  CHECK(!r.meter_ok);
  CHECK(near(r.reported, 20.0f));
  // Also in Bypass mode.
  in.mode = Mode::BYPASS;
  CHECK(compute(cfg, in).state == State::METER_FAULT);
  // Never seen a reading (just booted).
  Inputs fresh;
  fresh.mode = Mode::BYPASS;
  CHECK(compute(cfg, fresh).state == State::METER_FAULT);
}

static void test_meter_nan_is_fault() {
  Settings cfg;
  Inputs in = base(10.0f, NAN, 10.0f);
  in.mode = Mode::BYPASS;
  CHECK(compute(cfg, in).state == State::METER_FAULT);
}

static void test_fallback_when_controller_silent() {
  Settings cfg;  // fallback 6 A after 5 min
  Inputs in = base(12.0f, 12.0f, 12.0f);
  in.mode = Mode::MANUAL;
  in.target = 12.0f;
  in.now_ms = 400000;
  in.last_meter_ms = 399900;
  in.last_cmd_ms = 400000 - cfg.fallback_timeout_ms;  // exactly at the limit: still fine
  CHECK(compute(cfg, in).state == State::CHARGING);
  in.last_cmd_ms -= 1;
  Result r = compute(cfg, in);
  CHECK(r.fallback);
  CHECK(r.state == State::FALLBACK_CHARGING);
  CHECK(near(r.effective_target, 6.0f));
  CHECK(near(r.reported, 12.0f + 12.0f));
  // Pause mode also falls back to the fallback target.
  in.mode = Mode::PAUSE;
  CHECK(compute(cfg, in).state == State::FALLBACK_CHARGING);
  // Fallback target 0 means pause.
  cfg.fallback_target = 0.0f;
  CHECK(compute(cfg, in).state == State::FALLBACK_PAUSED);
  // Bypass ignores Aperta entirely.
  in.mode = Mode::BYPASS;
  CHECK(compute(cfg, in).state == State::BYPASS);
}

static void test_millis_wraparound() {
  Settings cfg;
  Inputs in = base(8.0f, 8.0f, 8.0f);
  in.mode = Mode::MANUAL;
  in.target = 8.0f;
  in.last_meter_ms = 0xFFFFFF00u;
  in.last_cmd_ms = 0xFFFFFF00u;
  in.now_ms = 0x00000100u;  // 512 ms later, after the wrap
  Result r = compute(cfg, in);
  CHECK(r.meter_ok);
  CHECK(!r.fallback);
  CHECK(r.state == State::CHARGING);
}

static void test_control_watch() {
  Settings cfg;
  WatchSettings ws;  // 1.5 A margin, 1 A in pause, 3 min hold
  ControlWatch watch;
  Inputs in = base(10.0f, 10.0f, 10.0f);
  in.mode = Mode::MANUAL;
  in.target = 10.0f;
  // Following the target: never an alarm.
  CHECK(!watch.update(ws, 0, compute(cfg, in)));
  CHECK(!watch.update(ws, 600000, compute(cfg, in)));
  // Drawing 16 A against a 10 A target: alarm only after the hold time.
  in.current[0] = in.current[1] = in.current[2] = 16.0f;
  CHECK(!watch.update(ws, 1000000, compute(cfg, in)));
  CHECK(!watch.update(ws, 1000000 + ws.hold_ms - 1, compute(cfg, in)));
  CHECK(watch.update(ws, 1000000 + ws.hold_ms, compute(cfg, in)));
  // Back on target: clears at once, and the timer restarts.
  in.current[0] = in.current[1] = in.current[2] = 10.0f;
  CHECK(!watch.update(ws, 1300000, compute(cfg, in)));
  in.current[0] = in.current[1] = in.current[2] = 16.0f;
  CHECK(!watch.update(ws, 1300001, compute(cfg, in)));
  // Current flowing in Pause.
  ControlWatch pw;
  in.mode = Mode::PAUSE;
  in.current[0] = in.current[1] = in.current[2] = 6.0f;
  CHECK(!pw.update(ws, 0, compute(cfg, in)));
  CHECK(pw.update(ws, ws.hold_ms, compute(cfg, in)));
  // Bypass is never judged.
  ControlWatch bw;
  in.mode = Mode::BYPASS;
  CHECK(!bw.update(ws, 0, compute(cfg, in)));
  CHECK(!bw.update(ws, 10 * ws.hold_ms, compute(cfg, in)));
}

static void test_shaper_ramps_target_changes() {
  Settings cfg;
  ShapeSettings ss;  // 0.25 A/s, dither 0.05 A
  ss.dither = 0.0f;
  Shaper sh;
  Inputs in = base(16.0f, 16.0f, 16.0f);
  in.mode = Mode::MANUAL;
  in.target = 16.0f;
  CHECK(near(sh.update(ss, 0, compute(cfg, in)), 16.0f + 2.0f));  // first value: no ramp
  // Target 16 -> 6 A: the offset climbs from 2 to 12 A at 0.25 A/s.
  in.target = 6.0f;
  CHECK(near(sh.update(ss, 1000, compute(cfg, in)), 16.0f + 2.25f));
  CHECK(near(sh.update(ss, 5000, compute(cfg, in)), 16.0f + 3.25f));
  // The car's own current still passes 1:1 during the ramp.
  in.current[0] = in.current[1] = in.current[2] = 15.0f;
  CHECK(near(sh.update(ss, 5000, compute(cfg, in)), 15.0f + 3.25f));
  // After 40 s the ramp is done and R is exactly the formula.
  float r = 0;
  for (uint32_t t = 5250; t <= 45000; t += 250) r = sh.update(ss, t, compute(cfg, in));
  CHECK(near(r, compute(cfg, in).reported));
  // Up again: the offset falls just as smoothly.
  in.target = 16.0f;
  CHECK(near(sh.update(ss, 46000, compute(cfg, in)), 15.0f + 11.75f));
}

static void test_shaper_never_steps() {
  // Whatever the mode changes, consecutive served values differ by no more
  // than the car's own change plus ramp_rate x dt plus the dither.
  Settings cfg;
  ShapeSettings ss;
  Shaper sh;
  Inputs in = base(10.0f, 10.0f, 10.0f);
  in.mode = Mode::MANUAL;
  in.target = 10.0f;
  float prev = sh.update(ss, 0, compute(cfg, in));
  uint32_t t = 0;
  const Mode modes[] = {Mode::PAUSE, Mode::BYPASS, Mode::MANUAL, Mode::PAUSE, Mode::MANUAL, Mode::BYPASS};
  for (Mode m : modes) {
    in.mode = m;
    in.target = (m == Mode::MANUAL) ? (in.target > 10.0f ? 6.0f : 16.0f) : in.target;
    for (int i = 0; i < 200; ++i) {
      t += 250;
      in.now_ms = t;
      in.last_meter_ms = t;
      in.last_cmd_ms = t;
      Result res = compute(cfg, in);
      float v = sh.update(ss, t, res);
      CHECK(std::fabs(v - prev) <= ss.ramp_rate * 0.25f + 2.0f * ss.dither + 1e-4f);
      CHECK(v >= res.max_current);
      prev = v;
    }
  }
}

static void test_shaper_dither_never_flat() {
  Settings cfg;
  ShapeSettings ss;
  Shaper sh;
  Inputs in = base(0.0f, 0.0f, 0.0f);
  in.mode = Mode::PAUSE;
  float lo = 1e9f, hi = -1e9f;
  int changes = 0;
  float prev = NAN;
  for (uint32_t t = 0; t < 60000; t += 250) {
    in.now_ms = t;
    in.last_meter_ms = t;
    in.last_cmd_ms = t;
    float v = sh.update(ss, t, compute(cfg, in));
    if (t > 40000) {  // after the start-up ramp
      lo = std::fmin(lo, v);
      hi = std::fmax(hi, v);
      if (!std::isnan(prev) && v != prev) ++changes;
      prev = v;
    }
  }
  CHECK(lo >= 20.0f - 1e-4f);         // only adds: L + margin is the floor
  CHECK(hi <= 20.0f + 0.1f + 1e-4f);  // and at most 2 x dither on top
  CHECK(changes > 50);
}

static void test_shaper_meter_fault_is_immediate() {
  Settings cfg;
  ShapeSettings ss;
  ss.dither = 0.0f;
  Shaper sh;
  Inputs in = base(10.0f, 10.0f, 10.0f);
  in.mode = Mode::MANUAL;
  in.target = 10.0f;
  CHECK(near(sh.update(ss, 10000, compute(cfg, in)), 18.0f));
  // Meter silent: L + margin at once.
  in.now_ms = 20000;
  Result fault = compute(cfg, in);
  CHECK(fault.state == State::METER_FAULT);
  CHECK(near(sh.update(ss, 20000, fault), 20.0f));
  // Meter back, car still at 10 A: start at the fault value, ramp down to 18.
  in.last_meter_ms = 20250;
  in.now_ms = 20250;
  in.last_cmd_ms = 20250;
  CHECK(near(sh.update(ss, 20250, compute(cfg, in)), 20.0f - 0.0625f));
  // Boot: the first value ever is a fault, then the meter appears in Bypass.
  Shaper boot;
  Inputs fresh;
  CHECK(near(boot.update(ss, 0, compute(cfg, fresh)), 20.0f));
  Inputs up = base(0.0f, 0.0f, 0.0f);
  up.mode = Mode::BYPASS;
  CHECK(near(boot.update(ss, 1000, compute(cfg, up)), 20.0f - 0.25f));
}

int main() {
  test_bypass_reports_honest_current();
  test_manual_formula();
  test_reported_follows_car_one_to_one();
  test_reported_never_below_wallbox_current();
  test_target_is_clamped();
  test_target_below_minimum_pauses();
  test_min_current_configurable();
  test_pause_mode();
  test_manual_with_idle_car_allows_start();
  test_meter_timeout_pauses();
  test_meter_nan_is_fault();
  test_fallback_when_controller_silent();
  test_millis_wraparound();
  test_control_watch();
  test_shaper_ramps_target_changes();
  test_shaper_never_steps();
  test_shaper_dither_never_flat();
  test_shaper_meter_fault_is_immediate();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
