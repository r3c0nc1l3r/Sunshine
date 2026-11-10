/** @file src/platform/linux/private_seat_input.h
 * @brief Fail-closed private-seat runtime and future capture handoff.
 */
#pragma once
#include "private_seat_client.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

/** @brief Linux private-seat producer context. */
namespace platf::private_seat_input {
  /** @brief Independent evdev contributor and alias tracker for the dispatcher. */
  class key_tracker {
  public:
    /** @brief Emit a transition; false signals a terminal producer failure. */
    using sink = std::function<bool(std::uint16_t, bool)>;
    /**
     * @brief Press a physical contributor, wrapping its down in transient synthetic modifiers.
     * @param contributor Physical contributor identity.
     * @param code Evdev key code for the press.
     * @param modifiers Transient evdev modifier codes.
     * @param emit Sink for boundary transitions.
     * @return True when the press and modifier transitions succeed.
     */
    bool down(std::uint64_t contributor, std::uint16_t code, const std::vector<std::uint16_t> &modifiers, const sink &emit);
    /**
     * @brief Release the evdev code recorded by the corresponding physical down.
     * @param contributor Physical contributor identity.
     * @param emit Sink for boundary transitions.
     * @return True when the contributor was released successfully.
     */
    bool up(std::uint64_t contributor, const sink &emit);
    /** @brief Remove all contributors without sending releases. */
    void reset();
    /**
     * @brief Return whether this physical contributor is currently held.
     * @param contributor Physical contributor identity.
     * @return True if the contributor is held.
     */
    bool held(std::uint64_t contributor) const;

  private:
    /** @brief Add or remove one evdev reference and emit only boundary transitions. */
    bool change(std::uint16_t code, bool down, const sink &emit);
    std::map<std::uint64_t, std::uint16_t> contributors_;  ///< Physical contributors and their original mapping.
    std::map<std::uint16_t, unsigned> refs_;  ///< Physical and transient synthetic references by evdev code.
  };

  /** @brief Capture owner's evidence from one authenticated Wayland connection. */
  struct capture_proof {
    private_seat::peer_credentials wayland_peer;  ///< Credentials from the actual capture fd.
    std::uint64_t producer_generation = 0;  ///< Local context generation associated with the snapshot.
    std::uint64_t output_id = 0;  ///< Bound wl_output identity.
    std::uint64_t epoch = 0;  ///< Binding snapshot epoch.
    std::uint64_t capture_generation = 0;  ///< Unique local capture instance.
    std::uint64_t frame_output_id = 0;  ///< Identity attached to the published frame.
    std::uint64_t frame_epoch = 0;  ///< Epoch attached to the published frame.
    std::uint64_t frame_capture_generation = 0;  ///< Capture instance attached to the frame.
    std::uint64_t snapshot_sequence = 0;  ///< Local serial before post-snapshot frame request.
    std::uint64_t frame_request_sequence = 0;  ///< Serial of the published frame request.
    std::uint64_t latest_published_sequence = 0;  ///< Latest frame published for this capture instance.
    std::uint32_t physical_width = 0;  ///< Snapshot physical width.
    std::uint32_t physical_height = 0;  ///< Snapshot physical height.
    std::uint32_t logical_width = 0;  ///< Snapshot logical width.
    std::uint32_t logical_height = 0;  ///< Snapshot logical height.
    std::uint32_t frame_width = 0;  ///< Published frame width.
    std::uint32_t frame_height = 0;  ///< Published frame height.
    std::uint32_t stream_width = 0;  ///< Encoder stream width.
    std::uint32_t stream_height = 0;  ///< Encoder stream height.
    bool sole_full_normal_origin = false;  ///< Sole full output, normal transform, origin zero.
    bool post_snapshot_frame_published = false;  ///< Frame requested after snapshot and published.
  };

  /**
   * @brief Shared capture publication and input barrier for one capture instance.
   *
   * The capture owner holds mutex across the actual frame-publication callback and
   * updates latest only after that callback succeeds. Invalidate synchronously
   * before any capture change; never revalidate a retired gate. Create a new gate
   * for each capture instance. No blocking waits or blocking I/O are allowed under mutex;
   * the private client flushes only a bounded byte budget on its nonblocking socket.
   */
  struct capture_gate {
    std::mutex mutex;  ///< Serializes frame publication, invalidation, qualification and input queueing.
    bool valid = true;  ///< False permanently retires this capture instance; protected by mutex.
    std::optional<capture_proof> latest;  ///< Latest actually published frame proof; protected by mutex.

