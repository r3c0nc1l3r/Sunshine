/**
 * @file src/handoff/instance_file.h
 * @brief Per-process instance files for active-port discovery and the consumers' fail-safe rule.
 *
 * See `docs/drain-handoff.md`, "Active-port discovery for external idle checks".
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
#include <vector>

namespace handoff {
  /**
   * @brief Role of a process.
   */
  enum class role_e {
    pending,  ///< A new process before `COMMIT`.
    active,  ///< Accepts new connections.
    draining,  ///< Keeps its sessions, accepts nothing.
  };

  /**
   * @brief Handoff state, as in the session API's `handoff` key.
   */
  enum class state_e {
    none,  ///< No handoff state.
    draining,  ///< A draining process exists.
    handover_failed,  ///< The main-PID handover keeps failing.
    kill_pending,  ///< A rollback waits for the new process to be reaped.
  };

  /**
   * @brief Wire name of a role: `pending`, `active` or `draining`.
   *
   * @param role Role.
   * @return Name.
   */
  std::string_view to_string(role_e role);

  /**
   * @brief Wire name of a state: `none`, `draining`, `handover_failed` or `kill_pending`.
   *
   * @param state State.
   * @return Name.
   */
  std::string_view to_string(state_e state);

  /**
   * @brief Parse a role name.
   *
   * @param name Name.
   * @return The role, or `std::nullopt` for an unknown name.
   */
  std::optional<role_e> parse_role(std::string_view name);

  /**
   * @brief Parse a state name.
   *
   * @param name Name.
   * @return The state, or `std::nullopt` for an unknown name.
   */
  std::optional<state_e> parse_state(std::string_view name);

  /**
   * @brief Content of one instance file.
   */
  struct instance_t {
    pid_t pid {0};  ///< Process id.
    std::uint64_t start_time {0};  ///< Field 22 of `/proc/<pid>/stat`.
    role_e role {role_e::active};  ///< Role.
    int udp_base {0};  ///< The process's UDP base.
    int sessions {0};  ///< Running sessions of the process.
    state_e handoff {state_e::none};  ///< Handoff state.

    /**
     * @brief Compare two instances field by field.
     *
     * @param other Instance to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const instance_t &other) const = default;
  };

  /**
   * @brief Serialize to the file's JSON: `{"pid":..,"start_time":..,"role":"..","udp_base":..,"sessions":..,
   * "handoff":".."}`.
   *
   * @param instance Instance.
   * @return JSON text.
   */
  std::string to_json(const instance_t &instance);

  /**
   * @brief Parse an instance file's JSON. All six members are required with the right types and known names.
   *
   * @param json File content.
   * @return The instance, or `std::nullopt` when invalid.
   */
  std::optional<instance_t> parse_instance(std::string_view json);

  /**
   * @brief Extract field 22 (`starttime`) from the content of `/proc/<pid>/stat`.
   *
   * The command name (field 2) is in parentheses and may contain spaces and `)`; fields are counted after the last
   * `)`.
   *
   * @param stat Content of the stat file.
   * @return The start time, or `std::nullopt` when it cannot be parsed.
   */
  std::optional<std::uint64_t> parse_stat_start_time(std::string_view stat);

  /**
   * @brief Read the start time of a live process from `/proc/<pid>/stat`.
   *
   * @param pid Process id.
   * @return The start time, or `std::nullopt` when the process does not exist or the file cannot be parsed.
   */
  std::optional<std::uint64_t> read_start_time(pid_t pid);

  /**
   * @brief Runtime directory for the control socket and the instance files.
   *
   * @param runtime_directory Value of `RUNTIME_DIRECTORY` (empty when unset; only its first `:`-separated entry is
   *        used).
   * @param xdg_runtime_dir Value of `XDG_RUNTIME_DIR` (empty when unset).
   * @return `RUNTIME_DIRECTORY` when set, else `XDG_RUNTIME_DIR/sunshine`, else `std::nullopt`.
   */
  std::optional<std::filesystem::path> runtime_dir(std::string_view runtime_directory, std::string_view xdg_runtime_dir);

  /**
   * @brief Writes and removes this process's own instance file. Thread-safe.
   */
  class instance_writer_t {
  public:
    /**
     * @brief Create a writer for `<dir>/<pid>.json`. Nothing is written yet.
     *
     * @param dir Instances directory (`<runtime dir>/instances`); created with mode 0700 on first write if missing.
     * @param initial Initial content; `pid` names the file.
     */
    instance_writer_t(std::filesystem::path dir, instance_t initial);

    /**
     * @brief Write the current content (temporary file in the same directory plus rename).
     *
     * @return `true` on success.
     */
    bool write();

    /**
     * @brief Update the role and write.
     *
     * @param role New role.
     * @return `true` on success.
     */
    bool set_role(role_e role);

    /**
     * @brief Update the handoff state and write.
     *
     * @param state New state.
     * @return `true` on success.
     */
    bool set_state(state_e state);

    /**
     * @brief Update the session count and write.
     *
     * @param sessions New count.
     * @return `true` on success.
     */
    bool set_sessions(int sessions);

    /**
     * @brief Current content.
     *
     * @return Copy of the content.
     */
    instance_t current() const;

    /**
     * @brief Delete the file. Safe to call when it does not exist.
     */
    void remove();

  private:
    std::filesystem::path _dir;  ///< Instances directory.
    instance_t _instance;  ///< Current content.
    mutable std::mutex _mutex;  ///< Guards the members.
  };

  /**
   * @brief Remove another process's instance file (the old process's cleanup of a dead new process).
   *
   * @param dir Instances directory.
   * @param pid Pid naming the file.
   */
  void remove_instance_file(const std::filesystem::path &dir, pid_t pid);

  /**
   * @brief Result of reading every instance file.
   */
  struct scan_result_t {
    bool dir_missing {false};  ///< The directory does not exist.
    bool any_unreadable {false};  ///< At least one `*.json` file could not be read or parsed.
    std::vector<instance_t> live;  ///< Files whose pid is alive with the recorded start time.
    std::vector<pid_t> stale;  ///< Pids of files whose process is gone or has another start time (pid reuse).
  };

  /**
   * @brief Read every `*.json` file in the instances directory; other names are ignored.
   *
   * @param dir Instances directory.
   * @param start_time_of Returns a pid's current start time, or `std::nullopt` when it does not exist (tests inject
   *        this; production passes `read_start_time`).
   * @return Scan result.
   */
  scan_result_t scan_instances(const std::filesystem::path &dir, const std::function<std::optional<std::uint64_t>(pid_t)> &start_time_of);

  /**
   * @brief The consumers' fail-safe rule.
   *
   * Busy when: the directory is missing while a Sunshine process runs; a running Sunshine pid has no live file; any
   * file is unreadable; any live file has role `pending`; any live file has `sessions > 0`; any live file has state
   * `kill_pending`. (Traffic and recent RTSP connections are checked by the consumer separately.)
   *
   * @param scan Scan result.
   * @param running_sunshine_pids Pids of running Sunshine processes.
   * @return `true` when Sunshine must be treated as busy.
   */
  bool consumer_busy(const scan_result_t &scan, const std::vector<pid_t> &running_sunshine_pids);

  /**
   * @brief Wait until the instance file of `pid` shows role `active` with the given start time.
   *
   * Polls at least every 100 ms.
   *
   * @param dir Instances directory.
   * @param pid Pid of the new process.
   * @param start_time Its start time, as sent in `committed`.
   * @param timeout Longest wait.
   * @return `true` when seen within the timeout.
   */
  bool wait_for_active(const std::filesystem::path &dir, pid_t pid, std::uint64_t start_time, std::chrono::milliseconds timeout);
}  // namespace handoff
