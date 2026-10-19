/**
 * @file tests/unit/handoff/test_mdns_holder.cpp
 * @brief Blind tests for the mDNS holder (src/handoff/mdns_holder.*), written from the header contract. Fake platform
 *        calls and a fake scheduler are injected, so no test waits for real time.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <chrono>
  #include <functional>
  #include <memory>
  #include <string>
  #include <thread>
  #include <utility>
  #include <vector>

  // local includes
  #include "src/handoff/mdns_holder.h"

namespace {
  using namespace std::chrono_literals;

  /**
   * @brief State shared by the fake platform calls (they are plain function pointers).
   */
  struct fake_t {
    std::vector<std::string> events;  ///< "publish", "destroy", "wait:<ms>".
    std::vector<std::pair<std::function<void()>, std::chrono::milliseconds>> tasks;  ///< Scheduled tasks.
    int publish_calls = 0;  ///< Calls of publish.
    int null_publishes = 0;  ///< The next this-many publish calls return null.
    bool renamed = false;  ///< Value returned by renamed().
    int renamed_calls = 0;  ///< Calls of renamed().
  };

  fake_t g_fake;  ///< Fake state; the tests run one at a time.

  /**
   * @brief A publisher whose destruction is recorded.
   */
  class fake_publisher_t: public platf::deinit_t {
  public:
    ~fake_publisher_t() override {
      g_fake.events.emplace_back("destroy");
    }
  };

  std::unique_ptr<platf::deinit_t> fake_publish() {
    ++g_fake.publish_calls;
    if (g_fake.null_publishes > 0) {
      --g_fake.null_publishes;
      return nullptr;
    }
    g_fake.events.emplace_back("publish");
    return std::make_unique<fake_publisher_t>();
  }

  bool fake_renamed() {
    ++g_fake.renamed_calls;
    return g_fake.renamed;
  }

  void fake_wait(std::chrono::milliseconds d) {
    g_fake.events.push_back("wait:" + std::to_string(d.count()));
  }

  void fake_schedule(std::function<void()> task, std::chrono::milliseconds delay) {
    g_fake.tasks.emplace_back(std::move(task), delay);
  }

  class HandoffMdnsHolderTest: public ::testing::Test {
  protected:
    void SetUp() override {
      g_fake = fake_t {};
      ops_.publish = &fake_publish;
      ops_.renamed = &fake_renamed;
      ops_.wait = &fake_wait;
      ops_.schedule = &fake_schedule;
    }

    void TearDown() override {
      holder_.reset();
      g_fake = fake_t {};
    }

    void make(const handoff::mdns_timing_t &timing = {}) {
      holder_ = std::make_unique<handoff::mdns_holder_t>(ops_, timing);
    }

    static int count(const std::string &what) {
      int n = 0;
      for (const auto &e : g_fake.events) {
        if (e == what) {
          ++n;
        }
      }
      return n;
    }

    handoff::mdns_ops_t ops_;
    std::unique_ptr<handoff::mdns_holder_t> holder_;
  };
}  // namespace

TEST_F(HandoffMdnsHolderTest, NothingPublishedBeforeStart) {
  make();
  EXPECT_FALSE(holder_->published()) << "a new holder must not be published";
  EXPECT_EQ(g_fake.publish_calls, 0) << "the constructor must not publish";
  EXPECT_TRUE(g_fake.tasks.empty()) << "the constructor must not schedule anything";
  EXPECT_EQ(holder_->rename_rebuilds(), 0u);
}

TEST_F(HandoffMdnsHolderTest, StartPublishesOnce) {
  make();
  EXPECT_TRUE(holder_->start()) << "start must return true when a publisher is alive after the call";
  EXPECT_TRUE(holder_->published());
  EXPECT_EQ(g_fake.publish_calls, 1);
  EXPECT_TRUE(g_fake.tasks.empty()) << "start(false) must not arm rename checks";
}

TEST_F(HandoffMdnsHolderTest, SecondStartDoesNotCreateAnotherPublisher) {
  make();
  ASSERT_TRUE(holder_->start());
  EXPECT_TRUE(holder_->start()) << "start with a live publisher returns true";
  EXPECT_EQ(g_fake.publish_calls, 1) << "start with a live publisher must not call publish again";
}

