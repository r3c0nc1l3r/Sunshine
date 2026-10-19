/**
 * @file tests/unit/handoff/test_coordinator.cpp
 * @brief Blind tests for the old side's glue (src/handoff/coordinator.h): the listener registry, `pidfd_child_t`,
 * `sunshine_old_ops_t` and `coordinator_t`.
 *
 * Written from `docs/drain-handoff.md` and the headers only (`coordinator.h`, `old_side.h`, `refusal.h`, `control.h`,
 * `spawn.h`, `responder.h`, `drain.h`, `runtime.h`, `instance_file.h`, `hooks.h` and `src/upnp.h`). Every effect goes
 * through the documented seams: `coordinator_env_t`, `old_ops_seams_t`, `child_seams_t`, the listener registry and the
 * fake IGD of `upnp::testing`. A scripted peer (the test body) plays the new process on a real channel pair. Nothing
 * here starts a real process (except a `/bin/sleep` child in the two tests that need a real pidfd), systemd, a display,
 * a GPU, root or the network.
 *
 * The coordinator tests use the process-wide `handoff::runtime()` (the production operations write to it), which the
 * fixture returns to a fresh state before and after each test.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <array>
  #include <atomic>
  #include <chrono>
  #include <cstdint>
  #include <cstdlib>
  #include <cstring>
  #include <filesystem>
  #include <fstream>
  #include <functional>
  #include <future>
  #include <map>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <set>
  #include <stdexcept>
  #include <string>
  #include <thread>
  #include <type_traits>
  #include <utility>
  #include <variant>
  #include <vector>

  // platform includes
  #include <cerrno>
  #include <csignal>
  #include <fcntl.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <sys/syscall.h>
  #include <sys/un.h>
  #include <sys/wait.h>
  #include <unistd.h>

  // local includes
  #include "src/config.h"
  #include "src/handoff/coordinator.h"
  #include "src/handoff/hooks.h"
  #include "src/handoff/sd_notify.h"
  #include "src/handoff/settings.h"
  #include "src/upnp.h"

using namespace std::chrono_literals;

namespace {
  namespace hf = handoff;
  namespace protocol = handoff::protocol;
  using tp_t = std::chrono::steady_clock::time_point;
  using fake_mapping_t = upnp::testing::fake_mapping_t;

  constexpr pid_t child_pid_value = 4242;  ///< Pid of the fake child.
  constexpr std::uint64_t child_start_value = 987654321;  ///< Start time of the fake child.
  constexpr int own_port = 47989;  ///< Configured `port` (also the old process's UDP base).
  constexpr int other_base = 48089;  ///< `port + 100`: the new process's UDP base.

  /**
   * @brief Thread-safe ordered record of events.
   */
  class event_log_t {
  public:
    /**
     * @brief Append an event.
     *
     * @param entry Event text.
     */
    void add(std::string entry) {
      std::lock_guard lock {_mutex};
      _entries.push_back(std::move(entry));
    }

    /**
     * @brief Copy of all events.
     *
     * @return Events in order.
     */
    std::vector<std::string> all() const {
      std::lock_guard lock {_mutex};
      return _entries;
    }

    /**
     * @brief Remove all events.
     */
    void clear() {
      std::lock_guard lock {_mutex};
      _entries.clear();
    }

    /**
     * @brief Number of events equal to `name`.
     *
     * @param name Event.
     * @return Count.
     */
    int count(const std::string &name) const {
      std::lock_guard lock {_mutex};
      return static_cast<int>(std::count(_entries.begin(), _entries.end(), name));
    }

    /**
     * @brief Number of events that start with `prefix`.
     *
     * @param prefix Prefix.
     * @return Count.
     */
    int count_prefix(const std::string &prefix) const {
      std::lock_guard lock {_mutex};
      int n = 0;
      for (const auto &e : _entries) {
        if (e.rfind(prefix, 0) == 0) {
          ++n;
        }
      }
      return n;
    }

    /**
     * @brief Index of the first event equal to `name`.
     *
     * @param name Event.
     * @return Index, or -1.
     */
    int first(const std::string &name) const {
      std::lock_guard lock {_mutex};
      for (std::size_t i = 0; i < _entries.size(); ++i) {
        if (_entries[i] == name) {
          return static_cast<int>(i);
        }
      }
      return -1;
    }

    /**
     * @brief Index of the last event equal to `name`.
     *
     * @param name Event.
     * @return Index, or -1.
     */
    int last(const std::string &name) const {
      std::lock_guard lock {_mutex};
      for (std::size_t i = _entries.size(); i > 0; --i) {
        if (_entries[i - 1] == name) {
          return static_cast<int>(i - 1);
        }
      }
      return -1;
    }

  private:
    mutable std::mutex _mutex;  ///< Guards the entries.
    std::vector<std::string> _entries;  ///< Events.
  };

  /**
   * @brief Poll a condition with a bound.
   *
   * @tparam P Predicate type.
   * @param pred Condition.
   * @param limit Longest wait.
   * @return `true` when the condition became true in time.
   */
  template<typename P>
  bool wait_until(P pred, std::chrono::milliseconds limit = 1500ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return pred();
      }
      std::this_thread::sleep_for(2ms);
    }
    return true;
  }

  /**
   * @brief Name of a reply kind.
   *
   * @param kind Kind.
   * @return Name.
   */
  std::string kind_name(hf::reply_kind_e kind) {
    switch (kind) {
      case hf::reply_kind_e::refused:
        return "refused";
      case hf::reply_kind_e::failed:
        return "failed";
      case hf::reply_kind_e::kill_pending:
        return "kill_pending";
      case hf::reply_kind_e::committed:
        return "committed";
    }
    return "?";
  }

  /**
   * @brief Name of an unmap scope.
   *
   * @param scope Scope.
   * @return Name.
   */
  std::string scope_name(upnp::unmap_scope_e scope) {
    switch (scope) {
      case upnp::unmap_scope_e::all:
        return "all";
      case upnp::unmap_scope_e::udp_only:
        return "udp_only";
      case upnp::unmap_scope_e::none:
        return "none";
    }
    return "?";
  }

  /**
   * @brief The fake IGD store entries of a plan.
   *
   * @param plan Planned mappings.
   * @return Store entries.
   */
  std::set<fake_mapping_t> store_of(const std::vector<upnp::planned_mapping_t> &plan) {
    std::set<fake_mapping_t> out;
    for (const auto &m : plan) {
      out.insert(fake_mapping_t {m.port, !m.udp});
    }
    return out;
  }

  /**
   * @brief The UDP store entries of a plan.
   *
   * @param plan Planned mappings.
   * @return UDP store entries.
   */
  std::set<fake_mapping_t> udp_store_of(const std::vector<upnp::planned_mapping_t> &plan) {
    std::set<fake_mapping_t> out;
    for (const auto &m : plan) {
      if (m.udp) {
        out.insert(fake_mapping_t {m.port, false});
      }
    }
    return out;
  }

  /**
   * @brief Human-readable store, for assertion messages.
   *
   * @param store Store.
   * @return Text such as "47984/tcp 48098/udp".
   */
  std::string describe(const std::set<fake_mapping_t> &store) {
    std::string out;
    for (const auto &m : store) {
      out += std::to_string(m.port) + (m.tcp ? "/tcp " : "/udp ");
    }
    return out;
  }

  /**
   * @brief A steady clock that a test moves by hand.
   */
  struct test_clock_t {
    tp_t base {std::chrono::steady_clock::now()};  ///< Time zero of the test.
    std::atomic<std::int64_t> offset_ms {0};  ///< Added to `base`.

    /**
     * @brief Current test time.
     *
     * @return Time.
     */
    tp_t now() const {
      return base + std::chrono::milliseconds {offset_ms.load()};
    }

    /**
     * @brief Test time at an offset from zero.
     *
     * @param d Offset.
     * @return Time.
     */
    tp_t at(std::chrono::milliseconds d) const {
      return base + d;
    }

    /**
     * @brief Move the clock forward.
     *
     * @param d Amount.
     */
    void advance(std::chrono::milliseconds d) {
      offset_ms += d.count();
    }
  };

  /**
   * @brief Shared state of the fake child, readable by the test while the coordinator owns the object.
   */
  struct child_state_t {
    std::atomic<int> kills {0};  ///< Calls of `kill_and_reap`.
    std::atomic<int> timeouts_left {0};  ///< Calls that still answer `timeout`.
    std::atomic<bool> gone {false};  ///< Result of `exited()`.
    event_log_t *log {nullptr};  ///< Event log.
  };

  /**
   * @brief Fake `child_ops_t`.
   */
  class fake_child_t final: public hf::child_ops_t {
  public:
    /**
     * @brief Create the fake.
     *
     * @param state Shared state.
     */
    explicit fake_child_t(std::shared_ptr<child_state_t> state):
        _state(std::move(state)) {
    }

    hf::reap_result_e kill_and_reap(std::chrono::milliseconds) override {
      ++_state->kills;
      if (_state->log != nullptr) {
        _state->log->add("kill_and_reap");
      }
      if (_state->timeouts_left.load() > 0) {
        --_state->timeouts_left;
        return hf::reap_result_e::timeout;
      }
      _state->gone = true;
      return hf::reap_result_e::reaped;
    }

    bool exited() override {
      return _state->gone.load();
    }

    pid_t pid() override {
      return child_pid_value;
    }

    std::uint64_t start_time() override {
      return child_start_value;
    }

  private:
    std::shared_ptr<child_state_t> _state;  ///< Shared state.
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

    /**
     * @brief Stop the pump thread.
     */
    ~peer_t() {
      stop_pump();
    }

    peer_t(const peer_t &) = delete;  ///< Not copyable.
    peer_t &operator=(const peer_t &) = delete;  ///< Not copyable.

    hf::channel_t channel;  ///< Peer end.
    std::thread pump_thread;  ///< Pump thread.
    std::atomic<bool> pump_stop {false};  ///< Asks the pump to end.
    std::atomic<bool> got_main {false};  ///< The pump saw `MAIN`.
    std::atomic<int> pings {0};  ///< Pings answered by the pump.
    std::mutex mutex;  ///< Guards `got`.
    std::vector<protocol::message_t> got;  ///< Other messages seen by the pump.

    /**
     * @brief Send a message.
     *
     * @param message Message.
     * @return `true` when sent.
     */
    bool send(const protocol::message_t &message) {
      return channel.send(message);
    }

    /**
     * @brief Wait for the next message of type `T`, answering pings on the way.
     *
     * @tparam T Wanted type.
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
        hf::received_t r;
        const auto status = channel.receive(r, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        if (status == hf::recv_status_e::timeout) {
          continue;
        }
        if (status != hf::recv_status_e::ok) {
          ADD_FAILURE() << "peer: receive status " << static_cast<int>(status) << " while waiting for " << wanted
                        << " (the old side closed the channel)";
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
     * @brief Start a thread that answers pings and collects every other message (`MAIN` sets `got_main`).
     */
    void start_pump() {
      if (pump_thread.joinable()) {
        return;
      }
      pump_thread = std::thread([this]() {
        while (!pump_stop.load()) {
          hf::received_t r;
          const auto status = channel.receive(r, 20ms);
          if (status == hf::recv_status_e::timeout) {
            continue;
          }
          if (status != hf::recv_status_e::ok) {
            return;
          }
          if (r.fd >= 0) {
            ::close(r.fd);
          }
          if (const auto *ping = std::get_if<protocol::ping_t>(&r.message)) {
            ++pings;
            channel.send(protocol::pong_t {ping->seq});
          } else if (std::holds_alternative<protocol::main_t>(r.message)) {
            got_main = true;
          } else {
            std::lock_guard lock {mutex};
            got.push_back(r.message);
          }
        }
      });
    }

    /**
     * @brief Stop the pump thread.
     */
    void stop_pump() {
      pump_stop = true;
      if (pump_thread.joinable()) {
        pump_thread.join();
      }
    }

    /**
     * @brief Wait until the pump collected a message of type `T`, and take it.
     *
     * @tparam T Wanted type.
     * @param limit Longest wait.
     * @return The message, or nothing.
     */
    template<typename T>
    std::optional<T> take(std::chrono::milliseconds limit = 1000ms) {
      std::optional<T> out;
      wait_until(
        [&]() {
          std::lock_guard lock {mutex};
          for (auto it = got.begin(); it != got.end(); ++it) {
            if (const auto *m = std::get_if<T>(&*it)) {
              out = *m;
              got.erase(it);
              return true;
            }
          }
          return false;
        },
        limit
      );
      return out;
    }
  };

  /**
   * @brief Fixture: a coordinator with fakes for every seam, a scripted peer and a controllable clock.
   */
  class HandoffCoordinatorTest: public testing::Test {
  protected:
    static constexpr std::array<hf::listener_e, 4> kinds {hf::listener_e::https, hf::listener_e::http, hf::listener_e::webui, hf::listener_e::rtsp};  ///< Listener order.

    /**
     * @brief State of one fake listener.
     */
    struct listener_fake_t {
      std::atomic<bool> pause_ok {true};  ///< Result of `pause`.
      std::atomic<bool> resume_ok {true};  ///< Result of `resume`.
      std::atomic<bool> close_ok {true};  ///< Result of `close_listener`.
      std::atomic<int> fd {-1};  ///< Result of `fd`.
      std::atomic<int> pauses {0};  ///< `pause` calls.
      std::atomic<int> resumes {0};  ///< `resume` calls.
      std::atomic<int> closes {0};  ///< `close_listener` calls.
      std::atomic<long long> last_pause_ms {-1};  ///< Timeout of the last `pause`.
      std::atomic<long long> last_resume_ms {-1};  ///< Timeout of the last `resume`.
      std::atomic<long long> last_close_ms {-1};  ///< Timeout of the last `close_listener`.
    };

    void SetUp() override {
      saved_port = config::sunshine.port;
      saved_flags = config::sunshine.flags;
      config::sunshine.port = own_port;

      hf::runtime().reset_for_test();
      hf::listeners().reset_for_test();
      upnp::testing::remove_fake_igd();
      upnp::testing::reset_upnp_for_test();
      upnp::set_unmap_scope(upnp::unmap_scope_e::all);

      char tmpl[] = "/tmp/sunshine-coord-XXXXXX";
      ASSERT_NE(nullptr, ::mkdtemp(tmpl)) << "mkdtemp failed";
      tmp = tmpl;
      inst_dir = tmp / "instances";
      std::filesystem::create_directories(inst_dir);

      hf::settings_t s;
      s.enabled = true;
      s.udp_offset = 100;
      s.deadline_s = 3600;
      s.ready_timeout_s = 30;
      s.rollback_window_s = 60;
      s.cooldown_s = 120;
      settings = s;
      hf::runtime().configure(s);

      hf::instance_t inst;
      inst.pid = ::getpid();
      inst.start_time = 1;
      inst.role = hf::role_e::active;
      inst.udp_base = own_port;
      inst.sessions = 0;
      inst.handoff = hf::state_e::none;
      hf::runtime().open_instance(inst_dir, inst);

      for (std::size_t i = 0; i < kinds.size(); ++i) {
        lf[i].fd = 31 + static_cast<int>(i);
      }
      register_listeners();

      timeouts.ready = 400ms;
      timeouts.in_flight = 100ms;
      timeouts.pairing = 100ms;
      timeouts.mdns_withdraw = 50ms;
      timeouts.go_ack = 400ms;
      timeouts.reap = 50ms;
      timeouts.ping_interval = 20ms;
      timeouts.misses_to_hang = 3;
      timeouts.rollback_window = 5000ms;
      timeouts.handover_retry = 40ms;

      child_state = std::make_shared<child_state_t>();
      child_state->log = &events;
    }

    void TearDown() override {
      if (coord) {
        coord->request_shutdown();
      }
      if (peer) {
        peer->stop_pump();
        peer->channel.close();
      }
      if (fut.valid()) {
        fut.wait_for(3000ms);
      }
      peer.reset();
      if (coord) {
        coord->stop_control();
        coord->stop();
        coord.reset();
      }
      upnp_guard.reset();
      upnp::testing::reset_upnp_for_test();
      upnp::testing::remove_fake_igd();
      upnp::set_unmap_scope(upnp::unmap_scope_e::all);
      hf::listeners().reset_for_test();
      hf::runtime().set_channel(nullptr);
      hf::runtime().instance_remove();
      hf::runtime().reset_for_test();
      if (real_pid > 0) {
        if (::waitpid(real_pid, nullptr, WNOHANG) == 0) {
          ::kill(real_pid, SIGKILL);
          ::waitpid(real_pid, nullptr, 0);
        }
      }
      for (int *k : {&keep_a, &keep_b, &peer_fd}) {
        if (*k >= 0) {
          ::close(*k);
          *k = -1;
        }
      }
      std::error_code ec;
      std::filesystem::remove_all(tmp, ec);
      config::sunshine.port = saved_port;
      config::sunshine.flags = saved_flags;
    }

    // -----------------------------------------------------------------------------------------------------------
    // Listeners
    // -----------------------------------------------------------------------------------------------------------

    /**
     * @brief Register the four fake listeners.
     */
    void register_listeners() {
      for (std::size_t i = 0; i < kinds.size(); ++i) {
        const auto kind = kinds[i];
        const std::string name {hf::to_string(kind)};
        hf::listener_ops_t ops;
        ops.pause = [this, i, name](std::chrono::milliseconds t) {
          ++lf[i].pauses;
          lf[i].last_pause_ms = t.count();
          events.add("pause:" + name);
          return lf[i].pause_ok.load();
        };
        ops.resume = [this, i, name](std::chrono::milliseconds t) {
          ++lf[i].resumes;
          lf[i].last_resume_ms = t.count();
          events.add("resume:" + name);
          return lf[i].resume_ok.load();
        };
        ops.close_listener = [this, i, name](std::chrono::milliseconds t) {
          ++lf[i].closes;
          lf[i].last_close_ms = t.count();
          events.add("close:" + name);
          return lf[i].close_ok.load();
        };
        ops.fd = [this, i]() {
          return lf[i].fd.load();
        };
        hf::listeners().set(kind, std::move(ops));
      }
    }

    // -----------------------------------------------------------------------------------------------------------
    // Refusal sources
    // -----------------------------------------------------------------------------------------------------------

    /**
     * @brief Sources in which every check passes; each source counts its calls.
     *
     * @return Sources.
     */
    hf::refusal_sources_t make_sources() {
      hf::refusal_sources_t s;
      s.notify_socket = [this]() {
        ++cnt_notify;
        return notify_value;
      };
      s.systemd_version = [this]() {
        ++cnt_version;
        return systemd_value;
      };
      s.cgroup = [this]() {
        ++cnt_cgroup;
        return cgroup_value;
      };
      s.handoff_in_progress = [this]() {
        ++cnt_inprogress;
        return hf::runtime().handoff_in_progress();
      };
      s.peer_exists = [this]() {
        ++cnt_peer;
        return peer_exists_value;
      };
      s.scan = [this]() {
        ++cnt_scan;
        return scan_value;
      };
      s.app_adoptable = [this](std::string &why) {
        ++cnt_app;
        if (!app_adoptable_value) {
          why = app_why;
        }
        return app_adoptable_value;
      };
      s.pairing_count = [this]() {
        ++cnt_pairing;
        return pairing_value.load();
      };
      s.pending_launch = [this]() {
        ++cnt_pending;
        return pending_value.load();
      };
      s.self_pid = []() {
        return ::getpid();
      };
      s.now = [this]() {
        return clk.now();
      };
      return s;
    }

    /**
     * @brief Sum of the calls of every refusal source.
     *
     * @return Total.
     */
    int sources_total() const {
      return cnt_notify + cnt_version + cnt_cgroup + cnt_inprogress + cnt_peer + cnt_scan + cnt_app + cnt_pairing + cnt_pending;
    }

    // -----------------------------------------------------------------------------------------------------------
    // Operations
    // -----------------------------------------------------------------------------------------------------------

    /**
     * @brief Seams for `sunshine_old_ops_t`: every module call is a fake that records an event. `set_unmap_scope` and
     * `remove_ports` stay empty (the production UPnP calls, which act on the fake IGD) unless a test asks for a hook.
     *
     * @return Seams.
     */
    hf::old_ops_seams_t make_ops_seams() {
      hf::old_ops_seams_t q;
      q.probe = []() {
        hf::probe_data_t d;
        d.results = protocol::probe_results_t {2, 1, true, false, false};
        d.identity.build_id = "build-x";
        d.identity.render_node = "/dev/dri/renderD128";
        d.identity.vendor_id = 4098;
        d.identity.device_id = 29772;
        d.identity.driver_version = "1.2.3";
        return d;
      };
      q.self_pidfd = [this]() {
        events.add("self_pidfd");
        return ::open("/dev/null", O_RDONLY | O_CLOEXEC);
      };
      q.sources = make_sources();
      q.sources_set = true;
      q.drop_pending_launch = [this]() {
        events.add("drop_pending_launch");
        return true;
      };
      q.sleep_for = [this](std::chrono::milliseconds d) {
        ++sleep_calls;
        sleep_max_ms = std::max<long long>(sleep_max_ms.load(), d.count());
        clk.advance(d);  // the refusal sources read the fake clock, so waiting must move it
        if (on_sleep) {
          on_sleep();
        }
        std::this_thread::sleep_for(1ms);
      };
      q.wait_in_flight = [this](std::chrono::milliseconds t) {
        events.add("wait_in_flight:" + std::to_string(t.count()));
        return in_flight_ok.load();
      };
      q.gate = [this](bool up) {
        events.add(up ? "gate_up" : "gate_down");
      };
      q.save_state = [this]() {
        events.add("save_state");
        if (throw_in_save) {
          throw std::runtime_error("save failed");
        }
        return save_ok.load();
      };
      q.freeze = [this](bool v) {
        events.add(v ? "freeze" : "unfreeze");
      };
      q.mdns_withdraw = [this](std::chrono::milliseconds) {
        events.add("mdns_withdraw");
      };
      q.mdns_start = [this]() {
        events.add("mdns_start");
        return true;
      };
      q.tray_suspend = [this](std::chrono::milliseconds) {
        events.add("tray_suspend");
        return true;
      };
      q.tray_resume = [this]() {
        events.add("tray_resume");
        return true;
      };
      q.snapshot = [this]() {
        events.add("snapshot");
        protocol::go_t go;
        go.host_audio = true;
        go.next_launch_session_id = 42;
        return go;
      };
      q.set_released = [this](bool v) {
        events.add(v ? "set_released:1" : "set_released:0");
      };
      q.drop_app = [this]() {
        events.add("drop_app");
      };
      q.reload = [this]() {
        events.add("reload");
        if (throw_in_reload) {
          throw std::runtime_error("reload failed");
        }
        return reload_ok.load();
      };
      if (remove_ports_hook) {
        q.remove_ports = [this](std::set<std::uint16_t> ports) {
          events.add("remove_ports");
          remove_ports_hook(std::move(ports));
        };
      }
      q.notify_socket = [this]() {
        ++cnt_notify_socket;
        return std::string {"/run/test-notify"};
      };
      q.cgroup = []() {
        return std::optional<std::string> {cgroup_text()};
      };
      q.notify = [this](const std::string &payload) {
        events.add("notify:" + payload);
        if (throw_in_notify) {
          throw std::runtime_error("notify failed");
        }
        return notify_ok.load();
      };
      q.barrier = [this](std::chrono::milliseconds) {
        events.add("barrier");
        return barrier_ok.load();
      };
      q.read_back = [this](const std::string &unit, bool user_unit, std::chrono::milliseconds) -> std::optional<pid_t> {
        events.add("read_back:" + unit + (user_unit ? ":user" : ":system"));
        if (read_back_pid.load() < 0) {
          return std::nullopt;
        }
        return static_cast<pid_t>(read_back_pid.load());
      };
      q.drain_begin = [this](tp_t) {
        events.add("drain_begin");
      };
      q.drain_cancel = [this]() {
        events.add("drain_cancel");
      };
      q.session_poke = [this]() {
        events.add("session_poke");
      };
      return q;
    }

    /**
     * @brief The cgroup text the fake process lives in.
     *
     * @return Content of a cgroup v2 file naming one system unit.
     */
    static std::string cgroup_text() {
      return "0::/system.slice/sunshine.service\n";
    }

    /**
     * @brief Operations with test seams, on their own (no coordinator).
     *
     * @param new_base The child's UDP base.
     * @param own_base This process's UDP base.
     * @return The operations.
     */
    std::unique_ptr<hf::sunshine_old_ops_t> direct_ops(int new_base = other_base, int own_base = own_port) {
      return std::make_unique<hf::sunshine_old_ops_t>(control, new_base, child_pid_value, clk.at(120s), inst_dir, own_base, make_ops_seams());
    }

    /**
     * @brief Responder operations: one live session (id 7), `END_ALL` recorded.
     *
     * @return Operations.
     */
    hf::responder_ops_t make_responder_ops() {
      hf::responder_ops_t ops;
      ops.list_sessions = []() {
        protocol::session_entry_t e;
        e.id = 7;
        e.started_unix_ms = 1000;
        e.transport = "udp/ipv4";
        e.client_category = "lan";
        return std::vector<protocol::session_entry_t> {e};
      };
      ops.close_session = [](std::uint32_t) {
        return hf::close_outcome_e::closed;
      };
      ops.close_cert = [](const std::string &) {
        return std::uint32_t {0};
      };
      ops.end_and_join = [this]() {
        events.add("end_and_join");
      };
      return ops;
    }

    /**
     * @brief The test environment of the coordinator.
     *
     * @return Environment.
     */
    hf::coordinator_env_t build_env() {
      hf::coordinator_env_t e;
      e.runtime = &hf::runtime();
      e.sources = make_sources();
      e.sources_set = true;
      e.now = [this]() {
        return clk.now();
      };
      e.make_channel_pair = [this]() -> std::optional<std::pair<int, int>> {
        ++channel_calls;
        if (on_channel) {
          on_channel();
        }
        if (channel_fail) {
          return std::nullopt;
        }
        auto p = hf::make_channel_pair();
        if (p && keep_channel_dups) {
          keep_a = ::fcntl(p->first, F_DUPFD_CLOEXEC, 100);
          keep_b = ::fcntl(p->second, F_DUPFD_CLOEXEC, 100);
          pair_first = p->first;
        }
        return p;
      };
      e.spawn = [this](const hf::spawn_request_t &req, std::string *err) -> std::optional<hf::child_t> {
        {
          std::lock_guard lock {spawn_mutex};
          last_req = req;
        }
        if (coord) {
          cooldown_at_spawn = coord->cooldown_until();
          end_all_at_spawn = coord->end_all_for_test().state();
        }
        in_progress_at_spawn = hf::runtime().handoff_in_progress();
        channel_set_at_spawn = hf::runtime().channel() != nullptr;
        if (on_spawn) {
          on_spawn();
        }
        std::optional<hf::child_t> result;
        if (keep_channel_dups) {
          int &mine = (req.fds.channel == pair_first) ? keep_a : keep_b;
          if (mine >= 0) {
            ::close(mine);
            mine = -1;
          }
        }
        if (spawn_fail) {
          if (err != nullptr) {
            *err = spawn_fail_reason;
          }
        } else if (use_real_child) {
          result = spawn_sleeper();
        } else {
          peer_fd = ::fcntl(req.fds.channel, F_DUPFD_CLOEXEC, 100);
          hf::child_t c;
          c.pid = child_pid_value;
          c.pidfd = -1;
          result = c;
        }
        ++spawn_calls;
        return result;
      };
      e.make_child = [this](hf::child_t c) -> std::unique_ptr<hf::child_ops_t> {
        ++make_child_calls;
        if (c.pidfd >= 0) {
          ::close(c.pidfd);
        }
        return std::make_unique<fake_child_t>(child_state);
      };
      e.start_time = [this](pid_t) {
        return start_time_result;
      };
      e.make_ops = [this](pid_t pid, int new_base, tp_t cooldown_until) -> std::unique_ptr<hf::old_side_ops_t> {
        ++make_ops_calls;
        last_new_base = new_base;
        last_cooldown_arg = cooldown_until;
        return std::make_unique<hf::sunshine_old_ops_t>(control, new_base, pid, cooldown_until, inst_dir, own_base_override.value_or(own_port), make_ops_seams());
      };
      e.adjust_timeouts = [this](hf::old_side_timeouts_t &t) {
        t = timeouts;
      };
      e.host_sink = [this]() -> std::string {
        return host_sink_value;
      };
      e.argv = []() {
        return std::vector<std::string> {"sunshine", "--config", "x"};
      };
      e.instances_dir = inst_dir;
      e.make_responder = [this](hf::channel_t &ch, hf::end_all_machine_t &m) {
        ++responder_calls;
        return std::make_unique<hf::drain_responder_t>(ch, m, make_responder_ops());
      };
      e.make_poller = [this]() {
        hf::revocation_poller_t::test_config_t cfg;
        cfg.state_file = tmp / "state.json";
        cfg.terminate = [](const std::string &) {
        };
        cfg.is_draining = []() {
          return true;
        };
        auto p = std::make_unique<hf::revocation_poller_t>(cfg);
        poller_raw = p.get();
        ++poller_calls;
        return p;
      };
      e.drain_handed_over = [this]() {
        events.add("drain_handed_over");
      };
      e.set_unmap_scope = [this](upnp::unmap_scope_e scope) {
        events.add("env_scope:" + scope_name(scope));
        upnp::set_unmap_scope(scope);
      };
      return e;
    }

    /**
     * @brief The coordinator under test (created on first use).
     *
     * @return The coordinator.
     */
    hf::coordinator_t &C() {
      if (!coord) {
        coord = std::make_unique<hf::coordinator_t>(control, build_env());
      }
      return *coord;
    }

    /**
     * @brief Fork a `/bin/sleep` child and open its pidfd.
     *
     * @return The child.
     */
    hf::child_t spawn_sleeper() {
      const pid_t pid = ::fork();
      if (pid == 0) {
        ::execl("/bin/sleep", "sleep", "5", static_cast<char *>(nullptr));
        ::_exit(127);
      }
      real_pid = pid;
      hf::child_t c;
      c.pid = pid;
      c.pidfd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
      return c;
    }

    // -----------------------------------------------------------------------------------------------------------
    // Requests and the scripted peer
    // -----------------------------------------------------------------------------------------------------------

    /**
     * @brief Run `handle()` on its own thread.
     *
     * @param peer_pid Peer pid given to `handle()`.
     */
    void start_handle(pid_t peer_pid = 777) {
      auto &c = C();
      fut = std::async(std::launch::async, [&c, peer_pid]() {
        return c.handle(peer_pid);
      });
    }

    /**
     * @brief Wait for the answer of the request started by `start_handle()`.
     *
     * @param limit Longest wait.
     * @return The reply, or nothing on timeout (a failure is recorded).
     */
    std::optional<hf::control_reply_t> get_reply(std::chrono::milliseconds limit = 2000ms) {
      if (!fut.valid()) {
        ADD_FAILURE() << "no request was started";
        return std::nullopt;
      }
      if (fut.wait_for(limit) != std::future_status::ready) {
        ADD_FAILURE() << "handle() did not answer within " << limit.count() << " ms";
        return std::nullopt;
      }
      return fut.get();
    }

    /**
     * @brief Wait until the spawn seam ran and take the peer end of the channel.
     *
     * @return `true` when the peer exists.
     */
    bool attach_peer() {
      if (!wait_until([this]() {
            return spawn_calls.load() >= 1;
          })) {
        ADD_FAILURE() << "the coordinator never spawned the child";
        return false;
      }
      if (peer_fd < 0) {
        ADD_FAILURE() << "the spawn request carried no channel fd";
        return false;
      }
      peer = std::make_unique<peer_t>(peer_fd);
      peer_fd = -1;
      return true;
    }

    /**
     * @brief Peer: receive the old `HELLO` and answer with a `HELLO`.
     *
     * @return `true` when it worked.
     */
    bool peer_handshake() {
      int fd = -1;
      const auto hello = peer->expect<protocol::hello_t>(800ms, &fd);
      if (!hello) {
        return false;
      }
      EXPECT_GE(fd, 0) << "the old side's HELLO must carry its pidfd";
      if (fd >= 0) {
        ::close(fd);
      }
      last_hello = *hello;
      return peer->send(protocol::hello_t {});
    }

    /**
     * @brief Peer: handshake, then `READY` ok.
     *
     * @return `true` when it worked.
     */
    bool peer_ready() {
      return peer_handshake() && peer->send(protocol::ready_t {true, ""});
    }

    /**
     * @brief Peer: up to and including `GO`.
     *
     * @return `true` when it worked.
     */
    bool peer_to_go() {
      return peer_ready() && peer->expect<protocol::go_t>().has_value();
    }

    /**
     * @brief Peer: up to and including `COMMIT`.
     *
     * @return `true` when it worked.
     */
    bool peer_to_commit() {
      return peer_to_go() && peer->send(protocol::go_ack_t {true, ""}) && peer->expect<protocol::commit_t>().has_value();
    }

    /**
     * @brief Start a request, attach the peer and take the whole path to `COMMIT`.
     *
     * @return `true` when it worked.
     */
    bool run_to_commit() {
      start_handle();
      return attach_peer() && peer_to_commit();
    }

    /**
     * @brief Install the fake IGD with the old process's mappings and the killed new process's UDP mappings, and start
     * UPnP in this (old) process.
     *
     * @param old_base The old process's UDP base.
     * @param new_base The new process's UDP base.
     */
    void install_igd_and_start(int old_base, int new_base) {
      config::sunshine.flags[config::flag::UPNP] = true;
      old_store = store_of(upnp::plan_mappings(own_port, old_base, false));
      new_udp_store = udp_store_of(upnp::plan_mappings(own_port, new_base, false));
      auto initial = old_store;
      initial.insert(new_udp_store.begin(), new_udp_store.end());
      upnp::testing::install_fake_igd(initial);
      upnp_guard = upnp::start(old_base);
      ASSERT_NE(nullptr, upnp_guard) << "upnp::start() returned no guard although UPnP is on and a fake gateway is installed";
    }

    /**
     * @brief Send a request and assert that it is a refusal with the exact reason, that nothing was started, and that
     * `handoff_in_progress` and the cooldown are the same as before the request.
     *
     * @param peer_pid Peer pid given to `handle()`.
     * @param reason Expected reason.
     */
    void expect_refused(pid_t peer_pid, std::string_view reason) {
      const bool in_progress_before = hf::runtime().handoff_in_progress();
      const auto cooldown_before = C().cooldown_until();
      const auto reply = C().handle(peer_pid);
      EXPECT_EQ(hf::reply_kind_e::refused, reply.kind) << "expected a refusal ('" << reason << "') but got " << kind_name(reply.kind) << " '" << reply.reason << "'";
      EXPECT_EQ(std::string {reason}, reply.reason) << "the refusal must carry its own reason";
      EXPECT_EQ(0, spawn_calls.load()) << "a refusal must not spawn the new process";
      EXPECT_EQ(0, channel_calls.load()) << "a refusal must not create the channel";
      EXPECT_EQ(0, make_ops_calls.load()) << "a refusal must not start the old side";
      EXPECT_EQ(in_progress_before, hf::runtime().handoff_in_progress()) << "a refusal must not change handoff_in_progress";
      EXPECT_EQ(cooldown_before, C().cooldown_until()) << "a refusal must not arm or extend the cooldown";
    }

    /**
     * @brief Make every refusal input apply at once, so that a test can peel them off in the documented order.
     */
    void set_everything_wrong() {
      hf::instance_t killed;
      killed.pid = 9001;
      killed.start_time = 1;
      killed.role = hf::role_e::draining;
      killed.handoff = hf::state_e::kill_pending;
      hf::instance_t pending;
      pending.pid = 9002;
      pending.start_time = 1;
      pending.role = hf::role_e::pending;
      scan_value.live = {killed, pending};
      peer_exists_value = true;
      notify_value = "";
      systemd_value = 200;
      cgroup_value = "0::/\n";
      app_adoptable_value = false;
      app_why = "a launcher is still running";
      pairing_value = 2;
      pending_value = true;
      C().set_cooldown_until(clk.at(60s));
      hf::runtime().set_handoff_in_progress(true);
    }

    test_clock_t clk;  ///< Test clock.
    event_log_t events;  ///< Events of the fakes.
    hf::settings_t settings;  ///< Settings given to the runtime.
    hf::old_side_timeouts_t timeouts;  ///< Timeouts the coordinator uses.
    hf::control_server_t control;  ///< Control server (never started unless a test does).
    std::unique_ptr<hf::coordinator_t> coord;  ///< Object under test.
    std::shared_ptr<child_state_t> child_state;  ///< Fake child state.
    std::unique_ptr<peer_t> peer;  ///< Scripted new process.
    std::future<hf::control_reply_t> fut;  ///< Result of `start_handle()`.
    std::unique_ptr<platf::deinit_t> upnp_guard;  ///< The old process's UPnP guard.
    std::array<listener_fake_t, 4> lf;  ///< Fake listeners.
    std::filesystem::path tmp;  ///< Temporary directory.
    std::filesystem::path inst_dir;  ///< Instances directory.

    // refusal inputs
    std::string notify_value {"/run/test-notify"};  ///< `NOTIFY_SOCKET`.
    std::optional<int> systemd_value {255};  ///< systemd version.
    std::optional<std::string> cgroup_value {cgroup_text()};  ///< `/proc/self/cgroup`.
    bool peer_exists_value {false};  ///< A draining peer exists.
    hf::scan_result_t scan_value;  ///< Instance scan.
    bool app_adoptable_value {true};  ///< App adoptable.
    std::string app_why;  ///< Why not.
    std::atomic<std::size_t> pairing_value {0};  ///< Pairing sessions.
    std::atomic<bool> pending_value {false};  ///< Pending launch.
    std::atomic<int> cnt_notify {0}, cnt_version {0}, cnt_cgroup {0}, cnt_inprogress {0}, cnt_peer {0}, cnt_scan {0}, cnt_app {0}, cnt_pairing {0}, cnt_pending {0};  ///< Source call counters.

    // seam controls
    std::atomic<int> spawn_calls {0}, channel_calls {0}, make_child_calls {0}, make_ops_calls {0}, responder_calls {0}, poller_calls {0};  ///< Seam call counters.
    std::atomic<int> sleep_calls {0}, cnt_notify_socket {0};  ///< Misc counters.
    std::atomic<long long> sleep_max_ms {0};  ///< Longest sleep requested.
    std::atomic<bool> in_flight_ok {true}, save_ok {true}, reload_ok {true}, notify_ok {true}, barrier_ok {true};  ///< Results.
    std::atomic<int> read_back_pid {child_pid_value};  ///< Read-back result (negative: none).
    bool throw_in_save {false}, throw_in_reload {false}, throw_in_notify {false};  ///< Make a seam throw.
    bool spawn_fail {false}, channel_fail {false}, use_real_child {false}, keep_channel_dups {false};  ///< Failure modes.
    std::string spawn_fail_reason {"exec failed"};  ///< Spawn failure reason.
    std::optional<std::uint64_t> start_time_result {child_start_value};  ///< Child start time.
    std::string host_sink_value;  ///< Recorded host sink (empty: none).
    std::optional<int> own_base_override;  ///< Own UDP base given to the operations.
    std::function<void()> on_spawn, on_channel, on_sleep;  ///< Hooks.
    std::function<void(std::set<std::uint16_t>)> remove_ports_hook;  ///< Replaces the removal when set.

    // observations
    std::mutex spawn_mutex;  ///< Guards `last_req`.
    hf::spawn_request_t last_req;  ///< Last spawn request.
    tp_t cooldown_at_spawn {};  ///< Cooldown seen by the spawn seam.
    hf::end_state_e end_all_at_spawn {hf::end_state_e::rolling_back};  ///< END_ALL state seen by the spawn seam.
    std::atomic<bool> in_progress_at_spawn {false}, channel_set_at_spawn {false};  ///< State seen by the spawn seam.
    std::atomic<int> last_new_base {0};  ///< Base given to `make_ops`.
    tp_t last_cooldown_arg {};  ///< Cooldown given to `make_ops`.
    protocol::hello_t last_hello;  ///< Last `HELLO` received by the peer.
    hf::revocation_poller_t *poller_raw {nullptr};  ///< Poller made by the seam (valid until the coordinator stops it).
    std::set<fake_mapping_t> old_store, new_udp_store;  ///< Fake IGD contents.

    int keep_a {-1}, keep_b {-1}, pair_first {-1}, peer_fd {-1};  ///< Fds.
    pid_t real_pid {0};  ///< Real child, when used.
    int saved_port {0};  ///< Saved `config::sunshine.port`.
    decltype(config::sunshine.flags) saved_flags;  ///< Saved flags.
  };
}  // namespace

