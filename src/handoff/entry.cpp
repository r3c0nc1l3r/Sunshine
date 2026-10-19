/**
 * @file src/handoff/entry.cpp
 * @brief Definitions for `entry.h`.
 */
// standard includes
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <variant>

// local includes
#include "control.h"
#include "coordinator.h"
#include "drain.h"
#include "entry.h"
#include "hooks.h"
#include "instance_file.h"
#include "mdns_holder.h"
#include "sd_notify.h"
#include "src/config.h"
#include "src/display_device.h"
#include "src/logging.h"

using namespace std::literals;

namespace handoff::entry {
  namespace {
    /**
     * @brief Read an environment variable through a reader that may be empty.
     *
     * @param getenv The reader.
     * @param name Variable name.
     * @return The value, or an empty string when unset.
     */
    std::string read_env(const std::function<const char *(const char *)> &getenv, const char *name) {
      if (!getenv) {
        return {};
      }
      const char *value = getenv(name);
      return value ? std::string {value} : std::string {};
    }

    /**
     * @brief Parse `SUNSHINE_DRAIN_HANDOFF_REPLY_TIMEOUT`: a positive decimal integer of seconds, 1 to 86400.
     *
     * @param text The value.
     * @return The seconds, or `std::nullopt` when invalid.
     */
    std::optional<int> parse_reply_timeout(const std::string &text) {
      int seconds = 0;
      const auto *begin = text.data();
      const auto *end = text.data() + text.size();
      const auto [ptr, ec] = std::from_chars(begin, end, seconds, 10);
      if (text.empty() || ec != std::errc {} || ptr != end || seconds < 1 || seconds > 86400) {
        return std::nullopt;
      }
      return seconds;
    }

    /**
     * @brief The runtime directory of the environment.
     *
     * @return The directory, or `std::nullopt` when neither variable gives one.
     */
    std::optional<std::filesystem::path> system_runtime_dir() {
      const char *runtime_directory = std::getenv("RUNTIME_DIRECTORY");
      const char *xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR");
      return runtime_dir(runtime_directory ? runtime_directory : "", xdg_runtime_dir ? xdg_runtime_dir : "");
    }
  }  // namespace

  int run_drain_handoff_cli(const std::vector<std::string> &args, const cli_env_t &env) {
    const auto write_err = [&env](const std::string &line) {
      if (env.write_err) {
        env.write_err(line);
      } else {
        std::cerr << line << '\n';
      }
    };

    helper_env_t helper_env;
    const auto runtime_directory = read_env(env.getenv, "RUNTIME_DIRECTORY");
    const auto xdg_runtime_dir = read_env(env.getenv, "XDG_RUNTIME_DIR");
    if (const auto dir = runtime_dir(runtime_directory, xdg_runtime_dir)) {
      helper_env.runtime_dir = *dir;
    }

    if (env.getenv && env.getenv("SUNSHINE_DRAIN_HANDOFF_REPLY_TIMEOUT") != nullptr) {
      if (const auto seconds = parse_reply_timeout(read_env(env.getenv, "SUNSHINE_DRAIN_HANDOFF_REPLY_TIMEOUT"))) {
        helper_env.reply_timeout = std::chrono::seconds {*seconds};
      } else {
        write_err("ignoring invalid SUNSHINE_DRAIN_HANDOFF_REPLY_TIMEOUT");
      }
    }

    if (helper_env.runtime_dir.empty()) {
      write_err("no runtime directory (RUNTIME_DIRECTORY and XDG_RUNTIME_DIR are not set)");
      return static_cast<int>(helper_exit_e::error);
    }

    std::string err;
    helper_exit_e code {};
    if (env.run) {
      code = env.run(args, helper_env, err);
    } else {
      code = run_helper(args, helper_env, err);
    }
    if (!err.empty()) {
      write_err(err);
    }
    return static_cast<int>(code);
  }

