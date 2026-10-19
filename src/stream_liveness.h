/**
 * @file src/stream_liveness.h
 * @brief Declarations for streaming-session liveness checks and session stop reasons.
 */
#pragma once

// standard includes
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

// lib includes
#include <enet/enet.h>

namespace stream {
  /**
   * @brief Why a streaming session ended. The first recorded reason wins.
   */
  enum class stop_reason_e : int {
    unknown,  ///< No reason was recorded.
    disconnect,  ///< The control peer disconnected (client request or ENet timeout; ENet reports both alike).
    ping_timeout,  ///< No control-stream message from the client within `ping_timeout`.
    media_ping_timeout,  ///< No video or audio UDP ping from the client within `ping_timeout`.
    initial_ping_timeout,  ///< The client did not send the initial video or audio ping.
    protocol_error,  ///< The client sent a control message that failed to decrypt or parse.
    api_close,  ///< Closed through the web UI session API.
    stream_ended,  ///< The video or audio pipeline ended (capture or encode stopped or failed).
    terminated,  ///< Ended by the host (app quit, client unpaired, shutdown).
  };

  /**
   * @brief Name of a stop reason as written in the session-end log line.
   *
   * @param reason Stop reason.
   * @return Lowercase name with dashes, for example `media-ping-timeout`; `unknown` for out-of-range values.
   */
  std::string_view to_string(stop_reason_e reason);

  namespace liveness {
    constexpr std::chrono::milliseconds default_ping_interval {1000};  ///< Interval between explicit ENet pings to a control peer.

    /**
     * @brief Set the ENet timeout of a control peer from the configured ping timeout.
     *
     * A peer that leaves reliable commands (including pings) unacknowledged is then disconnected by ENet
     * after about `timeout`, through the normal ENet DISCONNECT event.
     *
     * @param peer Control-stream peer. Nothing happens when it is null.
     * @param timeout Timeout; values below 1 ms leave the peer unchanged.
     */
    void configure_peer(ENetPeer *peer, std::chrono::milliseconds timeout);

    /**
     * @brief Send an explicit ENet ping to a connected peer when the ping interval has elapsed.
     *
     * Call only from the thread that services the ENet host.
     *
     * @param peer Control-stream peer. Nothing happens when it is null or not connected.
     * @param now Current time.
     * @param last_ping Time of the last ping sent to this peer; a default-constructed value means never. Updated when a ping is sent.
     * @param interval Minimum time between pings.
     * @return `true` when a ping was queued.
     */
    bool tick(ENetPeer *peer, std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point &last_ping, std::chrono::milliseconds interval = default_ping_interval);

    /**
     * @brief Tracks the client's periodic video and audio UDP pings after the session started.
     *
     * The check arms on the first ping. Clients that never send periodic pings are never expired.
     */
    class media_liveness_t {
    public:
      /**
       * @brief Record a video or audio ping from the client.
       *
       * @param now Time the ping was received.
       */
      void on_ping(std::chrono::steady_clock::time_point now);

      /**
       * @brief Whether at least one ping was recorded.
       *
       * @return `true` once `on_ping()` was called.
       */
      bool armed() const;

      /**
       * @brief Whether the client stopped pinging.
       *
       * @param now Current time.
       * @param timeout Allowed time since the last ping.
       * @return `true` when armed and `now - last ping >= timeout`.
       */
      bool expired(std::chrono::steady_clock::time_point now, std::chrono::milliseconds timeout) const;

    private:
      bool _armed {false};  ///< Whether a ping was recorded.
      std::chrono::steady_clock::time_point _last_ping {};  ///< Time of the last recorded ping.
    };
    /**
     * @brief Thread-safe wrapper around `media_liveness_t`, shared by the receive and broadcast threads.
     */
    class shared_media_liveness_t {
    public:
      /**
       * @brief Record a video or audio ping from the client.
       *
       * @param now Time the ping was received.
       */
      void on_ping(std::chrono::steady_clock::time_point now);

      /**
       * @brief Whether at least one ping was recorded.
       *
       * @return `true` once `on_ping()` was called.
       */
      bool armed() const;

      /**
       * @brief Whether the client stopped pinging.
       *
       * @param now Current time.
       * @param timeout Allowed time since the last ping.
       * @return Same as `media_liveness_t::expired()`.
       */
      bool expired(std::chrono::steady_clock::time_point now, std::chrono::milliseconds timeout) const;

    private:
      mutable std::mutex _mutex;  ///< Guards `_tracker`.
      media_liveness_t _tracker;  ///< Ping tracker.
    };

    /**
     * @brief Thread-safe map from a ping key (ping payload, or client address for legacy clients) to a session's tracker.
     */
    class media_ping_registry_t {
    public:
      /**
       * @brief Route pings with this key to a tracker. An existing entry for the key is replaced.
       *
       * @param key Ping payload or client address.
       * @param tracker Tracker of the session that owns the key. Nothing happens when it is null.
       */
      void add(const std::string &key, std::shared_ptr<shared_media_liveness_t> tracker);

      /**
       * @brief Remove every key that routes to this tracker. Keys routed to other trackers stay.
       *
       * @param tracker Tracker of the ending session.
       */
      void remove(const std::shared_ptr<shared_media_liveness_t> &tracker);

      /**
       * @brief Record a ping for the tracker registered under this key.
       *
       * The registry lock is not held while the tracker is updated.
       *
       * @param key Ping payload or client address.
       * @param now Time the ping was received.
       * @return `true` when a tracker was registered for the key.
       */
      bool on_ping(const std::string &key, std::chrono::steady_clock::time_point now);

      /**
       * @brief Number of registered keys.
       *
       * @return Key count.
       */
      std::size_t size() const;

    private:
      mutable std::mutex _mutex;  ///< Guards `_trackers`.
      std::map<std::string, std::shared_ptr<shared_media_liveness_t>, std::less<>> _trackers;  ///< Trackers by key.
    };
  }  // namespace liveness
}  // namespace stream
