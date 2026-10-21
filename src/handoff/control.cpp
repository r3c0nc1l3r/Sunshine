/**
 * @file src/handoff/control.cpp
 * @brief Definitions for `src/handoff/control.h`.
 */
// standard includes
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

// local includes
#include "control.h"
#include "instance_file.h"
#include "src/logging.h"

namespace handoff {
  namespace fs = std::filesystem;

  namespace {
    using clock_t_ = std::chrono::steady_clock;

    constexpr std::size_t max_request_bytes = 256;  ///< Longest request line, newline included.
    constexpr std::size_t max_reply_bytes = 4096;  ///< Longest reply line the helper accepts, newline included.
    constexpr std::size_t max_instance_file_bytes = 64 * 1024;  ///< Largest instance file the helper reads.
    constexpr std::size_t max_reason_bytes = 1000;  ///< Longest reason in a reply line; the rest is cut.
    constexpr std::chrono::milliseconds server_io_timeout {2000};  ///< Server read and write bound per connection.

    /**
     * @brief Closes a file descriptor when it goes out of scope.
     */
    class fd_guard_t {
    public:
      /**
       * @brief Take ownership of a file descriptor.
       *
       * @param fd File descriptor; negative values are ignored.
       */
      explicit fd_guard_t(int fd):
          _fd(fd) {}

      /**
       * @brief Closes the descriptor.
       */
      ~fd_guard_t() {
        if (_fd >= 0) {
          ::close(_fd);
        }
      }

      fd_guard_t(const fd_guard_t &) = delete;
      fd_guard_t &operator=(const fd_guard_t &) = delete;

      /**
       * @brief The guarded descriptor.
       *
       * @return The file descriptor.
       */
      int get() const {
        return _fd;
      }

      /**
       * @brief Give up ownership without closing.
       *
       * @return The file descriptor; the guard is left empty.
       */
      int release() {
        const int fd = _fd;
        _fd = -1;
        return fd;
      }

    private:
      int _fd;  ///< Guarded descriptor.
    };

    /**
     * @brief Convert milliseconds to a `timeval`; zero or negative values become 1 ms, since zero means "no timeout".
     *
     * @param value Duration.
     * @return The equivalent `timeval`.
     */
    timeval to_timeval(std::chrono::milliseconds value) {
      const auto ms = std::max<std::int64_t>(value.count(), 1);
      timeval tv {};
      tv.tv_sec = static_cast<time_t>(ms / 1000);
      tv.tv_usec = static_cast<suseconds_t>((ms % 1000) * 1000);
      return tv;
    }

    /**
     * @brief Fill a `sockaddr_un` for a path.
     *
     * @param path Socket path.
     * @param addr Receives the address.
     * @return `true` when the path fits in `sun_path`.
     */
    bool make_address(const fs::path &path, sockaddr_un &addr) {
      const auto text = path.string();
      if (text.size() >= sizeof(addr.sun_path)) {
        return false;
      }
      addr = {};
      addr.sun_family = AF_UNIX;
      std::memcpy(addr.sun_path, text.c_str(), text.size() + 1);
      return true;
    }

    /**
     * @brief Outcome of reading one line.
     */
    struct line_result_t {
      enum class status_e {
        line,  ///< A full line, newline included, is in `text`.
        timeout,  ///< The deadline passed first.
        too_long,  ///< No newline within the byte limit.
        failed,  ///< End of file, or an error, before a newline.
      };

      status_e status {status_e::failed};  ///< Outcome.
      std::string text;  ///< The line when `status` is `line`.
    };

    /**
     * @brief Read one line from a socket, byte by byte, within a deadline.
     *
     * @param fd Socket.
     * @param timeout Longest total wait.
     * @param max_bytes Longest line accepted, newline included.
     * @return The outcome.
     */
    line_result_t read_line(int fd, std::chrono::milliseconds timeout, std::size_t max_bytes) {
      const auto deadline = clock_t_::now() + timeout;
      line_result_t result;

      while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_t_::now());
        if (remaining.count() <= 0) {
          result.status = line_result_t::status_e::timeout;
          return result;
        }

