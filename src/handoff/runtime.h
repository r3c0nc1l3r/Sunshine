/**
 * @file src/handoff/runtime.h
 * @brief Process-wide drain-handoff state: mode, settings, UDP base, generation, flags, instance file, channel.
 *
 * See `docs/drain-handoff.md`, "Configuration", "Ports", step 3, and "Stopping, restarting and quitting". Every
 * other handoff piece reads its state from here. The class is not a singleton by itself, so tests construct their own
 * instance; production code uses `runtime()`.
 *
 * Thread-safety: every member function may be called from any thread at any time, unless its comment says
 * otherwise. Scalar state is held in atomics (sequentially consistent); compound state (settings, instance writer,
 * channel, peer, inherited fds, host sink) is guarded by one mutex that is never held across a call out of this class
 * except into `instance_writer_t`, which has its own lock and never calls back. The only functions that are safe in a
 * signal handler are `note_sigterm()` and `shutdown_wake_fd()`.
 */
#pragma once

// standard includes
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

// local includes
#include "channel.h"
#include "end_all.h"
#include "instance_file.h"
#include "settings.h"
#include "spawn.h"
#include "types.h"

namespace handoff {
  /**
   * @brief Where a process stands in the handoff life cycle.
   *
   * Legal transitions (everything else is refused by `runtime_t::transition()`):
   * - `normal` to `draining` (the old process at the switch) and back (a rollback);
   * - `new_pending` to `new_active` (`COMMIT` installed), and `new_active` to `normal` (`MAIN` received);
   * - `normal` to `disabled` and back are done only by `runtime_t::configure()` and `runtime_t::disable()`.
   */
  enum class mode_e {
    disabled,  ///< The feature is off (option off, or `check_enable` refused): no control socket, no handoff code runs.
    normal,  ///< Feature on; a normal start, or a handoff child that has become the main process. Accepts connections.
    new_pending,  ///< Started by a handoff and not yet committed: nothing adopted, accepting paused, role `pending`.
    new_active,  ///< Started by a handoff, committed, but not yet the main process (before `MAIN`).
    draining,  ///< A former active process keeping its sessions; accepts nothing, has no web UI, no tray.
  };

  /**
   * @brief Wire name of a mode: `disabled`, `normal`, `new_pending`, `new_active` or `draining`.
   *
   * @param mode Mode.
   * @return Name.
   */
  std::string_view to_string(mode_e mode);

  /**
   * @brief The environment `runtime_t::early_init()` works on. Tests inject fakes; production uses `system_environment()`.
   */
  struct early_init_env_t {
    std::function<const char *(const char *)> getenv;  ///< Returns the value of a variable, or null when unset.
    std::function<void(const char *)> unsetenv;  ///< Removes a variable.
    std::function<void()> scrub_fds;  ///< `scrub_inherited()`: `FD_CLOEXEC` on fds 3-7 and removal of every
                                      ///< `SUNSHINE_HANDOFF_*` variable. Called at most once, after parsing.
  };

  /**
   * @brief The real environment: `::getenv`, `::unsetenv` and `scrub_inherited()`.
   *
   * @return The environment.
   */
  early_init_env_t system_environment();

  /**
   * @brief Process-wide handoff state.
   */
  class runtime_t {
  public:
    /**
     * @brief Create the state: mode `normal`, default settings, generation 0, no UDP base override, main process, not
     * frozen, no shutdown requested, no instance file, no channel.
     *
     * The shutdown eventfd (`EFD_CLOEXEC | EFD_NONBLOCK`) is created here; if that fails `shutdown_wake_fd()` is -1.
     */
    runtime_t();

    /**
     * @brief Close the shutdown eventfd.
     */
    ~runtime_t();

    runtime_t(const runtime_t &) = delete;  ///< Not copyable.
    runtime_t &operator=(const runtime_t &) = delete;  ///< Not copyable.

    /**
     * @brief Read the inherited environment. Call once, as the first statement of `main()`, before any thread starts
     * and before the environment is sanitized.
     *
     * Rules:
     * - `SUNSHINE_HANDOFF_FDS` and `SUNSHINE_HANDOFF_CHANNEL` both unset: a normal start. Then `SUNSHINE_UDP_BASE`
     *   (decimal, 1 to 65535) and `SUNSHINE_GENERATION` (decimal, 0 to 65535) are read when set, so that an in-place
     *   restart keeps both; a value that does not parse is ignored (the defaults stay: no override, generation 0).
     *   Mode stays `normal`, `is_main()` stays `true`.
     * - exactly one of the two set, or both set but not accepted by `parse_inherited_fds`, or
     *   `SUNSHINE_HANDOFF_UDP_BASE` (decimal, 1 to 65535) or `SUNSHINE_HANDOFF_GENERATION` (decimal, 1 to 65535)
     *   missing or invalid: an error; nothing is stored, `scrub_fds` is not called, the mode stays `normal`.
     * - both set and valid: a handoff start. `inherited()` holds the fds, `started_by_handoff()` is `true`, the mode
     *   is `new_pending`, `is_main()` is `false`, the UDP base override and generation come from the
     *   `SUNSHINE_HANDOFF_` variables, and `SUNSHINE_HANDOFF_HOST_SINK` (optional, may be empty) is kept for
     *   `host_sink()`.
     *
     * On every non-error path, afterwards: `env.scrub_fds()` is called once, and `SUNSHINE_UDP_BASE` and
     * `SUNSHINE_GENERATION` are removed with `env.unsetenv`, so apps and commands inherit none of them. The restart
     * code sets the two again just before `execv`.
     *
     * @param env Environment to read; tests pass fakes.
     * @return Empty on success; otherwise a one-line reason. The caller prints it to stderr and exits with status 1.
     */
    std::string early_init(const early_init_env_t &env);