// =====================================================================================================================
// Constants and names
// =====================================================================================================================

TEST_F(HandoffCoordinatorTest, ServerOperationTimeoutIsTheInFlightBudget) {
  EXPECT_EQ(5000ms, hf::server_op_timeout) << "server_op_timeout is the 5 s in-flight budget";
  EXPECT_EQ(hf::old_side_timeouts_t {}.in_flight, hf::server_op_timeout) << "it equals old_side_timeouts_t::in_flight at the defaults";
}

TEST_F(HandoffCoordinatorTest, ListenerNamesAreTheFdTableNames) {
  EXPECT_EQ("https", hf::to_string(hf::listener_e::https));
  EXPECT_EQ("http", hf::to_string(hf::listener_e::http));
  EXPECT_EQ("webui", hf::to_string(hf::listener_e::webui));
  EXPECT_EQ("rtsp", hf::to_string(hf::listener_e::rtsp));
}

// =====================================================================================================================
// Listener registry
// =====================================================================================================================

TEST_F(HandoffCoordinatorTest, RegistryReportsRegisteredListeners) {
  for (auto k : kinds) {
    EXPECT_TRUE(hf::listeners().registered(k)) << "the fixture registered " << hf::to_string(k);
  }
  hf::listeners().clear(hf::listener_e::webui);
  EXPECT_FALSE(hf::listeners().registered(hf::listener_e::webui)) << "clear() must unregister";
  EXPECT_TRUE(hf::listeners().registered(hf::listener_e::rtsp)) << "clear() of one listener must not touch another";
}

