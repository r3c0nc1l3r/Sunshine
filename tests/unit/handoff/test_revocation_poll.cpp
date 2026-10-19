/**
 * @file tests/unit/handoff/test_revocation_poll.cpp
 * @brief Blind tests for the revocation poller (src/handoff/revocation_poll.h), written from the header and the spec
 * only.
 *
 * SPEC-QUESTION notes (also listed in the report):
 * - The production canonicalizer (`crypto::x509`) needs real certificates and is not reachable through the test
 *   config, so only the injected canonicalizer is tested.
 * - Whether inotify is available is not controllable; the event tests assume it works on Linux and use a short `cap`
 *   so they also pass on the timer alone.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <atomic>
  #include <chrono>
  #include <condition_variable>
  #include <cstdint>
  #include <filesystem>
  #include <fstream>
  #include <functional>
  #include <memory>
  #include <mutex>
  #include <set>
  #include <stdexcept>
  #include <string>
  #include <thread>
  #include <vector>

  // lib includes
  #include <boost/log/attributes/value_extraction.hpp>
  #include <boost/log/core.hpp>
  #include <boost/log/sinks/basic_sink_backend.hpp>
  #include <boost/log/sinks/sync_frontend.hpp>
  #include <boost/make_shared.hpp>

  // platform includes
  #include <stdlib.h>
  #include <sys/stat.h>
  #include <unistd.h>

  // local includes
  #include "src/handoff/revocation_poll.h"

using namespace std::chrono_literals;

namespace {
  using clock_type = std::chrono::steady_clock;
  namespace fs = std::filesystem;
  namespace bl = boost::log;

  /**
   * @brief One captured log record.
   */
  struct log_entry_t {
    int severity {-1};  ///< Boost severity (2 Info, 3 Warning, 4 Error).
    std::string message;  ///< Message text.
  };

  /**
   * @brief Shared store of captured log records.
   */
  struct log_store_t {
    std::mutex mutex;  ///< Guards the entries.
    std::vector<log_entry_t> entries;  ///< Captured records.
  };

  /**
   * @brief Sink backend that stores every record.
   */
  class capture_backend_t: public bl::sinks::basic_sink_backend<bl::sinks::synchronized_feeding> {
  public:
    explicit capture_backend_t(std::shared_ptr<log_store_t> store):
        _store(std::move(store)) {
    }

    void consume(const bl::record_view &record) {
      log_entry_t entry;
      if (auto severity = bl::extract<int>("Severity", record)) {
        entry.severity = *severity;
      }
      if (auto message = bl::extract<std::string>("Message", record)) {
        entry.message = *message;
      }
      std::lock_guard lock(_store->mutex);
      _store->entries.push_back(std::move(entry));
    }

  private:
    std::shared_ptr<log_store_t> _store;  ///< Where records go.
  };

  /**
   * @brief Installs a capturing sink for the lifetime of the object.
   */
  class log_capture_t {
  public:
    log_capture_t():
        _store(std::make_shared<log_store_t>()),
        _sink(boost::make_shared<sink_t>(boost::make_shared<capture_backend_t>(_store))) {
      bl::core::get()->add_sink(_sink);
    }

    ~log_capture_t() {
      bl::core::get()->remove_sink(_sink);
    }

    log_capture_t(const log_capture_t &) = delete;
    log_capture_t &operator=(const log_capture_t &) = delete;

    /**
     * @brief Number of records with a severity.
     */
    std::size_t count(int severity) const {
      std::lock_guard lock(_store->mutex);
      std::size_t n = 0;
      for (const auto &e : _store->entries) {
        n += e.severity == severity ? 1 : 0;
      }
      return n;
    }

    /**
     * @brief Wait until at least `n` records of a severity exist.
     */
    bool wait_count(int severity, std::size_t n, std::chrono::milliseconds limit) const {
      const auto deadline = clock_type::now() + limit;
      while (clock_type::now() < deadline) {
        if (count(severity) >= n) {
          return true;
        }
        std::this_thread::sleep_for(2ms);
      }
      return count(severity) >= n;
    }

    /**
     * @brief Whether any captured message contains a text.
     */
    bool contains(const std::string &text) const {
      std::lock_guard lock(_store->mutex);
      for (const auto &e : _store->entries) {
        if (e.message.find(text) != std::string::npos) {
          return true;
        }
      }
      return false;
    }

  private:
    using sink_t = bl::sinks::synchronous_sink<capture_backend_t>;
    std::shared_ptr<log_store_t> _store;  ///< Captured records.
    boost::shared_ptr<sink_t> _sink;  ///< The installed sink.
  };

  [[maybe_unused]] constexpr int sev_info = 2;  ///< Info.
  [[maybe_unused]] constexpr int sev_warning = 3;  ///< Warning.
  [[maybe_unused]] constexpr int sev_error = 4;  ///< Error.

  /**
   * @brief Fixture: a temp directory, a state file path and a recording terminate callback.
   */
  class HandoffRevocationPollTest: public ::testing::Test {
  protected:
    void SetUp() override {
      capture = std::make_unique<log_capture_t>();
      char pattern[] = "/tmp/hoff_revpoll_XXXXXX";
      const char *made = ::mkdtemp(pattern);
      ASSERT_NE(nullptr, made) << "mkdtemp must work.";
      dir = made;
      state_file = dir / "sunshine_state.json";
      saved_cwd = fs::current_path();
      draining = true;
    }

    void TearDown() override {
      gate_open();
      poller.reset();
      std::error_code ec;
      fs::current_path(saved_cwd, ec);
      fs::remove_all(dir, ec);
      capture.reset();
    }

    /**
     * @brief Build a test config with recording callbacks.
     */
    handoff::revocation_poller_t::test_config_t make_config() {
      handoff::revocation_poller_t::test_config_t config;
      config.state_file = state_file;
      config.terminate = [this](const std::string &pem) {
        {
          std::unique_lock lock(mutex);
          calls.push_back(pem);
          if (blocking) {
            blocked_entered = true;
            cv.notify_all();
            cv.wait_for(lock, 5s, [&] {
              return !blocking;
            });
          }
        }
        if (throws_remaining.load() > 0) {
          --throws_remaining;
          throw std::runtime_error("terminate");
        }
      };
      config.is_draining = [this] {
        return draining.load();
      };
      config.now = [this] {
        return clock_base + std::chrono::milliseconds(clock_ms.load());
      };
      config.cap = 50ms;
      return config;
    }

    void make_poller(handoff::revocation_poller_t::test_config_t config) {
      poller = std::make_unique<handoff::revocation_poller_t>(std::move(config));
    }

    void make_poller() {
      make_poller(make_config());
    }

    /**
     * @brief JSON state file content; each entry is {cert, enabled text}.
     */
    static std::string state_json(const std::vector<std::pair<std::string, std::string>> &devices) {
      std::string out = R"({"root":{"uniqueid":"x","named_devices":[)";
      bool first = true;
      for (const auto &[cert, enabled] : devices) {
        out += first ? "" : ",";
        first = false;
        out += R"({"name":"n","cert":")" + cert + R"(","uuid":"u","enabled":")" + enabled + R"("})";
      }
      return out + "]}}";
    }

    /**
     * @brief Replace the state file atomically (temp file in the same directory, then rename).
     */
    void write_state(const std::string &content) {
      const auto tmp = dir / "state.tmp";
      {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << content;
      }
      fs::rename(tmp, state_file);
    }

    void write_devices(const std::vector<std::pair<std::string, std::string>> &devices) {
      write_state(state_json(devices));
    }

    std::vector<std::string> recorded() {
      std::lock_guard lock(mutex);
      return calls;
    }

    std::size_t count_of(const std::string &pem) {
      std::lock_guard lock(mutex);
      std::size_t n = 0;
      for (const auto &c : calls) {
        n += c == pem ? 1 : 0;
      }
      return n;
    }

    std::size_t total_calls() {
      std::lock_guard lock(mutex);
      return calls.size();
    }

    template<class Pred>
    static bool wait_until(Pred pred, std::chrono::milliseconds limit) {
      const auto deadline = clock_type::now() + limit;
      while (clock_type::now() < deadline) {
        if (pred()) {
          return true;
        }
        std::this_thread::sleep_for(2ms);
      }
      return pred();
    }

    bool wait_passes(std::uint64_t n, std::chrono::milliseconds limit = 1000ms) {
      return wait_until(
        [&] {
          return poller->passes_for_test() >= n;
        },
        limit
      );
    }

    /**
     * @brief Wait for `n` more completed passes than now.
     */
    bool wait_more_passes(std::uint64_t n, std::chrono::milliseconds limit = 1000ms) {
      return wait_passes(poller->passes_for_test() + n, limit);
    }

    void gate_close() {
      std::lock_guard lock(mutex);
      blocking = true;
      blocked_entered = false;
    }

    void gate_open() {
      {
        std::lock_guard lock(mutex);
        blocking = false;
      }
      cv.notify_all();
    }

    bool wait_blocked(std::chrono::milliseconds limit) {
      std::unique_lock lock(mutex);
      return cv.wait_for(lock, limit, [&] {
        return blocked_entered;
      });
    }

    std::unique_ptr<log_capture_t> capture;  ///< Log capture.
    std::unique_ptr<handoff::revocation_poller_t> poller;  ///< Under test.
    fs::path dir;  ///< Temp directory.
    fs::path state_file;  ///< State file path.
    fs::path saved_cwd;  ///< Working directory to restore.
    std::atomic<bool> draining {true};  ///< The `is_draining` answer.
    std::atomic<int> throws_remaining {0};  ///< Callback calls that still throw.
    std::atomic<std::int64_t> clock_ms {0};  ///< Fake clock offset.
    clock_type::time_point clock_base = clock_type::now();  ///< Fake clock origin.
    std::mutex mutex;  ///< Guards the members below.
    std::condition_variable cv;  ///< Signals changes.
    std::vector<std::string> calls;  ///< PEMs passed to the callback.
    bool blocking {false};  ///< The callback blocks.
    bool blocked_entered {false};  ///< A callback call entered while blocking.
  };
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------------------------------------------------

