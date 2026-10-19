/**
 * @file src/system_tray.h
 * @brief Declarations for the system tray icon and notification system.
 */
#pragma once

// standard includes
#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#ifdef _WIN32
namespace lvh {
  struct LicenseStatus;
}
#endif

/**
 * @brief Handles the system tray icon and notification system.
 */
namespace system_tray {
  /**
   * @brief Callback for opening the UI from the system tray.
   * @param item The tray menu item.
   */
  void tray_open_ui_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief Callback for opening GitHub Sponsors from the system tray.
   * @param item The tray menu item.
   */
  void tray_donate_github_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief Callback for opening Patreon from the system tray.
   * @param item The tray menu item.
   */
  void tray_donate_patreon_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief Callback for opening PayPal donation from the system tray.
   * @param item The tray menu item.
   */
  void tray_donate_paypal_cb([[maybe_unused]] struct tray_menu *item);

#ifdef _WIN32
  /**
   * @brief Callback for opening Virtual HID Driver license settings in the Web UI.
   * @param item The tray menu item.
   */
  void tray_virtualhid_license_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief Callback for opening the latest Virtual HID Driver release.
   * @param item The tray menu item.
   */
  void tray_virtualhid_download_cb([[maybe_unused]] struct tray_menu *item);
#endif

  /**
   * @brief Callback for resetting display device configuration.
   * @param item The tray menu item.
   */
  void tray_reset_display_device_config_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief Callback for restarting Sunshine from the system tray.
   *
   * With the drain handoff on Linux, first asks `handoff::hooks::restart_blocked()`: when it is non-empty the restart
   * is not done and the text is logged at Warning level (a draining process, a handoff in progress, a draining peer, a
   * new process before `MAIN`). Otherwise as before.
   *
   * @param item The tray menu item.
   */
  void tray_restart_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief Callback for exiting Sunshine from the system tray.
   *
   * With the drain handoff on Linux, first asks `handoff::hooks::quit_blocked()`: when it is non-empty Quit is not
   * done and the text is logged at Warning level (a draining process, a new process before `MAIN`). Otherwise as
   * before.
   *
   * @param item The tray menu item.
   */
  void tray_quit_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief Initializes the system tray without starting a loop.
   *
   * Must run on the main thread on Linux and macOS (the Qt constraint). On success the lifecycle phase becomes
   * `running` (see `tray_phase_e`); on failure it is unchanged. Also used by the lifecycle machinery below, which runs
   * it on the main thread.
   *
   * @return 0 if initialization was successful, non-zero otherwise.
   */
  int init_tray();

  /**
   * @brief Processes a single tray event iteration.
   * @return 0 if processing was successful, non-zero otherwise.
   */
  int process_tray_events();

  /**
   * @brief Exit the system tray for good (shutdown).
   *
   * As before: asks the worker thread to stop, calls `tray_exit()` if the tray is initialized, joins the worker. New:
   * the lifecycle phase becomes `ended` (terminal), `run_main_loop` is woken and returns, and every later `suspend`,
   * `resume` and `request_start` does nothing (`resume`/`request_start` return `false`, `suspend` returns `true`).
   * Idempotent. Called from the SIGINT/SIGTERM handler threads, so it must stay safe to call from any thread, also
   * when the tray never started or is suspended; a `suspend` waiting at that moment returns `true`.
   *
   * @return 0 after exiting the system tray.
   */
  int end_tray();

  /**
   * @brief Sets the tray icon in playing mode and spawns the appropriate notification
   * @param app_name The started application name
   */
  void update_tray_playing(std::string app_name);

  /**
   * @brief Sets the tray icon in pausing mode (stream stopped but app running) and spawns the appropriate notification
   * @param app_name The paused application name
   */
  void update_tray_pausing(std::string app_name);

  /**
   * @brief Sets the tray icon in stopped mode (app and stream stopped) and spawns the appropriate notification
   * @param app_name The started application name
   */
  void update_tray_stopped(std::string app_name);

  /**
   * @brief Spawns a notification for PIN Pairing. Clicking it opens the PIN Web UI Page
   */
  void update_tray_require_pin();

#ifdef _WIN32
  /**
   * @brief Update the Virtual HID Driver license submenu and optional notification.
   *
   * @param license Latest machine license details.
   * @param notify_if_unlicensed Whether to notify the user when the machine is not activated and ViGEmBus is not exclusively selected.
   */
  void update_tray_virtualhid_license(const lvh::LicenseStatus &license, bool notify_if_unlicensed);

  /**
   * @brief Query the Virtual HID Driver license and prepare the startup tray state.
   */
  void prepare_tray_virtualhid_license();

