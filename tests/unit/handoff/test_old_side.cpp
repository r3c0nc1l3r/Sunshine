/**
 * @file tests/unit/handoff/test_old_side.cpp
 * @brief Tests for the old side of the drain handoff (src/handoff/old_side.h).
 *
 * Written from `docs/drain-handoff.md` (steps 3 to 7, "Stopping, restarting and quitting", "App ownership and prep
 * commands") and the header only. A real channel pair connects the old side under test to a scripted peer (the test
 * body); `old_side_ops_t` and `child_ops_t` are recording fakes.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <cerrno>
  #include <chrono>
  #include <condition_variable>
  #include <cstddef>
  #include <cstdint>
  #include <deque>
  #include <functional>
  #include <future>
  #include <initializer_list>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <stdexcept>
  #include <string>
  #include <thread>
  #include <type_traits>
  #include <vector>

  // platform includes
  #include <fcntl.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <unistd.h>

  // local includes
  #include "src/handoff/old_side.h"

using namespace std::chrono_literals;

namespace {
  using handoff::control_reply_t;
  using handoff::old_outcome_e;
  using handoff::recv_status_e;
  using handoff::reply_kind_e;
  namespace protocol = handoff::protocol;

  /**
   * @brief Thread-safe ordered record of every fake call.
   */
  class call_log_t {
  public:
    /**
     * @brief Append an entry.
     *
     * @param entry Entry text.
     */
    void add(std::string entry) {
      std::lock_guard lock {_mutex};
      _entries.push_back(std::move(entry));
    }

    /**
     * @brief Copy of all entries.
     *
     * @return Entries in call order.
     */
    std::vector<std::string> all() const {
      std::lock_guard lock {_mutex};
      return _entries;
    }

  private:
    mutable std::mutex _mutex;  ///< Guards the entries.
    std::vector<std::string> _entries;  ///< Entries.
  };

  /**
   * @brief Index of the first entry equal to `name`, from `from`.
   *
   * @param v Entries.
   * @param name Entry to find.
   * @param from First index to look at.
   * @return Index, or -1.
   */
  int index_of(const std::vector<std::string> &v, const std::string &name, std::size_t from = 0) {
    for (std::size_t i = from; i < v.size(); ++i) {
      if (v[i] == name) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  /**
   * @brief Index of the last entry equal to `name`.
   *
   * @param v Entries.
   * @param name Entry to find.
   * @return Index, or -1.
   */
  int last_index_of(const std::vector<std::string> &v, const std::string &name) {
    for (std::size_t i = v.size(); i > 0; --i) {
      if (v[i - 1] == name) {
        return static_cast<int>(i - 1);
      }
    }
    return -1;
  }

  /**
   * @brief Number of entries equal to `name`.
   *
   * @param v Entries.
   * @param name Entry to count.
   * @return Count.
   */
  int count_of(const std::vector<std::string> &v, const std::string &name) {
    return static_cast<int>(std::count(v.begin(), v.end(), name));
  }

  /**
   * @brief Wire-like name of a reply kind.
   *
   * @param kind Kind.
   * @return Name.
   */
  std::string kind_name(reply_kind_e kind) {
    switch (kind) {
      case reply_kind_e::refused:
        return "refused";
      case reply_kind_e::failed:
        return "failed";
      case reply_kind_e::kill_pending:
        return "kill_pending";
      case reply_kind_e::committed:
        return "committed";
    }
    return "?";
  }

  constexpr int test_new_base = 51000;  ///< UDP base handed to the old side.
  constexpr pid_t test_child_pid = 4242;  ///< Fake child pid.
  constexpr std::uint64_t test_child_start_time = 987654321;  ///< Fake child start time.

  /**
   * @brief Recording fake of the process effects.
   */
  class fake_old_ops_t: public handoff::old_side_ops_t {
  public:
    /**
     * @brief Create the fake.
     *
     * @param log Shared call log.
     */
    explicit fake_old_ops_t(call_log_t &log):
        log(log) {
    }

    call_log_t &log;  ///< Call log.
    protocol::hello_t hello_value;  ///< Returned by `hello()`.
    int pidfd_value {-1};  ///< Returned by `self_pidfd()`.
    bool in_flight_ok {true};  ///< Result of a bounded `wait_in_flight`.
    std::string pause_reason;  ///< Result of `pause_accept` (empty: all four confirmed).
    std::string recheck_reason;  ///< Result of `recheck`.
    bool save_ok {true};  ///< Result of `save_state`.
    std::atomic<bool> shutdown_flag {false};  ///< Result of `shutdown_requested`.
    bool reload_ok {true};  ///< Result of `reload_after_rollback`.
    protocol::go_t snapshot;  ///< Result of `take_snapshot`.
    bool snapshot_throws {false};  ///< When true, `take_snapshot` throws `std::runtime_error` (after it is logged).
    std::deque<bool> handover_results;  ///< Scripted `handover` results.
    bool handover_default {true};  ///< `handover` result once the script is used up.
    std::function<void(const std::string &)> hook;  ///< Called after each logged call, on the calling thread.
    std::vector<std::chrono::milliseconds> in_flight_args;  ///< Arguments of `wait_in_flight`.
    std::chrono::milliseconds recheck_arg {-5};  ///< Argument of `recheck`.
    std::chrono::milliseconds mdns_arg {-5};  ///< Argument of `withdraw_mdns`.

    protocol::hello_t hello() override {
      note("hello");
      return hello_value;
    }

    int self_pidfd() override {
      note("self_pidfd");
      return pidfd_value;
    }

    std::string pause_accept() override {
      note("pause_accept");
      return pause_reason;
    }

    void resume_accept() override {
      note("resume_accept");
    }

    void raise_gate() override {
      note("raise_gate");
    }

    void lower_gate() override {
      note("lower_gate");
    }

    bool wait_in_flight(std::chrono::milliseconds timeout) override {
      {
        std::lock_guard lock {_mutex};
        in_flight_args.push_back(timeout);
      }
      note("wait_in_flight:" + std::to_string(timeout.count()));
      return timeout.count() < 0 ? true : in_flight_ok;
    }

    std::string recheck(std::chrono::milliseconds pairing_wait) override {
      recheck_arg = pairing_wait;
      note("recheck");
      return recheck_reason;
    }

    bool save_state() override {
      note("save_state");
      return save_ok;
    }

    void freeze() override {
      note("freeze");
    }

    void unfreeze() override {
      note("unfreeze");
    }

    void withdraw_mdns(std::chrono::milliseconds timeout) override {
      mdns_arg = timeout;
      note("withdraw_mdns");
    }

    void publish_mdns() override {
      note("publish_mdns");
    }

    void remove_tray() override {
      note("remove_tray");
    }

    void restore_tray() override {
      note("restore_tray");
    }

    protocol::go_t take_snapshot() override {
      note("take_snapshot");
      if (snapshot_throws) {
        throw std::runtime_error("snapshot failed");
      }
      return snapshot;
    }

    bool shutdown_requested() override {
      note("shutdown_requested");
      return shutdown_flag.load();
    }

    void set_released(bool released) override {
      note(released ? "set_released:1" : "set_released:0");
    }

    void drop_app() override {
      note("drop_app");
    }

    bool reload_after_rollback() override {
      note("reload_after_rollback");
      return reload_ok;
    }

    void enter_drain_mode() override {
      note("enter_drain_mode");
    }

    void leave_drain_mode() override {
      note("leave_drain_mode");
    }

    void set_role(handoff::role_e role) override {
      note("set_role:" + std::string {handoff::to_string(role)});
    }

    void set_state(handoff::state_e state) override {
      note("set_state:" + std::string {handoff::to_string(state)});
    }

    void remove_new_instance_file() override {
      note("remove_new_instance_file");
    }

    void start_upnp_cleanup(int new_base) override {
      note("start_upnp_cleanup:" + std::to_string(new_base));
    }

    bool handover(pid_t new_pid) override {
      note("handover:" + std::to_string(new_pid));
      std::lock_guard lock {_mutex};
      if (!handover_results.empty()) {
        const bool result = handover_results.front();
        handover_results.pop_front();
        return result;
      }
      return handover_default;
    }

    void close_socket_copies() override {
      note("close_socket_copies");
    }

  private:
    /**
     * @brief Log a call and run the hook.
     *
     * @param name Entry.
     */
    void note(const std::string &name) {
      log.add(name);
      if (hook) {
        hook(name);
      }
    }

    std::mutex _mutex;  ///< Guards the scripted members.
  };

  /**
   * @brief Recording fake of the child.
   */
  class fake_child_t: public handoff::child_ops_t {
  public:
    /**
     * @brief Create the fake.
     *
     * @param log Shared call log.
     */
    explicit fake_child_t(call_log_t &log):
        log(log) {
    }

    call_log_t &log;  ///< Call log.
    std::atomic<int> timeouts_left {0};  ///< `kill_and_reap` calls that still return `timeout`.
    std::atomic<bool> exited_flag {false};  ///< Result of `exited()`.
    std::function<void()> on_kill;  ///< Called inside every `kill_and_reap`.

    handoff::reap_result_e pending_result {handoff::reap_result_e::timeout};  ///< Result while `timeouts_left` > 0.
    handoff::reap_result_e final_result {handoff::reap_result_e::reaped};  ///< Result once `timeouts_left` is used up.
    std::vector<std::chrono::milliseconds> kill_args;  ///< Arguments of `kill_and_reap` (run thread only).

    handoff::reap_result_e kill_and_reap(std::chrono::milliseconds timeout) override {
      kill_args.push_back(timeout);
      log.add("kill_and_reap");
      if (on_kill) {
        on_kill();
      }
      if (timeouts_left.load() > 0) {
        --timeouts_left;
        return pending_result;
      }
      return final_result;
    }

    bool exited() override {
      return exited_flag.load();
    }

    pid_t pid() override {
      return test_child_pid;
    }

    std::uint64_t start_time() override {
      return test_child_start_time;
    }
  };

  /**
   * @brief The scripted new process: the other end of the channel.
   */
  struct peer_t {
    /**
     * @brief Take the peer end.
     *
     * @param fd Fd of the peer end.
     */
    explicit peer_t(int fd):
        channel(fd) {
    }

    handoff::channel_t channel;  ///< Peer end.
    std::vector<std::uint64_t> pings;  ///< Sequence numbers of answered pings.
    int mains {0};  ///< `MAIN` messages seen by `pump`.
    std::vector<std::string> others;  ///< Types of other messages seen by `pump`.

    /**
     * @brief Send a message.
     *
     * @param message Message.
     * @param fd Fd to attach, or -1.
     * @return `true` when sent.
     */
    bool send(const protocol::message_t &message, int fd = -1) {
      return channel.send(message, fd);
    }

    /**
     * @brief Wait for the next message of type `T`, answering pings on the way.
     *
     * @tparam T Wanted message type.
     * @param limit Longest wait.
     * @param fd_out Receives the attached fd when not null; otherwise an attached fd is closed.
     * @return The message, or nothing (a failure is recorded).
     */
    template<typename T>
    std::optional<T> expect(std::chrono::milliseconds limit = 800ms, int *fd_out = nullptr) {
      const std::string wanted = protocol::type_name(protocol::message_t {T {}});
      const auto deadline = std::chrono::steady_clock::now() + limit;
      for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          ADD_FAILURE() << "peer: timed out waiting for " << wanted << " from the old side";
          return std::nullopt;
        }
        handoff::received_t r;
        const auto status = channel.receive(r, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        if (status == recv_status_e::timeout) {
          continue;
        }
        if (status != recv_status_e::ok) {
          ADD_FAILURE() << "peer: receive status " << static_cast<int>(status) << " while waiting for " << wanted
                        << " (the old side closed the channel or sent garbage)";
          return std::nullopt;
        }
        if (const auto *ping = std::get_if<protocol::ping_t>(&r.message); ping != nullptr && !std::is_same_v<T, protocol::ping_t>) {
          channel.send(protocol::pong_t {ping->seq});
          if (r.fd >= 0) {
            ::close(r.fd);
          }
          continue;
        }
        if (const auto *m = std::get_if<T>(&r.message)) {
          T out = *m;
          if (fd_out != nullptr) {
            *fd_out = r.fd;
          } else if (r.fd >= 0) {
            ::close(r.fd);
          }
          return out;
        }
        ADD_FAILURE() << "peer: got " << protocol::type_name(r.message) << " while waiting for " << wanted;
        if (r.fd >= 0) {
          ::close(r.fd);
        }
        return std::nullopt;
      }
    }

    /**
     * @brief How a `pump` ended.
     */
    enum class pump_end_e {
      main,  ///< `MAIN` received.
      eof,  ///< Channel closed or failed.
      timeout,  ///< Time ran out.
    };

    /**
     * @brief Answer pings until `MAIN`, end of channel or the time limit.
     *
     * @param total Longest time.
     * @return How it ended.
     */
    pump_end_e pump(std::chrono::milliseconds total) {
      const auto deadline = std::chrono::steady_clock::now() + total;
      for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          return pump_end_e::timeout;
        }
        handoff::received_t r;
        const auto status = channel.receive(r, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        if (status == recv_status_e::timeout) {
          continue;
        }
        if (status != recv_status_e::ok) {
          return pump_end_e::eof;
        }
        if (r.fd >= 0) {
          ::close(r.fd);
        }
        if (const auto *ping = std::get_if<protocol::ping_t>(&r.message)) {
          pings.push_back(ping->seq);
          channel.send(protocol::pong_t {ping->seq});
        } else if (std::holds_alternative<protocol::main_t>(r.message)) {
          ++mains;
          return pump_end_e::main;
        } else {
          others.push_back(protocol::type_name(r.message));
        }
      }
    }

    /**
     * @brief Read and collect message type names without answering, until the channel ends or time runs out.
     *
     * @param total Longest time.
     * @return Type names in order.
     */
    std::vector<std::string> collect(std::chrono::milliseconds total) {
      std::vector<std::string> names;
      const auto deadline = std::chrono::steady_clock::now() + total;
      for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          return names;
        }
        handoff::received_t r;
        const auto status = channel.receive(r, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        if (status == recv_status_e::timeout) {
          continue;
        }
        if (status != recv_status_e::ok) {
          return names;
        }
        if (r.fd >= 0) {
          ::close(r.fd);
        }
        names.push_back(protocol::type_name(r.message));
      }
    }
  };

  /**
   * @brief Build the default `GO` snapshot: a running placebo app.
   *
   * @return Snapshot.
   */
  protocol::go_t make_snapshot() {
    protocol::go_t go;
    protocol::app_snapshot_t app;
    app.app_id = 3;
    app.name = "Desktop";
    app.applied_prep = 2;
    app.digest = std::string(64, 'a');
    app.launch_env = {{"SUNSHINE_CLIENT_ID", "7"}};
    go.app = app;
    go.host_audio = true;
    go.next_launch_session_id = 42;
    return go;
  }

  /**
   * @brief The ten switch substeps that `old_side_t` performs before `GO`, in order, as logged by the fake.
   *
   * `wait_in_flight:100` relies on the fixture's `in_flight` timeout of 100 ms.
   */
  const std::vector<std::string> substeps = {
    "pause_accept",
    "raise_gate",
    "wait_in_flight:100",
    "recheck",
    "save_state",
    "freeze",
    "withdraw_mdns",
    "remove_tray",
    "take_snapshot",
    "set_released:1",
  };

  /**
   * @brief Whether a log entry is a rollback effect that must wait for the reap.
   *
   * @param entry Entry.
   * @return `true` for effects guarded by the kill-first rule.
   */
  bool is_guarded(const std::string &entry) {
    static const std::vector<std::string> guarded = {
      "resume_accept",
      "publish_mdns",
      "restore_tray",
      "lower_gate",
      "set_released:0",
      "drop_app",
      "reload_after_rollback",
      "unfreeze",
      "remove_new_instance_file",
      "leave_drain_mode",
      "wait_in_flight:-1",
      "set_role:active",
      "set_state:none",
    };
    return std::find(guarded.begin(), guarded.end(), entry) != guarded.end() || entry.rfind("start_upnp_cleanup", 0) == 0;
  }

  /**
   * @brief Answer entries of the helper in a log.
   *
   * @param v Log.
   * @return Entries starting with `answer:`.
   */
  std::vector<std::string> answers_of(const std::vector<std::string> &v) {
    std::vector<std::string> out;
    for (const auto &e : v) {
      if (e.rfind("answer:", 0) == 0) {
        out.push_back(e);
      }
    }
    return out;
  }

  /**
   * @brief Fixture: old side under test, fakes, real channel and a scripted peer.
   */
  class HandoffOldSideTest: public testing::Test {
  protected:
    void SetUp() override {
      const auto pair = handoff::make_channel_pair();
      ASSERT_TRUE(pair.has_value()) << "make_channel_pair failed";
      old_channel = std::make_unique<handoff::channel_t>(pair->first);
      peer = std::make_unique<peer_t>(pair->second);

      int fds[2] = {-1, -1};
      ASSERT_EQ(0, ::pipe2(fds, O_CLOEXEC));
      pipe_read = fds[0];
      ops.pidfd_value = fds[1];  // the side owns and closes it
      ops.hello_value.capabilities = {"test-cap"};
      ops.hello_value.probe = protocol::probe_results_t {2, 1, true};
      ops.hello_value.cooldown_deadline_ns = 123456789;
      ops.snapshot = make_snapshot();

      timeouts.ready = 300ms;
      timeouts.in_flight = 100ms;
      timeouts.pairing = 100ms;
      timeouts.mdns_withdraw = 50ms;
      timeouts.go_ack = 300ms;
      timeouts.reap = 50ms;
      timeouts.ping_interval = 20ms;
      timeouts.misses_to_hang = 3;
      timeouts.rollback_window = 150ms;
      timeouts.handover_retry = 40ms;
    }

    void TearDown() override {
      if (worker.joinable()) {
        peer->channel.close();
        if (side) {
          side->request_shutdown();
        }
        worker.join();
      }
      if (pipe_read >= 0) {
        ::close(pipe_read);
      }
    }

    /**
     * @brief Create the side and start `run()` on its own thread.
     *
     * @param shutdown_first Call `request_shutdown()` before `run()` starts.
     */
    void start(bool shutdown_first = false) {
      side = std::make_unique<handoff::old_side_t>(ops, child, *old_channel, end_all, timeouts, test_new_base, [this](const control_reply_t &reply) {
        {
          std::lock_guard lock {replies_mutex};
          replies.push_back(reply);
        }
        log.add("answer:" + kind_name(reply.kind));
      });
      if (shutdown_first) {
        side->request_shutdown();
      }
      future = promise.get_future();
      worker = std::thread([this]() {
        const auto outcome = side->run([this](handoff::received_t r) {
          if (r.fd >= 0) {
            ::close(r.fd);
          }
          std::unique_lock lock {handler_mutex};
          handled.push_back(protocol::type_name(r.message));
          handler_cv.notify_all();
          if (handler_blocks) {
            handler_cv.wait_for(lock, 700ms, [this]() {
              return handler_release;
            });
          }
        });
        promise.set_value(outcome);
      });
    }

    /**
     * @brief Wait for `run()` to return.
     *
     * @param limit Longest wait.
     * @return The outcome, or nothing when `run()` did not return in time.
     */
    std::optional<old_outcome_e> finish(std::chrono::milliseconds limit = 1500ms) {
      if (!worker.joinable()) {
        return std::nullopt;
      }
      if (future.wait_for(limit) != std::future_status::ready) {
        ADD_FAILURE() << "old_side_t::run() did not return within " << limit.count() << " ms";
        peer->channel.close();
        side->request_shutdown();
        future.wait_for(1000ms);
      }
      worker.join();
      return future.get();
    }

    /**
     * @brief Peer: receive the old `HELLO` (with its fd) and answer with a `HELLO`.
     *
     * @param major Major version of the peer's `HELLO`.
     * @return `true` when the exchange worked.
     */
    bool handshake(std::uint32_t major = protocol::version_major) {
      int fd = -1;
      const auto hello = peer->expect<protocol::hello_t>(800ms, &fd);
      if (!hello) {
        return false;
      }
      EXPECT_GE(fd, 0) << "the old side's HELLO must carry its pidfd (SCM_RIGHTS)";
      if (fd >= 0) {
        ::close(fd);
      }
      EXPECT_EQ(ops.hello_value, *hello) << "the first message must be the old side's own hello()";
      protocol::hello_t mine;
      mine.major = major;
      return peer->send(mine);
    }

    /**
     * @brief Peer: handshake, then `READY` ok.
     *
     * @return `true` when it worked.
     */
    bool to_ready() {
      return handshake() && peer->send(protocol::ready_t {true, ""});
    }

    /**
     * @brief Peer: up to and including receiving `GO`.
     *
     * @return The `GO`, or nothing.
     */
    std::optional<protocol::go_t> to_go() {
      if (!to_ready()) {
        return std::nullopt;
      }
      return peer->expect<protocol::go_t>();
    }

    /**
     * @brief Peer: up to and including receiving `COMMIT`.
     *
     * @return `true` when it worked.
     */
    bool to_committed() {
      if (!to_go()) {
        return false;
      }
      if (!peer->send(protocol::go_ack_t {true, ""})) {
        return false;
      }
      return peer->expect<protocol::commit_t>().has_value();
    }

    /**
     * @brief Check a finished rollback against the rules of the spec.
     *
     * @param reached Number of switch substeps that must have happened (and nothing beyond).
     * @param committed Whether `COMMIT` was sent.
     * @param tail Expected calls after the last `kill_and_reap` (without `set_state:none`), in any order.
     * @param expected_answers Expected helper answers over the whole run.
     */
    void verify_rollback(std::size_t reached, bool committed, std::vector<std::string> tail, const std::vector<std::string> &expected_answers) {
      const auto v = log.all();

      std::vector<std::string> seen;
      for (const auto &e : v) {
        if (std::find(substeps.begin(), substeps.end(), e) != substeps.end()) {
          seen.push_back(e);
        }
      }
      const std::vector<std::string> want(substeps.begin(), substeps.begin() + static_cast<std::ptrdiff_t>(reached));
      EXPECT_EQ(want, seen) << "wrong switch substeps happened before the rollback (only the first " << reached << " may run, in order)";

      const int last_kill = last_index_of(v, "kill_and_reap");
      ASSERT_GE(last_kill, 0) << "a rollback must call kill_and_reap";
      for (int i = 0; i < last_kill; ++i) {
        EXPECT_FALSE(is_guarded(v[i])) << "'" << v[i] << "' ran at position " << i << " before the last kill_and_reap at " << last_kill
                                       << " (kill-first rule: nothing of the rollback may run before the child is reaped)";
      }

      std::vector<std::string> actual_tail;
      for (std::size_t i = static_cast<std::size_t>(last_kill) + 1; i < v.size(); ++i) {
        if (v[i] != "set_state:none") {
          actual_tail.push_back(v[i]);
        }
      }
      std::sort(actual_tail.begin(), actual_tail.end());
      std::sort(tail.begin(), tail.end());
      EXPECT_EQ(tail, actual_tail) << "calls after the reap are not exactly the undo of the steps that happened";

      EXPECT_EQ(expected_answers, answers_of(v)) << "the helper must be answered exactly once, with the right kind";

      if (!committed) {
        for (const auto *never : {"set_role:draining", "set_state:draining", "enter_drain_mode", "answer:committed", "close_socket_copies"}) {
          EXPECT_EQ(0, count_of(v, never)) << never << " must not happen before COMMIT";
        }
        EXPECT_EQ(0, count_of(v, "handover:" + std::to_string(test_child_pid))) << "no handover before COMMIT";
      }

      const int answer_failed = index_of(v, "answer:failed");
      if (answer_failed >= 0) {
        EXPECT_GT(answer_failed, last_kill) << "the helper may be answered only after the kill-first step completed";
        for (const auto *later : {"wait_in_flight:-1", "reload_after_rollback", "unfreeze", "set_released:0", "drop_app", "lower_gate", "resume_accept", "publish_mdns", "restore_tray"}) {
          const int i = index_of(v, later, static_cast<std::size_t>(last_kill));
          if (i >= 0) {
            EXPECT_LT(answer_failed, i) << "the helper must be answered before '" << later << "' (before the in-flight wait, reload and swap)";
          }
        }
      }

      std::lock_guard lock {replies_mutex};
      for (const auto &reply : replies) {
        if (reply.kind == reply_kind_e::failed) {
          EXPECT_FALSE(reply.reason.empty()) << "a failed answer needs a reason";
        }
      }
    }

    /// Expected tail of a rollback of a failed start (before the first switch substep).
    static std::vector<std::string> start_tail() {
      return {"answer:failed", "remove_new_instance_file"};
    }

    /// Expected tail when the gate was raised but nothing was frozen (no wait_in_flight(-1): it only runs when frozen).
    static std::vector<std::string> gate_tail() {
      return {"answer:failed", "remove_new_instance_file", "lower_gate", "resume_accept"};
    }

    /// Expected tail when the state was frozen and mDNS withdrawn but the app was not released.
    static std::vector<std::string> withdrawn_tail() {
      return {"answer:failed", "wait_in_flight:-1", "reload_after_rollback", "unfreeze", "remove_new_instance_file", "lower_gate", "resume_accept", "publish_mdns", "restore_tray"};
    }

    /// Expected tail when `GO` was sent (app released) and the helper was not yet answered.
    static std::vector<std::string> released_tail() {
      auto t = withdrawn_tail();
      t.push_back("set_released:0");
      return t;
    }

    /// Expected tail of a rollback after `COMMIT` (helper already answered `committed`).
    static std::vector<std::string> committed_tail() {
      auto t = released_tail();
      t.erase(t.begin());  // no answer:failed
      t.push_back("start_upnp_cleanup:" + std::to_string(test_new_base));
      t.push_back("leave_drain_mode");
      t.push_back("set_role:active");
      return t;
    }

    call_log_t log;  ///< Call log.
    fake_old_ops_t ops {log};  ///< Fake effects.
    fake_child_t child {log};  ///< Fake child.
    handoff::end_all_machine_t end_all;  ///< Shared END_ALL machine.
    handoff::old_side_timeouts_t timeouts;  ///< Timeouts.
    std::unique_ptr<handoff::channel_t> old_channel;  ///< Old side's end.
    std::unique_ptr<peer_t> peer;  ///< Scripted peer.
    std::unique_ptr<handoff::old_side_t> side;  ///< Object under test.
    std::thread worker;  ///< Thread running `run()`.
    std::promise<old_outcome_e> promise;  ///< Outcome promise.
    std::future<old_outcome_e> future;  ///< Outcome future.
    int pipe_read {-1};  ///< Read end of the pidfd stand-in pipe.
    std::mutex replies_mutex;  ///< Guards `replies`.
    std::vector<control_reply_t> replies;  ///< Helper answers.
    std::mutex handler_mutex;  ///< Guards the drain handler state.
    std::condition_variable handler_cv;  ///< Signals handler calls.
    std::vector<std::string> handled;  ///< Types passed to the drain handler.
    bool handler_blocks {false};  ///< Whether the drain handler blocks until released.
    bool handler_release {false};  ///< Releases a blocked handler.
  };
}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------------------------------------------

