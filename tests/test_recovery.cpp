// Unit tests for components/teslawbc_web/recovery.h and blackbox.h. Run with: make -C tests
#include "../components/teslawbc_web/blackbox.h"
#include "../components/teslawbc_web/recovery.h"

#include <cstdio>
#include <cstring>

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

static const uint32_t MIN = 60000u, HOUR = 60u * MIN;

// ---------------------------------------------------------------- recovery

static void test_recovery_off_by_default() {
  RecoverySettings rs;
  Recovery rec;
  CHECK(!rec.update(rs, 1000, true));
  CHECK(!rec.active());
  CHECK(!rec.power_cut());
}

static void test_recovery_sequence() {
  RecoverySettings rs;
  rs.auto_enabled = true;
  Recovery rec;
  CHECK(!rec.update(rs, 1000, false));
  CHECK(rec.update(rs, 2000, true));  // starts
  CHECK(rec.power_cut());
  CHECK(rec.forces_bypass());
  CHECK(rec.remaining_ms(rs, 2000 + 20000) == 40000);
  rec.update(rs, 2000 + MIN - 1, false);
  CHECK(rec.power_cut());
  rec.update(rs, 2000 + MIN, false);  // power back on, Bypass for 2 h
  CHECK(!rec.power_cut());
  CHECK(rec.phase() == RecoveryPhase::BYPASS);
  CHECK(rec.forces_bypass());
  rec.update(rs, 2000 + MIN + 2 * HOUR - 1, false);
  CHECK(rec.active());
  rec.update(rs, 2000 + MIN + 2 * HOUR, false);
  CHECK(!rec.active());
  CHECK(!rec.forces_bypass());
  CHECK(rec.attempts() == 1);
}

static void test_recovery_once_per_day() {
  RecoverySettings rs;
  rs.auto_enabled = true;
  Recovery rec;
  uint32_t t0 = 5000;
  CHECK(rec.update(rs, t0, true));
  for (uint32_t t = t0; t < t0 + 3 * HOUR; t += 1000) rec.update(rs, t, false);
  CHECK(!rec.active());
  // Trust lost again 5 h after the first start: no second automatic attempt.
  CHECK(!rec.update(rs, t0 + 5 * HOUR, true));
  CHECK(!rec.active());
  CHECK(rec.blocked_ms(rs, t0 + 5 * HOUR) == 19 * HOUR);
  // 24 h after the first start it may try again.
  CHECK(rec.update(rs, t0 + 24 * HOUR, true));
  CHECK(rec.attempts() == 2);
}

static void test_recovery_manual_and_cancel() {
  RecoverySettings rs;  // automatic off
  Recovery rec;
  rec.start(100);
  CHECK(rec.power_cut());
  rec.cancel();
  CHECK(!rec.active());
  CHECK(!rec.power_cut());
  // A manual start also counts for the daily limit of automatic attempts.
  rs.auto_enabled = true;
  CHECK(!rec.update(rs, 200, true));
}

static void test_recovery_wraparound() {
  RecoverySettings rs;
  rs.auto_enabled = true;
  Recovery rec;
  uint32_t t = 0xFFFFFFFFu - 10000u;
  CHECK(rec.update(rs, t, true));
  rec.update(rs, t + MIN, false);  // wraps past zero
  CHECK(rec.phase() == RecoveryPhase::BYPASS);
  // Long idle period: the last start is forgotten, not misread after a wrap.
  for (uint32_t i = 0; i < 30; ++i) rec.update(rs, t + MIN + i * HOUR, false);
  CHECK(rec.blocked_ms(rs, t + 30 * HOUR) == 0);
}

// ---------------------------------------------------------------- blackbox

static Sample good(uint32_t t_ms, float w, float target, float L = 18.0f) {
  Sample s;
  s.t_ms = t_ms;
  for (auto &x : s.w) x = Sample::ca(w);
  s.reported = Sample::ca(w + (L - target) + 0.01f * (t_ms / 1000 % 7));  // dithered
  s.target = Sample::ca(target);
  s.vehicle = Sample::ca(w);
  s.poll_ms = 190;
  s.mode = 1;
  s.state = 1;
  s.flags = Sample::FLAG_METER_OK | Sample::FLAG_POLLING;
  return s;
}

static bool has(const std::vector<Finding> &f, Cause c) {
  for (auto &x : f)
    if (x.cause == c) return true;
  return false;
}

static void test_clean_signal_has_no_findings() {
  std::vector<Sample> s;
  for (uint32_t i = 0; i < 600; ++i) s.push_back(good(i * 1000, 10.0f + (i % 3) * 0.1f, 10.0f));
  CHECK(analyze(s, s.size(), AnalysisSettings{}).empty());
}

static void test_ramped_target_change_is_not_a_jump() {
  std::vector<Sample> s;
  float off = 2.0f;
  for (uint32_t i = 0; i < 100; ++i) {
    off = off + 0.25f > 12.0f ? 12.0f : off + 0.25f;  // ramp 16 -> 6 A
    Sample x = good(i * 1000, 16.0f, 16.0f);
    x.reported = Sample::ca(16.0f + off + 0.01f * (i % 5));
    s.push_back(x);
  }
  CHECK(!has(analyze(s, s.size(), AnalysisSettings{}), Cause::JUMP));
}

