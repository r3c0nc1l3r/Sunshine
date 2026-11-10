/** @file src/platform/linux/private_seat_wayland.cpp
 * @brief Private output binding on the existing capture connection.
 */
#include "private_seat_wayland.h"

#if defined(SUNSHINE_BUILD_WAYLAND) && !defined(__FreeBSD__)
  #include <sys/socket.h>
  #include <utility>
  #include <wayland-client.h>

namespace private_seat {
  namespace {
    /** @brief Validate protocol dimensions and the common area limit. */
    bool dimensions_valid(std::uint32_t w, std::uint32_t h) {
      return w && h && w <= 8192 && h <= 8192 && std::uint64_t(w) * h <= 7680ULL * 4320;
    }

    /** @brief Join high and low protocol words. */
    std::uint64_t join(std::uint32_t hi, std::uint32_t lo) {
      return (std::uint64_t(hi) << 32) | lo;
    }
  }  // namespace

  wayland_binding::wayland_binding(wl::interface_t &interface, wl::monitor_t &monitor, peer_credentials peer, std::uint64_t generation, callback notify):
      interface_(interface),
      monitor_(monitor),
      peer_(peer),
      generation_(generation),
      notify_(std::move(notify)) {}

  std::unique_ptr<wayland_binding> wayland_binding::bind(wl_display *display, wl::interface_t &interface, const wayland_selection &selected, peer_credentials input_peer, std::uint64_t generation, callback notify, std::string &error) {
    error.clear();
    auto reject = [&error](const char *message) -> std::unique_ptr<wayland_binding> {
      error = message;
      return nullptr;
    };
    if (!display || interface.capture_display != display || !interface.private_output_manager || interface.private_registry_changed || !generation || !notify) {
      return reject("private output manager or capture connection unavailable");
    }
    struct ucred credentials {};
    socklen_t length = sizeof(credentials);
    if (input_peer.pid <= 0 || getsockopt(wl_display_get_fd(display), SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 || length != sizeof(credentials) || credentials.pid != input_peer.pid || credentials.uid != input_peer.uid) {
      return reject("Wayland capture peer does not match authenticated input peer");
    }
    if (interface.monitors.size() != 1 || !selected.output || (!selected.global_id && selected.name.empty())) {
      return reject("private capture requires one explicitly selected output");
    }
    auto &monitor = *interface.monitors.front();
    if (monitor.private_metadata_changed) {
      return reject("private output already observed");
    }
    if (monitor.output != selected.output || (selected.global_id && selected.global_id != monitor.global_id) || (!selected.name.empty() && selected.name != monitor.name)) {
      return reject("selected output does not match the capture object");
    }
    const auto &v = monitor.viewport;
    if (!monitor.geometry_received || !monitor.position_received || !monitor.size_received || !monitor.mode_received || monitor.transform != WL_OUTPUT_TRANSFORM_NORMAL || v.offset_x != 0 || v.offset_y != 0 || v.width <= 0 || v.height <= 0 || v.logical_width <= 0 || v.logical_height <= 0 || !dimensions_valid(v.width, v.height) || !dimensions_valid(v.logical_width, v.logical_height) || selected.area.x || selected.area.y || selected.area.width != static_cast<std::uint32_t>(v.width) || selected.area.height != static_cast<std::uint32_t>(v.height)) {
      return reject("private capture requires a full, normal output at origin");
    }
    auto result = std::unique_ptr<wayland_binding>(new wayland_binding(interface, monitor, input_peer, generation, std::move(notify)));
    result->binding_ = lanternbox_private_output_manager_v1_bind_output(interface.private_output_manager, selected.output);
    if (!result->binding_) {
      return reject("could not create private output binding");
    }
    static const lanternbox_private_output_v1_listener listener {&wayland_binding::on_snapshot, &wayland_binding::on_invalidated};
    if (lanternbox_private_output_v1_add_listener(result->binding_, &listener, result.get()) != 0) {
      return reject("could not attach private output listener");
    }
    interface.private_registry_changed = [binding = result.get()] {
      binding->fail("Wayland registry changed");
    };
    monitor.private_metadata_changed = [binding = result.get()] {
      binding->fail("Wayland output metadata changed");
    };
    return result;
  }

  wayland_binding::~wayland_binding() {
    interface_.private_registry_changed = {};
    monitor_.private_metadata_changed = {};
    if (binding_) {
      lanternbox_private_output_v1_destroy(binding_);
    }
  }

  std::shared_ptr<const wayland_snapshot> wayland_binding::snapshot() const {
    return snapshot_;
  }

  bool wayland_binding::invalid() const {
    return invalid_;
  }

  const std::string &wayland_binding::error() const {
    return error_;
  }

  void wayland_binding::invalidate() {
    fail("capture binding retired");
  }

  void wayland_binding::fail(const char *reason) {
    if (invalid_) {
      return;
    }
    invalid_ = true;
    snapshot_.reset();
    error_ = reason;
    notify_(generation_, false);
  }

  void wayland_binding::on_snapshot(void *data, lanternbox_private_output_v1 *, std::uint32_t id_hi, std::uint32_t id_lo, std::uint32_t epoch_hi, std::uint32_t epoch_lo, std::uint32_t pw, std::uint32_t ph, std::uint32_t lw, std::uint32_t lh) {
    auto &self = *static_cast<wayland_binding *>(data);
    if (self.invalid_) {
      return;
    }
    const auto id = join(id_hi, id_lo);
    const auto epoch = join(epoch_hi, epoch_lo);
    const auto &m = self.monitor_;
    const auto &v = m.viewport;
    if (self.snapshot_ || !id || !epoch || !dimensions_valid(pw, ph) || !dimensions_valid(lw, lh) || !m.geometry_received || !m.position_received || !m.size_received || !m.mode_received || m.transform != WL_OUTPUT_TRANSFORM_NORMAL || v.offset_x != 0 || v.offset_y != 0 || v.width != static_cast<int>(pw) || v.height != static_cast<int>(ph) || v.logical_width != static_cast<int>(lw) || v.logical_height != static_cast<int>(lh)) {
      self.fail("invalid or inconsistent private output snapshot");
      return;
    }
    self.snapshot_ = std::make_shared<const wayland_snapshot>(wayland_snapshot {id, epoch, pw, ph, lw, lh, self.peer_, self.generation_});
    self.notify_(self.generation_, true);
  }

  void wayland_binding::on_invalidated(void *data, lanternbox_private_output_v1 *, std::uint32_t, std::uint32_t) {
    static_cast<wayland_binding *>(data)->fail("private output epoch invalidated");
  }
}  // namespace private_seat
#endif
