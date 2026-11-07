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

// A healthy meter reading at t = 10 s, Home Assistant heard from just now.
static Inputs base(float w1, float w2, float w3) {
  Inputs in;
  in.now_ms = 10000;
  in.meter_seen = true;
  in.last_meter_ms = 9800;
  in.current[0] = w1;
  in.current[1] = w2;
  in.current[2] = w3;
  in.last_ha_ms = 9000;
  return in;
}

static void test_stock_reports_honest_current() {
  Settings cfg;
  Inputs in = base(15.2f, 15.9f, 15.4f);
  in.mode = Mode::STOCK;
  Result r = compute(cfg, in);
  CHECK(r.state == State::STOCK);
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
  in.last_ha_ms = in.now_ms;
  Result r = compute(cfg, in);
  CHECK(r.state == State::METER_FAULT);
  CHECK(!r.meter_ok);
  CHECK(near(r.reported, 20.0f));
  // Also in Stock mode.
  in.mode = Mode::STOCK;
  CHECK(compute(cfg, in).state == State::METER_FAULT);
  // Never seen a reading (just booted).
  Inputs fresh;
  fresh.mode = Mode::STOCK;
  CHECK(compute(cfg, fresh).state == State::METER_FAULT);
}

static void test_meter_nan_is_fault() {
  Settings cfg;
  Inputs in = base(10.0f, NAN, 10.0f);
  in.mode = Mode::STOCK;
  CHECK(compute(cfg, in).state == State::METER_FAULT);
}

static void test_fallback_when_ha_silent() {
  Settings cfg;  // fallback 6 A after 5 min
  Inputs in = base(12.0f, 12.0f, 12.0f);
  in.mode = Mode::MANUAL;
  in.target = 12.0f;
  in.now_ms = 400000;
  in.last_meter_ms = 399900;
  in.last_ha_ms = 400000 - cfg.fallback_timeout_ms;  // exactly at the limit: still fine
  CHECK(compute(cfg, in).state == State::CHARGING);
  in.last_ha_ms -= 1;
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
  // Stock ignores Home Assistant entirely.
  in.mode = Mode::STOCK;
  CHECK(compute(cfg, in).state == State::STOCK);
}

static void test_millis_wraparound() {
  Settings cfg;
  Inputs in = base(8.0f, 8.0f, 8.0f);
  in.mode = Mode::MANUAL;
  in.target = 8.0f;
  in.last_meter_ms = 0xFFFFFF00u;
  in.last_ha_ms = 0xFFFFFF00u;
  in.now_ms = 0x00000100u;  // 512 ms later, after the wrap
  Result r = compute(cfg, in);
  CHECK(r.meter_ok);
  CHECK(!r.fallback);
  CHECK(r.state == State::CHARGING);
}

int main() {
  test_stock_reports_honest_current();
  test_manual_formula();
  test_reported_follows_car_one_to_one();
  test_reported_never_below_wallbox_current();
  test_target_is_clamped();
  test_target_below_minimum_pauses();
  test_pause_mode();
  test_manual_with_idle_car_allows_start();
  test_meter_timeout_pauses();
  test_meter_nan_is_fault();
  test_fallback_when_ha_silent();
  test_millis_wraparound();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