  /**
   * @brief Show an update notification for an unsupported Virtual HID Driver.
   *
   * Existing notifications are preserved while the driver choice is unset, when ViGEmBus is exclusively selected,
   * or when the driver is absent or supported.
   *
   * @param installed Whether the driver is installed.
   * @param version Installed driver version.
   * @param version_compatible Whether Sunshine supports the installed version.
   * @param supported_versions User-visible supported version range.
   */
  void update_tray_virtualhid_driver(
    bool installed,
    std::string_view version,
    bool version_compatible,
    std::string_view supported_versions
  );

  /**
   * @brief Query the Virtual HID Driver version and prepare its startup notification.
   */
  void prepare_tray_virtualhid_driver();
#endif

  /**
   * @brief Lifecycle phase of the main-thread tray (Linux; see "Tray lifecycle" in `run_main_loop`).
   */
  enum class tray_phase_e {
    not_started,  ///< No icon was ever shown (the initial phase; a handoff-started process stays here until `COMMIT`).
    running,  ///< The icon is shown (`init_tray` succeeded) and the main loop serves its events.
    suspended,  ///< The icon was removed by `suspend` and may come back with `resume`.
    ended,  ///< `end_tray` ran. Terminal.
  };

  /**
   * @brief Entry points of the tray backend the lifecycle drives; tests replace them (see
   * `set_backend_for_testing`).
   *
   * Function pointers, as the wave 2A handoff seams. The production values are `init_tray`, `process_tray_events` and
   * the library's `tray_exit`.
   */
  struct tray_backend_t {
    int (*init)() = nullptr;  ///< Create the icon on the calling (main) thread. 0 on success.
    int (*loop)() = nullptr;  ///< Run one blocking event iteration. 0 to continue, non-zero when the loop ended.
    void (*exit)() = nullptr;  ///< Ask `loop` to return non-zero and remove the icon. Callable from any thread.
    void (*poll)() = nullptr;  ///< Deliver pending events without blocking, on the loop thread (production: `tray_loop(0)`). Null: no-op.
  };

  /**
   * @brief Run the main-thread tray loop until the process ends. Replaces the loop in `mainThreadLoop`.
   *
   * **Tray lifecycle.** Today `mainThreadLoop` (in `main.cpp`) runs `while (process_tray_events() == 0);` after
   * `init_tray()`, and `end_tray()` is the only way a running tray stops. With the drain handoff the tray must also
   * go away and come back without ending the process, and a process that starts without a tray must be able to show
   * one later. The qt backend must be driven from the main thread only; the only call other threads may make is
   * `tray_exit()` (a queued Qt signal, as `end_tray` already does).
   *
   * Command channel. `suspend`, `resume` and `request_start` run on other threads (the handoff coordinator thread).
   * They do not touch the backend; they set a pending command under an internal mutex, notify a condition variable
   * and (for `suspend`) call the backend's `exit` to wake the blocked loop. Only this function executes commands, on
   * the thread that called it. Commands are not queued up: the pending command is a single slot where the last of
   * `suspend`/`resume`/`start` wins, which is correct because they all converge on "icon shown" or "icon removed".
   *
   * The loop, in this order, until it returns:
   * 1. If `end_tray` ran (phase `ended`), return.
   * 2. If a start or resume command is pending and the phase is `not_started` or `suspended`: call the backend's
   *    `init` on this thread. Success: phase `running`, the "removed/shown" waiters are notified. Failure: log a
   *    Warning "Failed to create system tray", keep the phase, drop the command (the next `resume` may retry).
   * 3. If the phase is `running` and a suspend command is pending, complete it without calling `loop` (this covers a
   *    suspend that arrived between two iterations, or before this function was entered after `init_tray`; a backend's
   *    `exit` has no memory, so the loop must look at the command itself): first call the backend's `poll` (if set) so a
   *    queued `exit` from `suspend()` is delivered to the old icon now and not to the next one, then `exit` once more
   *    on this thread (the call `suspend()` made from another thread was only queued, and nothing pumped it; Qt's exit
   *    handler is idempotent, and a fake backend sees two `exit` calls for such a suspend), then the teardown below,
   *    phase `suspended`, waiters notified, repeat from 1. Otherwise, if the phase is `running`: call the backend's `loop` (blocking).
   *    Production `process_tray_events` returns non-zero for every blocking call (`QtTrayMenu::loop(1)` runs
   *    `QApplication::exec()` and returns -1 when it ends), and 0 only for a non-blocking call; so "returns non-zero"
   *    means "the backend's loop ended", not an error. A fake `loop` may return 0 to model one served event, in which
   *    case repeat from 1. When it returns non-zero: if a suspend command is pending, complete it (as above) and repeat
   *    from 1; if `end_tray` ran, return; otherwise (Quit chosen in the menu, or the backend failed) return, exactly as
   *    the old loop ended.
   *
   *    Suspend and the state mutex (no deadlock): `update_tray_*` hold `tray_state_mutex` while the backend's
   *    `tray_update` is blocking-queued to this thread, so this thread must never wait for that mutex once `loop` has
   *    ended. Therefore the removal is done by the caller of `suspend`, as `end_tray` does today: under
   *    `tray_state_mutex` it exchanges the initialized flag to false, calls the backend's `exit`, and releases the
   *    mutex. `update_tray_*` test the flag under the same mutex and never call the backend once it is false. This
   *    thread only tears down what it owns, sets the phase and notifies, and takes no `tray_state_mutex` after `loop`
   *    returned.
   * 4. Otherwise (`not_started` or `suspended` with no command): wait on the condition variable with a 500 ms timeout,
   *    and return when `shutdown_requested()` is true. The poll is only for the idle phases; it is how a process
   *    with no tray (a draining process, a new process before `COMMIT`) notices a shutdown that did not come through
   *    `end_tray`. While the icon is running the loop blocks in the backend as before.
   *
   * Calling it on a platform whose tray runs on its own thread (Windows) or with the tray compiled out is a
   * programming error; `mainThreadLoop` does not call it there (it keeps its "wait for shutdown" branch).
   *
   * @param shutdown_requested Returns true once the process is shutting down (`shutdown_event->peek()`). May be empty
   *        (never true). Called on this thread, without the internal mutex.
   *
   * Thread-safety: call once, on the main thread. It records its thread id; `suspend` called on that thread (a tray
   * menu callback) does not wait.
   */
  void run_main_loop(const std::function<bool()> &shutdown_requested);

