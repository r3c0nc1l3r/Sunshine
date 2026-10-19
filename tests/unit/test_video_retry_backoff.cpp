/**
 * @file tests/unit/test_video_retry_backoff.cpp
 * @brief Tests for the encode-loop retry backoff (src/video_retry_backoff.h).
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <cstdint>

// local includes
#include <src/video_retry_backoff.h>

using namespace std::chrono_literals;

namespace {
  /**
   * @brief Backoff configured as in the encode loop: 100 ms initial, 2 s max, 1 s healthy run.
   * @return A new backoff instance.
   */
  video::retry_backoff_t make_backoff() {
    return video::retry_backoff_t {100ms, 2s, 1s};
  }
}  // namespace

/**
 * @brief A successful run returns no delay and resets the failure count (S3 retry_backoff).
 */
TEST(RetryBackoffTest, SuccessReturnsZeroAndResets) {
  auto backoff = make_backoff();
  EXPECT_EQ(backoff.on_run_end(0ms, true), 100ms);
  EXPECT_EQ(backoff.on_run_end(0ms, true), 200ms);
  EXPECT_EQ(backoff.on_run_end(0ms, false), 0ms);
  EXPECT_EQ(backoff.consecutive_failures(), 0u);
  EXPECT_EQ(backoff.on_run_end(0ms, true), 100ms);
}

/**
 * @brief A successful run that is also long returns no delay (S3 retry_backoff).
 */
TEST(RetryBackoffTest, LongSuccessReturnsZero) {
  auto backoff = make_backoff();
  EXPECT_EQ(backoff.on_run_end(10s, false), 0ms);
  EXPECT_EQ(backoff.consecutive_failures(), 0u);
}

/**
 * @brief Quick failures double the delay from 100 ms and cap it at 2 s (S3 retry_backoff).
 */
TEST(RetryBackoffTest, QuickFailuresGrowExponentiallyAndCap) {
  auto backoff = make_backoff();
  EXPECT_EQ(backoff.on_run_end(0ms, true), 100ms);
  EXPECT_EQ(backoff.on_run_end(0ms, true), 200ms);
  EXPECT_EQ(backoff.on_run_end(0ms, true), 400ms);
  EXPECT_EQ(backoff.on_run_end(0ms, true), 800ms);
  EXPECT_EQ(backoff.on_run_end(0ms, true), 1600ms);
  EXPECT_EQ(backoff.on_run_end(0ms, true), 2000ms);
  EXPECT_EQ(backoff.on_run_end(0ms, true), 2000ms);
}

/**
 * @brief A failure whose run lasted at least the healthy run returns no delay and resets (S3 retry_backoff).
 */
TEST(RetryBackoffTest, HealthyRunFailureReturnsZeroAndResets) {
  auto backoff = make_backoff();
  EXPECT_EQ(backoff.on_run_end(0ms, true), 100ms);
  EXPECT_EQ(backoff.on_run_end(0ms, true), 200ms);
  EXPECT_EQ(backoff.on_run_end(5s, true), 0ms);
  EXPECT_EQ(backoff.consecutive_failures(), 0u);
  EXPECT_EQ(backoff.on_run_end(0ms, true), 100ms);
}

/**
 * @brief A run of exactly the healthy run length counts as healthy (S3 retry_backoff, `d >= healthy_run`).
 */
TEST(RetryBackoffTest, BoundaryEqualToHealthyRunCountsAsHealthy) {
  auto backoff = make_backoff();
  EXPECT_EQ(backoff.on_run_end(1s, true), 0ms);
  EXPECT_EQ(backoff.consecutive_failures(), 0u);
}

/**
 * @brief A run one millisecond shorter than the healthy run is still a quick failure (S3 retry_backoff).
 */
TEST(RetryBackoffTest, BoundaryJustBelowHealthyRunIsQuick) {
  auto backoff = make_backoff();
  EXPECT_EQ(backoff.on_run_end(999ms, true), 100ms);
  EXPECT_EQ(backoff.consecutive_failures(), 1u);
}

/**
 * @brief consecutive_failures() counts quick failures and drops to zero after a success (S3 retry_backoff).
 */
TEST(RetryBackoffTest, ConsecutiveFailuresCounts) {
  auto backoff = make_backoff();
  EXPECT_EQ(backoff.consecutive_failures(), 0u);
  backoff.on_run_end(0ms, true);
  EXPECT_EQ(backoff.consecutive_failures(), 1u);
  backoff.on_run_end(0ms, true);
  backoff.on_run_end(0ms, true);
  EXPECT_EQ(backoff.consecutive_failures(), 3u);
  backoff.on_run_end(0ms, false);
  EXPECT_EQ(backoff.consecutive_failures(), 0u);
}

/**
 * @brief reset() forgets earlier failures so the next quick failure starts at the initial delay (S3 retry_backoff).
 */
TEST(RetryBackoffTest, ResetForgetsFailures) {
  auto backoff = make_backoff();
  backoff.on_run_end(0ms, true);
  backoff.on_run_end(0ms, true);
  backoff.on_run_end(0ms, true);
  ASSERT_EQ(backoff.consecutive_failures(), 3u);

  backoff.reset();
  EXPECT_EQ(backoff.consecutive_failures(), 0u);
  EXPECT_EQ(backoff.on_run_end(0ms, true), 100ms);
}
