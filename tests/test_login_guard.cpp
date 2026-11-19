// Unit tests for components/teslawbc_web/login_guard.h. Run with: make -C tests
#include "../components/teslawbc_web/login_guard.h"

#include <cstdio>

using teslawbc::LoginGuard;

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

static void test_wait_doubles_and_caps() {
  CHECK(LoginGuard::wait_ms(0) == 0);
  CHECK(LoginGuard::wait_ms(1) == 1000);
  CHECK(LoginGuard::wait_ms(2) == 2000);
  CHECK(LoginGuard::wait_ms(5) == 16000);
  CHECK(LoginGuard::wait_ms(12) == 2048000);
  CHECK(LoginGuard::wait_ms(13) == LoginGuard::MAX_WAIT_MS);
  CHECK(LoginGuard::wait_ms(32) == LoginGuard::MAX_WAIT_MS);
}

static void test_each_wrong_password_waits_longer() {
  LoginGuard g;
  const uint32_t ip = 0xC0A8B21Eu;
  uint32_t t = 100000;
  CHECK(g.check(ip, t) == 0);
  CHECK(g.fail(ip, t) == 1000);
  CHECK(g.check(ip, t + 500) == 500);  // refused, 0.5 s left
  CHECK(g.check(ip, t + 1000) == 0);   // may try again
  t += 1000;
  CHECK(g.fail(ip, t) == 2000);
  CHECK(g.check(ip, t + 1999) == 1);
  CHECK(g.check(ip, t + 2000) == 0);
  t += 2000;
  CHECK(g.fail(ip, t) == 4000);
  CHECK(g.fails(ip) == 3);
}

static void test_reset_after_twice_the_wait_without_attempts() {
  LoginGuard g;
  const uint32_t ip = 1;
  uint32_t t = 0;
  for (int i = 0; i < 4; ++i) {
    t += 60000;
    g.fail(ip, t);
  }
  CHECK(g.fails(ip) == 4);  // wait 8 s
  // A refused attempt at +5 s restarts the quiet time.
  CHECK(g.check(ip, t + 5000) == 3000);
  // 16 s after the last wrong password, but only 11 s after the refused try.
  CHECK(g.check(ip, t + 16000) == 0);
  CHECK(g.fails(ip) == 4);
  // Quiet for 16 s after that attempt: reset.
  CHECK(g.check(ip, t + 32000) == 0);
  CHECK(g.fails(ip) == 0);
}

static void test_correct_password_resets() {
  LoginGuard g;
  g.fail(7, 0);
  g.fail(7, 1000);
  g.success(7);
  CHECK(g.fails(7) == 0);
  CHECK(g.check(7, 1001) == 0);
}

static void test_clients_are_separate() {
  LoginGuard g;
  for (int i = 0; i < 10; ++i) g.fail(100, i * 100000u);
  CHECK(g.check(100, 900001) > 0);
  CHECK(g.check(200, 900001) == 0);  // Aperta on another address is not locked out
}

static void test_more_clients_than_slots() {
  LoginGuard g;
  for (uint32_t c = 0; c < 20; ++c) g.fail(c, c * 10);
  // The most recent clients are still tracked.
  CHECK(g.fails(19) == 1);
  CHECK(g.fails(12) == 1);
  CHECK(g.fails(0) == 0);
}

static void test_millis_wraparound() {
  LoginGuard g;
  uint32_t t = 0xFFFFFF00u;
  g.fail(5, t);
  CHECK(g.check(5, t + 500) == 500);  // wraps past zero
  CHECK(g.check(5, t + 1000) == 0);
}

int main() {
  test_wait_doubles_and_caps();
  test_each_wrong_password_waits_longer();
  test_reset_after_twice_the_wait_without_attempts();
  test_correct_password_resets();
  test_clients_are_separate();
  test_more_clients_than_slots();
  test_millis_wraparound();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
