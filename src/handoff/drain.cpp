/**
 * @file src/handoff/drain.cpp
 * @brief Definitions for `drain.h`.
 */
#ifdef __linux__
  // standard includes
  #include <cerrno>
  #include <string>
  #include <utility>

  // lib includes
  #include <poll.h>
  #include <sys/eventfd.h>
  #include <unistd.h>

  // local includes
  #include "drain.h"
  #include "peer_link.h"
  #include "protocol.h"
  #include "settings.h"
  #include "src/config.h"
  #include "src/entry_handler.h"
  #include "src/globals.h"
  #include "src/logging.h"
  #include "src/rtsp.h"
  #include "src/stream.h"
  #include "src/upnp.h"

using namespace std::literals;

namespace handoff {
  namespace {
    std::mutex device_release_mutex;  ///< Guards `device_release`.
    std::function<void()> device_release;  ///< The registered device release.
    std::atomic<std::shared_ptr<const std::string>> last_stage;  ///< Last name given to `note_shutdown_stage()`.

    /**
     * @brief Run the registered device release, if any.
     */
    void run_device_release() {
      std::function<void()> release;
      {
        std::lock_guard lock {device_release_mutex};
        release = device_release;
      }
      if (release) {
        release();
      }
    }

    /**
     * @brief Production default of `drain_ops_t::ensure_unmap_scope`.
     */
    void default_ensure_unmap_scope() {
      if (upnp::unmap_scope() != upnp::unmap_scope_e::all) {
        return;
      }

      const auto own_base = runtime().effective_udp_base(config::sunshine.port);
      const auto other_base = other_udp_base(config::sunshine.port, runtime().settings().udp_offset, own_base);

      BOOST_LOG(warning) << "UPnP unmap scope was not set at the switch"sv;
      upnp::set_unmap_scope(own_base == other_base ? upnp::unmap_scope_e::none : upnp::unmap_scope_e::udp_only);
    }

    /**
     * @brief Production default of `notifier_ops_t::peer_state`.
     *
     * @return The draining peer's state, or `std::nullopt` when there is no peer.
     */
    std::optional<state_e> default_peer_state() {
      auto peer = runtime().peer();
      if (!peer) {
        return std::nullopt;
      }
      if (!peer->exists()) {
        return state_e::none;
      }
      if (auto link = std::dynamic_pointer_cast<session_peer_t>(peer)) {
        return link->state();
      }
      return state_e::none;
    }

    /**
     * @brief Fill every null member of the notifier effects with its production default.
     *
     * @param ops Effects to complete.
     */
    void fill_notifier_defaults(notifier_ops_t &ops) {
      if (!ops.sessions) {
        ops.sessions = [] {
          return stream::session::running_session_count();
        };
      }
      if (!ops.write_sessions) {
        ops.write_sessions = [](int sessions) {
          runtime().instance_set_sessions(sessions);
        };
      }
      if (!ops.mode) {
        ops.mode = [] {
          return runtime().mode();
        };
      }
      if (!ops.current_state) {
        ops.current_state = [] {
          auto instance = runtime().instance_current();
          return instance ? instance->handoff : state_e::none;
        };
      }
      if (!ops.write_state) {
        ops.write_state = [](state_e state) {
          runtime().instance_set_state(state);
        };
      }
      if (!ops.send_state) {
        ops.send_state = [](state_e state) {
          if (auto channel = runtime().channel()) {
            // A failed send is ignored: the active process falls back to the instance file.
            channel->send(protocol::handoff_state_t {std::string {to_string(state)}});
          }
        };
      }
      if (!ops.peer_state) {
        ops.peer_state = default_peer_state;
      }
      if (!ops.shutdown_requested) {
        ops.shutdown_requested = [] {
          return runtime().shutdown_requested();
        };
      }
    }

    /**
     * @brief Fill every null member of the controller effects with its production default.
     *
     * @param ops Effects to complete.
     */
    void fill_drain_defaults(drain_ops_t &ops) {
      if (!ops.now) {
        ops.now = [] {
          return std::chrono::steady_clock::now();
        };
      }
      if (!ops.sessions) {
        ops.sessions = [] {
          return stream::session::running_session_count();
        };
      }
      if (!ops.shutdown_requested) {
        ops.shutdown_requested = [] {
          return runtime().shutdown_requested();
        };
      }
      if (!ops.end_sessions) {
        ops.end_sessions = [](stream::stop_reason_e reason) {
          rtsp_stream::terminate_sessions(reason);
        };
      }
      if (!ops.release_devices) {
        ops.release_devices = run_device_release;
      }
      if (!ops.ensure_unmap_scope) {
        ops.ensure_unmap_scope = default_ensure_unmap_scope;
      }
      if (!ops.arm_limit) {
        ops.arm_limit = [](std::chrono::milliseconds limit, std::function<void()> on_expiry) {
          task_pool.pushDelayed(std::move(on_expiry), limit);
        };
      }
      if (!ops.exit_graceful) {
        ops.exit_graceful = [] {
          lifetime::exit_sunshine(0, true);
        };
      }
      if (!ops.force_exit) {
        ops.force_exit = [](int code) {
          logging::log_flush();
          _exit(code);
        };
      }
      if (!ops.log_error) {
        ops.log_error = [](const std::string &text) {
          BOOST_LOG(error) << text;
        };
      }
    }
  }  // namespace