  std::optional<int> maybe_run_helper(int argc, char **argv) {
    if (argc < 2 || argv[1] == nullptr || std::strcmp(argv[1], "--drain-handoff") != 0) {
      return std::nullopt;
    }

    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) {
      args.emplace_back(argv[i]);
    }

    cli_env_t env;
    env.getenv = [](const char *name) {
      return std::getenv(name);
    };
    return run_drain_handoff_cli(args, env);
  }

  std::string early_start(bool privileged_execution, const early_init_env_t &env) {
    if (auto reason = runtime().early_init(env); !reason.empty()) {
      return reason;
    }
    if (runtime().started_by_handoff() && privileged_execution) {
      return "drain handoff start is refused with privileged execution";
    }
    return {};
  }

  std::string apply_startup_policy(bool privileged_execution) {
    runtime().configure(config::drain_handoff);

    enable_context_t context;
    context.capture = config::video.capture;
    context.fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
    context.display_settings_manager = display_device::has_settings_manager();
    context.runtime_dir_available = system_runtime_dir().has_value();

    auto refusal = hooks::enable_refusal(context, privileged_execution);
    if (!refusal.empty()) {
      BOOST_LOG(error) << refusal;
      config::drain_handoff.enabled = false;
      runtime().disable();
    }
    return refusal;
  }

  bool open_instance_file() {
    if (!runtime().enabled()) {
      return false;
    }

    const auto dir = system_runtime_dir();
    if (!dir) {
      BOOST_LOG(error) << "No runtime directory: the instance file is not written"sv;
      return false;
    }

    instance_t instance;
    instance.pid = getpid();
    instance.start_time = read_start_time(instance.pid).value_or(0);
    instance.role = runtime().started_by_handoff() ? role_e::pending : role_e::active;
    instance.udp_base = runtime().effective_udp_base(config::sunshine.port);
    instance.sessions = 0;
    instance.handoff = state_e::none;
    if (!runtime().open_instance(*dir / "instances", instance)) {
      BOOST_LOG(error) << "The instance file could not be written"sv;
      return false;
    }
    return true;
  }

  bool notify_ready_if_systemd() {
    const char *notify_socket = std::getenv("NOTIFY_SOCKET");
    if (notify_socket == nullptr || *notify_socket == '\0') {
      return false;
    }
    return sd::notify(notify_socket, "READY=1"sv);
  }

  listening_latch_t::listening_latch_t(int count, std::function<void()> on_complete):
      _on_complete {std::move(on_complete)},
      _remaining {count > 0 ? count : 0},
      _count {count} {
  }

  void listening_latch_t::arrive() {
    int remaining = _remaining.load();
    while (remaining > 0 && !_remaining.compare_exchange_weak(remaining, remaining - 1)) {
    }
    if (remaining <= 0) {
      return;
    }

    _arrived.fetch_add(1);
    if (remaining == 1 && _on_complete) {
      _on_complete();
    }
  }

  int listening_latch_t::arrived() const {
    return std::min(_arrived.load(), _count);
  }

  post_commit_t::post_commit_t(sunshine_new_ops_t &ops, std::shared_ptr<channel_t> channel):
      _set_peer {[](std::shared_ptr<peer_link_t> peer) {
        runtime().set_peer(std::move(peer));
      }},
      _ops {ops},
      _channel {std::move(channel)} {
    start(_ops.take_old_pidfd());
  }

#ifdef SUNSHINE_TESTS
  post_commit_t::post_commit_t(sunshine_new_ops_t &ops, std::shared_ptr<channel_t> channel, const post_commit_seams_t &seams):
      _set_peer {seams.set_peer},
      _ops {ops},
      _channel {std::move(channel)} {
    if (!_set_peer) {
      _set_peer = [](std::shared_ptr<peer_link_t> peer) {
        runtime().set_peer(std::move(peer));
      };
    }
    start(seams.old_pidfd ? seams.old_pidfd() : _ops.take_old_pidfd());
  }