TEST_F(HandoffMdnsHolderTest, StartReturnsFalseWhenPublishIsUnavailableAndCanRetry) {
  make();
  g_fake.null_publishes = 1;
  EXPECT_FALSE(holder_->start(true)) << "a null publisher means start returns false";
  EXPECT_FALSE(holder_->published());
  EXPECT_TRUE(g_fake.tasks.empty()) << "no rename checks are armed when nothing was published";
  EXPECT_TRUE(holder_->start()) << "a later start must try again";
  EXPECT_TRUE(holder_->published());
  EXPECT_EQ(g_fake.publish_calls, 2);
}

TEST_F(HandoffMdnsHolderTest, WithdrawOrderIsDestroyThenWait) {
  make();
  ASSERT_TRUE(holder_->start());
  holder_->withdraw(5000ms);
  ASSERT_EQ(g_fake.events.size(), 3u) << "expected publish, destroy, wait";
  EXPECT_EQ(g_fake.events[0], "publish");
  EXPECT_EQ(g_fake.events[1], "destroy") << "the publisher must be destroyed before the settle wait";
  EXPECT_EQ(g_fake.events[2], "wait:1000") << "the default settle caps a 5 s timeout at 1 s";
  EXPECT_FALSE(holder_->published());
}

TEST_F(HandoffMdnsHolderTest, WaitIsMinOfTimeoutAndSettle) {
  handoff::mdns_timing_t timing;
  timing.settle = 800ms;
  make(timing);
  ASSERT_TRUE(holder_->start());
  holder_->withdraw(300ms);
  EXPECT_EQ(count("wait:300"), 1) << "a timeout below the settle cap must be used as is";
  ASSERT_TRUE(holder_->start());
  holder_->withdraw(9000ms);
  EXPECT_EQ(count("wait:800"), 1) << "a timeout above the settle cap must be cut to the settle value";
}

TEST_F(HandoffMdnsHolderTest, WaitHappensOnceOnly) {
  make();
  ASSERT_TRUE(holder_->start());
  holder_->withdraw(5000ms);
  holder_->withdraw(5000ms);
  EXPECT_EQ(count("destroy"), 1);
  EXPECT_EQ(count("wait:1000"), 1) << "withdraw is idempotent: the second one neither destroys nor waits";
}

TEST_F(HandoffMdnsHolderTest, WithdrawWithoutPublisherReturnsAtOnceWithoutWaiting) {
  make();
  holder_->withdraw(5000ms);
  EXPECT_TRUE(g_fake.events.empty()) << "withdraw with no publisher must not wait or destroy anything";
}

TEST_F(HandoffMdnsHolderTest, RepublishAfterWithdrawOrdersWithdrawWaitPublish) {
  make();
  ASSERT_TRUE(holder_->start());
  holder_->withdraw(5000ms);
  ASSERT_TRUE(holder_->start()) << "start after withdraw is the republish";
  const std::vector<std::string> expected {"publish", "destroy", "wait:1000", "publish"};
  EXPECT_EQ(g_fake.events, expected) << "order must be withdraw (destroy), wait, republish";
  EXPECT_TRUE(holder_->published());
}

TEST_F(HandoffMdnsHolderTest, StartWithRenameChecksSchedulesTwoChecksAtConfiguredDelays) {
  handoff::mdns_timing_t timing;
  timing.rename_first = 30ms;
  timing.rename_second = 70ms;
  make(timing);
  ASSERT_TRUE(holder_->start(true));
  ASSERT_EQ(g_fake.tasks.size(), 2u) << "start(true) must schedule exactly two checks";
  EXPECT_EQ(g_fake.tasks[0].second, 30ms);
  EXPECT_EQ(g_fake.tasks[1].second, 70ms);
}

TEST_F(HandoffMdnsHolderTest, DefaultRenameDelaysAreThreeAndTenSeconds) {
  make();
  ASSERT_TRUE(holder_->start(true));
  ASSERT_EQ(g_fake.tasks.size(), 2u);
  EXPECT_EQ(g_fake.tasks[0].second, 3000ms) << "first rename check at +3 s";
  EXPECT_EQ(g_fake.tasks[1].second, 10000ms) << "second rename check at +10 s";
}

