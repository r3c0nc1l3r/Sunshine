/**
 * @file tests/unit/test_auth_rate_limit.cpp
 * @brief Tests for the per-source failed-login limiter used by the admin web UI (src/auth_rate_limit.h).
 *
 * Every test passes explicit time points, so the results do not depend on the wall clock.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// local includes
#include <src/auth_rate_limit.h>

namespace {
  using time_point_t = std::chrono::steady_clock::time_point;

  constexpr std::string_view SOURCE_A {"192.0.2.1"};  ///< Documentation address used as the first failing source.
  constexpr std::string_view SOURCE_B {"192.0.2.2"};  ///< Documentation address used as an independent source.
  constexpr std::size_t DEFAULT_MAX_FAILURES {10};  ///< Failures that start a lockout with the default limiter.
  constexpr auto DEFAULT_WINDOW {std::chrono::seconds {300}};  ///< Window and lockout length with the default limiter.

  /**
   * @brief Record several failures for one source at one time.
   *
   * @param limiter Limiter under test.
   * @param source Source address.
   * @param count Number of failures to record.
   * @param now Time of every failure.
   * @return How many of the recorded failures returned `true` (started a lockout).
   */
  std::size_t record_failures(confighttp::auth_rate_limiter_t &limiter, std::string_view source, std::size_t count, time_point_t now) {
    std::size_t started = 0;
    for (std::size_t i = 0; i < count; ++i) {
      if (limiter.on_failure(source, now)) {
        ++started;
      }
    }
    return started;
  }
}  // namespace

/**
 * @brief Test: a source with no failures is not blocked, and the limiter tracks nothing.
 */
TEST(AuthRateLimiterTest, FreshSourceIsNotBlockedAndTableIsEmpty) {
  confighttp::auth_rate_limiter_t limiter;
  const auto now = std::chrono::steady_clock::now();

  EXPECT_FALSE(limiter.blocked(SOURCE_A, now));
  EXPECT_EQ(limiter.size(), 0U);
}

/**
 * @brief Test: nine failures from one source do not start a lockout.
 */
TEST(AuthRateLimiterTest, NineFailuresDoNotStartLockout) {
  confighttp::auth_rate_limiter_t limiter;
  const auto t0 = std::chrono::steady_clock::now();

  EXPECT_EQ(record_failures(limiter, SOURCE_A, DEFAULT_MAX_FAILURES - 1, t0), 0U);
  EXPECT_FALSE(limiter.blocked(SOURCE_A, t0));
  EXPECT_EQ(limiter.size(), 1U);
}

/**
 * @brief Test: the tenth failure inside the window starts exactly one lockout, and the source is blocked.
 */
TEST(AuthRateLimiterTest, TenthFailureStartsLockoutExactlyOnce) {
  confighttp::auth_rate_limiter_t limiter;
  const auto t0 = std::chrono::steady_clock::now();

  ASSERT_EQ(record_failures(limiter, SOURCE_A, DEFAULT_MAX_FAILURES - 1, t0), 0U);
  EXPECT_TRUE(limiter.on_failure(SOURCE_A, t0)) << "The tenth failure must start the lockout";
  EXPECT_FALSE(limiter.on_failure(SOURCE_A, t0)) << "An eleventh failure must not start a second lockout";
  EXPECT_EQ(record_failures(limiter, SOURCE_A, 2, t0), 0U);
  EXPECT_TRUE(limiter.blocked(SOURCE_A, t0));
}

/**
 * @brief Test: the lockout lasts until the first failure of the window plus the window length, not the last failure.
 *
 * The first failure is at t0, the later nine are one second after it, so the lockout must end at t0 + 300 s.
 */
