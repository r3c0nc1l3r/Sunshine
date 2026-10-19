/**
 * @file src/upnp.h
 * @brief Declarations for UPnP port mapping.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

// lib includes
#include <miniupnpc/miniupnpc.h>

// local includes
#include "platform/common.h"

/**
 * @brief UPnP port mapping.
 */
namespace upnp {
  constexpr auto INET6_ADDRESS_STRLEN = 46;  ///< Protocol or platform constant for inet6 address strlen.
  constexpr auto IPv4 = 0;  ///< I pv4.
  constexpr auto IPv6 = 1;  ///< I pv6.
  constexpr auto PORT_MAPPING_LIFETIME = 3600s;  ///< GameStream port offset for port mapping lifetime.
  constexpr auto REFRESH_INTERVAL = 120s;  ///< Protocol or platform constant for refresh interval.

  /**
   * @brief Owning pointer to miniupnpc device discovery results.
   */
  using device_t = util::safe_ptr<UPNPDev, freeUPNPDevlist>;

  KITTY_USING_MOVE_T(urls_t, UPNPUrls, , {
    FreeUPNPUrls(&el);
  });  ///< Alias for element type.

  /**
   * @brief Get the valid IGD status.
   * @param device The device.
   * @param urls The URLs.
   * @param data The IGD data.
   * @param lan_addr The LAN address.
   * @return The UPnP Status.
   * @retval 0 No IGD found.
   * @retval 1 A valid connected IGD has been found.
   * @retval 2 A valid IGD has been found but it reported as not connected.
   * @retval 3 An UPnP device has been found but was not recognized as an IGD.
   */
  int UPNP_GetValidIGDStatus(device_t &device, urls_t *urls, IGDdatas *data, std::array<char, INET6_ADDRESS_STRLEN> &lan_addr);

  /**
   * @brief Which of this process's mappings its deinit guard removes when the process exits.
   *
   * The scope is process-wide, read by the guard's destructor, and set by the drain code (`set_unmap_scope`). A normal
   * process uses `all`. Sunshine's TCP ports (RTSP, client HTTP and HTTPS, web UI) are shared with the active process
   * during a handoff, so a draining process must never remove them.
   */
  enum class unmap_scope_e {
    all,  ///< Every mapping of this process, TCP and UDP. The normal exit.
    udp_only,  ///< Only the UDP mappings of this process's own UDP base. A draining process whose active peer uses another base.
    none,  ///< Nothing. A draining process whose UDP base equals the active process's base.
  };

  /**
   * @brief One mapping Sunshine wants on the gateway.
   */
  struct planned_mapping_t {
    std::uint16_t port;  ///< Port used on both the LAN and the WAN side; Sunshine maps the two alike.
    bool udp;  ///< `true` for a UDP mapping, `false` for a TCP mapping.
    std::string description;  ///< Description shown on the gateway, for example "Sunshine - Video".
  };

  /**
   * @brief The mappings that `start()` requests for a given configuration, in request order.
   *
   * TCP mappings are the RTSP setup port, the client HTTP and HTTPS ports and, when `web_ui_on_wan` is set, the web UI
   * port. They use `port` exactly as `net::map_port` does. UDP mappings are video (`udp_base + 9`), control
   * (`udp_base + 10`) and audio (`udp_base + 11`), the same offsets `net::map_udp_port` adds. Entries come in this
   * order: RTSP, video, audio, control, client HTTP, client HTTPS, then web UI. With `udp_base == port` the result is
   * the same as before the UDP base existed.
   *
   * Pure: no gateway, no logging, no shared state. Callable from any thread.
   *
   * @param port Configured `port`.
   * @param udp_base UDP base of this process (`handoff::hooks::udp_base()`).
   * @param web_ui_on_wan `true` when the web UI may be reached from the WAN (`origin_web_ui_allowed` above LAN).
   * @return The planned mappings, as described.
   */
  std::vector<planned_mapping_t> plan_mappings(int port, int udp_base, bool web_ui_on_wan);

