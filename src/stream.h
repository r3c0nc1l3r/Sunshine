/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <future>
#include <memory>
#include <utility>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "crypto.h"
#include "rtsp.h"
#include "stream_liveness.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;  ///< GameStream base-port offset used for the video UDP stream.
  constexpr auto CONTROL_PORT = 10;  ///< GameStream base-port offset used for the control channel.
  constexpr auto AUDIO_STREAM_PORT = 11;  ///< GameStream base-port offset used for the audio UDP stream.

  struct session_t;

  /**
   * @brief Stream configuration shared by capture and network senders.
   */
  struct config_t {
    audio::config_t audio;  ///< Audio capture configuration for the stream.
    video::config_t monitor;  ///< Video capture and encoder configuration for the selected monitor.

    int packetsize;  ///< Maximum payload size for network packets.
    int minRequiredFecPackets;  ///< Minimum recovery packets required before FEC is emitted.
    int mlFeatureFlags;  ///< Moonlight feature flags negotiated for this session.
    int controlProtocolType;  ///< GameStream control protocol variant selected by the client.
    int audioQosType;  ///< Audio QoS type.
    int videoQosType;  ///< Video QoS type.

    uint32_t encryptionFlagsEnabled;  ///< Bitmask of GameStream encryption features enabled for the session.

    std::optional<int> gcmap;  ///< Optional game-controller mapping override from the launch request.
  };

  namespace session {
    /**
     * @brief Enumerates supported state options.
     */
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    /**
     * @brief Allocate and initialize platform input state for a stream.
     *
     * @param config Configuration values to apply.
     * @param launch_session Launch session.
     * @param peer_address Client address as seen by the RTSP server. It is used only to derive the session's
     *                     `transport` and `client_category`; the address itself is not stored.
     * @return Allocated object or identifier, or an error value on failure.
     */
    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session, std::string_view peer_address = {});
    /**
     * @brief Start a streaming session for the supplied peer address.
     *
     * @param session Active streaming or pairing session for the request.
     * @param addr_string Addr string.
     * @return Start status.
     */
    int start(session_t &session, const std::string &addr_string);
    /**
     * @brief Stop a streaming session and prevent more packets from being queued.
     *
     * @param session Active streaming or pairing session for the request.
     */
    void stop(session_t &session);
    /**
     * @brief Stop a streaming session and record why it ended.
     *
     * The reason is recorded only by the call that moves the session from running to stopping, so the first reason
     * wins. Stopping a session that is not running records nothing.
     *
     * @param session Active streaming or pairing session for the request.
     * @param reason Why the session is being stopped.
     * @return `true` when this call moved the session from running to stopping (its reason is recorded).
     */
    bool stop(session_t &session, stop_reason_e reason);
    /**
     * @brief Get the reason recorded for the end of a session.
     *
     * @param session Active streaming or pairing session for the request.
     * @return The first recorded stop reason, or `stop_reason_e::unknown`.
     */
    stop_reason_e stop_reason(const session_t &session);
    /**
     * @brief Get the launch session id of a session.
     *
     * Safe to call from any thread: it reads only an immutable field.
     *
     * @param session Streaming session.
     * @return Launch session id.
     */
    std::uint32_t id(const session_t &session);
    /**
     * @brief Describe a session for the session list.
     *
     * Safe to call from any thread: it reads only immutable fields and the start time.
     *
     * @param session Streaming session to describe.
     * @return Public description of the session.
     */
    rtsp_stream::session_info_t describe(const session_t &session);
    /**
     * @brief Wait for worker threads owned by the session to exit.
     *
     * @param session Active streaming or pairing session for the request.
     */
    void join(session_t &session);
    /**
     * @brief Platform handle returned from stream setup.
     *
     * @param session Active streaming or pairing session for the request.
     * @return Current lifecycle state for the stream session.
     */
    state_e state(session_t &session);

    /**
     * @brief Number of sessions of this process that are running.
     *
     * A lock-free read of the running-session counter (`running_sessions`, changed when a session starts and when it
     * stops). It takes no lock, never joins and never waits, so it is safe under any lock and from any thread. The
     * handoff's session notifier and drain controller read it instead of `rtsp_stream::session_count()`.
     *
     * @return The current count.
     */
    int running_session_count();
    /**
     * @brief Return the paired client certificate for a stream session.
     *
     * @param session Active streaming or pairing session for the request.
     * @return PEM certificate associated with the session's client.
     */
    const std::string &client_cert(session_t &session);

#ifdef SUNSHINE_TESTS
    /**
     * @brief Test-only helpers.
     */
    namespace testing {
      /**
       * @brief Create a session with no worker threads.
       *
       * `join()` of the returned session skips thread joins, the control wait, the input reset, the hang guard and the
       * last-session platform callbacks. It describes itself with the creation time, `udp/ipv4` and `local`.
       *
       * @param id Launch session id.
       * @param state Initial session state.
       * @param join_gate When valid, `join()` waits until this future is ready.
       * @return The new session.
       */
      std::shared_ptr<session_t> make_fake(std::uint32_t id, state_e state, std::shared_future<void> join_gate);
    }  // namespace testing
#endif
  }  // namespace session
}  // namespace stream
