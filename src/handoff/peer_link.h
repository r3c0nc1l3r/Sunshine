/**
 * @file src/handoff/peer_link.h
 * @brief The active process's link to its draining peer: the `drain_peer_t` of the terminate rule, plus list, close and
 * close-by-certificate requests over the handoff channel.
 *
 * See `docs/drain-handoff.md`, "Terminate rule", "Visibility and control" and "The handoff channel". Linux only: include
 * it only from Linux code or behind `#ifdef __linux__` (it pulls in the channel and the pidfd handling).
 *
 * Wiring (done by the new-side glue, P18): after `COMMIT` the new process creates one `peer_link_t` from the channel
 * and the old process's pidfd (received with `HELLO`), stores it with `runtime().set_peer(link)`, and starts a
 * `channel_reader_t(channel, answer_pings=true, ...)` whose message handler calls `link->deliver(received.message)` for
 * every message it does not handle itself (`MAIN` and the like), and whose close handler calls
 * `link->on_channel_closed()`. Requirement on that glue (P18): it MUST call `on_channel_closed()` when the channel reports
 * EOF (or any final status), and also when the pidfd becomes readable (the draining process is gone or a zombie), so
 * that callers waiting in `end_all`, `list`, `close` or `close_cert` fail promptly instead of running to their timeout.
 * The link does not watch the pidfd while a request waits. The link never reads the channel itself; replies reach it only through `deliver`. The
 * link is destroyed (or `runtime().set_peer(nullptr)`) after the draining process is gone and no request is in flight;
 * callers copy the `shared_ptr` from `runtime().peer()` for the duration of a call, which keeps it alive.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "channel.h"
#include "end_all.h"
#include "instance_file.h"
#include "protocol.h"
#include "src/rtsp.h"

namespace handoff {
  constexpr std::chrono::milliseconds peer_request_timeout {500};  ///< Timeout of `SESSIONS?`, `CLOSE` and `CLOSE_CERT` (spec: 500 ms).

  /**
   * @brief Lowercase hexadecimal SHA-256 of a client certificate, as `CLOSE_CERT` carries it.
   *
   * It is the lowercase hex of `crypto::hash(cert)` over the exact bytes of the string, the same hash that
   * `rtsp_stream::terminate_sessions_by_cert_hash` compares, so the two processes agree. Callers pass the PEM string
   * they also pass to `rtsp_stream::terminate_sessions_by_cert`.
   *
   * @param cert Certificate PEM string.
   * @return 64 lowercase hex characters (the hash of an empty string for an empty input).
   */
  std::string cert_sha256_hex(std::string_view cert);

  /**
   * @brief The draining peer with the requests the web UI and the update-client path need, as an interface so tests can
   * fake it.
   *
   * `runtime().set_peer()` stores it as a `drain_peer_t`; code that needs more than the terminate rule casts with
   * `std::dynamic_pointer_cast<session_peer_t>(runtime().peer())`. A peer that is only a `drain_peer_t` (a test double)
   * has no session knowledge: the callers in `terminate.h` then treat the session count as unknown.
   *
   * Every request is independent: its own request id, its own timeout, no ordering between concurrent requests, and
   * several may be in flight at once from different threads (for example a `list` while a `close_cert` runs). Replies
   * are matched by id, so they may arrive in any order. Every request returns without waiting when it cannot be sent
   * (the channel was closed, `on_channel_closed` was called, or the send failed with `EAGAIN`): the result is then the
   * "no answer" value of that function. A request never retries and never queues.
   */
  class session_peer_t: public drain_peer_t {
  public:
    /**
     * @brief List the draining process's sessions (`SESSIONS?`).
     *
     * @param timeout Longest wait for the reply.
     * @return The sessions, possibly an empty vector (the draining process answered with none, or it is gone: a gone
     *         process has no sessions). `std::nullopt` when there was no reply in time (timeout, lost channel, failed
     *         send) and the peer is not gone: the count is unknown.
     */
    virtual std::optional<std::vector<protocol::session_entry_t>> list(std::chrono::milliseconds timeout) = 0;

    /**
     * @brief Close one draining session (`CLOSE <id>`).
     *
     * The wire result maps one to one: `closed` to `closed`, `already-stopping` to `already_stopping`, `not-found` to
     * `not_found`.
     *
     * @param id Launch session id.
     * @param timeout Longest wait for the reply.
     * @return The result. `not_found` when the peer is gone (it has no session). `std::nullopt` when there was no reply
     *         in time and the peer is not gone.
     */
    virtual std::optional<rtsp_stream::close_result_e> close(std::uint32_t id, std::chrono::milliseconds timeout) = 0;

    /**
     * @brief End the draining sessions of one client (`CLOSE_CERT <hash>`).
     *
     * @param cert_sha256 Lowercase hex SHA-256 from `cert_sha256_hex`. A value that is not exactly 64 lowercase hex
     *        characters is not sent and gives `std::nullopt`.
     * @param timeout Longest wait for the reply. The draining process ends and joins the sessions before it replies
     *        (its handler runs on its own thread), so the reply can take longer than 500 ms; a caller that only needs
     *        the request delivered may use a short timeout and ignore `std::nullopt`; the draining process finishes the
     *        request anyway.
     * @return The number of sessions the draining process ended. 0 when the peer is gone. `std::nullopt` when there was
     *         no reply in time and the peer is not gone.
     */
    virtual std::optional<std::uint32_t> close_cert(std::string_view cert_sha256, std::chrono::milliseconds timeout) = 0;

    /**
     * @brief The handoff state the draining process last announced (`HANDOFF_STATE`).
     *
     * @return `state_e::none` until the first `HANDOFF_STATE` arrives and once the peer is gone; otherwise the last one
     *         received through `deliver`. Never blocks.
     */
    virtual state_e state() = 0;
  };

  /**
   * @brief `session_peer_t` over the handoff channel, the old process's pidfd and a `request_tracker_t`.
   *
   * Thread-safety: every member may be called from any thread at the same time, except where noted. No member holds a
   * lock of this object across a wait or a send; the only waits are `request_tracker_t::wait` calls. No member may be
   * called from the channel reader's handler thread (`deliver` and `on_channel_closed` are the members meant for it): a
   * request waits for a reply that only that thread can deliver.
   *
   * Pidfd: `gone()` is `poll(pidfd, POLLIN)` with a zero timeout: the draining process is gone or a zombie when the fd
   * is readable (`POLLIN`, `POLLHUP` or `POLLERR`). It is sticky: once true it stays true. Any pollable fd behaves the
   * same, so tests pass an `eventfd` or a pipe's read end and make it readable to "kill" the peer. A pidfd of -1 can
   * never show "gone": `gone()` is `false` and `exists()` is `true`, so the terminate rule never terminates on a guess.
   *
   * Peer disappearing mid-request: a draining process that exits closes its channel end, so the reader sees `eof`,
   * the glue calls `on_channel_closed()` and every waiting request wakes at once. After a wait that returned without a
   * reply each request checks `gone()` once: gone gives the "gone" result of that member (empty list, `not_found`, 0
   * sessions, `END_ALL` `no_reply`, which the terminate rule then reads as `peer_gone`); not gone gives the "unknown"
   * result (`std::nullopt`, `no_reply`). The members never loop or retry on their own.
   */
  class peer_link_t final: public session_peer_t {
  public:
    /**
     * @brief Create the link.
     *
     * @param channel The active process's end of the channel; shared with the channel reader. May be null (every request
     *        then fails without waiting).
     * @param pidfd The draining process's pidfd (or a test fd). The link owns it and closes it in the destructor.
     *        -1 is allowed (see class comment).
     */
    peer_link_t(std::shared_ptr<channel_t> channel, int pidfd);

    /**
     * @brief Close the pidfd and wake every waiting request (`fail_all`). Callers must not be inside a member when the
     * object is destroyed.
     */
    ~peer_link_t() override;

    peer_link_t(const peer_link_t &) = delete;  ///< Not copyable.
    peer_link_t &operator=(const peer_link_t &) = delete;  ///< Not copyable.

    /**
     * @brief Whether a draining process exists: from construction until the pidfd shows it gone.
     *
     * @return `!gone()`. Never blocks (one zero-timeout poll); safe in `hooks::restart_blocked()`.
     */
    bool exists() override;

    /**
     * @brief Whether the draining process is gone or a zombie.
     *
     * @return `true` when the pidfd is readable (sticky). Never blocks.
     */
    bool gone() override;

    /**
     * @brief Send `END_ALL` and wait for the reply.
     *
     * With `timeout <= 0` nothing is sent and the result is `no_reply`. Otherwise one `END_ALL` with a fresh request id
     * is sent; the call waits up to `timeout` for the reply with that id: `ENDED` gives `ended`, `END_REJECTED` gives
     * `rejected`, and a wrong-typed reply, a timeout, a lost channel (`on_channel_closed`), a failed send, or a send on a
     * closed channel give `no_reply`. A late reply after a timeout is dropped by the tracker. Two concurrent calls send
     * two requests; the draining process answers both with the same result (its `end_all_machine_t` makes the second
     * wait for the first).
     *
     * @param timeout Longest wait.
     * @return Outcome. It does not look at the pidfd: the terminate rule (`await_terminate_permission`) polls `gone()`
     *         itself after a `no_reply` or `rejected`.
     */
    reply_e end_all(std::chrono::milliseconds timeout) override;

    /**
     * @brief `session_peer_t::list`: sends `SESSIONS?`; a `SESSIONS` reply with the request id gives its entries.
     *
     * A reply of another type for that id counts as no reply. Not gone and no reply: `std::nullopt`; gone: empty vector.
     *
     * @param timeout Longest wait.
     * @return See `session_peer_t::list`.
     */
    std::optional<std::vector<protocol::session_entry_t>> list(std::chrono::milliseconds timeout) override;

    /**
     * @brief `session_peer_t::close`: sends `CLOSE <id>`; a `CLOSED <result>` reply gives the mapped result.
     *
     * @param id Launch session id.
     * @param timeout Longest wait.
     * @return See `session_peer_t::close`.
     */
    std::optional<rtsp_stream::close_result_e> close(std::uint32_t id, std::chrono::milliseconds timeout) override;

    /**
     * @brief `session_peer_t::close_cert`: sends `CLOSE_CERT <hash>`; a `CLOSED_CERT <count>` reply gives the count.
     *
     * While a `list` is in flight or any other request waits, this sends and waits independently of them.
     *
     * @param cert_sha256 Lowercase hex SHA-256.
     * @param timeout Longest wait.
     * @return See `session_peer_t::close_cert`.
     */
    std::optional<std::uint32_t> close_cert(std::string_view cert_sha256, std::chrono::milliseconds timeout) override;

    /**
     * @brief The last announced state.
     *
     * @return See `session_peer_t::state`.
     */
    state_e state() override;

    /**
     * @brief Route a message that the channel reader received from the draining process.
     *
     * - `SESSIONS`, `CLOSED`, `CLOSED_CERT`, `ENDED`, `END_REJECTED`: delivered to the request tracker by request id;
     *   returns what `request_tracker_t::deliver` returns (`false` for an unknown, answered or timed-out id: a late
     *   reply, dropped).
     * - `HANDOFF_STATE`: stored for `state()`; returns `true`.
     * - Anything else (`MAIN`, `PONG`, `unknown_t`, requests): ignored; returns `false`. The caller handles it or drops it.
     *
     * The caller keeps ownership of any fd attached to the message. Never blocks; called on the reader's worker thread.
     *
     * @param message Decoded message.
     * @return `true` when the message was consumed here.
     */
    bool deliver(const protocol::message_t &message);

    /**
     * @brief The channel reached `eof`, `error`, `oversized` or `malformed`: wake every waiting request with no reply
     * (`request_tracker_t::fail_all`). Requests started afterwards fail at once without waiting (they find the channel
     * closed or the send fails). Idempotent.
     */
    void on_channel_closed();

  private:
    std::shared_ptr<channel_t> _channel;  ///< Channel (shared with the reader).
    int _pidfd;  ///< Owned pidfd or -1.
    request_tracker_t _tracker;  ///< Reply matching.
    std::atomic<bool> _gone {false};  ///< Sticky gone flag.
    std::atomic<bool> _closed {false};  ///< `on_channel_closed` was called.
    std::atomic<int> _state {0};  ///< Last `state_e` as an int.
  };
}  // namespace handoff
