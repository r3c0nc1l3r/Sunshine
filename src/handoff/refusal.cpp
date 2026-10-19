/**
 * @file src/handoff/refusal.cpp
 * @brief Implementation of `src/handoff/refusal.h` (wave C).
 */

// standard includes
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// local includes
#include "instance_file.h"
#include "refusal.h"
#include "runtime.h"
#include "sd_notify.h"
#include "src/logging.h"
#include "src/nvhttp.h"
#include "src/process.h"
#include "src/rtsp.h"

namespace handoff {
  namespace {
    /// Longest wait for `systemctl --version` (the whole run: spawn, output and exit).
    constexpr std::chrono::milliseconds systemctl_timeout {2000};

    /// Largest output accepted from `systemctl --version`; a longer output is not systemctl's answer.
    constexpr std::size_t systemctl_output_cap {4096};

    /// Largest number of bytes of `app_reason` that is shown in the refusal.
    constexpr std::size_t app_reason_cap {200};

    /// Guards `systemd_cache`.
    std::mutex systemd_cache_mutex;

    /// First successful parse of the systemd version (empty until one succeeds).
    std::optional<int> systemd_cache;

    /**
     * @brief Owns a file descriptor and closes it on destruction. Not copyable.
     */
    class fd_guard {
    public:
      /**
       * @brief Take ownership of a descriptor.
       *
       * @param fd Descriptor, or -1 for none.
       */
      explicit fd_guard(int fd = -1):
          _fd(fd) {}

      fd_guard(const fd_guard &) = delete;
      fd_guard &operator=(const fd_guard &) = delete;

      /**
       * @brief Close the descriptor, if any.
       */
      ~fd_guard() {
        reset();
      }

      /**
       * @brief The descriptor (not released).
       *
       * @return The descriptor, or -1.
       */
      int get() const {
        return _fd;
      }

      /**
       * @brief Close the descriptor now, if any.
       */
      void reset() {
        if (_fd >= 0) {
          ::close(_fd);
          _fd = -1;
        }
      }

    private:
      int _fd;  ///< Owned descriptor, or -1.
    };

    /**
     * @brief Owns a `posix_spawn_file_actions_t`. Not copyable.
     */
    class spawn_actions_t {
    public:
      /**
       * @brief Initialise the actions object.
       */
      spawn_actions_t() {
        _ok = posix_spawn_file_actions_init(&_actions) == 0;
      }

      spawn_actions_t(const spawn_actions_t &) = delete;
      spawn_actions_t &operator=(const spawn_actions_t &) = delete;

      /**
       * @brief Destroy the actions object when it was initialised.
       */
      ~spawn_actions_t() {
        if (_ok) {
          posix_spawn_file_actions_destroy(&_actions);
        }
      }

      /**
       * @brief Whether the object was initialised.
       *
       * @return `true` when usable.
       */
      bool ok() const {
        return _ok;
      }

      /**
       * @brief The underlying object.
       *
       * @return Pointer to the actions.
       */
      posix_spawn_file_actions_t *get() {
        return &_actions;
      }

    private:
      posix_spawn_file_actions_t _actions {};  ///< The actions.
      bool _ok {false};  ///< Initialisation succeeded.
    };

    /**
     * @brief Owns a `posix_spawnattr_t`. Not copyable.
     */
    class spawn_attr_t {
    public:
      /**
       * @brief Initialise the attributes object.
       */
      spawn_attr_t() {
        _ok = posix_spawnattr_init(&_attr) == 0;
      }

      spawn_attr_t(const spawn_attr_t &) = delete;
      spawn_attr_t &operator=(const spawn_attr_t &) = delete;

      /**
       * @brief Destroy the attributes object when it was initialised.
       */
      ~spawn_attr_t() {
        if (_ok) {
          posix_spawnattr_destroy(&_attr);
        }
      }

      /**
       * @brief Whether the object was initialised.
       *
       * @return `true` when usable.
       */
      bool ok() const {
        return _ok;
      }

      /**
       * @brief The underlying object.
       *
       * @return Pointer to the attributes.
       */
      posix_spawnattr_t *get() {
        return &_attr;
      }

    private:
      posix_spawnattr_t _attr {};  ///< The attributes.
      bool _ok {false};  ///< Initialisation succeeded.
    };

    /**
     * @brief Kill a child through its pidfd and reap it through the same pidfd. Used only on the failure paths.
     *
     * @param pidfd Pidfd of the child.
     */
    void kill_and_reap(int pidfd) {
      syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0);

      pollfd pfd {};
      pfd.fd = pidfd;
      pfd.events = POLLIN;
      poll(&pfd, 1, 1000);

      siginfo_t info {};
      waitid(P_PIDFD, static_cast<id_t>(pidfd), &info, WEXITED | WNOHANG);
    }

