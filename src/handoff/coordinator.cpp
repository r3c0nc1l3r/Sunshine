/**
 * @file src/handoff/coordinator.cpp
 * @brief Definitions for `coordinator.h`: the old side's glue.
 *
 * Locking summary (the contract is in the header): `coordinator_t::_mutex` guards the attempt state only and is never
 * held across a gatherer, a spawn, a channel call, an `old_side_ops_t` call, a join or a wait for the helper's answer.
 */
// standard includes
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <set>
#include <string>
#include <sys/eventfd.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

// local includes
#include "coordinator.h"
#include "drain.h"
#include "gate.h"
#include "hooks.h"
#include "mdns_holder.h"
#include "sd_notify.h"
#include "settings.h"
#include "src/audio.h"
#include "src/config.h"
#include "src/entry_handler.h"
#include "src/httpcommon.h"
#include "src/logging.h"
#include "src/nvhttp.h"
#include "src/process.h"
#include "src/rtsp.h"
#include "src/system_tray.h"

namespace handoff {
  using namespace std::literals;

  namespace {
    /// The four listeners in fd order, which is also the order of every pause, resume and close.
    constexpr listener_e all_listeners[] = {listener_e::https, listener_e::http, listener_e::webui, listener_e::rtsp};

    /// Wait of the kill-first of a child that could not be started properly.
    constexpr std::chrono::milliseconds early_kill_wait {5000};

    /// Prefix of the virtual sinks that Sunshine itself creates; such a sink is never passed on as the host sink.
    constexpr std::string_view virtual_sink_prefix = "sink-sunshine-";

    /**
     * @brief Kill-first of a child whose sequence never ran; an unconfirmed reap is logged as an Error.
     *
     * @param child The child.
     */
    void early_kill(child_ops_t &child) {
      const auto result = child.kill_and_reap(early_kill_wait);
      if (result != reap_result_e::reaped && result != reap_result_e::reaped_elsewhere) {
        BOOST_LOG(error) << "Drain handoff: the new process (pid "sv << child.pid() << ") did not confirm the reap in time"sv;
      }
    }

    /**
     * @brief Run `fn`; an exception is logged as an Error and `failure` is returned.
     *
     * @param what Operation name for the log.
     * @param failure Value returned after an exception.
     * @param fn Operation.
     * @return The result of `fn`, or `failure`.
     */
    template<class R, class F>
    R guarded(std::string_view what, R failure, F &&fn) {
      try {
        return fn();
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Drain handoff: "sv << what << " failed: "sv << e.what();
      } catch (...) {
        BOOST_LOG(error) << "Drain handoff: "sv << what << " failed"sv;
      }
      return failure;
    }

    /**
     * @brief Run a `void` operation; an exception is logged as an Error.
     *
     * @param what Operation name for the log.
     * @param fn Operation.
     */
    template<class F>
    void guarded_void(std::string_view what, F &&fn) {
      try {
        fn();
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Drain handoff: "sv << what << " failed: "sv << e.what();
      } catch (...) {
        BOOST_LOG(error) << "Drain handoff: "sv << what << " failed"sv;
      }
    }

    /**
     * @brief Cut a timeout to the range `[1 ms, server_op_timeout]`.
     *
     * @param timeout Requested timeout.
     * @return The usable timeout.
     */
    std::chrono::milliseconds clamp_timeout(std::chrono::milliseconds timeout) {
      if (timeout <= std::chrono::milliseconds::zero()) {
        return std::chrono::milliseconds {1};
      }
      return std::min(timeout, server_op_timeout);
    }

    /**
     * @brief The content of `/proc/self/cgroup`.
     *
     * @return The content, or `std::nullopt` when it cannot be read.
     */
    std::optional<std::string> read_self_cgroup() {
      std::ifstream in("/proc/self/cgroup");
      if (!in) {
        return std::nullopt;
      }
      return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    /**
     * @brief The instances directory of this process.
     *
     * @return `<runtime dir>/instances`, or an empty path when there is no runtime directory.
     */
    std::filesystem::path production_instances_dir() {
      const char *runtime_directory = std::getenv("RUNTIME_DIRECTORY");
      const char *xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR");
      auto dir = runtime_dir(runtime_directory ? runtime_directory : "", xdg_runtime_dir ? xdg_runtime_dir : "");
      return dir ? *dir / "instances" : std::filesystem::path {};
    }

    /**
     * @brief The UPnP scope of a draining process.
     *
     * @param new_base The new process's UDP base.
     * @param own_base This process's UDP base.
     * @return `none` for equal bases, else `udp_only`.
     */
    upnp::unmap_scope_e scope_for_drain(int new_base, int own_base) {
      return new_base == own_base ? upnp::unmap_scope_e::none : upnp::unmap_scope_e::udp_only;
    }

    /**
     * @brief Forwards every `old_side_ops_t` call and reports the call that comes just before `GO` is written.
     */
    class tracking_ops_t final: public old_side_ops_t {
    public:
      tracking_ops_t(std::unique_ptr<old_side_ops_t> inner, std::function<void()> before_go):
          _inner(std::move(inner)),
          _before_go(std::move(before_go)) {
      }

      protocol::hello_t hello() override {
        return _inner->hello();
      }

      int self_pidfd() override {
        return _inner->self_pidfd();
      }

      std::string pause_accept() override {
        return _inner->pause_accept();
      }

      void resume_accept() override {
        _inner->resume_accept();
      }

      void raise_gate() override {
        _inner->raise_gate();
      }

      void lower_gate() override {
        _inner->lower_gate();
      }

      bool wait_in_flight(std::chrono::milliseconds timeout) override {
        return _inner->wait_in_flight(timeout);
      }

      std::string recheck(std::chrono::milliseconds pairing_wait) override {
        return _inner->recheck(pairing_wait);
      }

      bool save_state() override {
        return _inner->save_state();
      }

      void freeze() override {
        _inner->freeze();
      }

      void unfreeze() override {
        _inner->unfreeze();
      }

      void withdraw_mdns(std::chrono::milliseconds timeout) override {
        _inner->withdraw_mdns(timeout);
      }

      void publish_mdns() override {
        _inner->publish_mdns();
      }

      void remove_tray() override {
        _inner->remove_tray();
      }

      void restore_tray() override {
        _inner->restore_tray();
      }

      protocol::go_t take_snapshot() override {
        return _inner->take_snapshot();
      }

      bool shutdown_requested() override {
        return _inner->shutdown_requested();
      }

      void set_released(bool released) override {
        if (released) {
          // The old side sets the flag immediately before it writes `GO`.
          _before_go();
        }
        _inner->set_released(released);
      }

      void drop_app() override {
        _inner->drop_app();
      }

      bool reload_after_rollback() override {
        return _inner->reload_after_rollback();
      }

      void enter_drain_mode() override {
        _inner->enter_drain_mode();
      }

      void leave_drain_mode() override {
        _inner->leave_drain_mode();
      }

      void set_role(role_e role) override {
        _inner->set_role(role);
      }

      void set_state(state_e state) override {
        _inner->set_state(state);
      }

      void remove_new_instance_file() override {
        _inner->remove_new_instance_file();
      }

      void start_upnp_cleanup(int new_base) override {
        _inner->start_upnp_cleanup(new_base);
      }

      bool handover(pid_t new_pid) override {
        return _inner->handover(new_pid);
      }

      void close_socket_copies() override {
        _inner->close_socket_copies();
      }

    private:
      std::unique_ptr<old_side_ops_t> _inner;  ///< The real operations.
      std::function<void()> _before_go;  ///< Called before the operation that precedes `GO`.
    };
  }  // namespace

