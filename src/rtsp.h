/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
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
   * @brief Get the number of active sessions of this process and of the draining process, if one exists.
   *
   * Used where a decision depends on whether anything is still streaming (the web UI's update-client path), because
   * during a drain handoff the old process keeps its sessions while this process holds the app. `session_count()`
   * stays per process, so the first `/launch` or `/resume` of a new process still reprobes its encoders.
   *
   * Result: `session_count()` plus the number of sessions the draining process reports. With no draining process (the
   * feature is off, no handoff happened, the draining process is gone, or on a platform without drain handoff) the
   * result equals `session_count()` and nothing is sent. On a platform other than Linux it always equals
   * `session_count()` (the Linux-only handoff code is not compiled there). With a draining process the call sends one `SESSIONS?` and
   * waits up to 500 ms for the reply; a reply with N entries adds N. A `SESSIONS?` that gets no reply in that time
   * (timeout, lost channel, failed send) is unknown and adds 1, so the result is then at least 1 and "no sessions" is
   * never claimed on a guess. A draining process that is found gone while the call runs adds 0.
   *
   * Thread-safety: any thread except the RTSP loop thread's own handlers; may block up to 500 ms; holds no lock while
   * it waits and must not be called while holding the RTSP slots lock, the clear lock or a lock that a channel reader
   * handler needs. Two concurrent calls send two independent requests.
   *
   * @return Count of sessions in both processes; at least 1 when the draining process could not be asked in time.
   */
  int total_session_count();

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
   * @brief Terminate active sessions whose client certificate has the given SHA-256 hash.
   *
   * Used by the draining process for `CLOSE_CERT <hash>` (the active process cannot send the certificate itself, only
   * its hash). A session matches when the lowercase hex of `crypto::hash(client_cert)` (SHA-256 of the exact PEM string
   * stored in its launch session, the string `terminate_sessions_by_cert` compares) equals `sha256_hex`. A session with
   * an empty client certificate never matches, even if `sha256_hex` is the hash of the empty string.
   *
   * Effects for the matching sessions are those of `terminate_sessions_by_cert`: stopped with `reason`, joined, erased
   * and logged by the normal path, and the gamepads of their certificates terminated (`input::terminate_gamepads`).
   * Sessions of other clients are untouched, and nothing else (application, pending launch, listener) changes.
   *
   * Hex comparison is exact, not case-insensitive: `sha256_hex` must be exactly 64 lowercase hexadecimal characters
   * (the form `crypto::hash` is rendered in, and the only form the channel decoder accepts). Anything else (wrong
   * length, uppercase digits, other characters) does nothing and returns 0, with no log above Debug.
   *
   * Counting: a session counts as "ended" when this call moved it to stopping, that is, when `stream::session::stop`
   * returned `true` for it. A matching session that was already stopping (another stop won) is still joined and erased
   * by this call, but it is not counted. The count is what the responder puts in the `CLOSED_CERT` reply.
   *
   * Thread-safety and locking: as `terminate_sessions_by_cert` (takes the clear lock, then the slots lock for the
   * selection only, joins outside the slots lock). It may block for as long as the joins take, so a responder calls it
   * on its own worker thread, never on the channel reader thread. Never call it holding the clear lock or the slots
   * lock. Concurrent calls and concurrent `terminate_sessions` calls are serialised by the clear lock; a session is
   * joined once, and a later caller only sees sessions still present when it selects.
   *
   * @param sha256_hex Lowercase hex SHA-256 of the client certificate (64 characters).
   * @param reason Stop reason to record; a session that is already stopping keeps its first reason. The default
   *        `terminated` is what the spec requires for a revoked client.
   * @return Number of sessions this call moved to stopping; 0 when none matched or the hash is invalid.
   */
  std::size_t terminate_sessions_by_cert_hash(std::string_view sha256_hex, stream::stop_reason_e reason = stream::stop_reason_e::terminated);

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

  /**
   * @brief Hand the RTSP server a listening socket that already exists, instead of binding one in `start()`.
   *
   * Drain handoff (new process): the RTSP listener is inherited from the old process, bound to the same port and
   * already listening. Call it at most once, before `start()`, from any thread (normally the thread that will call
   * `start()`). It only records the request and does no I/O, so it cannot block.
   *
   * What `start()` then does differently: it does not call `bind`, `listen` or `set_option(reuse_address)`; it takes
   * `::dup(fd)`, finds the address family with `getsockname`, and assigns the duplicate to the acceptor
   * (`acceptor.assign(tcp::v4() or tcp::v6(), dup)`). The caller's `fd` is never closed or modified, so the caller keeps
   * ownership and may close it after `start()` has returned or the process exits; the duplicate is owned by the
   * server and closed by `stop()`, `close_listener()` or destruction. The open file description is shared with every
   * other holder of the socket, so its accept queue is shared too. If `paused` is `true`, `start()` arms no accept and
   * the listener accepts nothing until `resume_accept()`; connections queue in the kernel backlog. If `paused` is
   * `false` the first accept is armed as in a normal start. Either way a work guard on the `io_context` is held (see
   * `pause_accept`), so the loop idles instead of spinning.
   *
   * Failure inside `start()` (the fd is not a listening stream socket, has an unsupported family, `dup` or `assign`
   * fails) is handled like a failed `bind`: one Fatal log, the shutdown event is raised and `start()` returns.
   *
   * What stays as before for a normal start: without `adopt_listener`, `start()` binds exactly as today
   * (`reuse_address`, `bind`, `listen(4096)`, the configured address family and port) and arms the first accept; the
   * work guard is additionally held and changes nothing observable there.
   *
   * @param fd A bound, listening, stream TCP socket (`AF_INET` or `AF_INET6`).
   * @param paused Whether accepting starts paused (the new process before `COMMIT`).
   * @return `true` when the request was recorded. `false`, with nothing recorded, when `fd < 0`, when a listener was
   *         already adopted, when `start()` has already begun, or on a platform other than Linux (where drain handoff
   *         does not exist and `start()` always binds).
   */
  bool adopt_listener(int fd, bool paused);

  /**
   * @brief Register a function that `start()` calls once the listener exists.
   *
   * Used for the `READY=1` latch, which counts one "listening" signal from each of the four servers. Call before
   * `start()`, from any thread; a second call replaces the first. `start()` calls it once, on the thread that runs
   * `start()`, right after the listener was bound (a normal start) or adopted (`adopt_listener`), and before the first
   * accept is armed and before the loop runs. It is not called when binding or adopting fails. The function must not
   * block and must not call `pause_accept`, `resume_accept` or `close_listener` (it runs on the loop thread's setup path,
   * where those return `false`).
   *
   * @param callback Called with the local TCP port of the listener. An empty function removes the registration.
   */
  void set_listening_callback(std::function<void(unsigned short)> callback);

  /**
   * @brief The RTSP listening socket, for the old process to pass to a new one.
   *
   * Works after a normal bind and after an adopted start. The descriptor is owned by the server: it stays valid until
   * `stop()` (end of `start()`), `close_listener()` or exit, and the caller must not close it. To give it to a child
   * the caller passes it to `spawn_child`, which duplicates it onto the child's descriptor number.
   *
   * @return The listening descriptor, or -1 before the listener exists (before `start()` bound or adopted it) and after
   *         it was closed. Thread-safety: any thread, never blocks (it reads an atomic copy).
   */
  int listener_fd();

  /**
   * @brief Stop arming accepts on the RTSP listener, and wait until that is true.
   *
   * Runs on the RTSP loop thread (the thread that calls `iterate()`): the call posts a handler to the server's
   * `io_context` and waits for it. The handler sets the paused flag, remembers which accept it cancels, cancels the
   * pending accept and fulfils the promise the caller waits on; it does not close the listener, end a session, touch a
   * pending launch or stop the loop. The loop keeps running on its work guard, so the loop does not spin: with no
   * session `iterate()` blocks in `run_one()` and wakes only for a posted handler, a session or `stop()`.
   *
   * The `operation_aborted` completion of the cancelled accept is silent: no Error log, no `clear()`, no re-arm. Only
   * the completion of an accept that this pause cancelled is silent. The implementation must identify the cancelled
   * accept itself (for example with a counter that each accept's completion handler captures), not with one shared
   * boolean, so that a real accept error is still an Error plus `clear()`, as today, whether it happens before, during
   * or after a pause or resume.
   *
   * An accept that had already succeeded when the pause ran, and whose handler has not run yet, is handled as today
   * (read the request if a launch is pending, else close the connection) and does not arm another accept. Connections
   * that arrive while paused wait in the kernel backlog, where the next process that accepts on the shared socket takes
   * them. Accept is speculative, so a connection already queued may still be accepted by a handler that had completed;
   * the pause promises only that no further accept is armed.
   *
   * Overlap rules. All of these hold for `pause_accept`, `resume_accept` and `close_listener`, which are executed in the
   * order their handlers are posted:
   * - Pause while a pause is in progress (two threads): both calls return `true`; the second is a barrier on the loop
   *   thread and arms and cancels nothing a second time.
   * - Resume called while a pause has not completed: the commands run in posting order, so the later call decides the
   *   final state; the resume never runs "inside" the pause. If the pause was posted first, the final state is
   *   accepting.
   * - Resume that runs before the aborted completion of the earlier pause has been handled: the resume must not arm
   *   an accept while a previous accept's completion (success or abort) is still to be handled. Rule: at most one
   *   accept is outstanding; the next one is armed, when not paused, by whoever finishes handling the previous
   *   completion (the resume handler when none is outstanding, otherwise the completion handler). So there is never a
   *   second accept on the same `next_socket`.
   * - A command timing out: its handler stays queued and may still run later; the call returns `false`.
   * - `stop()` while a call waits: the call returns `false` promptly, not after its timeout.
   *
   * @param timeout Longest wait for the loop thread to run the handler.
   * @return `true` when, on return, accepting is paused and no new accept will be armed. `false` when the listener does
   *         not exist or is closed (nothing to pause), when the call is made on the loop thread itself (a command
   *         handler, `handle_accept` or the listening callback: it would wait for itself, so it returns at once without
   *         posting), or when the loop thread did not run the handler within `timeout`.
   *
   * Thread-safety: any thread except the loop thread. Holds no lock of the server while it waits; must not be called
   * under the slots lock, the clear lock or a lock that a handler on the loop thread takes. Before the loop thread has
   * started running but after the listener exists, the call waits for it (up to `timeout`).
   */
  bool pause_accept(std::chrono::milliseconds timeout = std::chrono::milliseconds {5000});

  /**
   * @brief Arm accepting again after `pause_accept`, or for the first time after a paused adopted start.
   *
   * Posts to the loop thread: clear the paused flag and, if no accept is outstanding and the listener is open, arm one
   * accept on `next_socket`. Connections that queued in the backlog while paused are accepted (asio tries the `accept`
   * system call when the operation starts). Idempotent: resuming a server that is not paused returns `true` and arms
   * nothing; it never produces a second outstanding accept. For the overlap rules (resume before the pause completed,
   * resume racing the aborted completion of a pause, timeouts, `stop()`), see `pause_accept`.
   *
   * @param timeout Longest wait for the loop thread to run the handler.
   * @return `true` when accepting is armed or will be armed by the completion handler that is still to run. `false`
   *         when the listener does not exist or was closed by `close_listener`, when called on the loop thread, or on
   *         timeout.
   *
   * Thread-safety: as `pause_accept`.
   */
  bool resume_accept(std::chrono::milliseconds timeout = std::chrono::milliseconds {5000});

  /**
   * @brief Close this process's copy of the RTSP listening socket without touching sessions.
   *
   * For switch substep 10, `close_socket_copies`: the draining process must hold no listener. Posts to the loop thread:
   * cancel the pending accept (silently, as `pause_accept`), set the paused flag, close the acceptor and clear the
   * atomic copy that `listener_fd()` reads. It ends no session, leaves a pending launch alone and does not stop the
   * loop, which keeps idling on the work guard until `stop()`. The socket stays open in every other process that holds
   * it. Idempotent. After it `listener_fd()` is -1 and `resume_accept` returns `false`. Overlap rules as `pause_accept`.
   *
   * @param timeout Longest wait for the loop thread.
   * @return `true` when the listener is closed (or already was); `false` when it never existed, when called on the loop
   *         thread, or on timeout.
   *
   * Thread-safety: as `pause_accept`.
   */
  bool close_listener(std::chrono::milliseconds timeout = std::chrono::milliseconds {5000});

  /**
   * @brief Whether a launch session is raised and its client has not connected yet.
   *
   * True from `launch_session_raise` until the session is cleared (`launch_session_clear`, the client's RTSP
   * connection), dropped (`drop_pending_launch`) or expired (`ping_timeout`). The handoff switch re-checks it (substep 4)
   * and step 1 refuses on it.
   *
   * @return `true` while a launch session is pending. Thread-safety: any thread; takes the launch event's own lock
   *         briefly; never blocks on the loop.
   */
  bool has_pending_launch();

  /**
   * @brief Drop the pending launch session, if any, and cancel its expiry timer.
   *
   * Switch substep 4: with accepting paused, a raised launch could only expire, because its client's RTSP connection
   * would reach the new process. The launch event is popped (waiters of `launch_event` see no session) and the timer is
   * cancelled; the timer's late completion never discards another session (it compares the session it was armed for, so
   * a launch raised right after the drop is not removed by the expiry of the dropped one). The dropped session's own
   * state in `nvhttp` is not touched. Nothing else changes: sessions, the listener and the paused state stay as they were.
   *
   * Runs on the calling thread, not on the loop thread, so it works when the loop is not running or is blocked, and it
   * may be called from the loop thread. All operations on the launch event and its timer (`launch_session_raise`,
   * `launch_session_clear`, the timer arming and cancelling, this call) are serialised by one internal mutex that is
   * never held across a wait or a callback, because the asio timer is not safe for concurrent use.
   *
   * Race with `launch_session_raise`: if a raise runs concurrently, the result is as if one happened entirely before the
   * other: raise then drop leaves nothing pending; drop then raise leaves the new launch pending (and armed). During a
   * handoff the 503 gate and the in-flight wait (substeps 2 and 3) keep `/launch` from running, so this race only
   * matters outside a switch. A second drop in a row returns `false`.
   *
   * @return `true` when a pending launch existed and this call removed it; `false` when none was pending.
   *
   * Thread-safety: any thread. Never blocks beyond the internal mutex.
   */
  bool drop_pending_launch();

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

    /**
     * @brief Prepare the RTSP server to accept on a test-owned listening socket, as `start()` would, without starting a
     * loop thread or registering handlers on the global shutdown event.
     *
     * Does what `start()` does for `adopt_listener(fd, paused)` after the adoption has been recorded: `::dup(fd)`, assign
     * to the acceptor, create `next_socket`, take the work guard, store `paused`, publish `listener_fd()`, call the
     * listening callback, and arm the first accept unless `paused`. The caller's `fd` is not closed. The RTSP command
     * handlers are registered if they are not yet. Call `start_loop` afterwards.
     *
     * @param fd A bound, listening, stream TCP socket, for example a loopback socket the test created.
     * @param paused Whether accepting starts paused.
     * @return `true` on success; `false` when `fd` is invalid, a listener is already open (call `reset_server` first),
     *         or assignment failed (nothing changed).
     */
    bool open_listener(int fd, bool paused);

    /**
     * @brief Start a thread that runs the RTSP loop of `start()`: `iterate()`, then `clear(false)`, until `stop_loop`.
     *
     * This thread is the "loop thread" of `pause_accept`, `resume_accept` and `close_listener`. It does not look at the
     * global shutdown event. Call once after `open_listener`.
     */
    void start_loop();

    /**
     * @brief Stop the thread of `start_loop`: as the shutdown path does, call the server's `stop()` (closes the
     * listener, stops the `io_context`, ends all sessions with `terminated`) and join the thread. Safe to call twice and
     * without `start_loop`.
     */
    void stop_loop();

    /**
     * @brief Return the server to its state before `open_listener`: stop the loop if it runs, close the listener, drop
     * the adoption record and the listening callback, restart the `io_context` so a new `open_listener` works, drop
     * the pending launch, remove every session without joining it, and zero the counters below.
     */
    void reset_server();

    /**
     * @brief How many times `iterate()` has returned since `open_listener` or `reset_server`. An idle loop must stay
     * (nearly) constant over a quiet interval: a spinning loop grows it by thousands.
     *
     * @return The count.
     */
    std::uint64_t loop_iterations();

    /**
     * @brief How many accept completions (successful accepts) `handle_accept` has handled since `open_listener` or
     * `reset_server`. A connection queued while paused is counted after `resume_accept`.
     *
     * @return The count.
     */
    std::uint64_t accepted_connections();

    /**
     * @brief How many `operation_aborted` or other error completions `handle_accept` treated as a real error (the
     * Error log plus `clear()` path) since `open_listener` or `reset_server`. Pausing and resuming must leave it at 0.
     *
     * @return The count.
     */
    std::uint64_t accept_errors();
  }  // namespace testing
#endif

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