    /**
     * @brief Run `/usr/bin/systemctl --version` and return its stdout.
     *
     * The rules are those of `sd::read_back_mainpid`: an absolute path with no shell and no `PATH` search, stdout in a
     * pipe, stdin from `/dev/null`, fds 3 and up closed in the child, the default signal mask and dispositions, the
     * child tracked by pidfd only (the control loop reaps with `waitpid(-1)`), output capped at 4096 bytes, and a 2 s
     * timeout for the whole run.
     *
     * @return The output, or `std::nullopt` on a spawn failure, a timeout, an oversized output or a non-zero exit.
     */
    std::optional<std::string> run_systemctl_version() {
      try {
        // Pipe for stdout. Both ends are close-on-exec; the child gets its own copy through dup2, which clears the flag.
        int pipe_fds[2] {-1, -1};
        if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
          BOOST_LOG(error) << "Drain handoff: pipe for systemctl failed: " << std::strerror(errno);
          return std::nullopt;
        }
        fd_guard read_fd {pipe_fds[0]};
        fd_guard write_fd {pipe_fds[1]};

        // File actions, in order: stdout to the pipe, stdin from /dev/null, then close every fd from 3 up.
        spawn_actions_t actions;
        if (!actions.ok()) {
          BOOST_LOG(error) << "Drain handoff: could not set up the systemctl spawn";
          return std::nullopt;
        }
        posix_spawn_file_actions_adddup2(actions.get(), write_fd.get(), STDOUT_FILENO);
        posix_spawn_file_actions_addopen(actions.get(), STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addclosefrom_np(actions.get(), 3);

        // Empty signal mask and default dispositions for every signal except the two that cannot be caught.
        spawn_attr_t attr;
        if (!attr.ok()) {
          BOOST_LOG(error) << "Drain handoff: could not set up the systemctl spawn";
          return std::nullopt;
        }
        sigset_t empty_mask;
        sigset_t all_signals;
        sigemptyset(&empty_mask);
        sigfillset(&all_signals);
        sigdelset(&all_signals, SIGKILL);
        sigdelset(&all_signals, SIGSTOP);
        posix_spawnattr_setsigmask(attr.get(), &empty_mask);
        posix_spawnattr_setsigdefault(attr.get(), &all_signals);
        posix_spawnattr_setflags(attr.get(), POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

        // The environment is the parent's.
        std::vector<std::string> args {"systemctl", "--version"};
        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (auto &arg : args) {
          argv.push_back(arg.data());
        }
        argv.push_back(nullptr);

        std::vector<char *> envp;
        for (char **entry = environ; entry && *entry; ++entry) {
          envp.push_back(*entry);
        }
        envp.push_back(nullptr);

        // Spawn with a pidfd: the child is tracked by it only, never by pid.
        int raw_pidfd = -1;
        const int spawn_rc = pidfd_spawn(&raw_pidfd, "/usr/bin/systemctl", actions.get(), attr.get(), argv.data(), envp.data());
        write_fd.reset();
        const fd_guard pidfd {spawn_rc == 0 ? raw_pidfd : -1};
        if (spawn_rc != 0) {
          BOOST_LOG(error) << "Drain handoff: could not start systemctl: " << std::strerror(spawn_rc);
          return std::nullopt;
        }

        const auto deadline = std::chrono::steady_clock::now() + systemctl_timeout;
        const auto remaining_ms = [&deadline]() {
          return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        };

        // Read stdout until end-of-file, within the deadline.
        std::string output;
        bool eof = false;
        char buf[512];
        while (!eof) {
          const auto left = remaining_ms();
          if (left <= 0) {
            break;
          }

          pollfd pfd {};
          pfd.fd = read_fd.get();
          pfd.events = POLLIN;
          const int rc = poll(&pfd, 1, static_cast<int>(std::min<long long>(left, INT_MAX)));
          if (rc < 0) {
            if (errno == EINTR) {
              continue;
            }
            break;
          }
          if (rc == 0) {
            continue;
          }

          const ssize_t n = read(read_fd.get(), buf, sizeof(buf));
          if (n == 0) {
            eof = true;
          } else if (n > 0) {
            output.append(buf, static_cast<std::size_t>(n));
            if (output.size() > systemctl_output_cap) {
              break;
            }
          } else if (errno != EINTR && errno != EAGAIN) {
            break;
          }
        }

        if (!eof) {
          BOOST_LOG(error) << "Drain handoff: reading the systemd version with systemctl timed out or failed";
          kill_and_reap(pidfd.get());
          return std::nullopt;
        }

        // The child closed stdout; wait for its pidfd to become readable within the same deadline.
        while (true) {
          const auto left = remaining_ms();
          if (left <= 0) {
            BOOST_LOG(error) << "Drain handoff: systemctl did not exit in time";
            kill_and_reap(pidfd.get());
            return std::nullopt;
          }

          pollfd pfd {};
          pfd.fd = pidfd.get();
          pfd.events = POLLIN;
          const int rc = poll(&pfd, 1, static_cast<int>(std::min<long long>(left, INT_MAX)));
          if (rc < 0) {
            if (errno == EINTR) {
              continue;
            }
            kill_and_reap(pidfd.get());
            return std::nullopt;
          }
          if (rc > 0) {
            break;
          }
        }

        // Reap through the pidfd. ECHILD means another waiter reaped it after its output was read; the exit status is
        // then unknown, so the output is used.
        siginfo_t info {};
        while (waitid(P_PIDFD, static_cast<id_t>(pidfd.get()), &info, WEXITED) < 0) {
          if (errno == EINTR) {
            continue;
          }
          if (errno == ECHILD) {
            return output;
          }
          BOOST_LOG(error) << "Drain handoff: waiting for systemctl failed: " << std::strerror(errno);
          return std::nullopt;
        }

        if (info.si_code != CLD_EXITED || info.si_status != 0) {
          BOOST_LOG(error) << "Drain handoff: systemctl --version failed";
          return std::nullopt;
        }
        return output;
      } catch (...) {
        return std::nullopt;
      }
    }

    /**
     * @brief Content of `/proc/self/cgroup`.
     *
     * @return The content, or `std::nullopt` when it cannot be read.
     */
    std::optional<std::string> read_own_cgroup() {
      std::ifstream in("/proc/self/cgroup");
      if (!in) {
        return std::nullopt;
      }
      return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    /**
     * @brief The runtime directory of this process, from `RUNTIME_DIRECTORY` or `XDG_RUNTIME_DIR`.
     *
     * @return The directory, or `std::nullopt` when neither is usable.
     */
    std::optional<std::filesystem::path> runtime_directory_path() {
      const char *runtime_directory = std::getenv("RUNTIME_DIRECTORY");
      const char *xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR");
      return runtime_dir(runtime_directory ? runtime_directory : "", xdg_runtime_dir ? xdg_runtime_dir : "");
    }

    /**
     * @brief Scan the instance files of the runtime directory. A missing runtime directory is `dir_missing`.
     *
     * @return The scan.
     */
    scan_result_t scan_runtime_instances() {
      const auto dir = runtime_directory_path();
      if (!dir) {
        scan_result_t missing;
        missing.dir_missing = true;
        return missing;
      }
      return scan_instances(*dir / "instances", read_start_time);
    }

    /**
     * @brief Turn an app reason into one line of at most `app_reason_cap` bytes.
     *
     * @param reason The `why` text of `adoptable()`.
     * @return The sanitised text; empty when nothing is left.
     */
    std::string sanitise_app_reason(std::string reason) {
      std::replace(reason.begin(), reason.end(), '\r', ' ');
      std::replace(reason.begin(), reason.end(), '\n', ' ');
      if (reason.size() > app_reason_cap) {
        // Back up while the first cut-off byte is a UTF-8 continuation byte, so no multi-byte sequence is split.
        std::size_t cut = app_reason_cap;
        while (cut > 0 && (static_cast<unsigned char>(reason[cut]) & 0xC0) == 0x80) {
          --cut;
        }
        reason.resize(cut);
      }
      return reason;
    }
  }  // namespace

  std::string check_refusal(const refusal_inputs_t &inputs) {
    // Order is the precedence of the header: states that block a retry, then permanent properties, then transient ones.
    if (inputs.kill_pending) {
      return std::string(refusal_reason::kill_pending);
    }
    if (inputs.in_progress) {
      return std::string(refusal_reason::in_progress);
    }
    if (inputs.drain_instance_exists) {
      return std::string(refusal_reason::drain_exists);
    }
    if (inputs.cooldown_active) {
      return std::string(refusal_reason::cooldown);
    }
    if (!inputs.notify_socket_set) {
      return std::string(refusal_reason::no_notify_socket);
    }
    if (!inputs.systemd_version) {
      return std::string(refusal_reason::systemd_unknown);
    }
    if (*inputs.systemd_version < minimum_systemd_version) {
      return std::string(refusal_reason::systemd_old);
    }
    if (!inputs.unit_found) {
      return std::string(refusal_reason::no_unit);
    }
    if (!inputs.app_adoptable) {
      const std::string reason = sanitise_app_reason(inputs.app_reason);
      if (reason.empty()) {
        return std::string(refusal_reason::app_generic);
      }
      return std::string(refusal_reason::app_prefix) + reason;
    }
    if (inputs.pairing_count > 0) {
      return std::string(refusal_reason::pairing);
    }
    if (inputs.pending_launch) {
      return std::string(refusal_reason::pending_launch);
    }
    return {};
  }

  void log_refusal(std::string_view reason, pid_t peer_pid) {
    if (peer_pid > 0) {
      BOOST_LOG(warning) << "Drain handoff refused: " << reason << " (peer pid " << peer_pid << ")";
    } else {
      BOOST_LOG(warning) << "Drain handoff refused: " << reason << " (peer pid unknown)";
    }
  }

  instance_view_t classify_instances(const scan_result_t &scan, pid_t self_pid) {
    instance_view_t view;

    // An unreadable or incomplete listing fails closed: a drain may be hidden in the files that could not be read.
    if (scan.any_unreadable) {
      view.drain_instance_exists = true;
    }

    for (const auto &instance : scan.live) {
      // A rollback waiting for the new process is visible whoever wrote it, this process included.
      if (instance.handoff == state_e::kill_pending) {
        view.kill_pending = true;
      }

      // The other facts describe other processes only.
      if (instance.pid == self_pid) {
        continue;
      }
      if (instance.role == role_e::pending) {
        view.handoff_pending = true;
      } else if (instance.role == role_e::draining) {
        view.drain_instance_exists = true;
      }
    }
    return view;
  }

  bool cooldown_active(std::chrono::steady_clock::time_point cooldown_until, std::chrono::steady_clock::time_point now) {
    // `time_point::min()` means no cooldown; it must not be compared, since it is before every `now`.
    if (cooldown_until == std::chrono::steady_clock::time_point::min()) {
      return false;
    }
    return now < cooldown_until;
  }

  refusal_sources_t production_refusal_sources() {
    refusal_sources_t sources;
    sources.notify_socket = []() {
      const char *value = std::getenv("NOTIFY_SOCKET");
      return value ? std::string(value) : std::string();
    };
    sources.systemd_version = []() {
      return gather_systemd_version();
    };
    sources.cgroup = []() {
      return read_own_cgroup();
    };
    sources.handoff_in_progress = []() {
      return runtime().handoff_in_progress();
    };
    sources.peer_exists = []() {
      const auto peer = runtime().peer();
      return peer != nullptr && peer->exists();
    };
    sources.scan = []() {
      return scan_runtime_instances();
    };
    sources.app_adoptable = [](std::string &why) {
      return proc::proc.adoptable(&why);
    };
    sources.pairing_count = []() {
      return nvhttp::pairing_session_count();
    };
    sources.pending_launch = []() {
      return rtsp_stream::has_pending_launch();
    };
    sources.self_pid = []() {
      return getpid();
    };
    sources.now = []() {
      return std::chrono::steady_clock::now();
    };
    return sources;
  }

  std::optional<int> gather_systemd_version(const std::function<std::optional<std::string>()> &runner) {
    // An injected runner never touches the cache, so tests do not depend on order or repetition.
    if (runner) {
      const auto output = runner();
      return output ? sd::parse_systemd_version(*output) : std::nullopt;
    }

    // Concurrent callers wait here; the command runs at most once per success.
    const std::lock_guard lock(systemd_cache_mutex);
    if (systemd_cache) {
      return systemd_cache;
    }

    const auto output = run_systemctl_version();
    if (!output) {
      return std::nullopt;
    }
    const auto version = sd::parse_systemd_version(*output);
    if (version) {
      systemd_cache = version;
    }
    return version;
  }

  refusal_inputs_t gather_refusal_inputs(
    const refusal_sources_t &sources,
    std::chrono::steady_clock::time_point cooldown_until
  ) {
    refusal_inputs_t inputs;

    inputs.notify_socket_set = !sources.notify_socket().empty();
    inputs.systemd_version = sources.systemd_version();

    const auto cgroup = sources.cgroup();
    inputs.unit_found = cgroup && sd::unit_from_cgroup(*cgroup).has_value();

    // One scan serves both the handoff and the drain facts, and the kill-pending state.
    const auto view = classify_instances(sources.scan(), sources.self_pid());
    inputs.in_progress = sources.handoff_in_progress() || view.handoff_pending;
    inputs.drain_instance_exists = sources.peer_exists() || view.drain_instance_exists;
    inputs.kill_pending = view.kill_pending;
    inputs.cooldown_active = cooldown_active(cooldown_until, sources.now());

    std::string why;
    inputs.app_adoptable = sources.app_adoptable(why);
    if (!inputs.app_adoptable) {
      inputs.app_reason = why;
    }

    inputs.pairing_count = sources.pairing_count();
    inputs.pending_launch = sources.pending_launch();
    return inputs;
  }
}  // namespace handoff
