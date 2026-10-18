/**
 * @file tests/unit/platform/linux/test_wayland_stats.cpp
 * @brief Tests for the process-wide wlr screencopy capture-object counters and the periodic log gate.
 */

#ifdef __linux__

  // standard includes
  #include <chrono>
  #include <cstdint>
  #include <string>
  #include <thread>
  #include <vector>

  // lib includes
  #include <gtest/gtest.h>

  // local includes
  #include "src/platform/linux/fd_counts.h"
  #include "src/platform/linux/wayland_stats.h"

namespace {
  /**
   * @brief Fixture that resets the process-wide counters around every test.
   */
  class WaylandStatsTest: public testing::Test {
  protected:
    /**
     * @brief Start every test from zeroed counters.
     */
    void SetUp() override {
      wl::stats::reset();
    }

    /**
     * @brief Leave zeroed counters behind for the next test.
     */
    void TearDown() override {
      wl::stats::reset();
    }
  };

  /**
   * @brief Call a counter function `times` times.
   *
   * @param fn Counter function with no arguments.
   * @param times Number of calls.
   */
  void call_n(void (*fn)(), int times) {
    for (int i = 0; i < times; ++i) {
      fn();
    }
  }
}  // namespace

/**
 * @brief Created and destroyed buffer objects move the live count up and down.
 */
TEST_F(WaylandStatsTest, BoCreatedAndDestroyedMoveLiveCount) {
  wl::stats::bo_created();
  wl::stats::bo_created();
  wl::stats::bo_created();
  EXPECT_EQ(wl::stats::snapshot().bos, 3);

  wl::stats::bo_destroyed();
  EXPECT_EQ(wl::stats::snapshot().bos, 2);
}

/**
 * @brief Destroying more buffer objects than were created never takes the live count below zero.
 */
TEST_F(WaylandStatsTest, BoDestroyedBelowZeroClampsToZero) {
  wl::stats::bo_destroyed();
  wl::stats::bo_destroyed();
  EXPECT_EQ(wl::stats::snapshot().bos, 0);

  wl::stats::bo_created();
  EXPECT_EQ(wl::stats::snapshot().bos, 1);
}

/**
 * @brief Created and destroyed `wl_buffer`s move the live count up and down.
 */
TEST_F(WaylandStatsTest, WlBufferCreatedAndDestroyedMoveLiveCount) {
  wl::stats::wl_buffer_created();
  wl::stats::wl_buffer_created();
  EXPECT_EQ(wl::stats::snapshot().wl_buffers, 2);

  wl::stats::wl_buffer_destroyed();
  EXPECT_EQ(wl::stats::snapshot().wl_buffers, 1);
}

/**
 * @brief Destroying more `wl_buffer`s than were created never takes the live count below zero.
 */
TEST_F(WaylandStatsTest, WlBufferDestroyedBelowZeroClampsToZero) {
  wl::stats::wl_buffer_destroyed();
  EXPECT_EQ(wl::stats::snapshot().wl_buffers, 0);

  wl::stats::wl_buffer_created();
  EXPECT_EQ(wl::stats::snapshot().wl_buffers, 1);
}

/**
 * @brief Pending buffer-params requests rise on send and fall on resolution.
 */
TEST_F(WaylandStatsTest, ParamsPendingAndResolvedMoveLiveCount) {
  wl::stats::params_pending();
  wl::stats::params_pending();
  EXPECT_EQ(wl::stats::snapshot().pending_params, 2);

  wl::stats::params_resolved();
  EXPECT_EQ(wl::stats::snapshot().pending_params, 1);
}

/**
 * @brief Resolving more params requests than were pending never takes the count below zero.
 */
TEST_F(WaylandStatsTest, ParamsResolvedBelowZeroClampsToZero) {
  wl::stats::params_resolved();
  wl::stats::params_resolved();
  EXPECT_EQ(wl::stats::snapshot().pending_params, 0);

  wl::stats::params_pending();
  EXPECT_EQ(wl::stats::snapshot().pending_params, 1);
}

/**
 * @brief Cancelled requests and overwritten objects only increase their totals.
 */