TEST(AuthRateLimiterTest, LockoutEndsAtFirstFailurePlusWindow) {
  confighttp::auth_rate_limiter_t limiter;
  const auto t0 = std::chrono::steady_clock::now();
  const auto later = t0 + std::chrono::seconds {1};
  const auto window_end = t0 + DEFAULT_WINDOW;

  EXPECT_FALSE(limiter.on_failure(SOURCE_A, t0));
  EXPECT_EQ(record_failures(limiter, SOURCE_A, DEFAULT_MAX_FAILURES - 1, later), 1U);

  EXPECT_TRUE(limiter.blocked(SOURCE_A, window_end - std::chrono::seconds {1}))
    << "Still locked one second before first failure plus window";
  EXPECT_FALSE(limiter.blocked(SOURCE_A, window_end))
    << "Lockout must end at first failure plus window";
}

/**
 * @brief Test: after the window has passed, old failures are forgotten and a new window starts from the next failure.
 */
TEST(AuthRateLimiterTest, WindowExpiryForgetsOldFailures) {
  confighttp::auth_rate_limiter_t limiter;
  const auto t0 = std::chrono::steady_clock::now();
  const auto later = t0 + DEFAULT_WINDOW + std::chrono::seconds {1};

  ASSERT_EQ(record_failures(limiter, SOURCE_A, DEFAULT_MAX_FAILURES - 1, t0), 0U);

  // Nine more after the old window expired: together they are eighteen, but only nine are in the new window.
  EXPECT_EQ(record_failures(limiter, SOURCE_A, DEFAULT_MAX_FAILURES - 1, later), 0U)
    << "Failures from the expired window must not count toward a lockout";
  EXPECT_FALSE(limiter.blocked(SOURCE_A, later));

  EXPECT_TRUE(limiter.on_failure(SOURCE_A, later)) << "The tenth failure of the new window must start a lockout";
}

/**
 * @brief Test: a lockout on one source does not block another source.
 */
TEST(AuthRateLimiterTest, SourcesAreIndependent) {
  confighttp::auth_rate_limiter_t limiter;
  const auto t0 = std::chrono::steady_clock::now();

  ASSERT_EQ(record_failures(limiter, SOURCE_A, DEFAULT_MAX_FAILURES, t0), 1U);

  EXPECT_TRUE(limiter.blocked(SOURCE_A, t0));
  EXPECT_FALSE(limiter.blocked(SOURCE_B, t0));
  EXPECT_EQ(record_failures(limiter, SOURCE_B, DEFAULT_MAX_FAILURES - 1, t0), 0U)
    << "The other source must start with its own count";
}

/**
 * @brief Test: after a lockout ends, a new set of failures starts a second lockout, which is also reported once.
 */
TEST(AuthRateLimiterTest, SecondLockoutStartsAfterFirstExpires) {
  confighttp::auth_rate_limiter_t limiter;
  const auto t0 = std::chrono::steady_clock::now();
  const auto after_first = t0 + DEFAULT_WINDOW;

  ASSERT_EQ(record_failures(limiter, SOURCE_A, DEFAULT_MAX_FAILURES, t0), 1U);
  ASSERT_FALSE(limiter.blocked(SOURCE_A, after_first));

  EXPECT_EQ(record_failures(limiter, SOURCE_A, DEFAULT_MAX_FAILURES, after_first), 1U)
    << "A second lockout must start exactly once after the first one expired";
  EXPECT_TRUE(limiter.blocked(SOURCE_A, after_first));
}

/**
 * @brief Test: failures that arrive while a source is locked out do not extend the lockout.
 */
TEST(AuthRateLimiterTest, FailuresWhileBlockedDoNotExtendLockout) {
  confighttp::auth_rate_limiter_t limiter;
  const auto t0 = std::chrono::steady_clock::now();

  ASSERT_EQ(record_failures(limiter, SOURCE_A, DEFAULT_MAX_FAILURES, t0), 1U);
  EXPECT_EQ(record_failures(limiter, SOURCE_A, 5, t0 + std::chrono::seconds {10}), 0U);

  EXPECT_FALSE(limiter.blocked(SOURCE_A, t0 + DEFAULT_WINDOW));
}

/**
 * @brief Test: the default limiter uses ten failures and a five minute window.
 */