TEST_F(HandoffCoordinatorTest, RegistryFdIsTheRegisteredFdOrMinusOne) {
  EXPECT_EQ(31, hf::listeners().fd(hf::listener_e::https));
  EXPECT_EQ(34, hf::listeners().fd(hf::listener_e::rtsp));
  lf[2].fd = -1;
  EXPECT_EQ(-1, hf::listeners().fd(hf::listener_e::webui)) << "a server without a listener reports -1";
  hf::listeners().clear(hf::listener_e::http);
  EXPECT_EQ(-1, hf::listeners().fd(hf::listener_e::http)) << "nothing registered gives -1";
}

TEST_F(HandoffCoordinatorTest, RegistryWithNothingRegisteredFailsPauseAndResumeButClosesNothing) {
  hf::listeners().clear(hf::listener_e::http);
  EXPECT_FALSE(hf::listeners().pause(hf::listener_e::http, 100ms)) << "pause with nothing registered must be false";
  EXPECT_FALSE(hf::listeners().resume(hf::listener_e::http, 100ms)) << "resume with nothing registered must be false";
  EXPECT_TRUE(hf::listeners().close_listener(hf::listener_e::http, 100ms)) << "there is nothing to close: true";
  EXPECT_EQ(0, lf[1].pauses.load()) << "no registered function may run after clear()";
  hf::listeners().clear(hf::listener_e::http);  // safe twice
}

TEST_F(HandoffCoordinatorTest, RegistryPassesTheResultsOfTheRegisteredFunctions) {
  lf[0].pause_ok = false;
  lf[0].resume_ok = false;
  lf[0].close_ok = false;
  EXPECT_FALSE(hf::listeners().pause(hf::listener_e::https, 100ms));
  EXPECT_FALSE(hf::listeners().resume(hf::listener_e::https, 100ms));
  EXPECT_FALSE(hf::listeners().close_listener(hf::listener_e::https, 100ms));
  EXPECT_TRUE(hf::listeners().pause(hf::listener_e::http, 100ms));
  EXPECT_TRUE(hf::listeners().resume(hf::listener_e::http, 100ms));
  EXPECT_TRUE(hf::listeners().close_listener(hf::listener_e::http, 100ms));
}

TEST_F(HandoffCoordinatorTest, RegistryCutsALargeTimeoutToTheServerOperationTimeout) {
  hf::listeners().pause(hf::listener_e::https, 60000ms);
  hf::listeners().resume(hf::listener_e::https, 60000ms);
  hf::listeners().close_listener(hf::listener_e::https, 60000ms);
  EXPECT_EQ(5000, lf[0].last_pause_ms.load()) << "a pause timeout above server_op_timeout is cut to it";
  EXPECT_EQ(5000, lf[0].last_resume_ms.load()) << "a resume timeout above server_op_timeout is cut to it";
  EXPECT_EQ(5000, lf[0].last_close_ms.load()) << "a close timeout above server_op_timeout is cut to it";
  hf::listeners().pause(hf::listener_e::https, 300ms);
  EXPECT_EQ(300, lf[0].last_pause_ms.load()) << "a smaller timeout is passed unchanged";
}

TEST_F(HandoffCoordinatorTest, RegistryCutsZeroAndNegativeTimeoutsToOneMillisecond) {
  hf::listeners().pause(hf::listener_e::https, 0ms);
  EXPECT_EQ(1, lf[0].last_pause_ms.load()) << "zero is cut to 1 ms, never passed on";
  hf::listeners().resume(hf::listener_e::https, -5ms);
  EXPECT_EQ(1, lf[0].last_resume_ms.load()) << "a negative value is cut to 1 ms, never passed on";
  hf::listeners().close_listener(hf::listener_e::https, -1ms);
  EXPECT_EQ(1, lf[0].last_close_ms.load()) << "a negative value is cut to 1 ms, never passed on";
}

TEST_F(HandoffCoordinatorTest, RegistryIgnoresAnEntryWithAMissingMember) {
  hf::listeners().clear(hf::listener_e::rtsp);
  hf::listener_ops_t ops;
  ops.pause = [](std::chrono::milliseconds) {
    return true;
  };
  hf::listeners().set(hf::listener_e::rtsp, std::move(ops));
  EXPECT_FALSE(hf::listeners().registered(hf::listener_e::rtsp)) << "an entry with unset members must be ignored";
}

TEST_F(HandoffCoordinatorTest, RegistrySetReplacesAnEarlierEntry) {
  std::atomic<int> second_calls {0};
  hf::listener_ops_t ops;
  ops.pause = [&](std::chrono::milliseconds) {
    ++second_calls;
    return true;
  };
  ops.resume = ops.pause;
  ops.close_listener = ops.pause;
  ops.fd = []() {
    return 99;
  };
  hf::listeners().set(hf::listener_e::https, std::move(ops));
  EXPECT_EQ(99, hf::listeners().fd(hf::listener_e::https)) << "the replacement must be used";
  hf::listeners().pause(hf::listener_e::https, 100ms);
  EXPECT_EQ(1, second_calls.load());
  EXPECT_EQ(0, lf[0].pauses.load()) << "the replaced entry must not be called any more";
}

TEST_F(HandoffCoordinatorTest, RegistryClearWaitsForARunningCall) {
  std::atomic<bool> in_call {false};
  std::atomic<bool> release {false};
  hf::listener_ops_t ops;
  ops.pause = [&](std::chrono::milliseconds) {
    in_call = true;
    const auto limit = std::chrono::steady_clock::now() + 1500ms;
    while (!release.load() && std::chrono::steady_clock::now() < limit) {
      std::this_thread::sleep_for(1ms);
    }
    return true;
  };
  ops.resume = ops.pause;
  ops.close_listener = ops.pause;
  ops.fd = []() {
    return 5;
  };
  hf::listeners().set(hf::listener_e::https, std::move(ops));
  std::thread caller([]() {
    hf::listeners().pause(hf::listener_e::https, 1000ms);
  });
  ASSERT_TRUE(wait_until([&]() {
    return in_call.load();
  }));
  std::atomic<bool> cleared {false};
  std::thread clearer([&]() {
    hf::listeners().clear(hf::listener_e::https);
    cleared = true;
  });
  std::this_thread::sleep_for(60ms);
  EXPECT_FALSE(cleared.load()) << "clear() must wait for a running call on that listener";
  release = true;
  caller.join();
  clearer.join();
  EXPECT_TRUE(cleared.load());
  EXPECT_FALSE(hf::listeners().registered(hf::listener_e::https));
}

// =====================================================================================================================
// pidfd_child_t
// =====================================================================================================================

namespace {
  /**
   * @brief Shared record of the pidfd seams.
   */
  struct pidfd_record_t {
    std::atomic<int> kill_calls {0};  ///< Calls of the `kill_and_reap` seam.
    std::atomic<int> exited_calls {0};  ///< Calls of the `exited` seam.
    std::atomic<int> start_calls {0};  ///< Calls of the `start_time` seam.
    std::atomic<int> seen_fd {-2};  ///< Pidfd seen by the seams.
    std::atomic<int> kill_result {static_cast<int>(hf::reap_result_e::reaped)};  ///< Result of the kill seam.
    std::atomic<bool> exited_result {false};  ///< Result of the exited seam.
    std::optional<std::uint64_t> start_result {777};  ///< Result of the start-time seam.
  };

  /**
   * @brief Seams that report into a record.
   *
   * @param rec Record.
   * @return Seams.
   */
  hf::child_seams_t pidfd_seams(pidfd_record_t &rec) {
    hf::child_seams_t s;
    s.kill_and_reap = [&rec](int fd, std::chrono::milliseconds, int *status) {
      ++rec.kill_calls;
      rec.seen_fd = fd;
      if (status != nullptr) {
        *status = 0;
      }
      return static_cast<hf::reap_result_e>(rec.kill_result.load());
    };
    s.exited = [&rec](int fd) {
      ++rec.exited_calls;
      rec.seen_fd = fd;
      return rec.exited_result.load();
    };
    s.start_time = [&rec](pid_t) {
      ++rec.start_calls;
      return rec.start_result;
    };
    return s;
  }
}  // namespace

TEST_F(HandoffCoordinatorTest, PidfdChildReportsPidAndStartTimeReadOnce) {
  pidfd_record_t rec;
  int fds[2];
  ASSERT_EQ(0, ::pipe2(fds, O_CLOEXEC));
  ::close(fds[1]);
  hf::pidfd_child_t child {hf::child_t {4321, fds[0]}, pidfd_seams(rec)};
  EXPECT_EQ(4321, child.pid());
  EXPECT_EQ(777u, child.start_time());
  EXPECT_EQ(777u, child.start_time());
  EXPECT_EQ(1, rec.start_calls.load()) << "the start time is read once, at construction";
}

TEST_F(HandoffCoordinatorTest, PidfdChildStartTimeIsZeroWhenUnreadable) {
  pidfd_record_t rec;
  rec.start_result = std::nullopt;
  hf::pidfd_child_t child {hf::child_t {4321, -1}, pidfd_seams(rec)};
  EXPECT_EQ(0u, child.start_time()) << "an unreadable start time reads as 0";
}

TEST_F(HandoffCoordinatorTest, PidfdChildKillAndReapUsesItsPidfd) {
  pidfd_record_t rec;
  int fds[2];
  ASSERT_EQ(0, ::pipe2(fds, O_CLOEXEC));
  ::close(fds[1]);
  hf::pidfd_child_t child {hf::child_t {4321, fds[0]}, pidfd_seams(rec)};
  EXPECT_EQ(hf::reap_result_e::reaped, child.kill_and_reap(50ms));
  EXPECT_EQ(fds[0], rec.seen_fd.load()) << "the kill must go through the child's own pidfd";
}

TEST_F(HandoffCoordinatorTest, PidfdChildRemembersAReapedChildAndDoesNotSignalAgain) {
  pidfd_record_t rec;
  hf::pidfd_child_t child {hf::child_t {4321, -1}, pidfd_seams(rec)};
  ASSERT_EQ(hf::reap_result_e::reaped, child.kill_and_reap(50ms));
  EXPECT_EQ(hf::reap_result_e::reaped_elsewhere, child.kill_and_reap(50ms)) << "a second kill-first answers reaped_elsewhere";
  EXPECT_EQ(1, rec.kill_calls.load()) << "a second kill-first must not act on the (possibly recycled) pid";
  EXPECT_TRUE(child.exited()) << "a reaped child is remembered as gone";
  EXPECT_EQ(0, rec.exited_calls.load()) << "exited() must not touch the pidfd once the child is remembered as gone";
}

TEST_F(HandoffCoordinatorTest, PidfdChildReapedElsewhereIsRememberedToo) {
  pidfd_record_t rec;
  rec.kill_result = static_cast<int>(hf::reap_result_e::reaped_elsewhere);
  hf::pidfd_child_t child {hf::child_t {4321, -1}, pidfd_seams(rec)};
  ASSERT_EQ(hf::reap_result_e::reaped_elsewhere, child.kill_and_reap(50ms));
  EXPECT_EQ(hf::reap_result_e::reaped_elsewhere, child.kill_and_reap(50ms));
  EXPECT_EQ(1, rec.kill_calls.load());
  EXPECT_TRUE(child.exited());
}

TEST_F(HandoffCoordinatorTest, PidfdChildTimeoutIsNotRememberedAndTriesAgain) {
  pidfd_record_t rec;
  rec.kill_result = static_cast<int>(hf::reap_result_e::timeout);
  hf::pidfd_child_t child {hf::child_t {4321, -1}, pidfd_seams(rec)};
  EXPECT_EQ(hf::reap_result_e::timeout, child.kill_and_reap(50ms));
  EXPECT_EQ(hf::reap_result_e::timeout, child.kill_and_reap(50ms));
  EXPECT_EQ(2, rec.kill_calls.load()) << "a timeout means try again: the next call signals again";
  EXPECT_FALSE(child.exited()) << "a child that timed out is not gone";
}