  // -------------------------------------------------------------------------------------------------------------------
  // The four servers
  // -------------------------------------------------------------------------------------------------------------------

  std::string_view to_string(listener_e kind) {
    switch (kind) {
      case listener_e::https:
        return "https"sv;
      case listener_e::http:
        return "http"sv;
      case listener_e::webui:
        return "webui"sv;
      case listener_e::rtsp:
        return "rtsp"sv;
    }
    return "unknown"sv;
  }

  listener_ops_t rtsp_listener_ops() {
    listener_ops_t ops;
    ops.pause = [](std::chrono::milliseconds timeout) {
      return rtsp_stream::pause_accept(timeout);
    };
    ops.resume = [](std::chrono::milliseconds timeout) {
      return rtsp_stream::resume_accept(timeout);
    };
    ops.close_listener = [](std::chrono::milliseconds timeout) {
      return rtsp_stream::close_listener(timeout);
    };
    ops.fd = []() {
      return rtsp_stream::listener_fd();
    };
    return ops;
  }

  void listener_registry_t::set(listener_e kind, listener_ops_t ops) {
    if (!ops.pause || !ops.resume || !ops.close_listener || !ops.fd) {
      BOOST_LOG(error) << "Drain handoff: the "sv << to_string(kind) << " server registered incomplete operations; ignored"sv;
      return;
    }
    auto &entry = _entries[static_cast<std::size_t>(kind)];
    // Waits for a running call on the replaced entry first.
    std::lock_guard call {entry.call_mutex};
    std::lock_guard guard {_mutex};
    entry.ops = std::move(ops);
    entry.present = true;
  }

  void listener_registry_t::clear(listener_e kind) {
    auto &entry = _entries[static_cast<std::size_t>(kind)];
    std::lock_guard call {entry.call_mutex};
    std::lock_guard guard {_mutex};
    entry.present = false;
    entry.ops = {};
  }

  bool listener_registry_t::registered(listener_e kind) const {
    std::lock_guard guard {_mutex};
    return _entries[static_cast<std::size_t>(kind)].present;
  }

  bool listener_registry_t::pause(listener_e kind, std::chrono::milliseconds timeout) {
    auto &entry = _entries[static_cast<std::size_t>(kind)];
    std::lock_guard call {entry.call_mutex};
    std::function<bool(std::chrono::milliseconds)> fn;
    {
      std::lock_guard guard {_mutex};
      if (!entry.present) {
        return false;
      }
      fn = entry.ops.pause;
    }
    return guarded("pause of a server"sv, false, [&] {
      return fn(clamp_timeout(timeout));
    });
  }

  bool listener_registry_t::resume(listener_e kind, std::chrono::milliseconds timeout) {
    auto &entry = _entries[static_cast<std::size_t>(kind)];
    std::lock_guard call {entry.call_mutex};
    std::function<bool(std::chrono::milliseconds)> fn;
    {
      std::lock_guard guard {_mutex};
      if (!entry.present) {
        return false;
      }
      fn = entry.ops.resume;
    }
    return guarded("resume of a server"sv, false, [&] {
      return fn(clamp_timeout(timeout));
    });
  }

  bool listener_registry_t::close_listener(listener_e kind, std::chrono::milliseconds timeout) {
    auto &entry = _entries[static_cast<std::size_t>(kind)];
    std::lock_guard call {entry.call_mutex};
    std::function<bool(std::chrono::milliseconds)> fn;
    {
      std::lock_guard guard {_mutex};
      if (!entry.present) {
        return true;
      }
      fn = entry.ops.close_listener;
    }
    return guarded("close of a listener"sv, false, [&] {
      return fn(clamp_timeout(timeout));
    });
  }

  int listener_registry_t::fd(listener_e kind) {
    auto &entry = _entries[static_cast<std::size_t>(kind)];
    std::lock_guard call {entry.call_mutex};
    std::function<int()> fn;
    {
      std::lock_guard guard {_mutex};
      if (!entry.present) {
        return -1;
      }
      fn = entry.ops.fd;
    }
    return guarded("read of a listener fd"sv, -1, [&] {
      return fn();
    });
  }

#ifdef SUNSHINE_TESTS
  void listener_registry_t::reset_for_test() {
    for (auto &entry : _entries) {
      entry.present = false;
      entry.ops = {};
    }
  }
#endif

  listener_registry_t &listeners() {
    static listener_registry_t registry;
    return registry;
  }

  // -------------------------------------------------------------------------------------------------------------------
  // The child
  // -------------------------------------------------------------------------------------------------------------------

  pidfd_child_t::pidfd_child_t(child_t child):
      _child(child) {
    _start_time = read_start_time(child.pid).value_or(0);
  }

#ifdef SUNSHINE_TESTS
  pidfd_child_t::pidfd_child_t(child_t child, child_seams_t seams):
      _child(child),
      _seams(std::move(seams)) {
    if (_seams.start_time) {
      _start_time = _seams.start_time(child.pid).value_or(0);
    } else {
      _start_time = read_start_time(child.pid).value_or(0);
    }
  }
#endif

  pidfd_child_t::~pidfd_child_t() {
    if (_child.pidfd >= 0) {
      ::close(_child.pidfd);
      _child.pidfd = -1;
    }
  }

  reap_result_e pidfd_child_t::kill_and_reap(std::chrono::milliseconds timeout) {
    // A child that is known to be gone is never signalled again: its pid may belong to another process by now.
    if (_gone.load()) {
      return reap_result_e::reaped_elsewhere;
    }

    int status = 0;
    reap_result_e result;
#ifdef SUNSHINE_TESTS
    if (_seams.kill_and_reap) {
      result = _seams.kill_and_reap(_child.pidfd, timeout, &status);
    } else {
      result = handoff::kill_and_reap(_child.pidfd, timeout, &status);
    }
#else
    result = handoff::kill_and_reap(_child.pidfd, timeout, &status);
#endif

    switch (result) {
      case reap_result_e::reaped:
        _gone.store(true);
        BOOST_LOG(info) << "Drain handoff: the new process (pid "sv << _child.pid << ") was reaped, exit status "sv << status;
        break;
      case reap_result_e::reaped_elsewhere:
        _gone.store(true);
        BOOST_LOG(info) << "Drain handoff: the new process (pid "sv << _child.pid << ") is gone, exit status unknown"sv;
        break;
      case reap_result_e::timeout:
      case reap_result_e::error:
        break;
    }
    return result;
  }

  bool pidfd_child_t::exited() {
    if (_gone.load()) {
      return true;
    }
#ifdef SUNSHINE_TESTS
    if (_seams.exited) {
      return _seams.exited(_child.pidfd);
    }
#endif
    return pidfd_exited(_child.pidfd);
  }

  pid_t pidfd_child_t::pid() {
    return _child.pid;
  }

  std::uint64_t pidfd_child_t::start_time() {
    return _start_time;
  }

  // -------------------------------------------------------------------------------------------------------------------
  // The old side's operations
  // -------------------------------------------------------------------------------------------------------------------

