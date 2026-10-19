/**
 * @file src/handoff/mdns_holder.h
 * @brief Owner of the mDNS publisher, so the handoff can withdraw and republish it.
 *
 * See `docs/drain-handoff.md`, "mDNS" (table "Other resources"), switch substep 7, `COMMIT`, and the rollback. Linux
 * only. `platf::publish::start()` returns a `platf::deinit_t` whose destruction withdraws the service (the Avahi
 * client is destroyed, so Avahi drops the entry) and joins the publisher thread. `publish.cpp` needs no other change
 * for withdraw and republish, except `platf::publish::renamed()`.
 *
 * Ordering the holder guarantees, for one holder:
 * - **withdraw, wait, republish**: `withdraw(timeout)` returns only after the old publisher object was destroyed (its
 *   thread joined, the static Avahi client, group and poll cleared) and the settle wait ran; a following `start()`
 *   creates the next publisher. The two never overlap, so the single-publisher rule of `platf::publish::start()` holds.
 * - **deferral**: a holder publishes only when `start()` is called. In a process started by a handoff nothing calls it
 *   before `COMMIT` (spec: "nothing is published, mapped or shown before COMMIT"); the new side's `publish_mdns()` is
 *   the first call. The holder does not know about the handoff; the rule is on the caller, and tests of the glue check
 *   it with a fake `publish` function.
 *
 * Rename (decision R8): if the new process publishes while the old entry is still visible, Avahi may rename the new
 * service ("name #2"). `start(true)` arms two one-shot checks, at +3 s and +10 s after the publish; each rebuilds the
 * publisher (destroy and start again, without the settle wait) only if `renamed()` is true then, and logs it.
 *
 * Thread-safety: every member may be called from any thread except the publisher's own thread and a task that the
 * `schedule` function runs inline. One internal mutex serialises the members; `withdraw`, `start`, `stop` and a
 * rebuild hold it across the blocking join (bounded: `simple_poll_quit` wakes the loop at once) and the settle wait,
 * so a rebuild task that runs during a withdraw waits for it, then sees the new epoch and does nothing. Never call a
 * member while holding a lock that a task-pool task takes, and never from a signal handler.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#ifdef __linux__
  // local includes
  #include "src/platform/common.h"

namespace handoff {
  /**
   * @brief The platform calls the holder makes; the defaults are the production ones, tests pass fakes.
   *
   * Function pointers (not `std::function`) so a fake is a plain function with static state.
   */
  struct mdns_ops_t {
    /**
     * @brief Start publishing. Default: `platf::publish::start`.
     *
     * Returns the publisher (destroying it withdraws) or `nullptr` when unavailable.
     */
    std::unique_ptr<platf::deinit_t> (*publish)() = nullptr;

    /**
     * @brief Whether the live publisher was renamed. Default: `platf::publish::renamed`.
     */
    bool (*renamed)() = nullptr;

    /**
     * @brief Wait the given time (the settle wait of `withdraw`). Default: `std::this_thread::sleep_for`.
     *
     * Called with the mutex held, from the thread that called `withdraw`.
     */
    void (*wait)(std::chrono::milliseconds) = nullptr;

    /**
     * @brief Run a task once after a delay on another thread. Default: `task_pool.pushDelayed`.
     *
     * Used only for the rename checks. A fake may store the tasks and run them by hand. The task must not run inline
     * inside this call.
     */
    void (*schedule)(std::function<void()> task, std::chrono::milliseconds delay) = nullptr;
  };

  /**
   * @brief Timing of the holder; tests shorten it.
   */
  struct mdns_timing_t {
    /**
     * @brief Longest settle wait after a withdraw, a cap on the `timeout` argument (spec: wait up to 5 s for Avahi to
     * drop the entry; the old side passes `old_side_timeouts_t::mdns_withdraw`).
     *
     * There is no portable way to ask Avahi whether the entry is gone, so `withdraw(timeout)` waits
     * `min(timeout, settle)` through `ops.wait` after the destruction, once. The default of 1 s is enough for the
     * local daemon to process the client's disconnect and send the goodbye packets; the rename rebuild below is the
     * backstop when it was not.
     */
    std::chrono::milliseconds settle {1000};

    std::chrono::milliseconds rename_first {3000};  ///< First rename check after a `start(true)`.
    std::chrono::milliseconds rename_second {10000};  ///< Second rename check after a `start(true)`.
  };

  /**
   * @brief Owns the mDNS publisher and serialises withdraw, wait, republish and the rename rebuild.
   */
  class mdns_holder_t {
  public:
    /**
     * @brief Construct with the production platform calls and the default timing.
     *
     * Nothing is published until `start()`.
     */
    mdns_holder_t();

    /**
     * @brief Construct with injected calls (tests). A null member of `ops` takes its production default.
     *
     * @param ops Platform calls.
     * @param timing Timing.
     */
    mdns_holder_t(const mdns_ops_t &ops, const mdns_timing_t &timing);

    /**
     * @brief Stop publishing (as `stop()`).
     */
    ~mdns_holder_t();

    mdns_holder_t(const mdns_holder_t &) = delete;
    mdns_holder_t &operator=(const mdns_holder_t &) = delete;

    /**
     * @brief Publish.
     *
     * Under the mutex: if a publisher is alive, do nothing and return `true` (it never creates a second one, and does
     * not re-arm rename checks). Otherwise call `ops.publish()`; a null result is logged as a Warning (mDNS
     * unavailable), leaves the holder unpublished and returns `false`. On success bump the epoch, and when
     * `arm_rename_checks` is true call `ops.schedule` twice: `rename_first` and `rename_second` after now, each with a
     * task that holds the epoch it was armed in. A task, when it runs: lock; return if the epoch changed (a `start`,
     * `withdraw` or `stop` ran since) or no publisher is alive; return if `ops.renamed()` is false; otherwise log at
     * Info "mDNS name was changed by a collision, rebuilding" and rebuild: destroy the publisher, call `ops.publish()`
     * once (no settle wait, the epoch is kept so the sibling check still runs), and if that returns null log a Warning
     * and leave the holder unpublished. A check fires at most once and a failed rebuild is not retried.
     *
     * @param arm_rename_checks True in the new process at `COMMIT`; false for a normal start and for the old
     *        process's rollback republish (the new process is dead then, so no old entry can collide).
     * @return `true` when a publisher is alive after the call.
     */
    bool start(bool arm_rename_checks = false);

    /**
     * @brief Withdraw the service and wait for Avahi to drop it.
     *
     * Under the mutex: bump the epoch (so armed rename checks become no-ops); if a publisher is alive, destroy it
     * (this withdraws the entry and joins the publisher thread); then, only if a publisher was alive, call
     * `ops.wait(min(timeout, timing.settle))` once. With no publisher alive it returns at once, without waiting.
     * Idempotent. A later `start()` is the republish (rollback) and is allowed as soon as this returns.
     *
     * @param timeout Longest wait after the destruction (the old side passes 5 s).
     */
    void withdraw(std::chrono::milliseconds timeout);

    /**
     * @brief Stop publishing for good (shutdown): as `withdraw` but with no wait. Idempotent.
     */
    void stop();

    /**
     * @brief Whether a publisher is alive.
     *
     * @return `true` between a successful `start()` (or rebuild) and the next `withdraw`/`stop`.
     */
    bool published() const;

    /**
     * @brief Number of rename rebuilds the checks have done since construction (for tests and the log).
     *
     * @return The count.
     */
    std::uint32_t rename_rebuilds() const;

  private:
    mdns_ops_t ops_;  ///< Platform calls, defaults filled in.
    mdns_timing_t timing_;  ///< Timing.
    mutable std::mutex mutex_;  ///< Serialises every member and the rebuild tasks.
    std::unique_ptr<platf::deinit_t> publisher_;  ///< The live publisher, or null.
    std::uint64_t epoch_ {0};  ///< Bumped by `start` (on success), `withdraw` and `stop`; rename checks carry it.
    std::uint32_t rebuilds_ {0};  ///< Rename rebuilds done.
  };

  /**
   * @brief The process-wide holder with the production calls and timing.
   *
   * Used by the normal start (`start(false)` in place of the bare `platf::publish::start()`, so the deinit is no
   * longer kept in `main`), by the old side's `withdraw_mdns`/`publish_mdns`, by the new side's `publish_mdns`, and by
   * shutdown (`stop()`, which replaces the destruction of the `mDNS` pointer in `main`). Created on first use, never
   * destroyed before exit handlers run: call `stop()` explicitly at shutdown.
   *
   * @return The holder.
   */
  mdns_holder_t &mdns_holder();
}  // namespace handoff
#endif