TEST(HandoffRevocationPollConstantsTest, WakeCapIsTwoSeconds) {
  EXPECT_EQ(2000ms, handoff::revocation_poller_t::wake_cap) << "wake_cap must be 2 s.";
}

TEST_F(HandoffRevocationPollTest, StartRefusedWhenNotDraining) {
  make_poller();
  draining = false;
  write_devices({{"cert-a", "false"}});
  EXPECT_FALSE(poller->start()) << "start() must return false when the process is not draining.";
  EXPECT_FALSE(poller->running()) << "A refused start must leave the poller not running.";
  std::this_thread::sleep_for(150ms);
  EXPECT_EQ(0U, poller->passes_for_test()) << "No pass may run when start() was refused.";
  EXPECT_EQ(0U, total_calls()) << "No session may be ended when start() was refused.";
}

TEST_F(HandoffRevocationPollTest, StartRefusedWhenTheStatePathIsEmpty) {
  auto config = make_config();
  config.state_file.clear();
  make_poller(std::move(config));
  EXPECT_FALSE(poller->start()) << "start() must return false when the state file path is empty.";
  EXPECT_FALSE(poller->running()) << "The poller must not be running.";
}

TEST_F(HandoffRevocationPollTest, StartIsIdempotentAndStopIsIdempotent) {
  make_poller();
  write_devices({{"cert-a", "true"}});
  EXPECT_FALSE(poller->running()) << "Not running before start().";
  EXPECT_TRUE(poller->start()) << "start() must return true when it started.";
  EXPECT_TRUE(poller->running()) << "running() must be true after start().";
  EXPECT_TRUE(poller->start()) << "A second start() must return true (already running).";
  EXPECT_TRUE(poller->running()) << "Still running after the second start().";
  poller->stop();
  EXPECT_FALSE(poller->running()) << "running() must be false after stop().";
  EXPECT_NO_THROW(poller->stop()) << "A second stop() must be harmless.";
}