  sunshine_old_ops_t::sunshine_old_ops_t(control_server_t &control, int new_base, pid_t child_pid, std::chrono::steady_clock::time_point cooldown_until, std::filesystem::path instances_dir):
      _control(control),
      _new_base(new_base),
      _own_base(hooks::udp_base()),
      _child_pid(child_pid),
      _cooldown_until(cooldown_until),
      _instances_dir(std::move(instances_dir)) {
  }

#ifdef SUNSHINE_TESTS
  sunshine_old_ops_t::sunshine_old_ops_t(control_server_t &control, int new_base, pid_t child_pid, std::chrono::steady_clock::time_point cooldown_until, std::filesystem::path instances_dir, int own_base, old_ops_seams_t seams):
      _control(control),
      _new_base(new_base),
      _own_base(own_base),
      _child_pid(child_pid),
      _cooldown_until(cooldown_until),
      _instances_dir(std::move(instances_dir)),
      _seams(std::move(seams)) {
  }
#endif

  upnp::unmap_scope_e sunshine_old_ops_t::drain_scope() const {
    return scope_for_drain(_new_base, _own_base);
  }

  protocol::hello_t sunshine_old_ops_t::hello() {
    protocol::hello_t message;
    guarded_void("reading the probe results"sv, [&] {
      probe_data_t data;
#ifdef SUNSHINE_TESTS
      data = _seams.probe ? _seams.probe() : gather_probe();
#else
      data = gather_probe();
#endif
      message.probe = data.results;
      message.identity = data.identity;
    });
    // The new process compares this deadline with its own `CLOCK_MONOTONIC`, which is the clock of `steady_clock`.
    message.cooldown_deadline_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(_cooldown_until.time_since_epoch()).count();
    return message;
  }

  int sunshine_old_ops_t::self_pidfd() {
#ifdef SUNSHINE_TESTS
    if (_seams.self_pidfd) {
      return _seams.self_pidfd();
    }
#endif
    return open_self_pidfd();
  }

  std::string sunshine_old_ops_t::pause_accept() {
    for (auto kind : all_listeners) {
      if (!listeners().pause(kind, server_op_timeout)) {
        std::string reason = "the "s + std::string(to_string(kind)) + " server did not confirm the pause in time"s;
        BOOST_LOG(error) << "Drain handoff: "sv << reason;
        return reason;
      }
    }
    return {};
  }

  void sunshine_old_ops_t::resume_accept() {
    for (auto kind : all_listeners) {
      if (!listeners().resume(kind, server_op_timeout)) {
        BOOST_LOG(error) << "Drain handoff: the "sv << to_string(kind) << " server did not confirm the resume in time"sv;
      }
    }
  }

  void sunshine_old_ops_t::raise_gate() {
#ifdef SUNSHINE_TESTS
    if (_seams.gate) {
      _seams.gate(true);
      return;
    }
#endif
    route_gate().raise();
  }

  void sunshine_old_ops_t::lower_gate() {
#ifdef SUNSHINE_TESTS
    if (_seams.gate) {
      _seams.gate(false);
      return;
    }
#endif
    route_gate().lower();
  }

  bool sunshine_old_ops_t::wait_in_flight(std::chrono::milliseconds timeout) {
#ifdef SUNSHINE_TESTS
    if (_seams.wait_in_flight) {
      return _seams.wait_in_flight(timeout);
    }
#endif
    using clock = std::chrono::steady_clock;
    const bool unlimited = timeout < std::chrono::milliseconds::zero();
    const auto start = clock::now();
    const auto deadline = start + (unlimited ? std::chrono::milliseconds::zero() : timeout);
    auto next_warning = start + 5s;
    while (route_gate().in_flight() > 0) {
      const auto now = clock::now();
      if (!unlimited && now >= deadline) {
        return false;
      }
      if (unlimited && now >= next_warning) {
        BOOST_LOG(warning) << "Drain handoff: waiting for "sv << route_gate().in_flight() << " requests in flight"sv;
        next_warning = now + 5s;
      }
      std::this_thread::sleep_for(10ms);
    }
    return true;
  }