TEST(AuthRateLimiterTest, DefaultsAreTenFailuresInFiveMinutes) {
  confighttp::auth_rate_limiter_t limiter;
  const auto t0 = std::chrono::steady_clock::now();

  ASSERT_EQ(record_failures(limiter, SOURCE_A, DEFAULT_MAX_FAILURES - 1, t0), 0U);
  ASSERT_TRUE(limiter.on_failure(SOURCE_A, t0));

  EXPECT_TRUE(limiter.blocked(SOURCE_A, t0 + std::chrono::seconds {299}));
  EXPECT_FALSE(limiter.blocked(SOURCE_A, t0 + std::chrono::seconds {300}));
}

/**
 * @brief Test: with two allowed sources, a third source evicts the one whose first failure is oldest.
 *
 * With max_failures = 2, if `a` were still tracked its second failure would start a lockout. It must not.
 */
TEST(AuthRateLimiterTest, EvictsSourceWithOldestFirstFailureWhenFull) {
  confighttp::auth_rate_limiter_t limiter(std::size_t {2}, std::chrono::seconds {300}, std::size_t {2});
  const auto t0 = std::chrono::steady_clock::now();

  EXPECT_FALSE(limiter.on_failure("192.0.2.11", t0));
  EXPECT_FALSE(limiter.on_failure("192.0.2.12", t0 + std::chrono::seconds {1}));
  EXPECT_FALSE(limiter.on_failure("192.0.2.13", t0 + std::chrono::seconds {2}));
  EXPECT_EQ(limiter.size(), 2U);

  EXPECT_FALSE(limiter.on_failure("192.0.2.11", t0 + std::chrono::seconds {3}))
    << "The evicted source must start again with a count of one";
}

/**
 * @brief Test: the number of tracked sources never exceeds the limit given to the constructor.
 */
TEST(AuthRateLimiterTest, SizeIsBoundedByMaxSources) {
  confighttp::auth_rate_limiter_t limiter(std::size_t {10}, std::chrono::seconds {300}, std::size_t {4});
  const auto t0 = std::chrono::steady_clock::now();

  for (int i = 0; i < 20; ++i) {
    const std::string source = "192.0.2." + std::to_string(i + 1);
    (void) limiter.on_failure(source, t0 + std::chrono::milliseconds {i});
    EXPECT_LE(limiter.size(), 4U);
  }
  EXPECT_EQ(limiter.size(), 4U);
}

/**
 * @brief Test: the default limiter holds 1024 sources and evicts when a 1025th appears.
 */
TEST(AuthRateLimiterTest, DefaultMaxSourcesIs1024) {
  confighttp::auth_rate_limiter_t limiter;
  const auto t0 = std::chrono::steady_clock::now();

  for (int i = 0; i < 1025; ++i) {
    const std::string source = "src-" + std::to_string(i);
    (void) limiter.on_failure(source, t0);
  }
  EXPECT_EQ(limiter.size(), 1024U);
}

/**
 * @brief Test: concurrent failures on one source start exactly one lockout across all threads.
 */
TEST(AuthRateLimiterTest, ConcurrentFailuresOnOneSourceStartOneLockout) {
  confighttp::auth_rate_limiter_t limiter;
  const auto now = std::chrono::steady_clock::now();
  std::atomic<std::size_t> started {0};

  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&limiter, &started, now]() {
      started += record_failures(limiter, SOURCE_A, DEFAULT_MAX_FAILURES, now);
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  EXPECT_EQ(started.load(), 1U);
  EXPECT_TRUE(limiter.blocked(SOURCE_A, now));
}

/**
 * @brief Test: concurrent failures on distinct sources are all tracked, with no lost entries.
 */