    /**
     * @brief Apply the parsed configuration. Call after `apply_config` and after the `validate` and `check_enable`
     * results are known.
     *
     * Stores `settings`. For a normal start the mode becomes `normal` when `settings.enabled` and `disabled`
     * otherwise. After a handoff start the mode is left unchanged (the new side's validation reports the drift).
     * Calling it after a handoff has begun (mode `draining`, `new_active`) only stores the settings.
     *
     * @param settings Options as configured (after validation; an invalid or refused set arrives with `enabled`
     *        false).
     */
    void configure(const settings_t &settings);

    /**
     * @brief Turn the feature off for this process (a failed `check_enable`, privileged execution).
     *
     * Sets `settings().enabled` to `false` and the mode to `disabled`, only while the mode is `normal` or
     * `disabled`.
     *
     * @return `true` when done; `false` when a handoff has already begun (the mode is something else).
     */
    bool disable();

    /**
     * @brief Current mode.
     *
     * @return Mode.
     */
    mode_e mode() const;

    /**
     * @brief Move from one mode to another if, and only if, the process is in `from` and the edge is legal (see
     * `mode_e`). An atomic compare-and-swap.
     *
     * @param from Expected mode.
     * @param to New mode.
     * @return `true` when the mode changed; `false` when the process was not in `from` or the edge is illegal (the
     *         mode is then unchanged).
     */
    bool transition(mode_e from, mode_e to);

    /**
     * @brief Whether the feature is active in this process (`mode()` is anything but `disabled`).
     *
     * @return `true` when enabled.
     */
    bool enabled() const;

    /**
     * @brief Whether this process was started by a handoff (set by `early_init`, never cleared). Keeps lazy input
     * creation and the skipped startup work on for the life of the process.
     *
     * @return `true` for a handoff child.
     */
    bool started_by_handoff() const;

    /**
     * @brief Copy of the settings stored by `configure()` (defaults before the first call).
     *
     * @return Settings.
     */
    settings_t settings() const;

    /**
     * @brief Handoff generation: 0 for a normal start, the value of `SUNSHINE_HANDOFF_GENERATION` for a handoff
     * child, kept across in-place restarts through `SUNSHINE_GENERATION`.
     *
     * @return Generation.
     */
    unsigned generation() const;

    /**
     * @brief The UDP base set by the environment (`SUNSHINE_HANDOFF_UDP_BASE` or `SUNSHINE_UDP_BASE`) or by
     * `set_udp_base()`.
     *
     * @return The base, or `std::nullopt` when this process uses the configured `port`.
     */
    std::optional<int> udp_base() const;

    /**
     * @brief The UDP base to use now: the override, else the configured port.
     *
     * @param configured_port Configured `port`.
     * @return `udp_base()` when set, else `configured_port`.
     */
    int effective_udp_base(int configured_port) const;

    /**
     * @brief Override the UDP base (tests, and the coordinator for a process that keeps its base).
     *
     * @param base UDP base.
     */
    void set_udp_base(int base);

    /**
     * @brief Whether this process is systemd's main process of the unit. `true` at a normal start, `false` in a
     * handoff child until `MAIN`.
     *
     * @return `true` for the main process.
     */
    bool is_main() const;

    /**
     * @brief Set the main-process flag (`MAIN` received sets it, together with the `new_active` to `normal`
     * transition).
     *
     * @param value New value.
     */
    void set_main(bool value);

    /**
     * @brief Whether this process has frozen its state files. Informational for writers outside `nvhttp` (the
     * `apps.json` save, the credentials file); `nvhttp` keeps its own flag (`nvhttp::set_state_frozen`). The glue sets
     * both together.
     *
     * @return `true` while frozen.
     */
    bool frozen() const;

    /**
     * @brief Set the frozen flag.
     *
     * @param value New value.
     */
    void set_frozen(bool value);

    /**
     * @brief Tag for log lines of this process in its current mode: `drain` while the mode is `draining`, else
     * empty. The glue passes it to `logging::set_process_tag`.
     *
     * @return Tag.
     */
    std::string process_tag() const;

