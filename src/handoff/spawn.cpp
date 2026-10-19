/**
 * @file src/handoff/spawn.cpp
 * @brief Definitions for `src/handoff/spawn.h`.
 */
// standard includes
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <system_error>
#include <thread>

// platform includes
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

// local includes
#include "spawn.h"

extern char **environ;  ///< The parent environment, copied for the child.

#ifndef P_PIDFD
  #define P_PIDFD 3  ///< Value of `P_PIDFD` (waitid id type of a pidfd) when the headers do not define it.
#endif

namespace handoff {
  namespace {
    constexpr int temp_fd_floor = 100;  ///< Parent-side temporary duplicates sit at this fd or above.
    constexpr std::string_view env_prefix = "SUNSHINE_HANDOFF_";  ///< Prefix of every handoff variable.

    /**
     * @brief Text for an errno value.
     *
     * @param err The errno value.
     * @return Message text.
     */
    std::string errno_text(int err) {
      return std::generic_category().message(err);
    }

    /**
     * @brief Set a one-line error if the caller wants one.
     *
     * @param error Target, may be null.
     * @param text Text to store.
     */
    void set_error(std::string *error, std::string text) {
      if (error) {
        *error = std::move(text);
      }
    }

    /**
     * @brief Parse a decimal fd of 3 or more.
     *
     * @param text Text to parse; digits only.
     * @return The fd, or -1 when invalid.
     */
    int parse_fd(std::string_view text) {
      if (text.empty() || text.size() > 9) {
        return -1;
      }
      int value = 0;
      auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (ec != std::errc {} || end != text.data() + text.size() || value < first_inherited_fd) {
        return -1;
      }
      return value;
    }

    /**
     * @brief Close fds, ignoring errors, and reset them to -1.
     *
     * @param fds The fds to close.
     */
    void close_all(std::array<int, 5> &fds) {
      for (int &fd : fds) {
        if (fd >= 0) {
          ::close(fd);
          fd = -1;
        }
      }
    }

    /**
     * @brief Read the pid of a pidfd from `/proc/self/fdinfo`.
     *
     * @param pidfd The pidfd.
     * @return The pid, or -1.
     */
    pid_t pid_of_pidfd(int pidfd) {
      std::ifstream in("/proc/self/fdinfo/" + std::to_string(pidfd));
      std::string line;
      while (std::getline(in, line)) {
        if (line.rfind("Pid:", 0) == 0) {
          try {
            return static_cast<pid_t>(std::stol(line.substr(4)));
          } catch (...) {
            return -1;
          }
        }
      }
      return -1;
    }
  }  // namespace

  std::vector<std::string> child_environment(int udp_base, unsigned generation) {
    return {
      "SUNSHINE_HANDOFF_FDS=https:3,http:4,webui:5,rtsp:6",
      "SUNSHINE_HANDOFF_CHANNEL=7",
      "SUNSHINE_HANDOFF_UDP_BASE=" + std::to_string(udp_base),
      "SUNSHINE_HANDOFF_GENERATION=" + std::to_string(generation),
    };
  }

  std::optional<inherited_fds_t> parse_inherited_fds(std::string_view fds, std::string_view channel, std::string *error) {
    inherited_fds_t out;
    std::array<bool, 4> seen {};

    size_t pos = 0;
    while (pos <= fds.size()) {
      size_t comma = fds.find(',', pos);
      if (comma == std::string_view::npos) {
        comma = fds.size();
      }
      std::string_view item = fds.substr(pos, comma - pos);
      pos = comma + 1;

      size_t colon = item.find(':');
      if (colon == std::string_view::npos) {
        set_error(error, "malformed fd entry '" + std::string(item) + "'");
        return std::nullopt;
      }
      std::string_view name = item.substr(0, colon);
      int fd = parse_fd(item.substr(colon + 1));
      if (fd < 0) {
        set_error(error, "invalid fd for '" + std::string(name) + "'");
        return std::nullopt;
      }

      int *slot = nullptr;
      size_t index = 0;
      if (name == "https") {
        slot = &out.https;
        index = 0;
      } else if (name == "http") {
        slot = &out.http;
        index = 1;
      } else if (name == "webui") {
        slot = &out.webui;
        index = 2;
      } else if (name == "rtsp") {
        slot = &out.rtsp;
        index = 3;
      } else {
        set_error(error, "unknown listener name '" + std::string(name) + "'");
        return std::nullopt;
      }
      if (seen[index]) {
        set_error(error, "duplicate listener '" + std::string(name) + "'");
        return std::nullopt;
      }
      seen[index] = true;
      *slot = fd;
    }

    if (!std::all_of(seen.begin(), seen.end(), [](bool b) {
          return b;
        })) {
      set_error(error, "missing listener in SUNSHINE_HANDOFF_FDS");
      return std::nullopt;
    }

    out.channel = parse_fd(channel);
    if (out.channel < 0) {
      set_error(error, "invalid channel fd");
      return std::nullopt;
    }

    std::array<int, 5> all {out.https, out.http, out.webui, out.rtsp, out.channel};
    std::sort(all.begin(), all.end());
    if (std::adjacent_find(all.begin(), all.end()) != all.end()) {
      set_error(error, "repeated fd");
      return std::nullopt;
    }
    return out;
  }

