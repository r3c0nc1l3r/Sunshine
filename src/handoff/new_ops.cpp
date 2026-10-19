/**
 * @file src/handoff/new_ops.cpp
 * @brief Definitions for `new_ops.h`.
 *
 * The ops run on the new-side thread; the milestone members are called from `main()`, the servers' listening callbacks
 * and the shutdown path. One mutex guards the milestone fields; it is never held while a seam is called.
 */
// standard includes
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <future>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

// local includes
#include "coordinator.h"
#include "hooks.h"
#include "instance_file.h"
#include "mdns_holder.h"
#include "new_ops.h"
#include "probe.h"
#include "runtime.h"
#include "sd_notify.h"
#include "spawn.h"
#include "src/config.h"
#include "src/entry_handler.h"
#include "src/httpcommon.h"
#include "src/logging.h"
#include "src/nvhttp.h"
#include "src/process.h"
#include "src/system_tray.h"
#include "src/upnp.h"
#include "src/video.h"

using namespace std::literals;

namespace handoff {
  namespace {
    /**
     * @brief Move a stage forward; never backward.
     *
     * @param stage The stage variable.
     * @param next The stage to reach.
     */
    void advance(std::atomic<new_stage_e> &stage, new_stage_e next) {
      auto current = stage.load();
      while (current < next && !stage.compare_exchange_weak(current, next)) {
      }
    }

    std::mutex coordinator_mutex;  ///< Guards `current_coordinator` and every call made through it.
    coordinator_t *current_coordinator = nullptr;  ///< The coordinator of this process, or null.
    std::optional<std::chrono::steady_clock::time_point> pending_cooldown;  ///< Deadline received while there was no coordinator.

    /**
     * @brief The state that the default `prepare_adoption` and `install_adoption` seams share.
     */
    struct adoption_state_t {
      std::optional<proc::adoption_plan_t> plan;  ///< The prepared plan.
      std::optional<protocol::go_t> go;  ///< The `GO` message the plan came from.
    };

    /**
     * @brief Name of a load status, for the reason of a failed reload.
     *
     * @param status Status.
     * @return Name.
     */
    std::string_view load_status_name(nvhttp::load_status_e status) {
      switch (status) {
        case nvhttp::load_status_e::loaded:
          return "loaded"sv;
        case nvhttp::load_status_e::no_file:
          return "no_file"sv;
        case nvhttp::load_status_e::no_uniqueid:
          return "no_uniqueid"sv;
        case nvhttp::load_status_e::unreadable:
          return "unreadable"sv;
      }
      return "unknown"sv;
    }

    /**
     * @brief Production `reload_all`: the state file, the credentials file and `apps.json`, in this order.
     *
     * @return Empty on success; otherwise a one-line reason naming the file and the status.
     */
    std::string reload_all_files() {
      if (const auto status = nvhttp::reload_state(); status != nvhttp::load_status_e::loaded) {
        return "sunshine_state.json: " + std::string {load_status_name(status)};
      }

      if (http::reload_user_creds(config::sunshine.credentials_file) != 0) {
        return "credentials file: reload failed";
      }

      auto apps = proc::parse(config::stream.file_apps);
      if (!apps) {
        return "apps.json: parse failed";
      }
      proc::proc.swap_app_list(std::move(apps->get_apps()));
      return {};
    }

    /**
     * @brief Production `resume_servers`: resume the four servers at once, each bounded by `timeout`.
     *
     * @param timeout Timeout for each server.
     * @return Names of the servers whose resume did not confirm.
     */
    std::vector<std::string> resume_all_servers(std::chrono::milliseconds timeout) {
      constexpr std::array kinds {listener_e::https, listener_e::http, listener_e::webui, listener_e::rtsp};
      std::array<std::future<bool>, kinds.size()> results;
      for (std::size_t i = 0; i < kinds.size(); ++i) {
        results[i] = std::async(std::launch::async, [kind = kinds[i], timeout]() {
          return listeners().resume(kind, timeout);
        });
      }

      std::vector<std::string> failed;
      for (std::size_t i = 0; i < kinds.size(); ++i) {
        if (!results[i].get()) {
          failed.emplace_back(to_string(kinds[i]));
        }
      }
      return failed;
    }

