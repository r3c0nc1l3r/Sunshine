/**
 * @file src/handoff/revocation_poll.cpp
 * @brief Implementation of `src/handoff/revocation_poll.h` (wave C).
 */
// standard includes
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <iterator>
#include <map>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

// local includes
#include "revocation.h"
#include "revocation_poll.h"
#include "runtime.h"
#include "src/config.h"
#include "src/crypto.h"
#include "src/logging.h"
#include "src/rtsp.h"

namespace handoff {
  namespace {
    /// Longest state file that is read (4 MiB); a longer file counts as unreadable.
    constexpr std::size_t max_state_size = 4U * 1024U * 1024U;
    /// Smallest time between two Warnings of one failure episode.
    constexpr std::chrono::seconds warn_interval {60};
    /// Time to collect events that arrive close together into one pass.
    constexpr int debounce_ms = 10;

    /**
     * @brief Closes a file descriptor at scope exit.
     */
    struct fd_guard_t {
      int fd;  ///< The fd, or -1.

      /**
       * @brief Close the fd when valid.
       */
      ~fd_guard_t() {
        if (fd >= 0) {
          ::close(fd);
        }
      }
    };

    /**
     * @brief Read a whole file by path.
     *
     * @param path File path.
     * @return The content, or `std::nullopt` when it is missing, unreadable, empty or longer than 4 MiB.
     */
    std::optional<std::string> read_state_file(const std::filesystem::path &path) {
      const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
      if (fd < 0) {
        return std::nullopt;
      }
      fd_guard_t guard {fd};

      // Only a regular file is read: a FIFO, device or directory must never block the poller or hang stop().
      struct stat st {};
      if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        return std::nullopt;
      }

      std::string content;
      char buffer[8192];
      while (true) {
        const ssize_t got = ::read(fd, buffer, sizeof(buffer));
        if (got < 0) {
          if (errno == EINTR) {
            continue;
          }
          return std::nullopt;
        }
        if (got == 0) {
          break;
        }
        content.append(buffer, static_cast<std::size_t>(got));
        if (content.size() > max_state_size) {
          return std::nullopt;
        }
      }
      if (content.empty()) {
        return std::nullopt;
      }
      return content;
    }

    /**
     * @brief The production canonicalizer: the conversion `nvhttp` uses.
     *
     * @param pem Certificate PEM.
     * @return The canonical PEM, or an empty string when the certificate is invalid.
     */
    std::string canonical_pem(const std::string &pem) {
      auto certificate = crypto::x509(pem);
      return certificate ? crypto::pem(certificate) : std::string {};
    }
  }  // namespace

  revocation_poller_t::revocation_poller_t():
      _terminate {[](const std::string &pem) {
        rtsp_stream::terminate_sessions_by_cert(pem);
      }},
      _canonicalize {canonical_pem},
      _is_draining {[]() {
        return runtime().mode() == mode_e::draining;
      }},
      _now {[]() {
        return std::chrono::steady_clock::now();
      }} {}

  revocation_poller_t::~revocation_poller_t() {
    stop();
  }

#ifdef SUNSHINE_TESTS
  revocation_poller_t::revocation_poller_t(test_config_t config):
      _state_file {std::move(config.state_file)},
      _terminate {std::move(config.terminate)},
      _canonicalize {std::move(config.canonicalize)},
      _is_draining {std::move(config.is_draining)},
      _now {std::move(config.now)},
      _cap {config.cap},
      _use_config_path {false} {
    if (!_canonicalize) {
      _canonicalize = [](const std::string &pem) {
        return pem;
      };
    }
    if (!_is_draining) {
      _is_draining = []() {
        return true;
      };
    }
    if (!_now) {
      _now = []() {
        return std::chrono::steady_clock::now();
      };
    }
    if (!_terminate) {
      _terminate = [](const std::string &) {
      };
    }
  }

  std::uint64_t revocation_poller_t::passes_for_test() const {
    return _passes.load();
  }

  std::uint64_t revocation_poller_t::failed_passes_for_test() const {
    return _failed_passes.load();
  }