TEST_F(HandoffCoordinatorTest, PidfdChildErrorIsReturnedAsIs) {
  pidfd_record_t rec;
  rec.kill_result = static_cast<int>(hf::reap_result_e::error);
  hf::pidfd_child_t child {hf::child_t {4321, -1}, pidfd_seams(rec)};
  EXPECT_EQ(hf::reap_result_e::error, child.kill_and_reap(50ms));
}

TEST_F(HandoffCoordinatorTest, PidfdChildExitedAsksThePidfdUntilTheChildIsGone) {
  pidfd_record_t rec;
  hf::pidfd_child_t child {hf::child_t {4321, -1}, pidfd_seams(rec)};
  EXPECT_FALSE(child.exited());
  rec.exited_result = true;
  EXPECT_TRUE(child.exited());
  EXPECT_EQ(2, rec.exited_calls.load());
}

TEST_F(HandoffCoordinatorTest, PidfdChildClosesItsPidfdOnceInTheDestructor) {
  int fds[2];
  ASSERT_EQ(0, ::pipe2(fds, O_CLOEXEC));
  ::close(fds[1]);
  pidfd_record_t rec;
  {
    hf::pidfd_child_t child {hf::child_t {4321, fds[0]}, pidfd_seams(rec)};
    EXPECT_NE(-1, ::fcntl(fds[0], F_GETFD)) << "the pidfd must stay open while the object lives";
  }
  errno = 0;
  EXPECT_EQ(-1, ::fcntl(fds[0], F_GETFD)) << "the destructor must close the pidfd";
  EXPECT_EQ(EBADF, errno);
}

// =====================================================================================================================
// sunshine_old_ops_t: HELLO, pause, resume
// =====================================================================================================================

TEST_F(HandoffCoordinatorTest, OpsHelloCarriesVersionProbeIdentityAndCooldown) {
  auto ops = direct_ops();
  const auto hello = ops->hello();
  EXPECT_EQ(protocol::version_major, hello.major);
  EXPECT_EQ(protocol::version_minor, hello.minor);
  EXPECT_TRUE(hello.capabilities.empty()) << "the HELLO carries no capabilities";
  ASSERT_TRUE(hello.probe.has_value()) << "the old side's HELLO carries its probe results";
  EXPECT_EQ(2, hello.probe->hevc_mode);
  EXPECT_EQ(1, hello.probe->av1_mode);
  ASSERT_TRUE(hello.identity.has_value()) << "the old side's HELLO carries the identity of the probe";
  EXPECT_EQ("build-x", hello.identity->build_id);
  EXPECT_EQ("1.2.3", hello.identity->driver_version);
  ASSERT_TRUE(hello.cooldown_deadline_ns.has_value()) << "the cooldown deadline is always present";
  EXPECT_EQ(std::chrono::duration_cast<std::chrono::nanoseconds>(clk.at(120s).time_since_epoch()).count(), *hello.cooldown_deadline_ns) << "the deadline is the steady_clock time since its epoch, in nanoseconds";
}

TEST_F(HandoffCoordinatorTest, OpsSelfPidfdIsTheSeamValue) {
  auto seams = make_ops_seams();
  seams.self_pidfd = []() {
    return 1234;
  };
  hf::sunshine_old_ops_t ops {control, other_base, child_pid_value, clk.at(1s), inst_dir, own_port, seams};
  EXPECT_EQ(1234, ops.self_pidfd());
}

TEST_F(HandoffCoordinatorTest, OpsPauseAcceptPausesInOrderWithABoundedTimeoutAndReturnsEmpty) {
  auto ops = direct_ops();
  EXPECT_EQ("", ops->pause_accept()) << "all four confirmed: an empty reason";
  EXPECT_EQ((std::vector<std::string> {"pause:https", "pause:http", "pause:webui", "pause:rtsp"}), events.all()) << "pause order is https, http, webui, rtsp";
  for (const auto &l : lf) {
    EXPECT_GT(l.last_pause_ms.load(), 0) << "a pause timeout must be positive";
    EXPECT_LE(l.last_pause_ms.load(), 5000) << "a pause timeout must be at most the 5 s in-flight budget";
  }
}

TEST_F(HandoffCoordinatorTest, OpsPauseAcceptNamesTheFirstServerThatDoesNotConfirmAndStops) {
  for (std::size_t failing = 0; failing < kinds.size(); ++failing) {
    SCOPED_TRACE(std::string {"failing server: "} + std::string {hf::to_string(kinds[failing])});
    events.clear();
    for (auto &l : lf) {
      l.pause_ok = true;
      l.pauses = 0;
    }
    lf[failing].pause_ok = false;
    auto ops = direct_ops();
    const auto reason = ops->pause_accept();
    EXPECT_EQ("the " + std::string {hf::to_string(kinds[failing])} + " server did not confirm the pause in time", reason);
    for (std::size_t i = 0; i < kinds.size(); ++i) {
      if (i <= failing) {
        EXPECT_EQ(1, lf[i].pauses.load()) << hf::to_string(kinds[i]) << " is paused up to and including the failing one";
      } else {
        EXPECT_EQ(0, lf[i].pauses.load()) << hf::to_string(kinds[i]) << " comes after the failure and must not be touched";
      }
    }
  }
}

TEST_F(HandoffCoordinatorTest, OpsPauseAcceptFailsWhenAListenerIsNotRegistered) {
  hf::listeners().clear(hf::listener_e::webui);
  auto ops = direct_ops();
  EXPECT_EQ("the webui server did not confirm the pause in time", ops->pause_accept()) << "a missing server cannot confirm a pause";
}

TEST_F(HandoffCoordinatorTest, OpsResumeAcceptResumesAllFourInOrderAndNeverStopsAtAFailure) {
  lf[0].resume_ok = false;
  lf[2].resume_ok = false;
  auto ops = direct_ops();
  ops->resume_accept();
  EXPECT_EQ((std::vector<std::string> {"resume:https", "resume:http", "resume:webui", "resume:rtsp"}), events.all()) << "resume goes on after a failure, in the pause order";
  for (const auto &l : lf) {
    EXPECT_GT(l.last_resume_ms.load(), 0);
    EXPECT_LE(l.last_resume_ms.load(), 5000) << "a resume timeout must be at most server_op_timeout";
  }
}

// =====================================================================================================================
// sunshine_old_ops_t: freeze, drain mode and the UPnP scope
// =====================================================================================================================

TEST_F(HandoffCoordinatorTest, OpsFreezeSetsUdpOnlyScopeWhenTheBasesDiffer) {
  auto ops = direct_ops(other_base, own_port);
  ops->freeze();
  EXPECT_EQ(upnp::unmap_scope_e::udp_only, upnp::unmap_scope()) << "from freeze() on this process must never remove the shared TCP mappings";
  EXPECT_EQ(1, events.count("freeze"));
}

TEST_F(HandoffCoordinatorTest, OpsFreezeSetsScopeNoneWhenTheBasesAreEqual) {
  auto ops = direct_ops(own_port, own_port);
  ops->freeze();
  EXPECT_EQ(upnp::unmap_scope_e::none, upnp::unmap_scope()) << "equal bases: the drain process unmaps nothing";
}

TEST_F(HandoffCoordinatorTest, OpsUnfreezeRestoresScopeAll) {
  auto ops = direct_ops();
  ops->freeze();
  ASSERT_EQ(upnp::unmap_scope_e::udp_only, upnp::unmap_scope());
  ops->unfreeze();
  EXPECT_EQ(upnp::unmap_scope_e::all, upnp::unmap_scope()) << "unfreeze() restores all";
  EXPECT_EQ(1, events.count("unfreeze"));
}

TEST_F(HandoffCoordinatorTest, OpsLeaveDrainModeRestoresScopeAllWithoutAnUnfreeze) {
  auto ops = direct_ops();
  ops->freeze();
  ops->enter_drain_mode();
  ops->leave_drain_mode();
  EXPECT_EQ(upnp::unmap_scope_e::all, upnp::unmap_scope()) << "leave_drain_mode() restores all even when unfreeze() never ran (a rollback step failed)";
}

TEST_F(HandoffCoordinatorTest, OpsEnterDrainModeBeginsTheControllerThenPokesTheNotifier) {
  auto ops = direct_ops();
  ops->enter_drain_mode();
  EXPECT_EQ(hf::mode_e::draining, hf::runtime().mode()) << "enter_drain_mode() moves normal -> draining";
  EXPECT_EQ("drain", hf::runtime().process_tag());
  EXPECT_EQ(upnp::unmap_scope_e::udp_only, upnp::unmap_scope()) << "the scope is set again (idempotent) at drain entry";
  EXPECT_EQ(1, events.count("drain_begin")) << "drain_controller().begin() is called once";
  ASSERT_GE(events.first("session_poke"), 0) << "the notifier is poked so the instance file shows the draining state";
  EXPECT_LT(events.first("drain_begin"), events.first("session_poke")) << "begin() comes before the poke";
}

TEST_F(HandoffCoordinatorTest, OpsLeaveDrainModeCancelsTheControllerAndBecomesNormalAgain) {
  auto ops = direct_ops();
  ops->enter_drain_mode();
  ops->leave_drain_mode();
  EXPECT_EQ(1, events.count("drain_cancel")) << "leave_drain_mode() calls drain_controller().cancel()";
  EXPECT_EQ(hf::mode_e::normal, hf::runtime().mode());
  EXPECT_EQ("", hf::runtime().process_tag()) << "the process tag is cleared";
}

TEST_F(HandoffCoordinatorTest, OpsEnterDrainModeToleratesARefusedTransition) {
  hf::runtime().reset_for_test();  // mode disabled: normal -> draining is refused
  auto ops = direct_ops();
  EXPECT_NO_THROW(ops->enter_drain_mode()) << "a refused transition is logged and the steps go on";
  EXPECT_EQ(1, events.count("drain_begin"));
}

TEST_F(HandoffCoordinatorTest, OpsSetRoleAndStateWriteTheInstanceFile) {
  auto ops = direct_ops();
  ops->set_role(hf::role_e::draining);
  ops->set_state(hf::state_e::kill_pending);
  const auto cur = hf::runtime().instance_current();
  ASSERT_TRUE(cur.has_value());
  EXPECT_EQ(hf::role_e::draining, cur->role);
  EXPECT_EQ(hf::state_e::kill_pending, cur->handoff);
}

TEST_F(HandoffCoordinatorTest, OpsSetStateSendsHandoffStateOnlyWhileTheRoleIsDraining) {
  const auto pair = hf::make_channel_pair();
  ASSERT_TRUE(pair.has_value());
  hf::runtime().set_channel(std::make_shared<hf::channel_t>(pair->first));
  peer = std::make_unique<peer_t>(pair->second);
  auto ops = direct_ops();

  ops->set_state(hf::state_e::draining);  // role is still active
  hf::received_t r;
  EXPECT_EQ(hf::recv_status_e::timeout, peer->channel.receive(r, 100ms)) << "an active process sends no HANDOFF_STATE";

  ops->set_role(hf::role_e::draining);
  ops->set_state(hf::state_e::draining);
  const auto msg = peer->expect<protocol::handoff_state_t>(500ms);
  ASSERT_TRUE(msg.has_value()) << "a draining process tells the active process its state";
  EXPECT_EQ("draining", msg->state);
}

TEST_F(HandoffCoordinatorTest, OpsSetStateToleratesAMissingChannel) {
  auto ops = direct_ops();
  ops->set_role(hf::role_e::draining);
  hf::runtime().set_channel(nullptr);
  EXPECT_NO_THROW(ops->set_state(hf::state_e::draining)) << "a failed or impossible send is only a Debug line";
}

TEST_F(HandoffCoordinatorTest, OpsRemoveNewInstanceFileRemovesTheChildsFile) {
  const auto file = inst_dir / (std::to_string(child_pid_value) + ".json");
  std::ofstream(file) << "{}";
  ASSERT_TRUE(std::filesystem::exists(file));
  auto ops = direct_ops();
  ops->remove_new_instance_file();
  EXPECT_FALSE(std::filesystem::exists(file)) << "the killed new process's instance file must be removed";
}

// =====================================================================================================================
// sunshine_old_ops_t: UPnP cleanup, handover, sockets, recheck, misc
// =====================================================================================================================

TEST_F(HandoffCoordinatorTest, OpsStartUpnpCleanupRemovesTheNewBasesUdpPortsWhenUpnpIsOn) {
  config::sunshine.flags[config::flag::UPNP] = true;
  std::set<std::uint16_t> got;
  remove_ports_hook = [&](std::set<std::uint16_t> p) {
    got = std::move(p);
  };
  auto ops = direct_ops(other_base, own_port);
  ops->start_upnp_cleanup(other_base);
  EXPECT_EQ(1, events.count("remove_ports"));
  EXPECT_EQ(hf::upnp_ports_to_remove(other_base, own_port, own_port), got) << "the set is udp(new) - udp(old) - tcp(port)";
  EXPECT_EQ(3u, got.size());
}

TEST_F(HandoffCoordinatorTest, OpsStartUpnpCleanupIsSkippedWhenUpnpIsOff) {
  config::sunshine.flags[config::flag::UPNP] = false;
  remove_ports_hook = [&](std::set<std::uint16_t>) {
  };
  auto ops = direct_ops(other_base, own_port);
  ops->start_upnp_cleanup(other_base);
  EXPECT_EQ(0, events.count("remove_ports")) << "UPnP off: no removal";
}

TEST_F(HandoffCoordinatorTest, OpsStartUpnpCleanupIsSkippedWhenTheSetIsEmpty) {
  config::sunshine.flags[config::flag::UPNP] = true;
  remove_ports_hook = [&](std::set<std::uint16_t>) {
  };
  auto ops = direct_ops(own_port, own_port);
  ops->start_upnp_cleanup(own_port);
  EXPECT_EQ(0, events.count("remove_ports")) << "equal bases: nothing to remove, so no call";
}

TEST_F(HandoffCoordinatorTest, OpsHandoverSendsMainPidBarrierThenReadBackAndSucceeds) {
  auto ops = direct_ops();
  EXPECT_TRUE(ops->handover(child_pid_value));
  const auto v = events.all();
  ASSERT_EQ(3u, v.size()) << "handover is exactly notify, barrier, read-back";
  EXPECT_EQ("notify:" + hf::sd::mainpid_payload(child_pid_value), v[0]);
  EXPECT_EQ("barrier", v[1]);
  EXPECT_EQ("read_back:" + *hf::sd::unit_from_cgroup(cgroup_text()) + ":system", v[2]);
}

TEST_F(HandoffCoordinatorTest, OpsHandoverReadsNotifySocketAtEachCall) {
  auto ops = direct_ops();
  ops->handover(child_pid_value);
  ops->handover(child_pid_value);
  EXPECT_EQ(2, cnt_notify_socket.load()) << "NOTIFY_SOCKET is read now, at each call (the environment is never scrubbed of it)";
}

TEST_F(HandoffCoordinatorTest, OpsHandoverFailsWhenTheReadBackIsAnotherPidOrMissing) {
  auto ops = direct_ops();
  read_back_pid = child_pid_value + 1;
  EXPECT_FALSE(ops->handover(child_pid_value)) << "the read-back must equal the new pid";
  read_back_pid = -1;
  EXPECT_FALSE(ops->handover(child_pid_value)) << "no read-back value is a failure";
}

TEST_F(HandoffCoordinatorTest, OpsHandoverStopsAtTheFirstFailedStep) {
  auto ops = direct_ops();
  notify_ok = false;
  EXPECT_FALSE(ops->handover(child_pid_value));
  EXPECT_EQ(0, events.count("barrier")) << "a failed MAINPID notify must not go on to the barrier";
  notify_ok = true;
  barrier_ok = false;
  events.clear();
  EXPECT_FALSE(ops->handover(child_pid_value));
  EXPECT_EQ(0, events.count_prefix("read_back")) << "a failed barrier must not go on to the read-back";
}

TEST_F(HandoffCoordinatorTest, OpsCloseSocketCopiesClosesAllFourAndGoesOnAfterAFailure) {
  lf[1].close_ok = false;
  auto ops = direct_ops();
  EXPECT_NO_THROW(ops->close_socket_copies());
  for (const auto &l : lf) {
    EXPECT_EQ(1, l.closes.load());
    EXPECT_GT(l.last_close_ms.load(), 0);
    EXPECT_LE(l.last_close_ms.load(), 5000);
  }
}

TEST_F(HandoffCoordinatorTest, OpsRecheckRefusesAnAppThatIsNotAdoptable) {
  app_adoptable_value = false;
  auto ops = direct_ops();
  EXPECT_EQ(std::string {hf::refusal_reason::app_generic}, ops->recheck(30ms));
  app_why = "a launcher is still running";
  EXPECT_EQ(std::string {hf::refusal_reason::app_prefix} + "a launcher is still running", ops->recheck(30ms));
}

TEST_F(HandoffCoordinatorTest, OpsRecheckStopsAtTheFirstFailingStep) {
  app_adoptable_value = false;
  pairing_value = 3;
  auto ops = direct_ops();
  EXPECT_EQ(std::string {hf::refusal_reason::app_generic}, ops->recheck(30ms)) << "the app check decides before the pairing check";
  EXPECT_EQ(0, cnt_pairing.load()) << "the pairing count must not be read after the app check failed";
}

TEST_F(HandoffCoordinatorTest, OpsRecheckWaitsForPairingThenGivesThePairingReason) {
  pairing_value = 2;
  auto ops = direct_ops();
  const auto wall_start = std::chrono::steady_clock::now();
  const auto fake_start = clk.now();
  EXPECT_EQ(std::string {hf::refusal_reason::pairing}, ops->recheck(60ms));
  const auto fake_elapsed = clk.now() - fake_start;
  EXPECT_GE(fake_elapsed, 60ms) << "the pairing wait lasts at least pairing_wait on the sources' clock";
  EXPECT_LT(fake_elapsed, 60ms + hf::recheck_poll_period) << "and not more than one poll period longer";
  EXPECT_GT(sleep_calls.load(), 0) << "the wait polls through the sleep seam";
  EXPECT_LE(sleep_max_ms.load(), hf::recheck_poll_period.count()) << "the poll period is at most recheck_poll_period";
  EXPECT_LT(std::chrono::steady_clock::now() - wall_start, 1500ms) << "guard: the wait must not hang";
}