TEST_F(HandoffOldSideTest, HappyPathCallOrder) {
  start();
  ASSERT_TRUE(handshake());
  ASSERT_TRUE(peer->send(protocol::ready_t {true, ""}));
  const auto go = peer->expect<protocol::go_t>();
  ASSERT_TRUE(go.has_value());
  {
    const auto v = log.all();
    EXPECT_GE(index_of(v, "set_released:1"), 0) << "set_released(true) must happen before GO is sent";
    EXPECT_GE(index_of(v, "take_snapshot"), 0) << "take_snapshot() must happen before GO is sent";
    EXPECT_EQ(0, count_of(v, "answer:committed")) << "the helper must not hear 'committed' before GO_ACK and COMMIT";
  }
  ASSERT_TRUE(peer->send(protocol::go_ack_t {true, ""}));
  ASSERT_TRUE(peer->expect<protocol::commit_t>().has_value()) << "COMMIT must follow GO_ACK ok";
  ASSERT_EQ(peer->pump(1000ms), peer_t::pump_end_e::main) << "MAIN must be sent after the rollback window";
  {
    const auto v = log.all();
    EXPECT_GE(index_of(v, "handover:" + std::to_string(test_child_pid)), 0) << "handover must have run before MAIN reaches the peer";
    EXPECT_GE(index_of(v, "close_socket_copies"), 0) << "close_socket_copies() must run before MAIN is sent";
  }
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::handed_over, *outcome);

  std::vector<std::string> filtered;
  for (const auto &e : log.all()) {
    if (e != "hello" && e != "self_pidfd") {
      filtered.push_back(e);
    }
  }
  const std::vector<std::string> expected = {
    "pause_accept",
    "raise_gate",
    "wait_in_flight:100",
    "recheck",
    "save_state",
    "freeze",
    "withdraw_mdns",
    "remove_tray",
    "take_snapshot",
    "shutdown_requested",
    "set_released:1",
    "set_role:draining",
    "set_state:draining",
    "enter_drain_mode",
    "answer:committed",
    "handover:4242",
    "close_socket_copies",
  };
  EXPECT_EQ(expected, filtered) << "the ops calls of a successful handoff must follow substeps 1-10, then the post-COMMIT calls, then the handover, in exactly this order";

  EXPECT_EQ(timeouts.pairing, ops.recheck_arg) << "recheck() must get the pairing timeout";
  EXPECT_EQ(timeouts.mdns_withdraw, ops.mdns_arg) << "withdraw_mdns() must get the mdns_withdraw timeout";
  ASSERT_EQ(1U, ops.in_flight_args.size());
  EXPECT_EQ(timeouts.in_flight, ops.in_flight_args[0]) << "wait_in_flight() must get the in_flight timeout in the switch";
}

