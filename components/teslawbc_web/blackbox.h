// Incident recorder for the Control Lost alarm.
//
// Pure C++ with no ESPHome dependency, so it can be unit-tested on a PC
// (see tests/test_blackbox.cpp).
//
// TeslaWBC records one sample per second into a 10-minute ring. When
// Control Lost is raised, it freezes that ring, keeps recording for 3 more
// minutes, and stores the result as an incident (the last 3 are kept in
// RAM). Then it looks for the patterns known to make a Wall Connector
// distrust its meter. Tesla does not say why it stopped trusting, so these
// are candidates, not proof.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace teslawbc {

// One second of operation. Currents in centiamps; NO_VALUE when unknown.
struct Sample {
  static constexpr int16_t NO_VALUE = INT16_MIN;
  uint32_t t_ms = 0;  // millis()
  int16_t w[3] = {NO_VALUE, NO_VALUE, NO_VALUE};  // meter, per phase
  int16_t reported = NO_VALUE;                    // R served to the charger
  int16_t target = NO_VALUE;                      // effective target
  int16_t vehicle = NO_VALUE;                     // Wall Connector's own vehicle current
  uint16_t poll_ms = 0;                           // charger poll interval, 0 = unknown
  uint8_t mode = 0;                               // Mode
  uint8_t state = 0;                              // State
  uint8_t flags = 0;                              // FLAG_*

  static constexpr uint8_t FLAG_METER_OK = 1;
  static constexpr uint8_t FLAG_POLLING = 2;
  static constexpr uint8_t FLAG_LOST = 4;      // Control Lost alarm on
  static constexpr uint8_t FLAG_RECOVERY = 8;  // recovery running

  static int16_t ca(float a) {
    if (std::isnan(a) || std::isinf(a))
      return NO_VALUE;
    float v = a * 100.0f;
    if (v > 32000.0f) v = 32000.0f;
    if (v < -32000.0f) v = -32000.0f;
    return (int16_t) std::lround(v);
  }
  static float amps(int16_t v) { return v == NO_VALUE ? NAN : v / 100.0f; }
  float w_max() const {
    float m = NAN;
    for (int16_t v : w) {
      if (v == NO_VALUE)
        return NAN;
      float a = std::fabs(v / 100.0f);
      if (std::isnan(m) || a > m)
        m = a;
    }
    return m;
  }
};

// State value for a meter fault (State::METER_FAULT in control.h).
static constexpr uint8_t SAMPLE_STATE_METER_FAULT = 5;

enum class Cause : uint8_t {
  BELOW_CAR = 0,         // reported less than the wallbox drew
  FLAT = 1,              // reported value did not change
  JUMP = 2,              // reported value jumped more than the car's current explains
  POLL_GAP = 3,          // the charger stopped polling or polled slowly
  METER_FAULT = 4,       // no valid meter reading
  VEHICLE_MISMATCH = 5,  // meter and Wall Connector disagree on the car's current
};

inline const char *cause_key(Cause c) {
  switch (c) {
    case Cause::BELOW_CAR: return "below_car";
    case Cause::FLAT: return "flat";
    case Cause::JUMP: return "jump";
    case Cause::POLL_GAP: return "poll_gap";
    case Cause::METER_FAULT: return "meter_fault";
    case Cause::VEHICLE_MISMATCH: return "vehicle_mismatch";
  }
  return "unknown";
}

inline const char *cause_text(Cause c) {
  switch (c) {
    case Cause::BELOW_CAR: return "Reported current below what the wallbox drew";
    case Cause::FLAT: return "Reported current stayed flat";
    case Cause::JUMP: return "Reported current jumped";
    case Cause::POLL_GAP: return "Charger stopped polling or polled slowly";
    case Cause::METER_FAULT: return "Meter fault";
    case Cause::VEHICLE_MISMATCH: return "Meter and Wall Connector disagree on the car's current";
  }
  return "Unknown";
}

struct Finding {
  Cause cause;
  int32_t first_s;  // seconds relative to the alarm (negative = before)
  uint16_t count;   // samples affected
  float worst;      // largest deviation in A (or s for FLAT and POLL_GAP)
};

struct AnalysisSettings {
  float ramp_rate = 0.25f;       // A/s allowed for TeslaWBC's own offset changes
  float jump_margin = 0.5f;      // A on top of the ramp per second
  float below_margin = 0.05f;    // A
  uint32_t flat_s = 30;          // s without any change
  uint16_t slow_poll_ms = 1000;  // poll interval that counts as a gap
  float mismatch_a = 2.0f;       // A between meter and Wall Connector
  uint32_t mismatch_s = 30;      // s it must persist
};