        pollfd pfd {fd, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
        if (ready < 0) {
          if (errno == EINTR) {
            continue;
          }
          result.status = line_result_t::status_e::failed;
          return result;
        }
        if (ready == 0) {
          result.status = line_result_t::status_e::timeout;
          return result;
        }

        char byte = 0;
        const auto n = ::recv(fd, &byte, 1, 0);
        if (n < 0) {
          if (errno == EINTR || errno == EAGAIN) {
            continue;
          }
          result.status = line_result_t::status_e::failed;
          return result;
        }
        if (n == 0) {
          result.status = line_result_t::status_e::failed;
          return result;
        }

        result.text.push_back(byte);
        if (byte == '\n') {
          result.status = line_result_t::status_e::line;
          return result;
        }
        if (result.text.size() >= max_bytes) {
          result.status = line_result_t::status_e::too_long;
          return result;
        }
      }
    }

    /**
     * @brief Write all of the data to a socket within a deadline.
     *
     * @param fd Socket.
     * @param data Data to send.
     * @param timeout Longest total wait.
     * @return `true` when everything was sent.
     */
    bool write_all(int fd, std::string_view data, std::chrono::milliseconds timeout) {
      const auto deadline = clock_t_::now() + timeout;
      std::size_t sent = 0;

      while (sent < data.size()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_t_::now());
        if (remaining.count() <= 0) {
          return false;
        }

        pollfd pfd {fd, POLLOUT, 0};
        const int ready = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
        if (ready < 0) {
          if (errno == EINTR) {
            continue;
          }
          return false;
        }
        if (ready == 0) {
          return false;
        }

        const auto n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
          if (errno == EINTR || errno == EAGAIN) {
            continue;
          }
          return false;
        }
        sent += static_cast<std::size_t>(n);
      }
      return true;
    }

#ifdef SUNSHINE_TESTS
    /**
     * @brief Test seam: the peer uid to report instead of `SO_PEERCRED`'s, or empty. Read once per connection. It exists
     * only in test builds, so a production build always takes the uid from `SO_PEERCRED`.
     */
    std::mutex peer_uid_override_mutex;  ///< Guards `peer_uid_override`.
    std::optional<uid_t> peer_uid_override;  ///< Uid reported by the server for every connection; empty when unset.
#endif

    /**
     * @brief Answer the verb of one request line, running the handler of that verb.
     *
     * `handoff` with an empty handler is refused with `drain handoff is disabled`; `close-app` with an empty handler and
     * any other line are refused with `unknown verb`. A handler that throws is answered `refused internal error`.
     *
     * @param line The request line, newline included.
     * @param peer_pid Peer pid from `SO_PEERCRED`, passed to the handler.
     * @param handlers Verb handlers.
     * @return The encoded reply line.
     */
    std::string dispatch_verb(const std::string &line, pid_t peer_pid, const control_server_t::handlers_t &handlers) {
      const auto refuse = [](const std::string &reason) {
        return encode_reply(control_reply_t {reply_kind_e::refused, reason});
      };
      const auto run = [&](const control_server_t::handler_t &handler, std::string_view verb) {
        try {
          return encode_reply(handler(peer_pid));
        } catch (...) {
          BOOST_LOG(error) << "Drain handoff: handler failed for the " << verb << " verb";
          return refuse("internal error");
        }
      };

      if (line == std::string(handoff_verb) + "\n") {
        if (!handlers.handoff) {
          return refuse("drain handoff is disabled");
        }
        return run(handlers.handoff, handoff_verb);
      }
      if (line == std::string(close_app_verb) + "\n") {
        if (!handlers.close_app) {
          return refuse("unknown verb");
        }
        return run(handlers.close_app, close_app_verb);
      }
      return refuse("unknown verb");
    }

    /**
     * @brief Handle one accepted connection on the server thread.
     *
     * @param fd Accepted socket; the caller closes it.
     * @param handlers Verb handlers.
     */
    void serve_connection(int fd, const control_server_t::handlers_t &handlers) {
      ucred cred {};
      socklen_t cred_len = sizeof(cred);
      if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0) {
        BOOST_LOG(error) << "Drain handoff: cannot read control socket peer credentials: " << std::strerror(errno);
        return;
      }

#ifdef SUNSHINE_TESTS
      std::optional<uid_t> override_uid;
      {
        std::lock_guard lock(peer_uid_override_mutex);
        override_uid = peer_uid_override;
      }
      const uid_t peer_uid = override_uid ? *override_uid : cred.uid;
#else
      const uid_t peer_uid = cred.uid;
