/**
 * @file src/handoff/control.h
 * @brief The control socket of the active process, the `--drain-handoff` helper and the `--close-app` helper.
 *
 * See `docs/drain-handoff.md`, "The control socket" and "systemd unit" (helper exit codes).
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace handoff {
  constexpr std::string_view handoff_verb = "handoff";  ///< Verb: start a drain handoff.
  constexpr std::string_view close_app_verb = "close-app";  ///< Verb: close the running app, like `/api/apps/close`.

  /**
   * @brief Which control socket a process serves.
   */
  enum class control_socket_mode_e {
    none,  ///< No control socket.
    handoff,  ///< Both verbs; the drain handoff is enabled (the coordinator serves `handoff`).
    close_app_only,  ///< Only `close-app`; `handoff` is refused with `drain handoff is disabled`.
  };

  /**
   * @brief Decide which control socket a normal start serves.
   *
   * `drain_handoff = enabled` implies the control socket. Under privileged execution (for example file capabilities)
   * the drain handoff is refused at startup, so `handoff_enabled` is `false` there and `control_socket = enabled`
   * still gives `close_app_only`.
   *
   * @param handoff_enabled `runtime().enabled()` after the startup checks (the `drain_handoff` option and every enable
   *        check passed).
   * @param control_socket The `control_socket` option (`settings_t::control_socket`).
   * @return `handoff` when `handoff_enabled`; else `close_app_only` when `control_socket`; else `none`.
   */
  control_socket_mode_e control_socket_mode(bool handoff_enabled, bool control_socket);

  /**
   * @brief Path of a process's control socket: `<runtime dir>/control/<pid>.sock`.
   *
   * @param runtime_dir Runtime directory (see `runtime_dir()`).
   * @param pid Process id.
   * @return Socket path.
   */
  std::filesystem::path control_socket_path(const std::filesystem::path &runtime_dir, pid_t pid);

  /**
   * @brief Kind of answer the active process gives the helper.
   */
  enum class reply_kind_e {
    refused,  ///< Refused before anything started (exit 1).
    failed,  ///< Failed and rolled back (exit 2).
    kill_pending,  ///< The rollback could not reap the new process (exit 3).
    committed,  ///< `COMMIT` sent; the helper then waits for role `active` (exit 0 or 4).
    closed,  ///< `close-app` ran (exit 0); `closed_app` tells whether an app was running.
  };

  /**
   * @brief One answer on the control socket.
   */
  struct control_reply_t {
    reply_kind_e kind {reply_kind_e::refused};  ///< Kind.
    std::string reason;  ///< Reason for `refused`, `failed` and `kill_pending`; one line, no newline.
    pid_t new_pid {0};  ///< `committed` only: new process pid.
    std::uint64_t new_start_time {0};  ///< `committed` only: new process start time.
    bool closed_app {false};  ///< `closed` only: `true` when an app was running and was closed (`closed app`),
                              ///< `false` when none was running (`closed none`).

    /**
     * @brief Compare two replies field by field.
     *
     * @param other Reply to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const control_reply_t &other) const = default;
  };

  /**
   * @brief Encode a reply as one line ending in `\n`.
   *
   * Forms: `refused <reason>`, `failed <reason>`, `kill_pending <reason>`, `committed <pid> <start_time>`,
   * `closed app`, `closed none` (`closed` ignores `reason`). `\n` and
   * `\r` in a reason are replaced by spaces, and a reason longer than 1000 bytes is cut to its first 1000 bytes, so a
   * line always fits the helper's 4096-byte reply limit.
   *
   * @param reply Reply.
   * @return Encoded line.
   */
  std::string encode_reply(const control_reply_t &reply);

  /**
   * @brief Parse a reply line (with or without its trailing `\n`).
   *
   * @param line Line.
   * @return The reply, or `std::nullopt` when it is not one of the forms (a `committed` line needs a positive
   *         decimal pid and a decimal start time; a `closed` line is exactly `closed app` or `closed none`).
   */
  std::optional<control_reply_t> parse_reply(std::string_view line);

  /**
   * @brief Helper exit codes (see the table in the spec).
   */
  enum class helper_exit_e : int {
    switched = 0,  ///< Committed and role `active` observed.
    refused = 1,  ///< Refused; nothing changed.
    failed = 2,  ///< Failed and rolled back.
    kill_pending = 3,  ///< The rollback could not reap the new process.
    unconfirmed = 4,  ///< Committed, but role `active` was not observed within the bound.
    error = 5,  ///< Usage or internal error.
    unsupported = 6,  ///< `--close-app` only: the process does not serve the verb (`refused unknown verb`), or there
                      ///< is no control socket and no instance file (feature off, or an older binary).
  };

  /**
   * @brief Exit code for a reply when the helper does not need to wait (every kind but `committed`).
   *
   * @param kind Reply kind.
   * @return `refused` → 1, `failed` → 2, `kill_pending` → 3, `committed` → 0 (the caller then waits and may use 4),
   *         `closed` → 0.
   */
  helper_exit_e exit_code_for(reply_kind_e kind);

  /**
   * @brief Environment and hooks of the helper, injectable for tests.
   */
  struct helper_env_t {
    std::filesystem::path runtime_dir;  ///< Runtime directory.
    std::chrono::milliseconds connect_timeout {5000};  ///< Connect and send timeout.
    std::chrono::milliseconds reply_timeout {65000};  ///< Longest wait for the reply line: `ready_timeout +
                                                      ///< ping_timeout + 25 s`, so that with `active_timeout` the
                                                      ///< helper stays within the spec's bound of `ready_timeout +
                                                      ///< ping_timeout + 35 s` (75 s at the defaults).
    std::chrono::milliseconds active_timeout {10000};  ///< Wait for role `active` after `committed`.
  };

  /**
   * @brief Run the `--drain-handoff <MAINPID>` helper.
   *
   * Arguments are the ones after `--drain-handoff`: exactly one positive decimal pid, else exit 5. When the pid's
   * control socket does not exist, the helper reads `<runtime dir>/instances/<pid>.json` and checks, in this order:
   * state `kill_pending` gives exit 1 "kill pending"; state `handover_failed` gives exit 1 "handover failing"; role
   * `draining` gives exit 1 "a drain is in progress"; any other content, or no readable file, gives exit 1 "no control
   * socket (feature disabled?)". (A process in `handover_failed` or `kill_pending` keeps role `draining`, so the state
   * is checked first.) Otherwise it connects, sends
   * `handoff\n`, reads one reply line, and maps it with `exit_code_for()`; for `committed` it calls
   * `wait_for_active()` and returns 0 or 4. A reply timeout or an unparsable reply is exit 5. The reason is written to
   * `err` as one line.
   *
   * @param args Arguments after `--drain-handoff`.
   * @param env Environment.
   * @param err Receives the human-readable outcome line.
   * @return Exit code.
   */
  helper_exit_e run_helper(const std::vector<std::string> &args, const helper_env_t &env, std::string &err);

  /**
   * @brief Run the `--close-app <MAINPID>` helper.
   *
   * Arguments are the ones after `--close-app`: exactly one positive decimal pid, else exit 5 (`error`). When the pid's
   * control socket does not exist, the helper reads `<runtime dir>/instances/<pid>.json` exactly as `run_helper` does:
   * state `kill_pending` → exit 1 "kill pending"; state `handover_failed` → exit 1 "handover failing"; role `draining` →
   * exit 1 "a drain is in progress"; any other content, or no readable file → exit 6 (`unsupported`) "no control
   * socket (feature disabled?)". Otherwise it connects (within `env.connect_timeout`), sends `close-app\n`, and reads
   * one reply line within `env.reply_timeout` (the CLI sets 60 s). It does not wait for role `active`
   * (`env.active_timeout` is unused).
   *
   * Replies: `closed app` → 0 with `err` = "closed app"; `closed none` → 0 with `err` = "closed none";
   * `refused unknown verb` → 6 (`unsupported`, an older binary); any other `refused <reason>` → 1 with the reason;
   * `failed` → 2; `kill_pending` → 3; `committed` (never sent for this verb), a reply timeout, a connect failure or an
   * unparsable reply → 5. The outcome goes to `err` as one line.
   *
   * Exit codes 1 and 5 are worth a bounded retry (a rollback window, the terminate gate); 0 and 6 are final.
   *
   * @param args Arguments after `--close-app`.
   * @param env Environment.
   * @param err Receives the human-readable outcome line.
   * @return Exit code.
   */
  helper_exit_e run_close_app_helper(const std::vector<std::string> &args, const helper_env_t &env, std::string &err);

#ifdef SUNSHINE_TESTS
  /**
   * @brief Test-only: make the control server see `uid` as the peer uid of every connection instead of the one from
   * `SO_PEERCRED` (the peer pid still comes from `SO_PEERCRED`), or restore the real value with `std::nullopt`.
   *
   * Unit tests cannot connect as another user; this seam lets them pin the uid check. It is process-wide, read once
   * per connection, and has no effect unless set. It exists only in test builds (`SUNSHINE_TESTS`), so the production
   * binary has no way to bypass the uid check.
   *
   * @param uid Uid to report, or `std::nullopt`.
   */
  void set_peer_uid_override_for_tests(std::optional<uid_t> uid);
#endif

  /**
   * @brief The active process's control socket server.
   *
   * Listens on `control_socket_path()` with mode 0600 (the parent `control` directory is created with mode 0700).
   * Each connection: the peer is checked with `SO_PEERCRED` (other uid → closed with `refused peer uid`, and no
   * handler runs); one line is read (at most 256 bytes, 2 s). Exactly `handoff`: the `handoff` handler runs, or, when
   * it is empty, the reply is `refused drain handoff is disabled`. Exactly `close-app`: the `close_app` handler runs,
   * or, when it is empty, the reply is `refused unknown verb`. Anything else → `refused unknown verb`. The handler's
   * reply is written. Connections are served one at a time; a second helper waits in the backlog.
   *
   * The old process closes its control socket at the switch (`close_listener()`, from the handoff flow while the
   * handler still waits for the outcome) and opens it again after a rollback (`reopen_listener()`). All members are
   * guarded by one mutex that is never held while the handler runs or while joining; every method is safe to call from
   * any thread, including the server thread inside the handler, except where noted.
   */
  class control_server_t {
  public:
    /**
     * @brief Handler for the verb; receives the peer pid; runs on the server thread.
     */
    using handler_t = std::function<control_reply_t(pid_t peer_pid)>;

    /**
     * @brief The handler of each verb; an empty one refuses that verb (see the class description).
     */
    struct handlers_t {
      handler_t handoff;  ///< `handoff`; empty → `refused drain handoff is disabled`.
      handler_t close_app;  ///< `close-app`; empty → `refused unknown verb`.
    };

    /**
     * @brief Bind the socket and start serving on a thread. A stale socket file at the path is replaced.
     *
     * @param path Socket path.
     * @param handler Verb handler.
     * @param error Receives a one-line reason on failure; may be null.
     * @return `true` on success.
     */
    bool start(const std::filesystem::path &path, handler_t handler, std::string *error);

    /**
     * @brief Bind the socket and start serving on a thread, with one handler per verb. A stale socket file at the path
     * is replaced. `start(path, handler, error)` is `start(path, {handler, {}}, error)`.
     *
     * @param path Socket path.
     * @param handlers Verb handlers.
     * @param error Receives a one-line reason on failure; may be null.
     * @return `true` on success.
     */
    bool start(const std::filesystem::path &path, handlers_t handlers, std::string *error);

    /**
     * @brief Stop accepting: unlink the socket path (only if it is still the socket this server bound, checked with
     * `lstat` and the inode), close the listening fd, and return at once. The thread keeps running; a running handler
     * finishes and its reply is still written; no new connection is accepted. Safe to call twice, without `start()`,
     * and from the handler.
     */
    void close_listener();

    /**
     * @brief Listen again on the path given to `start()` after `close_listener()` (a stale socket file is replaced, as
     * in `start()`). The thread serves it as soon as a running handler has returned. Safe to call from the handler.
     *
     * @param error Receives a one-line reason on failure; may be null.
     * @return `true` when listening (also when it already was); `false` when not started, after `stop()`, or when the
     *         bind fails.
     */
    bool reopen_listener(std::string *error);

    /**
     * @brief Stop serving: `close_listener()`, wake the thread and join it. Safe to call twice and without `start()`.
     *
     * Called on the server thread (from the handler) it does not join; the thread then exits after the handler returns
     * and a later `stop()` or the destructor on another thread joins it. Called on another thread while a handler runs,
     * it returns only when the handler has returned, so the caller must make the handler return first (the handoff
     * glue calls `old_side_t::request_shutdown()` before `stop()` on the shutdown path). Concurrent `stop()` calls from
     * two threads are not supported (only one of them joins), and the destructor must not overlap any other call.
     */
    void stop();

    /**
     * @brief Calls `stop()`.
     */
    ~control_server_t();

  private:
    std::filesystem::path _path;  ///< Socket path.
    int _listen_fd {-1};  ///< Listening fd; -1 while closed.
    int _wake_fd {-1};  ///< Eventfd used to wake the thread on stop and on listener changes.
    std::thread _thread;  ///< Server thread.
    handlers_t _handlers;  ///< Verb handlers.
    bool _stopped {false};  ///< `stop()` was called.
    dev_t _bound_dev {0};  ///< Device of the socket file this server bound (for `close_listener()`).
    ino_t _bound_ino {0};  ///< Inode of the socket file this server bound; 0 when none.
    mutable std::mutex _mutex;  ///< Guards the members above.
  };
}  // namespace handoff
