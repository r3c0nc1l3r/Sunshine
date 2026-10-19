/**
 * @file tests/unit/handoff/test_ping_monitor.cpp
 * @brief Blind tests for the PING/PONG miss counter (src/handoff/ping_monitor.*), written from the spec and header.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <cstdint>

  // local includes
  #include "src/handoff/ping_monitor.h"

/**
 * @brief Sequence numbers start at 1 and increase by one per PING.
 */
TEST(HandoffPingMonitorTest, SequenceNumbersStartAtOneAndIncrease) {
  handoff::ping_monitor_t monitor;
  EXPECT_EQ(monitor.next_ping(), 1U) << "The first PING must carry sequence number 1.";
  EXPECT_EQ(monitor.next_ping(), 2U) << "The second PING must carry sequence number 2.";
  EXPECT_EQ(monitor.next_ping(), 3U) << "The third PING must carry sequence number 3.";
}

/**
 * @brief A new monitor has no misses and is not hung.
 */
TEST(HandoffPingMonitorTest, NewMonitorHasNoMissesAndIsNotHung) {
  handoff::ping_monitor_t monitor;
  EXPECT_EQ(monitor.misses(), 0) << "A new monitor must start with zero misses.";
  EXPECT_FALSE(monitor.hung()) << "A new monitor must not be hung.";
}

/**
 * @brief The first PING has no previous PING, so it is not a miss.
 */
TEST(HandoffPingMonitorTest, FirstPingIsNotAMiss) {
  handoff::ping_monitor_t monitor;
  monitor.next_ping();
  EXPECT_EQ(monitor.misses(), 0) << "Sending the first PING must not count a miss.";
}

/**
 * @brief Each PING sent without a PONG counts one miss, detected when the next PING is due.
 */
TEST(HandoffPingMonitorTest, UnansweredPingCountsAMissWhenNextPingIsDue) {
  handoff::ping_monitor_t monitor;
  monitor.next_ping();  // seq 1, no PONG
  monitor.next_ping();  // seq 2 is due; seq 1 was missed
  EXPECT_EQ(monitor.misses(), 1) << "One unanswered PING must count one miss once the next PING is sent.";
  monitor.next_ping();  // seq 3 is due; seq 2 was missed
  EXPECT_EQ(monitor.misses(), 2) << "Two unanswered PINGs must count two misses.";
}

/**
 * @brief Four misses are not hung yet.
 */
TEST(HandoffPingMonitorTest, FourMissesAreNotHung) {
  handoff::ping_monitor_t monitor;
  for (int i = 0; i < 5; ++i) {
    monitor.next_ping();  // the fifth call records the fourth miss
  }
  EXPECT_EQ(monitor.misses(), 4) << "Five PINGs without PONG must show four misses at that point.";
  EXPECT_FALSE(monitor.hung()) << "Four misses must not count as a hung new process.";
}

/**
 * @brief Five misses in a row mean a hung new process.
 */
TEST(HandoffPingMonitorTest, FiveMissesMeanHung) {
  handoff::ping_monitor_t monitor;
  for (int i = 0; i < 6; ++i) {
    monitor.next_ping();  // the sixth call records the fifth miss
  }
  EXPECT_EQ(monitor.misses(), 5) << "Six PINGs without PONG must show five misses.";
  EXPECT_TRUE(monitor.hung()) << "Five consecutive misses must mean the new process is hung.";
}

/**
 * @brief A PONG for the latest PING resets the miss count to zero.
 */
TEST(HandoffPingMonitorTest, PongForLatestPingResetsMisses) {
  handoff::ping_monitor_t monitor;
  monitor.next_ping();  // 1
  monitor.next_ping();  // 2; 1 missed
  EXPECT_EQ(monitor.misses(), 1) << "Precondition: one miss before the PONG.";
  monitor.on_pong(2);
  EXPECT_EQ(monitor.misses(), 0) << "A PONG for the latest PING must reset the miss count to 0.";
  EXPECT_FALSE(monitor.hung()) << "After a reset the monitor must not be hung.";
}

