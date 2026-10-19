/**
 * @file src/process.h
 * @brief Declarations for the startup and shutdown of the apps started by a streaming Session.
 */
#pragma once

#ifndef __kernel_entry
  /**
   * @def __kernel_entry
   * @brief Macro for kernel entry.
   */
  #define __kernel_entry
#endif

// standard includes
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unordered_map>
#include <utility>
#include <vector>

// lib includes
#include <boost/process/v1.hpp>

// local includes
#include "config.h"
#include "platform/common.h"
#include "rtsp.h"
#include "utility.h"

#ifdef __linux__
  #include "handoff/app_digest.h"
  #include "handoff/protocol.h"
#endif

/**
 * @def DEFAULT_APP_IMAGE_PATH
 * @brief Macro for DEFAULT APP IMAGE PATH.
 */
#define DEFAULT_APP_IMAGE_PATH SUNSHINE_ASSETS_DIR "/box.png"

namespace proc {
  /**
   * @brief Boost.Process pipe stream used for child-process I/O.
   */
  using file_t = util::safe_ptr_v2<FILE, int, fclose>;

  /**
   * @brief Parsed command arguments used when launching a child process.
   */
  typedef config::prep_cmd_t cmd_t;

  /**
   * pre_cmds -- guaranteed to be executed unless any of the commands fail.
   * detached -- commands detached from Sunshine
   * cmd -- Runs indefinitely until:
   *    No session is running and a different set of commands it to be executed
   *    Command exits
   * working_dir -- the process working directory. This is required for some games to run properly.
   * cmd_output --
   *    empty    -- The output of the commands are appended to the output of sunshine
   *    "null"   -- The output of the commands are discarded
   *    filename -- The output of the commands are appended to filename
   */
  struct ctx_t {
    std::vector<cmd_t> prep_cmds;  ///< Prep cmds.

    /**
     * Some applications, such as Steam, either exit quickly, or keep running indefinitely.
     *
     * Apps that launch normal child processes and terminate will be handled by the process
     * grouping logic (wait_all). However, apps that launch child processes indirectly or
     * into another process group (such as UWP apps) can only be handled by the auto-detach
     * heuristic which catches processes that exit 0 very quickly, but we won't have proper
     * process tracking for those.
     *
     * For cases where users just want to kick off a background process and never manage the
     * lifetime of that process, they can use detached commands for that.
     */
    std::vector<std::string> detached;

    std::string name;  ///< Human-readable name for this item.
    std::string cmd;  ///< Command line used to launch the application.
    std::string working_dir;  ///< Working dir.
    std::string output;  ///< Captured output from the launched process.
    std::string image_path;  ///< Image path.
    std::string id;  ///< Stable identifier for the configured application.
    bool elevated;  ///< Whether the process should be launched elevated.
    bool auto_detach;  ///< Whether the process should detach automatically.
    bool wait_all;  ///< Whether Sunshine waits for all child processes.
    std::chrono::seconds exit_timeout;  ///< Exit timeout.
  };

  /**
   * @brief The fields of the running app that are read from other threads, held as atomics.
   *
   * These are the only copies of the app id and of placebo mode (the plain `_app_id` and `placebo` fields of earlier
   * versions are gone), so the control loop's `running()`, the `/serverinfo` handler (which calls `running()` and stays
   * open while the process drains), the handoff threads and `drop_released_app()` can read and write them without a
   * data race. `execute()` sets `app_id` after the app is chosen, `applied` each time a prep command has run, `pgid`
   * after the launch and `placebo` when the command is empty; `running()` sets `placebo` on the auto-detach flip;
   * `terminate()` and `drop_released_app()` clear everything (`app_id` becomes -1 as before, and `placebo`, `pgid`,
   * `applied`, `released` and `adopted` are cleared). Non-atomic members (`_app`, `_env`, `_apps`, the prep iterators, `_pipe`,
   * `_process`, `_process_group`) are still owned by the thread that runs `execute()`, `terminate()` or the documented
   * handoff calls. All accesses are sequentially consistent. Atomics are not movable, and `proc::refresh` replaces the
   * whole `proc_t` by move assignment, so this wrapper has a move constructor and a move assignment that copy the
   * current values one by one (not as a whole; a move happens only in `refresh`, when no other thread uses the old
   * object). It is not copyable. `proc::refresh` (the apps.json saves in `confighttp.cpp`) already races with `running()`
   * outside a handoff, because it replaces the whole object while the control loop polls it; this change does not make
   * that worse, and during a handoff the 503 gate keeps the saves out.
   */
  struct app_atomics_t {
    std::atomic<int> app_id {0};  ///< Running app id: 0 at start, -1 after a terminate, above 0 while an app is recorded.
    std::atomic<bool> placebo {false};  ///< The app is in placebo mode.
    std::atomic<long> pgid {0};  ///< Process group id of the app, or 0 when it has none.
    std::atomic<std::uint32_t> applied {0};  ///< Number of applied prep commands (`_app_prep_it - _app_prep_begin`).
    std::atomic<bool> released {false};  ///< The app was released to another process (`set_released`).
    std::atomic<bool> adopted {false};  ///< The running app was adopted from another process (`install_adoption`).

