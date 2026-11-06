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

/**
 * @brief A requested re-init (display switch) resets an accumulated backoff and returns zero (reinit_delay).
 */
TEST(ReinitDelayTest, RequestedResetsBackoffAndReturnsZero) {
  auto backoff = make_backoff();
  EXPECT_EQ(video::reinit_delay(backoff, false, false, 0ms), 100ms);
  EXPECT_EQ(video::reinit_delay(backoff, false, false, 0ms), 200ms);
  ASSERT_EQ(backoff.consecutive_failures(), 2u);

  EXPECT_EQ(video::reinit_delay(backoff, true, false, 0ms), 0ms);
  EXPECT_EQ(backoff.consecutive_failures(), 0u);
  EXPECT_EQ(video::reinit_delay(backoff, false, false, 0ms), 100ms);
}

/**
 * @brief A requested re-init returns zero even when the previous run was quick and captured no frame (reinit_delay).
 */
TEST(ReinitDelayTest, RequestedIgnoresQuickNoFrameRun) {
  auto backoff = make_backoff();
  EXPECT_EQ(video::reinit_delay(backoff, true, false, 0ms), 0ms);
  EXPECT_EQ(backoff.consecutive_failures(), 0u);
}

/**
 * @brief A frame captured since the display was opened gives no delay and resets, even for a short lifetime (reinit_delay).
 */
TEST(ReinitDelayTest, FrameSinceOpenReturnsZeroAndResets) {
  auto backoff = make_backoff();
  EXPECT_EQ(video::reinit_delay(backoff, false, false, 0ms), 100ms);
  EXPECT_EQ(video::reinit_delay(backoff, false, false, 0ms), 200ms);

  EXPECT_EQ(video::reinit_delay(backoff, false, true, 10ms), 0ms);
  EXPECT_EQ(backoff.consecutive_failures(), 0u);
  EXPECT_EQ(video::reinit_delay(backoff, false, false, 0ms), 100ms);
}

/**
 * @brief Repeated quick re-inits without a frame back off 100, 200, 400 ms ... capped at 2 s, and are counted (reinit_delay).
 */
TEST(ReinitDelayTest, QuickNoFrameReinitsGrowAndCap) {
  auto backoff = make_backoff();
  const std::chrono::milliseconds expected[] = {100ms, 200ms, 400ms, 800ms, 1600ms, 2000ms, 2000ms};
  std::uint32_t count = 0;
  for (const auto delay : expected) {
    EXPECT_EQ(video::reinit_delay(backoff, false, false, 50ms), delay);
    EXPECT_EQ(backoff.consecutive_failures(), ++count);
  }
}

/**
 * @brief A lifetime of at least the healthy run returns zero and resets, but 999 ms is still quick (reinit_delay).
 */
TEST(ReinitDelayTest, LongLifetimeReturnsZeroAndResets) {
  auto backoff = make_backoff();
  EXPECT_EQ(video::reinit_delay(backoff, false, false, 999ms), 100ms);
  EXPECT_EQ(video::reinit_delay(backoff, false, false, 999ms), 200ms);

  EXPECT_EQ(video::reinit_delay(backoff, false, false, 1s), 0ms);
  EXPECT_EQ(backoff.consecutive_failures(), 0u);
  EXPECT_EQ(video::reinit_delay(backoff, false, false, 0ms), 100ms);

  EXPECT_EQ(video::reinit_delay(backoff, false, false, 30s), 0ms);
  EXPECT_EQ(backoff.consecutive_failures(), 0u);
}