TEST_F(HandoffCoordinatorTest, OpsRecheckSucceedsWhenPairingEndsDuringTheWait) {
  pairing_value = 1;
  on_sleep = [this]() {
    pairing_value = 0;
  };
  auto ops = direct_ops();
  EXPECT_EQ("", ops->recheck(1000ms)) << "a pairing that ends within the wait lets the switch go on";
}

TEST_F(HandoffCoordinatorTest, OpsRecheckDropsAPendingLaunchWithoutRefusing) {
  pending_value = true;
  auto ops = direct_ops();
  EXPECT_EQ("", ops->recheck(30ms)) << "a pending launch is dropped, not a failure";
  EXPECT_EQ(1, events.count("drop_pending_launch"));
}

TEST_F(HandoffCoordinatorTest, OpsRecheckDropsNothingWhenNoLaunchIsPending) {
  auto ops = direct_ops();
  EXPECT_EQ("", ops->recheck(30ms));
  EXPECT_EQ(0, events.count("drop_pending_launch")) << "control for the test above: no pending launch, no drop";
}

TEST_F(HandoffCoordinatorTest, OpsPassThroughTheSimpleModuleCalls) {
  auto ops = direct_ops();
  ops->raise_gate();
  ops->lower_gate();
  EXPECT_FALSE(((in_flight_ok = false), ops->wait_in_flight(100ms)));
  EXPECT_TRUE(((in_flight_ok = true), ops->wait_in_flight(-1ms)));
  EXPECT_TRUE(ops->save_state());
  save_ok = false;
  EXPECT_FALSE(ops->save_state()) << "a failed state save fails the switch";
  ops->withdraw_mdns(50ms);
  ops->publish_mdns();
  ops->remove_tray();
  ops->restore_tray();
  ops->set_released(true);
  ops->set_released(false);
  ops->drop_app();
  const auto go = ops->take_snapshot();
  EXPECT_TRUE(go.host_audio);
  EXPECT_EQ(42u, go.next_launch_session_id);
  for (const auto *e : {"gate_up", "gate_down", "wait_in_flight:100", "wait_in_flight:-1", "mdns_withdraw", "mdns_start", "tray_suspend", "tray_resume", "set_released:1", "set_released:0", "drop_app"}) {
    EXPECT_GE(events.first(e), 0) << e << " was not performed";
  }
}

TEST_F(HandoffCoordinatorTest, OpsShutdownRequestedFollowsTheRuntime) {
  auto ops = direct_ops();
  EXPECT_FALSE(ops->shutdown_requested());
  hf::runtime().request_shutdown();
  EXPECT_TRUE(ops->shutdown_requested());
}

TEST_F(HandoffCoordinatorTest, OpsMapAThrowingModuleToTheFailureValue) {
  auto ops = direct_ops();
  throw_in_save = true;
  throw_in_reload = true;
  throw_in_notify = true;
  EXPECT_NO_THROW(EXPECT_FALSE(ops->save_state())) << "an exception from a module is caught and mapped to false";
  EXPECT_NO_THROW(EXPECT_FALSE(ops->reload_after_rollback()));
  EXPECT_NO_THROW(EXPECT_FALSE(ops->handover(child_pid_value)));
}

TEST_F(HandoffCoordinatorTest, OpsReloadAfterRollbackIsFalseWhenTheReloadFails) {
  auto ops = direct_ops();
  EXPECT_TRUE(ops->reload_after_rollback());
  reload_ok = false;
  EXPECT_FALSE(ops->reload_after_rollback());
}

// =====================================================================================================================
// coordinator_t::handle(): refusals
// =====================================================================================================================

TEST_F(HandoffCoordinatorTest, DefaultCooldownIsNone) {
  EXPECT_EQ(tp_t::min(), C().cooldown_until()) << "no cooldown was armed or inherited";
  EXPECT_FALSE(C().attempt_running());
  EXPECT_FALSE(C().handed_over());
}

TEST_F(HandoffCoordinatorTest, NothingRefusesAHealthyRequestAndItSpawns) {
  // Control for every refusal test below: with every input healthy the request gets past the checks and spawns.
  start_handle();
  ASSERT_TRUE(attach_peer());
  EXPECT_EQ(1, spawn_calls.load()) << "a request with no blocker must reach the spawn";
  EXPECT_EQ(1, sources_total() > 0 ? 1 : 0);
}

TEST_F(HandoffCoordinatorTest, ShutdownRequestedRefusesWithTheShuttingDownReason) {
  hf::runtime().request_shutdown();
  expect_refused(5, hf::coordinator_reason::shutting_down);
}

TEST_F(HandoffCoordinatorTest, SigtermRefusesWithTheShuttingDownReason) {
  hf::runtime().note_sigterm();
  expect_refused(5, hf::coordinator_reason::shutting_down);
}

TEST_F(HandoffCoordinatorTest, RequestShutdownOfTheCoordinatorRefusesNewRequests) {
  C().request_shutdown();
  expect_refused(5, hf::coordinator_reason::shutting_down);
}

TEST_F(HandoffCoordinatorTest, StopRefusesNewRequests) {
  C().stop();
  expect_refused(5, hf::coordinator_reason::shutting_down);
}

TEST_F(HandoffCoordinatorTest, DrainingModeRefusesWithTheDrainingReason) {
  ASSERT_TRUE(hf::runtime().transition(hf::mode_e::normal, hf::mode_e::draining));
  expect_refused(5, hf::coordinator_reason::draining);
}

TEST_F(HandoffCoordinatorTest, DisabledModeRefusesWithTheNotActiveReason) {
  hf::runtime().reset_for_test();
  expect_refused(5, hf::coordinator_reason::not_active);
}

TEST_F(HandoffCoordinatorTest, NewPendingAndNewActiveModesRefuseWithInProgress) {
  hf::runtime().reset_for_test();
  std::map<std::string, std::string> vars {{"SUNSHINE_HANDOFF_FDS", "https:3,http:4,webui:5,rtsp:6"}, {"SUNSHINE_HANDOFF_CHANNEL", "7"}, {"SUNSHINE_HANDOFF_UDP_BASE", "48089"}, {"SUNSHINE_HANDOFF_GENERATION", "1"}};
  hf::early_init_env_t env;
  env.getenv = [&](const char *name) -> const char * {
    const auto it = vars.find(name);
    return it == vars.end() ? nullptr : it->second.c_str();
  };
  env.unsetenv = [](const char *) {
  };
  env.scrub_fds = []() {
  };
  ASSERT_EQ("", hf::runtime().early_init(env));
  ASSERT_EQ(hf::mode_e::new_pending, hf::runtime().mode());
  expect_refused(5, hf::refusal_reason::in_progress);
  ASSERT_TRUE(hf::runtime().transition(hf::mode_e::new_pending, hf::mode_e::new_active));
  expect_refused(5, hf::refusal_reason::in_progress);
}

TEST_F(HandoffCoordinatorTest, EachRefusalInputGivesItsOwnReason) {
  struct row_t {
    const char *what;
    std::function<void()> apply;
    std::string reason;
  };

  hf::instance_t other;
  other.pid = 9100;
  other.start_time = 1;
  const std::vector<row_t> rows {
    {"handoff in progress", [&]() {
       hf::runtime().set_handoff_in_progress(true);
     },
     std::string {hf::refusal_reason::in_progress}},
    {"draining peer", [&]() {
       peer_exists_value = true;
     },
     std::string {hf::refusal_reason::drain_exists}},
    {"cooldown", [&]() {
       C().set_cooldown_until(clk.at(30s));
     },
     std::string {hf::refusal_reason::cooldown}},
    {"no NOTIFY_SOCKET", [&]() {
       notify_value = "";
     },
     std::string {hf::refusal_reason::no_notify_socket}},
    {"systemd unknown", [&]() {
       systemd_value = std::nullopt;
     },
     std::string {hf::refusal_reason::systemd_unknown}},
    {"systemd old", [&]() {
       systemd_value = 253;
     },
     std::string {hf::refusal_reason::systemd_old}},
    {"no unit", [&]() {
       cgroup_value = "0::/\n";
     },
     std::string {hf::refusal_reason::no_unit}},
    {"app", [&]() {
       app_adoptable_value = false;
     },
     std::string {hf::refusal_reason::app_generic}},
    {"pairing", [&]() {
       pairing_value = 1;
     },
     std::string {hf::refusal_reason::pairing}},
    {"pending launch", [&]() {
       pending_value = true;
     },
     std::string {hf::refusal_reason::pending_launch}},
  };
  for (const auto &row : rows) {
    SCOPED_TRACE(row.what);
    // A fresh coordinator and fresh inputs for every row.
    coord.reset();
    hf::runtime().set_handoff_in_progress(false);
    notify_value = "/run/test-notify";
    systemd_value = 255;
    cgroup_value = cgroup_text();
    peer_exists_value = false;
    app_adoptable_value = true;
    pairing_value = 0;
    pending_value = false;
    spawn_calls = 0;
    row.apply();
    expect_refused(5, row.reason);
  }
}

TEST_F(HandoffCoordinatorTest, AnotherLiveInstanceWithRolePendingOrDrainingRefuses) {
  hf::instance_t pending;
  pending.pid = 9100;
  pending.start_time = 1;
  pending.role = hf::role_e::pending;
  scan_value.live = {pending};
  expect_refused(5, hf::refusal_reason::in_progress);
  coord.reset();
  hf::instance_t draining;
  draining.pid = 9101;
  draining.start_time = 1;
  draining.role = hf::role_e::draining;
  scan_value.live = {draining};
  expect_refused(5, hf::refusal_reason::drain_exists);
}

TEST_F(HandoffCoordinatorTest, RefusalOrderFollowsTheDocumentedRanking) {
  set_everything_wrong();
  const std::vector<std::string> expected {
    std::string {hf::refusal_reason::kill_pending},
    std::string {hf::refusal_reason::in_progress},
    std::string {hf::refusal_reason::drain_exists},
    std::string {hf::refusal_reason::cooldown},
    std::string {hf::refusal_reason::no_notify_socket},
    std::string {hf::refusal_reason::systemd_old},
    std::string {hf::refusal_reason::no_unit},
    std::string {hf::refusal_reason::app_prefix} + "a launcher is still running",
    std::string {hf::refusal_reason::pairing},
    std::string {hf::refusal_reason::pending_launch},
  };
  // Peel the blockers off one at a time, in the documented order.
  const std::vector<std::function<void()>> peel {
    [&]() {
      scan_value.live.erase(scan_value.live.begin());  // kill_pending instance gone (pending one stays)
    },
    [&]() {
      scan_value.live.clear();
      hf::runtime().set_handoff_in_progress(false);
    },
    [&]() {
      peer_exists_value = false;
    },
    [&]() {
      clk.advance(61s);
    },
    [&]() {
      notify_value = "/run/test-notify";
    },
    [&]() {
      systemd_value = 255;
    },
    [&]() {
      cgroup_value = cgroup_text();
    },
    [&]() {
      app_adoptable_value = true;
    },
    [&]() {
      pairing_value = 0;
    },
    [&]() {
      pending_value = false;
    },
  };
  for (std::size_t i = 0; i < expected.size(); ++i) {
    SCOPED_TRACE("step " + std::to_string(i));
    const auto reply = C().handle(5);
    EXPECT_EQ(hf::reply_kind_e::refused, reply.kind);
    EXPECT_EQ(expected[i], reply.reason) << "the first applicable row of the ranking must win";
    EXPECT_EQ(0, spawn_calls.load()) << "no refusal may spawn";
    peel[i]();
  }
  // Everything is healthy now: the request is no longer refused (positive control).
  start_handle();
  EXPECT_TRUE(attach_peer()) << "with every blocker gone the request must spawn";
}

TEST_F(HandoffCoordinatorTest, KillPendingInTheScanRanksFirstEvenWithADrainAndTheCooldown) {
  set_everything_wrong();
  const auto reply = C().handle(5);
  EXPECT_EQ(hf::reply_kind_e::refused, reply.kind);
  EXPECT_EQ(std::string {hf::refusal_reason::kill_pending}, reply.reason);
  EXPECT_EQ(0, spawn_calls.load());
}

TEST_F(HandoffCoordinatorTest, EverySourceIsReadExactlyOnceBeforeTheDecision) {
  set_everything_wrong();
  C().handle(5);
  EXPECT_EQ(1, cnt_notify.load());
  EXPECT_EQ(1, cnt_version.load());
  EXPECT_EQ(1, cnt_cgroup.load());
  EXPECT_EQ(1, cnt_inprogress.load());
  EXPECT_EQ(1, cnt_peer.load());
  EXPECT_EQ(1, cnt_scan.load()) << "the instance scan is read once, not once per field";
  EXPECT_EQ(1, cnt_app.load());
  EXPECT_EQ(1, cnt_pairing.load());
  EXPECT_EQ(1, cnt_pending.load());
}

TEST_F(HandoffCoordinatorTest, ARefusalDoesNotArmTheCooldownAndALaterRequestIsNotRefusedForIt) {
  app_adoptable_value = false;
  expect_refused(5, hf::refusal_reason::app_generic);
  app_adoptable_value = true;
  start_handle();
  ASSERT_TRUE(attach_peer()) << "the earlier refusal must not have started a cooldown: the next request spawns";
  EXPECT_EQ(1, spawn_calls.load());
}

TEST_F(HandoffCoordinatorTest, ARefusalForTheCooldownDoesNotExtendIt) {
  C().set_cooldown_until(clk.at(30s));
  clk.advance(10s);
  expect_refused(5, hf::refusal_reason::cooldown);
  EXPECT_EQ(clk.at(30s), C().cooldown_until()) << "a refusal never arms or extends the cooldown";
}

TEST_F(HandoffCoordinatorTest, RequestsAreAcceptedAgainWhenTheCooldownHasPassed) {
  C().set_cooldown_until(clk.at(30s));
  clk.advance(31s);
  start_handle();
  ASSERT_TRUE(attach_peer()) << "after the deadline the cooldown no longer refuses";
}

TEST_F(HandoffCoordinatorTest, AMissingListenerRefusesBeforeAnythingStartsAndNamesIt) {
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    SCOPED_TRACE(std::string {hf::to_string(kinds[i])});
    coord.reset();
    for (auto &l : lf) {
      l.fd = 40;
    }
    lf[i].fd = -1;
    expect_refused(5, std::string {hf::coordinator_reason::listener_missing_prefix} + std::string {hf::to_string(kinds[i])});
  }
}

TEST_F(HandoffCoordinatorTest, AnUnregisteredListenerAlsoRefuses) {
  hf::listeners().clear(hf::listener_e::rtsp);
  expect_refused(5, std::string {hf::coordinator_reason::listener_missing_prefix} + "rtsp");
}

TEST_F(HandoffCoordinatorTest, KillPendingStateOfThisProcessRefusesWithTheKillPendingReason) {
  ASSERT_TRUE(hf::runtime().instance_set_state(hf::state_e::kill_pending));
  const auto reply = C().handle(5);
  EXPECT_EQ(hf::reply_kind_e::refused, reply.kind);
  EXPECT_EQ(std::string {hf::refusal_reason::kill_pending}, reply.reason) << "kill_pending has its own reason, never in_progress";
  EXPECT_EQ(0, spawn_calls.load());
  EXPECT_EQ(hf::state_e::kill_pending, hf::runtime().instance_current()->handoff) << "a refusal never clears kill_pending";
}

TEST_F(HandoffCoordinatorTest, ARefusalThroughTheHandlerFunctionIsTheSameAsHandle) {
  notify_value = "";
  const auto reply = C().handler()(5);
  EXPECT_EQ(hf::reply_kind_e::refused, reply.kind);
  EXPECT_EQ(std::string {hf::refusal_reason::no_notify_socket}, reply.reason);
  EXPECT_EQ(0, spawn_calls.load());
}

TEST_F(HandoffCoordinatorTest, ASecondOverlappingRequestIsAnsweredInProgressWithoutGathering) {
  start_handle();
  ASSERT_TRUE(attach_peer());
  const int sources_before = sources_total();
  const auto second = C().handle(555);
  EXPECT_EQ(hf::reply_kind_e::refused, second.kind);
  EXPECT_EQ(std::string {hf::refusal_reason::in_progress}, second.reason) << "an overlapping request is answered in_progress at once";
  EXPECT_EQ(sources_before, sources_total()) << "no gatherer is called for an overlapping request";
  EXPECT_EQ(1, spawn_calls.load()) << "an overlapping request starts nothing";
  EXPECT_EQ(1, channel_calls.load());
  // The first attempt is undisturbed and still reaches COMMIT, answered committed exactly once.
  ASSERT_TRUE(peer_to_commit());
  const auto first = get_reply();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(hf::reply_kind_e::committed, first->kind);
}

TEST_F(HandoffCoordinatorTest, AnOverlappingRequestDoesNotTouchTheCooldown) {
  start_handle();
  ASSERT_TRUE(attach_peer());
  const auto before = C().cooldown_until();
  clk.advance(30s);
  C().handle(555);
  EXPECT_EQ(before, C().cooldown_until()) << "an in_progress refusal must not arm or move the cooldown";
}

// =====================================================================================================================
// coordinator_t::handle(): spawn request and failures before the run
// =====================================================================================================================

