/**
 * @file src/handoff/sd_notify.h
 * @brief Raw systemd notifications (`READY=1`, `MAINPID=`, `BARRIER=1`) and unit discovery, without libsystemd.
 *
 * See `docs/drain-handoff.md`, step 7 and "systemd unit".
 */
#pragma once

// standard includes
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace handoff::sd {
  /**
   * @brief Send one notification datagram to `NOTIFY_SOCKET`.
   *
   * A path starting with `@` is an abstract socket (the `@` becomes a leading NUL byte).
   *
   * @param notify_socket Value of `NOTIFY_SOCKET`; empty means not running under systemd (returns `false`).
   * @param payload Payload, for example `READY=1`.
   * @param attach_fd Fd to attach with `SCM_RIGHTS`, or -1.
   * @return `true` when the datagram was sent.
   */
  bool notify(std::string_view notify_socket, std::string_view payload, int attach_fd = -1);

  /**
   * @brief Payload that hands the main PID over.
   *
   * @param pid New main pid.
   * @return `MAINPID=<pid>`.
   */
  std::string mainpid_payload(pid_t pid);

  /**
   * @brief Send `BARRIER=1` with the write end of a fresh pipe and wait until the read end reports end-of-file.
   *
   * @param notify_socket Value of `NOTIFY_SOCKET`.
   * @param timeout Longest wait.
   * @return `true` when the barrier completed within the timeout.
   */
  bool barrier(std::string_view notify_socket, std::chrono::milliseconds timeout);

  /**
   * @brief Unit of this process, from the content of `/proc/self/cgroup`.
   *
   * Uses the cgroup v2 line (`0::<path>`). The unit is the last path component that ends in `.service`.
   *
   * @param cgroup Content of `/proc/self/cgroup`.
   * @return Unit name, or `std::nullopt` when none is found.
   */
  std::optional<std::string> unit_from_cgroup(std::string_view cgroup);

  /**
   * @brief Whether the cgroup path is in a user manager (`/user.slice/user-<uid>.slice/user@<uid>.service/...`).
   *
   * @param cgroup Content of `/proc/self/cgroup`.
   * @return `true` for a user unit.
   */
  bool is_user_unit(std::string_view cgroup);

  /**
   * @brief Version from the first line of `systemctl --version` (`systemd <n> (<extra>)`).
   *
   * @param output Command output.
   * @return The major version, or `std::nullopt`.
   */
  std::optional<int> parse_systemd_version(std::string_view output);

  /**
   * @brief Parse `systemctl show -p MainPID --value` output (a decimal pid and a newline).
   *
   * @param output Command output.
   * @return The pid (0 means none), or `std::nullopt` when not a number.
   */
  std::optional<pid_t> parse_mainpid(std::string_view output);

  /**
   * @brief Run `systemctl [--user] show -p MainPID --value <unit>` with a timeout and parse it.
   *
   * @param unit Unit name.
   * @param user_unit `true` for `--user`.
   * @param timeout Longest run time; the command is killed after it.
   * @return The main pid, or `std::nullopt` on failure or timeout.
   */
  std::optional<pid_t> read_back_mainpid(const std::string &unit, bool user_unit, std::chrono::milliseconds timeout);
}  // namespace handoff::sd