  // ---- session_notifier_t ----

  session_notifier_t::session_notifier_t() {
    fill_notifier_defaults(_ops);
    _wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (_wake_fd < 0) {
      BOOST_LOG(error) << "Session notifier: eventfd failed: "sv << errno;
    }
  }

  #ifdef SUNSHINE_TESTS
  session_notifier_t::session_notifier_t(const notifier_ops_t &ops, const notifier_timing_t &timing):
      _ops(ops),
      _timing(timing) {
    fill_notifier_defaults(_ops);
    _wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (_wake_fd < 0) {
      BOOST_LOG(error) << "Session notifier: eventfd failed: "sv << errno;
    }
  }
  #endif

  session_notifier_t::~session_notifier_t() {
    stop();
    if (_wake_fd >= 0) {
      close(_wake_fd);
      _wake_fd = -1;
    }
  }

  void session_notifier_t::start() {
    std::lock_guard lock {_start_mutex};
    if (_thread.joinable() || _stop.load()) {
      return;
    }

    _thread = std::thread {[this] {
      std::optional<int> last_sessions;  // Last value written to the instance file.
      std::optional<state_e> last_sent;  // State last sent to the active process.

      // One pass, see the class comment.
      auto pass = [&]() {
        const int sessions = _ops.sessions();
        const bool changed = !last_sessions || *last_sessions != sessions;
        if (changed) {
          _ops.write_sessions(sessions);
          last_sessions = sessions;
        }

        if (_ops.mode() == mode_e::draining) {
          const auto state = _ops.current_state();
          if (changed || !last_sent || *last_sent != state) {
            _ops.send_state(state);
            last_sent = state;
          }
        } else {
          // Not draining (any more): the next drain starts with nothing sent.
          last_sent.reset();
          if (const auto peer = _ops.peer_state(); peer && *peer != _ops.current_state()) {
            _ops.write_state(*peer);
          }
        }
        ++_passes;
      };

      pass();
      while (!_stop.load() && !_ops.shutdown_requested()) {
        pollfd fds[2];
        nfds_t count = 0;
        fds[count++] = {_wake_fd, POLLIN, 0};
        const int shutdown_fd = runtime().shutdown_wake_fd();
        if (shutdown_fd >= 0) {
          fds[count++] = {shutdown_fd, POLLIN, 0};
        }

        const int rc = poll(fds, count, static_cast<int>(_timing.period.count()));
        if (rc < 0 && errno != EINTR) {
          // Cannot wait: fall back to a plain sleep so the thread never spins.
          std::this_thread::sleep_for(_timing.period);
        }
        if (rc > 0 && (fds[0].revents & POLLIN)) {
          uint64_t value;
          while (read(_wake_fd, &value, sizeof(value)) > 0) {}
        }
        if (_stop.load()) {
          break;
        }
        pass();
        if (rc > 0 && count > 1 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
          // Shutdown: that was the last pass.
          break;
        }
      }
    }};
  }

  void session_notifier_t::poke() {
    if (_wake_fd < 0 || _stop.load(std::memory_order_relaxed)) {
      return;
    }
    const uint64_t one = 1;
    // The result is ignored: a full counter already means a pass is pending.
    [[maybe_unused]] auto written = write(_wake_fd, &one, sizeof(one));
  }

  void session_notifier_t::stop() {
    std::lock_guard lock {_start_mutex};
    _stop = true;
    if (_thread.joinable()) {
      const uint64_t one = 1;
      [[maybe_unused]] auto written = write(_wake_fd, &one, sizeof(one));
      _thread.join();
    }
  }

  unsigned session_notifier_t::passes() const {
    return _passes.load();
  }

  // ---- drain_controller_t ----

  drain_controller_t::drain_controller_t() {
    fill_drain_defaults(_ops);
  }

  #ifdef SUNSHINE_TESTS
  drain_controller_t::drain_controller_t(const drain_ops_t &ops, const drain_timing_t &timing):
      _ops(ops),
      _timing(timing) {
    fill_drain_defaults(_ops);
  }
  #endif

  drain_controller_t::~drain_controller_t() {
    cancel();

    std::thread thread;
    {
      std::lock_guard lock {_mutex};
      if (_thread.get_id() == std::this_thread::get_id()) {
        // Destroyed on its own thread: nothing to wait for.
        _thread.detach();
        return;
      }
      thread = std::move(_thread);
    }
    if (thread.joinable()) {
      thread.join();
    }
  }