  /**
   * @brief The part of a plan that a scope removes.
   *
   * `all` keeps every entry, `udp_only` keeps the UDP entries and `none` keeps nothing. Pure.
   *
   * @param plan Mappings to filter, typically from `plan_mappings`.
   * @param scope Scope to apply.
   * @return The entries of `plan` the scope selects, in their original order.
   */
  std::vector<planned_mapping_t> select_for_scope(const std::vector<planned_mapping_t> &plan, unmap_scope_e scope);

  /**
   * @brief The UDP mappings that a background removal may delete, given what this process maps itself.
   *
   * One UDP entry per requested port, in ascending order, for each requested port that is neither a UDP nor a TCP
   * port of `own_plan`. This is the second guard of `remove_ports_async`: a port that this process maps is never
   * selected, even when the caller passes it. TCP is never selected, because every entry is UDP. Pure.
   *
   * @param requested Ports the caller wants removed (`handoff::upnp_ports_to_remove` computes them).
   * @param own_plan This process's planned mappings (`plan_mappings` of its own configuration).
   * @return The UDP entries to delete. Empty when `requested` is empty or every requested port is in `own_plan`.
   */
  std::vector<planned_mapping_t> removal_plan(const std::set<std::uint16_t> &requested, const std::vector<planned_mapping_t> &own_plan);

  /**
   * @brief Set the scope that this process's deinit guard applies when it is destroyed.
   *
   * Called by the drain code when the process enters draining (`udp_only`, or `none` when the active process uses the
   * same UDP base), and back to `all` when a rollback makes it the active process again. Atomic; the default is `all`.
   * The guard reads the scope when it runs, so the change applies at exit, not earlier.
   *
   * @param scope New scope.
   */
  void set_unmap_scope(unmap_scope_e scope);

  /**
   * @brief The current unmap scope.
   *
   * @return The scope last set by `set_unmap_scope`, `all` by default.
   */
  unmap_scope_e unmap_scope();

  /**
   * @brief Start UPnP port mapping and return its shutdown guard.
   *
   * Maps the TCP ports at the configured `port` and the UDP ports of `udp_base` (`plan_mappings`). The mapping thread
   * refreshes them as before. In this process, the plan made here is also the `own_plan` that `remove_ports_async`
   * checks against, so a later call to it never removes a mapping this process made.
   *
   * Normal start: called once after `http::init()`, with `udp_base == port` unless `SUNSHINE_UDP_BASE` is set. A
   * handoff child calls it only after `COMMIT` (it maps nothing before the switch). Calling it twice in one process is
   * a programming error: the second call maps nothing, replaces no plan and returns `nullptr`.
   *
   * Thread-safety: call from the main thread. The guard may be destroyed from any thread, as before.
   *
   * @param udp_base UDP base of this process.
   * @return The shutdown guard, or `nullptr` when UPnP is off (`config::sunshine.flags[UPNP]` false), in which case
   *         nothing is mapped and `remove_ports_async` does nothing either.
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> start(int udp_base);

  /**
   * @brief Remove the UDP mappings of the given ports on the gateway, in the background.
   *
   * Used after a rollback killed a new process that had mapped its UDP base: the caller passes
   * `handoff::upnp_ports_to_remove(new_base, old_base, port)`. Two guards keep the old process's own mappings safe:
   * (1) the caller's set never contains a UDP or TCP port of the old process's configuration, because the set is
   * `udp_ports(new) - udp_ports(old) - tcp_ports(port)`, and `handoff::validate` already rejects overlapping bases; and
   * (2) this function drops every port that this process maps itself (`own_plan` from its `start()`) and deletes only
   * UDP entries (`removal_plan`). It never reads or changes the guard's mapping list, so the process's own mappings are
   * not selected even when the caller passes a wrong set. No TCP mapping is ever deleted by this function.
   *
   * The work runs on one background worker: discover the gateway (as the mapping thread does, 2 s), then delete each
   * planned UDP mapping with `UPNP_DeletePortMapping`. "No such entry" counts as removed. Any other failure is a
   * Warning, and there is no retry. Calls are serialised on the worker in call order. The function does not wait for
   * the worker, so it never delays accept, mDNS or the helper's answer.
   *
   * It does nothing when UPnP is off or `start()` was not called in this process, and when the set is empty.
   *
   * Thread-safety: any thread, but not while holding the handoff runtime's mutex or any Sunshine list lock. Takes a
   * short internal mutex only.
   *
   * @param ports UDP ports to remove, typically `upnp_ports_to_remove(...)`.
   */
  void remove_ports_async(std::set<std::uint16_t> ports);

#ifdef SUNSHINE_TESTS
  /**
   * @brief Test seam: a fake gateway (IGD) with an in-memory mapping store.
   *
   * While a fake gateway is installed, every gateway call of this module acts on the store instead of the network:
   * discovery (the 2 s step of `start()`'s mapping thread and of `remove_ports_async`'s worker) finds one connected
   * gateway at once; adding a mapping (`UPNP_AddPortMapping`) inserts or refreshes an entry; deleting one
   * (`UPNP_DeletePortMapping`) erases it, and deleting a missing entry counts as removed ("no such entry"); listing
   * returns the entries. So `start()`'s guard (map, refresh, and on destruction remove under the CURRENT
   * `unmap_scope()`: `all` removes the TCP and UDP entries of the plan, `udp_only` only the UDP entries, `none`
   * nothing) and `remove_ports_async` (UDP entries only, never an entry of this process's own plan) both act on the
   * store. With no fake gateway installed everything behaves as in production. All functions below are thread-safe.
   *
   * A blind test of a post-`COMMIT` rollback asserts the FINAL store: after `wait_idle_for_test()` returned `true`, the
   * old process's TCP and UDP entries are present, the new base's UDP entries (what the killed new process mapped) are
   * gone, and with equal bases the store is unchanged by the cleanup.
   */
  namespace testing {
    /**
     * @brief One entry of the fake gateway's store.
     */
    struct fake_mapping_t {
      std::uint16_t port;  ///< Port (the same on the LAN and the WAN side).
      bool tcp;  ///< `true` for TCP, `false` for UDP.