#endif

  bool revocation_poller_t::start() {
    std::lock_guard lock {_mutex};
    if (_thread.joinable()) {
      return true;
    }
    if (!_is_draining()) {
      return false;
    }
    if (_use_config_path) {
      _state_file = config::nvhttp.file_state;
    }
    if (_state_file.empty()) {
      return false;
    }

    _wake_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (_wake_fd < 0) {
      BOOST_LOG(warning) << "Drain handoff: revocation poller cannot create its wake fd: " << std::strerror(errno);
      _wake_fd = -1;
      return false;
    }

    _stop_requested = false;
    _handled.clear();
    _running = true;
    try {
      _thread = std::thread {[this]() {
        run();
      }};
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Drain handoff: cannot start the revocation poller: " << e.what();
      _running = false;
      ::close(_wake_fd);
      _wake_fd = -1;
      return false;
    }
    return true;
  }

  void revocation_poller_t::stop() {
    std::lock_guard lock {_mutex};
    if (!_thread.joinable()) {
      return;
    }
    _stop_requested = true;
    if (_wake_fd >= 0) {
      const std::uint64_t one = 1;
      if (::write(_wake_fd, &one, sizeof(one)) < 0) {
        BOOST_LOG(warning) << "Drain handoff: cannot wake the revocation poller: " << std::strerror(errno);
      }
    }
    _thread.join();
    if (_wake_fd >= 0) {
      ::close(_wake_fd);
      _wake_fd = -1;
    }
    _running = false;
  }

  bool revocation_poller_t::running() const {
    return _running.load();
  }

  void revocation_poller_t::run() {
    using steady_t = std::chrono::steady_clock;

    // Watch the directory, not the file, so a replacement by rename is seen.
    std::filesystem::path dir = _state_file.parent_path();
    if (dir.empty()) {
      dir = ".";
    }
    const std::string file_name = _state_file.filename().string();

    fd_guard_t inotify_guard {::inotify_init1(IN_CLOEXEC | IN_NONBLOCK)};
    const int inotify_fd = inotify_guard.fd;
    if (inotify_fd < 0) {
      BOOST_LOG(warning) << "Drain handoff: revocation poller cannot use inotify, using the timer alone: " << std::strerror(errno);
    } else if (::inotify_add_watch(inotify_fd, dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE | IN_MODIFY | IN_ONLYDIR) < 0) {
      BOOST_LOG(warning) << "Drain handoff: revocation poller cannot watch the state directory, using the timer alone: " << std::strerror(errno);
      inotify_guard.fd = -1;
      ::close(inotify_fd);
    }
    const int ino = inotify_guard.fd;
    bool overflow_logged = false;

    // Failure episode bookkeeping for the Warning rate limit.
    bool in_failure = false;
    std::optional<steady_t::time_point> last_warning;

    const auto fail = [&](const char *why) {
      const auto now = _now();
      if (!in_failure || !last_warning || now - *last_warning >= warn_interval) {
        BOOST_LOG(warning) << "Drain handoff: revocation poll cannot use the state file (" << why << "), retrying";
        last_warning = now;
      }
      in_failure = true;
      ++_failed_passes;
    };

    // Raw PEM -> time of the last logged callback Error, so a persistently throwing callback is not logged every pass.
    std::map<std::string, steady_t::time_point> error_logged;

    const auto pass = [&]() {
      auto content = read_state_file(_state_file);
      if (!content) {
        fail("missing, unreadable or empty");
        ++_passes;
        return;
      }
      auto disabled = disabled_client_certs(*content);
      if (!disabled) {
        fail("not parseable");
        ++_passes;
        return;
      }
      if (in_failure) {
        BOOST_LOG(info) << "Drain handoff: revocation poll can read the state file again";
        in_failure = false;
        last_warning.reset();
      }

      std::set<std::string> handled;
      for (const auto &pem : *disabled) {
        if (_stop_requested.load()) {
          break;  // Not processed: not in the handled set.
        }
        if (_handled.count(pem) != 0) {
          handled.insert(pem);
          continue;
        }
        try {
          const std::string canonical = _canonicalize(pem);
          if (canonical.empty()) {
            BOOST_LOG(warning) << "Drain handoff: a disabled client has an invalid certificate, skipped";
            handled.insert(pem);
            continue;
          }
          _terminate(canonical);
          handled.insert(pem);
          error_logged.erase(pem);
        } catch (...) {
          const auto now = _now();
          const auto it = error_logged.find(pem);
          if (it == error_logged.end() || now - it->second >= warn_interval) {
            error_logged[pem] = now;
            BOOST_LOG(error) << "Drain handoff: ending the sessions of a revoked client failed";
          }
        }
      }
      // Forget the Error timestamps of certificates that left the disabled set.
      for (auto it = error_logged.begin(); it != error_logged.end();) {
        it = disabled->count(it->first) != 0 ? std::next(it) : error_logged.erase(it);
      }
      _handled = std::move(handled);
      ++_passes;
    };

    // Drain the inotify queue; true when an event names the state file (or the queue overflowed).
    const auto drain_events = [&]() {
      bool relevant = false;
      alignas(struct inotify_event) char buffer[4096];
      while (true) {
        const ssize_t got = ::read(ino, buffer, sizeof(buffer));
        if (got <= 0) {
          break;  // EAGAIN, EINTR or error: nothing more now.
        }
        for (ssize_t offset = 0; offset < got;) {
          const auto *event = reinterpret_cast<const struct inotify_event *>(buffer + offset);
          if (event->mask & IN_Q_OVERFLOW) {
            if (!overflow_logged) {
              BOOST_LOG(warning) << "Drain handoff: revocation poller inotify queue overflowed";
              overflow_logged = true;
            }
            relevant = true;
          } else if (event->len > 0 && file_name == event->name) {
            relevant = true;
          }
          offset += static_cast<ssize_t>(sizeof(struct inotify_event) + event->len);
        }
      }
      return relevant;
    };

    const auto wait_fds = [&](int timeout_ms, bool &stop_seen, bool &ino_ready) {
      struct pollfd fds[2];
      fds[0] = {_wake_fd, POLLIN, 0};
      nfds_t count = 1;
      if (ino >= 0) {
        fds[1] = {ino, POLLIN, 0};
        count = 2;
      }
      const int rc = ::poll(fds, count, timeout_ms);
      stop_seen = false;
      ino_ready = false;
      if (rc > 0) {
        stop_seen = (fds[0].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0;
        ino_ready = count == 2 && (fds[1].revents & POLLIN) != 0;
      }
    };

    bool run_pass = true;  // A pass runs immediately at the start.
    auto last_wake = steady_t::now();
    while (!_stop_requested.load()) {
      if (run_pass) {
        if (_stop_requested.load()) {
          break;
        }
        if (_is_draining()) {
          pass();
        }
        run_pass = false;
        last_wake = steady_t::now();
      }

      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(last_wake + _cap - steady_t::now());
      const int timeout_ms = static_cast<int>(std::max<std::chrono::milliseconds::rep>(0, remaining.count()));
      bool stop_seen = false;
      bool ino_ready = false;
      wait_fds(timeout_ms, stop_seen, ino_ready);
      if (stop_seen || _stop_requested.load()) {
        break;
      }

      if (ino_ready) {
        if (drain_events()) {
          // Collect events that arrive close together into one pass.
          for (int i = 0; i < 5; ++i) {
            wait_fds(debounce_ms, stop_seen, ino_ready);
            if (stop_seen) {
              break;
            }
            if (!ino_ready) {
              break;
            }
            drain_events();
          }
          if (stop_seen || _stop_requested.load()) {
            break;
          }
          run_pass = true;
        }
      }
      if (!run_pass && steady_t::now() >= last_wake + _cap) {
        run_pass = true;  // The timer cap.
      }
    }
  }
}  // namespace handoff