  void drain_controller_t::begin(std::chrono::steady_clock::time_point switch_time) {
    const auto deadline_s = runtime().settings().deadline_s;

    std::lock_guard lock {_mutex};
    if (_phase.load() != drain_phase_e::idle || _thread.joinable()) {
      // A second begin() in one cycle, or one while the exit runs, changes nothing.
      return;
    }

    // A fresh cycle.
    _cancel = false;
    _limit_armed = false;
    _expired = false;
    _deadline = switch_time + std::chrono::seconds {deadline_s};
    _phase = drain_phase_e::draining;

    _thread = std::thread {[this] {
      std::optional<std::chrono::steady_clock::time_point> zero_since;

      for (;;) {
        bool handed_over;
        std::chrono::steady_clock::time_point deadline;
        {
          std::lock_guard lock {_mutex};
          if (_cancel) {
            return;
          }
          handed_over = _handed_over;
          deadline = _deadline;
        }

        // Evaluate the exit conditions with no lock held.
        const auto now = _ops.now();
        const bool deadline_passed = now >= deadline;
        if (_ops.sessions() == 0) {
          if (!zero_since) {
            zero_since = now;
          }
        } else {
          zero_since.reset();
        }
        const bool zero_stable = zero_since && now - *zero_since >= _timing.zero_grace;

        if (handed_over && (deadline_passed || zero_stable)) {
          // The signal path owns the exit when a shutdown was requested.
          if (_ops.shutdown_requested()) {
            return;
          }

          {
            std::lock_guard lock {_mutex};
            if (_cancel) {
              return;
            }
            _phase = drain_phase_e::exiting;
          }

          // The exit sequence: sessions and devices first, the limit only now, then the servers.
          _ops.ensure_unmap_scope();
          _ops.end_sessions(deadline_passed ? stream::stop_reason_e::drain_deadline : stream::stop_reason_e::stop);
          _ops.release_devices();
          {
            std::lock_guard lock {_mutex};
            _limit_armed = true;
          }
          _ops.arm_limit(_timing.forced_shutdown, [this] {
            on_limit_expired();
          });
          _ops.exit_graceful();
          _phase = drain_phase_e::done;
          return;
        }

        std::unique_lock lock {_mutex};
        if (_cancel) {
          return;
        }
        // A handed_over() call wakes the thread; so does cancel().
        _cv.wait_for(lock, _timing.tick, [&] {
          return _cancel || _handed_over != handed_over;
        });
      }
    }};
    _thread_id = _thread.get_id();
  }

  void drain_controller_t::handed_over() {
    {
      std::lock_guard lock {_mutex};
      _handed_over = true;
    }
    _cv.notify_all();
  }

  bool drain_controller_t::cancel() {
    std::thread thread;
    {
      std::lock_guard lock {_mutex};
      const auto phase = _phase.load();
      if (phase == drain_phase_e::exiting || phase == drain_phase_e::done) {
        return false;
      }
      if (_thread.joinable() && _thread.get_id() == std::this_thread::get_id()) {
        return false;
      }

      _cancel = true;
      thread = std::move(_thread);
    }
    _cv.notify_all();

    // Join with no lock held: the thread takes the mutex.
    if (thread.joinable()) {
      thread.join();
    }

    std::lock_guard lock {_mutex};
    if (_phase.load() == drain_phase_e::draining) {
      _phase = drain_phase_e::idle;
    }
    _handed_over = false;
    _limit_armed = false;
    return true;
  }

  drain_phase_e drain_controller_t::phase() const {
    return _phase.load();
  }

  void drain_controller_t::on_limit_expired() {
    {
      std::lock_guard lock {_mutex};
      if (!_limit_armed) {
        return;
      }
    }
    if (_expired.exchange(true)) {
      return;
    }

    auto stage = shutdown_stage();
    if (stage.empty()) {
      stage = "unknown stop";
    }
    _ops.log_error("Drain exit did not finish within "s + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(_timing.forced_shutdown).count()) + " s: "s + stage + " did not finish"s);
    _phase = drain_phase_e::done;
    _ops.force_exit(1);
  }

  // ---- process-wide objects and helpers ----

  session_notifier_t &session_notifier() {
    static session_notifier_t notifier;
    return notifier;
  }

  drain_controller_t &drain_controller() {
    static drain_controller_t controller;
    return controller;
  }

  void register_device_release(std::function<void()> release) {
    std::lock_guard lock {device_release_mutex};
    device_release = std::move(release);
  }

  void note_shutdown_stage(std::string_view name) {
    last_stage.store(std::make_shared<const std::string>(name));
  }

  std::string shutdown_stage() {
    auto stage = last_stage.load();
    return stage ? *stage : std::string {};
  }

  #ifdef SUNSHINE_TESTS
  void reset_drain_for_test() {
    {
      std::lock_guard lock {device_release_mutex};
      device_release = nullptr;
    }
    last_stage.store(nullptr);
  }
  #endif
}  // namespace handoff
#endif