TEST_F(HandoffOldSideTest, HelloIsFirstMessageWithPidfd) {
  start();
  int fd = -1;
  const auto hello = peer->expect<protocol::hello_t>(800ms, &fd);
  ASSERT_TRUE(hello.has_value()) << "the first message from the old side must be HELLO";
  EXPECT_EQ(ops.hello_value, *hello) << "HELLO must be the value of old_side_ops_t::hello()";
  ASSERT_GE(fd, 0) << "HELLO must carry a pidfd (SCM_RIGHTS)";
  EXPECT_NE(-1, ::fcntl(fd, F_GETFD)) << "the received fd must be valid";
  ::close(fd);
  peer->channel.close();
  finish();
}

TEST_F(HandoffOldSideTest, OwnPidfdIsClosedAfterSending) {
  start();
  ASSERT_TRUE(handshake());  // the peer closes its received copy
  // The pipe write end was the pidfd stand-in; once the side closed its copy the read end sees a hang-up.
  pollfd pfd {pipe_read, POLLIN, 0};
  const int rc = ::poll(&pfd, 1, 500);
  EXPECT_EQ(1, rc) << "the old side must close the fd it got from self_pidfd() after sending HELLO";
  if (rc == 1) {
    EXPECT_TRUE((pfd.revents & POLLHUP) != 0) << "the stand-in pipe must report hang-up once the old side closed its pidfd";
  }
  peer->channel.close();
  finish();
}

