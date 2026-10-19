/**
 * @file src/handoff/end_all.h
 * @brief `END_ALL` against rollback (draining side) and the terminate rule (active side).
 *
 * See `docs/drain-handoff.md`, step 6 ("`END_ALL` against rollback") and "App ownership and prep commands"
 * ("Terminate rule").
 */
#pragma once

// standard includes
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>

namespace handoff {
  /**
   * @brief State of the old (draining) process for one handoff.
   */
  enum class end_state_e {
    idle,  ///< No `END_ALL` yet, no rollback.
    ending,  ///< Ending and joining its sessions for an `END_ALL`.
    ended,  ///< Sessions ended; app claim given up; `ENDED` answered.
    rolling_back,  ///< A rollback was decided.
  };

  /**
   * @brief Answer to one `END_ALL`.
   */
  enum class end_all_answer_e {
    ended,  ///< Answer `ENDED`.
    rejected,  ///< Answer `END_REJECTED`.
  };

  /**
   * @brief What a decided rollback does with the released app.
   */
  enum class rollback_app_e {
    take_back,  ///< Decided in `idle`: clear the release flag.
    drop,  ///< Decided in `ending` or `ended`: drop the app state, run no `undo`.
  };

  /**
   * @brief The draining side's state machine. Thread-safe; the mutex is never held while `end_and_join` runs.
   */
  class end_all_machine_t {
  public:
    /**
     * @brief Handle one `END_ALL`.
     *
     * - `idle`: move to `ending`, run `end_and_join` (without the mutex), then move to `ended` and answer `ended`,
     *   unless a rollback was decided meanwhile (then the state stays `rolling_back` and the answer is `rejected`).
     * - `ending`: wait for the running call to finish and give the same answer.
     * - `ended`: answer `ended` at once (without running `end_and_join`).
     * - `rolling_back`: answer `rejected` at once.
     *
     * @param end_and_join Ends every session of the process (reason `terminated`), joins them, and gives up the app
     *        claim.
     * @return The answer to send.
     */
    end_all_answer_e handle_end_all(const std::function<void()> &end_and_join);

    /**
     * @brief Record a rollback decision (before the kill-first step). The state becomes `rolling_back`.
     *
     * @return `take_back` when decided in `idle`; `drop` when decided in `ending`, `ended` or (again) in
     *         `rolling_back` after an earlier `drop`; a second call after `take_back` returns `take_back` again.
     */
    rollback_app_e decide_rollback();

    /**
     * @brief Current state.
     *
     * @return State.
     */
    end_state_e state() const;

    /**
     * @brief Start a new handoff: state `idle`. Must not be called while `handle_end_all` runs.
     */
    void reset();

  private:
    mutable std::mutex _mutex;  ///< Guards the members.
    std::condition_variable _cv;  ///< Signals the end of `ending`.
    end_state_e _state {end_state_e::idle};  ///< State.
    bool _rollback_takes_back {false};  ///< Result of the first `decide_rollback()`.
    bool _ending_rejected {false};  ///< The current `ending` round ends `rejected`.
  };

  /**
   * @brief The active process's view of a draining peer, for the terminate rule. Implemented over the channel and the
   * draining process's pidfd; faked in tests.
   */
  class drain_peer_t {
  public:
    /**
     * @brief Virtual destructor.
     */
    virtual ~drain_peer_t() = default;

    /**
     * @brief Whether a draining process exists (from `HELLO` until its pidfd shows it gone).
     *
     * @return `true` while one exists.
     */
    virtual bool exists() = 0;

    /**
     * @brief Whether the draining process is gone or a zombie (its pidfd is readable).
     *
     * @return `true` when gone.
     */
    virtual bool gone() = 0;

    /**
     * @brief Outcome of one `END_ALL` request.
     */
    enum class reply_e {
      ended,  ///< `ENDED` received.
      rejected,  ///< `END_REJECTED` received.
      no_reply,  ///< Timeout, lost channel, or the send failed.
    };

    /**
     * @brief Send `END_ALL` and wait for the reply.
     *
     * @param timeout Longest wait.
     * @return Outcome.
     */
    virtual reply_e end_all(std::chrono::milliseconds timeout) = 0;
  };

  /**
   * @brief Result of asking for permission to terminate the app.
   */
  enum class terminate_permission_e {
    no_drain,  ///< No draining process exists: terminate as today.
    ended,  ///< `ENDED` received: terminate.
    peer_gone,  ///< The draining process is gone (pidfd): terminate.
    refused,  ///< No `ENDED` and the peer is alive within the budget (or `END_REJECTED`): do not terminate, run no
              ///< `undo`.
  };

  /**
   * @brief Whether a permission allows the terminate.
   *
   * @param permission Permission.
   * @return `true` for `no_drain`, `ended` and `peer_gone`.
   */
  bool allows_terminate(terminate_permission_e permission);

  /**
   * @brief The terminate rule: before any terminate, get `ENDED` or see the draining process gone.
   *
   * With no draining process: `no_drain`. Otherwise: if the peer is already gone, `peer_gone`; else send `END_ALL`
   * with the whole budget; `ended` → `ended`; on `rejected` or `no_reply`, `peer_gone` when the peer is gone by then,
   * else `refused`. Never returns before the budget for `refused` unless the peer answered `rejected`.
   *
   * @param peer Draining peer.
   * @param budget 15 s for request paths, 5 s on the shutdown path.
   * @return Permission.
   */
  terminate_permission_e await_terminate_permission(drain_peer_t &peer, std::chrono::milliseconds budget);
}  // namespace handoff