#endif

  post_commit_t::~post_commit_t() {
    stop();
  }

  void post_commit_t::start(int old_pidfd) {
    _link = std::make_shared<peer_link_t>(_channel, old_pidfd);
    if (old_pidfd >= 0) {
      _watch_pidfd = ::fcntl(old_pidfd, F_DUPFD_CLOEXEC, 0);
    }
    _stop_fd = ::eventfd(0, EFD_CLOEXEC);
    _set_peer(_link);

    _reader = std::make_unique<channel_reader_t>(
      *_channel,
      true,
      [this](received_t received) {
        if (std::holds_alternative<protocol::main_t>(received.message)) {
          _ops.on_main();
        } else {
          _link->deliver(received.message);
        }
        if (received.fd >= 0) {
          ::close(received.fd);
        }
      },
      [this](recv_status_e) {
        _link->on_channel_closed();
        BOOST_LOG(warning) << "The handoff channel to the other process closed"sv;
      }
    );
    _reader->start();

    if (_watch_pidfd >= 0 && _stop_fd >= 0) {
      _watcher = std::thread([link = _link.get(), pidfd = _watch_pidfd, stop_fd = _stop_fd]() {
        pollfd fds[2] = {{pidfd, POLLIN, 0}, {stop_fd, POLLIN, 0}};
        while (true) {
          const int ready = ::poll(fds, 2, -1);
          if (ready < 0) {
            if (errno == EINTR) {
              continue;
            }
            return;
          }
          if (fds[1].revents != 0) {
            return;
          }
          if (fds[0].revents != 0) {
            link->on_channel_closed();
            return;
          }
        }
      });
    }
  }

  void post_commit_t::stop() {
    if (_reader) {
      _reader->stop();
      _reader.reset();
    }

    if (_watcher.joinable()) {
      if (_stop_fd >= 0) {
        const std::uint64_t one = 1;
        [[maybe_unused]] const auto written = ::write(_stop_fd, &one, sizeof(one));
      }
      _watcher.join();
    }
    if (_stop_fd >= 0) {
      ::close(_stop_fd);
      _stop_fd = -1;
    }
    if (_watch_pidfd >= 0) {
      ::close(_watch_pidfd);
      _watch_pidfd = -1;
    }

    if (_link) {
      _set_peer(nullptr);
      _link.reset();
    }
  }

  peer_link_t &post_commit_t::link() {
    return *_link;
  }

  handoff_process_t::handoff_process_t(std::function<void()> wait_shutdown, std::function<void()> raise_shutdown):
      _wait_shutdown {std::move(wait_shutdown)},
      _raise_shutdown {std::move(raise_shutdown)} {
    start_watcher();
  }

#ifdef SUNSHINE_TESTS
  handoff_process_t::handoff_process_t(std::function<void()> wait_shutdown, std::function<void()> raise_shutdown, const process_seams_t &seams):
      _wait_shutdown {std::move(wait_shutdown)},
      _raise_shutdown {std::move(raise_shutdown)},
      _seams {seams} {
    start_watcher();
  }
