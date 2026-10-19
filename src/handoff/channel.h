/**
 * @file src/handoff/channel.h
 * @brief Transport of the drain-handoff channel: a `SOCK_SEQPACKET` socketpair with fd passing and request ids.
 */
#pragma once

// standard includes
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

// local includes
#include "protocol.h"

namespace handoff {
  /**
   * @brief Create the handoff channel socketpair (`AF_UNIX`, `SOCK_SEQPACKET | SOCK_CLOEXEC`).
   *
   * @return The two ends, or `std::nullopt` on failure (errno is kept).
   */
  std::optional<std::pair<int, int>> make_channel_pair();

  /**
   * @brief Outcome of a channel receive.
   */
  enum class recv_status_e {
    ok,  ///< A message was received and decoded.
    timeout,  ///< Nothing arrived within the timeout.
    eof,  ///< The peer closed its end.
    error,  ///< A socket error.
    oversized,  ///< The datagram exceeded `protocol::max_message_size` (it was truncated, `MSG_TRUNC`).
    malformed,  ///< The datagram did not decode (`protocol::decode_status_e::malformed`).
  };

  /**
   * @brief One received message and the fd attached to it, if any.
   */
  struct received_t {
    protocol::message_t message;  ///< Decoded message.
    int fd {-1};  ///< Fd received with `SCM_RIGHTS` (received with `MSG_CMSG_CLOEXEC`), or -1. The caller owns it.
  };

  /**
   * @brief One end of the handoff channel. Owns its fd.
   *
   * `send()` and `receive()` may be called from different threads at the same time; two concurrent senders are
   * serialized internally.
   */
  class channel_t {
  public:
    /**
     * @brief Take ownership of one end of the socketpair.
     *
     * @param fd Channel fd; -1 makes a closed channel.
     */
    explicit channel_t(int fd);

    /**
     * @brief Close the fd.
     */
    ~channel_t();

    channel_t(const channel_t &) = delete;  ///< Not copyable.
    channel_t &operator=(const channel_t &) = delete;  ///< Not copyable.

    /**
     * @brief Send one message, never blocking (`MSG_DONTWAIT | MSG_NOSIGNAL`).
     *
     * @param message Message to send.
     * @param attach_fd Fd to attach with `SCM_RIGHTS`, or -1. The caller keeps ownership.
     * @return `true` when the whole datagram was sent; `false` on `EAGAIN`, a closed channel, an encoding failure or
     *         any other error.
     */
    bool send(const protocol::message_t &message, int attach_fd = -1);

    /**
     * @brief Receive one message.
     *
     * Any received fd is close-on-exec. An fd that arrives with an oversized or malformed datagram is closed.
     *
     * @param out Receives the message and fd when the status is `ok`.
     * @param timeout Longest wait; zero polls once; negative waits without limit.
     * @return Receive status.
     */
    recv_status_e receive(received_t &out, std::chrono::milliseconds timeout);

    /**
     * @brief Close the channel. Later sends fail and later receives report `eof`. Safe to call twice.
     */
    void close();

    /**
     * @brief The channel fd.
     *
     * @return The fd, or -1 when closed.
     */
    int fd() const;

  private:
    int _fd;  ///< Channel fd, or -1.
    std::mutex _send_mutex;  ///< Serializes senders.
  };

  /**
   * @brief Matches replies to requests by request id; drops late and unknown replies.
   *
   * Used by the active process for `SESSIONS?`, `CLOSE`, `CLOSE_CERT` and `END_ALL`. Thread-safe.
   */
  class request_tracker_t {
  public:
    /**
     * @brief Allocate a new request id and start expecting its reply.
     *
     * @return The id; ids start at 1 and increase.
     */
    std::uint64_t begin();

    /**
     * @brief Deliver a reply that the channel reader received.
     *
     * @param request_id Id echoed by the reply.
     * @param reply Reply message.
     * @return `true` when a caller is waiting for that id (or will collect it); `false` when the id is unknown, was
     *         already answered, or its waiter has timed out (a late reply, dropped).
     */
    bool deliver(std::uint64_t request_id, protocol::message_t reply);

    /**
     * @brief Wait for the reply to a request started with `begin()`.
     *
     * After the call returns the id is forgotten, whatever the outcome, so a later reply is dropped.
     *
     * @param request_id Id from `begin()`.
     * @param timeout Longest wait.
     * @return The reply, or `std::nullopt` on timeout, an unknown id, or after `fail_all()`.
     */
    std::optional<protocol::message_t> wait(std::uint64_t request_id, std::chrono::milliseconds timeout);

    /**
     * @brief Wake every waiter with no reply (the channel was lost). Later `begin()` calls still work.
     */
    void fail_all();

  private:
    std::mutex _mutex;  ///< Guards the members below.
    std::condition_variable _cv;  ///< Signals delivered replies.
    std::uint64_t _next_id {1};  ///< Next id.
    std::uint64_t _fail_generation {0};  ///< Increased by `fail_all()`.
    std::map<std::uint64_t, std::optional<protocol::message_t>> _pending;  ///< Expected ids and their replies.
  };
}  // namespace handoff
