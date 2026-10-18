/**
 * @file src/platform/linux/wayland_stats.cpp
 * @brief Definitions for process-wide counters of live wlr screencopy capture objects.
 */

// standard includes
#include <atomic>
#include <cstdint>
#include <string>

// local includes
#include "wayland_stats.h"

namespace wl::stats {
  namespace {
    std::atomic<std::int64_t> bos_live {0};  ///< Live GBM buffer objects.
    std::atomic<std::int64_t> wl_buffers_live {0};  ///< Live `wl_buffer` proxies.
    std::atomic<std::int64_t> pending_params_live {0};  ///< Buffer-params requests waiting for a result.
    std::atomic<std::uint64_t> cancelled_total {0};  ///< Cancelled screencopy requests.
    std::atomic<std::uint64_t> overwritten_total {0};  ///< Live objects found where a new one was stored.

    /**
     * @brief Decrement a live count, never letting it go below 0.
     *
     * @param value Live count to decrement.
     */
    void decrement_clamped(std::atomic<std::int64_t> &value) {
      auto current = value.load();
      while (current > 0 && !value.compare_exchange_weak(current, current - 1)) {}
    }
  }  // namespace

  void bo_created() {
    bos_live.fetch_add(1);
  }

  void bo_destroyed() {
    decrement_clamped(bos_live);
  }

  void wl_buffer_created() {
    wl_buffers_live.fetch_add(1);
  }

  void wl_buffer_destroyed() {
    decrement_clamped(wl_buffers_live);
  }

  void params_pending() {
    pending_params_live.fetch_add(1);
  }

  void params_resolved() {
    decrement_clamped(pending_params_live);
  }

  void request_cancelled() {
    cancelled_total.fetch_add(1);
  }

  void object_overwritten() {
    overwritten_total.fetch_add(1);
  }

  counters_t snapshot() {
    counters_t counters;
    counters.bos = bos_live.load();
    counters.wl_buffers = wl_buffers_live.load();
    counters.pending_params = pending_params_live.load();
    counters.cancelled_requests = cancelled_total.load();
    counters.overwritten_objects = overwritten_total.load();
    return counters;
  }

  void reset() {
    bos_live.store(0);
    wl_buffers_live.store(0);
    pending_params_live.store(0);
    cancelled_total.store(0);
    overwritten_total.store(0);
  }

  bool over_cap(const counters_t &counters, std::int64_t cap) {
    return counters.bos > cap || counters.wl_buffers > cap || counters.pending_params > cap;
  }

  std::string format(const counters_t &counters, const platf::fd_counts_t &fds) {
    return "bos=" + std::to_string(counters.bos) +
           " wl_buffers=" + std::to_string(counters.wl_buffers) +
           " pending_params=" + std::to_string(counters.pending_params) +
           " cancelled=" + std::to_string(counters.cancelled_requests) +
           " overwritten=" + std::to_string(counters.overwritten_objects) +
           " dmabuf_fds=" + std::to_string(fds.dmabufs) +
           " render_fds=" + std::to_string(fds.render_nodes);
  }

  periodic_gate_t::periodic_gate_t(std::chrono::steady_clock::duration interval):
      _interval(interval) {
  }

  bool periodic_gate_t::due(std::chrono::steady_clock::time_point now) {
    if (_opened && now - _last < _interval) {
      return false;
    }

    _opened = true;
    _last = now;
    return true;
  }
}  // namespace wl::stats