  std::string validate_listener(int fd, int port, bool ipv6) {
    int value = 0;
    socklen_t len = sizeof(value);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &value, &len) < 0) {
      return "fd is not a socket: " + errno_text(errno);
    }
    if (value != SOCK_STREAM) {
      return "socket is not TCP (not a stream socket)";
    }

    len = sizeof(value);
    if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &value, &len) < 0) {
      return "cannot read the socket protocol: " + errno_text(errno);
    }
    if (value != IPPROTO_TCP) {
      return "socket is not TCP (other protocol)";
    }

    len = sizeof(value);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &value, &len) < 0) {
      return "cannot read the socket family: " + errno_text(errno);
    }
    if (value != (ipv6 ? AF_INET6 : AF_INET)) {
      return ipv6 ? "socket is not IPv6" : "socket is not IPv4";
    }

    if (ipv6) {
      // address_family = both must also serve IPv4, so the socket must be dual-stack.
      len = sizeof(value);
      if (getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &value, &len) < 0) {
        return "cannot read IPV6_V6ONLY: " + errno_text(errno);
      }
      if (value != 0) {
        return "socket is IPv6 only (IPV6_V6ONLY set)";
      }
    }

    len = sizeof(value);
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &value, &len) < 0) {
      return "cannot read SO_ACCEPTCONN: " + errno_text(errno);
    }
    if (value == 0) {
      return "socket is not listening";
    }

    sockaddr_storage addr {};
    len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) < 0) {
      return "getsockname failed: " + errno_text(errno);
    }
    int actual = -1;
    if (addr.ss_family == AF_INET6) {
      actual = ntohs(reinterpret_cast<sockaddr_in6 *>(&addr)->sin6_port);
    } else if (addr.ss_family == AF_INET) {
      actual = ntohs(reinterpret_cast<sockaddr_in *>(&addr)->sin_port);
    }
    if (actual != port) {
      return "socket is bound to port " + std::to_string(actual) + ", expected " + std::to_string(port);
    }
    return {};
  }

  void scrub_inherited() {
    for (int fd = first_inherited_fd; fd <= channel_child_fd; ++fd) {
      int flags = fcntl(fd, F_GETFD);
      if (flags >= 0 && !(flags & FD_CLOEXEC)) {
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
      }
    }

    // Collect the names first: unsetenv edits environ while it would be walked.
    std::vector<std::string> names;
    if (environ) {
      for (char **e = environ; *e; ++e) {
        std::string_view entry(*e);
        if (entry.substr(0, env_prefix.size()) == env_prefix) {
          names.emplace_back(entry.substr(0, entry.find('=')));
        }
      }
    }
    for (const auto &name : names) {
      unsetenv(name.c_str());
    }
  }

  std::optional<child_t> spawn_child(const spawn_request_t &request, std::string *error) {
    if (request.binary.empty() || request.argv.empty()) {
      set_error(error, "no binary or arguments to spawn");
      return std::nullopt;
    }

    const std::array<int, 5> sources {
      request.fds.https,
      request.fds.http,
      request.fds.webui,
      request.fds.rtsp,
      request.fds.channel,
    };

    // Duplicate every source above the target range so no dup2 can overwrite another source.
    std::array<int, 5> temp {-1, -1, -1, -1, -1};
    for (size_t i = 0; i < sources.size(); ++i) {
      if (sources[i] < 0) {
        set_error(error, "invalid source fd");
        close_all(temp);
        return std::nullopt;
      }
      temp[i] = fcntl(sources[i], F_DUPFD_CLOEXEC, temp_fd_floor);
      if (temp[i] < 0) {
        set_error(error, "cannot duplicate fd: " + errno_text(errno));
        close_all(temp);
        return std::nullopt;
      }
    }

    // Environment: the parent's, with the extra entries replacing or added.
    std::vector<std::string> env;
    if (environ) {
      for (char **e = environ; *e; ++e) {
        env.emplace_back(*e);
      }
    }
    for (const auto &extra : request.extra_env) {
      size_t eq = extra.find('=');
      if (eq == std::string::npos || eq == 0) {
        set_error(error, "invalid extra environment entry (needs NAME=value)");
        close_all(temp);
        return std::nullopt;
      }
      const std::string name = extra.substr(0, eq + 1);
      // Replace every existing entry of that name, keeping the first position; drop the duplicates.
      bool replaced = false;
      for (auto it = env.begin(); it != env.end();) {
        if (it->compare(0, name.size(), name) == 0) {
          if (!replaced) {
            *it = extra;
            replaced = true;
            ++it;
          } else {
            it = env.erase(it);
          }
        } else {
          ++it;
        }
      }
      if (!replaced) {
        env.push_back(extra);
      }
    }
    std::vector<char *> envp;
    envp.reserve(env.size() + 1);
    for (auto &entry : env) {
      envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    std::vector<std::string> args = request.argv;
    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (auto &arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc != 0) {
      set_error(error, "posix_spawn_file_actions_init failed: " + errno_text(rc));
      close_all(temp);
      return std::nullopt;
    }
    posix_spawnattr_t attr;
    rc = posix_spawnattr_init(&attr);
    if (rc != 0) {
      set_error(error, "posix_spawnattr_init failed: " + errno_text(rc));
      posix_spawn_file_actions_destroy(&actions);
      close_all(temp);
      return std::nullopt;
    }

    std::optional<child_t> result;
    std::string failure;

    sigset_t empty;
    sigset_t full;
    sigemptyset(&empty);
    sigfillset(&full);

    // dup2 onto a different fd clears FD_CLOEXEC on the target.
    for (size_t i = 0; i < temp.size() && rc == 0; ++i) {
      rc = posix_spawn_file_actions_adddup2(&actions, temp[i], first_inherited_fd + static_cast<int>(i));
    }
    if (rc == 0) {
      rc = posix_spawn_file_actions_addclosefrom_np(&actions, channel_child_fd + 1);
    }
    if (rc == 0) {
      rc = posix_spawnattr_setsigmask(&attr, &empty);
    }
    if (rc == 0) {
      rc = posix_spawnattr_setsigdefault(&attr, &full);
    }
    if (rc == 0) {
      rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    }

    if (rc != 0) {
      failure = "cannot prepare the spawn: " + errno_text(rc);
    } else {
      int pidfd = -1;
      rc = pidfd_spawn(&pidfd, request.binary.c_str(), &actions, &attr, argv.data(), envp.data());
      // pidfd_spawn needs glibc 2.39; posix_spawn_file_actions_addclosefrom_np needs glibc 2.34.
      if (rc != 0) {
        failure = "pidfd_spawn failed: " + errno_text(rc);
        if (pidfd >= 0) {
          ::close(pidfd);
        }
      } else {
        child_t child;
        child.pidfd = pidfd;
        child.pid = pid_of_pidfd(pidfd);
        result = child;
      }
    }

    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close_all(temp);

    if (!result) {
      set_error(error, failure);
    }
    return result;
  }

  reap_result_e kill_and_reap(int pidfd, std::chrono::milliseconds timeout, int *exit_status) {
    if (pidfd < 0) {
      return reap_result_e::error;
    }

    if (syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0) < 0 && errno != ESRCH) {
      return reap_result_e::error;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      int wait_ms = static_cast<int>(std::clamp<long long>(left.count(), 0, 1000 * 60 * 60));
      pollfd pfd {pidfd, POLLIN, 0};
      int n = poll(&pfd, 1, wait_ms);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return reap_result_e::error;
      }
      if (n == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
          return reap_result_e::timeout;
        }
        continue;
      }
      if (pfd.revents & POLLNVAL) {
        return reap_result_e::error;
      }
      break;
    }

    siginfo_t info {};
    for (;;) {
      info = {};
      if (waitid(static_cast<idtype_t>(P_PIDFD), static_cast<id_t>(pidfd), &info, WEXITED | WNOHANG) == 0) {
        if (info.si_pid != 0) {
          break;
        }
        // Readable but not yet reapable: retry briefly until the deadline instead of returning at once.
        if (std::chrono::steady_clock::now() >= deadline) {
          return reap_result_e::timeout;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == ECHILD) {
        return reap_result_e::reaped_elsewhere;
      }
      return reap_result_e::error;
    }
    if (exit_status) {
      *exit_status = info.si_status;
    }
    return reap_result_e::reaped;
  }

  bool pidfd_exited(int pidfd) {
    if (pidfd < 0) {
      return true;
    }
    for (;;) {
      pollfd pfd {pidfd, POLLIN, 0};
      int n = poll(&pfd, 1, 0);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return true;
      }
      return n > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
    }
  }

  int open_self_pidfd() {
    int fd = static_cast<int>(syscall(SYS_pidfd_open, getpid(), 0));
    if (fd < 0) {
      return -1;
    }
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || (!(flags & FD_CLOEXEC) && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)) {
      ::close(fd);
      return -1;
    }
    return fd;
  }
}  // namespace handoff