    /**
     * @brief Production `start_control_server`: start the control socket of the coordinator of this process.
     */
    void start_control_through_coordinator() {
      if (!runtime().enabled()) {
        return;
      }

      const char *runtime_directory = std::getenv("RUNTIME_DIRECTORY");
      const char *xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR");
      const auto dir = runtime_dir(runtime_directory ? runtime_directory : "", xdg_runtime_dir ? xdg_runtime_dir : "");
      if (!dir) {
        BOOST_LOG(warning) << "No runtime directory: the control socket is not started"sv;
        return;
      }

      std::lock_guard lock {coordinator_mutex};
      if (current_coordinator) {
        current_coordinator->start_control(control_socket_path(*dir, getpid()).string());
      } else {
        BOOST_LOG(error) << "control server not started: no coordinator"sv;
      }
    }

    /**
     * @brief Fill every null member of `seams` with its production default.
     *
     * @param seams Seams to complete.
     * @param config Configuration facts the defaults use.
     */
    void fill_defaults(new_ops_seams_t &seams, const new_ops_config_t &config) {
      if (!seams.set_unmap_scope) {
        seams.set_unmap_scope = [](upnp::unmap_scope_e scope) {
          upnp::set_unmap_scope(scope);
        };
      }
      if (!seams.own_identity) {
        seams.own_identity = []() {
          return gather_probe().identity;
        };
      }
      if (!seams.set_probe_override) {
        seams.set_probe_override = [](const protocol::probe_results_t &results) {
          video::handoff_probe_override_t value;
          value.hevc_mode = results.hevc_mode;
          value.av1_mode = results.av1_mode;
          value.yuv444 = {results.yuv444_h264, results.yuv444_hevc, results.yuv444_av1};
          video::set_handoff_probe_override(value);
        };
      }
      if (!seams.set_cooldown_deadline_ns) {
        seams.set_cooldown_deadline_ns = [](std::int64_t deadline_ns) {
          std::lock_guard lock {coordinator_mutex};
          const std::chrono::steady_clock::time_point deadline {std::chrono::nanoseconds {deadline_ns}};
          if (current_coordinator) {
            current_coordinator->set_cooldown_until(deadline);
          } else {
            pending_cooldown = deadline;
          }
        };
      }
      if (!seams.validate_listener) {
        seams.validate_listener = [](int fd, int port, bool ipv6) {
          return handoff::validate_listener(fd, port, ipv6);
        };
      }
      if (!seams.reload_all) {
        seams.reload_all = []() {
          return reload_all_files();
        };
      }

      // The prepared plan lives in the closures until the install seam consumes it.
      auto state = std::make_shared<adoption_state_t>();
      if (!seams.prepare_adoption) {
        seams.prepare_adoption = [state](const protocol::go_t &go) {
          std::string reason;
          auto plan = proc::proc.prepare_adoption(go, reason);
          if (!plan) {
            return reason.empty() ? "the app could not be adopted"s : reason;
          }
          state->plan = std::move(*plan);
          state->go = go;
          return std::string {};
        };
      }
      if (!seams.install_adoption) {
        seams.install_adoption = [state]() {
          if (state->plan) {
            proc::proc.install_adoption(std::move(*state->plan));
            state->plan.reset();
          }
          if (state->go) {
            nvhttp::set_host_audio(state->go->host_audio);
            nvhttp::set_next_launch_session_id(state->go->next_launch_session_id);
          }
        };
      }

      if (!seams.resume_servers) {
        seams.resume_servers = [](std::chrono::milliseconds timeout) {
          return resume_all_servers(timeout);
        };
      }
      if (!seams.publish_mdns) {
        seams.publish_mdns = []() {
          return mdns_holder().start(true);
        };
      }
      if (!seams.start_upnp) {
        seams.start_upnp = []() {
          return upnp::start(hooks::udp_base());
        };
      }
      if (!seams.request_tray_start) {
        seams.request_tray_start = [tray = config.tray_enabled]() {
          if (tray) {
            system_tray::set_lifecycle_entries_enabled(false);
            system_tray::request_start();
          }
        };
      }
      if (!seams.enable_tray_entries) {
        seams.enable_tray_entries = [tray = config.tray_enabled]() {
          if (tray) {
            system_tray::set_lifecycle_entries_enabled(true);
          }
        };
      }
      if (!seams.notify_ready) {
        seams.notify_ready = []() {
          const char *notify_socket = std::getenv("NOTIFY_SOCKET");
          if (notify_socket && *notify_socket) {
            sd::notify(notify_socket, "READY=1"sv);
          }
        };
      }
      if (!seams.start_control_server) {
        seams.start_control_server = []() {
          start_control_through_coordinator();
        };
      }
      if (!seams.exit_process) {
        seams.exit_process = [](int code) {
          lifetime::exit_sunshine(code, true);
        };
      }
      if (!seams.shutdown_channel) {
        seams.shutdown_channel = [](channel_t &channel) {
          if (const int fd = channel.fd(); fd != -1) {
            ::shutdown(fd, SHUT_RDWR);
          }
        };
      }
    }
  }  // namespace