  std::string sunshine_old_ops_t::recheck(std::chrono::milliseconds pairing_wait) {
    return guarded("the recheck"sv, "the recheck failed"s, [&]() -> std::string {
      refusal_sources_t sources;
#ifdef SUNSHINE_TESTS
      sources = _seams.sources_set ? _seams.sources : production_refusal_sources();
#else
      sources = production_refusal_sources();
#endif

      // (1) The app. The text is built by `check_refusal` so it is sanitised the same way as at the first check.
      std::string why;
      if (!sources.app_adoptable(why)) {
        refusal_inputs_t inputs;
        inputs.notify_socket_set = true;
        inputs.systemd_version = minimum_systemd_version;
        inputs.unit_found = true;
        inputs.app_adoptable = false;
        inputs.app_reason = why;
        return check_refusal(inputs);
      }

      // (2) Pairing sessions: wait for them to end, up to `pairing_wait`.
      const auto deadline = sources.now() + pairing_wait;
      while (sources.pairing_count() > 0) {
        const auto now = sources.now();
        if (now >= deadline) {
          return std::string(refusal_reason::pairing);
        }
        const auto step = std::min<std::chrono::milliseconds>(recheck_poll_period, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
#ifdef SUNSHINE_TESTS
        if (_seams.sleep_for) {
          _seams.sleep_for(step);
          continue;
        }
#endif
        std::this_thread::sleep_for(step);
      }

      // (3) A pending launch is dropped; this is not a failure.
      if (sources.pending_launch()) {
        bool dropped = false;
#ifdef SUNSHINE_TESTS
        dropped = _seams.drop_pending_launch ? _seams.drop_pending_launch() : rtsp_stream::drop_pending_launch();
#else
        dropped = rtsp_stream::drop_pending_launch();
#endif
        BOOST_LOG(info) << "Drain handoff: a pending launch was "sv << (dropped ? "dropped"sv : "already gone"sv);
      }
      return {};
    });
  }

  bool sunshine_old_ops_t::save_state() {
    return guarded("saving the state file"sv, false, [&] {
#ifdef SUNSHINE_TESTS
      if (_seams.save_state) {
        return _seams.save_state();
      }
#endif
      return nvhttp::save_state_checked();
    });
  }

  void sunshine_old_ops_t::freeze() {
    // The unmap scope first: it holds on every later path in which this process exits, before COMMIT can be sent.
    guarded_void("setting the UPnP scope"sv, [&] {
      const auto scope = drain_scope();
#ifdef SUNSHINE_TESTS
      if (_seams.set_unmap_scope) {
        _seams.set_unmap_scope(scope);
        return;
      }
#endif
      upnp::set_unmap_scope(scope);
    });

    guarded_void("freezing the state"sv, [&] {
#ifdef SUNSHINE_TESTS
      if (_seams.freeze) {
        _seams.freeze(true);
        return;
      }
#endif
      nvhttp::set_state_frozen(true);
      runtime().set_frozen(true);
    });
  }

  void sunshine_old_ops_t::unfreeze() {
    // The scope goes back to `all` whatever else fails here.
    guarded_void("restoring the UPnP scope"sv, [&] {
#ifdef SUNSHINE_TESTS
      if (_seams.set_unmap_scope) {
        _seams.set_unmap_scope(upnp::unmap_scope_e::all);
        return;
      }
#endif
      upnp::set_unmap_scope(upnp::unmap_scope_e::all);
    });

    guarded_void("unfreezing the state"sv, [&] {
#ifdef SUNSHINE_TESTS
      if (_seams.freeze) {
        _seams.freeze(false);
        return;
      }
#endif
      nvhttp::set_state_frozen(false);
      runtime().set_frozen(false);
    });
  }

  void sunshine_old_ops_t::withdraw_mdns(std::chrono::milliseconds timeout) {
    guarded_void("withdrawing mDNS"sv, [&] {
#ifdef SUNSHINE_TESTS
      if (_seams.mdns_withdraw) {
        _seams.mdns_withdraw(timeout);
        return;
      }
#endif
      mdns_holder().withdraw(timeout);
    });
  }

  void sunshine_old_ops_t::publish_mdns() {
    const bool ok = guarded("publishing mDNS"sv, false, [&] {
#ifdef SUNSHINE_TESTS
      if (_seams.mdns_start) {
        return _seams.mdns_start();
      }
#endif
      return mdns_holder().start(false);
    });
    if (!ok) {
      BOOST_LOG(warning) << "Drain handoff: mDNS could not be published again"sv;
    }
  }

  void sunshine_old_ops_t::remove_tray() {
    const bool ok = guarded("removing the tray icon"sv, false, [&] {
#ifdef SUNSHINE_TESTS
      if (_seams.tray_suspend) {
        return _seams.tray_suspend(tray_suspend_timeout);
      }
#endif
      return system_tray::suspend(tray_suspend_timeout);
    });
    if (!ok) {
      BOOST_LOG(warning) << "Drain handoff: the tray icon did not confirm its removal in time"sv;
    }
  }

  void sunshine_old_ops_t::restore_tray() {
    const bool ok = guarded("restoring the tray icon"sv, false, [&] {
#ifdef SUNSHINE_TESTS
      if (_seams.tray_resume) {
        return _seams.tray_resume();
      }
#endif
      return system_tray::resume();
    });
    if (!ok) {
      BOOST_LOG(warning) << "Drain handoff: the tray icon could not be restored"sv;
    }
  }

  protocol::go_t sunshine_old_ops_t::take_snapshot() {
#ifdef SUNSHINE_TESTS
    if (_seams.snapshot) {
      return _seams.snapshot();
    }
#endif
    protocol::go_t go;
    go.app = proc::proc.snapshot();
    go.host_audio = nvhttp::host_audio();
    go.next_launch_session_id = nvhttp::next_launch_session_id();
    return go;
  }

  bool sunshine_old_ops_t::shutdown_requested() {
    return runtime().shutdown_requested();
  }

  void sunshine_old_ops_t::set_released(bool released) {
    guarded_void("setting the release flag"sv, [&] {
#ifdef SUNSHINE_TESTS
      if (_seams.set_released) {
        _seams.set_released(released);
        return;
      }
#endif
      proc::proc.set_released(released);
    });
  }

  void sunshine_old_ops_t::drop_app() {
    guarded_void("dropping the released app"sv, [&] {
#ifdef SUNSHINE_TESTS
      if (_seams.drop_app) {
        _seams.drop_app();
        BOOST_LOG(warning) << "Drain handoff: the released app was dropped without an undo"sv;
        return;
      }
#endif
      if (proc::proc.drop_released_app()) {
        BOOST_LOG(warning) << "Drain handoff: the released app was dropped without an undo"sv;
      }
    });
  }

  bool sunshine_old_ops_t::reload_after_rollback() {
#ifdef SUNSHINE_TESTS
    if (_seams.reload) {
      return guarded("the reload"sv, false, [&] {
        return _seams.reload();
      });
    }
#endif
    bool ok = true;

    ok &= guarded("reloading the state file"sv, false, [&] {
      if (nvhttp::reload_state() != nvhttp::load_status_e::loaded) {
        BOOST_LOG(error) << "Drain handoff: the state file could not be reloaded after the rollback"sv;
        return false;
      }
      return true;
    });

    ok &= guarded("reloading the credentials"sv, false, [&] {
      if (http::reload_user_creds(config::sunshine.credentials_file) != 0) {
        BOOST_LOG(error) << "Drain handoff: the credentials file could not be reloaded after the rollback"sv;
        return false;
      }
      return true;
    });

    ok &= guarded("reloading the app list"sv, false, [&] {
      auto parsed = proc::parse(config::stream.file_apps);
      if (!parsed) {
        BOOST_LOG(error) << "Drain handoff: the app list could not be reloaded after the rollback"sv;
        return false;
      }
      proc::proc.swap_app_list(std::move(parsed->get_apps()));
      return true;
    });

    return ok;
  }

  void sunshine_old_ops_t::enter_drain_mode() {
    // (1) The mode.
    if (!runtime().transition(mode_e::normal, mode_e::draining)) {
      BOOST_LOG(error) << "Drain handoff: the mode could not change to draining"sv;
    }

    // (2) The log tag.
    guarded_void("setting the log tag"sv, [&] {
      logging::set_process_tag(runtime().process_tag());
    });

    // (3) The control socket stays served until the helper has its answer; only the listener closes.
    _control.close_listener();

    // (4) The UPnP scope again (idempotent), as a second guard.
    guarded_void("setting the UPnP scope"sv, [&] {
      const auto scope = drain_scope();
#ifdef SUNSHINE_TESTS
      if (_seams.set_unmap_scope) {
        _seams.set_unmap_scope(scope);
        return;
      }
#endif
      upnp::set_unmap_scope(scope);
    });

    // (5) The deadline, counted from the switch; the exit stays forbidden until `handed_over()`.
    guarded_void("starting the drain controller"sv, [&] {
      const auto now = std::chrono::steady_clock::now();
#ifdef SUNSHINE_TESTS
      if (_seams.drain_begin) {
        _seams.drain_begin(now);
      } else {
        drain_controller().begin(now);
      }
      if (_seams.session_poke) {
        _seams.session_poke();
        return;
      }
#else
      drain_controller().begin(now);
#endif
      session_notifier().poke();
    });
  }

  void sunshine_old_ops_t::leave_drain_mode() {
    guarded_void("stopping the drain controller"sv, [&] {
      bool cancelled = true;
#ifdef SUNSHINE_TESTS
      if (_seams.drain_cancel) {
        _seams.drain_cancel();
      } else {
        cancelled = drain_controller().cancel();
      }
#else
      cancelled = drain_controller().cancel();
#endif
      if (!cancelled) {
        BOOST_LOG(warning) << "Drain handoff: the drain controller could not be cancelled"sv;
      }
    });

    if (!runtime().transition(mode_e::draining, mode_e::normal)) {
      BOOST_LOG(error) << "Drain handoff: the mode could not change back to normal"sv;
    }

    guarded_void("clearing the log tag"sv, [&] {
      logging::set_process_tag(runtime().process_tag());
    });

    // The only active process again: every mapping is its own.
    guarded_void("restoring the UPnP scope"sv, [&] {
#ifdef SUNSHINE_TESTS
      if (_seams.set_unmap_scope) {
        _seams.set_unmap_scope(upnp::unmap_scope_e::all);
        return;
      }
#endif
      upnp::set_unmap_scope(upnp::unmap_scope_e::all);
    });

    std::string open_error;
    if (!_control.reopen_listener(&open_error)) {
      BOOST_LOG(error) << "Drain handoff: the control socket could not be opened again: "sv << open_error;
    }
  }

  void sunshine_old_ops_t::set_role(role_e role) {
    runtime().instance_set_role(role);
  }

  void sunshine_old_ops_t::set_state(state_e state) {
    runtime().instance_set_state(state);

    // A draining process tells the active one; the message is best effort.
    const auto current = runtime().instance_current();
    if (current && current->role == role_e::draining) {
      const auto channel = runtime().channel();
      if (channel && !channel->send(protocol::handoff_state_t {std::string(to_string(state))})) {
        BOOST_LOG(debug) << "Drain handoff: HANDOFF_STATE could not be sent"sv;
      }
    }
  }

  void sunshine_old_ops_t::remove_new_instance_file() {
    if (_instances_dir.empty()) {
      return;
    }
    guarded_void("removing the instance file of the new process"sv, [&] {
      remove_instance_file(_instances_dir, _child_pid);
    });
  }

  void sunshine_old_ops_t::start_upnp_cleanup(int new_base) {
    if (!config::sunshine.flags[config::flag::UPNP]) {
      BOOST_LOG(debug) << "Drain handoff: UPnP is off; no mappings to remove"sv;
      return;
    }
    auto ports = upnp_ports_to_remove(new_base, _own_base, config::sunshine.port);
    if (ports.empty()) {
      BOOST_LOG(debug) << "Drain handoff: no UPnP mappings to remove"sv;
      return;
    }
    guarded_void("starting the UPnP cleanup"sv, [&] {
#ifdef SUNSHINE_TESTS
      if (_seams.remove_ports) {
        _seams.remove_ports(std::move(ports));
        return;
      }
#endif
      upnp::remove_ports_async(std::move(ports));
    });
  }

  bool sunshine_old_ops_t::handover(pid_t new_pid) {
    return guarded("the main PID handover"sv, false, [&] {
      std::string socket;
#ifdef SUNSHINE_TESTS
      if (_seams.notify_socket) {
        socket = _seams.notify_socket();
      } else
#endif
      {
        const char *value = std::getenv("NOTIFY_SOCKET");
        socket = value ? value : "";
      }
      if (socket.empty()) {
        BOOST_LOG(error) << "Drain handoff: handover failed at the notify step: NOTIFY_SOCKET is not set"sv;
        return false;
      }

      // Step 1: MAINPID=
      const auto payload = sd::mainpid_payload(new_pid);
      bool sent;
#ifdef SUNSHINE_TESTS
      sent = _seams.notify ? _seams.notify(payload) : sd::notify(socket, payload);
#else
      sent = sd::notify(socket, payload);
#endif
      if (!sent) {
        BOOST_LOG(error) << "Drain handoff: handover failed at the notify step"sv;
        return false;
      }

      // Step 2: the barrier.
      bool barrier_ok;
#ifdef SUNSHINE_TESTS
      barrier_ok = _seams.barrier ? _seams.barrier(handover_step_timeout) : sd::barrier(socket, handover_step_timeout);
#else
      barrier_ok = sd::barrier(socket, handover_step_timeout);
#endif
      if (!barrier_ok) {
        BOOST_LOG(error) << "Drain handoff: handover failed at the barrier step"sv;
        return false;
      }

      // Step 3: the read-back.
      std::optional<std::string> cgroup;
#ifdef SUNSHINE_TESTS
      cgroup = _seams.cgroup ? _seams.cgroup() : read_self_cgroup();
#else
      cgroup = read_self_cgroup();
#endif
      const auto unit = cgroup ? sd::unit_from_cgroup(*cgroup) : std::nullopt;
      if (!unit) {
        BOOST_LOG(error) << "Drain handoff: handover failed at the read-back step: the unit could not be determined"sv;
        return false;
      }
      const bool user_unit = sd::is_user_unit(*cgroup);
      std::optional<pid_t> main_pid;
#ifdef SUNSHINE_TESTS
      main_pid = _seams.read_back ? _seams.read_back(*unit, user_unit, handover_step_timeout) : sd::read_back_mainpid(*unit, user_unit, handover_step_timeout);
#else
      main_pid = sd::read_back_mainpid(*unit, user_unit, handover_step_timeout);
#endif
      if (!main_pid || *main_pid != new_pid) {
        BOOST_LOG(error) << "Drain handoff: handover failed at the read-back step: the main PID is not the new process"sv;
        return false;
      }
      return true;
    });
  }

  void sunshine_old_ops_t::close_socket_copies() {
    for (auto kind : all_listeners) {
      if (!listeners().close_listener(kind, server_op_timeout)) {
        BOOST_LOG(warning) << "Drain handoff: the "sv << to_string(kind) << " server did not confirm the close of its listener in time"sv;
      }
    }
  }

  // -------------------------------------------------------------------------------------------------------------------
  // The coordinator
  // -------------------------------------------------------------------------------------------------------------------

  struct coordinator_t::attempt_t {
    int new_base {0};  ///< The child's UDP base.
    std::shared_ptr<channel_t> channel;  ///< The old process's channel end.
    std::unique_ptr<child_ops_t> child;  ///< The child.
    std::unique_ptr<old_side_ops_t> ops;  ///< The old side's effects.
    std::unique_ptr<drain_responder_t> responder;  ///< Serves drain requests (rollback window and drain).
    std::shared_ptr<old_side_t> side;  ///< The driver; assigned under the coordinator mutex before the threads start.
    std::function<void(received_t)> on_drain_request;  ///< The responder's handler, as `run()` takes it.
    std::mutex watcher_mutex;  ///< Serialises the stop and join of the watcher.
    std::thread watcher;  ///< The shutdown watcher.
    int stop_fd {-1};  ///< Eventfd that stops the watcher.
    bool answered {false};  ///< The helper has been answered (guarded by the coordinator mutex).
    control_reply_t reply;  ///< The answer (guarded by the coordinator mutex).
    bool started {false};  ///< The run thread was started (guarded by the coordinator mutex).
    bool commit_possible {false};  ///< The switch reached the point just before `GO` (and so `COMMIT`) can be written (guarded by the coordinator mutex).
    bool run_done {false};  ///< The run thread finished (guarded by the coordinator mutex).
    bool stop_started {false};  ///< `stop()` began on this attempt (guarded by the coordinator mutex).
    bool stop_done {false};  ///< `stop()` finished on this attempt (guarded by the coordinator mutex).

    attempt_t() = default;
    attempt_t(const attempt_t &) = delete;
    attempt_t &operator=(const attempt_t &) = delete;

    ~attempt_t() {
      stop_watcher();
      if (stop_fd >= 0) {
        ::close(stop_fd);
      }
    }

    /**
     * @brief Stop and join the watcher. Safe from several threads and to call twice.
     */
    void stop_watcher() {
      std::lock_guard lock {watcher_mutex};
      if (watcher.joinable()) {
        if (stop_fd >= 0) {
          std::uint64_t one = 1;
          [[maybe_unused]] auto written = ::write(stop_fd, &one, sizeof(one));
        }
        watcher.join();
      }
    }

    // --- environment (the test seams of `coordinator_env_t`, else the production value) ---------------------------

    static runtime_t &rt(const coordinator_t &c) {
#ifdef SUNSHINE_TESTS
      if (c._env.runtime) {
        return *c._env.runtime;
      }
#endif
      return runtime();
    }

    static std::chrono::steady_clock::time_point now(const coordinator_t &c) {
#ifdef SUNSHINE_TESTS
      if (c._env.now) {
        return c._env.now();
      }
#endif
      return std::chrono::steady_clock::now();
    }

    static refusal_sources_t sources(const coordinator_t &c) {
#ifdef SUNSHINE_TESTS
      if (c._env.sources_set) {
        return c._env.sources;
      }
#endif
      return production_refusal_sources();
    }

    static void set_scope(const coordinator_t &c, upnp::unmap_scope_e scope) {
#ifdef SUNSHINE_TESTS
      if (c._env.set_unmap_scope) {
        c._env.set_unmap_scope(scope);
        return;
      }
#endif
      upnp::set_unmap_scope(scope);
    }

    static std::filesystem::path instances_dir(const coordinator_t &c) {
#ifdef SUNSHINE_TESTS
      return c._env.instances_dir;
#else
      (void) c;
      return production_instances_dir();
#endif
    }

    static std::optional<std::pair<int, int>> make_pair(const coordinator_t &c) {
#ifdef SUNSHINE_TESTS
      if (c._env.make_channel_pair) {
        return c._env.make_channel_pair();
      }
#endif
      (void) c;
      return make_channel_pair();
    }

    static std::optional<child_t> spawn(const coordinator_t &c, const spawn_request_t &request, std::string *error) {
#ifdef SUNSHINE_TESTS
      if (c._env.spawn) {
        return c._env.spawn(request, error);
      }
#endif
      (void) c;
      return spawn_child(request, error);
    }

    static std::optional<std::uint64_t> start_time(const coordinator_t &c, pid_t pid) {
#ifdef SUNSHINE_TESTS
      if (c._env.start_time) {
        return c._env.start_time(pid);
      }
#endif
      (void) c;
      return read_start_time(pid);
    }

    static std::unique_ptr<child_ops_t> make_child(const coordinator_t &c, child_t child) {
#ifdef SUNSHINE_TESTS
      if (c._env.make_child) {
        return c._env.make_child(child);
      }
#endif
      (void) c;
      return std::make_unique<pidfd_child_t>(child);
    }

    static std::vector<std::string> argv(const coordinator_t &c) {
#ifdef SUNSHINE_TESTS
      if (c._env.argv) {
        return c._env.argv();
      }
#endif
      (void) c;
      std::vector<std::string> arguments;
      if (char **list = lifetime::get_argv()) {
        for (; *list; ++list) {
          arguments.emplace_back(*list);
        }
      }
      return arguments;
    }

    static std::string host_sink(const coordinator_t &c) {
#ifdef SUNSHINE_TESTS
      if (c._env.host_sink) {
        return c._env.host_sink();
      }
#endif
      (void) c;
      return audio::recorded_host_sink().value_or(std::string {});
    }

    // --- steps ----------------------------------------------------------------------------------------------------

    /**
     * @brief The common end of an attempt that did not hand over: stop the helpers, close the channel, re-arm the
     * cooldown, clear the in-progress flag, restore the UPnP scope where it applies, release the claim.
     *
     * @param c Coordinator.
     * @param att Attempt.
     * @param rearm Re-arm the cooldown.
     * @param scope_all Set the UPnP scope back to `all`.
     */
    static void finish_failed(coordinator_t &c, const std::shared_ptr<attempt_t> &att, bool rearm, bool scope_all) {
      auto &runtime_state = rt(c);
      att->stop_watcher();
      if (att->responder) {
        att->responder->stop();
      }
      if (att->channel) {
        att->channel->close();
      }
      runtime_state.set_channel(nullptr);
      if (rearm) {
        c.set_cooldown_until(now(c) + std::chrono::seconds(runtime_state.settings().cooldown_s));
      }
      runtime_state.set_handoff_in_progress(false);
      if (scope_all) {
        guarded_void("restoring the UPnP scope"sv, [&] {
          set_scope(c, upnp::unmap_scope_e::all);
        });
      }
      {
        std::lock_guard lock {c._mutex};
        c._claimed = false;
      }
      c._cv.notify_all();
    }

    /**
     * @brief The shutdown watcher: forwards the shutdown fd's readiness to `old_side_t::request_shutdown()` once.
     *
     * @param c Coordinator.
     * @param att Attempt (alive until the watcher is joined).
     */
    static void watch(const coordinator_t &c, attempt_t *att) {
      struct pollfd fds[2];
      fds[0] = {rt(c).shutdown_wake_fd(), POLLIN, 0};
      fds[1] = {att->stop_fd, POLLIN, 0};
      while (true) {
        auto rc = ::poll(fds, 2, -1);
        if (rc < 0) {
          if (errno == EINTR) {
            continue;
          }
          BOOST_LOG(error) << "Drain handoff: the shutdown watcher stopped: "sv << std::strerror(errno);
          return;
        }
        if (fds[1].revents != 0) {
          return;
        }
        if (fds[0].revents != 0) {
          att->side->request_shutdown();
          return;
        }
      }
    }

    /**
     * @brief The wiring after a handover (run thread, no lock held).
     *
     * @param c Coordinator.
     * @param att Attempt.
     */
    static void post_handover(coordinator_t &c, const std::shared_ptr<attempt_t> &att) {
      auto &runtime_state = rt(c);
      {
        std::lock_guard lock {c._mutex};
        c._handed_over = true;
      }
      c._cv.notify_all();

      // (1) The UPnP scope again, before anything else can end this process.
      guarded_void("setting the UPnP scope"sv, [&] {
        set_scope(c, scope_for_drain(att->new_base, hooks::udp_base()));
      });

      // (2) The handoff is over.
      runtime_state.set_handoff_in_progress(false);

      // (3) No shutdown watcher: the draining process shuts down through the normal path.
      att->stop_watcher();

      // (4) The channel reader serves the drain requests with the same responder.
      std::unique_ptr<channel_reader_t> reader;
      guarded_void("starting the channel reader"sv, [&] {
        auto *responder = att->responder.get();
        reader = std::make_unique<channel_reader_t>(
          *att->channel,
          false,
          [responder](received_t message) {
            responder->handle(std::move(message));
          },
          [](recv_status_e) {
            BOOST_LOG(warning) << "Drain handoff: the channel to the active process closed; this process keeps draining"sv;
          }
        );
        reader->start();
      });

      // (5) The revocation poller.
      std::unique_ptr<revocation_poller_t> poller;
      guarded_void("starting the revocation poller"sv, [&] {
#ifdef SUNSHINE_TESTS
        poller = c._env.make_poller ? c._env.make_poller() : std::make_unique<revocation_poller_t>();
#else
        poller = std::make_unique<revocation_poller_t>();
#endif
        if (!poller->start()) {
          BOOST_LOG(error) << "Drain handoff: the revocation poller could not start; the drain goes on"sv;
        }
      });

      {
        std::lock_guard lock {c._mutex};
        c._reader = std::move(reader);
        c._poller = std::move(poller);
      }

      // (6) The drain exit may run now.
      guarded_void("allowing the drain exit"sv, [&] {
#ifdef SUNSHINE_TESTS
        if (c._env.drain_handed_over) {
          c._env.drain_handed_over();
          return;
        }
#endif
        drain_controller().handed_over();
      });
    }

    /**
     * @brief The run thread: `old_side_t::run()`, the fallback answer, and the end of the attempt.
     *
     * @param c Coordinator.
     * @param att Attempt.
     */
    static void run(coordinator_t &c, std::shared_ptr<attempt_t> att) {
      old_outcome_e outcome = old_outcome_e::shutdown;
      bool completed = false;
      try {
        outcome = att->side->run(att->on_drain_request);
        completed = true;
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Drain handoff: the handoff sequence ended with an exception: "sv << e.what();
      } catch (...) {
        BOOST_LOG(error) << "Drain handoff: the handoff sequence ended with an exception"sv;
      }

      // The helper is answered exactly once; if `run()` ended without any answer, it is answered here.
      {
        std::lock_guard lock {c._mutex};
        if (!att->answered) {
          att->reply = control_reply_t {reply_kind_e::failed, std::string(coordinator_reason::no_outcome)};
          att->answered = true;
        }
      }
      c._cv.notify_all();

      if (completed && outcome == old_outcome_e::handed_over) {
        post_handover(c, att);
      } else {
        const bool rolled_back = completed && outcome == old_outcome_e::rolled_back;
        bool commit_possible;
        {
          std::lock_guard lock {c._mutex};
          commit_possible = att->commit_possible || att->reply.kind == reply_kind_e::committed;
        }
        commit_possible = commit_possible || rt(c).mode() == mode_e::draining;
        // Once COMMIT may have been written, the new process may own the shared mappings. Every outcome other than a
        // completed rollback (the new process confirmed gone) keeps the scope the freeze set. A mapping may be left
        // behind at shutdown; the active process's mapping is never removed.
        finish_failed(c, att, rolled_back, rolled_back || !commit_possible);
      }

      {
        std::lock_guard lock {c._mutex};
        att->run_done = true;
      }
      c._cv.notify_all();
    }
  };

  coordinator_t::coordinator_t(control_server_t &control):
      _control(control) {
#ifdef SUNSHINE_TESTS
    _env.instances_dir = production_instances_dir();
#endif
  }

#ifdef SUNSHINE_TESTS
  coordinator_t::coordinator_t(control_server_t &control, coordinator_env_t env):
      _control(control),
      _env(std::move(env)) {
  }

  end_all_machine_t &coordinator_t::end_all_for_test() {
    return _end_all;
  }
#endif

  coordinator_t::~coordinator_t() {
    stop();
  }

  control_server_t::handler_t coordinator_t::handler() {
    return [this](pid_t peer_pid) {
      return handle(peer_pid);
    };
  }

  std::chrono::steady_clock::time_point coordinator_t::cooldown_until() const {
    std::lock_guard lock {_mutex};
    return _cooldown_until;
  }

  void coordinator_t::set_cooldown_until(std::chrono::steady_clock::time_point deadline) {
    std::lock_guard lock {_mutex};
    _cooldown_until = std::max(_cooldown_until, deadline);
  }

  bool coordinator_t::attempt_running() const {
    std::lock_guard lock {_mutex};
    return _claimed && !_handed_over;
  }

  bool coordinator_t::handed_over() const {
    std::lock_guard lock {_mutex};
    return _handed_over;
  }

  control_reply_t coordinator_t::handle(pid_t peer_pid) {
    using A = attempt_t;
    auto &runtime_state = A::rt(*this);

    auto release_claim = [this]() {
      {
        std::lock_guard lock {_mutex};
        _claimed = false;
      }
      _cv.notify_all();
    };
    auto refuse = [&](std::string_view reason, bool release) {
      log_refusal(reason, peer_pid);
      if (release) {
        release_claim();
      }
      return control_reply_t {reply_kind_e::refused, std::string(reason)};
    };

    // 1. Kill pending, before the claim is looked at; then the claim.
    if (const auto current = runtime_state.instance_current(); current && current->handoff == state_e::kill_pending) {
      return refuse(refusal_reason::kill_pending, false);
    }
    {
      std::unique_lock lock {_mutex};
      if (_claimed) {
        const auto reason = _handed_over ? coordinator_reason::draining : refusal_reason::in_progress;
        lock.unlock();
        return refuse(reason, false);
      }
      _claimed = true;
    }

    // 2. Shutdown.
    bool stopping;
    {
      std::lock_guard lock {_mutex};
      stopping = _stopping;
    }
    if (stopping || runtime_state.shutdown_requested()) {
      return refuse(coordinator_reason::shutting_down, true);
    }

    // 3. Mode.
    switch (runtime_state.mode()) {
      case mode_e::normal:
        break;
      case mode_e::draining:
        return refuse(coordinator_reason::draining, true);
      case mode_e::new_pending:
      case mode_e::new_active:
        return refuse(refusal_reason::in_progress, true);
      case mode_e::disabled:
        return refuse(coordinator_reason::not_active, true);
    }

    // 4. Refusals: every source is read once, with no lock held, then the rules decide.
    {
      const auto sources = A::sources(*this);
      const auto inputs = gather_refusal_inputs(sources, cooldown_until());
      const auto reason = check_refusal(inputs);
      if (!reason.empty()) {
        return refuse(reason, true);
      }
    }

    // 5. Listeners.
    inherited_fds_t fds;
    {
      const int fd_values[4] = {
        listeners().fd(listener_e::https),
        listeners().fd(listener_e::http),
        listeners().fd(listener_e::webui),
        listeners().fd(listener_e::rtsp),
      };
      for (std::size_t i = 0; i < 4; ++i) {
        if (fd_values[i] < 0) {
          return refuse(std::string(coordinator_reason::listener_missing_prefix) + std::string(to_string(all_listeners[i])), true);
        }
      }
      fds.https = fd_values[0];
      fds.http = fd_values[1];
      fds.webui = fd_values[2];
      fds.rtsp = fd_values[3];
    }

    // 6. The cooldown counts from here, whatever the outcome.
    const auto settings = runtime_state.settings();
    set_cooldown_until(A::now(*this) + std::chrono::seconds(settings.cooldown_s));

    // 7. Attempt state.
    runtime_state.set_handoff_in_progress(true);
    std::thread previous;
    {
      std::lock_guard lock {_mutex};
      previous = std::move(_run_thread);
    }
    if (previous.joinable()) {
      previous.join();
    }

    auto att = std::make_shared<attempt_t>();
    {
      std::lock_guard lock {_mutex};
      _attempt = att;
    }
    _end_all.reset();

    auto fail = [&](const std::string &reason) {
      BOOST_LOG(error) << "Drain handoff failed: "sv << reason;
      {
        std::lock_guard lock {_mutex};
        att->answered = true;
      }
      A::finish_failed(*this, att, true, true);
      return control_reply_t {reply_kind_e::failed, reason};
    };

    auto pair = A::make_pair(*this);
    if (!pair) {
      return fail(std::string(coordinator_reason::channel_failed));
    }
    att->channel = std::make_shared<channel_t>(pair->first);
    const int child_channel_fd = pair->second;
    runtime_state.set_channel(att->channel);

    // 8. Spawn.
    att->new_base = other_udp_base(config::sunshine.port, settings.udp_offset, hooks::udp_base());
    spawn_request_t request;
    request.binary = settings.binary.empty() ? "/proc/self/exe"s : settings.binary;
    request.argv = A::argv(*this);
    request.extra_env = child_environment(att->new_base, runtime_state.generation() + 1);
    {
      const auto sink = A::host_sink(*this);
      if (!sink.empty() && sink.rfind(virtual_sink_prefix, 0) != 0) {
        request.extra_env.push_back("SUNSHINE_HANDOFF_HOST_SINK="s + sink);
      }
    }
    request.fds = fds;
    request.fds.channel = child_channel_fd;
    std::string spawn_error;
    const auto spawned = A::spawn(*this, request, &spawn_error);
    ::close(child_channel_fd);
    if (!spawned) {
      return fail(std::string(coordinator_reason::spawn_failed_prefix) + spawn_error);
    }

    // 9. The child.
    const auto child_start = A::start_time(*this, spawned->pid);
    auto child = A::make_child(*this, *spawned);
    if (!child_start) {
      early_kill(*child);
      return fail(std::string(coordinator_reason::start_time_unreadable));
    }
    {
      std::lock_guard lock {_mutex};
      stopping = _stopping;
    }
    if (stopping || runtime_state.shutdown_requested()) {
      early_kill(*child);
      return fail(std::string(coordinator_reason::shutting_down));
    }
    att->child = std::move(child);

    // 10. Run.
    old_side_timeouts_t timeouts;
    timeouts.ready = std::chrono::seconds(settings.ready_timeout_s);
    timeouts.rollback_window = std::chrono::seconds(settings.rollback_window_s);
    timeouts.pairing = config::stream.ping_timeout;
#ifdef SUNSHINE_TESTS
    if (_env.adjust_timeouts) {
      _env.adjust_timeouts(timeouts);
    }
#endif

    const auto cooldown = cooldown_until();
#ifdef SUNSHINE_TESTS
    if (_env.make_ops) {
      att->ops = _env.make_ops(att->child->pid(), att->new_base, cooldown);
    }
#endif
    if (!att->ops) {
      att->ops = std::make_unique<sunshine_old_ops_t>(_control, att->new_base, att->child->pid(), cooldown, A::instances_dir(*this));
    }
    // Note the point just before `GO` is written: from there on `COMMIT` may be written and the new process may commit.
    att->ops = std::make_unique<tracking_ops_t>(std::move(att->ops), [this, raw = att.get()]() {
      std::lock_guard lock {_mutex};
      raw->commit_possible = true;
    });

#ifdef SUNSHINE_TESTS
    if (_env.make_responder) {
      att->responder = _env.make_responder(*att->channel, _end_all);
    }
#endif
    if (!att->responder) {
      att->responder = std::make_unique<drain_responder_t>(*att->channel, _end_all);
    }
    {
      auto *responder = att->responder.get();
      att->on_drain_request = [responder](received_t message) {
        responder->handle(std::move(message));
      };
    }

    attempt_t *raw = att.get();
    auto answer_helper = [this, raw](const control_reply_t &reply) {
      {
        std::lock_guard lock {_mutex};
        if (raw->answered) {
          BOOST_LOG(debug) << "Drain handoff: a second answer for the helper was ignored"sv;
          return;
        }
        raw->reply = reply;
        raw->answered = true;
      }
      _cv.notify_all();
    };
    auto side = std::make_shared<old_side_t>(*att->ops, *att->child, *att->channel, _end_all, timeouts, att->new_base, answer_helper);

    att->stop_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (att->stop_fd < 0) {
      early_kill(*att->child);
      return fail("cannot create the shutdown watcher"s);
    }

    // Both threads start in one critical section with the stopping check, so `stop()` always sees them.
    bool refused_by_stop = false;
    bool thread_error = false;
    {
      std::lock_guard lock {_mutex};
      if (_stopping) {
        refused_by_stop = true;
      } else {
        att->side = side;
        try {
          att->watcher = std::thread([this, raw]() {
            A::watch(*this, raw);
          });
          _run_thread = std::thread([this, att]() {
            A::run(*this, att);
          });
          att->started = true;
        } catch (const std::system_error &e) {
          BOOST_LOG(error) << "Drain handoff: cannot start a thread: "sv << e.what();
          thread_error = true;
        }
      }
    }
    if (refused_by_stop || thread_error) {
      // The child was started but the sequence never ran: kill-first, and answer.
      if (att->watcher.joinable()) {
        att->stop_watcher();
      }
      early_kill(*att->child);
      return fail(refused_by_stop ? std::string(coordinator_reason::shutting_down) : "cannot start the handoff thread"s);
    }

    // 11. Answer: the first call of `answer_helper` (or the run thread's fallback).
    std::unique_lock lock {_mutex};
    _cv.wait(lock, [&] {
      return att->answered;
    });
    return att->reply;
  }

  bool coordinator_t::start_control(const std::string &path) {
    // The whole start runs under the mutex, so two callers start one server. `control_server_t::start()` only binds and
    // starts its thread; it never calls the handler or the coordinator before it returns.
    std::lock_guard lock {_mutex};
    if (_stopping) {
      return false;
    }
    if (_control_started) {
      return true;
    }
    std::string open_error;
    if (!_control.start(path, handler(), &open_error)) {
      BOOST_LOG(error) << "Drain handoff: the control socket could not be opened: "sv << open_error;
      return false;
    }
    _control_started = true;
    return true;
  }

  void coordinator_t::stop_control() {
    _control.stop();
    std::lock_guard lock {_mutex};
    _control_started = false;
  }

  void coordinator_t::request_shutdown() {
    std::shared_ptr<old_side_t> side;
    {
      std::lock_guard lock {_mutex};
      _stopping = true;
      if (_attempt) {
        side = _attempt->side;
      }
    }
    if (side) {
      side->request_shutdown();
    }
  }

  bool coordinator_t::join_attempt(std::chrono::milliseconds timeout) {
    std::unique_lock lock {_mutex};
    const auto att = _attempt;
    if (!att) {
      return true;
    }
    const bool done = _cv.wait_for(lock, timeout, [&] {
      return !att->started || att->run_done;
    });
    if (!done) {
      lock.unlock();
      BOOST_LOG(error) << "Drain handoff: the handoff attempt did not finish within "sv << std::chrono::duration_cast<std::chrono::seconds>(timeout).count() << " s at shutdown"sv;
      return false;
    }
    return true;
  }

  void coordinator_t::stop() {
    std::shared_ptr<attempt_t> att;
    std::thread run_thread;
    {
      std::unique_lock lock {_mutex};
      _stopping = true;
      att = _attempt;
      if (!att) {
        return;
      }
      if (att->stop_started) {
        // A second caller waits for the first one's joins.
        _cv.wait(lock, [&] {
          return att->stop_done;
        });
        return;
      }
      att->stop_started = true;
      run_thread = std::move(_run_thread);
    }

    // Forward the shutdown to a running attempt, then join its threads.
    if (att->side) {
      att->side->request_shutdown();
    }
    if (run_thread.joinable()) {
      run_thread.join();
    }
    att->stop_watcher();

    // What lives past the run thread: the poller, the reader, the responder and the channel.
    std::unique_ptr<revocation_poller_t> poller;
    std::unique_ptr<channel_reader_t> reader;
    {
      std::lock_guard lock {_mutex};
      poller = std::move(_poller);
      reader = std::move(_reader);
    }
    if (poller) {
      poller->stop();
    }
    if (reader) {
      reader->stop();
    }
    if (att->responder) {
      att->responder->stop();
    }
    if (att->channel) {
      att->channel->close();
      auto &runtime_state = attempt_t::rt(*this);
      if (runtime_state.channel() == att->channel) {
        runtime_state.set_channel(nullptr);
      }
    }
    poller.reset();
    reader.reset();

    {
      std::lock_guard lock {_mutex};
      att->stop_done = true;
    }
    _cv.notify_all();
  }
}  // namespace handoff
