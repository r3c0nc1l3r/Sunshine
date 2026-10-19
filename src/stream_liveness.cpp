/**
 * @file src/stream_liveness.cpp
 * @brief Definitions for streaming-session liveness checks and session stop reasons.
 */

// standard includes
#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

// local includes
#include "stream_liveness.h"

namespace stream {
  std::string_view to_string(stop_reason_e reason) {
    switch (reason) {
      case stop_reason_e::disconnect:
        return "disconnect";
      case stop_reason_e::ping_timeout:
        return "ping-timeout";
      case stop_reason_e::media_ping_timeout:
        return "media-ping-timeout";
      case stop_reason_e::initial_ping_timeout:
        return "initial-ping-timeout";
      case stop_reason_e::protocol_error:
        return "protocol-error";
      case stop_reason_e::api_close:
        return "api-close";
      case stop_reason_e::stream_ended:
        return "stream-ended";
      case stop_reason_e::terminated:
        return "terminated";
      case stop_reason_e::unknown:
        break;
    }
    return "unknown";
  }

  namespace liveness {
    void configure_peer(ENetPeer *peer, std::chrono::milliseconds timeout) {
      if (!peer || timeout.count() < 1) {
        return;
      }

      // Keep ENet's default timeout limit (0) and use the configured value as both minimum and maximum.
      const auto max_value = static_cast<std::int64_t>(std::numeric_limits<enet_uint32>::max());
      const auto value = static_cast<enet_uint32>(std::min<std::int64_t>(timeout.count(), max_value));
      enet_peer_timeout(peer, 0, value, value);
    }

    bool tick(ENetPeer *peer, std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point &last_ping, std::chrono::milliseconds interval) {
      if (!peer || peer->state != ENET_PEER_STATE_CONNECTED) {
        return false;
      }

      const bool never_pinged = last_ping == std::chrono::steady_clock::time_point {};
      if (!never_pinged && now - last_ping < interval) {
        return false;
      }

      enet_peer_ping(peer);
      last_ping = now;
      return true;
    }

    void media_liveness_t::on_ping(std::chrono::steady_clock::time_point now) {
      _armed = true;
      _last_ping = now;
    }

    bool media_liveness_t::armed() const {
      return _armed;
    }

    bool media_liveness_t::expired(std::chrono::steady_clock::time_point now, std::chrono::milliseconds timeout) const {
      return _armed && now - _last_ping >= timeout;
    }

    void shared_media_liveness_t::on_ping(std::chrono::steady_clock::time_point now) {
      std::lock_guard lg {_mutex};
      _tracker.on_ping(now);
    }

    bool shared_media_liveness_t::armed() const {
      std::lock_guard lg {_mutex};
      return _tracker.armed();
    }

    bool shared_media_liveness_t::expired(std::chrono::steady_clock::time_point now, std::chrono::milliseconds timeout) const {
      std::lock_guard lg {_mutex};
      return _tracker.expired(now, timeout);
    }

    void media_ping_registry_t::add(const std::string &key, std::shared_ptr<shared_media_liveness_t> tracker) {
      if (!tracker) {
        return;
      }

      std::lock_guard lg {_mutex};
      _trackers.insert_or_assign(key, std::move(tracker));
    }

    void media_ping_registry_t::remove(const std::shared_ptr<shared_media_liveness_t> &tracker) {
      std::lock_guard lg {_mutex};
      std::erase_if(_trackers, [&](const auto &entry) {
        return entry.second == tracker;
      });
    }

    bool media_ping_registry_t::on_ping(const std::string &key, std::chrono::steady_clock::time_point now) {
      std::shared_ptr<shared_media_liveness_t> tracker;
      {
        std::lock_guard lg {_mutex};
        auto it = _trackers.find(key);
        if (it == _trackers.end()) {
          return false;
        }
        tracker = it->second;
      }

      // Update outside the registry lock so a slow tracker never blocks other keys.
      tracker->on_ping(now);
      return true;
    }

    std::size_t media_ping_registry_t::size() const {
      std::lock_guard lg {_mutex};
      return _trackers.size();
    }
  }  // namespace liveness
}  // namespace stream