TEST_F(HandoffCoordinatorTest, SpawnRequestCarriesBinaryArgvEnvironmentAndFds) {
  settings.binary = "";
  start_handle();
  ASSERT_TRUE(attach_peer());
  hf::spawn_request_t req;
  {
    std::lock_guard lock {spawn_mutex};
    req = last_req;
  }
  EXPECT_EQ("/proc/self/exe", req.binary) << "an empty drain_handoff_binary means the path Sunshine was started from";
  EXPECT_EQ((std::vector<std::string> {"sunshine", "--config", "x"}), req.argv) << "argv is passed unchanged";
  const auto env = hf::child_environment(hf::other_udp_base(own_port, 100, hf::hooks::udp_base()), hf::runtime().generation() + 1);
  ASSERT_GE(req.extra_env.size(), env.size());
  for (std::size_t i = 0; i < env.size(); ++i) {
    EXPECT_EQ(env[i], req.extra_env[i]) << "the child environment is the documented one, in order";
  }
  EXPECT_EQ(env.size(), req.extra_env.size()) << "with no recorded host sink nothing follows the child environment";
  EXPECT_EQ(31, req.fds.https);
  EXPECT_EQ(32, req.fds.http);
  EXPECT_EQ(33, req.fds.webui);
  EXPECT_EQ(34, req.fds.rtsp);
  EXPECT_GE(req.fds.channel, 0) << "the child's end of the channel is passed";
  EXPECT_EQ(other_base, last_new_base.load()) << "the old side is built for the child's UDP base (port + udp_offset)";
}

TEST_F(HandoffCoordinatorTest, SpawnRequestUsesTheConfiguredBinary) {
  settings.binary = "/opt/test/sunshine-new";
  hf::settings_t s = settings;
  hf::runtime().configure(s);
  start_handle();
  ASSERT_TRUE(attach_peer());
  std::lock_guard lock {spawn_mutex};
  EXPECT_EQ("/opt/test/sunshine-new", last_req.binary);
}

TEST_F(HandoffCoordinatorTest, SpawnRequestPassesTheRecordedHostSinkUnlessItIsASunshineSink) {
  host_sink_value = "alsa_output.pci-0000_00_1f.3.analog-stereo";
  start_handle();
  ASSERT_TRUE(attach_peer());
  {
    std::lock_guard lock {spawn_mutex};
    ASSERT_FALSE(last_req.extra_env.empty());
    EXPECT_EQ("SUNSHINE_HANDOFF_HOST_SINK=alsa_output.pci-0000_00_1f.3.analog-stereo", last_req.extra_env.back()) << "the host sink follows the child environment";
  }
}

TEST_F(HandoffCoordinatorTest, SpawnRequestOmitsAnEmptyOrSunshineHostSink) {
  for (const char *sink : {"", "sink-sunshine-stereo"}) {
    SCOPED_TRACE(std::string {"sink '"} + sink + "'");
    coord.reset();
    spawn_calls = 0;
    if (peer) {
      peer->channel.close();
      peer.reset();
    }
    clk.advance(0ms);
    host_sink_value = std::string {sink};
    start_handle();
    ASSERT_TRUE(attach_peer());
    {
      std::lock_guard lock {spawn_mutex};
      for (const auto &e : last_req.extra_env) {
        EXPECT_NE(0u, e.rfind("SUNSHINE_HANDOFF_HOST_SINK=", 0) == 0 ? 0u : 1u) << "no host sink variable may be set for '" << sink << "'";
      }
    }
    peer->channel.close();
    fut.wait_for(2000ms);
  }
}

TEST_F(HandoffCoordinatorTest, SpawnRequestWithoutAHostSinkOptionalLeavesTheVariableOut) {
  host_sink_value.clear();
  start_handle();
  ASSERT_TRUE(attach_peer());
  std::lock_guard lock {spawn_mutex};
  for (const auto &e : last_req.extra_env) {
    EXPECT_NE(0u, e.rfind("SUNSHINE_HANDOFF_HOST_SINK=", 0) == 0 ? 0u : 1u) << "std::nullopt and an empty name alike: no variable";
  }
}

TEST_F(HandoffCoordinatorTest, AttemptStateIsSetBeforeTheSpawn) {
  start_handle();
  ASSERT_TRUE(attach_peer());
  EXPECT_TRUE(in_progress_at_spawn.load()) << "set_handoff_in_progress(true) comes before the spawn";
  EXPECT_TRUE(channel_set_at_spawn.load()) << "the channel is stored in the runtime before the spawn";
  EXPECT_EQ(hf::end_state_e::idle, end_all_at_spawn) << "end_all.reset() runs before the spawn";
  EXPECT_TRUE(C().attempt_running());
  EXPECT_TRUE(hf::runtime().handoff_in_progress());
}

TEST_F(HandoffCoordinatorTest, SpawnFailureAnswersFailedWithThePrefixAndTheReason) {
  spawn_fail = true;
  const auto reply = C().handle(5);
  EXPECT_EQ(hf::reply_kind_e::failed, reply.kind);
  EXPECT_EQ(std::string {hf::coordinator_reason::spawn_failed_prefix} + "exec failed", reply.reason);
  EXPECT_FALSE(hf::runtime().handoff_in_progress()) << "a failed attempt clears handoff_in_progress";
  EXPECT_EQ(nullptr, hf::runtime().channel()) << "a failed attempt drops the channel";
  EXPECT_FALSE(C().attempt_running()) << "the claim is released after a failed attempt";
  EXPECT_EQ(0, make_ops_calls.load()) << "the old side never started";
}

TEST_F(HandoffCoordinatorTest, ChannelFailureAnswersFailedAndSpawnsNothing) {
  channel_fail = true;
  const auto reply = C().handle(5);
  EXPECT_EQ(hf::reply_kind_e::failed, reply.kind);
  EXPECT_EQ(std::string {hf::coordinator_reason::channel_failed}, reply.reason);
  EXPECT_EQ(0, spawn_calls.load());
  EXPECT_FALSE(hf::runtime().handoff_in_progress());
  EXPECT_FALSE(C().attempt_running());
}

TEST_F(HandoffCoordinatorTest, TheParentsCopyOfTheChildsChannelEndIsClosedAfterAFailedSpawn) {
  keep_channel_dups = true;
  spawn_fail = true;
  const auto reply = C().handle(5);
  ASSERT_EQ(hf::reply_kind_e::failed, reply.kind);
  // The test closed its own copy of the child's end inside the spawn seam; the other kept fd is a copy of the old
  // process's end. Once the coordinator closed its copies of the child's end, that copy sees the peer gone.
  const int old_end_copy = keep_a >= 0 ? keep_a : keep_b;
  ASSERT_GE(old_end_copy, 0);
  hf::channel_t probe {::fcntl(old_end_copy, F_DUPFD_CLOEXEC, 100)};
  hf::received_t r;
  EXPECT_EQ(hf::recv_status_e::eof, probe.receive(r, 500ms)) << "the parent must close its copy of the child's channel end after the spawn, success or not";
}

TEST_F(HandoffCoordinatorTest, UnreadableChildStartTimeKillsTheChildFirstAndFails) {
  use_real_child = true;
  start_time_result = std::nullopt;
  const auto reply = C().handle(5);
  EXPECT_EQ(hf::reply_kind_e::failed, reply.kind);
  EXPECT_EQ(std::string {hf::coordinator_reason::start_time_unreadable}, reply.reason);
  ASSERT_GT(real_pid, 0);
  EXPECT_TRUE(wait_until([this]() {
    errno = 0;
    return ::kill(real_pid, 0) == -1 && errno == ESRCH;
  }))
    << "the child must be killed and reaped before the failed answer";
  EXPECT_EQ(0, make_child_calls.load()) << "no pidfd_child_t is built for a child that failed the start-time check";
  EXPECT_EQ(0, make_ops_calls.load());
  EXPECT_FALSE(hf::runtime().handoff_in_progress());
}

TEST_F(HandoffCoordinatorTest, AShutdownDuringTheSpawnKillsTheChildFirstAndAnswersFailed) {
  // SPEC-QUESTION: the doc says "the child is killed first (kill_and_reap, one reap wait)" after the pidfd_child_t
  // took the pidfd; the test assumes that kill goes through the child_ops_t that make_child returned.
  on_spawn = []() {
    hf::runtime().request_shutdown();
  };
  const auto reply = C().handle(5);
  EXPECT_EQ(hf::reply_kind_e::failed, reply.kind);
  EXPECT_EQ(std::string {hf::coordinator_reason::shutting_down}, reply.reason);
  EXPECT_GE(child_state->kills.load(), 1) << "the child must be killed first";
  EXPECT_EQ(0, make_ops_calls.load()) << "the old side must not start after a shutdown";
  EXPECT_FALSE(C().attempt_running());
}

// =====================================================================================================================
// coordinator_t::handle(): the answer, exactly once
// =====================================================================================================================

TEST_F(HandoffCoordinatorTest, CommittedIsAnsweredOnlyAfterCommitIsSent) {
  start_handle();
  ASSERT_TRUE(attach_peer());
  ASSERT_TRUE(peer_to_go());
  EXPECT_EQ(std::future_status::timeout, fut.wait_for(150ms)) << "the helper must not be answered before COMMIT (GO is sent, no GO_ACK yet)";
  ASSERT_TRUE(peer->send(protocol::go_ack_t {true, ""}));
  ASSERT_TRUE(peer->expect<protocol::commit_t>().has_value());
  const auto reply = get_reply();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(hf::reply_kind_e::committed, reply->kind) << "the helper is answered committed after COMMIT";
  EXPECT_EQ(child_pid_value, reply->new_pid) << "committed carries the child's pid";
  EXPECT_EQ(child_start_value, reply->new_start_time) << "committed carries the child's start time";
}

TEST_F(HandoffCoordinatorTest, ACommittedAnswerIsNotFollowedByAnotherWhenTheRollbackFollows) {
  ASSERT_TRUE(run_to_commit());
  const auto reply = get_reply();
  ASSERT_TRUE(reply.has_value());
  ASSERT_EQ(hf::reply_kind_e::committed, reply->kind);
  peer->channel.close();
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }))
    << "the attempt must end after the channel closed";
  EXPECT_FALSE(fut.valid()) << "the one request was answered once; the later rollback has no helper left to answer";
}

TEST_F(HandoffCoordinatorTest, AFailedStartIsAnsweredFailedWithAReason) {
  start_handle();
  ASSERT_TRUE(attach_peer());
  ASSERT_TRUE(peer_handshake());
  ASSERT_TRUE(peer->send(protocol::ready_t {false, "no gpu"}));
  const auto reply = get_reply();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(hf::reply_kind_e::failed, reply->kind);
  EXPECT_FALSE(reply->reason.empty()) << "a failed answer carries a reason";
  EXPECT_GE(child_state->kills.load(), 1) << "the failed start kills the child first";
  EXPECT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  EXPECT_FALSE(hf::runtime().handoff_in_progress());
}

TEST_F(HandoffCoordinatorTest, APauseThatDoesNotConfirmNamesTheServerAndRollsBack) {
  lf[1].pause_ok = false;
  start_handle();
  ASSERT_TRUE(attach_peer());
  ASSERT_TRUE(peer_ready());
  const auto reply = get_reply();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(hf::reply_kind_e::failed, reply->kind) << "a failed pause is a failed switch, not a refusal";
  EXPECT_NE(std::string::npos, reply->reason.find("the http server did not confirm the pause in time")) << "the reason must name the server; got: " << reply->reason;
  EXPECT_EQ(1, lf[0].pauses.load());
  EXPECT_EQ(1, lf[1].pauses.load());
  EXPECT_EQ(0, lf[2].pauses.load()) << "listeners after the failing one are not paused";
  EXPECT_EQ(0, lf[3].pauses.load());
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  for (const auto &l : lf) {
    EXPECT_GE(l.resumes.load(), 1) << "the rollback resumes every listener, also those never paused";
  }
  EXPECT_GE(child_state->kills.load(), 1) << "the new process is killed";
  EXPECT_EQ(0, events.count("gate_up")) << "the switch stopped before the gate was raised";
  EXPECT_FALSE(hf::runtime().handoff_in_progress());
  EXPECT_NE(tp_t::min(), C().cooldown_until());
}

TEST_F(HandoffCoordinatorTest, EveryPauseUsesABoundedTimeoutInTheSwitch) {
  ASSERT_TRUE(run_to_commit());
  for (const auto &l : lf) {
    EXPECT_GT(l.last_pause_ms.load(), 0);
    EXPECT_LE(l.last_pause_ms.load(), 5000) << "pause_accept passes a finite timeout, at most the in-flight budget";
  }
}

TEST_F(HandoffCoordinatorTest, AResumeTimeoutInTheRollbackDoesNotBlockIt) {
  lf[0].resume_ok = false;
  start_handle();
  ASSERT_TRUE(attach_peer());
  ASSERT_TRUE(peer_to_go());
  ASSERT_TRUE(peer->send(protocol::go_ack_t {false, "adoption failed"}));
  const auto start = std::chrono::steady_clock::now();
  const auto reply = get_reply();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(hf::reply_kind_e::failed, reply->kind);
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }))
    << "a resume that does not confirm must not block the rollback";
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1500ms);
  for (const auto &l : lf) {
    EXPECT_EQ(1, l.resumes.load()) << "all four are resumed, a failure does not stop the loop";
  }
  EXPECT_EQ(1, events.count("mdns_start")) << "the rollback went on to republish mDNS";
  EXPECT_EQ(1, events.count("tray_resume")) << "the rollback went on to restore the tray";
}

TEST_F(HandoffCoordinatorTest, ShutdownAfterGoBeforeTheAckAnswersFailedAndRollsNothingBack) {
  // SPEC-QUESTION: the exact reason of the helper's answer when the shutdown arrives after GO and before GO_ACK. The
  // coordinator doc says the helper "gets coordinator_reason::no_outcome" when no committed answer was given; this test
  // only requires a failed answer with a reason.
  start_handle();
  ASSERT_TRUE(attach_peer());
  ASSERT_TRUE(peer_to_go());
  hf::runtime().request_shutdown();
  const auto reply = get_reply();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(hf::reply_kind_e::failed, reply->kind) << "a shutdown after GO with no committed answer is answered failed";
  EXPECT_FALSE(reply->reason.empty());
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  EXPECT_EQ(0, child_state->kills.load()) << "after GO a shutdown means no rollback: the child is left to the unit's stop";
  for (const auto &l : lf) {
    EXPECT_EQ(0, l.resumes.load()) << "no rollback effects after GO and a shutdown";
  }
  EXPECT_EQ(0, events.count("mdns_start"));
}

TEST_F(HandoffCoordinatorTest, AShutdownBeforeGoRollsBackKillFirstAndAnswersFailed) {
  start_handle();
  ASSERT_TRUE(attach_peer());
  ASSERT_TRUE(peer_handshake());  // READY has not been sent yet
  hf::runtime().request_shutdown();
  const auto reply = get_reply();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(hf::reply_kind_e::failed, reply->kind);
  EXPECT_GE(child_state->kills.load(), 1) << "before GO a shutdown aborts with the kill-first rollback";
  EXPECT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
}

TEST_F(HandoffCoordinatorTest, ASigtermBeforeGoAbortsTheSwitchLikeAnyShutdown) {
  start_handle();
  ASSERT_TRUE(attach_peer());
  ASSERT_TRUE(peer_handshake());
  hf::runtime().note_sigterm();
  const auto reply = get_reply();
  ASSERT_TRUE(reply.has_value()) << "the shutdown watcher must forward a SIGTERM to the old side";
  EXPECT_EQ(hf::reply_kind_e::failed, reply->kind);
  EXPECT_GE(child_state->kills.load(), 1);
}

TEST_F(HandoffCoordinatorTest, CoordinatorRequestShutdownAbortsARunningSwitch) {
  start_handle();
  ASSERT_TRUE(attach_peer());
  ASSERT_TRUE(peer_handshake());
  C().request_shutdown();
  const auto reply = get_reply();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(hf::reply_kind_e::failed, reply->kind);
  EXPECT_GE(child_state->kills.load(), 1);
  EXPECT_TRUE(C().join_attempt(1500ms)) << "the attempt ends after the abort";
}

TEST_F(HandoffCoordinatorTest, AShutdownInTheRollbackWindowEndsWithoutHandoverOrRollback) {
  ASSERT_TRUE(run_to_commit());
  peer->start_pump();
  const auto committed = get_reply();
  ASSERT_TRUE(committed.has_value());
  ASSERT_EQ(hf::reply_kind_e::committed, committed->kind);
  C().request_shutdown();
  ASSERT_TRUE(C().join_attempt(1500ms));
  EXPECT_FALSE(C().handed_over()) << "a shutdown in the window means no handover";
  EXPECT_EQ(0, child_state->kills.load()) << "no rollback after GO and a shutdown";
  EXPECT_EQ(0, events.count("drain_handed_over"));
  for (const auto &l : lf) {
    EXPECT_EQ(0, l.resumes.load()) << "no rollback effects";
  }
}

TEST_F(HandoffCoordinatorTest, EndAllMachineIsResetAtTheStartOfEachAttempt) {
  // Attempt 1 fails before COMMIT; its rollback decided "rolling_back".
  start_handle();
  ASSERT_TRUE(attach_peer());
  ASSERT_TRUE(peer_to_go());
  ASSERT_TRUE(peer->send(protocol::go_ack_t {false, "no"}));
  ASSERT_TRUE(get_reply().has_value());
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  EXPECT_EQ(hf::end_state_e::rolling_back, C().end_all_for_test().state()) << "the rollback decided; the state stays for assertions";
  const int responders_before = responder_calls.load();
  // Attempt 2 (after the cooldown): the spawn seam sees a reset machine.
  clk.advance(121s);
  peer.reset();
  spawn_calls = 0;
  child_state->gone = false;
  start_handle();
  ASSERT_TRUE(attach_peer());
  EXPECT_EQ(hf::end_state_e::idle, end_all_at_spawn) << "end_all.reset() must run for the new attempt";
  EXPECT_EQ(responders_before + 1, responder_calls.load()) << "each attempt constructs its own responder";
}

// =====================================================================================================================
// kill_pending
// =====================================================================================================================