TEST_F(WaylandStatsTest, CancelledAndOverwrittenAreTotals) {
  wl::stats::request_cancelled();
  wl::stats::request_cancelled();
  wl::stats::object_overwritten();

  const auto counters = wl::stats::snapshot();
  EXPECT_EQ(counters.cancelled_requests, 2U);
  EXPECT_EQ(counters.overwritten_objects, 1U);
  EXPECT_EQ(counters.bos, 0);
  EXPECT_EQ(counters.wl_buffers, 0);
  EXPECT_EQ(counters.pending_params, 0);
}

/**
 * @brief A snapshot reports every counter after a mixed sequence of calls.
 */
TEST_F(WaylandStatsTest, SnapshotReflectsAllCounters) {
  call_n(wl::stats::bo_created, 2);
  wl::stats::wl_buffer_created();
  call_n(wl::stats::params_pending, 3);
  wl::stats::params_resolved();
  wl::stats::request_cancelled();
  call_n(wl::stats::object_overwritten, 2);

  const auto counters = wl::stats::snapshot();
  EXPECT_EQ(counters.bos, 2);
  EXPECT_EQ(counters.wl_buffers, 1);
  EXPECT_EQ(counters.pending_params, 2);
  EXPECT_EQ(counters.cancelled_requests, 1U);
  EXPECT_EQ(counters.overwritten_objects, 2U);
}

/**
 * @brief Reset sets every live count and every total back to zero.
 */
TEST_F(WaylandStatsTest, ResetZeroesAllCounters) {
  call_n(wl::stats::bo_created, 2);
  wl::stats::wl_buffer_created();
  wl::stats::params_pending();
  wl::stats::request_cancelled();
  wl::stats::object_overwritten();

  wl::stats::reset();

  const auto counters = wl::stats::snapshot();
  EXPECT_EQ(counters.bos, 0);
  EXPECT_EQ(counters.wl_buffers, 0);
  EXPECT_EQ(counters.pending_params, 0);
  EXPECT_EQ(counters.cancelled_requests, 0U);
  EXPECT_EQ(counters.overwritten_objects, 0U);
}

/**
 * @brief With the default cap of 8, 8 live objects is allowed and 9 is over the cap for bos.
 */
TEST_F(WaylandStatsTest, OverCapDefaultCapForBos) {
  EXPECT_EQ(wl::stats::default_live_cap, 8);

  call_n(wl::stats::bo_created, 8);
  EXPECT_FALSE(wl::stats::over_cap(wl::stats::snapshot()));

  wl::stats::bo_created();
  EXPECT_TRUE(wl::stats::over_cap(wl::stats::snapshot()));
}

/**
 * @brief With the default cap of 8, 8 live `wl_buffer`s is allowed and 9 is over the cap.
 */
TEST_F(WaylandStatsTest, OverCapDefaultCapForWlBuffers) {
  call_n(wl::stats::wl_buffer_created, 8);
  EXPECT_FALSE(wl::stats::over_cap(wl::stats::snapshot()));

  wl::stats::wl_buffer_created();
  EXPECT_TRUE(wl::stats::over_cap(wl::stats::snapshot()));
}

/**
 * @brief With the default cap of 8, 8 pending params is allowed and 9 is over the cap.
 */
TEST_F(WaylandStatsTest, OverCapDefaultCapForPendingParams) {
  call_n(wl::stats::params_pending, 8);
  EXPECT_FALSE(wl::stats::over_cap(wl::stats::snapshot()));

  wl::stats::params_pending();
  EXPECT_TRUE(wl::stats::over_cap(wl::stats::snapshot()));
}

/**
 * @brief Totals of cancelled requests and overwritten objects do not count towards the cap.
 */
TEST_F(WaylandStatsTest, OverCapIgnoresTotals) {
  wl::stats::counters_t counters;
  counters.cancelled_requests = 100;
  counters.overwritten_objects = 100;

  EXPECT_FALSE(wl::stats::over_cap(counters));
}

/**
 * @brief An explicit cap is honoured in place of the default.
 */
TEST_F(WaylandStatsTest, OverCapExplicitCap) {
  wl::stats::counters_t counters;
  counters.bos = 3;
  EXPECT_TRUE(wl::stats::over_cap(counters, 2));
  EXPECT_FALSE(wl::stats::over_cap(counters, 3));

  counters.bos = 0;
  counters.wl_buffers = 3;
  EXPECT_TRUE(wl::stats::over_cap(counters, 2));
  EXPECT_FALSE(wl::stats::over_cap(counters, 3));

  counters.wl_buffers = 0;
  counters.pending_params = 3;
  EXPECT_TRUE(wl::stats::over_cap(counters, 2));
  EXPECT_FALSE(wl::stats::over_cap(counters, 3));
}