TEST_F(HandoffOldSideTest, GoCarriesTakeSnapshotValue) {
  start();
  const auto go = to_go();
  ASSERT_TRUE(go.has_value());
  EXPECT_EQ(ops.snapshot, *go) << "GO must carry exactly what take_snapshot() returned";
  peer->channel.close();
  finish();
}

TEST_F(HandoffOldSideTest, GoCarriesEmptySnapshot) {
  ops.snapshot = protocol::go_t {};
  ops.snapshot.next_launch_session_id = 9;
  start();
  const auto go = to_go();
  ASSERT_TRUE(go.has_value());
  EXPECT_FALSE(go->app.has_value()) << "no app in the snapshot means no app in GO";
  EXPECT_EQ(9U, go->next_launch_session_id);
  peer->channel.close();
  finish();
}

TEST_F(HandoffOldSideTest, NoCommitBeforeGoAck) {
  start();
  ASSERT_TRUE(to_go().has_value());
  const auto names = peer->collect(80ms);
  EXPECT_EQ(0, count_of(names, "COMMIT")) << "COMMIT must not be sent before GO_ACK";
  EXPECT_EQ(0, count_of(log.all(), "answer:committed")) << "the helper must not hear 'committed' before GO_ACK";
  peer->channel.close();
  finish();
}

TEST_F(HandoffOldSideTest, HelperGetsCommittedWithChildPidAndStartTimeExactlyOnce) {
  start();
  ASSERT_TRUE(to_committed());
  ASSERT_EQ(peer->pump(1000ms), peer_t::pump_end_e::main);
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::handed_over, *outcome);
  std::lock_guard lock {replies_mutex};
  ASSERT_EQ(1U, replies.size()) << "the helper must be answered exactly once";
  EXPECT_EQ(reply_kind_e::committed, replies[0].kind);
  EXPECT_EQ(test_child_pid, replies[0].new_pid) << "committed must carry the child pid";
  EXPECT_EQ(test_child_start_time, replies[0].new_start_time) << "committed must carry the child start time";
}

TEST_F(HandoffOldSideTest, PostCommitCallsFollowCommitInOrder) {
  start();
  ASSERT_TRUE(to_committed());
  ASSERT_EQ(peer->pump(1000ms), peer_t::pump_end_e::main);
  finish();
  const auto v = log.all();
  const int role = index_of(v, "set_role:draining");
  const int state = index_of(v, "set_state:draining");
  const int drain = index_of(v, "enter_drain_mode");
  const int answer = index_of(v, "answer:committed");
  ASSERT_GE(role, 0);
  ASSERT_GE(state, 0);
  ASSERT_GE(drain, 0);
  ASSERT_GE(answer, 0);
  EXPECT_LT(role, state) << "set_role(draining) comes before set_state(draining)";
  EXPECT_LT(state, drain) << "set_state(draining) comes before enter_drain_mode()";
  EXPECT_LT(drain, answer) << "the helper is answered after enter_drain_mode()";
  EXPECT_LT(index_of(v, "set_released:1"), role) << "the release flag is set before COMMIT effects";
}

TEST_F(HandoffOldSideTest, PongsKeepTheChildAliveThroughTheWindow) {
  timeouts.rollback_window = 250ms;
  start();
  ASSERT_TRUE(to_committed());
  ASSERT_EQ(peer->pump(1500ms), peer_t::pump_end_e::main) << "answered PINGs must keep the handoff going until MAIN";
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::handed_over, *outcome);
  EXPECT_GE(peer->pings.size(), 4U) << "PING must be sent every ping_interval (20 ms) in the rollback window of 250 ms";
  for (std::size_t i = 1; i < peer->pings.size(); ++i) {
    EXPECT_LT(peer->pings[i - 1], peer->pings[i]) << "PING sequence numbers must increase";
  }
  EXPECT_EQ(0, count_of(log.all(), "kill_and_reap")) << "a live, answering child must not be killed";
}

TEST_F(HandoffOldSideTest, HandoverHappensOnlyAfterTheRollbackWindow) {
  timeouts.rollback_window = 250ms;
  std::atomic<long long> commit_done_ns {0};
  std::atomic<long long> handover_ns {0};
  ops.hook = [&](const std::string &name) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    if (name == "enter_drain_mode") {
      commit_done_ns = now;
    } else if (name.rfind("handover:", 0) == 0) {
      handover_ns = now;
    }
  };
  start();
  ASSERT_TRUE(to_committed());
  ASSERT_EQ(peer->pump(1500ms), peer_t::pump_end_e::main);
  finish();
  ASSERT_NE(0, commit_done_ns.load());
  ASSERT_NE(0, handover_ns.load());
  const auto gap = std::chrono::nanoseconds(handover_ns.load() - commit_done_ns.load());
  EXPECT_GE(gap, 200ms) << "handover() must not run before rollback_window (250 ms) has passed since COMMIT";
}

TEST_F(HandoffOldSideTest, HandoverFailureThenSuccessSetsStatesAndSendsMainOnce) {
  timeouts.rollback_window = 100ms;
  timeouts.handover_retry = 40ms;
  ops.handover_results = {false, true};
  start();
  ASSERT_TRUE(to_committed());
  ASSERT_EQ(peer->pump(1500ms), peer_t::pump_end_e::main);
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::handed_over, *outcome);
  const auto v = log.all();
  const std::string handover = "handover:" + std::to_string(test_child_pid);
  EXPECT_EQ(2, count_of(v, handover)) << "handover must be retried after a failure";
  const int first = index_of(v, handover);
  const int failed = index_of(v, "set_state:handover_failed");
  ASSERT_GE(first, 0);
  ASSERT_GE(failed, 0) << "a failed handover must set the state handover_failed";
  EXPECT_LT(first, failed);
  const int second = index_of(v, handover, static_cast<std::size_t>(first) + 1);
  ASSERT_GE(second, 0);
  EXPECT_LT(failed, second);
  const int back = index_of(v, "set_state:draining", static_cast<std::size_t>(failed));
  EXPECT_GT(back, failed) << "a success after failures must set the state back to draining";
  EXPECT_LT(second, back);
  EXPECT_EQ(1, count_of(v, "close_socket_copies")) << "the socket copies are closed once, after the successful handover";
  EXPECT_GT(index_of(v, "close_socket_copies"), second) << "no close_socket_copies before the handover succeeds";
  EXPECT_EQ(1, peer->mains);
  EXPECT_EQ(0, count_of(peer->collect(100ms), "MAIN")) << "MAIN must be sent only once";
  EXPECT_EQ(0, count_of(v, "kill_and_reap"));
  EXPECT_GE(peer->pings.size(), 3U) << "PINGs continue while the handover is retried";
}

TEST_F(HandoffOldSideTest, HandoverFailureThenDeadChildRollsBackKillFirst) {
  timeouts.rollback_window = 80ms;
  timeouts.handover_retry = 40ms;
  ops.handover_default = false;
  ops.hook = [this](const std::string &name) {
    if (name == "set_state:handover_failed") {
      child.exited_flag = true;
    }
  };
  start();
  ASSERT_TRUE(to_committed());
  peer->pump(400ms);
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome) << "a dead child while the handover keeps failing is a rollback";
  EXPECT_GE(count_of(log.all(), "set_state:handover_failed"), 1);
  verify_rollback(10, true, committed_tail(), {"answer:committed"});
  const auto v = log.all();
  EXPECT_EQ("set_state:none", v.back()) << "the rollback ends with set_state(none)";
}

// ---------------------------------------------------------------------------------------------------------------
// Rollback paths: kill-first and only-what-happened
// ---------------------------------------------------------------------------------------------------------------

TEST_F(HandoffOldSideTest, FailedReadyRollsBackKillFirst) {
  start();
  ASSERT_TRUE(handshake());
  ASSERT_TRUE(peer->send(protocol::ready_t {false, "port mismatch"}));
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  verify_rollback(0, false, start_tail(), {"answer:failed"});
}

TEST_F(HandoffOldSideTest, ReadyTimeoutRollsBackKillFirst) {
  timeouts.ready = 100ms;
  start();
  ASSERT_TRUE(handshake());
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  verify_rollback(0, false, start_tail(), {"answer:failed"});
}

TEST_F(HandoffOldSideTest, NoHelloInTimeRollsBackKillFirst) {
  timeouts.ready = 100ms;
  start();
  ASSERT_TRUE(peer->expect<protocol::hello_t>().has_value());
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  verify_rollback(0, false, start_tail(), {"answer:failed"});
}

TEST_F(HandoffOldSideTest, EarlyCloseBeforeReadyRollsBackKillFirst) {
  start();
  ASSERT_TRUE(handshake());
  peer->channel.close();
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  verify_rollback(0, false, start_tail(), {"answer:failed"});
}

TEST_F(HandoffOldSideTest, EarlyCloseBeforeHelloRollsBackKillFirst) {
  start();
  ASSERT_TRUE(peer->expect<protocol::hello_t>().has_value());
  peer->channel.close();
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  verify_rollback(0, false, start_tail(), {"answer:failed"});
}