TEST_F(HandoffCoordinatorTest, KillPendingAttemptHelperGetsKillPendingAndALaterHelperIsRefusedWithItsReason) {
  child_state->timeouts_left = 1000000;  // the new process cannot be reaped
  start_handle();
  ASSERT_TRUE(attach_peer());
  ASSERT_TRUE(peer_handshake());
  peer->channel.close();  // a failed start: the rollback kills first and cannot reap

  const auto own = get_reply();
  ASSERT_TRUE(own.has_value());
  EXPECT_EQ(hf::reply_kind_e::kill_pending, own->kind) << "the attempt's own helper is answered kill_pending (exit 3), not failed";
  ASSERT_TRUE(wait_until([]() {
    const auto cur = hf::runtime().instance_current();
    return cur && cur->handoff == hf::state_e::kill_pending;
  }))
    << "the instance file must show kill_pending";
  ASSERT_TRUE(C().attempt_running() || hf::runtime().instance_current()->handoff == hf::state_e::kill_pending);

  // A later helper, while the rollback still holds the claim.
  const int sources_before = sources_total();
  const auto later = C().handle(888);
  EXPECT_EQ(hf::reply_kind_e::refused, later.kind) << "a later helper is refused (exit 1)";
  EXPECT_EQ(std::string {hf::refusal_reason::kill_pending}, later.reason) << "with the kill-pending reason";
  EXPECT_NE(std::string {hf::refusal_reason::in_progress}, later.reason) << "never in_progress";
  EXPECT_EQ(sources_before, sources_total()) << "kill_pending is decided before any gatherer runs";

  // And again: nothing retries, nothing clears it.
  const auto again = C().handle(889);
  EXPECT_EQ(std::string {hf::refusal_reason::kill_pending}, again.reason);
  EXPECT_EQ(1, spawn_calls.load()) << "nothing retries the handoff: still one spawn";
  EXPECT_EQ(1, make_ops_calls.load());
  EXPECT_EQ(hf::state_e::kill_pending, hf::runtime().instance_current()->handoff) << "the refusals did not clear kill_pending";

  // The kill finally takes effect: the rollback ends and the state goes away.
  child_state->timeouts_left = 0;
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }))
    << "the rollback must finish once the child is reaped";
  EXPECT_EQ(hf::state_e::none, hf::runtime().instance_current()->handoff) << "only the finished rollback ends kill_pending";
}

TEST_F(HandoffCoordinatorTest, KillPendingInOtherInstanceFilesRefusesAndSpawnsNothing) {
  hf::instance_t other;
  other.pid = 9300;
  other.start_time = 1;
  other.role = hf::role_e::draining;
  other.handoff = hf::state_e::kill_pending;
  scan_value.live = {other};
  expect_refused(5, hf::refusal_reason::kill_pending);
}

// =====================================================================================================================
// Cooldown
// =====================================================================================================================

TEST_F(HandoffCoordinatorTest, CooldownIsArmedAtAttemptStartAndPassedToHello) {
  start_handle();
  ASSERT_TRUE(attach_peer());
  EXPECT_EQ(clk.at(120s), cooldown_at_spawn) << "the cooldown is now() + cooldown_s, armed before the spawn";
  EXPECT_EQ(clk.at(120s), C().cooldown_until());
  ASSERT_TRUE(peer_handshake());
  ASSERT_TRUE(last_hello.cooldown_deadline_ns.has_value());
  EXPECT_EQ(std::chrono::duration_cast<std::chrono::nanoseconds>(clk.at(120s).time_since_epoch()).count(), *last_hello.cooldown_deadline_ns) << "the new process inherits the deadline through HELLO";
  EXPECT_EQ(clk.at(120s), last_cooldown_arg) << "make_ops receives the cooldown of the attempt";
}

TEST_F(HandoffCoordinatorTest, CooldownIsReArmedAfterASpawnFailure) {
  spawn_fail = true;
  on_spawn = [this]() {
    clk.advance(10s);
  };
  ASSERT_EQ(hf::reply_kind_e::failed, C().handle(5).kind);
  EXPECT_TRUE(wait_until([this]() {
    return C().cooldown_until() == clk.at(130s);
  }))
    << "after a failed attempt the cooldown is max(current, now + cooldown_s) at the failure";
}

TEST_F(HandoffCoordinatorTest, CooldownIsReArmedAfterAChannelFailure) {
  channel_fail = true;
  on_channel = [this]() {
    clk.advance(10s);
  };
  ASSERT_EQ(hf::reply_kind_e::failed, C().handle(5).kind);
  EXPECT_TRUE(wait_until([this]() {
    return C().cooldown_until() == clk.at(130s);
  }));
}

TEST_F(HandoffCoordinatorTest, CooldownIsReArmedAfterARolledBackAttemptAndRefusesTheNextRequest) {
  ASSERT_TRUE(run_to_commit());
  ASSERT_TRUE(get_reply().has_value());
  EXPECT_EQ(clk.at(120s), C().cooldown_until());
  clk.advance(50s);
  peer->channel.close();  // the new process is lost: rollback
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  EXPECT_EQ(clk.at(170s), C().cooldown_until()) << "a rolled-back attempt re-arms the cooldown from its end";
  // The next request is refused for the cooldown (its own reason).
  spawn_calls = 0;
  const auto reply = C().handle(5);
  EXPECT_EQ(hf::reply_kind_e::refused, reply.kind);
  EXPECT_EQ(std::string {hf::refusal_reason::cooldown}, reply.reason);
  EXPECT_EQ(0, spawn_calls.load());
}

TEST_F(HandoffCoordinatorTest, ASuccessfulHandoverLeavesTheStartCooldownInPlace) {
  timeouts.rollback_window = 150ms;
  ASSERT_TRUE(run_to_commit());
  peer->start_pump();
  ASSERT_TRUE(get_reply().has_value());
  ASSERT_TRUE(wait_until([this]() {
    return C().handed_over();
  }));
  EXPECT_EQ(clk.at(120s), C().cooldown_until()) << "a handover does not re-arm";
}

TEST_F(HandoffCoordinatorTest, AShutdownOutcomeDoesNotReArmTheCooldown) {
  ASSERT_TRUE(run_to_commit());
  peer->start_pump();
  ASSERT_TRUE(get_reply().has_value());
  clk.advance(50s);
  C().request_shutdown();
  ASSERT_TRUE(C().join_attempt(1500ms));
  EXPECT_EQ(clk.at(120s), C().cooldown_until()) << "a shutdown outcome must not re-arm the cooldown";
}

TEST_F(HandoffCoordinatorTest, SetCooldownRaisesAndNeverLowers) {
  C().set_cooldown_until(clk.at(100s));
  EXPECT_EQ(clk.at(100s), C().cooldown_until());
  C().set_cooldown_until(clk.at(50s));
  EXPECT_EQ(clk.at(100s), C().cooldown_until()) << "set_cooldown_until never lowers the deadline";
  C().set_cooldown_until(clk.at(200s));
  EXPECT_EQ(clk.at(200s), C().cooldown_until());
}

TEST_F(HandoffCoordinatorTest, AnInheritedCooldownRefusesUntilItPasses) {
  C().set_cooldown_until(clk.at(20s));
  expect_refused(5, hf::refusal_reason::cooldown);
}

// =====================================================================================================================
// UPnP: scope sequence and the final mapping state
// =====================================================================================================================

TEST_F(HandoffCoordinatorTest, ScopeIsUdpOnlyFromTheFreezeOnAndAllAfterAFailureBeforeCommit) {
  start_handle();
  ASSERT_TRUE(attach_peer());
  ASSERT_TRUE(peer_to_go());
  EXPECT_EQ(upnp::unmap_scope_e::udp_only, upnp::unmap_scope()) << "the scope is set at freeze(), before COMMIT can be sent";
  ASSERT_TRUE(peer->send(protocol::go_ack_t {false, "no"}));
  ASSERT_TRUE(get_reply().has_value());
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  EXPECT_EQ(upnp::unmap_scope_e::all, upnp::unmap_scope()) << "a failure before COMMIT leaves no udp_only behind";
}

TEST_F(HandoffCoordinatorTest, ScopeIsAllAfterARollbackEvenWhenTheReloadFailed) {
  reload_ok = false;
  ASSERT_TRUE(run_to_commit());
  ASSERT_TRUE(get_reply().has_value());
  EXPECT_EQ(upnp::unmap_scope_e::udp_only, upnp::unmap_scope()) << "after COMMIT the draining process holds udp_only";
  peer->channel.close();
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  EXPECT_GE(events.first("reload"), 0) << "the rollback attempted the reload";
  EXPECT_EQ(1, events.count("unfreeze")) << "unfreeze still ran although the reload failed";
  EXPECT_EQ(upnp::unmap_scope_e::all, upnp::unmap_scope()) << "unfreeze()/leave_drain_mode() restore all whatever failed";
}

TEST_F(HandoffCoordinatorTest, ScopeStaysUdpOnlyAfterAShutdownFollowingCommit) {
  ASSERT_TRUE(run_to_commit());
  peer->start_pump();
  ASSERT_TRUE(get_reply().has_value());
  C().request_shutdown();
  ASSERT_TRUE(C().join_attempt(1500ms));
  EXPECT_EQ(upnp::unmap_scope_e::udp_only, upnp::unmap_scope()) << "a new process may hold the shared mappings: a shutdown after COMMIT must not restore all";
}

TEST_F(HandoffCoordinatorTest, ScopeIsAssertedAfterHandoverAndNothingRestoresIt) {
  timeouts.rollback_window = 150ms;
  ASSERT_TRUE(run_to_commit());
  peer->start_pump();
  ASSERT_TRUE(get_reply().has_value());
  ASSERT_TRUE(wait_until([this]() {
    return C().handed_over();
  }));
  EXPECT_EQ(upnp::unmap_scope_e::udp_only, upnp::unmap_scope());
  EXPECT_GE(events.count("env_scope:udp_only"), 1) << "the scope of freeze() is asserted again after the handover";
  EXPECT_EQ(0, events.count("env_scope:all")) << "nothing restores all after handed_over";
  EXPECT_EQ(0, events.count("env_scope:none"));
  EXPECT_EQ(0, events.count("unfreeze")) << "no unfreeze after a handover";
  EXPECT_EQ(0, events.count("drain_cancel")) << "no cancel after a handover";
}

TEST_F(HandoffCoordinatorTest, EqualBasesFreezeWithScopeNoneAndAreRestoredByTheRollback) {
  own_base_override = other_base;  // the old process's own base equals the child's base
  start_handle();
  ASSERT_TRUE(attach_peer());
  ASSERT_TRUE(peer_to_go());
  EXPECT_EQ(upnp::unmap_scope_e::none, upnp::unmap_scope()) << "equal bases: the draining process unmaps nothing";
  ASSERT_TRUE(peer->send(protocol::go_ack_t {true, ""}));
  ASSERT_TRUE(peer->expect<protocol::commit_t>().has_value());
  ASSERT_TRUE(get_reply().has_value());
  peer->channel.close();
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  EXPECT_EQ(upnp::unmap_scope_e::all, upnp::unmap_scope());
}

TEST_F(HandoffCoordinatorTest, FinalMappingStateAfterAPostCommitRollbackKeepsTheOldMappingsAndRemovesTheNewBasesUdp) {
  // SPEC-QUESTION: assumes that upnp::start() works against the fake IGD without any other module being initialised.
  install_igd_and_start(own_port, other_base);
  ASSERT_FALSE(new_udp_store.empty());
  for (const auto &m : new_udp_store) {
    ASSERT_EQ(1u, upnp::testing::fake_igd_mappings().count(m)) << "setup: the killed new process's UDP mapping " << m.port << " exists";
  }
  ASSERT_TRUE(run_to_commit());
  ASSERT_TRUE(get_reply().has_value());
  peer->channel.close();  // the new process is lost after COMMIT: rollback kills it and cleans up
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  ASSERT_TRUE(upnp::testing::wait_idle_for_test(1500ms)) << "the background removal must finish";
  const auto store = upnp::testing::fake_igd_mappings();
  EXPECT_EQ(old_store, store) << "final store must hold exactly the old process's TCP and UDP mappings; got: " << describe(store);
  for (const auto &m : old_store) {
    EXPECT_EQ(1u, store.count(m)) << "the old process's " << (m.tcp ? "TCP" : "UDP") << " mapping " << m.port << " must still be present";
  }
  for (const auto &m : new_udp_store) {
    EXPECT_EQ(0u, store.count(m)) << "the killed new process's UDP mapping " << m.port << " must be removed";
  }
  EXPECT_GE(child_state->kills.load(), 1);
  EXPECT_EQ(upnp::unmap_scope_e::all, upnp::unmap_scope());
}

TEST_F(HandoffCoordinatorTest, FinalMappingStateWithEqualBasesIsUntouched) {
  own_base_override = other_base;
  install_igd_and_start(other_base, other_base);
  const auto before = upnp::testing::fake_igd_mappings();
  ASSERT_TRUE(run_to_commit());
  ASSERT_TRUE(get_reply().has_value());
  peer->channel.close();
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  ASSERT_TRUE(upnp::testing::wait_idle_for_test(1500ms));
  EXPECT_EQ(before, upnp::testing::fake_igd_mappings()) << "equal bases: the cleanup must not touch the store; got: " << describe(upnp::testing::fake_igd_mappings());
}

TEST_F(HandoffCoordinatorTest, NoUpnpRemovalAfterAFailureBeforeCommit) {
  config::sunshine.flags[config::flag::UPNP] = true;
  remove_ports_hook = [](std::set<std::uint16_t>) {
  };
  start_handle();
  ASSERT_TRUE(attach_peer());
  ASSERT_TRUE(peer_to_go());
  ASSERT_TRUE(peer->send(protocol::go_ack_t {false, "no"}));
  ASSERT_TRUE(get_reply().has_value());
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  EXPECT_EQ(0, events.count("remove_ports")) << "the new process mapped nothing before COMMIT: no removal";
}

TEST_F(HandoffCoordinatorTest, UpnpRemovalAfterAPostCommitRollbackRunsAfterTheReapAndOnlyWhenUpnpIsOn) {
  config::sunshine.flags[config::flag::UPNP] = true;
  std::set<std::uint16_t> removed;
  remove_ports_hook = [&](std::set<std::uint16_t> p) {
    removed = std::move(p);
  };
  ASSERT_TRUE(run_to_commit());
  ASSERT_TRUE(get_reply().has_value());
  peer->channel.close();
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  ASSERT_EQ(1, events.count("remove_ports")) << "the old process removes the new base's UDP mappings after a post-COMMIT rollback";
  EXPECT_LT(events.last("kill_and_reap"), events.first("remove_ports")) << "the removal comes after the new process was reaped";
  EXPECT_EQ(hf::upnp_ports_to_remove(other_base, own_port, own_port), removed);
}

TEST_F(HandoffCoordinatorTest, UpnpRemovalIsSkippedWhenUpnpIsOff) {
  config::sunshine.flags[config::flag::UPNP] = false;
  remove_ports_hook = [](std::set<std::uint16_t>) {
  };
  ASSERT_TRUE(run_to_commit());
  ASSERT_TRUE(get_reply().has_value());
  peer->channel.close();
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  EXPECT_EQ(0, events.count("remove_ports")) << "UPnP off: no removal";
}

// =====================================================================================================================
// drain_controller entry points
// =====================================================================================================================

TEST_F(HandoffCoordinatorTest, DrainControllerBeginsAtCommitAndIsHandedOverAfterTheHandover) {
  timeouts.rollback_window = 150ms;
  ASSERT_TRUE(run_to_commit());
  peer->start_pump();
  ASSERT_TRUE(get_reply().has_value());
  ASSERT_TRUE(wait_until([this]() {
    return events.count("drain_handed_over") > 0;
  }))
    << "drain_controller().handed_over() must be called after the handover";
  EXPECT_EQ(1, events.count("drain_begin"));
  EXPECT_EQ(1, events.count("drain_handed_over"));
  EXPECT_EQ(0, events.count("drain_cancel"));
  EXPECT_LT(events.first("drain_begin"), events.first("barrier")) << "begin() comes at the switch, before the handover steps";
  EXPECT_LT(events.first("barrier"), events.first("drain_handed_over")) << "handed_over() only after the main-PID handover";
  for (const char *c : {"close:https", "close:http", "close:webui", "close:rtsp"}) {
    EXPECT_LT(events.first(c), events.first("drain_handed_over")) << c << ": the socket copies are closed before handed_over()";
  }
}

TEST_F(HandoffCoordinatorTest, DrainControllerIsCancelledByTheRollback) {
  ASSERT_TRUE(run_to_commit());
  ASSERT_TRUE(get_reply().has_value());
  peer->channel.close();
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  EXPECT_EQ(1, events.count("drain_begin"));
  EXPECT_EQ(1, events.count("drain_cancel")) << "leave_drain_mode() cancels the controller";
  EXPECT_LT(events.first("drain_begin"), events.first("drain_cancel"));
  EXPECT_EQ(0, events.count("drain_handed_over")) << "handed_over() is never called on a rollback";
}

TEST_F(HandoffCoordinatorTest, DrainControllerIsNotTouchedWhenTheSwitchFailsBeforeCommit) {
  start_handle();
  ASSERT_TRUE(attach_peer());
  ASSERT_TRUE(peer_to_go());
  ASSERT_TRUE(peer->send(protocol::go_ack_t {false, "no"}));
  ASSERT_TRUE(get_reply().has_value());
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  EXPECT_EQ(0, events.count("drain_begin"));
  EXPECT_EQ(0, events.count("drain_cancel"));
  EXPECT_EQ(0, events.count("drain_handed_over"));
}

// =====================================================================================================================
// After the handover
// =====================================================================================================================

TEST_F(HandoffCoordinatorTest, AfterTheHandoverTheProcessIsDrainingAndRefusesNewRequests) {
  timeouts.rollback_window = 150ms;
  ASSERT_TRUE(run_to_commit());
  peer->start_pump();
  ASSERT_TRUE(get_reply().has_value());
  ASSERT_TRUE(wait_until([this]() {
    return C().handed_over();
  }))
    << "the run must end in a handover";
  EXPECT_TRUE(wait_until([this]() {
    return peer->got_main.load();
  }))
    << "the new process must receive MAIN";
  EXPECT_FALSE(C().attempt_running()) << "after a handover no attempt is running";
  EXPECT_FALSE(hf::runtime().handoff_in_progress()) << "handoff_in_progress is cleared after the handover";
  EXPECT_EQ(hf::mode_e::draining, hf::runtime().mode());
  for (const auto &l : lf) {
    EXPECT_EQ(1, l.closes.load()) << "the socket copies are closed";
  }
  const int spawns = spawn_calls.load();
  const auto reply = C().handle(5);
  EXPECT_EQ(hf::reply_kind_e::refused, reply.kind);
  EXPECT_EQ(std::string {hf::coordinator_reason::draining}, reply.reason) << "the claim is never released after a handover";
  EXPECT_EQ(spawns, spawn_calls.load());
}