      /**
       * @brief Order by port, then protocol, so a test can compare stores.
       *
       * @param other Other entry.
       * @return `true` when this entry sorts first.
       */
      bool operator<(const fake_mapping_t &other) const {
        return port != other.port ? port < other.port : tcp < other.tcp;
      }

      /**
       * @brief Equality of both fields.
       *
       * @param other Other entry.
       * @return `true` when equal.
       */
      bool operator==(const fake_mapping_t &other) const {
        return port == other.port && tcp == other.tcp;
      }
    };

    /**
     * @brief Install the fake gateway with the given entries (replacing any earlier fake and its store).
     *
     * @param initial Entries present at the start (for example what the other process mapped).
     */
    void install_fake_igd(const std::set<fake_mapping_t> &initial = {});

    /**
     * @brief Remove the fake gateway; the store is discarded and production behaviour returns.
     */
    void remove_fake_igd();

    /**
     * @brief Add entries to the installed fake store (as a second process mapping on the same gateway would). Does
     * nothing when no fake is installed.
     *
     * @param entries Entries to add.
     */
    void fake_igd_add(const std::set<fake_mapping_t> &entries);

    /**
     * @brief The current store.
     *
     * @return A copy of the entries, ordered; empty when no fake is installed.
     */
    std::set<fake_mapping_t> fake_igd_mappings();

    /**
     * @brief Wait until the background worker of `remove_ports_async` has finished every queued call.
     *
     * @param timeout Longest wait.
     * @return `true` when the worker is idle (also when nothing was ever queued); `false` on timeout.
     */
    bool wait_idle_for_test(std::chrono::milliseconds timeout);

    /**
     * @brief Forget the state of this module: the plan of `start()` (so `start()` may be called again in a test), the
     * unmap scope (back to `all`), and the worker's queue (waits for it to go idle first). Keeps the installed fake.
     */
    void reset_upnp_for_test();
  }  // namespace testing
#endif
}  // namespace upnp