TEST_F(HandoffOldSideTest, MajorVersionMismatchInHelloRollsBackKillFirst) {
  start();
  ASSERT_TRUE(handshake(protocol::version_major + 1));
  peer->send(protocol::ready_t {true, ""});  // must not be honoured
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  verify_rollback(0, false, start_tail(), {"answer:failed"});
}

TEST_F(HandoffOldSideTest, MinorVersionDifferenceIsAccepted) {
  start();
  int fd = -1;
  ASSERT_TRUE(peer->expect<protocol::hello_t>(800ms, &fd).has_value());
  if (fd >= 0) {
    ::close(fd);
  }
  protocol::hello_t mine;
  mine.minor = protocol::version_minor + 5;
  mine.capabilities = {"unknown-capability"};
  ASSERT_TRUE(peer->send(mine));
  ASSERT_TRUE(peer->send(protocol::ready_t {true, ""}));
  EXPECT_TRUE(peer->expect<protocol::go_t>().has_value()) << "a different minor version and unknown capabilities must not fail the start";
  peer->channel.close();
  finish();
}

TEST_F(HandoffOldSideTest, SelfPidfdFailureFailsTheStartKillFirst) {
  ::close(ops.pidfd_value);
  ops.pidfd_value = -1;
  start();
  peer->collect(50ms);
  peer->channel.close();
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  verify_rollback(0, false, start_tail(), {"answer:failed"});
}

TEST_F(HandoffOldSideTest, PauseReasonFailsTheSwitchAndCarriesTheReason) {
  ops.pause_reason = "the http server did not confirm the pause in time";
  start();
  ASSERT_TRUE(to_ready());
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome) << "a non-empty pause_accept() reason must fail the switch (rollback)";
  verify_rollback(1, false, {"answer:failed", "remove_new_instance_file", "resume_accept"}, {"answer:failed"});
  std::lock_guard lock {replies_mutex};
  ASSERT_EQ(1u, replies.size()) << "the helper must be answered exactly once";
  EXPECT_EQ(reply_kind_e::failed, replies[0].kind) << "a failed pause is a failure (helper exit 2), not a refusal";
  EXPECT_NE(std::string::npos, replies[0].reason.find(ops.pause_reason)) << "the helper's reason must carry the pause_accept() reason; got: " << replies[0].reason;
}

TEST_F(HandoffOldSideTest, PauseReasonStopsTheSwitchBeforeTheGate) {
  ops.pause_reason = "the rtsp server did not confirm the pause in time";
  start();
  ASSERT_TRUE(to_ready());
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  const auto v = log.all();
  for (const auto *later : {"raise_gate", "recheck", "save_state", "freeze", "withdraw_mdns", "remove_tray", "take_snapshot", "set_released:1"}) {
    EXPECT_EQ(0, count_of(v, later)) << later << " must not run after a failed pause_accept()";
  }
  EXPECT_EQ(0, count_of(v, "answer:committed")) << "a failed pause never commits";
  EXPECT_EQ(0, count_of(v, "enter_drain_mode")) << "a failed pause never drains";
}

TEST_F(HandoffOldSideTest, EmptyPauseReasonLetsTheSwitchContinue) {
  // Control for the two tests above: an empty reason gets past substep 1.
  ops.pause_reason.clear();
  start();
  ASSERT_TRUE(to_ready());
  ASSERT_TRUE(peer->expect<protocol::go_t>().has_value()) << "an empty pause_accept() result must let the switch reach GO";
  EXPECT_EQ(1, count_of(log.all(), "raise_gate")) << "substep 2 must run after a successful pause";
  peer->channel.close();
  finish();
}

TEST_F(HandoffOldSideTest, WaitInFlightTimeoutRollsBackAfterGate) {
  ops.in_flight_ok = false;
  start();
  ASSERT_TRUE(to_ready());
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  EXPECT_EQ(0, count_of(log.all(), "wait_in_flight:-1")) << "wait_in_flight(-1) in a rollback runs only when the state was frozen";
  verify_rollback(3, false, gate_tail(), {"answer:failed"});
  EXPECT_EQ(0, count_of(log.all(), "reload_after_rollback")) << "a failure before the freeze reloads nothing";
  EXPECT_EQ(0, count_of(log.all(), "unfreeze")) << "a failure before the freeze does not unfreeze";
  EXPECT_EQ(0, count_of(log.all(), "publish_mdns")) << "mDNS was not withdrawn, so it is not republished";
  EXPECT_EQ(0, count_of(log.all(), "set_released:0")) << "the app was not released";
}

TEST_F(HandoffOldSideTest, RecheckReasonRollsBackAfterGate) {
  ops.recheck_reason = "pairing session still open";
  start();
  ASSERT_TRUE(to_ready());
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  verify_rollback(4, false, gate_tail(), {"answer:failed"});
  EXPECT_EQ(0, count_of(log.all(), "wait_in_flight:-1")) << "wait_in_flight(-1) in a rollback runs only when the state was frozen";
  EXPECT_EQ(0, count_of(log.all(), "reload_after_rollback")) << "a failure before the freeze reloads nothing";
  EXPECT_EQ(0, count_of(log.all(), "unfreeze"));
}

TEST_F(HandoffOldSideTest, SaveStateFailureRollsBackBeforeFreeze) {
  ops.save_ok = false;
  start();
  ASSERT_TRUE(to_ready());
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  verify_rollback(5, false, gate_tail(), {"answer:failed"});
  EXPECT_EQ(0, count_of(log.all(), "wait_in_flight:-1")) << "wait_in_flight(-1) in a rollback runs only when the state was frozen";
  EXPECT_EQ(0, count_of(log.all(), "freeze")) << "save_state failure comes before freeze()";
  EXPECT_EQ(0, count_of(log.all(), "unfreeze"));
  EXPECT_EQ(0, count_of(log.all(), "reload_after_rollback"));
}

TEST_F(HandoffOldSideTest, ShutdownRequestedAtSubstep9RollsBackKillFirst) {
  ops.shutdown_flag = true;
  start();
  ASSERT_TRUE(to_ready());
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  // SPEC-QUESTION: shutdown_requested() true at substep 9 aborts "with the kill-first rollback"; the outcome may be
  // rolled_back or shutdown. Both are accepted, handed_over is not.
  EXPECT_TRUE(*outcome == old_outcome_e::rolled_back || *outcome == old_outcome_e::shutdown) << "a shutdown at substep 9 must not hand over";
  verify_rollback(9, false, withdrawn_tail(), {"answer:failed"});
  EXPECT_EQ(0, count_of(log.all(), "set_released:1")) << "the release flag must not be set when the shutdown aborts the switch";
  EXPECT_EQ(0, count_of(log.all(), "set_released:0")) << "the app was never released";
  EXPECT_EQ(0, count_of(log.all(), "drop_app"));
}

TEST_F(HandoffOldSideTest, GoAckFailedRollsBackKillFirstAndTakesAppBack) {
  start();
  ASSERT_TRUE(to_go().has_value());
  ASSERT_TRUE(peer->send(protocol::go_ack_t {false, "digest mismatch"}));
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  verify_rollback(10, false, released_tail(), {"answer:failed"});
  EXPECT_EQ(0, count_of(log.all(), "start_upnp_cleanup:" + std::to_string(test_new_base))) << "no UPnP cleanup before COMMIT";
  EXPECT_EQ(0, count_of(log.all(), "leave_drain_mode")) << "drain mode was never entered";
  EXPECT_EQ(0, count_of(peer->collect(30ms), "COMMIT")) << "no COMMIT after a failed GO_ACK";
}

TEST_F(HandoffOldSideTest, GoAckTimeoutRollsBackKillFirst) {
  timeouts.go_ack = 100ms;
  start();
  ASSERT_TRUE(to_go().has_value());
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  verify_rollback(10, false, released_tail(), {"answer:failed"});
  EXPECT_EQ(0, count_of(peer->collect(30ms), "COMMIT")) << "no COMMIT after a GO_ACK timeout";
}

TEST_F(HandoffOldSideTest, GoAckAfterDeadlineGetsNoCommit) {
  timeouts.go_ack = 100ms;
  start();
  ASSERT_TRUE(to_go().has_value());
  std::this_thread::sleep_for(200ms);
  peer->send(protocol::go_ack_t {true, ""});
  const auto names = peer->collect(150ms);
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  EXPECT_EQ(0, count_of(names, "COMMIT")) << "a GO_ACK that arrives after the go_ack deadline counts as a timeout and gets no COMMIT";
  verify_rollback(10, false, released_tail(), {"answer:failed"});
}

TEST_F(HandoffOldSideTest, ChannelCloseAfterGoRollsBackKillFirst) {
  start();
  ASSERT_TRUE(to_go().has_value());
  peer->channel.close();
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  verify_rollback(10, false, released_tail(), {"answer:failed"});
}

TEST_F(HandoffOldSideTest, ChannelCloseRightAfterReadyRollsBackKillFirst) {
  start();
  ASSERT_TRUE(to_ready());
  peer->channel.close();
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  const auto v = log.all();
  const int kill = last_index_of(v, "kill_and_reap");
  ASSERT_GE(kill, 0) << "a channel close between READY and GO must kill the child";
  EXPECT_EQ(std::vector<std::string> {"answer:failed"}, answers_of(v)) << "the helper is answered failed exactly once";
  for (int i = 0; i < kill; ++i) {
    EXPECT_FALSE(is_guarded(v[i])) << "'" << v[i] << "' ran before kill_and_reap";
  }
  EXPECT_EQ(0, count_of(v, "answer:committed"));
}

TEST_F(HandoffOldSideTest, HungChildWithoutPongsRollsBackKillFirst) {
  timeouts.rollback_window = 900ms;
  start();
  ASSERT_TRUE(to_committed());
  // No PONG from here on.
  const auto names = peer->collect(300ms);
  EXPECT_GE(count_of(names, "PING"), 2) << "PINGs must be sent every ping_interval in the window";
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome) << "misses_to_hang (3) unanswered PINGs mean a hung child: kill and roll back";
  verify_rollback(10, true, committed_tail(), {"answer:committed"});
  EXPECT_EQ(0, count_of(log.all(), "handover:" + std::to_string(test_child_pid))) << "no handover for a hung child";
}

