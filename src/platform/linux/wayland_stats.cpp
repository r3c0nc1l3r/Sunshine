/**
 * @file src/platform/linux/wayland_stats.cpp
 * @brief Definitions for process-wide counters of live wlr screencopy capture objects.
 */

// standard includes
#include <atomic>
#include <cstdint>
#include <string>

// local includes
#include "src/logging.h"
#include "wayland_stats.h"

namespace wl::stats {
  namespace {
    std::atomic<std::int64_t> bos_live {0};  ///< Live GBM buffer objects.
    std::atomic<std::int64_t> wl_buffers_live {0};  ///< Live `wl_buffer` proxies.
    std::atomic<std::int64_t> pending_params_live {0};  ///< Buffer-params requests waiting for a result.
    std::atomic<std::uint64_t> cancelled_total {0};  ///< Cancelled screencopy requests.
    std::atomic<std::uint64_t> overwritten_total {0};  ///< Live objects found where a new one was stored.

    std::atomic<bool> bo_underflow_logged {false};  ///< Set once the BO underflow has been logged.
    std::atomic<bool> wl_buffer_underflow_logged {false};  ///< Set once the wl_buffer underflow has been logged.
    std::atomic<bool> params_underflow_logged {false};  ///< Set once the pending-params underflow has been logged.

    /**
     * @brief Decrement a live count, never letting it go below 0.
     *
     * A decrement that would go below 0 means a destroy happened without a matching create. The count is still
     * clamped at 0, and the imbalance is logged once per counter kind at debug level.
     *
     * @param value Live count to decrement.
     * @param logged Flag that is set after the first underflow of this counter has been logged.
     * @param name Counter name used in the log message.
     */
    void decrement_clamped(std::atomic<std::int64_t> &value, std::atomic<bool> &logged, const char *name) {
      auto current = value.load();
      while (current > 0 && !value.compare_exchange_weak(current, current - 1)) {}
      if (current <= 0 && !logged.exchange(true)) {
        BOOST_LOG(debug) << "[wayland] capture stats: " << name << " count would go negative";
      }
    }
  }  // namespace

  void bo_created() {
    bos_live.fetch_add(1);
  }

  void bo_destroyed() {
    decrement_clamped(bos_live, bo_underflow_logged, "bo");
  }

  void wl_buffer_created() {
    wl_buffers_live.fetch_add(1);
  }

  void wl_buffer_destroyed() {
    decrement_clamped(wl_buffers_live, wl_buffer_underflow_logged, "wl_buffer");
  }

  void params_pending() {
    pending_params_live.fetch_add(1);
  }

  void params_resolved() {
    decrement_clamped(pending_params_live, params_underflow_logged, "pending params");
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
