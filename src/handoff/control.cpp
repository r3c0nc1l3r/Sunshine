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
#include <fstream>
#include <poll.h>
#include <sstream>
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

    /**
     * @brief Handle one accepted connection on the server thread.
     *
     * @param fd Accepted socket; the caller closes it.
     * @param handler Verb handler.
     */
    void serve_connection(int fd, const control_server_t::handler_t &handler) {
      ucred cred {};
      socklen_t cred_len = sizeof(cred);
      if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0) {
        BOOST_LOG(error) << "Drain handoff: cannot read control socket peer credentials: " << std::strerror(errno);
        return;
      }

      if (cred.uid != ::geteuid()) {
        BOOST_LOG(warning) << "Drain handoff: refused control connection from another user (pid " << cred.pid << ")";
        write_all(fd, encode_reply(control_reply_t {reply_kind_e::refused, "peer uid"}), server_io_timeout);
        return;
      }
      BOOST_LOG(info) << "Drain handoff: control connection from pid " << cred.pid;

      const auto line = read_line(fd, server_io_timeout, max_request_bytes);
      std::string reply;
      switch (line.status) {
        case line_result_t::status_e::line:
          if (line.text != std::string(handoff_verb) + "\n") {
            reply = encode_reply(control_reply_t {reply_kind_e::refused, "unknown verb"});
            break;
          }
          try {
            reply = encode_reply(handler(cred.pid));
          } catch (...) {
            BOOST_LOG(error) << "Drain handoff: handler failed for the handoff verb";
            reply = encode_reply(control_reply_t {reply_kind_e::refused, "internal error"});
          }
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
     * @brief Server thread body: accept and serve one connection at a time until the wake fd is signalled.
     *
     * @param listen_fd Listening socket.
     * @param wake_fd Eventfd written by `stop()`.
     * @param handler Verb handler.
     */
    void serve_loop(int listen_fd, int wake_fd, control_server_t::handler_t handler) {
      while (true) {
        pollfd fds[2] = {{listen_fd, POLLIN, 0}, {wake_fd, POLLIN, 0}};
        if (::poll(fds, 2, -1) < 0) {
          if (errno == EINTR) {
            continue;
          }
          BOOST_LOG(error) << "Drain handoff: control socket poll failed: " << std::strerror(errno);
          return;
        }

        if (fds[1].revents != 0) {
          return;
        }

        if (fds[0].revents != 0) {
          const int client = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
          if (client < 0) {
            if (errno != EINTR && errno != EAGAIN && errno != ECONNABORTED) {
              BOOST_LOG(error) << "Drain handoff: control socket accept failed: " << std::strerror(errno);
              // Back off so a persistent error (for example out of descriptors) does not spin the thread.
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
          }

          fd_guard_t guard(client);
          serve_connection(client, handler);
        }
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
          return "kill pending";
        case reply_kind_e::committed:
          return "committed";
      }
      return "refused";
    }
  }  // namespace

  fs::path control_socket_path(const fs::path &runtime_dir, pid_t pid) {
    return runtime_dir / "control" / (std::to_string(pid) + ".sock");
  }

  std::string encode_reply(const control_reply_t &reply) {
    if (reply.kind == reply_kind_e::committed) {
      return "committed " + std::to_string(reply.new_pid) + " " + std::to_string(reply.new_start_time) + "\n";
    }

    std::string text = reply.reason;
    std::replace(text.begin(), text.end(), '\n', ' ');
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
    // kill pending); otherwise there is simply no socket.
    const auto socket_path = control_socket_path(env.runtime_dir, *pid);
    struct stat st {};
    if (::stat(socket_path.c_str(), &st) != 0 && errno == ENOENT) {
      std::ifstream in(env.runtime_dir / "instances" / (std::to_string(*pid) + ".json"), std::ios::binary);
      if (in) {
        std::ostringstream content;
        content << in.rdbuf();
        const auto instance = parse_instance(content.str());
        if (instance && instance->role == role_e::draining) {
          err = "a drain is in progress";
          return helper_exit_e::refused;
        }
        if (instance && instance->handoff == state_e::handover_failed) {
          err = "handover failing";
          return helper_exit_e::refused;
        }
        if (instance && instance->handoff == state_e::kill_pending) {
          err = "kill pending";
          return helper_exit_e::refused;
        }
      }
      err = no_socket;
      return helper_exit_e::refused;
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
        err = no_socket;
        return helper_exit_e::refused;
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

  bool control_server_t::start(const fs::path &path, handler_t handler, std::string *error_out) {
    const auto fail = [error_out](const std::string &message) {
      if (error_out) {
        *error_out = message;
      }
      BOOST_LOG(error) << "Drain handoff: control socket: " << message;
      return false;
    };

    if (_listen_fd >= 0) {
      return fail("already started");
    }

    // The control directory is private to the user: created or fixed to mode 0700.
    const auto dir = path.parent_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
      return fail("cannot create the control directory: " + ec.message());
    }
    if (::chmod(dir.c_str(), 0700) != 0) {
      return fail(std::string("cannot set the control directory mode: ") + std::strerror(errno));
    }

    // Replace a stale socket left by a previous run; never remove anything else.
    struct stat st {};
    if (::lstat(path.c_str(), &st) == 0) {
      if (!S_ISSOCK(st.st_mode)) {
        return fail("the socket path exists and is not a socket");
      }
      ::unlink(path.c_str());
    } else if (errno != ENOENT) {
      return fail(std::string("cannot inspect the socket path: ") + std::strerror(errno));
    }

    sockaddr_un addr {};
    if (!make_address(path, addr)) {
      return fail("socket path too long");
    }

    fd_guard_t listener(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (listener.get() < 0) {
      return fail(std::string("socket: ") + std::strerror(errno));
    }

    // umask 0177 makes the socket file 0600 at bind time; the chmod below makes it explicit.
    const auto old_umask = ::umask(0177);
    const int bind_rc = ::bind(listener.get(), reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
    const int bind_errno = errno;
    ::umask(old_umask);
    if (bind_rc != 0) {
      return fail(std::string("bind: ") + std::strerror(bind_errno));
    }
    if (::chmod(path.c_str(), 0600) != 0 || ::listen(listener.get(), 8) != 0) {
      const auto message = std::string("listen or chmod: ") + std::strerror(errno);
      ::unlink(path.c_str());
      return fail(message);
    }

    fd_guard_t wake(::eventfd(0, EFD_CLOEXEC));
    if (wake.get() < 0) {
      const auto message = std::string("eventfd: ") + std::strerror(errno);
      ::unlink(path.c_str());
      return fail(message);
    }

    try {
      _thread = std::thread(serve_loop, listener.get(), wake.get(), std::move(handler));
    } catch (const std::system_error &e) {
      ::unlink(path.c_str());
      return fail(std::string("cannot start the server thread: ") + e.what());
    }

    // The thread now uses these descriptors; stop() closes them.
    _path = path;
    _listen_fd = listener.release();
    _wake_fd = wake.release();
    BOOST_LOG(info) << "Drain handoff: control socket listening";
    return true;
  }

  void control_server_t::stop() {
    if (_thread.joinable()) {
      const std::uint64_t one = 1;
      if (::write(_wake_fd, &one, sizeof(one)) < 0) {
        BOOST_LOG(error) << "Drain handoff: cannot wake the control socket thread: " << std::strerror(errno);
      }
      _thread.join();
    }

    if (_listen_fd >= 0) {
      ::close(_listen_fd);
      _listen_fd = -1;
      ::unlink(_path.c_str());
    }
    if (_wake_fd >= 0) {
      ::close(_wake_fd);
      _wake_fd = -1;
    }
  }

  control_server_t::~control_server_t() {
    stop();
  }
}  // namespace handoff