TEST_F(HandoffOldSideTest, ExitedChildInWindowRollsBackKillFirst) {
  timeouts.rollback_window = 120ms;
  ops.hook = [this](const std::string &name) {
    if (name == "enter_drain_mode") {
      child.exited_flag = true;
    }
  };
  start();
  ASSERT_TRUE(to_committed());
  peer->pump(300ms);
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome) << "exited() true in the window is a dead child: rollback";
  verify_rollback(10, true, committed_tail(), {"answer:committed"});
  EXPECT_EQ(0, count_of(log.all(), "handover:" + std::to_string(test_child_pid))) << "no handover (not even an attempt) for an exited child";
}

TEST_F(HandoffOldSideTest, ExitedChildAtWindowEndIsNotHandedOver) {
  timeouts.rollback_window = 60ms;
  ops.hook = [this](const std::string &name) {
    if (name == "enter_drain_mode") {
      child.exited_flag = true;
    }
  };
  start();
  ASSERT_TRUE(to_committed());
  peer->pump(300ms);
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_NE(old_outcome_e::handed_over, *outcome);
  EXPECT_EQ(0, count_of(log.all(), "close_socket_copies")) << "the socket copies must stay open for an exited child";
  EXPECT_EQ(0, peer->mains) << "no MAIN for an exited child";
}

TEST_F(HandoffOldSideTest, ChannelEofInWindowRollsBackKillFirst) {
  timeouts.rollback_window = 900ms;
  start();
  ASSERT_TRUE(to_committed());
  peer->channel.close();
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome) << "end of file on the channel before MAIN is a hung child at once";
  verify_rollback(10, true, committed_tail(), {"answer:committed"});
}

TEST_F(HandoffOldSideTest, MalformedMessageInWindowRollsBackKillFirst) {
  timeouts.rollback_window = 900ms;
  start();
  ASSERT_TRUE(to_committed());
  const char garbage[] = "{not json";
  ASSERT_EQ(static_cast<ssize_t>(sizeof(garbage) - 1), ::send(peer->channel.fd(), garbage, sizeof(garbage) - 1, MSG_NOSIGNAL));
  peer->collect(150ms);
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome) << "garbage on the channel before MAIN makes the old side roll back";
  verify_rollback(10, true, committed_tail(), {"answer:committed"});
}

TEST_F(HandoffOldSideTest, RollbackAfterCommitCleansUpUpnpOfNewBase) {
  timeouts.rollback_window = 900ms;
  start();
  ASSERT_TRUE(to_committed());
  peer->channel.close();
  finish();
  const auto v = log.all();
  EXPECT_EQ(1, count_of(v, "start_upnp_cleanup:" + std::to_string(test_new_base))) << "after COMMIT a rollback starts the UPnP cleanup of the new base";
  EXPECT_GT(index_of(v, "start_upnp_cleanup:" + std::to_string(test_new_base)), last_index_of(v, "kill_and_reap"));
  EXPECT_EQ(1, count_of(v, "leave_drain_mode"));
  EXPECT_GE(index_of(v, "set_role:active"), 0);
  EXPECT_EQ("set_state:none", v.back());
}

TEST_F(HandoffOldSideTest, RollbackStepsFollowTheDocumentedOrder) {
  timeouts.rollback_window = 900ms;
  start();
  ASSERT_TRUE(to_committed());
  peer->channel.close();
  finish();
  const auto v = log.all();
  const int kill = last_index_of(v, "kill_and_reap");
  const int wait = index_of(v, "wait_in_flight:-1");
  const int reload = index_of(v, "reload_after_rollback");
  const int back = index_of(v, "set_released:0");
  const int lower = index_of(v, "lower_gate");
  const int resume = index_of(v, "resume_accept");
  const int publish = index_of(v, "publish_mdns");
  ASSERT_GE(kill, 0);
  EXPECT_LT(kill, wait) << "kill-first, then the unbounded in-flight wait";
  EXPECT_LT(wait, reload) << "the in-flight wait comes before the reload";
  EXPECT_LT(reload, back) << "the reload comes before taking the app back";
  EXPECT_LT(back, lower) << "the app is taken back before the gate is lowered";
  EXPECT_LT(lower, resume) << "the gate is lowered before accepting resumes";
  EXPECT_LT(resume, publish) << "accepting resumes before mDNS is republished";
  EXPECT_LT(index_of(v, "unfreeze"), lower) << "unfreeze comes before the gate is lowered";
}

TEST_F(HandoffOldSideTest, FailedReloadAfterRollbackStillCompletesRollback) {
  ops.reload_ok = false;
  start();
  ASSERT_TRUE(to_go().has_value());
  ASSERT_TRUE(peer->send(protocol::go_ack_t {false, "x"}));
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome) << "a reload that fails keeps the frozen state but the rollback completes";
  verify_rollback(10, false, released_tail(), {"answer:failed"});
}

// ---------------------------------------------------------------------------------------------------------------
// kill_pending
// ---------------------------------------------------------------------------------------------------------------

TEST_F(HandoffOldSideTest, KillPendingBeforeCommitAnswersHelperKillPendingAndWaits) {
  child.timeouts_left = 2;
  start();
  ASSERT_TRUE(handshake());
  ASSERT_TRUE(peer->send(protocol::ready_t {false, "boom"}));
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  const auto v = log.all();
  EXPECT_EQ(3, count_of(v, "kill_and_reap")) << "kill_and_reap is repeated while it returns timeout";
  const int first = index_of(v, "kill_and_reap");
  const int second = index_of(v, "kill_and_reap", static_cast<std::size_t>(first) + 1);
  const int last = last_index_of(v, "kill_and_reap");
  const int pending = index_of(v, "set_state:kill_pending");
  ASSERT_GE(pending, 0) << "after the first kill timeout the state must be kill_pending";
  EXPECT_GT(pending, first);
  EXPECT_LT(pending, second) << "set_state(kill_pending) must follow the first timeout at once";
  const int answer = index_of(v, "answer:kill_pending");
  ASSERT_GE(answer, 0) << "a still-waiting helper is answered kill_pending";
  EXPECT_GT(answer, first);
  EXPECT_LT(answer, last) << "the helper is answered when the kill first times out, not after the reap";
  EXPECT_EQ(std::vector<std::string> {"answer:kill_pending"}, answers_of(v)) << "the helper is answered exactly once (kill_pending, no later failed)";
  EXPECT_EQ("set_state:none", v.back()) << "when the rollback completes the state returns to none";
  EXPECT_GT(last_index_of(v, "set_state:none"), last);
  verify_rollback(0, false, {"remove_new_instance_file"}, {"answer:kill_pending"});
}

TEST_F(HandoffOldSideTest, KillPendingAfterCommitDoesNotAnswerHelperAgain) {
  timeouts.rollback_window = 900ms;
  child.timeouts_left = 1;
  start();
  ASSERT_TRUE(to_committed());
  peer->channel.close();
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  const auto v = log.all();
  const int first = index_of(v, "kill_and_reap");
  const int last = last_index_of(v, "kill_and_reap");
  EXPECT_NE(first, last) << "kill_and_reap must be repeated after a timeout";
  const int pending = index_of(v, "set_state:kill_pending");
  ASSERT_GE(pending, 0);
  EXPECT_GT(pending, first);
  EXPECT_LT(pending, last);
  verify_rollback(10, true, committed_tail(), {"answer:committed"});
  EXPECT_EQ("set_state:none", v.back());
}

TEST_F(HandoffOldSideTest, KillPendingBlocksResumeAndPublishUntilReaped) {
  child.timeouts_left = 3;
  start();
  ASSERT_TRUE(to_go().has_value());
  ASSERT_TRUE(peer->send(protocol::go_ack_t {false, "x"}));
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  const auto v = log.all();
  EXPECT_EQ(4, count_of(v, "kill_and_reap"));
  const int last = last_index_of(v, "kill_and_reap");
  for (const auto *guarded : {"resume_accept", "publish_mdns", "restore_tray", "lower_gate", "set_released:0", "drop_app"}) {
    const int i = index_of(v, guarded);
    if (i >= 0) {
      EXPECT_GT(i, last) << guarded << " must wait for the reap, however many kill attempts time out";
    }
  }
  EXPECT_EQ(std::vector<std::string> {"answer:kill_pending"}, answers_of(v)) << "an unanswered helper is answered kill_pending, once";
}

// ---------------------------------------------------------------------------------------------------------------
// END_ALL interplay
// ---------------------------------------------------------------------------------------------------------------

TEST_F(HandoffOldSideTest, DecideRollbackRunsBeforeKillOnFailedStart) {
  std::vector<handoff::end_state_e> states;
  std::mutex states_mutex;
  child.on_kill = [&]() {
    std::lock_guard lock {states_mutex};
    states.push_back(end_all.state());
  };
  start();
  ASSERT_TRUE(handshake());
  ASSERT_TRUE(peer->send(protocol::ready_t {false, "x"}));
  finish();
  std::lock_guard lock {states_mutex};
  ASSERT_FALSE(states.empty());
  EXPECT_EQ(handoff::end_state_e::rolling_back, states.front()) << "end_all.decide_rollback() must run before kill_and_reap (state rolling_back inside kill_and_reap)";
}

TEST_F(HandoffOldSideTest, DecideRollbackRunsBeforeKillAfterGoAckFailure) {
  std::vector<handoff::end_state_e> states;
  std::mutex states_mutex;
  child.on_kill = [&]() {
    std::lock_guard lock {states_mutex};
    states.push_back(end_all.state());
  };
  start();
  ASSERT_TRUE(to_go().has_value());
  ASSERT_TRUE(peer->send(protocol::go_ack_t {false, "x"}));
  finish();
  std::lock_guard lock {states_mutex};
  ASSERT_FALSE(states.empty());
  EXPECT_EQ(handoff::end_state_e::rolling_back, states.front()) << "end_all.decide_rollback() must run before kill_and_reap";
}

TEST_F(HandoffOldSideTest, RollbackInIdleTakesTheAppBack) {
  timeouts.rollback_window = 900ms;
  start();
  ASSERT_TRUE(to_committed());
  EXPECT_EQ(handoff::end_state_e::idle, end_all.state());
  peer->channel.close();
  finish();
  const auto v = log.all();
  EXPECT_EQ(1, count_of(v, "set_released:0")) << "a rollback decided in idle takes the app back (set_released(false))";
  EXPECT_EQ(0, count_of(v, "drop_app")) << "a rollback decided in idle must not drop the app";
  EXPECT_EQ(handoff::end_state_e::rolling_back, end_all.state());
}