  /**
   * @brief Remove the tray icon, keeping the process and the option to bring it back (the old side's `remove_tray`).
   *
   * Sets the pending command to suspend and, if the phase is `running`, does what `end_tray` does today: under
   * `tray_state_mutex` it exchanges the initialized flag to false and calls the backend's `exit`, then releases the
   * mutex (it never waits for the main thread while holding it). The loop then sets the phase and acknowledges, taking
   * no `tray_state_mutex` (see `run_main_loop`). Waits for the acknowledgement, with no lock held. After the flag is
   * cleared `update_tray_*` never call the backend.
   *
   * A start or resume command still pending (not yet executed) is replaced by the suspend command (last wins).
   *
   * Returns `true` at once, without a command, when there is nothing to remove: phase `not_started`, `suspended` or
   * `ended`, or the loop is not running and the phase is not `running`. After `true` the icon is gone and
   * `update_tray_playing/pausing/stopped/require_pin` do nothing until a resume.
   *
   * @param timeout Longest wait for the acknowledgement.
   * @return `true` when the icon is gone (or was never shown). `false` on timeout (the command stays pending and will
   *         still run), or when called on the main-loop thread (it posts the command and returns `false` without
   *         waiting, to avoid waiting for itself), or on a platform without the main-thread tray (see below).
   *
   * Thread-safety: any thread. Takes only the internal mutex. Must not be called while holding a lock that a tray menu
   * callback or `update_tray_*` takes (the tray state mutex is never held across the wait).
   * Platforms: on other than Linux and macOS (Windows runs the tray on its own thread, and the handoff is Linux only)
   * it does nothing and returns `false`.
   */
  bool suspend(std::chrono::milliseconds timeout = std::chrono::milliseconds {2000});

  /**
   * @brief Bring the icon back after `suspend` (the old side's `restore_tray`, on rollback).
   *
   * Sets the pending command to resume and wakes the loop's condition variable; the loop calls `init` on the main
   * thread. Does not wait for the icon to appear.
   *
   * A suspend still in progress (its command pending or its teardown not yet acknowledged) is never replaced: the
   * resume is accepted and deferred, the loop first completes the suspend (phase `suspended`, waiters notified, so that
   * `suspend` returns `true`), and then executes the resume (`init`, phase `running`). This is the rollback order
   * `remove_tray` then `restore_tray`, also when `remove_tray` timed out.
   *
   * @return `true` when the command was accepted: the phase is `suspended` (re-init is done by the loop), a suspend is in
   *         progress (deferred, as above), or already `running` with no suspend in progress (nothing to do). `false` when the phase is `not_started` (there was no icon to restore; use
   *         `request_start`), `ended`, the main loop was never entered and the tray is not enabled, or the platform has
   *         no main-thread tray.
   *
   * Thread-safety: any thread; non-blocking. Re-initialising Qt's tray after `tray_exit` is assumed to work (the
   * backend's `init` checks `trayIcon` is null); if it fails the loop logs a Warning and the phase stays `suspended`.
   */
  bool resume();