TEST_F(HandoffRevocationPollTest, StopWithoutStartIsHarmless) {
  make_poller();
  EXPECT_NO_THROW(poller->stop()) << "stop() before start() must be harmless.";
  EXPECT_FALSE(poller->running()) << "Not running.";
}

TEST_F(HandoffRevocationPollTest, DestructorStopsAndJoinsTheThread) {
  make_poller();
  write_devices({{"cert-a", "true"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_passes(1)) << "A pass must run.";
  EXPECT_NO_THROW(poller.reset()) << "Destroying a running poller must stop and join it.";
}

TEST_F(HandoffRevocationPollTest, FirstPassRunsImmediatelyAtStart) {
  auto config = make_config();
  config.cap = 5000ms;
  make_poller(std::move(config));
  write_devices({{"cert-a", "false"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  EXPECT_TRUE(wait_until(
    [&] {
      return count_of("cert-a") == 1;
    },
    500ms
  )) << "A pass must run immediately at start, not after the cap.";
}

TEST_F(HandoffRevocationPollTest, StopWakesPromptlyWithoutWaitingForTheCap) {
  auto config = make_config();
  config.cap = 5000ms;
  make_poller(std::move(config));
  write_devices({{"cert-a", "true"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_passes(1)) << "The first pass must run.";
  const auto begin = clock_type::now();
  poller->stop();
  EXPECT_LT(clock_type::now() - begin, 500ms) << "stop() must wake the thread at once, not wait for the wake cap.";
  EXPECT_FALSE(poller->running()) << "Not running after stop().";
}

TEST_F(HandoffRevocationPollTest, RestartAfterStopStartsWithAnEmptyHandledSet) {
  make_poller();
  write_devices({{"cert-a", "false"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_until(
    [&] {
      return count_of("cert-a") == 1;
    },
    1000ms
  )) << "cert-a must be terminated once.";
  poller->stop();
  ASSERT_TRUE(poller->start()) << "start() after stop() must work.";
  EXPECT_TRUE(wait_until(
    [&] {
      return count_of("cert-a") == 2;
    },
    1000ms
  )) << "After a restart the previous disabled set is empty, so the disabled client is terminated again.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Disabled clients
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffRevocationPollTest, ADisabledCertIsTerminatedExactlyOnce) {
  make_poller();
  write_devices({{"cert-a", "false"}, {"cert-b", "true"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_until(
    [&] {
      return count_of("cert-a") >= 1;
    },
    1000ms
  )) << "A disabled client must be terminated.";
  ASSERT_TRUE(wait_more_passes(4)) << "More passes must run.";
  EXPECT_EQ(1U, count_of("cert-a")) << "A disabled client must be terminated once, not on every wake.";
  EXPECT_EQ(0U, count_of("cert-b")) << "An enabled client must not be terminated.";
}

TEST_F(HandoffRevocationPollTest, AMissingEnabledMemberOrTrueEndsNothing) {
  make_poller();
  write_state(R"({"root":{"named_devices":[{"cert":"cert-a","enabled":"true"},{"cert":"cert-b"},{"cert":"cert-c","enabled":"1"}]}})");
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_passes(3)) << "Passes must run.";
  EXPECT_EQ(0U, total_calls()) << "Enabled clients must end nothing.";
  EXPECT_EQ(0U, poller->failed_passes_for_test()) << "A valid file is not a failed pass.";
}

TEST_F(HandoffRevocationPollTest, TheCanonicalPemIsPassedToTheCallback) {
  auto config = make_config();
  config.canonicalize = [](const std::string &pem) {
    return "canon:" + pem;
  };
  make_poller(std::move(config));
  write_devices({{"cert-a", "false"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_until(
    [&] {
      return total_calls() >= 1;
    },
    1000ms
  )) << "The disabled client must be terminated.";
  EXPECT_EQ((std::vector<std::string> {"canon:cert-a"}), recorded()) << "The callback must get the canonicalized PEM, not the raw one.";
}

TEST_F(HandoffRevocationPollTest, ACertDisabledAfterStartIsTerminatedOnce) {
  make_poller();
  write_devices({{"cert-a", "true"}, {"cert-b", "true"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_passes(2)) << "Initial passes must run.";
  EXPECT_EQ(0U, total_calls()) << "Nothing is disabled yet.";
  write_devices({{"cert-a", "true"}, {"cert-b", "false"}});
  ASSERT_TRUE(wait_until(
    [&] {
      return count_of("cert-b") >= 1;
    },
    1000ms
  )) << "A client disabled in a rewritten file must be terminated.";
  ASSERT_TRUE(wait_more_passes(4)) << "More passes must run.";
  EXPECT_EQ(1U, count_of("cert-b")) << "It must be terminated once.";
  EXPECT_EQ(0U, count_of("cert-a")) << "The other client must not be touched.";
}

TEST_F(HandoffRevocationPollTest, OnlyTheNewlyDisabledCertIsTerminatedWhenAnotherIsAlreadyHandled) {
  make_poller();
  write_devices({{"cert-a", "false"}, {"cert-b", "true"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_until(
    [&] {
      return count_of("cert-a") == 1;
    },
    1000ms
  )) << "cert-a must be terminated.";
  write_devices({{"cert-a", "false"}, {"cert-b", "false"}});
  ASSERT_TRUE(wait_until(
    [&] {
      return count_of("cert-b") == 1;
    },
    1000ms
  )) << "cert-b must be terminated.";
  ASSERT_TRUE(wait_more_passes(4)) << "More passes must run.";
  EXPECT_EQ(1U, count_of("cert-a")) << "An already handled certificate must not be terminated again.";
  EXPECT_EQ(1U, count_of("cert-b")) << "The new certificate is terminated once.";
}

TEST_F(HandoffRevocationPollTest, AnEnabledThenDisabledAgainCertIsNewAgain) {
  make_poller();
  write_devices({{"cert-a", "false"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_until(
    [&] {
      return count_of("cert-a") == 1;
    },
    1000ms
  )) << "First termination.";
  write_devices({{"cert-a", "true"}});
  ASSERT_TRUE(wait_more_passes(3)) << "A pass must see the file with the client enabled.";
  write_devices({{"cert-a", "false"}});
  EXPECT_TRUE(wait_until(
    [&] {
      return count_of("cert-a") == 2;
    },
    1000ms
  )) << "A client that was enabled again and disabled again is new again.";
}

TEST_F(HandoffRevocationPollTest, AClientMissingFromTheFileEndsNothingAndIsNotForgotten) {
  make_poller();
  write_devices({{"cert-a", "false"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_until(
    [&] {
      return count_of("cert-a") == 1;
    },
    1000ms
  )) << "cert-a must be terminated.";
  write_devices({{"cert-z", "true"}});
  ASSERT_TRUE(wait_more_passes(3)) << "A pass must see the file without cert-a.";
  EXPECT_EQ(1U, total_calls()) << "A client missing from the file ends nothing.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Invalid certificates
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffRevocationPollTest, AnInvalidCertIsSkippedWithASingleWarning) {
  auto config = make_config();
  config.canonicalize = [](const std::string &pem) {
    return pem == "SECRET-INVALID-MARKER" ? std::string {} : pem;
  };
  make_poller(std::move(config));
  write_devices({{"SECRET-INVALID-MARKER", "false"}, {"cert-good", "false"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_until(
    [&] {
      return count_of("cert-good") == 1;
    },
    1000ms
  )) << "The valid cert must still be terminated.";
  ASSERT_TRUE(wait_more_passes(6)) << "Many passes must run.";
  EXPECT_EQ(1U, total_calls()) << "The invalid certificate must not reach the callback (not even as an empty string).";
  EXPECT_EQ(1U, capture->count(sev_warning)) << "The invalid certificate must be logged as a Warning once, not on every wake.";
  EXPECT_FALSE(capture->contains("SECRET-INVALID-MARKER")) << "A Warning must never contain the certificate.";
}

TEST_F(HandoffRevocationPollTest, AnInvalidCertWarnsAgainAfterItLeavesAndReturns) {
  auto config = make_config();
  config.canonicalize = [](const std::string &pem) {
    return pem == "bad" ? std::string {} : pem;
  };
  make_poller(std::move(config));
  write_devices({{"bad", "false"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(capture->wait_count(sev_warning, 1, 1000ms)) << "The first Warning.";
  write_devices({{"bad", "true"}});
  ASSERT_TRUE(wait_more_passes(3)) << "A pass must see it enabled.";
  write_devices({{"bad", "false"}});
  EXPECT_TRUE(capture->wait_count(sev_warning, 2, 1000ms)) << "When the certificate returns to the disabled set it is new again and warns again.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Callback failures
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffRevocationPollTest, AThrowingCallbackIsRetriedAtTheNextPass) {
  make_poller();
  throws_remaining = 2;
  write_devices({{"cert-a", "false"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  EXPECT_TRUE(wait_until(
    [&] {
      return count_of("cert-a") >= 3;
    },
    1500ms
  )) << "A certificate whose callback threw stays new and must be retried at the next wake.";
  ASSERT_TRUE(wait_more_passes(4)) << "More passes must run.";
  EXPECT_EQ(3U, count_of("cert-a")) << "After a successful call the certificate is handled and must not be called again.";
  EXPECT_EQ(capture->count(sev_error), 1U) << "A throwing callback logs one Error per certificate within 60 s (injected clock).";
}

TEST_F(HandoffRevocationPollTest, AThrowingCallbackLogsAgainAfterTheErrorInterval) {
  make_poller();
  throws_remaining = 1000;
  write_devices({{"cert-a", "false"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(capture->wait_count(sev_error, 1, 1000ms)) << "The first throw must log an Error.";
  ASSERT_TRUE(wait_more_passes(3)) << "More passes must run.";
  EXPECT_EQ(capture->count(sev_error), 1U) << "Further throws within 60 s must not log another Error.";
  clock_ms += 61'000;
  EXPECT_TRUE(capture->wait_count(sev_error, 2, 1000ms)) << "A throw 60 s or more after the last Error must log one again.";
}

TEST_F(HandoffRevocationPollTest, AThrowingCallbackDoesNotStopTheOtherCerts) {
  make_poller();
  throws_remaining = 1;
  write_devices({{"cert-a", "false"}, {"cert-b", "false"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_until(
    [&] {
      return count_of("cert-b") >= 1 && count_of("cert-a") >= 2;
    },
    1500ms
  )) << "The first certificate threw once and must be retried; the other one must still be processed.";
  ASSERT_TRUE(wait_more_passes(4)) << "More passes must run.";
  EXPECT_EQ(1U, count_of("cert-b")) << "The certificate that succeeded must not be called again.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Failed passes
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffRevocationPollTest, AMissingFileIsAFailedPassWithOneWarning) {
  make_poller();
  ASSERT_TRUE(poller->start()) << "start() must succeed even if the file does not exist yet.";
  ASSERT_TRUE(wait_passes(5)) << "Passes must keep running.";
  EXPECT_GE(poller->failed_passes_for_test(), 5U) << "Each pass with a missing file is a failed pass.";
  EXPECT_EQ(1U, capture->count(sev_warning)) << "A failure episode logs one Warning, then at most one per 60 s.";
  EXPECT_EQ(0U, total_calls()) << "A missing file ends no session.";
}

TEST_F(HandoffRevocationPollTest, TheFailureWarningIsRateLimitedBy60SecondsOfTheInjectedClock) {
  make_poller();
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(capture->wait_count(sev_warning, 1, 1000ms)) << "The first failed pass logs a Warning.";
  ASSERT_TRUE(wait_more_passes(4)) << "More failed passes must run.";
  EXPECT_EQ(1U, capture->count(sev_warning)) << "No further Warning within 60 s.";
  clock_ms = 61000;
  EXPECT_TRUE(capture->wait_count(sev_warning, 2, 1000ms)) << "After 60 s a further failed pass of the same episode logs another Warning.";
}

TEST_F(HandoffRevocationPollTest, RemovingTheFileEndsNothingAndKeepsTheHandledSet) {
  make_poller();
  write_devices({{"cert-a", "false"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_until(
    [&] {
      return count_of("cert-a") == 1;
    },
    1000ms
  )) << "cert-a must be terminated.";
  fs::remove(state_file);
  ASSERT_TRUE(wait_until(
    [&] {
      return poller->failed_passes_for_test() >= 1;
    },
    1000ms
  )) << "A pass that finds the file removed must count as failed.";
  EXPECT_TRUE(capture->wait_count(sev_warning, 1, 500ms)) << "A removed file must be logged as a Warning.";
  EXPECT_EQ(1U, total_calls()) << "A removed file ends no further session.";
  write_devices({{"cert-a", "false"}});
  ASSERT_TRUE(wait_more_passes(3)) << "The file is read again.";
  EXPECT_EQ(1U, count_of("cert-a")) << "A failed pass leaves the handled set unchanged: cert-a is not terminated again.";
  EXPECT_GE(capture->count(sev_info), 1U) << "The first successful pass after a failure logs an Info line.";
}

TEST_F(HandoffRevocationPollTest, APartialFileEndsNothingAndIsRetried) {
  make_poller();
  const auto full = state_json({{"cert-a", "false"}});
  write_state(full.substr(0, full.size() / 2));
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_passes(3)) << "Passes must run.";
  EXPECT_GE(poller->failed_passes_for_test(), 3U) << "A partial file is a failed pass every time.";
  EXPECT_EQ(0U, total_calls()) << "A partial file ends no session.";
  EXPECT_GE(capture->count(sev_warning), 1U) << "A partial file is logged as a Warning.";
  write_state(full);
  EXPECT_TRUE(wait_until(
    [&] {
      return count_of("cert-a") == 1;
    },
    1000ms
  )) << "Once the file is complete it is read and the client is terminated.";
}

TEST_F(HandoffRevocationPollTest, AFifoAtTheStatePathIsAFailedPassAndNeverBlocks) {
  make_poller();
  ASSERT_EQ(0, ::mkfifo(state_file.c_str(), 0600)) << "mkfifo must work.";
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_passes(3, 1500ms)) << "Passes must keep completing: a FIFO must never block the poller.";
  EXPECT_GE(poller->failed_passes_for_test(), 3U) << "A non-regular state file (FIFO) counts as unreadable: a failed pass.";
  EXPECT_EQ(0U, total_calls()) << "It ends nothing.";
  EXPECT_GE(capture->count(sev_warning), 1U) << "It is logged as a Warning.";
  const auto begin = clock_type::now();
  poller->stop();
  EXPECT_LT(clock_type::now() - begin, 500ms) << "stop() must return promptly with a FIFO at the state path.";
}

TEST_F(HandoffRevocationPollTest, TwoQuickWritesAreCoalescedIntoOnePassThatSeesTheFinalContent) {
  auto config = make_config();
  config.cap = 5000ms;
  make_poller(std::move(config));
  write_devices({{"cert-a", "true"}, {"cert-x", "true"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_passes(1)) << "The first pass runs at start.";
  const auto before = poller->passes_for_test();
  write_devices({{"cert-a", "true"}, {"cert-x", "false"}});
  std::this_thread::sleep_for(5ms);
  write_devices({{"cert-a", "false"}, {"cert-x", "true"}});
  ASSERT_TRUE(wait_until(
    [&] {
      return count_of("cert-a") == 1;
    },
    800ms
  )) << "The final content must be seen.";
  std::this_thread::sleep_for(150ms);
  EXPECT_EQ(before + 1, poller->passes_for_test()) << "Events that arrive close together must be coalesced into one pass.";
  EXPECT_EQ(0U, count_of("cert-x")) << "The intermediate content must not be read: the pass runs after the writes settle.";
}

TEST_F(HandoffRevocationPollTest, RecoveryAfterAFailedEpisodeLogsOneInfoLine) {
  make_poller();
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(capture->wait_count(sev_warning, 1, 1000ms)) << "The failed episode starts with a Warning.";
  ASSERT_TRUE(wait_more_passes(2)) << "More failed passes.";
  const auto infos = capture->count(sev_info);
  write_devices({{"cert-a", "true"}});
  EXPECT_TRUE(capture->wait_count(sev_info, infos + 1, 1000ms)) << "The first successful pass after a failure must log an Info line.";
  ASSERT_TRUE(wait_more_passes(4)) << "More successful passes.";
  EXPECT_EQ(infos + 1, capture->count(sev_info)) << "Only the first successful pass logs the Info line.";
}

TEST_F(HandoffRevocationPollTest, AnEmptyFileIsAFailedPass) {
  make_poller();
  write_state("");
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_passes(3)) << "Passes must run.";
  EXPECT_GE(poller->failed_passes_for_test(), 3U) << "An empty read is a failed pass.";
  EXPECT_EQ(0U, total_calls()) << "An empty file ends nothing.";
}

TEST_F(HandoffRevocationPollTest, ValidJsonWithoutAClientListIsAFailedPass) {
  make_poller();
  write_state(R"({"root":{"uniqueid":"x"}})");
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_passes(3)) << "Passes must run.";
  EXPECT_GE(poller->failed_passes_for_test(), 3U) << "A file with no parseable client list is a failed pass.";
  EXPECT_EQ(0U, total_calls()) << "It ends nothing.";
  EXPECT_GE(capture->count(sev_warning), 1U) << "It is logged as a Warning.";
}

TEST_F(HandoffRevocationPollTest, AnUnreadablePathIsAFailedPassAndNeverLogsContent) {
  make_poller();
  fs::create_directory(state_file);
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_passes(3)) << "Passes must run.";
  EXPECT_GE(poller->failed_passes_for_test(), 3U) << "A path that cannot be read as a file is a failed pass.";
  EXPECT_EQ(0U, total_calls()) << "It ends nothing.";
  EXPECT_GE(capture->count(sev_warning), 1U) << "It is logged as a Warning.";
}

TEST_F(HandoffRevocationPollTest, AFileOverFourMebibytesCountsAsUnreadable) {
  make_poller();
  auto content = state_json({{"cert-a", "false"}});
  content.append(4 * 1024 * 1024 + 16 - content.size() % 16, ' ');
  ASSERT_GT(content.size(), 4U * 1024 * 1024) << "The test file must exceed 4 MiB.";
  write_state(content);
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_passes(2, 1500ms)) << "Passes must run.";
  EXPECT_GE(poller->failed_passes_for_test(), 2U) << "A file longer than 4 MiB counts as unreadable.";
  EXPECT_EQ(0U, total_calls()) << "It ends nothing.";
}

TEST_F(HandoffRevocationPollTest, WarningsNeverContainTheFileContent) {
  make_poller();
  write_state(R"({"root":{"named_devices":[{"cert":"SECRET-CONTENT-MARKER", "enabled": )");
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(capture->wait_count(sev_warning, 1, 1000ms)) << "A Warning is expected.";
  EXPECT_FALSE(capture->contains("SECRET-CONTENT-MARKER")) << "A Warning must never contain the file content or a certificate.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Draining gate
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffRevocationPollTest, WakesWhileNotDrainingRunNoPassAndAreNotCounted) {
  make_poller();
  write_devices({{"cert-a", "true"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_passes(2)) << "Passes run while draining.";
  draining = false;
  std::this_thread::sleep_for(100ms);
  const auto frozen = poller->passes_for_test();
  write_devices({{"cert-a", "false"}});
  std::this_thread::sleep_for(250ms);
  EXPECT_EQ(frozen, poller->passes_for_test()) << "A wake while not draining must run no pass and not count in passes_for_test().";
  EXPECT_EQ(0U, total_calls()) << "No session may be ended once the process is no longer draining.";
  EXPECT_TRUE(poller->running()) << "The thread keeps running until stop().";
  draining = true;
  EXPECT_TRUE(wait_until(
    [&] {
      return count_of("cert-a") == 1;
    },
    1000ms
  )) << "When draining again the pass runs and the disabled client is terminated.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Wakes, directory handling
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffRevocationPollTest, TheTimerAloneKeepsPassesRunning) {
  make_poller();
  write_devices({{"cert-a", "true"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  EXPECT_TRUE(wait_passes(6, 1500ms)) << "With a 50 ms cap passes must keep running without any event.";
}

TEST_F(HandoffRevocationPollTest, AnEventOnTheStateFileWakesAPassBeforeTheCap) {
  auto config = make_config();
  config.cap = 5000ms;
  make_poller(std::move(config));
  write_devices({{"cert-a", "true"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_passes(1)) << "The first pass runs at start.";
  write_devices({{"cert-a", "false"}});
  EXPECT_TRUE(wait_until(
    [&] {
      return count_of("cert-a") == 1;
    },
    800ms
  )) << "A replacement by rename must start a pass at once (the directory is watched), well before the cap.";
}

TEST_F(HandoffRevocationPollTest, AnInPlaceWriteWakesAPassBeforeTheCap) {
  auto config = make_config();
  config.cap = 5000ms;
  make_poller(std::move(config));
  write_devices({{"cert-a", "true"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_passes(1)) << "The first pass runs at start.";
  {
    std::ofstream out(state_file, std::ios::binary | std::ios::trunc);
    out << state_json({{"cert-a", "false"}});
  }
  EXPECT_TRUE(wait_until(
    [&] {
      return count_of("cert-a") == 1;
    },
    800ms
  )) << "A modification in place must start a pass at once.";
}

TEST_F(HandoffRevocationPollTest, DeletingTheFileWakesAFailedPassBeforeTheCap) {
  auto config = make_config();
  config.cap = 5000ms;
  make_poller(std::move(config));
  write_devices({{"cert-a", "true"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_passes(1)) << "The first pass runs at start.";
  fs::remove(state_file);
  EXPECT_TRUE(wait_until(
    [&] {
      return poller->failed_passes_for_test() >= 1;
    },
    800ms
  )) << "Deleting the state file must start a pass at once.";
}

TEST_F(HandoffRevocationPollTest, EventsForOtherFilesStartNoPass) {
  auto config = make_config();
  config.cap = 5000ms;
  make_poller(std::move(config));
  write_devices({{"cert-a", "true"}});
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_passes(1)) << "The first pass runs at start.";
  const auto before = poller->passes_for_test();
  for (int i = 0; i < 5; ++i) {
    std::ofstream out(dir / ("other" + std::to_string(i) + ".txt"));
    out << "x";
  }
  fs::remove(dir / "other0.txt");
  std::this_thread::sleep_for(250ms);
  EXPECT_EQ(before, poller->passes_for_test()) << "Events for files other than the state file must not start a pass.";
}

TEST_F(HandoffRevocationPollTest, ABareFileNameWatchesTheCurrentDirectory) {
  fs::current_path(dir);
  auto config = make_config();
  config.state_file = fs::path("sunshine_state.json");
  config.cap = 300ms;
  make_poller(std::move(config));
  write_devices({{"cert-a", "true"}});
  ASSERT_TRUE(poller->start()) << "A bare file name must be accepted.";
  ASSERT_TRUE(wait_passes(2)) << "Passes must run.";
  EXPECT_EQ(0U, poller->failed_passes_for_test()) << "The bare name is read relative to the current directory.";
  EXPECT_EQ(0U, capture->count(sev_warning)) << "A bare file name must not warn about a missing directory.";
  write_devices({{"cert-a", "false"}});
  EXPECT_TRUE(wait_until(
    [&] {
      return count_of("cert-a") == 1;
    },
    1500ms
  )) << "A rewrite of the file in the current directory must be seen.";
}

TEST_F(HandoffRevocationPollTest, AMissingDirectoryMeansTimerOnlyWithoutStoppingThePoller) {
  auto config = make_config();
  config.state_file = dir / "missing_dir" / "sunshine_state.json";
  make_poller(std::move(config));
  ASSERT_TRUE(poller->start()) << "start() must succeed although the directory is missing.";
  ASSERT_TRUE(wait_passes(4, 1500ms)) << "The timer must keep passes running.";
  EXPECT_GE(poller->failed_passes_for_test(), 4U) << "The missing file is reported as a failure on each pass.";
  EXPECT_GE(capture->count(sev_warning), 1U) << "A missing directory is logged as a Warning.";
  EXPECT_LE(capture->count(sev_warning), 2U) << "One Warning for the directory and one for the failure episode, not one per wake.";
  EXPECT_TRUE(poller->running()) << "The poller keeps running.";
  fs::create_directory(dir / "missing_dir");
  {
    std::ofstream out(dir / "missing_dir" / "sunshine_state.json", std::ios::binary);
    out << state_json({{"cert-a", "false"}});
  }
  EXPECT_TRUE(wait_until(
    [&] {
      return count_of("cert-a") == 1;
    },
    1500ms
  )) << "With the timer alone, the file is read once it appears.";
}

// ---------------------------------------------------------------------------------------------------------------------
// stop() against a running callback
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffRevocationPollTest, StopWaitsForARunningCallbackAndStartsNoFurtherOne) {
  make_poller();
  write_devices({{"cert-a", "false"}, {"cert-b", "false"}, {"cert-c", "false"}});
  gate_close();
  ASSERT_TRUE(poller->start()) << "start() must succeed.";
  ASSERT_TRUE(wait_blocked(1000ms)) << "The first callback must be running.";
  std::atomic<bool> stopped {false};
  std::thread stopper([&] {
    poller->stop();
    stopped = true;
  });
  std::this_thread::sleep_for(100ms);
  EXPECT_FALSE(stopped.load()) << "stop() must wait for a callback that is running.";
  gate_open();
  stopper.join();
  EXPECT_TRUE(stopped.load()) << "stop() must return once the callback finished.";
  EXPECT_EQ(1U, total_calls()) << "Once stop is requested no further callback may start; the rest are skipped.";
  EXPECT_FALSE(poller->running()) << "Not running after stop().";
}

#endif