/**
 * @brief Answered PINGs never accumulate misses, however many there are.
 */
TEST(HandoffPingMonitorTest, AnsweredPingsNeverAccumulateMisses) {
  handoff::ping_monitor_t monitor;
  for (int i = 0; i < 20; ++i) {
    const auto seq = monitor.next_ping();
    monitor.on_pong(seq);
    EXPECT_EQ(monitor.misses(), 0) << "Round " << i << ": a PING answered before the next one must add no miss.";
    EXPECT_FALSE(monitor.hung()) << "Round " << i << ": answered PINGs must never mean hung.";
  }
}

/**
 * @brief A PONG that arrives after the process was counted hung clears the hung state.
 */
TEST(HandoffPingMonitorTest, PongAfterHungClearsHungState) {
  handoff::ping_monitor_t monitor;
  for (int i = 0; i < 6; ++i) {
    monitor.next_ping();
  }
  ASSERT_TRUE(monitor.hung()) << "Precondition: five misses must be hung.";
  monitor.on_pong(6);
  EXPECT_FALSE(monitor.hung()) << "A PONG for the latest PING must clear the hung state.";
  EXPECT_EQ(monitor.misses(), 0) << "A PONG for the latest PING must reset the miss count.";
}

/**
 * @brief A late PONG for an earlier PING resets the miss count, even though the latest PING is still unanswered.
 */
TEST(HandoffPingMonitorTest, LatePongForEarlierPingResetsMissCount) {
  handoff::ping_monitor_t monitor;
  monitor.next_ping();  // 1
  monitor.next_ping();  // 2; 1 missed
  monitor.next_ping();  // 3; 2 missed
  ASSERT_EQ(monitor.misses(), 2) << "Precondition: two misses before the late PONG.";
  monitor.on_pong(1);
  EXPECT_EQ(monitor.misses(), 0) << "A late PONG for an earlier, sent PING must reset the miss count to 0.";
}

/**
 * @brief After a late PONG for an earlier PING, the still-unanswered latest PING counts as a miss at the next PING.
 */
TEST(HandoffPingMonitorTest, LatestPingStaysUnansweredAfterLatePongForEarlierPing) {
  handoff::ping_monitor_t monitor;
  monitor.next_ping();  // 1
  monitor.next_ping();  // 2
  monitor.next_ping();  // 3
  monitor.on_pong(1);
  monitor.next_ping();  // 4: PING 3 has still had no PONG
  EXPECT_EQ(monitor.misses(), 1)
    << "A late PONG for seq 1 must not mark seq 3 answered; at the next PING seq 3 counts as one miss.";
}

/**
 * @brief A late PONG never makes the process hung on its own.
 */
TEST(HandoffPingMonitorTest, LatePongNeverCausesHung) {
  handoff::ping_monitor_t monitor;
  for (int i = 0; i < 4; ++i) {
    monitor.next_ping();
  }
  monitor.on_pong(1);
  EXPECT_FALSE(monitor.hung()) << "A PONG, late or not, must never make the monitor report hung.";
}

/**
 * @brief A sequence number that was never sent (0) is ignored.
 */
TEST(HandoffPingMonitorTest, PongWithSequenceZeroIsIgnored) {
  handoff::ping_monitor_t monitor;
  monitor.on_pong(0);
  EXPECT_EQ(monitor.misses(), 0) << "A PONG for seq 0 before any PING must leave the miss count at 0.";

  monitor.next_ping();  // 1
  monitor.next_ping();  // 2; 1 missed
  monitor.on_pong(0);
  EXPECT_EQ(monitor.misses(), 1) << "A PONG for seq 0 must not reset the miss count.";
}

/**
 * @brief A sequence number above the latest sent PING is ignored and does not answer the latest PING.
 */
TEST(HandoffPingMonitorTest, PongAboveLatestSentIsIgnored) {
  handoff::ping_monitor_t monitor;
  monitor.next_ping();  // 1
  monitor.next_ping();  // 2; 1 missed
  monitor.next_ping();  // 3; 2 missed
  monitor.on_pong(4);
  EXPECT_EQ(monitor.misses(), 2) << "A PONG for seq 4, never sent, must not reset the miss count.";

  monitor.next_ping();  // 4 is now sent; 3 was never answered
  EXPECT_EQ(monitor.misses(), 3)
    << "A PONG for a never-sent sequence number must not mark the latest PING answered.";
}

