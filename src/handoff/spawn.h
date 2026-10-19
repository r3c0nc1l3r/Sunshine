/**
 * @file src/handoff/spawn.h
 * @brief Spawning the new process with inherited listeners, validating them in the child, and the kill-first reap.
 *
 * See `docs/drain-handoff.md`, steps 2, 3 and 6.
 */
#pragma once

// standard includes
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace handoff {
  constexpr int first_inherited_fd = 3;  ///< Listeners sit at fds 3-6 in the child.
  constexpr int channel_child_fd = 7;  ///< The channel sits at fd 7 in the child.

  /**
   * @brief The four inherited listeners and the channel, as fds.
   */
  struct inherited_fds_t {
    int https {-1};  ///< Client HTTPS listener.
    int http {-1};  ///< Client HTTP listener.
    int webui {-1};  ///< Web UI listener.
    int rtsp {-1};  ///< RTSP listener.
    int channel {-1};  ///< Handoff channel.
  };

  /**
   * @brief Environment variables a new process starts with, besides the parent's own.
   *
   * @param udp_base UDP base for the new process.
   * @param generation Handoff generation (keeps virtual device ids unique); the first handoff of a unit is 1.
   * @return Entries `NAME=value`, exactly: `SUNSHINE_HANDOFF_FDS=https:3,http:4,webui:5,rtsp:6`,
   *         `SUNSHINE_HANDOFF_CHANNEL=7`, `SUNSHINE_HANDOFF_UDP_BASE=<udp_base>`,
   *         `SUNSHINE_HANDOFF_GENERATION=<generation>`, in this order.
   */
  std::vector<std::string> child_environment(int udp_base, unsigned generation);

  /**
   * @brief Parse the handoff variables in the new process.
   *
   * `fds` must name each of `https`, `http`, `webui` and `rtsp` exactly once (any order), each with a decimal fd of 3
   * or more; `channel` must be a decimal fd of 3 or more; no fd may repeat.
   *
   * @param fds Value of `SUNSHINE_HANDOFF_FDS`.
   * @param channel Value of `SUNSHINE_HANDOFF_CHANNEL`.
   * @param error Receives a one-line reason on failure; may be null.
   * @return The fds, or `std::nullopt` when invalid.
   */
  std::optional<inherited_fds_t> parse_inherited_fds(std::string_view fds, std::string_view channel, std::string *error);

  /**
   * @brief Check that an inherited fd is a listening TCP socket on the expected port and family.
   *
   * @param fd Fd to check.
   * @param port Expected local port.
   * @param ipv6 `true` when the expected family is IPv6 (`address_family = both`), `false` for IPv4.
   * @return Empty when valid; otherwise a one-line reason (not a socket, not TCP, not listening (`SO_ACCEPTCONN`),
   *         other port, other family).
   */
  std::string validate_listener(int fd, int port, bool ipv6);

  /**
   * @brief In the new process, before any other thread starts: set `FD_CLOEXEC` on fds 3-7 and unset every
   * environment variable whose name starts with `SUNSHINE_HANDOFF_`.
   */
  void scrub_inherited();

  /**
   * @brief What to spawn.
   */
  struct spawn_request_t {
    std::string binary;  ///< Executable path.
    std::vector<std::string> argv;  ///< Arguments including argv[0].
    std::vector<std::string> extra_env;  ///< `NAME=value` entries added to (or replacing in) the parent environment.
    inherited_fds_t fds;  ///< Parent-side fds to place at 3-7 in the child.
  };

  /**
   * @brief A spawned child.
   */
  struct child_t {
    pid_t pid {-1};  ///< Child pid (for logs only; always act through the pidfd).
    int pidfd {-1};  ///< Pidfd of the child; the caller owns it.
  };

  /**
   * @brief Spawn the new process.
   *
   * Uses `pidfd_spawn` (or `clone3` with `CLONE_PIDFD`). In the child: the sources are first duplicated to fds of
   * 100 or above, then placed at 3-7 (https, http, webui, rtsp, channel), every other fd above 2 is closed, the signal
   * mask is empty and every signal has its default disposition.
   *
   * @param request What to spawn.
   * @param error Receives a one-line reason on failure; may be null.
   * @return The child, or `std::nullopt` on failure.
   */
  std::optional<child_t> spawn_child(const spawn_request_t &request, std::string *error);

  /**
   * @brief Outcome of the kill-first step.
   */
  enum class reap_result_e {
    reaped,  ///< Killed (or already dead) and reaped here; `exit_status` is valid.
    reaped_elsewhere,  ///< Gone, but another `waitpid(-1)` reaped it (`ECHILD`); exit status unknown.
    timeout,  ///< Still not gone after the timeout (`kill_pending`).
    error,  ///< The pidfd is invalid.
  };

  /**
   * @brief Send `SIGKILL` through the pidfd and wait until the child is gone and reaped.
   *
   * @param pidfd Child pidfd.
   * @param timeout Longest wait for the pidfd to become readable.
   * @param exit_status Receives the `waitid` status when the result is `reaped`; may be null.
   * @return Outcome.
   */
  reap_result_e kill_and_reap(int pidfd, std::chrono::milliseconds timeout, int *exit_status);

  /**
   * @brief Whether the process behind a pidfd has exited (the pidfd is readable). A zombie counts as exited.
   *
   * @param pidfd Pidfd.
   * @return `true` when it has exited or the pidfd is invalid.
   */
  bool pidfd_exited(int pidfd);

  /**
   * @brief Open a pidfd for this process (`pidfd_open(getpid())`), close-on-exec.
   *
   * @return The pidfd, or -1.
   */
  int open_self_pidfd();
}  // namespace handoff