  void set_new_ops_coordinator(coordinator_t *coordinator) {
    std::lock_guard lock {coordinator_mutex};
    current_coordinator = coordinator;
    if (coordinator && pending_cooldown) {
      coordinator->set_cooldown_until(*pending_cooldown);
    }
    pending_cooldown.reset();
  }

  sunshine_new_ops_t::sunshine_new_ops_t(const new_ops_config_t &config, std::shared_ptr<channel_t> channel, std::unique_ptr<platf::deinit_t> &upnp_slot):
      _config {config},
      _channel {std::move(channel)},
      _upnp_slot {upnp_slot} {
    fill_defaults(_seams, _config);
  }

#ifdef SUNSHINE_TESTS
  sunshine_new_ops_t::sunshine_new_ops_t(const new_ops_config_t &config, std::shared_ptr<channel_t> channel, std::unique_ptr<platf::deinit_t> &upnp_slot, const new_ops_seams_t &seams):
      _config {config},
      _seams {seams},
      _channel {std::move(channel)},
      _upnp_slot {upnp_slot} {
    fill_defaults(_seams, _config);
  }
#endif

  sunshine_new_ops_t::~sunshine_new_ops_t() {
    if (const int fd = _old_pidfd.exchange(-1); fd >= 0) {
      ::close(fd);
    }
  }

  void sunshine_new_ops_t::close_inherited_listeners() {
    {
      std::lock_guard lock {_mutex};
      if (_listeners_closed) {
        return;
      }
      _listeners_closed = true;
    }

    // The servers hold their own duplicates; the channel fd is not touched here.
    if (const auto inherited = runtime().inherited()) {
      for (const int fd : {inherited->https, inherited->http, inherited->webui, inherited->rtsp}) {
        if (fd >= 0) {
          ::close(fd);
        }
      }
    }
  }

  protocol::hello_t sunshine_new_ops_t::hello() {
    _stage = new_stage_e::starting;
    return protocol::hello_t {};
  }

  void sunshine_new_ops_t::accept_old_hello(const protocol::hello_t &hello, int old_pidfd) {
    if (const int previous = _old_pidfd.exchange(old_pidfd); previous >= 0 && previous != old_pidfd) {
      ::close(previous);
    }
    runtime().set_channel(_channel);

    if (hello.cooldown_deadline_ns) {
      _seams.set_cooldown_deadline_ns(*hello.cooldown_deadline_ns);
    }

    const auto accepted = accepted_probe(hello, _seams.own_identity());
    if (accepted) {
      _seams.set_probe_override(*accepted);
    } else {
      BOOST_LOG(info) << "The encoder probe results of the previous process are not used; the encoder probe runs"sv;
    }

    {
      std::lock_guard lock {_mutex};
      _hello_done = true;
    }
    _cv.notify_all();
  }