/**
 * @brief A very large sequence number that was never sent is ignored.
 */
TEST(HandoffPingMonitorTest, PongWithHugeUnsentSequenceIsIgnored) {
  handoff::ping_monitor_t monitor;
  monitor.next_ping();  // 1
  monitor.next_ping();  // 2; 1 missed
  monitor.on_pong(UINT64_MAX);
  EXPECT_EQ(monitor.misses(), 1) << "A PONG for UINT64_MAX (never sent) must leave the miss count unchanged.";
}

/**
 * @brief A repeated PONG for the same sequence number is harmless.
 */
TEST(HandoffPingMonitorTest, DuplicatePongIsHarmless) {
  handoff::ping_monitor_t monitor;
  monitor.next_ping();
  monitor.on_pong(1);
  monitor.on_pong(1);
  monitor.next_ping();  // 2 sent; 1 was answered
  EXPECT_EQ(monitor.misses(), 0) << "A duplicate PONG for an answered PING must not add misses or otherwise break counting.";
}

/**
 * @brief The threshold is configurable: two misses with a threshold of two mean hung.
 */
TEST(HandoffPingMonitorTest, CustomThresholdTwoMeansHungAtTwoMisses) {
  handoff::ping_monitor_t monitor(2);
  monitor.next_ping();
  monitor.next_ping();  // one miss
  EXPECT_FALSE(monitor.hung()) << "With threshold 2, one miss must not be hung.";
  monitor.next_ping();  // two misses
  EXPECT_EQ(monitor.misses(), 2) << "Three PINGs without PONG must show two misses.";
  EXPECT_TRUE(monitor.hung()) << "With threshold 2, two misses must be hung.";
}

/**
 * @brief The threshold is configurable upward: four misses are not hung with a threshold of ten.
 */
TEST(HandoffPingMonitorTest, CustomThresholdTenNotHungAtFiveMisses) {
  handoff::ping_monitor_t monitor(10);
  for (int i = 0; i < 6; ++i) {
    monitor.next_ping();
  }
  EXPECT_EQ(monitor.misses(), 5) << "Precondition: five misses.";
  EXPECT_FALSE(monitor.hung()) << "With threshold 10, five misses must not be hung.";
}

/**
 * @brief A threshold of 0 counts as 1: a single miss means hung.
 */
TEST(HandoffPingMonitorTest, ThresholdZeroCountsAsOne) {
  handoff::ping_monitor_t monitor(0);
  monitor.next_ping();
  EXPECT_FALSE(monitor.hung()) << "Before any miss a threshold-0 monitor must not be hung.";
  monitor.next_ping();  // one miss
  EXPECT_TRUE(monitor.hung()) << "A threshold below 1 must count as 1, so one miss means hung.";
}

/**
 * @brief A negative threshold counts as 1 as well.
 */
TEST(HandoffPingMonitorTest, NegativeThresholdCountsAsOne) {
  handoff::ping_monitor_t monitor(-4);
  monitor.next_ping();
  EXPECT_FALSE(monitor.hung()) << "Before any miss a negative-threshold monitor must not be hung.";
  monitor.next_ping();  // one miss
  EXPECT_TRUE(monitor.hung()) << "A negative threshold must count as 1, so one miss means hung.";
}

/**
 * @brief The default constructor uses a threshold of five.
 */
TEST(HandoffPingMonitorTest, DefaultThresholdIsFive) {
  handoff::ping_monitor_t monitor;
  for (int i = 0; i < 5; ++i) {
    monitor.next_ping();  // four misses
  }
  EXPECT_FALSE(monitor.hung()) << "The default threshold must not trigger at four misses.";
  monitor.next_ping();  // five misses
  EXPECT_TRUE(monitor.hung()) << "The default threshold must trigger at five misses.";
}
#endif
