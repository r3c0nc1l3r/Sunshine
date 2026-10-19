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
 * message at a time, in arrival order. It never blocks that thread: `SESSIONS?` and `CLOSE` are answered inline, while
 * `END_ALL` and `CLOSE_CERT` run on their own threads, because they join sessions (up to the 10 s hang guard each).
 * The inline operations (`list_sessions`, `close_session`) take only the RTSP slots lock (briefly), never the clear
 * lock, and never wait for a session join (lb.2 [C16]). The threaded operations (`close_cert`, `end_and_join`) may take
 * the clear lock and the slots lock and wait for joins. So `SESSIONS?` and `CLOSE` stay answerable for as long as an
 * `END_ALL` or `CLOSE_CERT` waits.
 *
 * Lifetime: the `channel_reader_t` that calls `handle` must be stopped and joined (`channel_reader_t::stop()`) before
 * the responder is destroyed, or the responder must outlive the reader. After `stop()` has returned, `handle()` stays
 * safe to call for as long as the object lives: it only closes an attached fd and ignores the message. The channel and
 * the machine must outlive the responder. The coordinator calls `end_all_machine_t::reset()` (a new handoff) only after
 * `stop()` has returned, because `reset()` must not run while `handle_end_all` runs.
 *
 * The responder does not check the runtime mode: constructing it is the coordinator's statement that this process is
 * draining. It starts no thread until the first `END_ALL` or `CLOSE_CERT` arrives.
 *
 * Replies echo the request id. Each reply is sent with `channel_t::send`, which never blocks; a failed send (peer gone,
 * `EAGAIN`) is logged as a Warning and not retried: the active process times out (500 ms, 15 s for `END_ALL`) and, for
 * `END_ALL`, the machine stays in `ended`, so a repeated `END_ALL` is answered `ENDED` at once. Messages of any other
 * type (including `PING`, `HANDOFF_STATE`, replies and `unknown_t`) are ignored with a Debug line.
 *
 * Fd ownership: `handle` takes ownership of an attached fd on entry and closes it at once, before it looks at the
 * message, whatever the message is (the responder never keeps one and never closes it a second time).
 *
 * `handle` never throws. An exception from an inline operation, from a send, or a `std::system_error` from starting a
 * thread is caught, logged as an Error, and answered as each request says below (an inline request, `SESSIONS?` or
 * `CLOSE`, gets no reply: the active process times out after 500 ms). `handle` closes no fd on that path (it closed it
 * on entry).
 *
 * Threads and locks: one internal mutex guards only the list of handler threads and the stopping flag. It is never held
 * across a call to an operation (`responder_ops_t`), a `channel_t::send` or a `join`. The operations are called with no
 * responder lock held and may take the RTSP clear lock and slots lock; they must not call back into the responder
 * (`handle`, `stop`).
 *
 * Overlap cases (all four are part of the contract):
 * - A second `END_ALL` while one runs: there is one `END_ALL` runner thread at a time. A duplicate costs no thread: its
 *   request id is registered as a waiter and the running handler answers it, with the same answer as the request that
 *   started the round (`ENDED` or `END_REJECTED`, whichever `handle_end_all` returned; `end_and_join` runs once). The
 *   `END_ALL` runner is not counted against `max_close_cert_threads`, so no number of blocked `CLOSE_CERT` handlers can
 *   delay or drop an `END_ALL`. Testable bound: with `max_close_cert_threads` blocked `CLOSE_CERT` handlers, an
 *   `END_ALL` is answered within 100 ms of the machine's answer, and every duplicate registered before the runner
 *   collects its waiters is answered at the same moment. The runner takes the waiter list and clears its "running" flag
 *   in one critical section, so a duplicate is either answered by that round or starts a new round, which the machine
 *   answers at once from `ended` (`ENDED`) or `rolling_back` (`END_REJECTED`). An `END_ALL` is never left unanswered by
 *   the responder (except after `stop()`, or when the send fails, or when the runner thread cannot be started and the
 *   machine's state is still undecided: then there is an Error and no reply).
 * - If the runner thread cannot be started (`std::system_error`): the request is answered from `machine.state()` only
 *   where the final answer is already known (`ended` gives `ENDED`, `rolling_back` gives `END_REJECTED`); otherwise it
 *   gets no reply. No early reply ever contradicts the machine's final answer.
 * - Runner failure or stop: when the runner fails to start, fails, or is stopped by `stop()`, the `_end_all_running` flag
 *   is cleared and its waiters are answered (per the fallback above) or dropped in the same critical section, so a later
 *   round never answers a stale waiter.
 * - The runner's exception path takes the waiter list in that same critical section (and clears the running flag)
 *   before it answers every request of the round with `END_REJECTED`.
 * - A request id registered twice (the peer reuses an id) gets two replies. This is harmless, because the tracker drops
 *   the second; it is stated for the test writer.
 * - `CLOSE_CERT` during `END_ALL` (or the other way round): independent handler threads. The operations serialise
 *   inside the RTSP server (clear lock), so the `CLOSE_CERT` count is the number of sessions this request ended, which
 *   is 0 when `END_ALL` had ended that client's sessions first. Both are answered. `SESSIONS?` meanwhile lists the
 *   sessions that are still stopping.
 * - Stopped while a handler runs: see `stop()`.
 * - Too many `CLOSE_CERT` requests: at most `max_close_cert_threads` `CLOSE_CERT` handler threads exist at once
 *   (finished ones do not count). A request beyond that is dropped with a Warning and gets no reply (the peer gives
 *   up after 500 ms anyway).
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
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
   * - `close_cert`: `rtsp_stream::terminate_sessions_by_cert_hash(hex)`, which ends and joins the sessions of the
   *   client whose `crypto::hash(client_cert)` equals `hex`, with reason `terminated`, and returns how many it ended
   *   (a `std::size_t`; the binding clamps it to `UINT32_MAX` for the `uint32_t` count);
   * - `end_and_join`: `rtsp_stream::terminate_sessions(terminated)` followed by `proc::proc.drop_released_app()`: ends
   *   and joins every session and gives up the claim on the released app (no `undo`, no process group terminate).
   */
  struct responder_ops_t {
    std::function<std::vector<protocol::session_entry_t>()> list_sessions;  ///< Live (running or stopping) sessions.
                                                                            ///< Takes only the slots lock; no join.
    std::function<close_outcome_e(std::uint32_t)> close_session;  ///< Ask one session to stop (reason `api-close`).
                                                                  ///< Takes only the slots lock; no join.
    std::function<std::uint32_t(const std::string &)> close_cert;  ///< End and join the sessions of one client;
                                                                   ///< argument is the validated 64-character
                                                                   ///< lowercase hex SHA-256. Returns the count.
    std::function<void()> end_and_join;  ///< Passed to `end_all_machine_t::handle_end_all`. If it throws, the round's
                                         ///< requests are all answered `END_REJECTED` (see `handle`).
  };

  /**
   * @brief Answers the active process's requests on the draining side. Thread-safe.
   */
  class drain_responder_t {
  public:
    /**
     * @brief Most `CLOSE_CERT` handler threads alive at once. The single `END_ALL` runner is not counted.
     */
    static constexpr std::size_t max_close_cert_threads = 8;

    /**
     * @brief Longest an `END_ALL` handler may take before the active process has given up (15 s). A round that exceeds
     * it logs a Warning when it finishes ("late reply") and still sends its replies.
     */
    static constexpr std::chrono::milliseconds end_all_late_after {15000};

    /**
     * @brief Same for a `CLOSE_CERT` handler (500 ms).
     */
    static constexpr std::chrono::milliseconds close_cert_late_after {500};

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
     *   calls `close_cert(hash)` and replies `close_cert_reply_t{count}`; over the cap or when the thread cannot be
     *   started: Warning (Error for a thread failure), no reply. An exception from `close_cert`: Error, no reply.
     * - `end_all_t`: register the request id with the `END_ALL` runner, starting the runner when none runs (see the
     *   overlap cases). The runner calls `machine.handle_end_all(end_and_join)` and replies `ended_t` or
     *   `end_rejected_t` to every registered request id. If `end_and_join` (or `handle_end_all`) throws, the runner
     *   logs an Error and replies `end_rejected_t` to every request of that round (the active process then refuses the
     *   terminate; the machine's state is whatever it left).
     * - anything else: ignored.
     *
     * After `stop()` has begun every message is ignored and nothing is sent. Returns without waiting for any handler
     * thread. Never throws (see the file comment).
     *
     * @param message Received message; `handle` closes an attached fd on entry.
     */
    void handle(received_t message);

    /**
     * @brief Stop the responder and join every handler thread. Idempotent and thread-safe.
     *
     * Every call, including a second concurrent call and the destructor, returns only after all handler threads are
     * joined (a later caller waits for the first one's joins). Sets the stopping flag first (so `handle` ignores new
     * messages and running handlers skip their reply), then joins.
     * A handler that is inside `end_and_join` or `close_cert` is not interrupted: `stop()` waits for it, which
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
     * @param now Returns the current time; used only for the "late reply" Warning. Empty means
     *        `steady_clock::now`.
     */
    drain_responder_t(
      channel_t &channel,
      end_all_machine_t &machine,
      responder_ops_t ops,
      std::function<std::chrono::steady_clock::time_point()> now = {}
    );

    /**
     * @brief Test-only: number of handler threads (the `END_ALL` runner and the `CLOSE_CERT` threads) that have not
     * finished yet.
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
     * @brief Start a handler thread running `work`, unless stopping. Reaps finished threads first.
     *
     * @param what Request name for the log.
     * @param counted_against_cap `true` for `CLOSE_CERT`: refuse when `max_close_cert_threads` are alive.
     * @param work The handler body.
     * @return `false` when stopping, at the cap, or the thread could not be started (already logged).
     */
    bool spawn(const char *what, bool counted_against_cap, std::function<void()> work);

    channel_t &_channel;  ///< Channel.
    end_all_machine_t &_machine;  ///< `END_ALL` state machine.
    responder_ops_t _ops;  ///< Operations.
    std::function<std::chrono::steady_clock::time_point()> _now;  ///< Clock.
    mutable std::mutex _mutex;  ///< Guards the members below.
    std::condition_variable _stop_cv;  ///< Signals the end of the first `stop()`'s joins.
    bool _stopping {false};  ///< `stop()` was called.
    bool _stopped {false};  ///< The first `stop()` finished joining.
    bool _end_all_running {false};  ///< An `END_ALL` runner thread is active.
    std::vector<std::uint64_t> _end_all_waiters;  ///< Request ids the runner answers.
    std::vector<handler_t> _handlers;  ///< Handler threads.
  };
}  // namespace handoff