    /**
     * @brief Create cleared state.
     */
    app_atomics_t() = default;

    /**
     * @brief Copy the current values of another instance, one by one: `app_id`, `placebo`, `pgid`, `applied`,
     * `released` and `adopted`.
     *
     * @param other Source.
     */
    app_atomics_t(app_atomics_t &&other) noexcept;

    /**
     * @brief Copy the current values of another instance, one by one: `app_id`, `placebo`, `pgid`, `applied`,
     * `released` and `adopted`.
     *
     * @param other Source.
     * @return This object.
     */
    app_atomics_t &operator=(app_atomics_t &&other) noexcept;

    app_atomics_t(const app_atomics_t &) = delete;  ///< Not copyable.
    app_atomics_t &operator=(const app_atomics_t &) = delete;  ///< Not copyable.
  };

  /**
   * @brief Tells whether a process group still has a process.
   *
   * The production probe (used when none is given) is `kill(-pgid, 0)`: `pgid <= 1` means none; success or `EPERM`
   * means a process is present (zombies count, a reused pgid gives a false "present", which refuses safely); `ESRCH`
   * means none. Tests inject their own.
   */
  using group_probe_t = std::function<bool(pid_t pgid)>;

  /**
   * @brief Names of the launch environment variables that `execute()` sets (prefix `SUNSHINE_APP_` or
   * `SUNSHINE_CLIENT_`).
   *
   * @param name Environment variable name.
   * @return `true` when the name starts with `SUNSHINE_APP_` or `SUNSHINE_CLIENT_`.
   */
  bool is_launch_env_name(std::string_view name);

  /**
   * @brief The base environment for the app digest: the entries of the process environment plus the `apps.json`
   * `env` section, without the launch variables (names for which `is_launch_env_name` is true), without
   * `SUNSHINE_UDP_BASE` and `SUNSHINE_GENERATION` (restart variables) and without names starting with
   * `SUNSHINE_HANDOFF_`. Both processes compute the digest from this, so values that differ only by process or launch
   * do not break adoption. The result is sorted by name (byte-wise).
   *
   * Both processes must compute the digest from an environment that has not changed since `proc::parse` built it, apart
   * from the stripped names: `execute()` adds only launch variables, which are stripped. Any other later change to
   * `_env` (or a differing process environment between the two processes) makes the digests differ and the adoption
   * fail, safely.
   *
   * @param env The `proc_t` environment.
   * @return Name and value pairs.
   */
  std::vector<std::pair<std::string, std::string>> base_env_for_digest(const boost::process::v1::environment &env);

#ifdef __linux__
  /**
   * @brief The digest inputs of one app in an environment.
   *
   * `prep` is `app.prep_cmds` mapped to (`do_cmd`, `undo_cmd`, `elevated`) in order (the list is already merged with
   * the global commands when the apps are loaded); `working_dir` and `output` are the app's; `exit_timeout_s` is
   * `app.exit_timeout` in seconds; `base_env` is `base_env_for_digest(env)`.
   *
   * @param app App.
   * @param env The `proc_t` environment.
   * @return Digest inputs for `handoff::app_digest()`.
   */
  handoff::app_digest_input_t digest_input(const ctx_t &app, const boost::process::v1::environment &env);

  /**
   * @brief An adoption that was checked and prepared but not installed. Plain data; movable and copyable.
   */
  struct adoption_plan_t {
    bool has_app {false};  ///< `false` when `GO` carried no app: installing it does nothing.
    int app_id {0};  ///< App id to adopt.
    ctx_t app {};  ///< Copy of the app from the new process's own app list.
    std::uint32_t applied_prep {0};  ///< Number of applied prep commands.
    std::vector<std::pair<std::string, std::string>> launch_env;  ///< Launch environment from the snapshot.
  };
#endif

