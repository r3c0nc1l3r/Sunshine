/**
 * @file src/handoff/ping_monitor.cpp
 * @brief Definitions for `src/handoff/ping_monitor.h`.
 */
// standard includes
#include <climits>

// local includes
#include "ping_monitor.h"

namespace handoff {
  ping_monitor_t::ping_monitor_t(int misses_to_hang):
      _misses_to_hang {misses_to_hang < 1 ? 1 : misses_to_hang} {
  }

  std::uint64_t ping_monitor_t::next_ping() {
    // The previous PING had no PONG before this one is due: that is one miss.
    if (!_last_answered && _misses < INT_MAX) {
      ++_misses;
    }

    ++_last_sent;
    _last_answered = false;
    return _last_sent;
  }

  void ping_monitor_t::on_pong(std::uint64_t seq) {
    // Sequence 0 and numbers never sent are ignored.
    if (seq == 0 || seq > _last_sent) {
      return;
    }

    // Any answer to a sent PING, even a late one, clears the miss count.
    _misses = 0;
    if (seq == _last_sent) {
      _last_answered = true;
    }
  }

  int ping_monitor_t::misses() const {
    return _misses;
  }

  bool ping_monitor_t::hung() const {
    return _misses >= _misses_to_hang;
  }
}  // namespace handoff