static void test_detects_each_cause() {
  AnalysisSettings as;
  // Jump
  {
    std::vector<Sample> s;
    for (uint32_t i = 0; i < 60; ++i) s.push_back(good(i * 1000, 10.0f, i < 30 ? 16.0f : 6.0f));
    auto f = analyze(s, s.size(), as);
    CHECK(has(f, Cause::JUMP));
    CHECK(f.size() == 1);
    CHECK(f[0].first_s == 30 - 59);  // relative to the last sample
    CHECK(std::fabs(f[0].worst - 10.0f) < 0.1f);
  }
  // Below the car
  {
    std::vector<Sample> s;
    for (uint32_t i = 0; i < 60; ++i) s.push_back(good(i * 1000, 10.0f, 10.0f));
    s[20].reported = Sample::ca(9.0f);
    CHECK(has(analyze(s, s.size(), as), Cause::BELOW_CAR));
  }
  // Flat
  {
    std::vector<Sample> s;
    for (uint32_t i = 0; i < 60; ++i) {
      Sample x = good(i * 1000, 10.0f, 10.0f);
      x.reported = Sample::ca(18.0f);
      s.push_back(x);
    }
    auto f = analyze(s, s.size(), as);
    CHECK(has(f, Cause::FLAT));
    CHECK(f.size() == 1);
  }
  // Polling gap
  {
    std::vector<Sample> s;
    for (uint32_t i = 0; i < 60; ++i) s.push_back(good(i * 1000, 10.0f, 10.0f));
    s[40].poll_ms = 2500;
    CHECK(has(analyze(s, s.size(), as), Cause::POLL_GAP));
  }
  // Meter fault
  {
    std::vector<Sample> s;
    for (uint32_t i = 0; i < 60; ++i) s.push_back(good(i * 1000, 10.0f, 10.0f));
    s[10].state = SAMPLE_STATE_METER_FAULT;
    s[10].flags &= ~Sample::FLAG_METER_OK;
    CHECK(has(analyze(s, s.size(), as), Cause::METER_FAULT));
  }
  // Meter and Wall Connector disagree for longer than 30 s (e.g. a CT on the wrong phase)
  {
    std::vector<Sample> s;
    for (uint32_t i = 0; i < 60; ++i) {
      Sample x = good(i * 1000, 10.0f, 10.0f);
      x.vehicle = Sample::ca(16.0f);
      s.push_back(x);
    }
    CHECK(has(analyze(s, s.size(), as), Cause::VEHICLE_MISMATCH));
    // ... but a 10 s difference (vitals lag) is not reported
    for (uint32_t i = 0; i < 60; ++i) s[i].vehicle = Sample::ca(i < 10 ? 16.0f : 10.0f);
    CHECK(!has(analyze(s, s.size(), as), Cause::VEHICLE_MISMATCH));
  }
  // Only samples before the alarm count
  {
    std::vector<Sample> s;
    for (uint32_t i = 0; i < 60; ++i) s.push_back(good(i * 1000, 10.0f, 10.0f));
    s[50].reported = Sample::ca(5.0f);
    CHECK(analyze(s, 40, as).empty());
  }
}

static void test_recorder_captures_incident() {
  Recorder rec;
  AnalysisSettings as;
  uint32_t t = 0;
  const Incident *done = nullptr;
  // 15 min of operation at 4 Hz, with a jump at minute 12.
  for (; t < 15 * MIN; t += 250) done = rec.add(good(t, 10.0f, t < 12 * MIN ? 16.0f : 6.0f), false, 0, as);
  CHECK(done == nullptr);
  CHECK(rec.incidents().empty());
  // Alarm at minute 15
  done = rec.add(good(t, 10.0f, 6.0f), true, 1762590000u, as);
  CHECK(done == nullptr);
  CHECK(rec.capturing());
  for (t += 250; t < 18 * MIN + 2000 && done == nullptr; t += 250)
    done = rec.add(good(t, 10.0f, 6.0f), false, 0, as);
  CHECK(done != nullptr);
  if (done == nullptr) return;
  CHECK(done->id == 1);
  CHECK(done->complete);
  CHECK(done->alarm_epoch == 1762590000u);
  CHECK(done->alarm_index == Recorder::PRE_SAMPLES);
  CHECK(done->samples.size() == Recorder::PRE_SAMPLES + Recorder::POST_SAMPLES);
  CHECK(done->samples[done->alarm_index].t_ms == 15 * MIN);
  CHECK(done->samples.front().t_ms == 5 * MIN);  // 10 min before
  CHECK(has(done->findings, Cause::JUMP));
  CHECK(done->findings[0].first_s == -180);  // the jump 3 min before the alarm
  std::string sum = incident_summary(*done);
  CHECK(sum.find("Incident 1: Reported current jumped (from -180 s)") == 0);
  char row[200];
  int n = csv_row(row, sizeof(row), done->samples[done->alarm_index], done->alarm_ms);
  CHECK(n > 0);
  CHECK(std::strncmp(row, "0.0,10.00,10.00,10.00,", 22) == 0);
}

static void test_recorder_keeps_last_three() {
  Recorder rec;
  AnalysisSettings as;
  uint32_t t = 0;
  for (int k = 0; k < 5; ++k) {
    rec.add(good(t, 10.0f, 10.0f), true, 0, as);
    for (int i = 0; i < 4 * 200; ++i) {
      t += 250;
      rec.add(good(t, 10.0f, 10.0f), false, 0, as);
    }
  }
  CHECK(rec.incidents().size() == 3);
  CHECK(rec.incidents().front().id == 3);
  CHECK(rec.find(5) != nullptr);
  CHECK(rec.find(2) == nullptr);
  CHECK(rec.find(5)->complete);
  CHECK(incident_summary(*rec.find(5)) == "Incident 5: no known cause found");
}

int main() {
  test_recovery_off_by_default();
  test_recovery_sequence();
  test_recovery_once_per_day();
  test_recovery_manual_and_cancel();
  test_recovery_wraparound();
  test_clean_signal_has_no_findings();
  test_ramped_target_change_is_not_a_jump();
  test_detects_each_cause();
  test_recorder_captures_incident();
  test_recorder_keeps_last_three();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