  std::string sunshine_new_ops_t::validate_listeners() {
    std::string reason;
    if (!_config.startup_refusal.empty()) {
      reason = _config.startup_refusal;
    } else if (!_config.feature_enabled) {
      reason = "drain handoff is disabled in the new process's configuration";
    } else if (const auto inherited = runtime().inherited(); !inherited) {
      reason = "not started by a handoff";
    } else {
      struct check_t {
        listener_e kind;
        int fd;
        int port;
      };

      const std::array checks {
        check_t {listener_e::https, inherited->https, _config.port - 5},
        check_t {listener_e::http, inherited->http, _config.port},
        check_t {listener_e::webui, inherited->webui, _config.port + 1},
        check_t {listener_e::rtsp, inherited->rtsp, _config.port + 21},
      };
      for (const auto &check : checks) {
        if (auto why = _seams.validate_listener(check.fd, check.port, _config.ipv6); !why.empty()) {
          reason = std::string {to_string(check.kind)} + ": " + why;
          break;
        }
      }
    }

    if (!reason.empty()) {
      BOOST_LOG(warning) << "Listener validation failed: "sv << reason;
    }

    {
      std::lock_guard lock {_mutex};
      _validation = reason.empty() ? validation_e::ok : validation_e::failed;
    }
    _cv.notify_all();
    return reason;
  }

  std::string sunshine_new_ops_t::start_servers_paused() {
    const auto deadline = std::chrono::steady_clock::now() + _config.servers_listening_timeout;
    std::unique_lock lock {_mutex};
    while (true) {
      if (_servers_failed) {
        return *_servers_failed;
      }
      if (_listening >= 4) {
        break;
      }
      if (_aborted || runtime().shutdown_requested()) {
        return "shutdown requested";
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return "servers did not start listening within " +
               std::to_string(std::chrono::duration_cast<std::chrono::seconds>(_config.servers_listening_timeout).count()) +
               " s (" + std::to_string(_listening) + " of 4)";
      }
      _cv.wait_until(lock, std::min(deadline, now + 100ms));
    }
    lock.unlock();

    close_inherited_listeners();
    advance(_stage, new_stage_e::ready);
    return {};
  }

  std::string sunshine_new_ops_t::reload_all() {
    advance(_stage, new_stage_e::go);
    return _seams.reload_all();
  }

  std::string sunshine_new_ops_t::prepare_adoption(const protocol::go_t &go) {
    return _seams.prepare_adoption(go);
  }

  void sunshine_new_ops_t::install_adoption() {
    {
      std::lock_guard lock {_mutex};
      if (_aborted) {
        _commit_failed = true;
        BOOST_LOG(warning) << "A shutdown was requested before the adoption was installed; nothing is installed"sv;
        return;
      }
      advance(_stage, new_stage_e::committing);
    }

    runtime().transition(mode_e::new_pending, mode_e::new_active);
    _seams.install_adoption();
  }

  void sunshine_new_ops_t::resume_accept() {
    if (_commit_failed) {
      return;
    }

    const auto failed = _seams.resume_servers(_config.resume_timeout);
    if (failed.empty()) {
      return;
    }

    std::string names;
    for (const auto &name : failed) {
      names += (names.empty() ? "" : ", ") + name;
    }
    BOOST_LOG(error) << "Resume did not confirm: "sv << names;
    _commit_failed = true;
  }

  void sunshine_new_ops_t::publish_mdns() {
    if (_commit_failed) {
      return;
    }

    if (!_seams.publish_mdns()) {
      BOOST_LOG(warning) << "mDNS is not available; the service is not published"sv;
    }
  }