#endif
      if (peer_uid != ::geteuid()) {
        BOOST_LOG(warning) << "Drain handoff: refused control connection from another user (pid " << cred.pid << ")";
        write_all(fd, encode_reply(control_reply_t {reply_kind_e::refused, "peer uid"}), server_io_timeout);
        return;
      }
      BOOST_LOG(info) << "Drain handoff: control connection from pid " << cred.pid;

      const auto line = read_line(fd, server_io_timeout, max_request_bytes);
      std::string reply;
      switch (line.status) {
        case line_result_t::status_e::line:
          reply = dispatch_verb(line.text, cred.pid, handlers);
          break;

        case line_result_t::status_e::too_long:
          reply = encode_reply(control_reply_t {reply_kind_e::refused, "unknown verb"});
          break;

        case line_result_t::status_e::timeout:
        case line_result_t::status_e::failed:
          BOOST_LOG(warning) << "Drain handoff: control connection ended without a request line";
          return;
      }

      if (!write_all(fd, reply, server_io_timeout)) {
        BOOST_LOG(warning) << "Drain handoff: cannot send the control reply";
      }
    }

    /**
     * @brief Parse a positive decimal process id.
     *
     * @param text Text to parse.
     * @return The pid, or `std::nullopt` when the text is not a positive decimal int.
     */
    std::optional<pid_t> parse_pid(std::string_view text) {
      int value = 0;
      const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (ec != std::errc() || end != text.data() + text.size() || text.empty() || value <= 0) {
        return std::nullopt;
      }
      return static_cast<pid_t>(value);
    }

    /**
     * @brief Name of a reply kind for the helper's message.
     *
     * @param kind Reply kind.
     * @return Keyword used on the wire.
     */
    std::string_view kind_name(reply_kind_e kind) {
      switch (kind) {
        case reply_kind_e::refused:
          return "refused";
        case reply_kind_e::failed:
          return "failed";
        case reply_kind_e::kill_pending:
          return "kill_pending";
        case reply_kind_e::committed:
          return "committed";
        case reply_kind_e::closed:
          return "closed";
      }
      return "refused";
    }

    /**
     * @brief Identity of a socket file: device and inode.
     */
    using file_id_t = std::pair<dev_t, ino_t>;

    /**
     * @brief Why a pid has no control socket, read from its instance file.
     *
     * A process in `handover_failed` or `kill_pending` keeps role `draining`, so the state is checked first.
     *
     * @param runtime_dir Runtime directory.
     * @param pid Process id.
     * @return `kill pending`, `handover failing` or `a drain is in progress`; `std::nullopt` when the file gives none of
     *         them (missing, not a regular file, too large, unreadable or not an instance file).
     */
    std::optional<std::string> missing_socket_reason(const fs::path &runtime_dir, pid_t pid) {
      // Open without following a symlink and without blocking on a FIFO; only a regular file is read.
      const auto file_path = runtime_dir / "instances" / (std::to_string(pid) + ".json");
      fd_guard_t file(::open(file_path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW));
      struct stat file_st {};
      if (file.get() < 0 || ::fstat(file.get(), &file_st) != 0 || !S_ISREG(file_st.st_mode) || static_cast<std::size_t>(file_st.st_size) > max_instance_file_bytes) {
        return std::nullopt;
      }

      // A file larger than the cap counts as unreadable.
      std::string content;
      char buffer[4096];
      bool ok = true;
      while (true) {
        const auto n = ::read(file.get(), buffer, sizeof(buffer));
        if (n < 0) {
          if (errno == EINTR) {
            continue;
          }
          ok = false;
          break;
        }
        if (n == 0) {
          break;
        }
        content.append(buffer, static_cast<std::size_t>(n));
        if (content.size() > max_instance_file_bytes) {
          ok = false;
          break;
        }
      }
      const auto instance = ok ? parse_instance(content) : std::nullopt;
      if (instance && instance->handoff == state_e::kill_pending) {
        return std::string("kill pending");
      }
      if (instance && instance->handoff == state_e::handover_failed) {
        return std::string("handover failing");
      }
      if (instance && instance->role == role_e::draining) {
        return std::string("a drain is in progress");
      }
      return std::nullopt;
    }

    /**
     * @brief Make a directory and its missing parents; created directories get mode 0700.
     *
     * @param dir Directory.
     * @param error Receives a one-line reason on failure.
     * @return `true` when `dir` exists as a directory afterwards.
     */
    bool make_directories_0700(const fs::path &dir, std::string &error) {
      // The runtime directory is assumed to be user-owned and private (`/run/user/<uid>` or `RuntimeDirectory=`), so
      // existing parents are trusted; only the control directory itself is checked in prepare_control_directory().
      if (dir.empty() || dir == dir.root_path()) {
        return true;
      }
      struct stat st {};
      if (::stat(dir.c_str(), &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
          error = "a parent of the control directory is not a directory";
          return false;
        }
        return true;
      }
      if (errno != ENOENT) {
        error = std::string("cannot inspect the control directory path: ") + std::strerror(errno);
        return false;
      }
      if (!make_directories_0700(dir.parent_path(), error)) {
        return false;
      }
      if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        error = std::string("cannot create the control directory: ") + std::strerror(errno);
        return false;
      }
      return true;
    }

    /**
     * @brief Create the control directory if needed and check and fix it: it must be a real directory (not a
     * symlink) owned by the effective user, and gets mode 0700.
     *
     * @param dir Control directory.
     * @param error Receives a one-line reason on failure.
     * @return `true` when the directory is safe to use.
     */
    bool prepare_control_directory(const fs::path &dir, std::string &error) {
      if (!make_directories_0700(dir.parent_path(), error)) {
        return false;
      }
      if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        error = std::string("cannot create the control directory: ") + std::strerror(errno);
        return false;
      }

      // O_NOFOLLOW makes a symlink fail here; the checks and the chmod then act on the opened directory itself.
      fd_guard_t fd(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (fd.get() < 0) {
        error = std::string("the control directory is not a real directory: ") + std::strerror(errno);
        return false;
      }
      struct stat st {};
      if (::fstat(fd.get(), &st) != 0) {
        error = std::string("cannot inspect the control directory: ") + std::strerror(errno);
        return false;
      }
      if (!S_ISDIR(st.st_mode) || st.st_uid != ::geteuid()) {
        error = "the control directory is not owned by the current user";
        return false;
      }
      if (::fchmod(fd.get(), 0700) != 0) {
        error = std::string("cannot set the control directory mode: ") + std::strerror(errno);
        return false;
      }
      return true;
    }

    /**
     * @brief Bind and listen on a socket path, replacing a stale socket file.
     *
     * The listening socket is non-blocking: the server thread accepts only after `poll()` and under the server lock.
     *
     * @param path Socket path.
     * @param listen_fd Receives the listening fd on success.
     * @param id Receives device and inode of the new socket file on success.
     * @param error Receives a one-line reason on failure.
     * @return `true` on success.
     */
    bool bind_listener(const fs::path &path, int &listen_fd, file_id_t &id, std::string &error) {
      if (!prepare_control_directory(path.parent_path().empty() ? fs::path(".") : path.parent_path(), error)) {
        return false;
      }

      // Replace a stale socket left by a previous run; never remove anything else.
      struct stat st {};
      if (::lstat(path.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
          error = "the socket path exists and is not a socket";
          return false;
        }
        if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
          error = std::string("cannot remove the stale socket: ") + std::strerror(errno);
          return false;
        }
      } else if (errno != ENOENT) {
        error = std::string("cannot inspect the socket path: ") + std::strerror(errno);
        return false;
      }

      sockaddr_un addr {};
      if (!make_address(path, addr)) {
        error = "socket path too long";
        return false;
      }

      fd_guard_t listener(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
      if (listener.get() < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
      }

      // The socket file is created inside the 0700 control directory and made 0600 right after bind. The process
      // umask is not touched: it is process-wide and would race other threads.
      if (::bind(listener.get(), reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
        error = std::string("bind: ") + std::strerror(errno);
        return false;
      }
      // From here the file is ours; record its identity first so that a failure can unlink exactly it.
      if (::lstat(path.c_str(), &st) != 0) {
        error = std::string("cannot inspect the new socket: ") + std::strerror(errno);
        return false;
      }
      id = {st.st_dev, st.st_ino};
      if (::chmod(path.c_str(), 0600) != 0 || ::listen(listener.get(), 8) != 0) {
        error = std::string("listen or chmod: ") + std::strerror(errno);
        ::unlink(path.c_str());
        return false;
      }

      listen_fd = listener.release();
      return true;
    }

    /**
     * @brief Increment an eventfd so that a thread polling it wakes.
     *
     * @param fd Eventfd; negative values are ignored.
     */
    void signal_wake(int fd) {
      if (fd < 0) {
        return;
      }
      const std::uint64_t one = 1;
      if (::write(fd, &one, sizeof(one)) < 0) {
        BOOST_LOG(error) << "Drain handoff: cannot wake the control socket thread: " << std::strerror(errno);
      }
    }
  }  // namespace

  control_socket_mode_e control_socket_mode(bool handoff_enabled, bool control_socket) {
    if (handoff_enabled) {
      return control_socket_mode_e::handoff;
    }
    if (control_socket) {
      return control_socket_mode_e::close_app_only;
    }
    return control_socket_mode_e::none;
  }

