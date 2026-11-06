/**
 * @file src/video_retry_backoff.cpp
 * @brief Definitions for the backoff between failed encode runs and display re-inits.
 */

// standard includes
#include <algorithm>

// local includes
#include "video_retry_backoff.h"

namespace video {
  retry_backoff_t::retry_backoff_t(std::chrono::milliseconds initial, std::chrono::milliseconds max, std::chrono::milliseconds healthy_run):
      _initial(initial),
      _max(max),
      _healthy_run(healthy_run) {
  }

  std::chrono::milliseconds retry_backoff_t::on_run_end(std::chrono::milliseconds run_duration, bool failed) {
    if (!failed || run_duration >= _healthy_run) {
      reset();
      return std::chrono::milliseconds::zero();
    }

    ++_failures;

    // Double the delay for each further quick failure, stopping at the cap so the value cannot overflow.
    auto delay = _initial;
    for (auto i = std::uint32_t {1}; i < _failures && delay < _max; ++i) {
      delay *= 2;
    }
    return std::min(delay, _max);
  }

  std::uint32_t retry_backoff_t::consecutive_failures() const {
    return _failures;
  }

  void retry_backoff_t::reset() {
    _failures = 0;
  }

  std::chrono::milliseconds reinit_delay(retry_backoff_t &backoff, bool requested, bool frame_since_open, std::chrono::milliseconds lifetime) {
    if (requested) {
      backoff.reset();
      return std::chrono::milliseconds::zero();
    }

    return backoff.on_run_end(lifetime, !frame_since_open);
  }
}  // namespace video