// Looks for known distrust triggers in samples[0 .. alarm_index).
inline std::vector<Finding> analyze(const std::vector<Sample> &s, size_t alarm_index, const AnalysisSettings &as) {
  std::vector<Finding> out;
  if (alarm_index > s.size())
    alarm_index = s.size();
  const uint32_t alarm_ms = alarm_index < s.size() ? s[alarm_index].t_ms : (s.empty() ? 0 : s.back().t_ms);
  auto rel = [&](size_t i) { return (int32_t) (s[i].t_ms - alarm_ms) / 1000; };
  auto note = [&](Cause c, size_t i, float dev) {
    for (auto &f : out) {
      if (f.cause == c) {
        f.count++;
        if (dev > f.worst) f.worst = dev;
        return;
      }
    }
    out.push_back(Finding{c, rel(i), 1, dev});
  };

  size_t flat_start = 0, mismatch_start = 0;
  bool flat_run = false, mismatch_run = false;
  for (size_t i = 0; i < alarm_index; ++i) {
    const Sample &a = s[i];
    float w = a.w_max();
    float r = Sample::amps(a.reported);
    bool recovering = a.flags & Sample::FLAG_RECOVERY;

    if (a.state == SAMPLE_STATE_METER_FAULT || !(a.flags & Sample::FLAG_METER_OK))
      note(Cause::METER_FAULT, i, 0.0f);

    if (!std::isnan(w) && !std::isnan(r) && r < w - as.below_margin)
      note(Cause::BELOW_CAR, i, w - r);

    if (!(a.flags & Sample::FLAG_POLLING) || a.poll_ms >= as.slow_poll_ms)
      note(Cause::POLL_GAP, i, a.poll_ms / 1000.0f);

    if (i > 0 && !recovering) {
      const Sample &p = s[i - 1];
      float pw = p.w_max(), pr = Sample::amps(p.reported);
      float dt = (a.t_ms - p.t_ms) / 1000.0f;
      if (!std::isnan(w) && !std::isnan(r) && !std::isnan(pw) && !std::isnan(pr) && dt > 0.0f && dt < 5.0f &&
          p.state != SAMPLE_STATE_METER_FAULT && a.state != SAMPLE_STATE_METER_FAULT) {
        float excess = std::fabs((r - pr) - (w - pw)) - as.ramp_rate * dt;
        if (excess > as.jump_margin)
          note(Cause::JUMP, i, std::fabs(r - pr));
      }
      // Flat: bit-identical for flat_s seconds.
      if (a.reported == p.reported && a.reported != Sample::NO_VALUE) {
        if (!flat_run) {
          flat_run = true;
          flat_start = i - 1;
        }
        float len = (a.t_ms - s[flat_start].t_ms) / 1000.0f;
        if (len >= as.flat_s) {
          bool first = true;
          for (auto &f : out)
            if (f.cause == Cause::FLAT) first = false;
          if (first) {
            out.push_back(Finding{Cause::FLAT, rel(flat_start), 0, 0.0f});
          }
          for (auto &f : out)
            if (f.cause == Cause::FLAT) {
              f.count++;
              if (len > f.worst) f.worst = len;
            }
        }
      } else {
        flat_run = false;
      }
    }

    float v = Sample::amps(a.vehicle);
    if (!std::isnan(v) && !std::isnan(w) && std::fabs(v - w) > as.mismatch_a) {
      if (!mismatch_run) {
        mismatch_run = true;
        mismatch_start = i;
      }
      if ((a.t_ms - s[mismatch_start].t_ms) / 1000u >= as.mismatch_s)
        note(Cause::VEHICLE_MISMATCH, i, std::fabs(v - w));
    } else {
      mismatch_run = false;
    }
  }
  return out;
}

struct Incident {
  uint32_t id = 0;
  uint32_t alarm_ms = 0;     // millis() when Control Lost was raised
  uint32_t alarm_epoch = 0;  // Unix time of the alarm, 0 if the clock was not set
  size_t alarm_index = 0;    // index of the alarm sample in samples
  bool complete = false;     // the 3 minutes after the alarm are recorded
  std::vector<Sample> samples;
  std::vector<Finding> findings;
};

class Recorder {
 public:
  static constexpr size_t PRE_SAMPLES = 600;  // 10 min at 1 Hz
  static constexpr size_t POST_SAMPLES = 180; // 3 min at 1 Hz
  static constexpr size_t KEEP = 3;
  static constexpr uint32_t PERIOD_MS = 1000;