TEST_F(HandoffOldSideTest, RollbackAfterEndedDropsTheApp) {
  timeouts.rollback_window = 900ms;
  start();
  ASSERT_TRUE(to_committed());
  EXPECT_EQ(handoff::end_all_answer_e::ended, end_all.handle_end_all([]() {
  }));
  EXPECT_EQ(handoff::end_state_e::ended, end_all.state());
  peer->channel.close();
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  const auto v = log.all();
  EXPECT_EQ(1, count_of(v, "drop_app")) << "a rollback after ENDED drops the app (no undo)";
  EXPECT_EQ(0, count_of(v, "set_released:0")) << "a rollback after ENDED must never take the app back";
  EXPECT_GT(index_of(v, "drop_app"), last_index_of(v, "kill_and_reap")) << "drop_app waits for the reap";
}

// ---------------------------------------------------------------------------------------------------------------
// Drain requests
// ---------------------------------------------------------------------------------------------------------------

TEST_F(HandoffOldSideTest, DrainRequestsReachTheHandlerAndDoNotBlockPings) {
  timeouts.rollback_window = 700ms;
  handler_blocks = true;
  start();
  ASSERT_TRUE(to_committed());
  ASSERT_TRUE(peer->send(protocol::end_all_t {11}));
  {
    std::unique_lock lock {handler_mutex};
    ASSERT_TRUE(handler_cv.wait_for(lock, 500ms, [this]() {
      return !handled.empty();
    }))
      << "END_ALL must be passed to on_drain_request";
    EXPECT_EQ("END_ALL", handled[0]);
  }
  const auto before = peer->pings.size();
  peer->pump(150ms);
  EXPECT_GE(peer->pings.size() - before, 3U) << "PINGs must keep flowing while the drain handler is blocked";
  {
    std::lock_guard lock {handler_mutex};
    handler_release = true;
    handler_cv.notify_all();
  }
  ASSERT_TRUE(peer->send(protocol::sessions_request_t {12}));
  {
    std::unique_lock lock {handler_mutex};
    ASSERT_TRUE(handler_cv.wait_for(lock, 500ms, [this]() {
      return handled.size() >= 2;
    }))
      << "SESSIONS? must be passed to on_drain_request";
    EXPECT_EQ("SESSIONS?", handled[1]);
  }
  ASSERT_EQ(peer->pump(1500ms), peer_t::pump_end_e::main);
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::handed_over, *outcome);
  std::lock_guard lock {handler_mutex};
  EXPECT_EQ(2U, handled.size()) << "PONG, HELLO, READY and GO_ACK must not be passed to on_drain_request";
}

TEST_F(HandoffOldSideTest, CloseAndCloseCertReachTheHandler) {
  timeouts.rollback_window = 400ms;
  start();
  ASSERT_TRUE(to_committed());
  ASSERT_TRUE(peer->send(protocol::close_request_t {1, 5}));
  ASSERT_TRUE(peer->send(protocol::close_cert_request_t {2, std::string(64, 'b')}));
  ASSERT_EQ(peer->pump(1500ms), peer_t::pump_end_e::main);
  finish();
  std::lock_guard lock {handler_mutex};
  ASSERT_EQ(2U, handled.size());
  EXPECT_EQ("CLOSE", handled[0]);
  EXPECT_EQ("CLOSE_CERT", handled[1]);
}

// ---------------------------------------------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------------------------------------------

TEST_F(HandoffOldSideTest, RequestShutdownWhileWaitingForReadyRollsBackKillFirst) {
  timeouts.ready = 1500ms;
  start();
  ASSERT_TRUE(handshake());
  side->request_shutdown();
  const auto outcome = finish(1000ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::shutdown, *outcome) << "a shutdown request while waiting for READY ends with the shutdown outcome";
  verify_rollback(0, false, start_tail(), {"answer:failed"});
}

TEST_F(HandoffOldSideTest, RequestShutdownBeforeRunRollsBackKillFirst) {
  timeouts.ready = 1500ms;
  start(true);
  const auto outcome = finish(1000ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_NE(old_outcome_e::handed_over, *outcome);
  const auto v = log.all();
  EXPECT_GE(last_index_of(v, "kill_and_reap"), 0) << "a shutdown before GO rolls back with the kill-first rule";
  for (int i = 0; i < last_index_of(v, "kill_and_reap"); ++i) {
    EXPECT_FALSE(is_guarded(v[i])) << "'" << v[i] << "' ran before kill_and_reap";
  }
  EXPECT_EQ(0, count_of(v, "answer:committed"));
}

TEST_F(HandoffOldSideTest, ChildDeathAfterGoWhileShuttingDownStartsNoRollback) {
  start();
  ASSERT_TRUE(to_go().has_value());
  side->request_shutdown();
  peer->channel.close();
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::shutdown, *outcome) << "a child death after GO while shutting down returns shutdown";
  const auto v = log.all();
  for (const auto *effect : {"resume_accept", "publish_mdns", "restore_tray", "lower_gate", "set_released:0", "drop_app", "unfreeze", "reload_after_rollback", "leave_drain_mode"}) {
    EXPECT_EQ(0, count_of(v, effect)) << effect << " must not run: a child death while shutting down starts no rollback";
  }
  EXPECT_EQ(0, count_of(v, "answer:committed"));
}

TEST_F(HandoffOldSideTest, ExitedChildAfterGoWhileShuttingDownStartsNoRollback) {
  timeouts.go_ack = 600ms;
  start();
  ASSERT_TRUE(to_go().has_value());
  side->request_shutdown();
  child.exited_flag = true;
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::shutdown, *outcome) << "an exited child after GO while shutting down returns shutdown";
  const auto v = log.all();
  for (const auto *effect : {"resume_accept", "publish_mdns", "restore_tray", "lower_gate", "set_released:0", "drop_app"}) {
    EXPECT_EQ(0, count_of(v, effect)) << effect << " must not run while shutting down";
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Review additions: reap results, drain routing and floods, shutdown in the window, kill_pending details
// ---------------------------------------------------------------------------------------------------------------

TEST_F(HandoffOldSideTest, ReapErrorWithLiveChildIsTreatedLikeTimeout) {
  child.pending_result = handoff::reap_result_e::error;
  child.timeouts_left = 2;
  child.exited_flag = false;
  start();
  ASSERT_TRUE(to_go().has_value());
  ASSERT_TRUE(peer->send(protocol::go_ack_t {false, "x"}));
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  const auto v = log.all();
  EXPECT_EQ(3, count_of(v, "kill_and_reap")) << "a reap error with exited()==false must be retried like a timeout";
  const int first = index_of(v, "kill_and_reap");
  const int last = last_index_of(v, "kill_and_reap");
  const int pending = index_of(v, "set_state:kill_pending");
  ASSERT_GE(pending, 0) << "a reap error with a live child shows kill_pending";
  EXPECT_GT(pending, first);
  EXPECT_LT(pending, last);
  EXPECT_EQ(std::vector<std::string> {"answer:kill_pending"}, answers_of(v));
  verify_rollback(10, false, {"remove_new_instance_file", "wait_in_flight:-1", "reload_after_rollback", "unfreeze", "lower_gate", "resume_accept", "publish_mdns", "restore_tray", "set_released:0"}, {"answer:kill_pending"});
}

TEST_F(HandoffOldSideTest, ReapErrorWithExitedChildCountsAsGone) {
  child.final_result = handoff::reap_result_e::error;
  child.exited_flag = true;
  start();
  ASSERT_TRUE(handshake());
  ASSERT_TRUE(peer->send(protocol::ready_t {false, "x"}));
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  const auto v = log.all();
  EXPECT_EQ(1, count_of(v, "kill_and_reap")) << "a reap error with exited()==true means the child is gone: no retry";
  EXPECT_EQ(0, count_of(v, "set_state:kill_pending"));
  verify_rollback(0, false, start_tail(), {"answer:failed"});
}

TEST_F(HandoffOldSideTest, ReapedElsewhereCountsAsGone) {
  child.final_result = handoff::reap_result_e::reaped_elsewhere;
  start();
  ASSERT_TRUE(handshake());
  ASSERT_TRUE(peer->send(protocol::ready_t {false, "x"}));
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  const auto v = log.all();
  EXPECT_EQ(1, count_of(v, "kill_and_reap"));
  EXPECT_EQ(0, count_of(v, "set_state:kill_pending"));
  verify_rollback(0, false, start_tail(), {"answer:failed"});
}

TEST_F(HandoffOldSideTest, KillPendingResendsKillEveryReapIntervalAndAnswersHelperOnce) {
  child.timeouts_left = 3;
  start();
  ASSERT_TRUE(handshake());
  ASSERT_TRUE(peer->send(protocol::ready_t {false, "x"}));
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome);
  const auto v = log.all();
  EXPECT_EQ(4, count_of(v, "kill_and_reap")) << "kill_and_reap must be repeated for as long as it times out";
  ASSERT_EQ(4U, child.kill_args.size());
  for (const auto &arg : child.kill_args) {
    EXPECT_EQ(timeouts.reap, arg) << "every kill attempt waits one reap interval";
  }
  EXPECT_EQ(std::vector<std::string> {"answer:kill_pending"}, answers_of(v)) << "after kill_pending the helper must not be answered again with failed";
  EXPECT_EQ(1, count_of(v, "set_state:kill_pending")) << "kill_pending is set once";
  EXPECT_EQ("set_state:none", v.back());
}