TEST(AuthRateLimiterTest, ConcurrentDistinctSourcesAreAllTracked) {
  confighttp::auth_rate_limiter_t limiter;
  const auto now = std::chrono::steady_clock::now();

  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&limiter, now, t]() {
      for (int j = 0; j < 100; ++j) {
        const std::string source = "t" + std::to_string(t) + "-" + std::to_string(j);
        (void) limiter.on_failure(source, now);
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  EXPECT_EQ(limiter.size(), 800U);
}

/**
 * @brief Test: with the table full, a flood of new sources never evicts a locked-out source while unlocked entries exist.
 *
 * The locked source is the oldest entry by far, so a plain oldest-first rule would evict it first.
 */
TEST(AuthRateLimiterTest, FloodOfNewSourcesNeverEvictsLockedSource) {
  confighttp::auth_rate_limiter_t limiter(std::size_t {2}, DEFAULT_WINDOW, std::size_t {3});
  const auto t0 = std::chrono::steady_clock::now();

  ASSERT_EQ(record_failures(limiter, SOURCE_A, 2, t0), 1U);

  for (int i = 0; i < 50; ++i) {
    const std::string source = "flood-" + std::to_string(i);
    (void) limiter.on_failure(source, t0 + std::chrono::seconds {1} + std::chrono::milliseconds {i});
    EXPECT_LE(limiter.size(), 3U);
  }

  EXPECT_TRUE(limiter.blocked(SOURCE_A, t0 + std::chrono::seconds {10}))
    << "A locked source must survive a flood of new sources while unlocked entries exist";
  EXPECT_TRUE(limiter.blocked(SOURCE_A, t0 + std::chrono::seconds {299}));
  EXPECT_EQ(limiter.size(), 3U);
}

/**
 * @brief Test: when the table is full, an entry whose window has ended is dropped before any live entry.
 *
 * The table holds an expired unlocked source and a live unlocked source. Adding a third source must drop the expired one,
 * so the live source keeps its count: its next failures still reach the threshold on the second failure after the insert.
 */
TEST(AuthRateLimiterTest, ExpiredEntriesAreDroppedBeforeLiveEntries) {
  confighttp::auth_rate_limiter_t limiter(std::size_t {3}, DEFAULT_WINDOW, std::size_t {2});
  const auto t0 = std::chrono::steady_clock::now();
  const auto insert_time = t0 + DEFAULT_WINDOW;

  EXPECT_FALSE(limiter.on_failure("192.0.2.31", t0)) << "Expired at insert time";
  EXPECT_FALSE(limiter.on_failure(SOURCE_B, t0 + std::chrono::seconds {100})) << "Still live at insert time";
  ASSERT_EQ(limiter.size(), 2U);

  EXPECT_FALSE(limiter.on_failure("192.0.2.33", insert_time));
  EXPECT_EQ(limiter.size(), 2U);

  EXPECT_FALSE(limiter.on_failure(SOURCE_B, insert_time)) << "The live source must still hold its first failure (count two)";
  EXPECT_TRUE(limiter.on_failure(SOURCE_B, insert_time))
    << "The live source must still hold its earlier failures, so its third one starts a lockout";
}

/**
 * @brief Test: when every tracked source is locked out, the one with the oldest first failure is evicted and size stays at the limit.
 */
TEST(AuthRateLimiterTest, FullTableOfLockedSourcesEvictsOldestLockedAndKeepsSize) {
  confighttp::auth_rate_limiter_t limiter(std::size_t {2}, DEFAULT_WINDOW, std::size_t {2});
  const auto t0 = std::chrono::steady_clock::now();
  const std::string_view source_c {"192.0.2.3"};
  const std::string_view source_d {"192.0.2.4"};

  ASSERT_EQ(record_failures(limiter, SOURCE_A, 2, t0), 1U);
  ASSERT_EQ(record_failures(limiter, SOURCE_B, 2, t0 + std::chrono::seconds {1}), 1U);
  ASSERT_EQ(limiter.size(), 2U);

  EXPECT_FALSE(limiter.on_failure(source_c, t0 + std::chrono::seconds {2}));
  EXPECT_EQ(limiter.size(), 2U) << "Adding a source to a full table must keep the size at the limit";
  EXPECT_FALSE(limiter.blocked(SOURCE_A, t0 + std::chrono::seconds {2})) << "The oldest locked source is the one evicted";
  EXPECT_TRUE(limiter.blocked(SOURCE_B, t0 + std::chrono::seconds {2}));

  EXPECT_FALSE(limiter.on_failure(source_d, t0 + std::chrono::seconds {3}));
  EXPECT_EQ(limiter.size(), 2U);
  EXPECT_TRUE(limiter.blocked(SOURCE_B, t0 + std::chrono::seconds {3})) << "An unlocked source is evicted before a locked one";
}

/**
 * @brief Test: retry_after is nullopt for a source the limiter has never seen.
 */
TEST(AuthRateLimiterTest, RetryAfterIsNulloptForUnknownSource) {
  confighttp::auth_rate_limiter_t limiter;
  const auto now = std::chrono::steady_clock::now();

  EXPECT_FALSE(limiter.retry_after(SOURCE_A, now).has_value());
}

/**
 * @brief Test: retry_after is nullopt for a source with failures but no lockout.
 */
TEST(AuthRateLimiterTest, RetryAfterIsNulloptWithoutLockout) {
  confighttp::auth_rate_limiter_t limiter;
  const auto t0 = std::chrono::steady_clock::now();

  ASSERT_EQ(record_failures(limiter, SOURCE_A, DEFAULT_MAX_FAILURES - 1, t0), 0U);
  EXPECT_FALSE(limiter.retry_after(SOURCE_A, t0).has_value());
}

/**
 * @brief Test: at the exact lockout start, retry_after reports the full window (300 seconds).
 */
TEST(AuthRateLimiterTest, RetryAfterAtLockoutStartIsFullWindow) {
  confighttp::auth_rate_limiter_t limiter;
  const auto t0 = std::chrono::steady_clock::now();

  ASSERT_EQ(record_failures(limiter, SOURCE_A, DEFAULT_MAX_FAILURES, t0), 1U);

  const auto retry = limiter.retry_after(SOURCE_A, t0);
  ASSERT_TRUE(retry.has_value());
  EXPECT_EQ(retry.value_or(std::chrono::seconds {0}), std::chrono::seconds {300});
}

/**
 * @brief Test: retry_after rounds the remaining lockout up to whole seconds, with a minimum of one second.
 *
 * Half a second left gives 1, one millisecond left gives 1, and 299.2 seconds left gives 300.
 */
TEST(AuthRateLimiterTest, RetryAfterRoundsRemainingTimeUp) {
  confighttp::auth_rate_limiter_t limiter;
  const auto t0 = std::chrono::steady_clock::now();

  ASSERT_EQ(record_failures(limiter, SOURCE_A, DEFAULT_MAX_FAILURES, t0), 1U);

  EXPECT_EQ(limiter.retry_after(SOURCE_A, t0 + std::chrono::milliseconds {299500}).value_or(std::chrono::seconds {0}), std::chrono::seconds {1})
    << "0.5 s left must round up to 1";
  EXPECT_EQ(limiter.retry_after(SOURCE_A, t0 + std::chrono::milliseconds {299999}).value_or(std::chrono::seconds {0}), std::chrono::seconds {1})
    << "1 ms left must round up to 1";
  EXPECT_EQ(limiter.retry_after(SOURCE_A, t0 + std::chrono::milliseconds {800}).value_or(std::chrono::seconds {0}), std::chrono::seconds {300})
    << "299.2 s left must round up to 300";
}

/**
 * @brief Test: retry_after is nullopt from the moment the lockout ends (first failure plus window) onward.
 */
TEST(AuthRateLimiterTest, RetryAfterIsNulloptAtAndAfterLockoutEnd) {
  confighttp::auth_rate_limiter_t limiter;
  const auto t0 = std::chrono::steady_clock::now();

  ASSERT_EQ(record_failures(limiter, SOURCE_A, DEFAULT_MAX_FAILURES, t0), 1U);

  EXPECT_FALSE(limiter.retry_after(SOURCE_A, t0 + DEFAULT_WINDOW).has_value())
    << "Exactly at first failure plus window the lockout has ended";
  EXPECT_FALSE(limiter.retry_after(SOURCE_A, t0 + DEFAULT_WINDOW + std::chrono::seconds {1}).has_value());
}
