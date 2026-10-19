/**
 * @file src/handoff/new_side.cpp
 * @brief Definitions for `src/handoff/new_side.h`.
 */
// standard includes
#include <algorithm>
#include <unistd.h>
#include <variant>

// local includes
#include "new_side.h"
#include "src/logging.h"

namespace handoff {
  namespace {
    using steady_t = std::chrono::steady_clock;

    /**
     * @brief Result of waiting for one wanted message.
     */
    enum class wait_result_e {
      got,  ///< The wanted message arrived in time.
      failed,  ///< Timeout, close, error, malformed or oversized message, or a late read.
    };

    /**
     * @brief Close the fd of a received message, if any.
     *
     * @param received Message with its fd.
     */
    void drop_fd(received_t &received) {
      if (received.fd >= 0) {
        ::close(received.fd);
        received.fd = -1;
      }
    }

    /**
     * @brief Wait until a message of type `T` arrives, answering `PING` with `PONG` and ignoring everything else.
     *
     * @tparam T Wanted message type.
     * @param channel Channel.
     * @param deadline Latest time (monotonic) the message may be read.
     * @param what Name for the log.
     * @param out Receives the message and its fd.
     * @return Result.
     */
    template<typename T>
    wait_result_e wait_for(channel_t &channel, steady_t::time_point deadline, const char *what, received_t &out) {
      for (;;) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - steady_t::now());
        if (remaining.count() <= 0) {
          BOOST_LOG(error) << "Drain handoff (new process): timeout waiting for " << what;
          return wait_result_e::failed;
        }
        received_t received;
        auto status = channel.receive(received, remaining);
        if (status == recv_status_e::timeout) {
          BOOST_LOG(error) << "Drain handoff (new process): timeout waiting for " << what;
          return wait_result_e::failed;
        }
        if (status != recv_status_e::ok) {
          BOOST_LOG(error) << "Drain handoff (new process): channel lost while waiting for " << what;
          return wait_result_e::failed;
        }
        if (std::holds_alternative<T>(received.message)) {
          if (steady_t::now() > deadline) {
            // Read after the bound: the old process may already have rolled back.
            drop_fd(received);
            BOOST_LOG(error) << "Drain handoff (new process): " << what << " arrived after its deadline";
            return wait_result_e::failed;
          }
          out = std::move(received);
          return wait_result_e::got;
        }
        if (auto *ping = std::get_if<protocol::ping_t>(&received.message)) {
          if (!channel.send(protocol::pong_t {ping->seq})) {
            BOOST_LOG(warning) << "Drain handoff (new process): could not send PONG " << ping->seq;
          }
        }
        if (!std::holds_alternative<protocol::unknown_t>(received.message)) {
          BOOST_LOG(warning) << "Drain handoff (new process): dropping " << protocol::type_name(received.message) << " received out of order while waiting for " << what;
        }
        drop_fd(received);  // Unknown or unexpected messages are ignored.
      }
    }

    /**
     * @brief Send a failed `READY`.
     *
     * @param channel Channel.
     * @param reason Reason.
     */
    void send_failed_ready(channel_t &channel, const std::string &reason) {
      BOOST_LOG(error) << "Drain handoff (new process): start failed: " << reason;
      channel.send(protocol::ready_t {false, reason});
    }
  }  // namespace

  new_outcome_e run_new_side(new_side_ops_t &ops, channel_t &channel, const new_side_timeouts_t &timeouts) {
    // The old process's HELLO, with its pidfd.
    received_t old_hello;
    if (wait_for<protocol::hello_t>(channel, steady_t::now() + timeouts.hello, "HELLO", old_hello) != wait_result_e::got) {
      return new_outcome_e::exit_failure;
    }
    const auto &hello = std::get<protocol::hello_t>(old_hello.message);
    if (hello.major != protocol::version_major) {
      drop_fd(old_hello);
      send_failed_ready(channel, "unsupported protocol major version");
      return new_outcome_e::exit_failure;
    }
    if (old_hello.fd < 0) {
      send_failed_ready(channel, "the HELLO of the old process carries no pidfd");
      return new_outcome_e::exit_failure;
    }
    ops.accept_old_hello(hello, old_hello.fd);  // The ops own the pidfd from here on.
    old_hello.fd = -1;
    if (!channel.send(ops.hello())) {
      BOOST_LOG(error) << "Drain handoff (new process): cannot send HELLO";
      return new_outcome_e::exit_failure;
    }

    if (auto reason = ops.validate_listeners(); !reason.empty()) {
      send_failed_ready(channel, reason);
      return new_outcome_e::exit_failure;
    }
    if (auto reason = ops.start_servers_paused(); !reason.empty()) {
      send_failed_ready(channel, reason);
      return new_outcome_e::exit_failure;
    }
    auto ready_sent = steady_t::now();
    if (!channel.send(protocol::ready_t {true, {}})) {
      BOOST_LOG(error) << "Drain handoff (new process): cannot send READY";
      return new_outcome_e::exit_failure;
    }

    // GO.
    received_t go_message;
    if (wait_for<protocol::go_t>(channel, ready_sent + timeouts.go, "GO", go_message) != wait_result_e::got) {
      return new_outcome_e::exit_failure;
    }
    const auto &go = std::get<protocol::go_t>(go_message.message);
    drop_fd(go_message);
    std::string reason = ops.reload_all();
    if (reason.empty()) {
      reason = ops.prepare_adoption(go);
    }
    auto go_ack_sent = steady_t::now();
    if (!reason.empty()) {
      BOOST_LOG(error) << "Drain handoff (new process): GO failed: " << reason;
      channel.send(protocol::go_ack_t {false, reason});
      return new_outcome_e::exit_failure;
    }
    if (!channel.send(protocol::go_ack_t {true, {}})) {
      BOOST_LOG(error) << "Drain handoff (new process): cannot send GO_ACK";
      return new_outcome_e::exit_failure;
    }

    // COMMIT.
    received_t commit_message;
    if (wait_for<protocol::commit_t>(channel, go_ack_sent + timeouts.commit, "COMMIT", commit_message) != wait_result_e::got) {
      return new_outcome_e::exit_failure;
    }
    drop_fd(commit_message);
    ops.install_adoption();
    ops.resume_accept();
    ops.publish_mdns();
    ops.map_upnp();
    ops.start_tray();
    ops.become_active();
    return new_outcome_e::committed;
  }
}  // namespace handoff