  /**
   * @brief Tracks launched child processes and terminates them during shutdown.
   */
  class proc_t {
  public:
    KITTY_DEFAULT_CONSTR_MOVE_THROW(proc_t)

    /**
     * @brief Construct a process manager.
     *
     * @param env Environment used when launching processes.
     * @param apps Application launch contexts.
     */
    proc_t(
      boost::process::v1::environment &&env,
      std::vector<ctx_t> &&apps
    ):
        _env(std::move(env)),
        _apps(std::move(apps)) {
    }

    /**
     * @brief Launch the configured application process.
     *
     * Clears the `adopted` flag (an app started here is never adopted), together with the other per-app atomics it
     * resets at its start.
     *
     * @param app_id App ID.
     * @param launch_session Launch session.
     * @return Process exit code or launch error status.
     */
    int execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session);

    /**
     * @brief Whether an app is running.
     *
     * Thread-safe for the placebo branch: it loads `placebo` and then `app_id` (both atomic) and returns the id when it
     * is above 0, else 0, so a concurrent `drop_released_app()` or `terminate()` makes it return the old id or 0, never
     * -1 or a torn value. When `placebo` is false and the app id is 0 or below while `_process` is still valid (an
     * app dropped by `drop_released_app()`), it only resets `_process` and `_process_group` and returns 0: it never
     * calls `terminate()`, so no gamepad, process group, tray or display function runs and the auto-detach check is
     * not reached. Otherwise it behaves as before (the non-atomic `_process` and `_process_group` are read and written
     * by the control-loop thread only; `drop_released_app()` never touches them).
     *
     * @return The app id if an app is running, otherwise `0`.
     */
    int running();

    ~proc_t();

    /**
     * @brief Return the configured applications.
     *
     * @return Immutable application list owned by the process manager.
     */
    const std::vector<ctx_t> &get_apps() const;
    /**
     * @brief Return the configured applications.
     *
     * @return Mutable application list owned by the process manager.
     */
    std::vector<ctx_t> &get_apps();
    /**
     * @brief Get app image.
     *
     * @param app_id App ID.
     * @return Validated image path for the requested application.
     */
    std::string get_app_image(int app_id);
    /**
     * @brief Get last run app name.
     *
     * @return Name of the most recently launched application.
     */
    std::string get_last_run_app_name();
    /**
     * @brief Terminate the launched application process.
     *
     * When the app is released (`set_released(true)`), it only drops the app state: the released flag is cleared,
     * `placebo` is false, the prep iterators are set to `_app_prep_begin` (so no `undo` command can run), the output
     * pipe is closed, `_process` and `_process_group` are reset, the app id is set to -1 as an ordinary terminate
     * does and the atomics are cleared, with the released flag cleared last. It neither
     * calls `terminate_process_group` (there is none: only adoptable apps are handed off), nor runs any `undo`
     * command, nor calls `input::terminate_gamepads`, `system_tray::update_tray_stopped` or
     * `display_device::revert_configuration`. Otherwise it behaves as before and also clears the atomics.
     */
    void terminate();

    /**
     * @brief Whether the running app (if any) can be handed to another process.
     *
     * Drain handoff, spec step 1 ("adoptable") and switch substep 4. Reads only the atomics and the group
     * probe; never takes a lock, never calls `running()` (which can terminate the app and run `undo` commands) and
     * never changes any state, so any thread may call it at any time.
     *
     * - No app recorded (the atomic app id is 0 or below): adoptable (there is nothing to hand over); `why` is untouched.
     * - The app is not in placebo mode: not adoptable, `why` = "the running app is not in placebo mode". This includes
     *   a non-placebo app whose process has exited but has not been polled yet.
     * - Placebo mode and a process group id above 1 for which `probe(pgid)` is true: not adoptable, `why` = "the
     *   running app still has processes in its process group".
     * - Otherwise adoptable.
     *
     * Compiled on every platform (it reads only atomics), but only the Linux handoff code calls it. Where
     * `kill(-pgid, 0)` does not exist (Windows) the process group id is always 0, so the production probe is never
     * asked and the answer depends on the placebo flag alone.
     *
     * @param why Receives the reason when not adoptable; may be null.
     * @param probe Process-group probe; an empty function uses the production probe (see @ref group_probe_t).
     * @return `true` when adoptable.
     */
    bool adoptable(std::string *why, const group_probe_t &probe = {}) const;