TEST_F(HandoffCoordinatorTest, AfterTheHandoverTheResponderAnswersSessionsAndEndAll) {
  timeouts.rollback_window = 150ms;
  ASSERT_TRUE(run_to_commit());
  peer->start_pump();
  ASSERT_TRUE(get_reply().has_value());
  ASSERT_TRUE(wait_until([this]() {
    return C().handed_over();
  }));
  ASSERT_TRUE(peer->send(protocol::sessions_request_t {5}));
  const auto sessions = peer->take<protocol::sessions_reply_t>();
  ASSERT_TRUE(sessions.has_value()) << "the draining process must answer SESSIONS? through the channel reader and the responder";
  EXPECT_EQ(5u, sessions->request_id);
  ASSERT_EQ(1u, sessions->sessions.size());
  EXPECT_EQ(7u, sessions->sessions[0].id);

  ASSERT_TRUE(peer->send(protocol::end_all_t {9}));
  const auto ended = peer->take<protocol::ended_t>();
  ASSERT_TRUE(ended.has_value()) << "END_ALL after the handover is answered ENDED";
  EXPECT_EQ(9u, ended->request_id);
  EXPECT_EQ(1, events.count("end_and_join"));
}

TEST_F(HandoffCoordinatorTest, AfterTheHandoverTheRevocationPollerRuns) {
  timeouts.rollback_window = 150ms;
  ASSERT_TRUE(run_to_commit());
  peer->start_pump();
  ASSERT_TRUE(get_reply().has_value());
  ASSERT_TRUE(wait_until([this]() {
    return C().handed_over();
  }));
  ASSERT_EQ(1, poller_calls.load()) << "one poller is made for the handover";
  ASSERT_NE(nullptr, poller_raw);
  EXPECT_TRUE(wait_until([this]() {
    return poller_raw->running();
  }))
    << "the revocation poller must be started after the handover";
}

TEST_F(HandoffCoordinatorTest, ALostChannelAfterTheHandoverEndsNothingAndTheDrainGoesOn) {
  timeouts.rollback_window = 150ms;
  ASSERT_TRUE(run_to_commit());
  peer->start_pump();
  ASSERT_TRUE(get_reply().has_value());
  ASSERT_TRUE(wait_until([this]() {
    return C().handed_over();
  }));
  peer->stop_pump();
  peer->channel.close();
  std::this_thread::sleep_for(100ms);
  EXPECT_TRUE(C().handed_over());
  EXPECT_EQ(0, events.count("end_and_join")) << "a closed channel ends no session";
  EXPECT_EQ(hf::mode_e::draining, hf::runtime().mode());
  const auto start = std::chrono::steady_clock::now();
  C().stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1500ms) << "stop() after a lost channel must be bounded";
}

TEST_F(HandoffCoordinatorTest, EndAllInTheRollbackWindowIsAnsweredAndTheRollbackDropsTheApp) {
  ASSERT_TRUE(run_to_commit());
  peer->start_pump();
  ASSERT_TRUE(get_reply().has_value());
  ASSERT_TRUE(peer->send(protocol::end_all_t {11}));
  const auto ended = peer->take<protocol::ended_t>();
  ASSERT_TRUE(ended.has_value()) << "an END_ALL in the rollback window is answered by the same responder that serves the drain";
  EXPECT_EQ(11u, ended->request_id);
  EXPECT_EQ(1, events.count("end_and_join"));
  peer->stop_pump();
  peer->channel.close();
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  EXPECT_EQ(1, events.count("drop_app")) << "a rollback after END_ALL drops the app state";
  EXPECT_EQ(0, events.count("set_released:0")) << "and does not take the app back";
}

// =====================================================================================================================
// join_attempt, stop, control server
// =====================================================================================================================

TEST_F(HandoffCoordinatorTest, JoinAttemptWithoutAnAttemptReturnsTrueAtOnce) {
  const auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(C().join_attempt(1500ms));
  EXPECT_LT(std::chrono::steady_clock::now() - start, 300ms);
}

TEST_F(HandoffCoordinatorTest, JoinAttemptIsBoundedWhileTheAttemptRuns) {
  timeouts.ready = 1500ms;
  start_handle();
  ASSERT_TRUE(attach_peer());  // the peer stays silent: the attempt waits for HELLO
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(C().join_attempt(100ms)) << "an attempt that is still running makes join_attempt() give up";
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_GE(elapsed, 90ms) << "join_attempt() waits for its timeout";
  EXPECT_LT(elapsed, 1000ms) << "and not much longer";
  EXPECT_TRUE(C().attempt_running()) << "giving up does not end the attempt";
  peer->channel.close();
  EXPECT_TRUE(C().join_attempt(1500ms)) << "once the attempt ended, join_attempt() succeeds";
}

TEST_F(HandoffCoordinatorTest, StopDuringAnAttemptForwardsTheShutdownAndReturnsBounded) {
  timeouts.ready = 1500ms;
  start_handle();
  ASSERT_TRUE(attach_peer());
  const auto start = std::chrono::steady_clock::now();
  C().stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1500ms) << "stop() must forward the shutdown and join in bounded time";
  const auto reply = get_reply();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(hf::reply_kind_e::failed, reply->kind) << "the waiting helper is answered failed";
  EXPECT_GE(child_state->kills.load(), 1);
  EXPECT_FALSE(C().attempt_running());
}

TEST_F(HandoffCoordinatorTest, StopIsIdempotentAndRefusesAfterwards) {
  C().stop();
  EXPECT_NO_THROW(C().stop());
  const auto reply = C().handle(5);
  EXPECT_EQ(hf::reply_kind_e::refused, reply.kind);
  EXPECT_EQ(std::string {hf::coordinator_reason::shutting_down}, reply.reason);
  EXPECT_FALSE(C().start_control((tmp / "late.sock").string())) << "start_control() after stop() fails";
}

TEST_F(HandoffCoordinatorTest, RequestShutdownIsIdempotent) {
  C().request_shutdown();
  EXPECT_NO_THROW(C().request_shutdown());
}

TEST_F(HandoffCoordinatorTest, StartControlServesTheHandlerAndIsIdempotent) {
  notify_value = "";  // a healthy request would start an attempt; this refusal is identified by its own reason
  const auto path = hf::control_socket_path(tmp, ::getpid());
  ASSERT_TRUE(C().start_control(path.string())) << "the control server must listen";
  EXPECT_TRUE(C().start_control(path.string())) << "a second call while running returns true";

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(fd, 0);
  sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  ASSERT_EQ(0, ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)));
  const std::string verb = "handoff\n";
  ASSERT_EQ(static_cast<ssize_t>(verb.size()), ::write(fd, verb.data(), verb.size()));
  std::string line;
  const auto deadline = std::chrono::steady_clock::now() + 1500ms;
  while (line.find('\n') == std::string::npos && std::chrono::steady_clock::now() < deadline) {
    pollfd p {fd, POLLIN, 0};
    if (::poll(&p, 1, 100) > 0) {
      char buf[256];
      const auto n = ::read(fd, buf, sizeof(buf));
      if (n <= 0) {
        break;
      }
      line.append(buf, static_cast<std::size_t>(n));
    }
  }
  ::close(fd);
  const auto reply = hf::parse_reply(line);
  ASSERT_TRUE(reply.has_value()) << "unparsable reply line: '" << line << "'";
  EXPECT_EQ(hf::reply_kind_e::refused, reply->kind);
  EXPECT_EQ(std::string {hf::refusal_reason::no_notify_socket}, reply->reason) << "the verb reaches the coordinator's handler";
  EXPECT_EQ(0, spawn_calls.load());

  C().stop_control();
  EXPECT_NO_THROW(C().stop_control()) << "stop_control() is idempotent";
}

TEST_F(HandoffCoordinatorTest, StopControlWithoutStartIsSafe) {
  EXPECT_NO_THROW(C().stop_control());
}

TEST_F(HandoffCoordinatorTest, StartControlFailsAfterRequestShutdown) {
  C().request_shutdown();
  EXPECT_FALSE(C().start_control(hf::control_socket_path(tmp, ::getpid()).string()));
}

// =====================================================================================================================
// Scope after COMMIT, kill_pending, control server restart (lead rulings)
// =====================================================================================================================

namespace {
  /**
   * @brief Send the `handoff` verb to a control socket and parse the reply line.
   *
   * @param path Socket path.
   * @return The reply, or nothing when connecting or parsing failed.
   */
  std::optional<handoff::control_reply_t> control_request(const std::string &path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return std::nullopt;
    }
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      ::close(fd);
      return std::nullopt;
    }
    const std::string verb = "handoff\n";
    if (::write(fd, verb.data(), verb.size()) != static_cast<ssize_t>(verb.size())) {
      ::close(fd);
      return std::nullopt;
    }
    std::string line;
    const auto deadline = std::chrono::steady_clock::now() + 1500ms;
    while (line.find('\n') == std::string::npos && std::chrono::steady_clock::now() < deadline) {
      pollfd p {fd, POLLIN, 0};
      if (::poll(&p, 1, 100) > 0) {
        char buf[256];
        const auto n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) {
          break;
        }
        line.append(buf, static_cast<std::size_t>(n));
      }
    }
    ::close(fd);
    return handoff::parse_reply(line);
  }
}  // namespace

TEST_F(HandoffCoordinatorTest, ShutdownRightAfterCommitKeepsScopeAndTheSharedTcpMappings) {
  install_igd_and_start(own_port, other_base);
  const auto tcp_before = upnp::testing::fake_igd_mappings();
  start_handle();
  ASSERT_TRUE(attach_peer());
  ASSERT_TRUE(peer_to_commit());
  C().request_shutdown();  // the committed answer may not have been sent yet
  ASSERT_TRUE(C().join_attempt(2000ms));
  EXPECT_EQ(upnp::unmap_scope_e::udp_only, upnp::unmap_scope()) << "once COMMIT is written a shutdown must never restore all";
  ASSERT_TRUE(upnp::testing::wait_idle_for_test(1500ms));
  const auto store = upnp::testing::fake_igd_mappings();
  for (const auto &m : old_store) {
    EXPECT_EQ(1u, store.count(m)) << "the shared " << (m.tcp ? "TCP" : "UDP") << " mapping " << m.port << " must survive; got: " << describe(store);
  }
  EXPECT_EQ(tcp_before, store) << "a shutdown after COMMIT removes nothing";
}

TEST_F(HandoffCoordinatorTest, ShutdownAfterCommitWithEqualBasesKeepsScopeNone) {
  own_base_override = other_base;
  ASSERT_TRUE(run_to_commit());
  C().request_shutdown();
  ASSERT_TRUE(C().join_attempt(2000ms));
  EXPECT_EQ(upnp::unmap_scope_e::none, upnp::unmap_scope()) << "equal bases: the scope stays none after COMMIT and a shutdown";
}

TEST_F(HandoffCoordinatorTest, ShutdownAfterCommitAndTheCommittedAnswerKeepsScope) {
  ASSERT_TRUE(run_to_commit());
  peer->start_pump();
  const auto reply = get_reply();
  ASSERT_TRUE(reply.has_value());
  ASSERT_EQ(hf::reply_kind_e::committed, reply->kind);
  C().request_shutdown();
  ASSERT_TRUE(C().join_attempt(2000ms));
  EXPECT_EQ(upnp::unmap_scope_e::udp_only, upnp::unmap_scope());
  EXPECT_EQ(0, events.count("env_scope:all")) << "nothing may restore all after COMMIT";
}

TEST_F(HandoffCoordinatorTest, ChannelLostRightAfterCommitNeverLeavesTheScopeOnTheWayToAll) {
  // SPEC-QUESTION: a send error after COMMIT was written is not reproducible through the public seams; the closest
  // observable case is the peer vanishing as COMMIT arrives. A confirmed reap is a rollback and restores all, so this
  // test only requires that a shutdown which wins the race keeps the scope.
  ASSERT_TRUE(run_to_commit());
  C().request_shutdown();
  peer->channel.close();
  ASSERT_TRUE(C().join_attempt(2000ms));
  const auto scope = upnp::unmap_scope();
  if (events.count("drain_cancel") == 0) {
    EXPECT_EQ(upnp::unmap_scope_e::udp_only, scope) << "no confirmed rollback happened, so all must not be restored";
  }
}

TEST_F(HandoffCoordinatorTest, ShutdownBeforeTheSwitchRestoresScopeAll) {
  // Control for the tests above: a shutdown before any GO or COMMIT restores all.
  start_handle();
  ASSERT_TRUE(attach_peer());
  ASSERT_TRUE(peer_handshake());
  hf::runtime().request_shutdown();
  const auto reply = get_reply();
  ASSERT_TRUE(reply.has_value());
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  EXPECT_EQ(upnp::unmap_scope_e::all, upnp::unmap_scope()) << "a shutdown before the switch started leaves the scope at all";
}

TEST_F(HandoffCoordinatorTest, KillPendingOutcomeNeverRestoresScopeAllAndNeverRemovesPorts) {
  config::sunshine.flags[config::flag::UPNP] = true;
  remove_ports_hook = [](std::set<std::uint16_t>) {
  };
  child_state->timeouts_left = 1000000;  // the kill is never confirmed
  ASSERT_TRUE(run_to_commit());
  ASSERT_TRUE(get_reply().has_value());
  peer->channel.close();
  ASSERT_TRUE(wait_until([]() {
    const auto cur = hf::runtime().instance_current();
    return cur && cur->handoff == hf::state_e::kill_pending;
  }))
    << "setup: the rollback must reach kill_pending";
  std::this_thread::sleep_for(150ms);
  EXPECT_EQ(upnp::unmap_scope_e::udp_only, upnp::unmap_scope()) << "an unconfirmed kill must keep the new process's mappings protected: no all";
  EXPECT_EQ(0, events.count("remove_ports")) << "no removal while the new process may still be alive";
  EXPECT_EQ(0, events.count("env_scope:all"));
  child_state->timeouts_left = 0;
}

TEST_F(HandoffCoordinatorTest, ConfirmedRollbackRestoresScopeAllAndCallsTheRemoval) {
  // Control for the kill_pending test: the same path with a confirmed reap.
  config::sunshine.flags[config::flag::UPNP] = true;
  remove_ports_hook = [](std::set<std::uint16_t>) {
  };
  ASSERT_TRUE(run_to_commit());
  ASSERT_TRUE(get_reply().has_value());
  peer->channel.close();
  ASSERT_TRUE(wait_until([this]() {
    return !C().attempt_running();
  }));
  EXPECT_EQ(upnp::unmap_scope_e::all, upnp::unmap_scope());
  EXPECT_EQ(1, events.count("remove_ports"));
}

TEST_F(HandoffCoordinatorTest, StartControlTwiceStartsOneServerAndServesRequests) {
  notify_value = "";
  const auto path = hf::control_socket_path(tmp, ::getpid()).string();
  ASSERT_TRUE(C().start_control(path));
  ASSERT_TRUE(C().start_control(path)) << "a second start while running returns true";
  const auto reply = control_request(path);
  ASSERT_TRUE(reply.has_value()) << "the single server must answer";
  EXPECT_EQ(std::string {hf::refusal_reason::no_notify_socket}, reply->reason);
  EXPECT_EQ(1, cnt_notify.load()) << "one request reached one handler";
}

TEST_F(HandoffCoordinatorTest, StartControlWorksAgainAfterStopControl) {
  notify_value = "";
  const auto path = hf::control_socket_path(tmp, ::getpid()).string();
  ASSERT_TRUE(C().start_control(path));
  ASSERT_TRUE(control_request(path).has_value());
  C().stop_control();
  EXPECT_FALSE(control_request(path).has_value()) << "after stop_control() nothing listens";
  ASSERT_TRUE(C().start_control(path)) << "stop_control() then start_control() starts the server again";
  const auto reply = control_request(path);
  ASSERT_TRUE(reply.has_value()) << "the restarted server must answer";
  EXPECT_EQ(std::string {hf::refusal_reason::no_notify_socket}, reply->reason);
}

TEST_F(HandoffCoordinatorTest, ConcurrentStartControlStartsOneServerAndNoCallFails) {
  notify_value = "";
  const auto path = hf::control_socket_path(tmp, ::getpid()).string();
  for (int round = 0; round < 4; ++round) {
    constexpr int callers = 6;
    std::atomic<bool> go {false};
    std::atomic<int> ready {0};
    std::atomic<int> succeeded {0};
    std::atomic<int> failed {0};
    std::vector<std::thread> threads;
    for (int i = 0; i < callers; ++i) {
      threads.emplace_back([&] {
        ++ready;
        while (!go.load()) {
          std::this_thread::yield();
        }
        if (C().start_control(path)) {
          ++succeeded;
        } else {
          ++failed;
        }
      });
    }
    while (ready.load() < callers) {
      std::this_thread::yield();
    }
    go = true;
    for (auto &t : threads) {
      t.join();
    }
    EXPECT_EQ(0, failed.load()) << "round " << round << ": a start_control() that raced another one must return true (idempotent), not fail";
    EXPECT_EQ(callers, succeeded.load()) << "round " << round << ": every concurrent start_control() must return true";
    const int before = cnt_notify.load();
    const auto reply = control_request(path);
    ASSERT_TRUE(reply.has_value()) << "round " << round << ": the one server must answer";
    EXPECT_EQ(std::string {hf::refusal_reason::no_notify_socket}, reply->reason);
    EXPECT_EQ(before + 1, cnt_notify.load()) << "round " << round << ": one request must reach exactly one handler";
    C().stop_control();
    EXPECT_FALSE(control_request(path).has_value()) << "round " << round << ": after stop_control() no second server may still listen on the path";
  }
}

#endif  // __linux__