#ifdef SUNSHINE_TESTS
  void set_peer_uid_override_for_tests(std::optional<uid_t> uid) {
    std::lock_guard lock(peer_uid_override_mutex);
    peer_uid_override = uid;
  }
#endif

  /**
   * @brief Path of the control socket of process `pid` under the runtime directory.
   *
   * @param runtime_dir Runtime directory (see `runtime_dir()`).
   * @param pid Process id.
   * @return Socket path.
   */
  fs::path control_socket_path(const fs::path &runtime_dir, pid_t pid) {
    return runtime_dir / "control" / (std::to_string(pid) + ".sock");
  }

  std::string encode_reply(const control_reply_t &reply) {
    if (reply.kind == reply_kind_e::committed) {
      return "committed " + std::to_string(reply.new_pid) + " " + std::to_string(reply.new_start_time) + "\n";
    }
    if (reply.kind == reply_kind_e::closed) {
      return reply.closed_app ? "closed app\n" : "closed none\n";
    }

    // One line, and short enough for the helper's reply limit.
    std::string text = reply.reason.substr(0, max_reason_bytes);
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    const std::string keyword(kind_name(reply.kind));
    return text.empty() ? keyword + "\n" : keyword + " " + text + "\n";
  }

  std::optional<control_reply_t> parse_reply(std::string_view line) {
    std::string_view text = line;
    if (!text.empty() && text.back() == '\n') {
      text.remove_suffix(1);
    }
    if (text.find('\n') != std::string_view::npos) {
      return std::nullopt;
    }

    if (text.starts_with("committed ")) {
      const auto rest = text.substr(std::string_view("committed ").size());
      const auto space = rest.find(' ');
      if (space == std::string_view::npos) {
        return std::nullopt;
      }

      const auto pid = parse_pid(rest.substr(0, space));
      const auto start_text = rest.substr(space + 1);
      std::uint64_t start_time = 0;
      const auto [end, ec] = std::from_chars(start_text.data(), start_text.data() + start_text.size(), start_time);
      if (!pid || start_text.empty() || ec != std::errc() || end != start_text.data() + start_text.size()) {
        return std::nullopt;
      }

      control_reply_t reply;
      reply.kind = reply_kind_e::committed;
      reply.new_pid = *pid;
      reply.new_start_time = start_time;
      return reply;
    }

    if (text == "closed app" || text == "closed none") {
      control_reply_t reply;
      reply.kind = reply_kind_e::closed;
      reply.closed_app = text == "closed app";
      return reply;
    }

    const std::pair<std::string_view, reply_kind_e> reasons[] = {
      {"refused", reply_kind_e::refused},
      {"failed", reply_kind_e::failed},
      {"kill_pending", reply_kind_e::kill_pending},
    };
    for (const auto &[keyword, kind] : reasons) {
      if (text == keyword) {
        return control_reply_t {kind, ""};
      }
      if (text.starts_with(keyword) && text.size() > keyword.size() && text[keyword.size()] == ' ') {
        return control_reply_t {kind, std::string(text.substr(keyword.size() + 1))};
      }
    }
    return std::nullopt;
  }

  helper_exit_e exit_code_for(reply_kind_e kind) {
    switch (kind) {
      case reply_kind_e::refused:
        return helper_exit_e::refused;
      case reply_kind_e::failed:
        return helper_exit_e::failed;
      case reply_kind_e::kill_pending:
        return helper_exit_e::kill_pending;
      case reply_kind_e::committed:
        return helper_exit_e::switched;
      case reply_kind_e::closed:
        return helper_exit_e::switched;  ///< `closed` is exit 0, the same code as `switched`.
    }
    return helper_exit_e::error;
  }

  helper_exit_e run_helper(const std::vector<std::string> &args, const helper_env_t &env, std::string &err) {
    if (args.size() != 1) {
      err = "usage: --drain-handoff <MAINPID>";
      return helper_exit_e::error;
    }
    const auto pid = parse_pid(args.front());
    if (!pid) {
      err = "invalid MAINPID, expected a positive decimal pid";
      return helper_exit_e::error;
    }

    constexpr auto no_socket = "no control socket (feature disabled?)";
    if (env.runtime_dir.empty()) {
      err = no_socket;
      return helper_exit_e::refused;
    }

    // With no control socket, the instance file of the pid explains why (drain in progress, handover failing,
    // kill pending); otherwise there is simply no socket. The same check runs when connect() finds the socket gone
    // (the process closed it between the stat() and the connect()).
    const auto socket_path = control_socket_path(env.runtime_dir, *pid);
    const auto explain_missing_socket = [&]() {
      if (const auto reason = missing_socket_reason(env.runtime_dir, *pid)) {
        err = *reason;
        return helper_exit_e::refused;
      }
      err = no_socket;
      return helper_exit_e::refused;
    };

    struct stat st {};
    if (::stat(socket_path.c_str(), &st) != 0 && errno == ENOENT) {
      return explain_missing_socket();
    }

    fd_guard_t guard(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (guard.get() < 0) {
      err = std::string("socket: ") + std::strerror(errno);
      return helper_exit_e::error;
    }

    // Blocking connect bounded by SO_SNDTIMEO (honoured by AF_UNIX connect on Linux).
    const auto connect_tv = to_timeval(env.connect_timeout);
    ::setsockopt(guard.get(), SOL_SOCKET, SO_SNDTIMEO, &connect_tv, sizeof(connect_tv));

    sockaddr_un addr {};
    if (!make_address(socket_path, addr)) {
      err = "control socket path too long";
      return helper_exit_e::error;
    }
    if (::connect(guard.get(), reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
      if (errno == ENOENT || errno == ECONNREFUSED) {
        return explain_missing_socket();
      }
      err = std::string("connect: ") + std::strerror(errno);
      return helper_exit_e::error;
    }

    if (!write_all(guard.get(), std::string(handoff_verb) + "\n", env.connect_timeout)) {
      err = "sending the request failed or timed out";
      return helper_exit_e::error;
    }

    const auto line = read_line(guard.get(), env.reply_timeout, max_reply_bytes);
    if (line.status != line_result_t::status_e::line) {
      err = "no reply from the active process";
      return helper_exit_e::error;
    }
    const auto reply = parse_reply(line.text);
    if (!reply) {
      err = "unparsable reply from the active process";
      return helper_exit_e::error;
    }

    if (reply->kind != reply_kind_e::committed) {
      const auto code = exit_code_for(reply->kind);
      err = std::string(kind_name(reply->kind));
      if (!reply->reason.empty()) {
        err += ": " + reply->reason;
      }
      return code;
    }

    // Committed: exit 0 only once the new process's instance file shows role active.
    if (wait_for_active(env.runtime_dir / "instances", reply->new_pid, reply->new_start_time, env.active_timeout)) {
      err = "switched: new process " + std::to_string(reply->new_pid) + " is active";
      return helper_exit_e::switched;
    }
    err = "committed, but role active was not observed in time";
    return helper_exit_e::unconfirmed;
  }

  helper_exit_e run_close_app_helper(const std::vector<std::string> &args, const helper_env_t &env, std::string &err) {
    if (args.size() != 1) {
      err = "usage: --close-app <MAINPID>";
      return helper_exit_e::error;
    }
    const auto pid = parse_pid(args.front());
    if (!pid) {
      err = "invalid MAINPID, expected a positive decimal pid";
      return helper_exit_e::error;
    }

    constexpr auto no_socket = "no control socket (feature disabled?)";
    const auto no_socket_result = [&]() {
      err = no_socket;
      return helper_exit_e::unsupported;
    };
    if (env.runtime_dir.empty()) {
      return no_socket_result();
    }

    // Without a socket, the instance file says whether a drain explains it; otherwise the verb is not served here.
    const auto socket_path = control_socket_path(env.runtime_dir, *pid);
    const auto missing_socket = [&]() {
      if (const auto reason = missing_socket_reason(env.runtime_dir, *pid)) {
        err = *reason;
        return helper_exit_e::refused;
      }
      return no_socket_result();
    };

    struct stat st {};
    if (::stat(socket_path.c_str(), &st) != 0 && errno == ENOENT) {
      return missing_socket();
    }

    fd_guard_t guard(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (guard.get() < 0) {
      err = std::string("socket: ") + std::strerror(errno);
      return helper_exit_e::error;
    }

    const auto connect_tv = to_timeval(env.connect_timeout);
    ::setsockopt(guard.get(), SOL_SOCKET, SO_SNDTIMEO, &connect_tv, sizeof(connect_tv));

    sockaddr_un addr {};
    if (!make_address(socket_path, addr)) {
      err = "control socket path too long";
      return helper_exit_e::error;
    }
    if (::connect(guard.get(), reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
      if (errno == ENOENT || errno == ECONNREFUSED) {
        return missing_socket();
      }
      err = std::string("connect: ") + std::strerror(errno);
      return helper_exit_e::error;
    }

    if (!write_all(guard.get(), std::string(close_app_verb) + "\n", env.connect_timeout)) {
      err = "sending the request failed or timed out";
      return helper_exit_e::error;
    }

    const auto line = read_line(guard.get(), env.reply_timeout, max_reply_bytes);
    if (line.status != line_result_t::status_e::line) {
      err = "no reply from the active process";
      return helper_exit_e::error;
    }
    const auto reply = parse_reply(line.text);
    if (!reply) {
      err = "unparsable reply from the active process";
      return helper_exit_e::error;
    }

    if (reply->kind == reply_kind_e::closed) {
      err = reply->closed_app ? "closed app" : "closed none";
      return helper_exit_e::switched;
    }
    if (reply->kind == reply_kind_e::committed) {
      err = "unexpected reply from the active process";
      return helper_exit_e::error;
    }

    err = std::string(kind_name(reply->kind));
    if (!reply->reason.empty()) {
      err += ": " + reply->reason;
    }
    // An older binary does not know the verb: final, not a retry.
    if (reply->kind == reply_kind_e::refused && reply->reason == "unknown verb") {
      return helper_exit_e::unsupported;
    }
    return exit_code_for(reply->kind);
  }

  bool control_server_t::start(const std::filesystem::path &path, handler_t handler, std::string *error_out) {
    return start(path, handlers_t {std::move(handler), {}}, error_out);
  }

  bool control_server_t::start(const std::filesystem::path &path, handlers_t handlers, std::string *error_out) {
    const auto fail = [error_out](const std::string &message) {
      if (error_out) {
        *error_out = message;
      }
      BOOST_LOG(error) << "Drain handoff: control socket: " << message;
      return false;
    };

    std::lock_guard lock(_mutex);
    if (_thread.joinable() || _listen_fd >= 0) {
      return fail("already started");
    }
    // stop() on another thread is still joining: _stopped is set, the thread is moved out, and the wake fd stays open
    // until the joiner has closed it. Starting now would close an fd the joiner still uses.
    if (_stopped && _wake_fd >= 0) {
      return fail("stopping");
    }

    std::string reason;
    int listen_fd = -1;
    file_id_t id {};
    if (!bind_listener(path, listen_fd, id, reason)) {
      return fail(reason);
    }
    fd_guard_t listener(listen_fd);

    fd_guard_t wake(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (wake.get() < 0) {
      const auto message = std::string("eventfd: ") + std::strerror(errno);
      ::unlink(path.c_str());
      return fail(message);
    }

    _path = path;
    _handlers = std::move(handlers);
    _stopped = false;
    const int wake_fd = wake.get();

    try {
      // The thread polls a snapshot of the listen fd, taken under the lock for every poll. close_listener() closes
      // the fd under the lock and wakes the thread; the thread re-checks under the lock, immediately before
      // accept(), that the snapshot is still the current listen fd. The listener is non-blocking, so accept() under
      // the lock never blocks. Hence the thread never accepts on a closed or reused fd number. A stale snapshot
      // can at worst make poll() report a spurious event, which the re-check discards.
      _thread = std::thread([this, wake_fd]() {
        while (true) {
          try {
            int snapshot = -1;
            {
              std::lock_guard guard(_mutex);
              if (_stopped) {
                return;
              }
              snapshot = _listen_fd;
            }

            // A negative fd is ignored by poll(): while no listener is open only the wake fd is polled.
            pollfd fds[2] = {{snapshot, POLLIN, 0}, {wake_fd, POLLIN, 0}};
            if (::poll(fds, 2, -1) < 0) {
              if (errno == EINTR) {
                continue;
              }
              BOOST_LOG(error) << "Drain handoff: control socket poll failed: " << std::strerror(errno);
              return;
            }

            if (fds[1].revents != 0) {
              // Reset the counter; the loop top then re-reads _stopped and the listen fd.
              std::uint64_t counter = 0;
              if (::read(wake_fd, &counter, sizeof(counter)) < 0 && errno != EAGAIN && errno != EINTR) {
                BOOST_LOG(error) << "Drain handoff: cannot read the control socket wake fd: " << std::strerror(errno);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
              }
              continue;
            }
            if (snapshot < 0 || fds[0].revents == 0) {
              continue;
            }

            int client = -1;
            int accept_errno = 0;
            handlers_t current_handlers;
            {
              std::lock_guard guard(_mutex);
              if (_stopped) {
                return;
              }
              if (_listen_fd != snapshot) {
                continue;  // closed or replaced while polling
              }
              client = ::accept4(snapshot, nullptr, nullptr, SOCK_CLOEXEC);
              accept_errno = errno;
              current_handlers = _handlers;
            }

            if (client < 0) {
              if (accept_errno != EINTR && accept_errno != EAGAIN && accept_errno != ECONNABORTED) {
                BOOST_LOG(error) << "Drain handoff: control socket accept failed: " << std::strerror(accept_errno);
              }
              if ((fds[0].revents & (POLLERR | POLLHUP)) != 0 || (accept_errno != EINTR && accept_errno != EAGAIN && accept_errno != ECONNABORTED)) {
                // Back off so a persistent error (for example out of descriptors) does not spin the thread.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
              }
              continue;
            }

            // Served without the lock: the handler may call close_listener(), reopen_listener() or stop().
            fd_guard_t client_guard(client);
            serve_connection(client, current_handlers);
          } catch (...) {
            // Never let an exception end the thread (std::terminate); the fd guards have closed the connection.
            BOOST_LOG(error) << "Drain handoff: control socket thread caught an exception";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
        }
      });
    } catch (const std::system_error &e) {
      ::unlink(path.c_str());
      return fail(std::string("cannot start the server thread: ") + e.what());
    }

    _listen_fd = listener.release();
    _wake_fd = wake.release();
    _bound_dev = id.first;
    _bound_ino = id.second;
    BOOST_LOG(info) << "Control socket listening";
    return true;
  }

  void control_server_t::close_listener() {
    std::lock_guard lock(_mutex);
    if (_listen_fd < 0) {
      return;
    }

    // Unlink only the socket this server bound: the path may have been replaced by another process since.
    struct stat st {};
    if (_bound_ino != 0 && ::lstat(_path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) && st.st_dev == _bound_dev && st.st_ino == _bound_ino) {
      if (::unlink(_path.c_str()) != 0 && errno != ENOENT) {
        BOOST_LOG(warning) << "Drain handoff: cannot remove the control socket: " << std::strerror(errno);
      }
    }
    _bound_dev = 0;
    _bound_ino = 0;

    ::close(_listen_fd);
    _listen_fd = -1;
    signal_wake(_wake_fd);
    BOOST_LOG(info) << "Drain handoff: control socket closed";
  }

  bool control_server_t::reopen_listener(std::string *error_out) {
    const auto fail = [error_out](const std::string &message) {
      if (error_out) {
        *error_out = message;
      }
      BOOST_LOG(error) << "Drain handoff: control socket: " << message;
      return false;
    };

    std::lock_guard lock(_mutex);
    if (_stopped || !_thread.joinable()) {
      return fail("the control server is not running");
    }
    if (_listen_fd >= 0) {
      return true;
    }

    std::string reason;
    int listen_fd = -1;
    file_id_t id {};
    if (!bind_listener(_path, listen_fd, id, reason)) {
      return fail(reason);
    }

    _listen_fd = listen_fd;
    _bound_dev = id.first;
    _bound_ino = id.second;
    signal_wake(_wake_fd);  // the thread polls the new listener once a running handler has returned
    BOOST_LOG(info) << "Control socket listening again";
    return true;
  }

  void control_server_t::stop() {
    {
      std::lock_guard lock(_mutex);
      _stopped = true;
    }
    close_listener();

    std::thread joinable;
    {
      std::lock_guard lock(_mutex);
      signal_wake(_wake_fd);
      // On the server thread (called from the handler) the thread cannot join itself: it exits after the handler
      // returns, and a later stop() or the destructor on another thread joins it.
      if (_thread.joinable() && _thread.get_id() != std::this_thread::get_id()) {
        joinable = std::move(_thread);
      }
    }

    if (joinable.joinable()) {
      joinable.join();  // never under the lock: the thread takes it
      std::lock_guard lock(_mutex);
      if (_wake_fd >= 0) {
        ::close(_wake_fd);
        _wake_fd = -1;
      }
    }
  }

  control_server_t::~control_server_t() {
    stop();
    std::lock_guard lock(_mutex);
    if (_wake_fd >= 0 && !_thread.joinable()) {
      ::close(_wake_fd);
      _wake_fd = -1;
    }
  }
}  // namespace handoff
