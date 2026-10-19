/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

// local includes
#include "crypto.h"
#include "handoff/types.h"
#include "stream_liveness.h"
#include "thread_safe.h"

namespace stream {
  struct session_t;

  namespace session {
    enum class state_e : int;
  }  // namespace session
}  // namespace stream

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 21;  ///< GameStream base-port offset used for the RTSP setup listener.

  /**
   * @brief RTSP launch session state shared with stream setup.
   */
  struct launch_session_t {
    uint32_t id;  ///< RTSP launch-session identifier assigned before stream startup.

    crypto::aes_t gcm_key;  ///< AES-GCM key negotiated for encrypted RTSP messages.
    crypto::aes_t iv;  ///< Initial RTSP AES-GCM IV supplied by the client.

    std::string av_ping_payload;  ///< AV ping payload.
    uint32_t control_connect_data;  ///< Client-provided token used when connecting the control channel.

    bool host_audio;  ///< Whether host audio should be played locally.
    std::string unique_id;  ///< Moonlight client unique identifier for this launch request.
    int width;  ///< Frame or display width in pixels.
    int height;  ///< Frame or display height in pixels.
    int fps;  ///< Requested video frame rate.
    int gcmap;  ///< Game controller mapping requested by the client.
    int appid;  ///< Application ID requested for launch or resume.
    int surround_info;  ///< Encoded GameStream surround-sound capability flags.
    std::string surround_params;  ///< Client-provided surround-sound layout parameters.
    bool continuous_audio;  ///< Whether audio packets continue during silence.
    bool enable_hdr;  ///< Whether HDR streaming is requested.
    bool enable_sops;  ///< Whether sequence output protection is requested.
    std::string client_name;  ///< Friendly client name from initial pairing.

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;  ///< AES-GCM cipher used once encrypted RTSP is negotiated.
    std::string rtsp_url_scheme;  ///< URL scheme selected by the RTSP SETUP flow.
    uint32_t rtsp_iv_counter;  ///< Counter value mixed into encrypted RTSP IVs.
    std::string client_cert;  ///< PEM certificate for the paired Moonlight client.
  };

  /**
   * @brief Queue a launch session until the RTSP client connects.
   *
   * @param launch_session Session state prepared by the GameStream launch handler.
   */
  void launch_session_raise(std::shared_ptr<launch_session_t> launch_session);

  /**
   * @brief Clear state for the specified launch session.
   * @param launch_session_id The ID of the session to clear.
   */
  void launch_session_clear(uint32_t launch_session_id);

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int session_count();

  /**
   * @brief Map a handoff shutdown reason to the reason that session stops record.
   *
   * `terminated` maps to `stop_reason_e::terminated` and `stop` maps to `stop_reason_e::stop`. Shared code calls it with
   * `handoff::hooks::shutdown_reason()`, so a normal shutdown keeps recording `terminated`.
   *
   * @param reason Shutdown reason from `handoff::hooks::shutdown_reason()`.
   * @return The matching session stop reason. Pure; callable from any thread.
   */
  stream::stop_reason_e to_stop_reason(handoff::shutdown_reason_e reason);

  /**
   * @brief Terminates all running streaming sessions.
   *
   * Every session present when the call starts is stopped with `reason` and joined, as `clear(true)` does. A session
   * that is already stopping keeps the reason it recorded first (`stream::session::stop` semantics), so the reason is
   * only the one recorded when this call is the first to stop the session. The application is not touched here; the
   * gamepad teardown is the same as before.
   *
   * Thread-safety: as before; takes the clear lock and then the slots lock, joins outside the slots lock. Never call it
   * while holding the clear lock or the slots lock.
   *
   * @param reason Stop reason to record. The default `stop_reason_e::terminated` keeps the existing behaviour for every
   *               caller in the tree (web UI, unpair, `/cancel`, shutdown without a handoff); `stop` is for a draining
   *               process that was stopped, and `drain_deadline` for the drain deadline.
   */
  void terminate_sessions(stream::stop_reason_e reason = stream::stop_reason_e::terminated);
  /**
   * @brief Terminate active sessions associated with a client certificate.
   *
   * @param cert Certificate data or object used by the operation.
   */
  void terminate_sessions_by_cert(std::string_view cert);

  /**
   * @brief Minimal description of one streaming session, for an operator to pick a session to close.
   *
   * It holds only what is needed to choose a session. It never contains client identities (names, UUIDs,
   * addresses, certificates), keys, IVs, ping payloads or connect data.
   */
  struct session_info_t {
    std::uint32_t id {0};  ///< Launch session id; stable for the session's lifetime.
    std::chrono::system_clock::time_point started {};  ///< Time the session was created.
    std::string transport;  ///< `udp/ipv4` or `udp/ipv6`.
    std::string client_category;  ///< Network class of the client: `local`, `lan` or `wan`.
  };

  /**
   * @brief List the live streaming sessions (running or stopping) that the RTSP server tracks.
   *
   * Sessions that have not started yet are not listed.
   *
   * @return One entry per live session, ordered by id.
   */
  std::vector<session_info_t> list_sessions();

  /**
   * @brief Outcome of a session close request.
   */
  enum class close_result_e {
    closed,  ///< The session was running and this request stopped it.
    already_stopping,  ///< The session was already stopping, or another stop won the race.
    not_found,  ///< No session with this id, or it has not started yet.
  };

  /**
   * @brief Ask one streaming session to stop, through the normal stop path.
   *
   * The session is stopped with reason `api-close`; the RTSP server then joins and removes it as usual.
   * Callers must authenticate the requester first (admin web UI only).
   *
   * @param id Launch session id from `list_sessions()`.
   * @return `closed` when this request stopped the running session, `already_stopping` when the session was already
   *         stopping or another stop won the race, `not_found` for an unknown id or a session that has not started yet.
   */
  close_result_e request_session_close(std::uint32_t id);

#ifdef SUNSHINE_TESTS
  /**
   * @brief Test-only access to the RTSP server's session slots.
   */
  namespace testing {
    /**
     * @brief Insert a fake session with no worker threads into the RTSP server's slots.
     *
     * The session describes itself with the insertion time, `udp/ipv4` and `local`. Joining it (through `clear`)
     * completes at once and does not run the last-session platform callbacks.
     *
     * @param id Launch session id.
     * @param state Initial session state.
     * @param join_gate When valid, joining the session waits until this future is ready.
     * @return The inserted session.
     */
    std::shared_ptr<stream::session_t> insert_fake_session(std::uint32_t id, stream::session::state_e state, std::shared_future<void> join_gate = {});

    /**
     * @brief Run the RTSP server's `clear()`.
     *
     * The sessions that are ended (all of them with `all`, otherwise the stopping ones) are stopped with `reason`, which
     * defaults to `terminated`, the reason `clear()` recorded before the parameter existed. A session that is already
     * stopping keeps its first recorded reason. The reason of each ended session is then visible through
     * `stream::session::stop_reason()` on the `shared_ptr` that `insert_fake_session` returned.
     *
     * @param all `true` to end every session, `false` to end only stopping ones.
     * @param reason Stop reason to record for the sessions this call stops.
     */
    void clear(bool all, stream::stop_reason_e reason = stream::stop_reason_e::terminated);

    /**
     * @brief Remove every session from the slots without joining them.
     */
    void remove_all();
  }  // namespace testing
#endif

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
