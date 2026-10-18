/**
 * @file src/platform/linux/wayland_stats.h
 * @brief Declarations for process-wide counters of live wlr screencopy capture objects.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <string>

// local includes
#include "fd_counts.h"

namespace wl::stats {
  constexpr std::int64_t default_live_cap = 8;  ///< Live objects of one kind above which capture reinitializes.

  /**
   * @brief Snapshot of the capture-object counters.
   */
  struct counters_t {
    std::int64_t bos {0};  ///< Live GBM buffer objects.
    std::int64_t wl_buffers {0};  ///< Live `wl_buffer` proxies.
    std::int64_t pending_params {0};  ///< `zwp_linux_buffer_params_v1` requests waiting for `created` or `failed`.
    std::uint64_t cancelled_requests {0};  ///< Screencopy requests cancelled before completion, since start or reset.
    std::uint64_t overwritten_objects {0};  ///< Live BOs or buffers found where a new one was about to be stored, since start or reset.
  };

  void bo_created();  ///< Count a created GBM buffer object.
  void bo_destroyed();  ///< Count a destroyed GBM buffer object. The live count never goes below 0.
  void wl_buffer_created();  ///< Count a created `wl_buffer`.
  void wl_buffer_destroyed();  ///< Count a destroyed `wl_buffer`. The live count never goes below 0.
  void params_pending();  ///< Count a buffer-params request sent to the compositor.
  void params_resolved();  ///< Count a buffer-params request that got `created` or `failed`. The count never goes below 0.
  void request_cancelled();  ///< Count a cancelled screencopy request.
  void object_overwritten();  ///< Count a live object that was found before a new one was stored.

  /**
   * @brief Read all counters.
   *
   * @return Current values.
   */
  counters_t snapshot();

  /**
   * @brief Set all counters to zero.
   */
  void reset();

  /**
   * @brief Whether a live count exceeds the cap.
   *
   * @param counters Snapshot to check.
   * @param cap Maximum allowed live objects of one kind.
   * @return `true` when `bos`, `wl_buffers` or `pending_params` is greater than `cap`.
   */
  bool over_cap(const counters_t &counters, std::int64_t cap = default_live_cap);

  /**
   * @brief Format counters and fd counts for the log.
   *
   * @param counters Capture-object counters.
   * @param fds Open fd counts.
   * @return `bos=<n> wl_buffers=<n> pending_params=<n> cancelled=<n> overwritten=<n> dmabuf_fds=<n> render_fds=<n>`.
   */
  std::string format(const counters_t &counters, const platf::fd_counts_t &fds);

  /**
   * @brief Rate limiter for a periodic log line.
   */
  class periodic_gate_t {
  public:
    /**
     * @brief Create a gate that opens at most once per interval.
     *
     * @param interval Minimum time between two open results.
     */
    explicit periodic_gate_t(std::chrono::steady_clock::duration interval);

    /**
     * @brief Whether the periodic action is due.
     *
     * @param now Current time.
     * @return `true` on the first call, then once at least `interval` has passed since the last `true`.
     */
    bool due(std::chrono::steady_clock::time_point now);

  private:
    std::chrono::steady_clock::duration _interval;  ///< Minimum time between two open results.
    bool _opened {false};  ///< Whether the gate has opened before.
    std::chrono::steady_clock::time_point _last {};  ///< Time of the last open result.
  };
}  // namespace wl::stats