TEST_F(HandoffMdnsHolderTest, ScheduledTasksDoNotRunInsideStart) {
  make();
  g_fake.renamed = true;
  ASSERT_TRUE(holder_->start(true));
  EXPECT_EQ(g_fake.publish_calls, 1) << "no check may run before the scheduler runs it";
  EXPECT_EQ(g_fake.renamed_calls, 0);
}

TEST_F(HandoffMdnsHolderTest, SecondStartWhileAliveDoesNotRearmChecks) {
  make();
  ASSERT_TRUE(holder_->start(true));
  ASSERT_TRUE(holder_->start(true));
  EXPECT_EQ(g_fake.tasks.size(), 2u) << "a start with a live publisher must not arm more checks";
}

TEST_F(HandoffMdnsHolderTest, CheckWithoutRenameDoesNothing) {
  make();
  ASSERT_TRUE(holder_->start(true));
  g_fake.renamed = false;
  g_fake.tasks[0].first();
  g_fake.tasks[1].first();
  EXPECT_EQ(g_fake.publish_calls, 1) << "no rebuild when renamed() is false";
  EXPECT_EQ(count("destroy"), 0);
  EXPECT_EQ(holder_->rename_rebuilds(), 0u);
  EXPECT_TRUE(holder_->published());
}

TEST_F(HandoffMdnsHolderTest, CheckAfterRenameRebuildsWithoutSettleWait) {
  make();
  ASSERT_TRUE(holder_->start(true));
  g_fake.renamed = true;
  g_fake.events.clear();
  g_fake.tasks[0].first();
  const std::vector<std::string> expected {"destroy", "publish"};
  EXPECT_EQ(g_fake.events, expected) << "a rebuild destroys and publishes again, with no settle wait";
  EXPECT_EQ(holder_->rename_rebuilds(), 1u);
  EXPECT_TRUE(holder_->published());
}

TEST_F(HandoffMdnsHolderTest, SiblingCheckStillRunsAfterARebuild) {
  make();
  ASSERT_TRUE(holder_->start(true));
  g_fake.renamed = true;
  g_fake.tasks[0].first();
  ASSERT_EQ(holder_->rename_rebuilds(), 1u);
  g_fake.tasks[1].first();
  EXPECT_EQ(holder_->rename_rebuilds(), 2u) << "the rebuild keeps the epoch so the +10 s check still runs";
  g_fake.renamed = false;
}

TEST_F(HandoffMdnsHolderTest, SecondCheckSkipsWhenNotRenamedAnymore) {
  make();
  ASSERT_TRUE(holder_->start(true));
  g_fake.renamed = true;
  g_fake.tasks[0].first();
  g_fake.renamed = false;
  g_fake.tasks[1].first();
  EXPECT_EQ(holder_->rename_rebuilds(), 1u);
}

TEST_F(HandoffMdnsHolderTest, ACheckFiresAtMostOnce) {
  // SPEC-QUESTION: "A check fires at most once": running the same task twice is not a documented scheduler behaviour,
  // the test pins the sentence.
  make();
  ASSERT_TRUE(holder_->start(true));
  g_fake.renamed = true;
  g_fake.tasks[0].first();
  g_fake.tasks[0].first();
  EXPECT_EQ(holder_->rename_rebuilds(), 1u) << "the same check must not rebuild twice";
}

TEST_F(HandoffMdnsHolderTest, FailedRebuildLeavesUnpublishedAndIsNotRetried) {
  make();
  ASSERT_TRUE(holder_->start(true));
  g_fake.renamed = true;
  g_fake.null_publishes = 1;
  g_fake.tasks[0].first();
  EXPECT_FALSE(holder_->published()) << "a failed rebuild leaves the holder unpublished";
  const int publishes = g_fake.publish_calls;
  g_fake.tasks[1].first();
  EXPECT_EQ(g_fake.publish_calls, publishes) << "the sibling check finds no publisher alive and must not retry";
  EXPECT_FALSE(holder_->published());
}

