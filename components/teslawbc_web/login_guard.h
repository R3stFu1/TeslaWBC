// Login lockout with a growing wait, like a FRITZ!Box.
//
// Pure C++ with no ESPHome dependency, so it can be unit-tested on a PC
// (see tests/test_login_guard.cpp).
//
// Each wrong password from a client doubles the time that client has to
// wait before it may try again: 1 s, 2 s, 4 s, ... up to max_wait_ms. While
// it waits, every attempt is refused without checking the password. The
// count only resets after a correct password, or when the client has not
// tried for twice its current wait. Clients are tracked separately, so
// someone guessing from one computer does not lock Aperta out.
#pragma once

#include <cstdint>

namespace teslawbc {

class LoginGuard {
 public:
  static constexpr int SLOTS = 8;
  static constexpr uint32_t BASE_WAIT_MS = 1000;
  static constexpr uint32_t MAX_WAIT_MS = 60u * 60u * 1000u;

  // Wait after the given number of wrong passwords in a row.
  static uint32_t wait_ms(uint8_t fails) {
    if (fails == 0)
      return 0;
    uint32_t w = BASE_WAIT_MS;
    for (uint8_t i = 1; i < fails && w < MAX_WAIT_MS; ++i)
      w *= 2;
    return w < MAX_WAIT_MS ? w : MAX_WAIT_MS;
  }

  // Before checking a password. Returns 0 if the client may try now,
  // otherwise how many ms it still has to wait. A refused attempt counts as
  // an attempt: it restarts the quiet time needed for a reset.
  uint32_t check(uint32_t client, uint32_t now_ms) {
    Slot *s = this->find_(client);
    if (s == nullptr)
      return 0;
    uint32_t wait = wait_ms(s->fails);
    if (elapsed_(now_ms, s->last_ms) >= 2 * wait) {
      *s = Slot{};
      return 0;
    }
    uint32_t since_fail = elapsed_(now_ms, s->fail_ms);
    if (since_fail < wait) {
      s->last_ms = now_ms;
      return wait - since_fail;
    }
    return 0;
  }

  // After a wrong password. Returns the wait that now applies.
  uint32_t fail(uint32_t client, uint32_t now_ms) {
    Slot *s = this->find_(client);
    if (s == nullptr)
      s = this->allocate_(client, now_ms);
    if (s->fails < 32)
      s->fails++;
    s->last_ms = now_ms;
    s->fail_ms = now_ms;
    return wait_ms(s->fails);
  }

  // After a correct password.
  void success(uint32_t client) {
    Slot *s = this->find_(client);
    if (s != nullptr)
      *s = Slot{};
  }

  uint8_t fails(uint32_t client) {
    Slot *s = this->find_(client);
    return s == nullptr ? 0 : s->fails;
  }

 private:
  struct Slot {
    bool used = false;
    uint32_t client = 0;
    uint8_t fails = 0;
    uint32_t last_ms = 0;  // last attempt, wrong or refused
    uint32_t fail_ms = 0;  // last wrong password
  };

  static uint32_t elapsed_(uint32_t now, uint32_t then) { return (uint32_t) (now - then); }

  Slot *find_(uint32_t client) {
    for (auto &s : this->slots_)
      if (s.used && s.client == client)
        return &s;
    return nullptr;
  }

  // A free slot, or else the one idle for longest.
  Slot *allocate_(uint32_t client, uint32_t now_ms) {
    Slot *best = nullptr;
    for (auto &s : this->slots_) {
      if (!s.used) {
        best = &s;
        break;
      }
      if (best == nullptr || elapsed_(now_ms, s.last_ms) > elapsed_(now_ms, best->last_ms))
        best = &s;
    }
    *best = Slot{};
    best->used = true;
    best->client = client;
    return best;
  }

  Slot slots_[SLOTS];
};

}  // namespace teslawbc