#endif

  handoff_process_t::~handoff_process_t() {
    finish_new_side();
    _stopping = true;

    if (_post_commit) {
      _post_commit->stop();
    }
    runtime().set_channel(nullptr);
    session_notifier().stop();
    drain_controller().cancel();
    mdns_holder().stop();
    runtime().instance_remove();

    if (_raise_shutdown) {
      _raise_shutdown();
    }
    if (_watcher.joinable()) {
      _watcher.join();
    }
    if (_stop_fd >= 0) {
      ::close(_stop_fd);
    }
  }

  void handoff_process_t::start_watcher() {
    _watcher = std::thread([this]() {
      if (_wait_shutdown) {
        _wait_shutdown();
      }
      if (_stopping.load()) {
        return;
      }

      runtime().request_shutdown();
      sunshine_new_ops_t *ops = nullptr;
      {
        std::lock_guard lock {_mutex};
        ops = _ops.get();
      }
      if (ops) {
        ops->abort();
      }
    });
  }

  bool handoff_process_t::start_new_side(const new_ops_config_t &config, std::unique_ptr<platf::deinit_t> &upnp_slot, const new_side_timeouts_t &timeouts) {
    if (!runtime().started_by_handoff()) {
      return false;
    }
    const auto inherited = runtime().inherited();
    if (!inherited || inherited->channel < 0) {
      return false;
    }
    {
      std::lock_guard lock {_mutex};
      if (_ops) {
        return false;
      }
    }
    return launch_new_side(config, std::make_shared<channel_t>(inherited->channel), upnp_slot, timeouts, nullptr);
  }

#ifdef SUNSHINE_TESTS
  bool handoff_process_t::start_new_side_for_test(const new_ops_config_t &config, std::shared_ptr<channel_t> channel, std::unique_ptr<platf::deinit_t> &upnp_slot, const new_side_timeouts_t &timeouts, const new_ops_seams_t &seams) {
    return launch_new_side(config, std::move(channel), upnp_slot, timeouts, &seams);
  }
#endif

  bool handoff_process_t::launch_new_side(const new_ops_config_t &config, std::shared_ptr<channel_t> channel, std::unique_ptr<platf::deinit_t> &upnp_slot, const new_side_timeouts_t &timeouts, const new_ops_seams_t *seams) {
    std::lock_guard lock {_mutex};
    if (_ops || !channel) {
      return false;
    }

    _channel = channel;
#ifdef SUNSHINE_TESTS
    if (seams) {
      _ops = std::make_unique<sunshine_new_ops_t>(config, channel, upnp_slot, *seams);
    } else
#endif
    {
      (void) seams;
      _ops = std::make_unique<sunshine_new_ops_t>(config, channel, upnp_slot);
    }

    sunshine_new_ops_t *ops = _ops.get();
    if (runtime().shutdown_requested()) {
      // The watcher ran before the ops existed.
      ops->abort();
    }

    auto run = _seams.run_new_side ? _seams.run_new_side : std::function<new_outcome_e(new_side_ops_t &, channel_t &, const new_side_timeouts_t &)> {handoff::run_new_side};
    auto make_post_commit = _seams.make_post_commit ? _seams.make_post_commit : std::function<std::unique_ptr<post_commit_t>(sunshine_new_ops_t &, std::shared_ptr<channel_t>)> {[](sunshine_new_ops_t &ops_ref, std::shared_ptr<channel_t> channel_ref) {
      return std::make_unique<post_commit_t>(ops_ref, std::move(channel_ref));
    }};

    // The new-side thread is the only thread that runs `run_new_side` or any op of the new side. It writes
    // `_post_commit` and nothing else of this object; `finish_new_side()` joins it before anyone reads that member.
    _new_side = std::thread([this, ops, channel, timeouts, run = std::move(run), make_post_commit = std::move(make_post_commit)]() {
      const auto outcome = run(*ops, *channel, timeouts);
      ops->mark_finished();
      if (outcome == new_outcome_e::exit_failure) {
        ops->exit_failure();
        return;
      }

      _post_commit = make_post_commit(*ops, channel);
      if (ops->commit_failed()) {
        ops->exit_failure();
      }
    });
    return true;
  }

  sunshine_new_ops_t *handoff_process_t::new_ops() {
    std::lock_guard lock {_mutex};
    return _ops.get();
  }

  void handoff_process_t::finish_new_side() {
    std::thread thread;
    {
      std::lock_guard lock {_mutex};
      if (_ops) {
        _ops->abort();
      }
      thread = std::move(_new_side);
    }
    if (thread.joinable()) {
      thread.join();
    }
  }
}  // namespace handoff::entry