TEST_F(HandoffOldSideTest, OnlyDrainRequestsReachTheHandlerAndOtherFdsAreClosed) {
  timeouts.rollback_window = 400ms;
  start();
  ASSERT_TRUE(to_committed());
  int fds[2];
  ASSERT_EQ(0, ::pipe2(fds, O_CLOEXEC));
  ASSERT_TRUE(peer->send(protocol::ended_t {1}, fds[1])) << "sending an fd with ENDED";
  ::close(fds[1]);
  ASSERT_TRUE(peer->send(protocol::ping_t {99}));
  ASSERT_TRUE(peer->send(protocol::go_t {}));
  ASSERT_TRUE(peer->send(protocol::commit_t {}));
  ASSERT_TRUE(peer->send(protocol::main_t {}));
  ASSERT_TRUE(peer->send(protocol::end_rejected_t {2}));
  ASSERT_TRUE(peer->send(protocol::sessions_request_t {3}));
  pollfd pfd {fds[0], POLLIN, 0};
  // keep answering pings while we wait
  const auto deadline = std::chrono::steady_clock::now() + 300ms;
  int rc = 0;
  while (rc == 0 && std::chrono::steady_clock::now() < deadline) {
    peer->pump(10ms);
    rc = ::poll(&pfd, 1, 0);
  }
  EXPECT_EQ(1, rc) << "the fd attached to a dropped message must be closed";
  ::close(fds[0]);
  if (peer->mains == 0) {
    peer->pump(1500ms);
  }
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::handed_over, *outcome) << "stray PING/GO/COMMIT/MAIN/ENDED/END_REJECTED from the child must not disturb the window";
  std::lock_guard lock {handler_mutex};
  EXPECT_EQ(std::vector<std::string> {"SESSIONS?"}, handled) << "only END_ALL, SESSIONS?, CLOSE and CLOSE_CERT may reach on_drain_request";
}

TEST_F(HandoffOldSideTest, FloodOfDrainRequestsWithBlockedHandlerDoesNotDelayHandoverAndIsBounded) {
  timeouts.rollback_window = 200ms;
  handler_blocks = true;
  start();
  ASSERT_TRUE(to_committed());
  const auto committed_at = std::chrono::steady_clock::now();
  for (int i = 0; i < 2000; ++i) {
    peer->send(protocol::sessions_request_t {static_cast<std::uint64_t>(i + 1)});  // may fail with EAGAIN once the socket is full
    if (i % 40 == 0 && peer->mains == 0) {
      peer->pump(2ms);
    }
  }
  if (peer->mains == 0) {
    peer->pump(1500ms);
  }
  const auto elapsed = std::chrono::steady_clock::now() - committed_at;
  EXPECT_EQ(1, peer->mains) << "MAIN must still arrive while the drain handler is blocked and requests flood in";
  EXPECT_LT(elapsed, 650ms) << "the handover must not wait for the blocked drain handler (it would take until the handler's 700 ms cap)";
  {
    std::lock_guard lock {handler_mutex};
    handler_release = true;
    handler_cv.notify_all();
  }
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::handed_over, *outcome);
  std::lock_guard lock {handler_mutex};
  EXPECT_LE(handled.size(), 1025U) << "the number of requests waiting for the handler is bounded";
}

TEST_F(HandoffOldSideTest, FloodOfPongsDoesNotDelayPingsOrHandover) {
  timeouts.rollback_window = 200ms;
  start();
  ASSERT_TRUE(to_committed());
  const auto committed_at = std::chrono::steady_clock::now();
  for (int i = 0; i < 2000; ++i) {
    peer->send(protocol::pong_t {100000});
    if (i % 40 == 0 && peer->mains == 0) {
      peer->pump(2ms);
    }
  }
  if (peer->mains == 0) {
    peer->pump(1500ms);
  }
  const auto elapsed = std::chrono::steady_clock::now() - committed_at;
  EXPECT_EQ(1, peer->mains) << "MAIN must arrive after a flood of PONGs";
  EXPECT_LT(elapsed, 900ms) << "the handover must happen about rollback_window after COMMIT despite the flood";
  EXPECT_GE(peer->pings.size(), 2U) << "PINGs keep being sent during the flood";
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::handed_over, *outcome);
  std::lock_guard lock {handler_mutex};
  EXPECT_TRUE(handled.empty()) << "PONGs never reach on_drain_request";
}

TEST_F(HandoffOldSideTest, RequestShutdownInRollbackWindowEndsWithShutdownAndNoHandover) {
  timeouts.rollback_window = 600ms;
  start();
  ASSERT_TRUE(to_committed());
  side->request_shutdown();
  peer->pump(100ms);
  // SPEC-QUESTION: the spec only defines a child death during shutdown; a shutdown with a live child in the window
  // is assumed to end the run with `shutdown`, no handover and no rollback.
  const auto outcome = finish(1000ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::shutdown, *outcome);
  const auto v = log.all();
  EXPECT_EQ(0, count_of(v, "close_socket_copies")) << "no handover once a shutdown arrived";
  for (const auto *effect : {"resume_accept", "publish_mdns", "restore_tray", "lower_gate", "set_released:0", "drop_app"}) {
    EXPECT_EQ(0, count_of(v, effect)) << effect << " must not run: a shutdown starts no rollback";
  }
}

TEST_F(HandoffOldSideTest, RequestShutdownDuringHandoverRetriesEndsWithShutdown) {
  timeouts.rollback_window = 60ms;
  timeouts.handover_retry = 40ms;
  ops.handover_default = false;
  std::atomic<bool> failed_seen {false};
  ops.hook = [&](const std::string &name) {
    if (name == "set_state:handover_failed") {
      failed_seen = true;
    }
  };
  start();
  ASSERT_TRUE(to_committed());
  for (int i = 0; i < 40 && !failed_seen; ++i) {
    peer->pump(20ms);
  }
  ASSERT_TRUE(failed_seen) << "the handover must have failed once before the shutdown";
  side->request_shutdown();
  peer->pump(100ms);
  const auto outcome = finish(1000ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::shutdown, *outcome) << "a shutdown during handover retries ends the run with shutdown";
  const auto v = log.all();
  EXPECT_EQ(0, count_of(v, "close_socket_copies"));
  EXPECT_EQ(0, peer->mains);
  for (const auto *effect : {"resume_accept", "publish_mdns", "restore_tray", "lower_gate", "set_released:0", "drop_app"}) {
    EXPECT_EQ(0, count_of(v, effect)) << effect << " must not run: a shutdown starts no rollback";
  }
}

TEST_F(HandoffOldSideTest, ChildDeathDuringHandoverRetriesWhileShuttingDownStartsNoRollback) {
  timeouts.rollback_window = 60ms;
  timeouts.handover_retry = 40ms;
  ops.handover_default = false;
  std::atomic<bool> failed_seen {false};
  ops.hook = [&](const std::string &name) {
    if (name == "set_state:handover_failed") {
      failed_seen = true;
    }
  };
  start();
  ASSERT_TRUE(to_committed());
  for (int i = 0; i < 40 && !failed_seen; ++i) {
    peer->pump(20ms);
  }
  ASSERT_TRUE(failed_seen);
  side->request_shutdown();
  child.exited_flag = true;
  peer->channel.close();
  const auto outcome = finish(1000ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(old_outcome_e::shutdown, *outcome);
  EXPECT_EQ(0, count_of(log.all(), "resume_accept")) << "no rollback while shutting down";
  EXPECT_EQ(0, count_of(log.all(), "publish_mdns"));
}

// ---------------------------------------------------------------------------------------------------------------
// A take_snapshot that throws fails the switch and the full rollback runs
// ---------------------------------------------------------------------------------------------------------------

TEST_F(HandoffOldSideTest, ThrowingTakeSnapshotFailsTheSwitchWithAReasonAndRunsTheFullRollback) {
  ops.snapshot_throws = true;
  start();
  ASSERT_TRUE(to_ready());
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value()) << "run() must return (not hang) when take_snapshot throws";
  EXPECT_EQ(old_outcome_e::rolled_back, *outcome) << "a snapshot that cannot be taken fails the switch: the outcome is rolled_back";
  const auto v = log.all();
  EXPECT_EQ(1, count_of(v, "take_snapshot")) << "setup: take_snapshot must have been reached";
  verify_rollback(9, false, withdrawn_tail(), {"answer:failed"});
  // The rollback effects, one by one (positive identification of each undo).
  for (const auto *effect : {"unfreeze", "lower_gate", "resume_accept", "publish_mdns", "restore_tray", "reload_after_rollback", "remove_new_instance_file"}) {
    EXPECT_EQ(1, count_of(v, effect)) << effect << " must run exactly once after take_snapshot threw";
  }
  EXPECT_EQ(1, count_of(v, "kill_and_reap")) << "the rollback is kill-first: the child must be killed and reaped";
  EXPECT_EQ(0, count_of(v, "set_released:1")) << "the app must not be released when the snapshot failed";
  EXPECT_EQ(0, count_of(v, "start_upnp_cleanup:" + std::to_string(test_new_base))) << "COMMIT was never sent";
  EXPECT_EQ(1, count_of(v, "set_state:none")) << "the handoff state must be reset";
  EXPECT_EQ(0, count_of(v, "answer:committed"));
  {
    std::lock_guard lock {replies_mutex};
    ASSERT_EQ(1U, replies.size()) << "the helper must be answered exactly once";
    EXPECT_EQ(reply_kind_e::failed, replies[0].kind);
    // SPEC-QUESTION: the header does not fix the reason text; the lead's ruling for this case is "taking the snapshot
    // failed". Only a non-empty reason that mentions the snapshot is asserted.
    EXPECT_NE(std::string::npos, replies[0].reason.find("snapshot")) << "the reason must say that taking the snapshot failed; it was: " << replies[0].reason;
  }
  const auto got_go = peer->collect(50ms);
  EXPECT_EQ(0, count_of(got_go, "GO")) << "no GO may be sent when take_snapshot threw";
}

TEST_F(HandoffOldSideTest, NonThrowingTakeSnapshotReachesGoAndRunsNoRollbackEffect) {
  start();
  const auto go = to_go();
  ASSERT_TRUE(go.has_value()) << "control: with a normal take_snapshot the switch must reach GO";
  const auto v = log.all();
  EXPECT_EQ(1, count_of(v, "take_snapshot"));
  for (const auto *effect : {"unfreeze", "lower_gate", "resume_accept", "publish_mdns", "restore_tray", "kill_and_reap", "answer:failed"}) {
    EXPECT_EQ(0, count_of(v, effect)) << effect << " must not run when the switch succeeded";
  }
  peer->channel.close();
  finish(1000ms);
}

#endif  // __linux__