  void sunshine_new_ops_t::map_upnp() {
    if (_commit_failed) {
      return;
    }

    // The guard must never exist with scope `all` before MAIN: a graceful exit in between would unmap shared TCP ports.
    _seams.set_unmap_scope(upnp::unmap_scope_e::udp_only);
    if (auto guard = _seams.start_upnp()) {
      _upnp_slot = std::move(guard);
    }
  }

  void sunshine_new_ops_t::start_tray() {
    if (_commit_failed) {
      return;
    }

    _seams.request_tray_start();
  }

  void sunshine_new_ops_t::become_active() {
    if (!_commit_failed) {
      runtime().instance_set_state(state_e::draining);
      runtime().instance_set_role(role_e::active);
    }
    if (_stage.load() >= new_stage_e::committing) {
      advance(_stage, new_stage_e::committed);
    }
  }

  void sunshine_new_ops_t::on_main() {
    // Called from the single worker of the post-COMMIT reader, so calls do not overlap.
    const auto current = _stage.load();
    if (current == new_stage_e::main) {
      return;
    }
    if (_commit_failed || current != new_stage_e::committed) {
      BOOST_LOG(warning) << "MAIN ignored: the commit effects have not completed"sv;
      return;
    }

    runtime().transition(mode_e::new_active, mode_e::normal);
    runtime().set_main(true);
    _seams.set_unmap_scope(upnp::unmap_scope_e::all);
    _seams.enable_tray_entries();
    _seams.notify_ready();
    _seams.start_control_server();
    advance(_stage, new_stage_e::main);
  }

  int sunshine_new_ops_t::take_old_pidfd() {
    return _old_pidfd.exchange(-1);
  }

  bool sunshine_new_ops_t::wait_probe_decision(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock {_mutex};
    while (!_hello_done) {
      if (runtime().shutdown_requested()) {
        return false;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return false;
      }
      _cv.wait_until(lock, std::min(deadline, now + 100ms));
    }
    return true;
  }

  validation_e sunshine_new_ops_t::wait_listeners_validated(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock {_mutex};
    while (!_validation) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return validation_e::timeout;
      }
      _cv.wait_until(lock, std::min(deadline, now + 100ms));
    }
    return *_validation;
  }

  void sunshine_new_ops_t::server_listening() {
    {
      std::lock_guard lock {_mutex};
      if (_listening < 4) {
        ++_listening;
      }
    }
    _cv.notify_all();
  }

  void sunshine_new_ops_t::servers_failed(const std::string &reason) {
    {
      std::lock_guard lock {_mutex};
      if (!_servers_failed) {
        _servers_failed = reason;
      }
      if (!_validation) {
        _validation = validation_e::failed;
      }
    }
    _cv.notify_all();
  }

  void sunshine_new_ops_t::mark_finished() {
    {
      std::lock_guard lock {_mutex};
      _finished = true;
      _hello_done = true;
      if (!_validation) {
        _validation = validation_e::failed;
      }
    }
    _cv.notify_all();
    // Not adopted (or already closed): close the inherited listeners here.
    close_inherited_listeners();
  }

  void sunshine_new_ops_t::abort() {
    {
      std::lock_guard lock {_mutex};
      if (_stage.load() >= new_stage_e::committing || _aborted) {
        return;
      }
      _aborted = true;
    }
    _cv.notify_all();

    if (_channel) {
      _seams.shutdown_channel(*_channel);
    }
  }

  void sunshine_new_ops_t::exit_failure() {
    if (_exited.exchange(true)) {
      return;
    }

    mark_finished();
    BOOST_LOG(info) << "The new process exits without taking over"sv;
    _seams.exit_process(1);
  }

  bool sunshine_new_ops_t::commit_failed() const {
    return _commit_failed;
  }

  new_stage_e sunshine_new_ops_t::stage() const {
    return _stage.load();
  }
}  // namespace handoff
