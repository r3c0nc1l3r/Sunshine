/**
 * @file src/handoff/new_ops.h
 * @brief The Sunshine implementation of `new_side_ops_t`: what each op does in production, the deferral invariant, and
 * the milestones that let `main()` and the new-side thread run side by side.
 *
 * See `docs/drain-handoff.md`, step 3 ("New process start"), the `COMMIT` paragraph of step 5, "systemd unit"
 * (readiness) and "Other resources". `new_side.h` owns the protocol order (`run_new_side`); this header owns the
 * effects. `entry.h` owns the thread that calls `run_new_side` and the order of `main()`. Linux only.
 *
 * ## Deferral invariant (binding)
 *
 * Until `COMMIT` is read in time, the process published nothing, mapped nothing and showed nothing. Concretely, these
 * four ops do nothing observable before `install_adoption()` has been called, and nothing in this class calls them
 * earlier:
 * - `publish_mdns()` is the only code in the process that starts mDNS in a handoff-started process, and it runs only
 *   as a `COMMIT` effect (`main()` takes no mDNS handle, see `entry.h`);
 * - `map_upnp()` is the only place a handoff-started process calls `upnp::start()` (`main()` skips its own call when
 *   `hooks::started_by_handoff()`);
 * - `start_tray()` never calls `init_tray()`: it asks the main loop to start the tray (`system_tray::request_start()`),
 *   and the loop is entered with no tray (`entry.h`);
 * - `become_active()` is the only writer of role `active`. Until then the instance file says role `pending` and the
 *   runtime mode is `new_pending` (it becomes `new_active` at the start of `install_adoption()`, which is what
 *   `hooks::quit_blocked()` and `hooks::restart_blocked()` need, since both treat the two modes alike).
 *
 * A process that exits before `COMMIT` (any `exit_failure`, or an abort) therefore leaves no mDNS entry, no UPnP
 * mapping, no tray icon and no `active` file behind. The tests of this class check the deferral with fakes: the
 * `publish_mdns`, `start_upnp` and `request_tray_start` seams are called only after `install_adoption`, the role in
 * `runtime().instance_current()` is `pending` until `become_active()`, and the seams are never called when
 * `run_new_side` returns `exit_failure`.
 *
 * ## Threads and locks
 *
 * Every `new_side_ops_t` op runs on the new-side thread (the thread that runs `run_new_side`, see `entry.h`), one at
 * a time. The milestone members (`wait_probe_decision`, `wait_listeners_validated`, `server_listening`,
 * `servers_failed`, `abort`, `stage`) are thread-safe and are called from `main()`'s thread, the servers' listening
 * callbacks and the shutdown path. No op holds a lock of this class while it calls a seam, and no seam is called under
 * the slots lock, the clear lock or the runtime's mutex, so a seam may take any of those itself. An op may call the
 * `runtime()` accessors (each takes the runtime's mutex briefly and never calls back). No op calls `run_new_side`,
 * `abort()` or `on_main()`. The `resume_servers` seam posts to the servers' io threads and waits, so it must never
 * run on one of them; the new-side thread is not one.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// local includes
#include "channel.h"
#include "new_side.h"
#include "protocol.h"
#include "src/platform/common.h"

namespace handoff {
  /**
   * @brief How far the new process has come; read by the shutdown path and by the overlap rules in `entry.h`.
   *
   * Only moves forward. Set by the ops themselves, at the points named on each value.
   */
  enum class new_stage_e {
    starting,  ///< Constructed; `run_new_side` has not returned from `hello()` and the old `HELLO` is not accepted yet.
    ready,  ///< `start_servers_paused()` returned `""` (the process then sends `READY` ok).
    go,  ///< `reload_all()` was called (a `GO` arrived).
    committing,  ///< `install_adoption()` was called (`COMMIT` was read in time).
    committed,  ///< `become_active()` returned: every `COMMIT` effect ran. Waiting for `MAIN`.
    main,  ///< `on_main()` ran: this process is systemd's main process.
  };

  /**
   * @brief Result of `sunshine_new_ops_t::wait_listeners_validated()`.
   */
  enum class validation_e {
    ok,  ///< `validate_listeners()` returned `""`: `main()` may start the servers in adopted, paused mode.
    failed,  ///< It returned a reason, or the new side ended before it ran: `main()` must not start the servers.
    timeout,  ///< Neither happened within the timeout.
  };

  /**
   * @brief Fixed facts the ops need from the configuration; copied at construction.
   */
  struct new_ops_config_t {
    int port {0};  ///< Configured `port` (`config::sunshine.port`): the TCP ports are `port - 5` (HTTPS), `port` (HTTP), `port + 1` (web UI) and `port + 21` (RTSP).
    bool ipv6 {false};  ///< `true` when the effective address family is not IPv4 (`address_family = both`); passed to `validate_listener`.
    bool feature_enabled {false};  ///< `config::drain_handoff.enabled` after `apply_config` (false when `config_refusal` refused).
    std::string startup_refusal;  ///< Non-empty when `hooks::enable_refusal(...)` refused at startup (see `entry.h`); `validate_listeners()` returns it.
    bool tray_enabled {false};  ///< `tray_is_enabled && config::sunshine.system_tray` (false on Windows/macOS builds and with the tray off).
    std::chrono::milliseconds servers_listening_timeout {30000};  ///< Longest `start_servers_paused()` waits for all four listening callbacks.
    std::chrono::milliseconds resume_timeout {5000};  ///< Finite timeout passed to every server's resume at `COMMIT`.
  };

  /**
   * @brief The platform calls the ops make. A null member takes its production default (named on each member); tests
   * pass fakes. Every seam is called on the new-side thread unless noted, never under a lock of the class.
   */
  struct new_ops_seams_t {
    /**
     * @brief Identity of this host for the probe comparison. Default: `gather_probe().identity` (`probe.h`).
     */
    std::function<protocol::probe_identity_t()> own_identity;

    /**
     * @brief Install the accepted probe results as the `/serverinfo` override. Default: convert with the mapping in
     * `probe.h` (`accepted_probe` documentation) and call `video::set_handoff_probe_override`.
     *
     * Called at most once, from `accept_old_hello`, and only with results that `accepted_probe()` accepted.
     */
    std::function<void(const protocol::probe_results_t &)> set_probe_override;

    /**
     * @brief Store the cooldown deadline the old process sent (`CLOCK_MONOTONIC` nanoseconds). Default: the
     * coordinator's cooldown setter (`coordinator.h`), which makes `cooldown_active` true until then.
     */
    std::function<void(std::int64_t)> set_cooldown_deadline_ns;

    /**
     * @brief `validate_listener(fd, port, ipv6)`. Default: `handoff::validate_listener` (`spawn.h`).
     */
    std::function<std::string(int, int, bool)> validate_listener;

    /**
     * @brief Reload `sunshine_state.json`, the credentials file and `apps.json` under `client_auth_mutex`.
     *
     * Default: `nvhttp::reload_state()` (any status but `loaded` fails), `reload_user_creds` and the `apps.json`
     * parse plus `proc::swap_app_list`, in this order, stopping at the first failure. Returns `""` on success;
     * otherwise a one-line reason that names the file and the status (for example `sunshine_state.json: no_uniqueid`).
     * A missing state file or a newly generated host unique id is a failure, never a success.
     */
    std::function<std::string()> reload_all;

    /**
     * @brief Check the snapshot against the reloaded app list and keep the prepared plan inside the seam. Default:
     * `proc::proc.prepare_adoption(go, reason)`; the plan is kept by the production closure until `install_adoption`.
     * Returns `""` on success or the reason (the spec's "app or prep commands changed while an app runs; cancel the
     * app first" for a mismatch).
     */
    std::function<std::string(const protocol::go_t &)> prepare_adoption;

    /**
     * @brief Install the prepared plan: `proc::proc.install_adoption(plan)` (when the snapshot had an app), set
     * `host_audio` (`nvhttp::set_host_audio`) and the next launch session id (`nvhttp::set_next_launch_session_id`).
     * Runs once, on the new-side thread, before the process accepts any request.
     */
    std::function<void()> install_adoption;

    /**
     * @brief Resume accepting on all four servers (HTTPS, HTTP, web UI, RTSP) with `resume_timeout` each.
     *
     * Default: the server control functions that `sunshine_old_ops_t::resume_accept` also uses (`coordinator.h`),
     * started for the four servers at once, so that the whole call takes at most one `timeout` (not four). Returns the
     * names of the servers whose resume did not confirm (empty on success). All four are tried even if one fails.
     */
    std::function<std::vector<std::string>(std::chrono::milliseconds)> resume_servers;

    /**
     * @brief Start mDNS. Default: `handoff::mdns_holder().start(true)` (rename checks armed, `mdns_holder.h`). Returns
     * the holder's result (`false` when unavailable, which is a Warning, not a failure).
     */
    std::function<bool()> publish_mdns;

    /**
     * @brief Start UPnP. Default: `upnp::start(hooks::udp_base())`. The returned guard (null when UPnP is off) is
     * stored in the slot given to the constructor.
     */
    std::function<std::unique_ptr<platf::deinit_t>()> start_upnp;

    /**
     * @brief Show the tray. Default: when `new_ops_config_t::tray_enabled`,
     * `system_tray::set_lifecycle_entries_enabled(false)` and then `system_tray::request_start()` (in this order, so
     * the icon never appears with Quit and Restart enabled); otherwise nothing. Never `init_tray()` (the deferral
     * invariant).
     */
    std::function<void()> request_tray_start;

    /**
     * @brief Allow Quit and Restart in the tray again. Default: `system_tray::set_lifecycle_entries_enabled(true)`
     * when `tray_enabled`. Called by `on_main()` (any thread of the post-`COMMIT` reader).
     */
    std::function<void()> enable_tray_entries;

    /**
     * @brief Tell systemd the service is ready. Default: `sd::notify(getenv("NOTIFY_SOCKET"), "READY=1")`, reading
     * `NOTIFY_SOCKET` at call time (nothing scrubs it). An unset variable makes it do nothing. Called by `on_main()`.
     */
    std::function<void()> notify_ready;

    /**
     * @brief Start the control socket of this process, which has just become the main process. Default: the
     * coordinator's start call (`coordinator.h`), with the same handler as a normal start; does nothing when the
     * feature is disabled. Called by `on_main()`.
     */
    std::function<void()> start_control_server;

    /**
     * @brief Exit the process with a status. Default: `lifetime::exit_sunshine(code, true)` (it raises the shutdown
     * event and returns; the normal shutdown path then runs). Tests must replace it: it must never really exit.
     */
    std::function<void(int)> exit_process;

    /**
     * @brief Half-close the channel so a blocked `receive` returns at once. Default: `::shutdown(channel->fd(),
     * SHUT_RDWR)` when the fd is not -1. Called by `abort()` from any thread.
     */
    std::function<void(channel_t &)> shutdown_channel;
  };

  /**
   * @brief Production `new_side_ops_t`.
   *
   * Construct one per handoff-started process in `main()` (see `entry.h`), after the configuration was parsed and
   * before the new-side thread starts.
   */
  class sunshine_new_ops_t: public new_side_ops_t {
  public:
    /**
     * @brief Create the ops with the production seams.
     *
     * @param config Configuration facts.
     * @param channel The process's end of the channel (`inherited().channel`, wrapped by the caller). Used by
     *        `abort()`; stored into `runtime().set_channel()` by `accept_old_hello`.
     * @param upnp_slot Where `map_upnp()` stores the UPnP guard: `main()`'s `upnp_unmap` variable, so the guard is
     *        destroyed by the normal shutdown path. Written only by the new-side thread; `main()` reads it only after
     *        that thread was joined.
     */
    sunshine_new_ops_t(const new_ops_config_t &config, std::shared_ptr<channel_t> channel, std::unique_ptr<platf::deinit_t> &upnp_slot);

#ifdef SUNSHINE_TESTS
    /**
     * @brief Create the ops with injected seams (tests only). A null member of `seams` takes its production default,
     * so a test must set every seam its scenario reaches (in particular `exit_process`, `publish_mdns`, `start_upnp`,
     * `request_tray_start`, `resume_servers` and `shutdown_channel`, whose defaults act on the real process).
     *
     * @param config Configuration facts.
     * @param channel Channel (a test socketpair end).
     * @param upnp_slot Guard slot.
     * @param seams Platform calls.
     */
    sunshine_new_ops_t(const new_ops_config_t &config, std::shared_ptr<channel_t> channel, std::unique_ptr<platf::deinit_t> &upnp_slot, const new_ops_seams_t &seams);
#endif

    /**
     * @brief Close the old process's pidfd if `take_old_pidfd()` did not take it.
     */
    ~sunshine_new_ops_t() override;

    sunshine_new_ops_t(const sunshine_new_ops_t &) = delete;  ///< Not copyable.
    sunshine_new_ops_t &operator=(const sunshine_new_ops_t &) = delete;  ///< Not copyable.

    /**
     * @brief Own `HELLO`: the default `protocol::hello_t` (current version, no capabilities required, no probe results,
     * no identity, no cooldown). Sets the stage `starting`.
     *
     * @return The message.
     */
    protocol::hello_t hello() override;

    /**
     * @brief Take the old `HELLO` and pidfd. In this order:
     * 1. Store the pidfd (owned from here; closed by `take_old_pidfd()`'s caller or the destructor) and
     *    `runtime().set_channel(channel)`.
     * 2. Cooldown: when `hello.cooldown_deadline_ns` is set, call `set_cooldown_deadline_ns` with it.
     * 3. Probe: `accepted_probe(hello, own_identity())`; when it returns a value, call `set_probe_override` with it
     *    (so `hooks::startup_probe_skipped()` is true from here on); when it returns `std::nullopt` do nothing (no
     *    override, so the startup probe runs).
     * 4. Release the `wait_probe_decision` waiters. This is the last action, so a waiter that returns `true` sees the
     *    override already installed. Nothing here logs an Error; a rejected probe identity is one Info line.
     *
     * Never fails and never blocks.
     *
     * @param hello Old process's `HELLO`.
     * @param old_pidfd Old process's pidfd.
     */
    void accept_old_hello(const protocol::hello_t &hello, int old_pidfd) override;

    /**
     * @brief Validate the four inherited listeners against the configuration.
     *
     * Returns, in this order, the first of: `startup_refusal` when it is not empty; "drain handoff is disabled in the
     * new process's configuration" when `feature_enabled` is false; for each of HTTPS (`port - 5`), HTTP (`port`),
     * web UI (`port + 1`) and RTSP (`port + 21`), in this order, the reason `validate_listener(fd, port, ipv6)` gives
     * prefixed by the server's name (`https: ...`); otherwise `""`. Releases the `wait_listeners_validated` waiters
     * with `ok` or `failed` before it returns. Reads `runtime().inherited()`; it is a programming error to call it in
     * a process that was not started by a handoff (returns "not started by a handoff").
     *
     * @return Empty when valid; otherwise the reason (sent in a failed `READY`).
     */
    std::string validate_listeners() override;

    /**
     * @brief Wait until the servers that `main()` started in adopted, paused mode are all listening.
     *
     * Waits for four `server_listening()` calls (HTTPS, HTTP, web UI, RTSP) for at most
     * `servers_listening_timeout`, and also returns when `servers_failed()` was called, `abort()` was called, or
     * `runtime().shutdown_requested()` became true (polled every 100 ms). The servers are paused by construction
     * (`start_adopted(fd, true, ...)`, `rtsp_stream::adopt_listener(fd, true)`), so this op does not pause anything.
     * Sets the stage `ready` on success.
     *
     * @return `""` on success; otherwise a one-line reason: the one given to `servers_failed`, or "servers did not
     *         start listening within <n> s (<k> of 4)", or "shutdown requested".
     */
    std::string start_servers_paused() override;

    /**
     * @brief On `GO`: `reload_all` seam. Sets the stage `go` first.
     *
     * @return Empty on success; otherwise the reason (sent in a failed `GO_ACK`).
     */
    std::string reload_all() override;

    /**
     * @brief On `GO`: `prepare_adoption` seam. Installs nothing.
     *
     * @param go The `GO` message.
     * @return Empty on success; otherwise the reason.
     */
    std::string prepare_adoption(const protocol::go_t &go) override;

    /**
     * @brief First `COMMIT` effect. Under the milestone mutex: if `abort()` already ran, install nothing, set
     * `commit_failed()` (so this and every later effect is a no-op, the role stays `pending`) and return; otherwise set
     * the stage `committing`. Then move the runtime mode from `new_pending` to `new_active`
     * (`runtime().transition`) and call the `install_adoption` seam. After it the process may run an `undo` command
     * (a terminate of the adopted app). Taking the stage and the abort flag under one mutex makes the race between a
     * shutdown and `COMMIT` deterministic: either the adoption is installed and the channel stays open (so the shutdown
     * can still send `END_ALL`), or nothing is installed.
     */
    void install_adoption() override;

    /**
     * @brief Second `COMMIT` effect: `resume_servers(resume_timeout)` (one bounded call for all four). A non-empty answer is one Error naming the
     * servers ("resume did not confirm: <names>"), sets `commit_failed()`, and makes the three ops below do nothing
     * (a server that does not accept must not be announced). It does not exit by itself: `entry.h` reads
     * `commit_failed()` after `run_new_side` returned `committed`, completes the post-`COMMIT` wiring (so the shutdown
     * path can still ask the draining process to end its sessions before it terminates the adopted app) and then
     * exits with status 1.
     */
    void resume_accept() override;

    /**
     * @brief Third `COMMIT` effect: `publish_mdns` seam (mDNS, with the rename checks armed). Does nothing after a
     * failed `resume_accept()`.
     */
    void publish_mdns() override;

    /**
     * @brief Fourth `COMMIT` effect: `start_upnp` seam; the guard goes into the slot. A handoff-started process starts
     * UPnP here and nowhere else. Does nothing after a failed `resume_accept()`, and does not touch the slot when the
     * seam returns null (UPnP off).
     */
    void map_upnp() override;

    /**
     * @brief Fifth `COMMIT` effect: `request_tray_start` seam (Quit and Restart disabled until `MAIN`). Does nothing
     * after a failed `resume_accept()`.
     */
    void start_tray() override;

    /**
     * @brief Last `COMMIT` effect. Writes role `active` and state `draining` to the instance file (a draining process
     * exists by definition; `session_notifier_t` keeps the state current from then on, see `drain.h`) and sets the stage
     * `committed`. Does nothing to the role after a failed `resume_accept()` (the role stays `pending`), but still sets
     * the stage.
     */
    void become_active() override;

    /**
     * @brief The `MAIN` message was read (called by the post-`COMMIT` reader, never by `run_new_side`).
     *
     * Idempotent (a second call does nothing). In this order: `runtime().transition(new_active, normal)` and
     * `runtime().set_main(true)`; `enable_tray_entries`; `notify_ready` (the only `READY=1` a handoff-started process
     * ever sends); `start_control_server`; stage `main`. A call before the stage is `committed` is ignored with a
     * Warning (a `MAIN` cannot precede `COMMIT`; a confused peer must not turn the process into the main process).
     * Takes no lock while it calls a seam.
     */
    void on_main();

    /**
     * @brief Give the post-`COMMIT` wiring the old process's pidfd (for `peer_link_t`).
     *
     * @return The pidfd, or -1 when none was accepted or it was already taken. The caller owns it from here.
     */
    int take_old_pidfd();

    /**
     * @brief Wait until the probe decision is made: the old `HELLO` was processed by `accept_old_hello`, or the new side
     * ended without one (`mark_finished`). `main()` calls this before its startup probe gate.
     *
     * @param timeout Longest wait.
     * @return `true` when decided (the override is installed or definitely will not be); `false` on timeout (the
     *         caller then lets the probe run).
     */
    bool wait_probe_decision(std::chrono::milliseconds timeout);

    /**
     * @brief Wait until `validate_listeners()` has run, or the new side ended.
     *
     * @param timeout Longest wait.
     * @return `ok`, `failed` or `timeout`; see `validation_e`.
     */
    validation_e wait_listeners_validated(std::chrono::milliseconds timeout);

    /**
     * @brief One server is listening (adopted, paused). Called by the four listening callbacks; the fourth call wakes
     * `start_servers_paused()`. Extra calls are ignored. Never blocks; callable on a server's io thread.
     */
    void server_listening();

    /**
     * @brief The servers cannot start (for example `http::init` failed). Wakes `start_servers_paused()` with `reason`
     * and `wait_listeners_validated()` with `failed` if it still waits. The first reason wins.
     *
     * @param reason One line.
     */
    void servers_failed(const std::string &reason);

    /**
     * @brief Called by the new-side thread when `run_new_side` returned (either outcome): releases every waiter
     * (`wait_probe_decision` with `true`, `wait_listeners_validated` with `failed` if validation never ran) and wakes
     * `start_servers_paused()` if it somehow still waits.
     */
    void mark_finished();

    /**
     * @brief Abort a new side that has not committed: the `shutdown_channel` seam, so a blocked `receive` in
     * `run_new_side` returns `eof` and the outcome is `exit_failure`. A no-op when the stage is `committing` or later
     * (the channel is then needed by the draining peer's `END_ALL` at shutdown) and on a second call. Thread-safe;
     * callable from the shutdown path. Does not call `exit_process`.
     */
    void abort();

    /**
     * @brief Exit with status 1 after `run_new_side` returned `exit_failure`.
     *
     * Calls the `exit_process` seam with 1 exactly once (a second call does nothing), and `mark_finished()`. Nothing was
     * adopted, accepted, published, mapped or shown, so no `undo` runs and the normal shutdown path only joins the
     * servers (paused) and removes the instance file. Called by the new-side thread (`entry.h`), and by `entry.h` for
     * the `commit_failed()` case (there after the post-`COMMIT` wiring).
     */
    void exit_failure();

    /**
     * @brief Whether a `COMMIT` effect failed (see `resume_accept()`).
     *
     * @return `true` after a failed resume.
     */
    bool commit_failed() const;

    /**
     * @brief The stage reached so far.
     *
     * @return The stage; lock-free.
     */
    new_stage_e stage() const;

  private:
    new_ops_config_t _config;  ///< Configuration facts.
    new_ops_seams_t _seams;  ///< Seams with defaults filled in.
    std::shared_ptr<channel_t> _channel;  ///< The channel (for `abort()` and `runtime().set_channel()`).
    std::unique_ptr<platf::deinit_t> &_upnp_slot;  ///< Where `map_upnp()` stores the guard.
    std::atomic<new_stage_e> _stage {new_stage_e::starting};  ///< Stage.
    std::atomic<bool> _commit_failed {false};  ///< A `COMMIT` effect failed.
    std::atomic<bool> _aborted {false};  ///< `abort()` ran.
    std::atomic<bool> _exited {false};  ///< `exit_failure()` ran.
    std::atomic<int> _old_pidfd {-1};  ///< The old process's pidfd until taken.
    mutable std::mutex _mutex;  ///< Guards the milestone fields below; never held across a seam call.
    std::condition_variable _cv;  ///< Wakes the milestone waiters.
    bool _hello_done {false};  ///< `accept_old_hello` ran or the side finished.
    std::optional<validation_e> _validation;  ///< Result of `validate_listeners()` or `failed` on finish.
    int _listening {0};  ///< Number of `server_listening()` calls.
    std::optional<std::string> _servers_failed;  ///< First reason given to `servers_failed()`.
    bool _finished {false};  ///< `mark_finished()` ran.
  };
}  // namespace handoff
