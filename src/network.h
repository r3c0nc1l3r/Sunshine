/**
 * @file src/network.h
 * @brief Declarations for networking related functions.
 */
#pragma once

// standard includes
#include <tuple>
#include <utility>

// lib includes
#include <boost/asio.hpp>
#include <enet/enet.h>

// local includes
#include "utility.h"

namespace net {
  /**
   * @brief Destroy an ENet host allocated by host_create().
   *
   * @param host Host name or address to resolve.
   */
  void free_host(ENetHost *host);

  /**
   * @brief Map a specified port based on the base port.
   * @param port The port to map as a difference from the base port.
   * @return The mapped port number.
   * @examples
   * std::uint16_t mapped_port = net::map_port(1);
   * @examples_end
   * @todo Ensure port is not already in use by another application.
   */
  std::uint16_t map_port(int port);

  /**
   * @brief Map a UDP port of this process's UDP base.
   *
   * The UDP base is the one `handoff::hooks::udp_base()` returns: the configured `port` in a normal start, or the base
   * the drain handoff gave this process (`port + drain_handoff_udp_offset`, or the base an in-place restart kept). The
   * TCP ports keep using `map_port`, which always adds to the configured `port`.
   *
   * Callers: the video, control and audio UDP ports in `stream::start_broadcast`, the `server_port` of
   * RTSP SETUP replies (`rtsp.cpp`, audio, video and control), and the UDP entries of the UPnP mappings. Each reply
   * advertises the port the sockets actually use, so a client follows the base of the process that answers.
   *
   * Default (no handoff setting, no `SUNSHINE_UDP_BASE`): `map_udp_port(n)` returns the same value as `map_port(n)`,
   * including the same out-of-range warning, so a normal start is unchanged.
   *
   * Environment round trip: `SUNSHINE_UDP_BASE` is read once by `handoff::runtime().early_init()` at the start of
   * `main()` (decimal, 1 to 65524). The upper limit keeps every mapped UDP port (base + 9, 10, 11) at or below 65535. A
   * value that does not parse, or is outside 1 to 65524, is ignored, and the base stays the configured `port`. The variable is removed from the environment before any app or thread can inherit it.
   * An in-place restart (`/api/restart`, the tray restart) sets it again just before `execv`, through
   * `handoff::hooks::restart_environment()`, only when the base differs from the configured `port`, and unsets it
   * when it does not. A handoff child gets its base from `SUNSHINE_HANDOFF_UDP_BASE` instead, which is invalid
   * (refused, exit 1) when it does not parse or is outside 1 to 65524. `early_init` range-checks the value only: there is no
   * collision check against the TCP ports or the other base. Sunshine sets the variable itself, and the old process
   * validated the configured offset before it spawned the child, so the variable is trusted once it parses.
   *
   * Thread-safety: reads only the handoff runtime's override, which is fixed after `early_init()` (except the test
   * seam). Callable from any thread.
   *
   * @param offset The port as an offset from the UDP base (for example 9 for video, 10 for control, 11 for audio).
   * @return The mapped port, `hooks::udp_base() + offset` as an unsigned 16-bit value. It warns when the result is
   *         outside 1024-65535, as `map_port` does.
   * @examples
   * std::uint16_t video_port = net::map_udp_port(stream::VIDEO_STREAM_PORT);
   * @examples_end
   */
  std::uint16_t map_udp_port(int offset);

  /**
   * @brief Owning ENet host pointer released with `enet_host_destroy`.
   */
  using host_t = util::safe_ptr<ENetHost, free_host>;
  /**
   * @brief Raw ENet peer handle owned by an ENet host.
   */
  using peer_t = ENetPeer *;
  /**
   * @brief Owning ENet packet pointer released with `enet_packet_destroy`.
   */
  using packet_t = util::safe_ptr<ENetPacket, enet_packet_destroy>;

  /**
   * @brief Enumerates supported net options.
   */
  enum net_e : int {
    PC,  ///< PC
    LAN,  ///< LAN
    WAN  ///< WAN
  };

  /**
   * @brief Enumerates supported af options.
   */
  enum af_e : int {
    IPV4,  ///< IPv4 only
    BOTH  ///< IPv4 and IPv6
  };

  /**
   * @brief Convert configuration text to a network enum value.
   *
   * @param view Boost.Log record view being formatted.
   * @return Value converted from enum string.
   */
  net_e from_enum_string(const std::string_view &view);
  /**
   * @brief Convert a network enum value to configuration text.
   *
   * @param net Network scope to convert or format.
   * @return Value converted to enum string.
   */
  std::string_view to_enum_string(net_e net);

  /**
   * @brief Convert a Boost address family to Sunshine network enum value.
   *
   * @param view Boost.Log record view being formatted.
   * @return Value converted from address.
   */
  net_e from_address(const std::string_view &view);

  /**
   * @brief Create an ENet host with the requested address family.
   *
   * @param af Address family used for socket creation or binding.
   * @param addr Network address to bind, parse, or format.
   * @param port TCP or UDP port number.
   * @return ENet host bound to the requested address and port, or an empty handle on failure.
   */
  host_t host_create(af_e af, ENetAddress &addr, std::uint16_t port);

  /**
   * @brief Convert a config address-family string to the matching enum.
   * @param view The config option value.
   * @return Address-family enum represented by the string.
   */
  af_e af_from_enum_string(const std::string_view &view);

  /**
   * @brief Get the effective address family for listening sockets.
   * @details An IPv4 bind address narrows dual-stack mode to IPv4 so the socket protocol matches the bind endpoint.
   * @param af Configured address family.
   * @return IPv4 for a valid IPv4 bind address in dual-stack mode, or `af` otherwise.
   */
  af_e get_effective_address_family(af_e af);

  /**
   * @brief Get the wildcard binding address for a given address family.
   * @param af Address family.
   * @return Normalized address.
   */
  std::string_view af_to_any_address_string(af_e af);

  /**
   * @brief Get the binding address to use based on config.
   * @param af Address family.
   * @return The configured bind address or wildcard if not configured.
   */
  std::string get_bind_address(af_e af);

  /**
   * @brief Get the configured bind address formatted as a URL host.
   * @details IPv6 addresses are enclosed in brackets. An empty bind address is represented as `localhost`.
   * @return URL host for links to services bound by Sunshine.
   */
  std::string get_bind_address_url_host();

  /**
   * @brief Convert an address to a normalized form.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize.
   * @return Normalized address.
   */
  boost::asio::ip::address normalize_address(boost::asio::ip::address address);

  /**
   * @brief Get the given address in normalized string form.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize.
   * @return Normalized address in string form.
   */
  std::string addr_to_normalized_string(boost::asio::ip::address address);

  /**
   * @brief Get the given address in a normalized form for the host portion of a URL.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize and escape.
   * @return Normalized address in URL-escaped string.
   */
  std::string addr_to_url_escaped_string(boost::asio::ip::address address);

  /**
   * @brief Get the encryption mode for the given remote endpoint address.
   * @param address The address used to look up the desired encryption mode.
   * @return The WAN or LAN encryption mode, based on the provided address.
   */
  int encryption_mode_for_address(boost::asio::ip::address address);

  /**
   * @brief Returns a string for use as the instance name for mDNS.
   * @param hostname The hostname to use for instance name generation.
   * @return Hostname-based instance name or "Sunshine" if hostname is invalid.
   */
  std::string mdns_instance_name(const std::string_view &hostname);
}  // namespace net