    /** @brief Synchronously retire the gate and discard its proof. */
    void invalidate();
  };

  /** @brief Values received with the fresh WELCOME cleanup acknowledgement. */
  struct capture_transition {
    std::uint64_t producer_generation;  ///< Local producer generation for the new connection.
    private_seat::welcome welcome;  ///< Authenticated server announcement.
    private_seat::peer_credentials peer;  ///< Authenticated socket peer credentials.
  };

  /**
   * @brief Allocate one process-monotonic capture identity shared by capture owners.
   * @return A nonzero fresh identity, or zero on counter exhaustion.
   */
  std::uint64_t allocate_capture_generation();

  /** @brief Dispatcher-owned context; call only from one serialized dispatcher. */
  class context: public std::enable_shared_from_this<context> {
  public:
    /** @brief Construct an inactive context without opening a socket or creating devices. */
    context();
    /** @brief Asynchronous cleanup acknowledgement. */
    using welcome_callback = std::function<void(bool, std::uint64_t, private_seat::welcome, private_seat::peer_credentials)>;
    /**
     * @brief Retire the current producer and begin a fresh authenticated connection.
     * @param completion Optional callback receiving success, generation, WELCOME and peer.
     * @return True if the connection was started.
     */
    bool begin_transition(welcome_callback completion = {});
    /**
     * @brief Drive the connection with poll revents and current monotonic time.
     * @param revents Poll results, or zero for a timer tick.
     * @param now Current monotonic time.
     * @return True while the connection remains usable.
     */
    bool drive(short revents, private_seat::client::clock::time_point now);
    /**
     * @brief Return pollable descriptor or -1.
     * @return The descriptor, or -1 without a client.
     */
    int fd() const;
    /**
     * @brief Return required poll events.
     * @return Required poll events, or zero without a client.
     */
    short poll_events() const;
    /**
     * @brief Report a fresh WELCOME cleanup acknowledgement.
     * @return True when local reset is complete and WELCOME is acknowledged.
     */
    bool welcome_cleanup_ack() const;
    /**
     * @brief Return WELCOME, or null until acknowledged.
     * @return The acknowledged WELCOME, or null.
     */
    const private_seat::welcome *announcement() const;
    /**
     * @brief Return authenticated peer for future Wayland credential comparison.
     * @return Peer credentials, or default credentials without a client.
     */
    private_seat::peer_credentials peer() const;
    /**
     * @brief Validate capture evidence against the fresh authenticated WELCOME.
     * @param proof Capture evidence to validate.
     * @return True when the mapping is qualified.
     */
    bool publish_qualified_mapping(const capture_proof &proof);
    /**
     * @brief Attach and qualify the gate's current proof and queue READY once.
     * @param gate Shared gate for the current capture instance.
     * @return True only when READY was queued; call on the dispatcher.
     */
    bool qualify_capture(const std::shared_ptr<capture_gate> &gate);
    /**
     * @brief Send READY after capture qualification.
     * @return True when READY is queued.
     */
    bool ready();
    /**
     * @brief Report whether matching ACTIVE permits input.
     * @return True when qualified input is active.
     */
    bool active() const;
    /** @brief Retire the connection and clear producer state on the dispatcher. */
    void reset();
    /**
     * @brief Return the current producer generation, safe for queue tagging.
     * @return Current local producer generation.
     */
    std::uint64_t generation() const;
    /**
     * @brief Register dispatcher cleanup after invalidation.
     * @param callback Receives the new local generation; acknowledge that same token with reset_complete after clearing old state.
     */
    void on_reset(std::function<void(std::uint64_t)> callback);
    /**
     * @brief Emit an evdev keyboard transition after dispatcher aggregation.
     * @param code Evdev key code.
     * @param down True for a press, false for a release.
     * @return True when the transition is queued.
     */
    bool send_key(std::uint16_t code, bool down);
    /**
     * @brief Acknowledge completion of dispatcher state reset for this generation.
     * @param generation Generation whose reset has completed.
     */
    void reset_complete(std::uint64_t generation);
    /**
     * @brief Install dispatcher enqueue and connection pump callbacks before publication.
     * @param enqueue Callback that queues dispatcher work.
     * @param pump Callback that starts the generation-tagged connection pump.
     */
    void set_dispatcher(std::function<bool(std::function<void()>)> enqueue, std::function<void(std::weak_ptr<context>, std::uint64_t)> pump);
    /**
     * @brief Enqueue an operation on the dispatcher using weak lifetime ownership.
     * @param operation Work to run if the context and generation remain current.
     * @return True if the dispatcher accepted the operation.
     */
    bool post(std::function<void(context &)> operation);
    /**
     * @brief Enqueue a serialized capture-owner transition even if an older reset advances the generation first.
     * @param operation Work to run if the context remains alive.
     * @return True if the dispatcher accepted the operation.
     */
    bool post_transition(std::function<void(context &)> operation);
    /**
     * @brief Map raw Moonlight absolute packet coordinates using the qualified snapshot.
     * @param x Horizontal packet coordinate.
     * @param y Vertical packet coordinate.
     * @param packet_width Packet coordinate width.
     * @param packet_height Packet coordinate height.
     * @return True if the mapped event was queued.
     */
    bool absolute_packet(double x, double y, double packet_width, double packet_height);
    /**
     * @brief Queue relative motion if active.
     * @param x Horizontal relative motion.
     * @param y Vertical relative motion.
     */
    void move(int x, int y);
    /**
     * @brief Queue a Linux button transition if active.
     * @param button Input button identifier.
     * @param release True for release, false for press.
     */
    void button(int button, bool release);
    /**
     * @brief Queue horizontal or vertical wheel v120 if active.
     * @param amount Wheel displacement in v120 units.
     * @param horizontal True for horizontal, false for vertical.
     */
    void axis(int amount, bool horizontal);
    /**
     * @brief Map a virtual key with the shared mapper and queue it if active.
     * @param keycode Virtual key code.
     * @param release True for release, false for press.
     * @param flags Input flags; currently unused.
     */
    void key(std::uint16_t keycode, bool release, std::uint8_t flags);

