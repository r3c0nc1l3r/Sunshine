/**
 * @file src/handoff/peer_link.cpp
 * @brief Implementation of `src/handoff/peer_link.h` (wave C).
 */
#include "peer_link.h"

// standard includes
#include <cerrno>
#include <utility>

// lib includes
#include <poll.h>
#include <unistd.h>

// local includes
#include "src/crypto.h"

namespace handoff {
  namespace {
    /**
     * @brief Whether a string is exactly 64 lowercase hexadecimal characters.
     *
     * @param text Text to check.
     * @return `true` for a well-formed SHA-256 hex string.
     */
    bool is_sha256_hex(std::string_view text) {
      if (text.size() != 64) {
        return false;
      }
      for (const char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        if (!digit && !lower) {
          return false;
        }
      }
      return true;
    }
  }  // namespace

  std::string cert_sha256_hex(std::string_view cert) {
    static constexpr char digits[] = "0123456789abcdef";
    const auto hash = crypto::hash(cert);
    std::string out;
    out.reserve(hash.size() * 2);
    for (const auto byte : hash) {
      out.push_back(digits[byte >> 4]);
      out.push_back(digits[byte & 0x0f]);
    }
    return out;
  }

  peer_link_t::peer_link_t(std::shared_ptr<channel_t> channel, int pidfd):
      _channel(std::move(channel)),
      _pidfd(pidfd) {
  }

  peer_link_t::~peer_link_t() {
    _tracker.fail_all();
    if (_pidfd >= 0) {
      ::close(_pidfd);
      _pidfd = -1;
    }
  }

  bool peer_link_t::exists() {
    return !gone();
  }

  bool peer_link_t::gone() {
    if (_gone.load()) {
      return true;
    }
    if (_pidfd < 0) {
      return false;
    }

    pollfd entry {};
    entry.fd = _pidfd;
    entry.events = POLLIN;
    int result;
    do {
      result = ::poll(&entry, 1, 0);
    } while (result < 0 && errno == EINTR);

    if (result > 0 && (entry.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      _gone.store(true);
      return true;
    }
    return false;
  }

  namespace {
    /**
     * @brief One request of the common flow: steps (2) to (5) of the class comment.
     *
     * @tparam Make Callable `(std::uint64_t id) -> protocol::message_t`.
     * @param tracker Request tracker.
     * @param channel Channel, may be null.
     * @param closed Whether `on_channel_closed` ran.
     * @param timeout Longest wait.
     * @param make Builds the request for a fresh id.
     * @return The reply, or `std::nullopt` when there was none (the caller then checks `gone()`).
     */
    template<typename Make>
    std::optional<protocol::message_t> run_request(request_tracker_t &tracker, const std::shared_ptr<channel_t> &channel, const std::atomic<bool> &closed, std::chrono::milliseconds timeout, Make &&make) {
      if (timeout <= std::chrono::milliseconds::zero()) {
        return std::nullopt;
      }

      const auto id = tracker.begin();
      if (closed.load() || !channel || !channel->send(make(id))) {
        // Nothing was sent (or nobody will answer): forget the id at once.
        tracker.wait(id, std::chrono::milliseconds::zero());
        return std::nullopt;
      }
      return tracker.wait(id, timeout);
    }
  }  // namespace

  peer_link_t::reply_e peer_link_t::end_all(std::chrono::milliseconds timeout) {
    const auto reply = run_request(_tracker, _channel, _closed, timeout, [](std::uint64_t id) {
      return protocol::message_t {protocol::end_all_t {id}};
    });
    if (reply) {
      if (std::holds_alternative<protocol::ended_t>(*reply)) {
        return reply_e::ended;
      }
      if (std::holds_alternative<protocol::end_rejected_t>(*reply)) {
        return reply_e::rejected;
      }
    }
    return reply_e::no_reply;
  }

  std::optional<std::vector<protocol::session_entry_t>> peer_link_t::list(std::chrono::milliseconds timeout) {
    const auto reply = run_request(_tracker, _channel, _closed, timeout, [](std::uint64_t id) {
      return protocol::message_t {protocol::sessions_request_t {id}};
    });
    if (reply) {
      if (const auto *sessions = std::get_if<protocol::sessions_reply_t>(&*reply)) {
        return sessions->sessions;
      }
    }
    if (gone()) {
      return std::vector<protocol::session_entry_t> {};
    }
    return std::nullopt;
  }

  std::optional<rtsp_stream::close_result_e> peer_link_t::close(std::uint32_t id, std::chrono::milliseconds timeout) {
    const auto reply = run_request(_tracker, _channel, _closed, timeout, [id](std::uint64_t request_id) {
      return protocol::message_t {protocol::close_request_t {request_id, id}};
    });
    if (reply) {
      if (const auto *closed = std::get_if<protocol::close_reply_t>(&*reply)) {
        if (closed->result == "closed") {
          return rtsp_stream::close_result_e::closed;
        }
        if (closed->result == "already-stopping") {
          return rtsp_stream::close_result_e::already_stopping;
        }
        if (closed->result == "not-found") {
          return rtsp_stream::close_result_e::not_found;
        }
        // An unknown result string counts as no reply.
      }
    }
    if (gone()) {
      return rtsp_stream::close_result_e::not_found;
    }
    return std::nullopt;
  }

  std::optional<std::uint32_t> peer_link_t::close_cert(std::string_view cert_sha256, std::chrono::milliseconds timeout) {
    if (!is_sha256_hex(cert_sha256)) {
      return std::nullopt;
    }

    const std::string hash {cert_sha256};
    const auto reply = run_request(_tracker, _channel, _closed, timeout, [&hash](std::uint64_t id) {
      return protocol::message_t {protocol::close_cert_request_t {id, hash}};
    });
    if (reply) {
      if (const auto *closed = std::get_if<protocol::close_cert_reply_t>(&*reply)) {
        return closed->count;
      }
    }
    if (gone()) {
      return std::uint32_t {0};
    }
    return std::nullopt;
  }

  state_e peer_link_t::state() {
    if (gone()) {
      return state_e::none;
    }
    return static_cast<state_e>(_state.load());
  }

  bool peer_link_t::deliver(const protocol::message_t &message) {
    // Replies carry the request id of the request they answer.
    const auto route = [&](const auto &reply) {
      return _tracker.deliver(reply.request_id, message);
    };

    if (const auto *r = std::get_if<protocol::sessions_reply_t>(&message)) {
      return route(*r);
    }
    if (const auto *r = std::get_if<protocol::close_reply_t>(&message)) {
      return route(*r);
    }
    if (const auto *r = std::get_if<protocol::close_cert_reply_t>(&message)) {
      return route(*r);
    }
    if (const auto *r = std::get_if<protocol::ended_t>(&message)) {
      return route(*r);
    }
    if (const auto *r = std::get_if<protocol::end_rejected_t>(&message)) {
      return route(*r);
    }
    if (const auto *r = std::get_if<protocol::handoff_state_t>(&message)) {
      if (const auto parsed = parse_state(r->state)) {
        _state.store(static_cast<int>(*parsed));
      }
      return true;
    }
    return false;
  }

  void peer_link_t::on_channel_closed() {
    // Set the flag first: a request that begins after this fails at its closed check, one that began before is woken.
    _closed.store(true);
    _tracker.fail_all();
  }
}  // namespace handoff
