/**
 * @file src/handoff/sd_notify.cpp
 * @brief Definitions for `src/handoff/sd_notify.h`.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

// local includes
#include "sd_notify.h"
#include "src/logging.h"

// The parent environment, copied for the systemctl child in `read_back_mainpid`.
extern char **environ;

namespace handoff::sd {
  namespace {
    using clock_t_ = std::chrono::steady_clock;

    /**
     * @brief Split a string on a separator, skipping empty pieces.
     *
     * @param text Text to split.
     * @param sep Separator character.
     * @return The non-empty pieces, in order.
     */
    std::vector<std::string_view> split_nonempty(std::string_view text, char sep) {
      std::vector<std::string_view> pieces;
      std::size_t start = 0;
      while (start <= text.size()) {
        std::size_t end = text.find(sep, start);
        if (end == std::string_view::npos) {
          end = text.size();
        }
        if (end > start) {
          pieces.push_back(text.substr(start, end - start));
        }
        start = end + 1;
      }
      return pieces;
    }

    /**
     * @brief Remove leading and trailing whitespace.
     *
     * @param text Text to trim.
     * @return The trimmed view.
     */
    std::string_view trim(std::string_view text) {
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
      }
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
      }
      return text;
    }

    /**
     * @brief Find the cgroup v2 path (the `0::` line) in the content of `/proc/self/cgroup`.
     *
     * @param cgroup Content of `/proc/self/cgroup`.
     * @return The path after `0::`, or `std::nullopt` when there is no such line.
     */
    std::optional<std::string_view> cgroup_v2_path(std::string_view cgroup) {
      constexpr std::string_view prefix = "0::";
      std::size_t start = 0;
      while (start < cgroup.size()) {
        std::size_t end = cgroup.find('\n', start);
        if (end == std::string_view::npos) {
          end = cgroup.size();
        }
        std::string_view line = cgroup.substr(start, end - start);
        if (line.starts_with(prefix)) {
          return line.substr(prefix.size());
        }
        start = end + 1;
      }
      return std::nullopt;
    }

    /**
     * @brief Whether a cgroup path component is a service unit name (`<name>.service`, with a name).
     *
     * @param component One path component.
     * @return `true` for a service unit name.
     */
    bool is_service_component(std::string_view component) {
      constexpr std::string_view suffix = ".service";
      return component.size() > suffix.size() && component.ends_with(suffix);
    }

    /**
     * @brief Build a `sockaddr_un` for a `NOTIFY_SOCKET` value.
     *
     * A leading `@` selects an abstract socket: the address starts with a NUL byte and the length covers only the name.
     *
     * @param path The `NOTIFY_SOCKET` value.
     * @param addr Output address.
     * @return The address length to pass to `sendmsg`, or 0 when the path is empty or too long.
     */
    socklen_t make_address(std::string_view path, sockaddr_un &addr) {
      std::memset(&addr, 0, sizeof(addr));
      addr.sun_family = AF_UNIX;

      const bool abstract = !path.empty() && path.front() == '@';
      const std::string_view name = abstract ? path.substr(1) : path;
      if (name.empty() || name.size() > sizeof(addr.sun_path) - 1) {
        return 0;
      }

      if (abstract) {
        // sun_path[0] stays NUL; the name follows it and the length is exact (no trailing NUL).
        std::memcpy(addr.sun_path + 1, name.data(), name.size());
        return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
      }

      std::memcpy(addr.sun_path, name.data(), name.size());
      return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name.size() + 1);
    }

    /**
     * @brief Send one datagram to a notify socket, optionally with an fd attached.
     *
     * @param notify_socket The `NOTIFY_SOCKET` value.
     * @param payload Datagram payload.
     * @param attach_fd Fd to pass with `SCM_RIGHTS`, or -1.
     * @return `true` when the whole payload was sent.
     */
    bool send_datagram(std::string_view notify_socket, std::string_view payload, int attach_fd) {
      if (notify_socket.empty()) {
        return false;
      }

      sockaddr_un addr {};
      const socklen_t addr_len = make_address(notify_socket, addr);
      if (addr_len == 0) {
        BOOST_LOG(error) << "Drain handoff: NOTIFY_SOCKET is empty or too long";
        return false;
      }

      const int sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
      if (sock < 0) {
        BOOST_LOG(error) << "Drain handoff: socket for NOTIFY_SOCKET failed: " << std::strerror(errno);
        return false;
      }

      iovec iov {};
      iov.iov_base = const_cast<char *>(payload.data());
      iov.iov_len = payload.size();

      msghdr msg {};
      msg.msg_name = &addr;
      msg.msg_namelen = addr_len;
      msg.msg_iov = &iov;
      msg.msg_iovlen = 1;

      // Room for one int in an SCM_RIGHTS control message.
      alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
      if (attach_fd >= 0) {
        std::memset(control, 0, sizeof(control));
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);

        cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &attach_fd, sizeof(int));
      }

      ssize_t sent;
      do {
        sent = sendmsg(sock, &msg, MSG_NOSIGNAL);
      } while (sent < 0 && errno == EINTR);
      const int send_errno = errno;
      close(sock);

      if (sent < 0 || static_cast<std::size_t>(sent) != payload.size()) {
        BOOST_LOG(error) << "Drain handoff: sending to NOTIFY_SOCKET failed: " << std::strerror(send_errno);
        return false;
      }
      return true;
    }

    /**
     * @brief Wait until the read end of a pipe reports end-of-file (every write end is closed).
     *
     * Data written to the pipe is read and discarded; only end-of-file counts.
     *
     * @param read_fd Read end of the pipe.
     * @param timeout Longest wait.
     * @return `true` at end-of-file, `false` on timeout or error.
     */
    bool wait_for_eof(int read_fd, std::chrono::milliseconds timeout) {
      const auto deadline = clock_t_::now() + timeout;
      char buf[256];

      while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_t_::now()).count();
        const int wait_ms = static_cast<int>(std::clamp<long long>(left, 0, INT_MAX));

        pollfd pfd {};
        pfd.fd = read_fd;
        pfd.events = POLLIN;
        const int rc = poll(&pfd, 1, wait_ms);
        if (rc < 0) {
          if (errno == EINTR) {
            continue;
          }
          BOOST_LOG(error) << "Drain handoff: poll on the barrier pipe failed: " << std::strerror(errno);
          return false;
        }
        if (rc == 0) {
          if (clock_t_::now() >= deadline) {
            return false;
          }
          continue;
        }

        const ssize_t n = read(read_fd, buf, sizeof(buf));
        if (n == 0) {
          return true;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
          continue;
        }
        if (n < 0) {
          BOOST_LOG(error) << "Drain handoff: read on the barrier pipe failed: " << std::strerror(errno);
          return false;
        }
        // Unexpected data: discard it and keep waiting for end-of-file.
      }
    }

    /**
     * @brief Kill a child by its pid and reap it.
     *
     * @param pid Child pid.
     */
    void kill_and_reap(pid_t pid) {
      kill(pid, SIGKILL);
      while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
  }  // namespace

  bool notify(std::string_view notify_socket, std::string_view payload, int attach_fd) {
    try {
      return send_datagram(notify_socket, payload, attach_fd);
    } catch (...) {
      return false;
    }
  }

  std::string mainpid_payload(pid_t pid) {
    try {
      return "MAINPID=" + std::to_string(pid);
    } catch (...) {
      return {};
    }
  }

  bool barrier(std::string_view notify_socket, std::chrono::milliseconds timeout) {
    try {
      if (notify_socket.empty()) {
        return false;
      }

      int fds[2] {-1, -1};
      if (pipe2(fds, O_CLOEXEC) != 0) {
        BOOST_LOG(error) << "Drain handoff: pipe for the barrier failed: " << std::strerror(errno);
        return false;
      }
      const int read_fd = fds[0];
      const int write_fd = fds[1];

      // systemd gets its own copy of the write end through SCM_RIGHTS; ours is closed right after the send so the
      // read end sees end-of-file once systemd drops its copy.
      const bool sent = send_datagram(notify_socket, "BARRIER=1", write_fd);
      close(write_fd);
      if (!sent) {
        close(read_fd);
        return false;
      }

      const bool done = wait_for_eof(read_fd, timeout);
      close(read_fd);
      if (!done) {
        BOOST_LOG(error) << "Drain handoff: systemd did not complete the barrier in time";
      }
      return done;
    } catch (...) {
      return false;
    }
  }

  std::optional<std::string> unit_from_cgroup(std::string_view cgroup) {
    try {
      const auto path = cgroup_v2_path(cgroup);
      if (!path) {
        return std::nullopt;
      }

      // The innermost (last) service component is this process's unit.
      const auto components = split_nonempty(*path, '/');
      for (auto it = components.rbegin(); it != components.rend(); ++it) {
        if (is_service_component(*it)) {
          return std::string(*it);
        }
      }
      return std::nullopt;
    } catch (...) {
      return std::nullopt;
    }
  }

  bool is_user_unit(std::string_view cgroup) {
    try {
      const auto path = cgroup_v2_path(cgroup);
      if (!path) {
        return false;
      }

      // A user manager's service is named user@<uid>.service.
      for (const auto component : split_nonempty(*path, '/')) {
        if (component.starts_with("user@") && is_service_component(component)) {
          return true;
        }
      }
      return false;
    } catch (...) {
      return false;
    }
  }

  std::optional<int> parse_systemd_version(std::string_view output) {
    try {
      // Only the first line matters: "systemd <n> (<extra>)".
      const std::size_t newline = output.find('\n');
      std::string_view line = output.substr(0, newline);
      line = trim(line);

      constexpr std::string_view name = "systemd";
      if (!line.starts_with(name)) {
        return std::nullopt;
      }
      line.remove_prefix(name.size());
      if (line.empty() || !std::isspace(static_cast<unsigned char>(line.front()))) {
        return std::nullopt;
      }
      line = trim(line);

      int version = 0;
      std::size_t digits = 0;
      while (digits < line.size() && std::isdigit(static_cast<unsigned char>(line[digits]))) {
        version = version * 10 + (line[digits] - '0');
        if (version > 100000) {
          return std::nullopt;
        }
        ++digits;
      }
      if (digits == 0) {
        return std::nullopt;
      }
      return version;
    } catch (...) {
      return std::nullopt;
    }
  }

  std::optional<pid_t> parse_mainpid(std::string_view output) {
    try {
      const std::string_view text = trim(output);
      if (text.empty()) {
        return std::nullopt;
      }

      long long value = 0;
      for (const char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
          return std::nullopt;
        }
        value = value * 10 + (c - '0');
        if (value > std::numeric_limits<pid_t>::max()) {
          return std::nullopt;
        }
      }
      return static_cast<pid_t>(value);
    } catch (...) {
      return std::nullopt;
    }
  }

  std::optional<pid_t> read_back_mainpid(const std::string &unit, bool user_unit, std::chrono::milliseconds timeout) {
    try {
      // A unit name starting with '-' would be read by systemctl as an option.
      if (unit.empty() || unit.front() == '-') {
        return std::nullopt;
      }

      // argv: systemctl [--user] show -p MainPID --value <unit>; the unit is one element, no shell involved.
      std::vector<std::string> args {"systemctl"};
      if (user_unit) {
        args.emplace_back("--user");
      }
      for (const char *arg : {"show", "-p", "MainPID", "--value"}) {
        args.emplace_back(arg);
      }
      args.push_back(unit);

      std::vector<char *> argv;
      argv.reserve(args.size() + 1);
      for (auto &arg : args) {
        argv.push_back(arg.data());
      }
      argv.push_back(nullptr);

      // The environment is the parent's, with XDG_RUNTIME_DIR pointing at the user manager for user units.
      const std::string runtime_dir = "XDG_RUNTIME_DIR=/run/user/" + std::to_string(getuid());
      std::vector<std::string> env_strings;
      for (char **entry = environ; entry && *entry; ++entry) {
        if (user_unit && std::string_view(*entry).starts_with("XDG_RUNTIME_DIR=")) {
          continue;
        }
        env_strings.emplace_back(*entry);
      }
      if (user_unit) {
        env_strings.push_back(runtime_dir);
      }
      std::vector<char *> envp;
      envp.reserve(env_strings.size() + 1);
      for (auto &entry : env_strings) {
        envp.push_back(entry.data());
      }
      envp.push_back(nullptr);

      int out_fds[2] {-1, -1};
      if (pipe2(out_fds, O_CLOEXEC) != 0) {
        BOOST_LOG(error) << "Drain handoff: pipe for systemctl failed: " << std::strerror(errno);
        return std::nullopt;
      }
      const int read_fd = out_fds[0];
      const int write_fd = out_fds[1];

      posix_spawn_file_actions_t actions;
      if (posix_spawn_file_actions_init(&actions) != 0) {
        close(read_fd);
        close(write_fd);
        return std::nullopt;
      }
      // stdout goes to the pipe (dup2 clears FD_CLOEXEC on the copy); stdin is /dev/null; stderr stays the journal.
      posix_spawn_file_actions_adddup2(&actions, write_fd, STDOUT_FILENO);
      posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

      pid_t pid = -1;
      const int spawn_rc = posix_spawnp(&pid, "systemctl", &actions, nullptr, argv.data(), envp.data());
      posix_spawn_file_actions_destroy(&actions);
      close(write_fd);
      if (spawn_rc != 0) {
        close(read_fd);
        BOOST_LOG(error) << "Drain handoff: could not start systemctl: " << std::strerror(spawn_rc);
        return std::nullopt;
      }

      const auto deadline = clock_t_::now() + timeout;
      std::string output;
      bool eof = false;
      bool failed = false;
      char buf[512];

      // Read stdout until end-of-file or the deadline.
      while (!eof) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_t_::now()).count();
        if (left <= 0) {
          failed = true;
          break;
        }

        pollfd pfd {};
        pfd.fd = read_fd;
        pfd.events = POLLIN;
        const int rc = poll(&pfd, 1, static_cast<int>(std::min<long long>(left, INT_MAX)));
        if (rc < 0) {
          if (errno == EINTR) {
            continue;
          }
          failed = true;
          break;
        }
        if (rc == 0) {
          continue;
        }

        const ssize_t n = read(read_fd, buf, sizeof(buf));
        if (n == 0) {
          eof = true;
        } else if (n > 0) {
          output.append(buf, static_cast<std::size_t>(n));
          // A MainPID answer is a few bytes; anything much larger is not systemctl's answer.
          if (output.size() > 4096) {
            failed = true;
            break;
          }
        } else if (errno != EINTR && errno != EAGAIN) {
          failed = true;
          break;
        }
      }
      close(read_fd);

      if (failed || !eof) {
        BOOST_LOG(error) << "Drain handoff: reading the main PID with systemctl timed out or failed";
        kill_and_reap(pid);
        return std::nullopt;
      }

      // The child closed stdout; wait for it to exit within the same deadline, then kill it if it does not.
      int status = 0;
      while (true) {
        const pid_t reaped = waitpid(pid, &status, WNOHANG);
        if (reaped == pid) {
          break;
        }
        if (reaped < 0 && errno != EINTR) {
          BOOST_LOG(error) << "Drain handoff: waiting for systemctl failed: " << std::strerror(errno);
          return std::nullopt;
        }
        if (clock_t_::now() >= deadline) {
          BOOST_LOG(error) << "Drain handoff: systemctl did not exit in time";
          kill_and_reap(pid);
          return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }

      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        BOOST_LOG(error) << "Drain handoff: systemctl show MainPID failed";
        return std::nullopt;
      }
      return parse_mainpid(output);
    } catch (...) {
      return std::nullopt;
    }
  }
}  // namespace handoff::sd
