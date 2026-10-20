/**
 * @file src/handoff/settings.h
 * @brief Drain-handoff options, their validation, the UDP bases, and the preconditions for enabling the feature.
 *
 * See `docs/drain-handoff.md`, "Configuration", "Ports" and "GPU".
 */
#pragma once

// standard includes
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

namespace handoff {
  /**
   * @brief Drain-handoff options as read from the config file.
   */
  struct settings_t {
    bool enabled {false};  ///< `drain_handoff`: `enabled` or `disabled`.
    int udp_offset {100};  ///< `drain_handoff_udp_offset`.
    int deadline_s {3600};  ///< `drain_handoff_deadline`, seconds (1 to 86400).
    int ready_timeout_s {30};  ///< `drain_handoff_ready_timeout`, seconds.
    int rollback_window_s {60};  ///< `drain_handoff_rollback_window`, seconds.
    int cooldown_s {120};  ///< `drain_handoff_cooldown`, seconds.
    std::string binary;  ///< `drain_handoff_binary`; empty means the path Sunshine was started from.
    bool control_socket {false};  ///< `control_socket`: `enabled` or `disabled`; serve the control socket for the
                                  ///< `close-app` verb without the drain handoff (see `control_socket_mode()`).
  };

  /**
   * @brief UDP ports of one base: video (`base + 9`), control (`base + 10`) and audio (`base + 11`).
   *
   * @param base UDP base (the configured `port`, or `port + udp_offset`).
   * @return The three ports.
   */
  std::set<std::uint16_t> udp_ports(int base);

  /**
   * @brief The fixed TCP ports for a configured `port`: HTTPS (`port - 5`), HTTP (`port`), web UI (`port + 1`) and
   * RTSP (`port + 21`).
   *
   * @param port Configured `port`.
   * @return The four ports.
   */
  std::set<std::uint16_t> tcp_ports(int port);

  /**
   * @brief The UDP base a new process gets: the one of `port` and `port + udp_offset` that `current_base` is not.
   *
   * @param port Configured `port`.
   * @param udp_offset Configured `drain_handoff_udp_offset`.
   * @param current_base The spawning process's UDP base.
   * @return `port + udp_offset` when `current_base == port`, else `port`.
   */
  int other_udp_base(int port, int udp_offset, int current_base);

  /**
   * @brief UPnP ports to remove after a rollback killed a new process that had mapped its UDP base.
   *
   * @param new_base The killed new process's UDP base.
   * @param old_base The old process's own UDP base.
   * @param port Configured `port` (its TCP ports are shared and never removed).
   * @return `udp_ports(new_base)` minus `udp_ports(old_base)` minus `tcp_ports(port)`; empty when the bases are equal.
   */
  std::set<std::uint16_t> upnp_ports_to_remove(int new_base, int old_base, int port);

  /**
   * @brief Validate the options against the configured `port`.
   *
   * Rejects (first failing rule wins, in this order): a deadline outside 1 to 86400; a ready timeout below 1; a
   * rollback window below 1; a cooldown shorter than the rollback window; a deadline shorter than the rollback
   * window; an offset of 0; any UDP port of either base outside 1024-65535 (checked for every port of both bases
   * before the next rule; computed in 64-bit, so a huge offset cannot overflow); any UDP port of either base equal to a
   * TCP port; the two bases sharing a UDP port.
   *
   * @param settings Options to check.
   * @param port Configured `port`.
   * @return Empty when valid; otherwise a one-line reason naming the option.
   */
  std::string validate(const settings_t &settings, int port);

  /**
   * @brief Facts about the host that decide whether the feature may enable.
   */
  struct enable_context_t {
    std::string capture;  ///< The `capture` setting as configured (may be empty).
    bool fresh_state {false};  ///< The `FRESH_STATE` flag is set.
    bool display_settings_manager {false};  ///< `display_device` was initialised with a non-null settings manager.
    bool runtime_dir_available {false};  ///< `RUNTIME_DIRECTORY` or `XDG_RUNTIME_DIR` is set.
  };

  /**
   * @brief Decide whether an enabled feature may run; fail closed.
   *
   * Refuses (first failing rule wins, in this order): `capture` other than exactly `wlr` (including empty), with the
   * reason "drain handoff is supported only with capture = wlr (capture = <value>)", where an empty value is shown
   * as `<empty>`; `FRESH_STATE`; a display settings manager; no runtime directory.
   *
   * @param context Host facts.
   * @return Empty when the feature may enable; otherwise the Error text to log.
   */
  std::string check_enable(const enable_context_t &context);
}  // namespace handoff