/**
 * @brief The log line has the exact field order and layout for given counters and fd counts.
 */
TEST_F(WaylandStatsTest, FormatProducesExactString) {
  wl::stats::counters_t counters;
  counters.bos = 2;
  counters.wl_buffers = 1;
  counters.pending_params = 0;
  counters.cancelled_requests = 4;
  counters.overwritten_objects = 5;

  platf::fd_counts_t fds;
  fds.total = 40;
  fds.render_nodes = 1;
  fds.dmabufs = 3;

  EXPECT_EQ(
    wl::stats::format(counters, fds),
    "bos=2 wl_buffers=1 pending_params=0 cancelled=4 overwritten=5 dmabuf_fds=3 render_fds=1"
  );
}

/**
 * @brief Formatting zeroed counters and empty fd counts gives all zeros in the same layout.
 */
TEST_F(WaylandStatsTest, FormatZeroCounters) {
  EXPECT_EQ(
    wl::stats::format(wl::stats::counters_t {}, platf::fd_counts_t {}),
    "bos=0 wl_buffers=0 pending_params=0 cancelled=0 overwritten=0 dmabuf_fds=0 render_fds=0"
  );
}

/**
 * @brief The first call to due() always returns true.
 */
TEST(WaylandStatsPeriodicGateTest, FirstCallIsDue) {
  wl::stats::periodic_gate_t gate(std::chrono::seconds {10});
  const std::chrono::steady_clock::time_point base {std::chrono::seconds {1000}};

  EXPECT_TRUE(gate.due(base));
}

/**
 * @brief After an open, due() stays false until exactly one interval has passed, then opens again.
 */
TEST(WaylandStatsPeriodicGateTest, OpensOncePerInterval) {
  wl::stats::periodic_gate_t gate(std::chrono::seconds {10});
  const std::chrono::steady_clock::time_point base {std::chrono::seconds {1000}};

  EXPECT_TRUE(gate.due(base));
  EXPECT_FALSE(gate.due(base + std::chrono::seconds {1}));
  EXPECT_FALSE(gate.due(base + std::chrono::seconds {9}));
  EXPECT_TRUE(gate.due(base + std::chrono::seconds {10}));
  EXPECT_FALSE(gate.due(base + std::chrono::seconds {11}));
}

/**
 * @brief The interval is measured from the last call that returned true, not from calls that returned false.
 */
TEST(WaylandStatsPeriodicGateTest, MeasuresFromLastOpenResult) {
  wl::stats::periodic_gate_t gate(std::chrono::seconds {10});
  const std::chrono::steady_clock::time_point base {std::chrono::seconds {1000}};

  EXPECT_TRUE(gate.due(base));
  EXPECT_FALSE(gate.due(base + std::chrono::seconds {5}));
  EXPECT_FALSE(gate.due(base + std::chrono::seconds {15}));
  EXPECT_TRUE(gate.due(base + std::chrono::seconds {20}));
  EXPECT_FALSE(gate.due(base + std::chrono::seconds {25}));
  EXPECT_TRUE(gate.due(base + std::chrono::seconds {30}));
}

/**
 * @brief Balanced create and destroy calls from many threads leave every live count at zero.
 */
TEST_F(WaylandStatsTest, ConcurrentBalancedCallsEndAtZero) {
  constexpr int thread_count = 8;
  constexpr int iterations = 1000;

  std::vector<std::thread> threads;
  threads.reserve(thread_count);
  for (int t = 0; t < thread_count; ++t) {
    threads.emplace_back([]() {
      for (int i = 0; i < iterations; ++i) {
        wl::stats::bo_created();
        wl::stats::bo_destroyed();
        wl::stats::wl_buffer_created();
        wl::stats::wl_buffer_destroyed();
        wl::stats::params_pending();
        wl::stats::params_resolved();
        wl::stats::request_cancelled();
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  const auto counters = wl::stats::snapshot();
  EXPECT_EQ(counters.bos, 0);
  EXPECT_EQ(counters.wl_buffers, 0);
  EXPECT_EQ(counters.pending_params, 0);
  EXPECT_EQ(counters.cancelled_requests, static_cast<std::uint64_t>(thread_count) * iterations);
}
#endif