    /**
     * @brief Release or take back the running app.
     *
     * The old process sets the flag right before it sends `GO`, and clears it when a rollback takes the app back.
     * `running()` ignores the flag, so the control loop still sees the app and its sessions keep streaming. Only
     * `terminate()` reads it (see there). Lock-free; callable from any thread.
     *
     * @param released `true` to release, `false` to take back.
     */
    void set_released(bool released);

    /**
     * @brief Whether the app is released.
     *
     * @return The flag.
     */
    bool released() const;

    /**
     * @brief Whether the running app was adopted from another process rather than launched by this one.
     *
     * Set by `install_adoption` (with the other atomics) and cleared by `terminate()`, `drop_released_app()` and
     * `execute()` (a new launch). An app this process launched, including a placebo app with no process group, returns
     * `false`; with no app running it returns `false`. Lock-free; callable from any thread.
     *
     * @return The flag.
     */
    bool adopted() const;

    /**
     * @brief Drop a released app's state without running anything (a rollback after `ENDED`, and the exit of a
     * draining process).
     *
     * If the app is released: first sets the non-atomic prep iterators to `_app_prep_begin` and closes the output pipe
     * while `placebo` and the released flag are still set, then clears the atomics (`app_id` set to -1, `applied`,
     * `pgid`, `placebo`) and clears the released flag last. It runs no `undo`
     * command, terminates no process group, calls no tray, gamepad or display function. It writes only the atomics above and
     * members that no other thread reads while the app is released (`_app_prep_it`, `_pipe`; `terminate()` is not
     * reached by `running()` in placebo mode), and never assigns `_process` or `_process_group`. The caller is the
     * handoff thread (the rollback worker, or the draining process's exit path) with the 503 gate up; it may run
     * concurrently with the control loop's `running()` and with `/serverinfo`, which see the old id or 0 (see
     * `running()`).
     * If the app is not released it does nothing, so it can never skip an `undo` by mistake. The caller logs the
     * Warning.
     *
     * @return `true` when a released app was dropped; `false` when nothing was released.
     */
    bool drop_released_app();

    /**
     * @brief Replace only the application list (the old process, on rollback), not the whole `proc_t`.
     *
     * `proc::refresh` replaces the whole object while the control loop polls `running()`; this replaces `_apps` and
     * nothing else (`_app`, the environment, the atomics and the iterators stay), so a running app keeps its own copy
     * of its prep list and a changed `apps.json` does not change what its `undo` runs. Same rule as adoption: the
     * caller guarantees that no request that reads or changes the list is running (the 503 gate is up and the in-flight
     * counters are zero) and takes no lock here.
     *
     * @param apps The new application list.
     */
    void swap_app_list(std::vector<ctx_t> &&apps);

    /**
     * @brief The environment's base entries for the digest, from this object's environment.
     *
     * @return `proc::base_env_for_digest(_env)`. Same caller rule as `snapshot()` (no concurrent `execute()`).
     */
    std::vector<std::pair<std::string, std::string>> digest_base_env() const;

#ifdef __linux__
    /**
     * @brief Take the snapshot that `GO` carries.
     *
     * Precondition (the caller's, not checked): the gate is up, the in-flight requests have finished and `adoptable()`
     * was true, so nothing changes `_app`, `_env` or the atomics during the call.
     *
     * @return `std::nullopt` when no app is recorded (atomic app id 0 or below). Otherwise `app_snapshot_t{app_id = the atomic app
     *         id, name = _app.name, applied_prep = the atomic applied count, digest = handoff::app_digest(digest_input(_app,
     *         _env)), launch_env = the entries of _env for which is_launch_env_name is true, sorted by name}`.
     */
    std::optional<handoff::protocol::app_snapshot_t> snapshot() const;