    /**
     * @brief Record that `SIGTERM` arrived. Async-signal-safe: only atomic stores and one `write` to the eventfd.
     *
     * Latches the shutdown reason (`stop` when the mode is `draining`, else `terminated`) before it returns, so a
     * caller can raise the shutdown event right after. The first call latches; later calls change nothing.
     */
    void note_sigterm();

    /**
     * @brief Whether `note_sigterm()` has been called.
     *
     * @return `true` after a `SIGTERM`.
     */
    bool sigterm_requested() const;

    /**
     * @brief Reason for session stops during shutdown.
     *
     * @return The latched reason after `note_sigterm()`; before that, `stop` while the mode is `draining` and
     *         `terminated` otherwise.
     */
    shutdown_reason_e shutdown_reason() const;

    /**
     * @brief The eventfd that `note_sigterm()` writes. Threads that block (the coordinator, the notifier) poll it
     * next to their own wake source.
     *
     * @return The fd, or -1 if it could not be created. Owned by the runtime; do not close it.
     */
    int shutdown_wake_fd() const;

    /**
     * @brief The inherited fds of a handoff start.
     *
     * @return The fds, or `std::nullopt` for a normal start.
     */
    std::optional<inherited_fds_t> inherited() const;

    /**
     * @brief The host's own default audio sink as passed by the old process (`SUNSHINE_HANDOFF_HOST_SINK`).
     *
     * @return The name, or `std::nullopt` for a normal start or when the variable was unset or empty.
     */
    std::optional<std::string> host_sink() const;

    /**
     * @brief Create this process's instance writer and write the first file.
     *
     * Replaces (without removing the file of) an earlier writer.
     *
     * @param dir Instances directory.
     * @param initial Initial content (`pid`, `start_time`, role, UDP base, sessions, state).
     * @return `true` when the first write succeeded; the writer stays installed either way.
     */
    bool open_instance(std::filesystem::path dir, instance_t initial);

    /**
     * @brief Update the role in the instance file. Does nothing and returns `false` when no writer is installed.
     *
     * @param role New role.
     * @return `true` when written.
     */
    bool instance_set_role(role_e role);

    /**
     * @brief Update the handoff state in the instance file. Does nothing and returns `false` without a writer.
     *
     * @param state New state.
     * @return `true` when written.
     */
    bool instance_set_state(state_e state);

    /**
     * @brief Update the session count in the instance file. Does nothing and returns `false` without a writer.
     *
     * @param sessions New count.
     * @return `true` when written.
     */
    bool instance_set_sessions(int sessions);

    /**
     * @brief Current content of the instance file.
     *
     * @return The content, or `std::nullopt` without a writer.
     */
    std::optional<instance_t> instance_current() const;

    /**
     * @brief Delete the instance file and drop the writer (the next `instance_*` calls do nothing). Safe without a
     * writer.
     */
    void instance_remove();

    /**
     * @brief Store the process's end of the handoff channel (the new process after `HELLO`, the old process while
     * a handoff runs).
     *
     * @param channel Channel, or null to drop it.
     */
    void set_channel(std::shared_ptr<channel_t> channel);

    /**
     * @brief The channel.
     *
     * @return The channel, or null.
     */
    std::shared_ptr<channel_t> channel() const;

    /**
     * @brief Store the active process's view of its draining peer (see `peer_link_t`).
     *
     * @param peer Peer, or null to drop it.
     */
    void set_peer(std::shared_ptr<drain_peer_t> peer);

    /**
     * @brief The draining-peer view.
     *
     * @return The peer, or null when none exists.
     */
    std::shared_ptr<drain_peer_t> peer() const;

  private:
    std::atomic<mode_e> _mode {mode_e::normal};  ///< Mode.
    std::atomic<bool> _started_by_handoff {false};  ///< Set by `early_init`.
    std::atomic<unsigned> _generation {0};  ///< Generation.
    std::atomic<int> _udp_base {0};  ///< UDP base override; 0 means none.
    std::atomic<bool> _is_main {true};  ///< Main-process flag.
    std::atomic<bool> _frozen {false};  ///< Frozen flag.
    std::atomic<bool> _sigterm {false};  ///< `SIGTERM` seen.
    std::atomic<int> _latched_reason {-1};  ///< Latched `shutdown_reason_e`, or -1.
    int _wake_fd {-1};  ///< Eventfd written by `note_sigterm`; fixed after construction.
    mutable std::mutex _mutex;  ///< Guards the members below.
    settings_t _settings;  ///< Stored settings.
    std::optional<inherited_fds_t> _inherited;  ///< Inherited fds.
    std::optional<std::string> _host_sink;  ///< Host sink from the old process.
    std::shared_ptr<instance_writer_t> _instance;  ///< Instance writer.
    std::shared_ptr<channel_t> _channel;  ///< Channel.
    std::shared_ptr<drain_peer_t> _peer;  ///< Draining peer.
  };

  /**
   * @brief The process-wide instance (function-local static, constructed on first use, which is `early_init` in
   * `main()`).
   *
   * @return The runtime.
   */
  runtime_t &runtime();
}  // namespace handoff
