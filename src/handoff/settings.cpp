/**
 * @file src/handoff/settings.cpp
 * @brief Definitions for `src/handoff/settings.h`.
 */
// standard includes
#include <algorithm>
#include <iterator>
#include <string>

// local includes
#include "settings.h"

namespace handoff {
  /**
   * @brief Lowest UDP port the feature may use.
   */
  constexpr int MIN_UDP_PORT = 1024;

  /**
   * @brief Highest UDP port the feature may use.
   */
  constexpr int MAX_UDP_PORT = 65535;

  /**
   * @brief Largest allowed `drain_handoff_deadline`, in seconds (24 hours).
   */
  constexpr int MAX_DEADLINE_S = 86400;

  /**
   * @brief Whether a port number is inside the UDP range the feature may use.
   *
   * @param port Port number, computed as a wide integer so overflow is caught.
   * @return `true` when the port is between 1024 and 65535.
   */
  static bool udp_port_in_range(int port) {
    return port >= MIN_UDP_PORT && port <= MAX_UDP_PORT;
  }

  std::set<std::uint16_t> udp_ports(int base) {
    // The three media ports of one base; the caller has validated the range.
    return {
      static_cast<std::uint16_t>(base + 9),
      static_cast<std::uint16_t>(base + 10),
      static_cast<std::uint16_t>(base + 11),
    };
  }

  std::set<std::uint16_t> tcp_ports(int port) {
    // HTTPS, HTTP, web UI and RTSP ports, relative to the configured port.
    return {
      static_cast<std::uint16_t>(port - 5),
      static_cast<std::uint16_t>(port),
      static_cast<std::uint16_t>(port + 1),
      static_cast<std::uint16_t>(port + 21),
    };
  }

  int other_udp_base(int port, int udp_offset, int current_base) {
    return current_base == port ? port + udp_offset : port;
  }

  std::set<std::uint16_t> upnp_ports_to_remove(int new_base, int old_base, int port) {
    if (new_base == old_base) {
      return {};
    }

    // Keep only the new base's ports that the old process does not map itself and that are not shared TCP ports.
    auto ports = udp_ports(new_base);
    for (auto port_to_keep : udp_ports(old_base)) {
      ports.erase(port_to_keep);
    }
    for (auto tcp_port : tcp_ports(port)) {
      ports.erase(tcp_port);
    }
    return ports;
  }

  std::string validate(const settings_t &settings, int port) {
    if (settings.deadline_s < 1 || settings.deadline_s > MAX_DEADLINE_S) {
      return "drain_handoff_deadline must be between 1 and 86400 seconds (got " + std::to_string(settings.deadline_s) + ")";
    }
    if (settings.ready_timeout_s < 1) {
      return "drain_handoff_ready_timeout must be at least 1 second (got " + std::to_string(settings.ready_timeout_s) + ")";
    }
    if (settings.rollback_window_s < 1) {
      return "drain_handoff_rollback_window must be at least 1 second (got " + std::to_string(settings.rollback_window_s) + ")";
    }
    if (settings.cooldown_s < settings.rollback_window_s) {
      return "drain_handoff_cooldown (" + std::to_string(settings.cooldown_s) + ") must be at least drain_handoff_rollback_window (" +
             std::to_string(settings.rollback_window_s) + ")";
    }
    if (settings.deadline_s < settings.rollback_window_s) {
      return "drain_handoff_deadline (" + std::to_string(settings.deadline_s) + ") must be at least drain_handoff_rollback_window (" +
             std::to_string(settings.rollback_window_s) + ")";
    }
    if (settings.udp_offset == 0) {
      return "drain_handoff_udp_offset must not be 0, or both UDP bases would be the same";
    }

    // Both bases: the configured port and port + offset. Computed in int so an overflowing offset is caught by the range check.
    const int bases[] = {port, port + settings.udp_offset};
    // The TCP ports as wide integers, so a small configured port cannot wrap into a valid UDP port.
    const int tcp[] = {port - 5, port, port + 1, port + 21};

    for (auto base : bases) {
      // The three UDP ports of each base must be inside the range and must not collide with a TCP port.
      for (int offset : {9, 10, 11}) {
        const int udp_port = base + offset;
        if (!udp_port_in_range(udp_port)) {
          return "drain_handoff_udp_offset puts UDP port " + std::to_string(udp_port) + " outside 1024-65535";
        }
        if (std::find(std::begin(tcp), std::end(tcp), udp_port) != std::end(tcp)) {
          return "drain_handoff_udp_offset puts UDP port " + std::to_string(udp_port) + " on a TCP port of the server";
        }
      }
    }

    // The two bases must not share a UDP port. Only the offset decides this, since the bases differ by it.
    for (auto udp_a : udp_ports(bases[0])) {
      if (udp_ports(bases[1]).count(udp_a)) {
        return "drain_handoff_udp_offset makes the two UDP bases share UDP port " + std::to_string(udp_a);
      }
    }

    return {};
  }

  std::string check_enable(const enable_context_t &context) {
    if (context.capture != "wlr") {
      const std::string shown = context.capture.empty() ? std::string("<empty>") : context.capture;
      return "drain handoff is supported only with capture = wlr (capture = " + shown + ")";
    }
    if (context.fresh_state) {
      return "drain handoff cannot be enabled with the FRESH_STATE flag, because state would never be saved";
    }
    if (context.display_settings_manager) {
      return "drain handoff cannot be enabled with a display settings manager";
    }
    if (!context.runtime_dir_available) {
      return "drain handoff needs RUNTIME_DIRECTORY or XDG_RUNTIME_DIR to be set";
    }
    return {};
  }
}  // namespace handoff