  private:
    /** @brief Submit a validated event only while active. */
    void send(const private_seat::event &event);
    std::shared_ptr<capture_gate> gate_;  ///< Dispatcher-owned gate; its contents are mutex-protected.
    std::unique_ptr<private_seat::client> client_;  ///< Current producer connection.
    std::atomic<std::uint64_t> generation_;  ///< Local producer generation; never wraps.
    std::function<void(std::uint64_t)> reset_callback_;  ///< Invalidated generation notification.
    bool qualified_ = false;  ///< Capture proof has passed validation.
    bool reset_complete_ = true;  ///< Dispatcher has cleared old producer state.
    std::optional<capture_proof> mapping_;  ///< Immutable qualified capture mapping.
    std::mutex enqueue_mutex_;  ///< Protects dispatcher enqueue publication and copying.
    std::function<bool(std::function<void()>)> enqueue_;  ///< Dispatcher enqueue function.
    welcome_callback welcome_callback_;  ///< Pending cleanup acknowledgement callback.
    std::function<void(std::weak_ptr<context>, std::uint64_t)> pump_;  ///< Starts the bounded nonblocking poll loop.
  };

  /**
   * @brief Invalidate the old gate, request a fresh connection and wait for cleanup.
   * @param weak Weak published context handle.
   * @param old_gate Gate to synchronously retire before posting; may be null.
   * @return Fresh local generation, WELCOME and peer, or null on failure or timeout.
   *
   * Serialize transition attempts on the capture owner. Do not call from the input
   * dispatcher or with any gate mutex held. The wait is at most six seconds. A timed-out request remains
   * fail-closed; its late callback cannot access stack storage.
   */
  std::optional<capture_transition> begin_capture_transition(std::weak_ptr<context> weak, const std::shared_ptr<capture_gate> &old_gate);

  /**
   * @brief Post qualification of the current proof with weak context ownership.
   * @param weak Weak published context handle.
   * @param gate Capture instance whose latest proof will be read on the dispatcher.
   * @return Whether the dispatcher accepted the operation, not qualification success.
   *
   * Call after publishing a frame and updating gate->latest under the gate mutex.
   * The dispatcher reads the current proof, rather than a copy captured here.
   */
  bool post_capture_qualification(std::weak_ptr<context> weak, std::shared_ptr<capture_gate> gate);
}  // namespace platf::private_seat_input

/** @brief Input dispatch API used by capture owners. */
namespace input {
  /**
   * @brief Return a weak handle; use context::post for every operation.
   * @return Weak handle to the published context, possibly expired.
   */
  std::weak_ptr<platf::private_seat_input::context> private_seat_handle();
}  // namespace input
