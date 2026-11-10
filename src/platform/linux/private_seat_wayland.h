/** @file src/platform/linux/private_seat_wayland.h
 * @brief Binding of a private-seat snapshot to the capture Wayland connection.
 */
#pragma once

#include "private_seat_client.h"
#include "wayland.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#if defined(SUNSHINE_BUILD_WAYLAND) && !defined(__FreeBSD__)
namespace private_seat {
  /** @brief Immutable compositor output identity and authenticated capture peer. */
  struct wayland_snapshot {
    std::uint64_t output_id;  ///< Compositor output instance.
    std::uint64_t epoch;  ///< Compositor output epoch.
    std::uint32_t physical_width;  ///< Full physical width.
    std::uint32_t physical_height;  ///< Full physical height.
    std::uint32_t logical_width;  ///< Full logical width.
    std::uint32_t logical_height;  ///< Full logical height.
    peer_credentials peer;  ///< Credentials of the actual capture connection.
    std::uint64_t generation;  ///< Caller-supplied nonzero local callback generation.
  };

  /** @brief Exact selection and full-output capture region chosen by the capture owner. */
  struct wayland_selection {
    wl_output *output = nullptr;  ///< The very object passed to capture_output.
    std::string name;  ///< Explicit xdg-output name, or empty when global_id is provided.
    std::uint32_t global_id = 0;  ///< Explicit registry ID, or zero when name is provided.
    viewport area;  ///< Full physical output viewport.
  };

  /**
   * @brief Single-use, fail-closed private output binding, owned by the capture thread.
   *
   * Call only on the display's dispatch thread. The display and interface must outlive
   * this object. Its callback must not destroy it or dispatch Wayland recursively.
   * Snapshot copies are immutable but become stale after invalidation: compare the
   * generation under the shared capture gate and clear that gate in the callback.
   * The owner must also invalidate the gate on display failure and teardown. A new
   * binding and fresh socket acknowledgement are required after any invalidation.
   */
  class wayland_binding {
  public:
    /** @brief Notify snapshot availability (true) or terminal invalidation (false). */
    using callback = std::function<void(std::uint64_t, bool)>;
    /**
     * @brief Bind the selected output on the already-discovered capture connection.
     * @param display Capture display passed to interface.listen_capture().
     * @param interface Registry state belonging to that same display.
     * @param selected Exact selected object, selector and full capture viewport.
     * @param input_peer Authenticated peer from the fresh private input connection.
     * @param generation Nonzero, never-reused capture callback generation.
     * @param notify Gate notification, required and invoked on the dispatch thread.
     * @param error Receives an actionable failure reason.
     * @return Binding awaiting its snapshot, or null on failure.
     */
    static std::unique_ptr<wayland_binding> bind(wl_display *display, wl::interface_t &interface, const wayland_selection &selected, peer_credentials input_peer, std::uint64_t generation, callback notify, std::string &error);
    /** @brief Destroy the protocol binding before the display and registry state. */
    ~wayland_binding();
    /** @brief Bindings have a single owner. */
    wayland_binding(const wayland_binding &) = delete;
    /** @brief Bindings cannot be assigned. */
    wayland_binding &operator=(const wayland_binding &) = delete;
    /**
     * @brief Return the current immutable snapshot, or null before receipt or after invalidation.
     * @return Current snapshot, or null.
     */
    std::shared_ptr<const wayland_snapshot> snapshot() const;
    /**
     * @brief Report whether this binding has failed permanently.
     * @return True if permanently invalidated.
     */
    bool invalid() const;
    /**
     * @brief Terminal reason, empty while valid or awaiting a snapshot.
     * @return Terminal failure reason, or an empty string.
     */
    const std::string &error() const;
    /** @brief Invalidate after capture/display failure or before any capture transition. */
    void invalidate();

  private:
    /** @brief Create listener state after all prerequisites have been checked. */
    wayland_binding(wl::interface_t &interface, wl::monitor_t &monitor, peer_credentials peer, std::uint64_t generation, callback notify);
    /** @brief Process the protocol snapshot. */
    static void on_snapshot(void *, lanternbox_private_output_v1 *, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
    /** @brief Process the protocol epoch invalidation. */
    static void on_invalidated(void *, lanternbox_private_output_v1 *, std::uint32_t, std::uint32_t);
    /** @brief Mark the binding dead and notify the owner exactly once. */
    void fail(const char *reason);
    wl::interface_t &interface_;  ///< Capture connection's registry state.
    wl::monitor_t &monitor_;  ///< Selected output metadata on that connection.
    peer_credentials peer_;  ///< Authenticated capture peer.
    std::uint64_t generation_;  ///< Capture owner's callback generation.
    callback notify_;  ///< Capture gate notification.
    lanternbox_private_output_v1 *binding_ = nullptr;  ///< Owned protocol object.
    std::shared_ptr<const wayland_snapshot> snapshot_;  ///< Current immutable snapshot.
    std::string error_;  ///< Terminal failure reason.
    bool invalid_ = false;  ///< Terminal invalidation latch.
  };
}  // namespace private_seat
#endif
