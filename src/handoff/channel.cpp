/**
 * @file src/handoff/channel.cpp
 * @brief Definitions for `src/handoff/channel.h`.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstring>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

// local includes
#include "channel.h"

namespace handoff {
  namespace {
    /// Most fds taken from one datagram's control data; any beyond the first are closed.
    constexpr std::size_t max_received_fds = 8;

    /**
     * @brief Closed channel marker: a closed channel stores `-2 - fd` (its fd stays open but shut down until the
     * destructor, so no thread can ever hit a reused fd number).
     *
     * @param fd Open fd.
     * @return The stored value for a closed channel.
     */
    int closed_value(int fd) {
      return -2 - fd;
    }
  }  // namespace

  std::optional<std::pair<int, int>> make_channel_pair() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) != 0) {
      return std::nullopt;
    }
    return std::pair<int, int> {fds[0], fds[1]};
  }

  channel_t::channel_t(int fd):
      _fd(fd < 0 ? -1 : fd) {
  }

  channel_t::~channel_t() {
    // No other thread may use the channel any more, so the plain read is safe.
    if (_fd >= 0) {
      ::close(_fd);
    } else if (_fd < -1) {
      ::close(closed_value(_fd));
    }
  }

  bool channel_t::send(const protocol::message_t &message, int attach_fd) {
    std::string payload = protocol::encode(message);
    if (payload.empty() || payload.size() > protocol::max_message_size) {
      errno = EMSGSIZE;
      return false;
    }

    std::lock_guard lock {_send_mutex};
    const int fd = std::atomic_ref<int> {_fd}.load();
    if (fd < 0) {
      errno = EBADF;
      return false;
    }

    iovec iov {};
    iov.iov_base = payload.data();
    iov.iov_len = payload.size();

    msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] {};
    if (attach_fd >= 0) {
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
      sent = ::sendmsg(fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    return sent >= 0 && static_cast<std::size_t>(sent) == payload.size();
  }

  recv_status_e channel_t::receive(received_t &out, std::chrono::milliseconds timeout) {
    using clock = std::chrono::steady_clock;
    const bool unlimited = timeout.count() < 0;
    const auto deadline = clock::now() + (unlimited ? std::chrono::milliseconds {0} : timeout);

    std::vector<char> buffer(protocol::max_message_size + 1);

    while (true) {
      const int fd = std::atomic_ref<int> {_fd}.load();
      if (fd < 0) {
        return recv_status_e::eof;
      }

      int wait_ms = -1;
      if (!unlimited) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now() + std::chrono::microseconds {999});
        wait_ms = static_cast<int>(std::clamp<long long>(remaining.count(), 0, INT_MAX));
      }

      pollfd pfd {};
      pfd.fd = fd;
      pfd.events = POLLIN;
      const int ready = ::poll(&pfd, 1, wait_ms);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        return recv_status_e::error;
      }
      if (ready == 0) {
        return recv_status_e::timeout;
      }
      if (pfd.revents & POLLNVAL) {
        return recv_status_e::error;
      }
      // A close while we waited: the fd is shut down but still open, so check the flag before reading.
      if (std::atomic_ref<int> {_fd}.load() < 0) {
        return recv_status_e::eof;
      }

      iovec iov {};
      iov.iov_base = buffer.data();
      iov.iov_len = buffer.size();

      alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * max_received_fds)] {};
      msghdr msg {};
      msg.msg_iov = &iov;
      msg.msg_iovlen = 1;
      msg.msg_control = control;
      msg.msg_controllen = sizeof(control);

      const ssize_t n = ::recvmsg(fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
      if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }

      // Collect every received fd: keep the first, close the rest and, on any non-ok result, close the first too.
      int received_fd = -1;
      if (n >= 0) {
        for (cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
          if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            continue;
          }
          const std::size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
          for (std::size_t i = 0; i < count; ++i) {
            int one;
            std::memcpy(&one, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
            if (received_fd < 0) {
              received_fd = one;
            } else {
              ::close(one);
            }
          }
        }
      }
      auto fail = [&received_fd](recv_status_e status) {
        if (received_fd >= 0) {
          ::close(received_fd);
          received_fd = -1;
        }
        return status;
      };

      if (n < 0) {
        return errno == ECONNRESET ? recv_status_e::eof : recv_status_e::error;
      }
      if (n == 0) {
        return fail(recv_status_e::eof);
      }
      if ((msg.msg_flags & MSG_TRUNC) || static_cast<std::size_t>(n) > protocol::max_message_size) {
        return fail(recv_status_e::oversized);
      }

      protocol::message_t decoded;
      switch (protocol::decode(std::string_view {buffer.data(), static_cast<std::size_t>(n)}, decoded)) {
        case protocol::decode_status_e::ok:
          break;
        case protocol::decode_status_e::oversized:
          return fail(recv_status_e::oversized);
        case protocol::decode_status_e::malformed:
          return fail(recv_status_e::malformed);
      }
      out.message = std::move(decoded);
      out.fd = received_fd;
      return recv_status_e::ok;
    }
  }

  void channel_t::close() {
    std::atomic_ref<int> ref {_fd};
    int fd = ref.load();
    // Only the first close wins. The fd is shut down, which wakes a blocked receive and shows the peer end-of-file,
    // but it is closed only in the destructor: closing it here could let a concurrent receive or send hit a fd number
    // that was reused.
    while (fd >= 0) {
      if (ref.compare_exchange_weak(fd, closed_value(fd))) {
        ::shutdown(fd, SHUT_RDWR);
        return;
      }
    }
  }

  int channel_t::fd() const {
    const int fd = std::atomic_ref<int> {const_cast<int &>(_fd)}.load();
    return fd < 0 ? -1 : fd;
  }
}  // namespace handoff