    /**
     * @brief Check a received snapshot against this process's own app list and prepare the adoption (the new process,
     * at `GO`). Installs nothing, starts nothing, changes no member.
     *
     * Rules, first failing wins; `reason` receives the text:
     * - `go.app` is empty: success with a plan whose `has_app` is `false`.
     * - this object already records a running app: "this process already runs an app".
     * - the snapshot's digest is empty, or the computed digest is empty: "invalid app snapshot".
     * - `_apps` has no entry with `id == std::to_string(app_id)` and `name == snapshot.name`, or its
     *   `digest_input` digest differs from the snapshot's (compared as given, lowercase hex), or the applied count is
     *   larger than that entry's `prep_cmds.size()`: "app or prep commands changed while an app runs; cancel the app
     *   first". This is the reason the old process logs and the helper reports.
     *
     * @param go The `GO` message.
     * @param reason Receives the failure reason; cleared on success.
     * @return The plan, or `std::nullopt` with `reason` set.
     */
    std::optional<adoption_plan_t> prepare_adoption(const handoff::protocol::go_t &go, std::string &reason) const;

    /**
     * @brief Install a prepared adoption (the new process, at `COMMIT`).
     *
     * Does nothing when `plan.has_app` is `false`. Otherwise sets the atomic app id, `_app` (the plan's copy, which also gives
     * `get_last_run_app_name()`), `_app_prep_begin` (the begin of `_app.prep_cmds`), `_app_prep_it` (begin plus
     * `applied_prep`), placebo mode, and the environment: each launch entry is stored in `_env`; then reopens the app's
     * `output` file for appending when it is neither empty nor `null` (a failure logs a Warning and continues without
     * a pipe). Sets the atomics (app id, placebo true, process group 0, applied count, released false, adopted true). Starts no
     * process, runs no command and sends no tray notification. Precondition: it runs before the process accepts any
     * request, on the plan from `prepare_adoption()` of this same object with no `refresh` or `swap_app_list` in
     * between; no lock is taken.
     *
     * @param plan The prepared adoption.
     */
    void install_adoption(adoption_plan_t &&plan);
#endif

#ifdef SUNSHINE_TESTS
    /**
     * @brief Test-only: make the object look like it runs an app, without starting anything.
     *
     * Sets `_app` to `app`, the atomic app id to `app_id`, the placebo flag, the process group, and the prep
     * iterators and the atomic applied count to `applied_prep` (clamped to the prep
     * list size). Does not touch `_env`.
     *
     * @param app The app (its `prep_cmds` are used).
     * @param app_id App id.
     * @param is_placebo Placebo mode.
     * @param pgid Process group id (0 for none).
     * @param applied_prep Number of applied prep commands.
     */
    void set_running_for_test(const ctx_t &app, int app_id, bool is_placebo, pid_t pgid, std::uint32_t applied_prep);

    /**
     * @brief Test-only: set an environment variable of the object.
     *
     * @param name Name.
     * @param value Value.
     */
    void set_env_for_test(const std::string &name, const std::string &value);
#endif

  private:
    app_atomics_t _live;  ///< Atomic running-app fields (see @ref app_atomics_t); replace the old `_app_id`, `placebo`.

    boost::process::v1::environment _env;
    std::vector<ctx_t> _apps;
    ctx_t _app;
    std::chrono::steady_clock::time_point _app_launch_time;

    boost::process::v1::child _process;
    boost::process::v1::group _process_group;

    file_t _pipe;
    std::vector<cmd_t>::const_iterator _app_prep_it;
    std::vector<cmd_t>::const_iterator _app_prep_begin;
  };

  /**
   * @brief Calculate a stable id based on name and image data
   * @return Tuple of id calculated without index (for use if no collision) and one with.
   *
   * @param app_name App name.
   * @param app_image_path App image path.
   * @param index Zero-based index of the item being addressed.
   */
  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index);

  bool check_valid_png(const std::filesystem::path &path);
  /**
   * @brief Validate app image path.
   *
   * @param app_image_path Candidate image path from the application configuration.
   * @return Existing PNG path, or the default application image when validation fails.
   */
  std::string validate_app_image_path(std::string app_image_path);
  /**
   * @brief Refresh cached platform state from the operating system.
   *
   * @param file_name File name.
   */
  void refresh(const std::string &file_name);
  /**
   * @brief Parse serialized text into the corresponding runtime representation.
   *
   * @param file_name File name.
   * @return Parsed value or parse status.
   */
  std::optional<proc::proc_t> parse(const std::string &file_name);

  /**
   * @brief Initialize proc functions
   * @return Unique pointer to `deinit_t` to manage cleanup
   */
  std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Terminates all child processes in a process group.
   * @param proc The child process itself.
   * @param group The group of all children in the process tree.
   * @param exit_timeout The timeout to wait for the process group to gracefully exit.
   */
  void terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout);

  extern proc_t proc;
}  // namespace proc
