/**
 * @file src/handoff/control.h
 * @brief The control socket of the active process and the `--drain-handoff` helper.
 *
 * See `docs/drain-handoff.md`, "The control socket" and "systemd unit" (helper exit codes).
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace handoff {
  constexpr std::string_view handoff_verb = "handoff";  ///< The single verb the control socket accepts.

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
  };

  /**
   * @brief One answer on the control socket.
   */
  struct control_reply_t {
    reply_kind_e kind {reply_kind_e::refused};  ///< Kind.
    std::string reason;  ///< Reason for `refused`, `failed` and `kill_pending`; one line, no newline.
    pid_t new_pid {0};  ///< `committed` only: new process pid.
    std::uint64_t new_start_time {0};  ///< `committed` only: new process start time.

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
   * Forms: `refused <reason>`, `failed <reason>`, `kill_pending <reason>`, `committed <pid> <start_time>`. Newlines in
   * a reason are replaced by spaces.
   *
   * @param reply Reply.
   * @return Encoded line.
   */
  std::string encode_reply(const control_reply_t &reply);

  /**
   * @brief Parse a reply line (with or without its trailing `\n`).
   *
   * @param line Line.
   * @return The reply, or `std::nullopt` when it is not one of the four forms (a `committed` line needs a positive
   *         decimal pid and a decimal start time).
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
  };

  /**
   * @brief Exit code for a reply when the helper does not need to wait (every kind but `committed`).
   *
   * @param kind Reply kind.
   * @return `refused` → 1, `failed` → 2, `kill_pending` → 3, `committed` → 0 (the caller then waits and may use 4).
   */
  helper_exit_e exit_code_for(reply_kind_e kind);

  /**
   * @brief Environment and hooks of the helper, injectable for tests.
   */
  struct helper_env_t {
    std::filesystem::path runtime_dir;  ///< Runtime directory.
    std::chrono::milliseconds connect_timeout {5000};  ///< Connect and send timeout.
    std::chrono::milliseconds reply_timeout {75000};  ///< Longest wait for the reply line.
    std::chrono::milliseconds active_timeout {10000};  ///< Wait for role `active` after `committed`.
  };

  /**
   * @brief Run the `--drain-handoff <MAINPID>` helper.
   *
   * Arguments are the ones after `--drain-handoff`: exactly one positive decimal pid, else exit 5. When the pid's
   * control socket does not exist, the helper reads `<runtime dir>/instances/<pid>.json`: role `draining` gives exit 1
   * "a drain is in progress"; state `handover_failed` gives exit 1 "handover failing"; state `kill_pending` gives
   * exit 1 "kill pending"; no file gives exit 1 "no control socket (feature disabled?)". Otherwise it connects, sends
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
   * @brief The active process's control socket server.
   *
   * Listens on `control_socket_path()` with mode 0600 (the parent `control` directory is created with mode 0700).
   * Each connection: the peer is checked with `SO_PEERCRED` (other uid → closed with `refused peer uid`); one line is
   * read (at most 256 bytes, 2 s); anything but exactly `handoff` → `refused unknown verb`; otherwise the handler
   * runs and its reply is written. Connections are served one at a time.
   */
  class control_server_t {
  public:
    /**
     * @brief Handler for the verb; receives the peer pid; runs on the server thread.
     */
    using handler_t = std::function<control_reply_t(pid_t peer_pid)>;

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
     * @brief Stop serving, join the thread and unlink the socket. Safe to call twice and without `start()`.
     */
    void stop();

    /**
     * @brief Calls `stop()`.
     */
    ~control_server_t();

  private:
    std::filesystem::path _path;  ///< Socket path.
    int _listen_fd {-1};  ///< Listening fd.
    int _wake_fd {-1};  ///< Eventfd used to wake the thread on stop.
    std::thread _thread;  ///< Server thread.
  };
}  // namespace handoff