TEST_F(HandoffMdnsHolderTest, WithdrawMakesArmedChecksNoOps) {
  make();
  ASSERT_TRUE(holder_->start(true));
  holder_->withdraw(100ms);
  g_fake.renamed = true;
  const int publishes = g_fake.publish_calls;
  g_fake.tasks[0].first();
  g_fake.tasks[1].first();
  EXPECT_EQ(g_fake.publish_calls, publishes) << "checks armed before a withdraw must do nothing";
  EXPECT_FALSE(holder_->published());
  EXPECT_EQ(holder_->rename_rebuilds(), 0u);
}

TEST_F(HandoffMdnsHolderTest, ChecksOfAnOldEpochAreNoOpsAfterRepublish) {
  make();
  ASSERT_TRUE(holder_->start(true));
  holder_->withdraw(100ms);
  ASSERT_TRUE(holder_->start(false));
  g_fake.renamed = true;
  const int publishes = g_fake.publish_calls;
  g_fake.tasks[0].first();
  g_fake.tasks[1].first();
  EXPECT_EQ(g_fake.publish_calls, publishes) << "a start bumps the epoch, so the older checks must do nothing";
  EXPECT_EQ(holder_->rename_rebuilds(), 0u);
}

TEST_F(HandoffMdnsHolderTest, ChecksOfTheNewEpochWorkAfterRepublishWithChecks) {
  make();
  ASSERT_TRUE(holder_->start(true));
  holder_->withdraw(100ms);
  ASSERT_TRUE(holder_->start(true));
  ASSERT_EQ(g_fake.tasks.size(), 4u);
  g_fake.renamed = true;
  g_fake.tasks[2].first();
  EXPECT_EQ(holder_->rename_rebuilds(), 1u) << "the checks armed by the second start must work";
}

TEST_F(HandoffMdnsHolderTest, StopDestroysWithoutWaitAndIsIdempotent) {
  make();
  ASSERT_TRUE(holder_->start());
  holder_->stop();
  holder_->stop();
  EXPECT_FALSE(holder_->published());
  EXPECT_EQ(count("destroy"), 1);
  EXPECT_EQ(count("wait:1000") + count("wait:0"), 0) << "stop must not wait";
  for (const auto &e : g_fake.events) {
    EXPECT_NE(e.rfind("wait", 0), 0u) << "stop must not call wait, saw " << e;
  }
}

TEST_F(HandoffMdnsHolderTest, StopWithoutPublisherIsHarmless) {
  make();
  holder_->stop();
  EXPECT_TRUE(g_fake.events.empty());
}

TEST_F(HandoffMdnsHolderTest, StopMakesArmedChecksNoOps) {
  make();
  ASSERT_TRUE(holder_->start(true));
  holder_->stop();
  g_fake.renamed = true;
  const int publishes = g_fake.publish_calls;
  g_fake.tasks[0].first();
  g_fake.tasks[1].first();
  EXPECT_EQ(g_fake.publish_calls, publishes) << "checks armed before stop must do nothing";
}

TEST_F(HandoffMdnsHolderTest, DestructorStopsPublishing) {
  make();
  ASSERT_TRUE(holder_->start());
  holder_.reset();
  EXPECT_EQ(count("destroy"), 1) << "destroying the holder must destroy the publisher";
}

TEST_F(HandoffMdnsHolderTest, CheckRunningAfterHolderStoppedFromAnotherThreadIsSafe) {
  make();
  ASSERT_TRUE(holder_->start(true));
  g_fake.renamed = true;
  auto task = g_fake.tasks[0].first;
  std::thread t([this] {
    holder_->stop();
  });
  t.join();
  task();
  EXPECT_FALSE(holder_->published()) << "a check that runs after stop must not republish";
}

TEST_F(HandoffMdnsHolderTest, NullOpsMembersTakeProductionDefaultsWithoutPublishingAtConstruction) {
  // Only construction and destruction: a holder built with all-null ops must not publish or crash.
  handoff::mdns_ops_t none;
  handoff::mdns_holder_t holder {none, handoff::mdns_timing_t {}};
  EXPECT_FALSE(holder.published()) << "nothing may be published before start";
  holder.stop();
  holder.withdraw(10ms);
  EXPECT_FALSE(holder.published());
}
#endif
