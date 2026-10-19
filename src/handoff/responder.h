/**
 * @file src/handoff/responder.h
 * @brief The draining process's answers to the active process's requests: `SESSIONS?`, `CLOSE`, `CLOSE_CERT` and
 * `END_ALL`.
 *
 * See `docs/drain-handoff.md`, "App ownership and prep commands" (the terminate rule, `END_ALL` on its own thread),
 * "The handoff channel" and "Visibility and control".
 *
 * Use: after `old_side_t::run()` returned `handed_over`, the coordinator constructs one `drain_responder_t` over the
 * same `channel_t` and the same `end_all_machine_t` that `old_side_t` used, and passes `handle` as the message handler
 * of a `channel_reader_t` (with `answer_pings` false). `handle` is therefore called on that reader's worker thread, one
 * message at a time, in arrival order. It never blocks that thread: `SESSIONS?` and `CLOSE` are answered inline (the
 * session operations behind them do not wait for a session join, lb.2 [C16]), while `END_ALL` and `CLOSE_CERT` run on
 * their own threads, because they join sessions (up to the 10 s hang guard each). So `SESSIONS?` and `CLOSE` stay
 * answerable for as long as an `END_ALL` or `CLOSE_CERT` waits.
 *
 * The responder does not check the runtime mode: constructing it is the coordinator's statement that this process is
 * draining. It starts no thread until the first `END_ALL` or `CLOSE_CERT` arrives.
 *
 * Replies echo the request id. Each reply is sent with `channel_t::send`, which never blocks; a failed send (peer gone,
 * `EAGAIN`) is logged as a Warning and not retried: the active process times out (500 ms, 15 s for `END_ALL`) and, for
 * `END_ALL`, the machine stays in `ended`, so a repeated `END_ALL` is answered `ENDED` at once. Messages of any other
 * type (including `PING`, `HANDOFF_STATE`, replies and `unknown_t`) are ignored with a Debug line. An fd attached to
 * any message is closed by `handle` (the responder never keeps one).
 *
 * Threads and locks: one internal mutex guards only the list of handler threads and the stopping flag. It is never held
 * across a call to an operation (`responder_ops_t`), a `channel_t::send` or a `join`. The operations are called with no
 * responder lock held and may take the RTSP clear lock and slots lock; they must not call back into the responder
 * (`handle`, `stop`).
 *
 * Overlap cases (all four are part of the contract):
 * - A second `END_ALL` while one runs: it gets its own handler thread, which calls `end_all_machine_t::handle_end_all`;
 *   the machine makes it wait for the running one and give the same answer. `ends_and_join` runs once. Each request
 *   id gets its own reply, `ENDED` or `END_REJECTED`. An `END_ALL` after `ended` is answered `ENDED` at once, after a
 *   decided rollback `END_REJECTED` at once.
 * - `CLOSE_CERT` during `END_ALL` (or the other way round): independent handler threads. The operations serialise
 *   inside the RTSP server (clear lock), so the `CLOSE_CERT` count is the number of sessions this request ended, which
 *   is 0 when `END_ALL` had ended that client's sessions first. Both are answered. `SESSIONS?` meanwhile lists the
 *   sessions that are still stopping.
 * - Stopped while a handler runs: see `stop()`.
 * - Too many requests: at most `max_handler_threads` handler threads exist at once (finished ones do not count). A
 *   request that arrives beyond that is dropped with a Warning and gets no reply (the active process times out and
 *   treats it as no reply, which for `END_ALL` means the terminate is refused).
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "channel.h"
#include "end_all.h"
#include "protocol.h"

namespace handoff {
  /**
   * @brief Result of a `CLOSE <id>` on the draining process.
   */
  enum class close_outcome_e {
    closed,  ///< Reply `CLOSED closed`.
    already_stopping,  ///< Reply `CLOSED already-stopping`.
    not_found,  ///< Reply `CLOSED not-found`.
  };

  /**
   * @brief The session operations of the draining process. The production values are bound by the production
   * constructor; tests pass fakes through the test constructor.
   *
   * Every member must be set. Production bindings:
   * - `list_sessions`: `rtsp_stream::list_sessions()` mapped to `protocol::session_entry_t` (`started` as Unix epoch
   *   milliseconds);
   * - `close_session`: `rtsp_stream::request_session_close(id)` mapped (`closed`, `already_stopping`, `not_found`);
   * - `close_cert`: `rtsp_stream::terminate_sessions_by_cert_hash(hex)`, which ends and joins the sessions of the client
   *   whose `crypto::hash(client_cert)` equals `hex`, with reason `terminated`, and returns how many it ended;
   * - `end_and_join`: `rtsp_stream::terminate_sessions(terminated)` followed by `proc::proc.drop_released_app()`: ends
   *   and joins every session and gives up the claim on the released app (no `undo`, no process group terminate).
   */
  struct responder_ops_t {
    std::function<std::vector<protocol::session_entry_t>()> list_sessions;  ///< Live (running or stopping) sessions.
                                                                            ///< Must not wait for a join.
    std::function<close_outcome_e(std::uint32_t)> close_session;  ///< Ask one session to stop (reason `api-close`).
                                                                  ///< Must not wait for a join.
    std::function<std::uint32_t(const std::string &)> close_cert;  ///< End and join the sessions of one client;
                                                                   ///< argument is the validated 64-character
                                                                   ///< lowercase hex SHA-256. Returns the count.
    std::function<void()> end_and_join;  ///< Passed to `end_all_machine_t::handle_end_all`.
  };

  /**
   * @brief Answers the active process's requests on the draining side. Thread-safe.
   */
  class drain_responder_t {
  public:
    /**
     * @brief Most handler threads (`END_ALL` plus `CLOSE_CERT`) alive at once.
     */
    static constexpr std::size_t max_handler_threads = 8;

    /**
     * @brief Longest an `END_ALL` or `CLOSE_CERT` handler may take before the active process has given up (15 s). A
     * handler that exceeds it logs a Warning when it finishes ("late reply") and still sends its reply.
     */
    static constexpr std::chrono::milliseconds late_after {15000};

    /**
     * @brief Create a responder with the production operations. Nothing runs yet.
     *
     * @param channel The channel; must outlive the responder.
     * @param machine The `END_ALL` state machine shared with `old_side_t`; must outlive the responder.
     */
    drain_responder_t(channel_t &channel, end_all_machine_t &machine);

    /**
     * @brief Calls `stop()`.
     */
    ~drain_responder_t();

    drain_responder_t(const drain_responder_t &) = delete;  ///< Not copyable.
    drain_responder_t &operator=(const drain_responder_t &) = delete;  ///< Not copyable.

    /**
     * @brief Handle one received message; the `channel_reader_t` message handler.
     *
     * - `sessions_request_t`: reply `sessions_reply_t` with `list_sessions()` inline.
     * - `close_request_t`: reply `close_reply_t` with the outcome of `close_session(id)` inline.
     * - `close_cert_request_t`: when `cert_sha256` is not exactly 64 lowercase hex characters, reply
     *   `close_cert_reply_t{count 0}` inline with a Warning and call nothing; otherwise start a handler thread that
     *   calls `close_cert(hash)` and replies `close_cert_reply_t{count}`.
     * - `end_all_t`: start a handler thread that calls `machine.handle_end_all(end_and_join)` and replies `ended_t` or
     *   `end_rejected_t`.
     * - anything else: ignored.
     *
     * After `stop()` has begun every message is ignored (its fd closed) and nothing is sent. Returns without waiting for
     * any handler thread. Exceptions from an operation are caught in the handler, logged as an Error, and produce no
     * reply (for `END_ALL` the machine's state is whatever `handle_end_all` left; it does not throw into the machine).
     *
     * @param message Received message; the responder closes an attached fd.
     */
    void handle(received_t message);

    /**
     * @brief Stop the responder and join every handler thread. Idempotent and thread-safe.
     *
     * Sets the stopping flag first (so `handle` ignores new messages and running handlers skip their reply), then
     * joins. A handler that is inside `end_and_join` or `close_cert` is not interrupted: `stop()` waits for it, which
     * is bounded by the session join hang guard (10 s). The session operations still complete, and `end_all_machine_t`
     * still reaches `ended`, so the work is not left half done; only the reply is skipped. The channel is neither
     * closed nor otherwise touched. Must not be called from a handler thread or from an operation (it would join
     * itself); it may be called from the channel reader's worker thread (its close handler) because the handler
     * threads never depend on that thread.
     */
    void stop();

#ifdef SUNSHINE_TESTS
    /**
     * @brief Test-only constructor: fake operations and a fake clock.
     *
     * @param channel The channel.
     * @param machine The `END_ALL` state machine.
     * @param ops Fake operations.
     * @param now Returns the current time; used only for the "late reply" Warning (`late_after`). Empty means
     *        `steady_clock::now`.
     */
    drain_responder_t(channel_t &channel, end_all_machine_t &machine, responder_ops_t ops, std::function<std::chrono::steady_clock::time_point()> now = {});

    /**
     * @brief Test-only: number of handler threads that have not finished yet.
     *
     * @return Count of live handler threads.
     */
    std::size_t live_handlers_for_test() const;
#endif

  private:
    /**
     * @brief One handler thread and its done flag.
     */
    struct handler_t {
      std::thread thread;  ///< The thread.
      std::shared_ptr<std::atomic<bool>> done;  ///< Set by the thread as its last action.
    };

    /**
     * @brief Start a handler thread running `work`, unless stopping or at the cap. Reaps finished threads first.
     *
     * @param what Request name for the log.
     * @param work The handler body.
     */
    void spawn(const char *what, std::function<void()> work);

    channel_t &_channel;  ///< Channel.
    end_all_machine_t &_machine;  ///< `END_ALL` state machine.
    responder_ops_t _ops;  ///< Operations.
    std::function<std::chrono::steady_clock::time_point()> _now;  ///< Clock.
    mutable std::mutex _mutex;  ///< Guards the members below.
    bool _stopping {false};  ///< `stop()` was called.
    std::vector<handler_t> _handlers;  ///< Handler threads.
  };
}  // namespace handoff