  // Offer a sample (every 250 ms is fine): one per second is kept.
  // alarm_rising: Control Lost was just raised. Returns the incident that
  // just completed, or nullptr.
  const Incident *add(const Sample &smp, bool alarm_rising, uint32_t epoch_now, const AnalysisSettings &as) {
    if (alarm_rising && !capturing_)
      this->begin_incident_(smp, epoch_now);
    if (have_sample_ && (uint32_t) (smp.t_ms - last_ms_) < PERIOD_MS)
      return nullptr;
    have_sample_ = true;
    last_ms_ = smp.t_ms;
    ring_.push_back(smp);
    if (ring_.size() > PRE_SAMPLES)
      ring_.pop_front();
    if (!capturing_)
      return nullptr;
    Incident &inc = incidents_.back();
    inc.samples.push_back(smp);
    if (inc.samples.size() - inc.alarm_index >= POST_SAMPLES) {
      capturing_ = false;
      inc.complete = true;
      inc.findings = analyze(inc.samples, inc.alarm_index, as);
      return &inc;
    }
    return nullptr;
  }

  const std::deque<Incident> &incidents() const { return incidents_; }
  const Incident *find(uint32_t id) const {
    for (const auto &i : incidents_)
      if (i.id == id)
        return &i;
    return nullptr;
  }
  bool capturing() const { return capturing_; }

 private:
  void begin_incident_(const Sample &smp, uint32_t epoch_now) {
    if (incidents_.size() >= KEEP)
      incidents_.pop_front();
    Incident inc;
    inc.id = ++next_id_;
    inc.alarm_ms = smp.t_ms;
    inc.alarm_epoch = epoch_now;
    inc.samples.reserve(ring_.size() + POST_SAMPLES + 1);
    inc.samples.assign(ring_.begin(), ring_.end());
    inc.alarm_index = inc.samples.size();
    incidents_.push_back(std::move(inc));
    capturing_ = true;
    have_sample_ = false;  // record the alarm sample itself
  }

  std::deque<Sample> ring_;
  std::deque<Incident> incidents_;
  bool capturing_ = false;
  bool have_sample_ = false;
  uint32_t last_ms_ = 0;
  uint32_t next_id_ = 0;
};

// Short text for the alarm, e.g. "Incident 2: jump 6.2 A at -250 s; poll gap".
inline std::string incident_summary(const Incident &inc) {
  char buf[64];
  std::string s;
  snprintf(buf, sizeof(buf), "Incident %u: ", (unsigned) inc.id);
  s = buf;
  if (!inc.complete) {
    s += "recording";
    return s;
  }
  if (inc.findings.empty()) {
    s += "no known cause found";
    return s;
  }
  bool first = true;
  for (const auto &f : inc.findings) {
    if (!first) s += "; ";
    first = false;
    s += cause_text(f.cause);
    snprintf(buf, sizeof(buf), " (from %d s)", (int) f.first_s);
    s += buf;
  }
  return s;
}

// One CSV row per sample; header first.
inline const char *csv_header() {
  return "t_s,w1_a,w2_a,w3_a,reported_a,target_a,vehicle_a,poll_ms,mode,state,meter_ok,polling,control_lost,"
         "recovery\n";
}

inline int csv_row(char *buf, size_t size, const Sample &s, uint32_t alarm_ms) {
  auto f = [](int16_t v, char *out) {
    if (v == Sample::NO_VALUE)
      out[0] = '\0';
    else
      snprintf(out, 12, "%.2f", v / 100.0f);
  };
  char w1[12], w2[12], w3[12], r[12], t[12], v[12];
  f(s.w[0], w1);
  f(s.w[1], w2);
  f(s.w[2], w3);
  f(s.reported, r);
  f(s.target, t);
  f(s.vehicle, v);
  int32_t rel = (int32_t) (s.t_ms - alarm_ms);
  return snprintf(buf, size, "%.1f,%s,%s,%s,%s,%s,%s,%u,%u,%u,%u,%u,%u,%u\n", rel / 1000.0, w1, w2, w3, r, t, v,
                  (unsigned) s.poll_ms, (unsigned) s.mode, (unsigned) s.state,
                  (unsigned) !!(s.flags & Sample::FLAG_METER_OK), (unsigned) !!(s.flags & Sample::FLAG_POLLING),
                  (unsigned) !!(s.flags & Sample::FLAG_LOST), (unsigned) !!(s.flags & Sample::FLAG_RECOVERY));
}

}  // namespace teslawbc