  /**
   * @brief Ask the main loop to show the icon for the first time (the new side's `start_tray`, at `COMMIT`).
   *
   * Sets the pending command to start and wakes the condition variable; the loop calls `init` on the main thread,
   * which is the same thread as the caller in the new process (`run_new_side` runs on the main thread before the loop
   * is entered): the command is then executed when the loop starts. Does not wait. In the `suspended` phase, and
   * while a suspend is in progress, it behaves as `resume`.
   *
   * The deferral rule: the handoff-started process must not call this, nor `init_tray`, before `COMMIT`; the glue
   * guarantees it and the glue's tests check it with a fake backend.
   *
   * @return `true` when accepted or already `running`; `false` when `ended`, when the tray is not enabled
   *         (`config::sunshine.system_tray` false or compiled out), or the platform has no main-thread tray.
   *
   * Requirement on the glue (P18): the new process's main thread must enter `run_main_loop` even when `init_tray` never
   * ran (a handoff-started process before `COMMIT`), because this command is executed only there. A normal start whose
   * `init_tray` fails now also idles in the loop, with the 500 ms poll, until shutdown; that is intended and
   * equivalent to the old "wait for shutdown" branch.
   *
   * Thread-safety: any thread, including the main thread before `run_main_loop`; non-blocking.
   */
  bool request_start();

  /**
   * @brief Enable or disable the Quit and Restart menu entries (the handoff keeps them off until `MAIN`).
   *
   * The setting is remembered and applied when the icon is created and updated at once when it is shown (as the other
   * `tray_update` callers, under the tray state mutex). Disabled entries cannot be chosen; the callbacks also refuse
   * through the hooks, so this is only the visible part. The default is enabled.
   *
   * @param enabled Whether Quit and Restart can be chosen.
   *
   * Thread-safety: any thread; non-blocking.
   */
  void set_lifecycle_entries_enabled(bool enabled);

  /**
   * @brief The lifecycle phase.
   *
   * @return The phase. Thread-safety: any thread.
   */
  tray_phase_e current_phase();

  /**
   * @brief Initializes and runs the system tray in a separate thread.
   * @return 0 if initialization was successful, non-zero otherwise.
   */
  int init_tray_threaded();

#ifdef SUNSHINE_TESTS
  /**
   * @brief Get the tray data used by the system tray implementation.
   *
   * @return Read-only tray data for unit-test assertions.
   */
  const struct tray &tray_data_for_testing();

  /**
   * @brief Check whether the system tray is initialized.
   *
   * @return true if the system tray is initialized; otherwise, false.
   */
  bool tray_initialized_for_testing();

  /**
   * @brief Replace the backend entry points the lifecycle drives (a null member restores its production value), for
   * tests of `run_main_loop`, `suspend`, `resume` and `request_start`.
   *
   * With a fake backend the lifecycle code makes no Qt call. A fake `loop` typically blocks on a test condition
   * variable until the fake `exit` is called, as the real one blocks in `QApplication::exec`.
   *
   * @param backend The entry points.
   */
  void set_backend_for_testing(const tray_backend_t &backend);

  /**
   * @brief Return the lifecycle to its fresh state: phase `not_started`, no pending command, Quit and Restart entries
   * enabled, backend production defaults. Not thread-safe: call it when no other thread uses the tray.
   */
  void reset_lifecycle_for_testing();

  /**
   * @brief Make the lifecycle treat the tray as enabled although `config::sunshine.system_tray` is not set, so a unit
   * test can run `request_start` and `resume` with a fake backend.
   *
   * The override works in both directions and replaces `config::sunshine.system_tray` until
   * `reset_lifecycle_for_testing()` clears it: `true` counts as enabled, `false` counts as not enabled (so
   * `request_start` and `resume` return `false`).
   *
   * @param enabled Whether the tray counts as enabled.
   */
  void set_enabled_for_testing(bool enabled);

  /**
   * @brief Restore the persistent tray data to its initial state between tests.
   */
  void reset_tray_data_for_testing();

  /**
   * @brief Resolve a tray resource path using the production platform logic.
   *
   * @param relative_path Resource path relative to the executable or application bundle.
   * @return Stable resource path used by the tray backend.
   */
  const char *resource_path_for_testing(const char *relative_path);

  /**
   * @brief Resolve all tray icon paths using the production platform logic.
   */
  void resolve_tray_icon_paths_for_testing();
#endif
}  // namespace system_tray
