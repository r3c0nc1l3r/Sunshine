/**
 * @file src/handoff/revocation_poll.h
 * @brief The draining process's revocation backstop: watch `sunshine_state.json` and end the sessions of clients that
 * are disabled there, with no channel needed.
 *
 * See `docs/drain-handoff.md`, "Visibility and control" (revoking a client). The pure parsing is
 * `disabled_client_certs()` in `revocation.h`; this class is the thread around it.
 *
 * One pass:
 * 1. Open and read the whole state file by path (never a cached fd, so a file that was replaced by a rename is read
 *    whole; at most 4 MiB, a longer file counts as unreadable). It is opened non-blocking and must be a regular file;
 *    anything else (a FIFO, device or directory) counts as unreadable, so a bad path never blocks the poller or `stop()`.
 * 2. `disabled_client_certs(content)`. `std::nullopt`, an unreadable or missing file, or an empty read ends no
 *    session: log a Warning (never the content or any certificate) and retry at the next wake. A client missing from
 *    the file ends nothing.
 * 3. For each returned PEM: canonicalize it (production: the same conversion as `nvhttp`'s
 *    `canonical_certificate_pem`: `crypto::x509(pem)`, and `crypto::pem()` of it only when that is not null; when it is
 *    null the canonicalizer returns an empty string without calling `crypto::pem`). An empty result means an invalid
 *    certificate: it is skipped with a Warning, and (see the handled set) that Warning is logged once, not every wake.
 *    Otherwise call the terminate callback (production: `rtsp_stream::terminate_sessions_by_cert(canonical_pem)`) once
 *    for each certificate that is new: not in the handled set.
 * 4. The handled set, keyed by the raw PEM from the file, becomes the current disabled set minus the certificates whose
 *    callback threw (logged as an Error; they stay new for the next pass) and minus those that were not processed
 *    because `stop()` was requested. A skipped (invalid) certificate is in the handled set, so it is not retried or
 *    logged again until it leaves the file's disabled set and returns. The first successful pass has an empty handled
 *    set, so every disabled certificate is new. A certificate that was enabled again and disabled again is new again.
 *    A failed pass leaves the handled set unchanged.
 *
 * Wakes: a pass runs immediately at the start, on every `inotify` event of the state file's directory that names the
 * state file (events `IN_CLOSE_WRITE`, `IN_MOVED_TO`, `IN_MOVED_FROM`, `IN_CREATE`, `IN_DELETE`, `IN_MODIFY`; the
 * directory is watched, not the file, so a replacement by rename is seen), and, at the latest, when `wake_cap` (2 s)
 * has passed since the previous wake, with or without an event. Events for other files do not start a pass. Events that
 * arrive close together are coalesced into one pass: after a relevant event the poller waits for further events for at
 * most 50 ms (stopping early on `stop()`) before it runs the pass, so a file being written is read once complete. If `inotify` cannot be used (init or watch failed, queue overflow
 * `IN_Q_OVERFLOW`), a Warning is logged once and the poller continues with the timer alone; an overflow also forces a
 * pass.
 *
 * Warning rate: the first failed pass of a failure episode logs a Warning, and further failed passes of the same
 * episode log at most one Warning per 60 s (by the injected clock); the first successful pass after a failure logs an
 * Info line.
 *
 * Directory: the watched directory is the state file's parent path; a relative path with no parent directory (a bare
 * file name) watches `"."`. If the directory does not exist (or the watch cannot be added), the poller logs one Warning
 * and continues with the timer alone; the passes then report the missing file as a failure (Warning rate limit above).
 *
 * Only while draining: `start()` refuses unless the process is draining, and each wake first checks again; a wake in
 * which the process is no longer draining (a rollback happened) runs no pass and does not count in
 * `passes_for_test()`. The coordinator calls `stop()` when the process leaves draining mode, from a thread that can
 * wait: `stop()` may block for as long as one running terminate callback (see `stop()`).
 *
 * Thread-safety: `start()`, `stop()` and `running()` may be called from any thread except the poller's own thread and
 * the terminate callback. They are serialised by one mutex that is not held while the poller thread runs a pass.
 * The terminate callback runs on the poller thread with no poller lock held; it takes the RTSP clear lock and joins
 * sessions (up to the 10 s hang guard each), and must not call `start()` or `stop()`.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace handoff {
  /**
   * @brief Watches the state file while the process drains.
   */
  class revocation_poller_t {
  public:
    /**
     * @brief Upper bound of the time between two wakes.
     */
    static constexpr std::chrono::milliseconds wake_cap {2000};

    /**
     * @brief Create a poller with the production inputs. Nothing runs yet. The state file path is
     * `config::nvhttp.file_state`, read at `start()`.
     */
    revocation_poller_t();

    /**
     * @brief Calls `stop()`.
     */
    ~revocation_poller_t();

    revocation_poller_t(const revocation_poller_t &) = delete;  ///< Not copyable.
    revocation_poller_t &operator=(const revocation_poller_t &) = delete;  ///< Not copyable.

    /**
     * @brief Start the poller thread.
     *
     * Does nothing and returns `false` when the process is not draining (production: `runtime().mode() ==
     * mode_e::draining`; the test config's `is_draining` gates `start()` the same way) or when the state file path is
     * empty. Returns `true` when the thread is running after the
     * call, also when it already was (idempotent, no second thread). After `stop()` it can start again with a fresh
     * "previous disabled set" (empty).
     *
     * @return `true` when running.
     */
    bool start();

    /**
     * @brief Stop and join the poller thread. Idempotent.
     *
     * The thread is woken at once (an internal eventfd or pipe is polled with the inotify fd), so `stop()` does not wait
     * for the 2 s cap. A pass that is running finishes reading, but once the stop is requested no further terminate
     * callback is started: the remaining certificates are skipped (they will not be revisited). A terminate callback
     * that is already running is not interrupted; `stop()` waits for it (it may block for as long as one running
     * callback, bounded by the session join hang guard), so the caller must be a thread that can wait. A
     * wake or inotify event that arrives during `stop()` is discarded. The inotify fd is closed by the thread before it
     * exits.
     */
    void stop();

    /**
     * @brief Whether the thread is running.
     *
     * @return `true` between a successful `start()` and the end of `stop()`.
     */
    bool running() const;

#ifdef SUNSHINE_TESTS
    /**
     * @brief Test-only inputs.
     */
    struct test_config_t {
      std::filesystem::path state_file;  ///< The state file to watch (its directory must exist).
      std::function<void(const std::string &)> terminate;  ///< Called with the canonical PEM instead of
                                                           ///< `rtsp_stream::terminate_sessions_by_cert`.
      std::function<std::string(const std::string &)> canonicalize;  ///< Canonicalizer; empty means the identity
                                                                     ///< function (the production one needs real
                                                                     ///< certificates). An empty return skips the cert.
      std::function<bool()> is_draining;  ///< Replaces the mode check (also the one in `start()`); empty means "always
                                          ///< draining".
      std::function<std::chrono::steady_clock::time_point()> now;  ///< Clock for the Warning rate limit; empty means
                                                                   ///< `steady_clock::now`.
      std::chrono::milliseconds cap {50};  ///< Replaces `wake_cap` so a test needs no 2 s waits.
    };

    /**
     * @brief Test-only constructor.
     *
     * @param config Inputs.
     */
    explicit revocation_poller_t(test_config_t config);

    /**
     * @brief Test-only: number of completed passes (successful or not), counted after the pass's last callback. A wake
     * that was skipped because the process was not draining is not counted.
     *
     * @return Count.
     */
    std::uint64_t passes_for_test() const;

    /**
     * @brief Test-only: number of passes that ended with a failure (missing, unreadable or partial file, or
     * `std::nullopt` from `disabled_client_certs`).
     *
     * @return Count.
     */
    std::uint64_t failed_passes_for_test() const;
#endif

  private:
    /**
     * @brief The poller thread body.
     */
    void run();

    std::atomic<bool> _stop_requested {false};  ///< Set by `stop()`.
    std::atomic<std::uint64_t> _passes {0};  ///< Completed passes.
    std::atomic<std::uint64_t> _failed_passes {0};  ///< Failed passes.
    std::atomic<bool> _running {false};  ///< The thread runs.
    mutable std::mutex _mutex;  ///< Serialises `start` and `stop`.
    std::thread _thread;  ///< Poller thread.
    int _wake_fd {-1};  ///< Eventfd that `stop()` writes; created by `start()`, closed after the join.
    std::filesystem::path _state_file;  ///< State file for this run.
    std::set<std::string> _handled;  ///< Disabled set of the previous successful pass (poller thread only).
    std::function<void(const std::string &)> _terminate;  ///< Terminate callback.
    std::function<std::string(const std::string &)> _canonicalize;  ///< Canonicalizer.
    std::function<bool()> _is_draining;  ///< Draining predicate.
    std::function<std::chrono::steady_clock::time_point()> _now;  ///< Clock.
    std::chrono::milliseconds _cap {wake_cap};  ///< Wake cap.
    bool _use_config_path {true};  ///< The path comes from `config::nvhttp.file_state` at `start()`.
  };
}  // namespace handoff
