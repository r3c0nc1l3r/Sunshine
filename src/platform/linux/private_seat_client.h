/**
 * @file src/platform/linux/private_seat_client.h
 * @brief Standalone client for the LanternBox private-seat LBS1 protocol.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <sys/types.h>
#include <vector>

/** @brief Private-seat wire and transport interfaces. */
namespace private_seat {
  /** @brief Maximum buffered bytes per direction. */
  constexpr std::size_t buffer_limit = 4096;

  /** @brief A server-issued identity and its capture instance. */
  struct tokens {
    std::uint64_t generation = 0;  ///< Server producer generation.
    std::uint64_t epoch = 0;  ///< Output epoch.
    std::uint64_t output_id = 0;  ///< Output instance identifier.
    std::uint64_t capture_generation = 0;  ///< Producer capture instance identifier.
  };

  /** @brief Output dimensions announced by the server. */
  struct welcome {
    tokens identity;  ///< Server identities; capture generation is zero.
    std::uint32_t physical_width = 0;  ///< Physical width in pixels.
    std::uint32_t physical_height = 0;  ///< Physical height in pixels.
    std::uint32_t logical_width = 0;  ///< Logical width in pixels.
    std::uint32_t logical_height = 0;  ///< Logical height in pixels.
  };

  /** @brief Full physical capture viewport. */
  struct viewport {
    std::uint32_t x = 0;  ///< Left edge, required to be zero.
    std::uint32_t y = 0;  ///< Top edge, required to be zero.
    std::uint32_t width = 0;  ///< Physical width.
    std::uint32_t height = 0;  ///< Physical height.
  };
  /** @brief Producer input message types. */
  enum class event_type : std::uint16_t {
    key = 10,  ///< Evdev keyboard transition.
    relative = 11,  ///< Relative logical motion.
    absolute = 12,  ///< Normalized absolute motion.
    button = 13,  ///< Linux button transition.
    axis = 14  ///< Horizontal and vertical wheel v120.
  };

  /** @brief Input event values; only fields for the selected type are used. */
  struct event {
    event_type type = event_type::key;  ///< Event kind.
    std::uint16_t code = 0;  ///< Evdev key or button code.
    bool down = false;  ///< Transition direction.
    double x = 0;  ///< Relative dx or normalized u.
    double y = 0;  ///< Relative dy or normalized v.
    std::int32_t horizontal = 0;  ///< Horizontal wheel v120.
    std::int32_t vertical = 0;  ///< Vertical wheel v120.
  };

  /** @brief Complete decoded server frame. */
  struct server_frame {
    std::uint16_t type = 0;  ///< WELCOME (1) or ACTIVE (3).
    welcome announcement;  ///< WELCOME values when type is 1.
    tokens identity;  ///< ACTIVE echo when type is 3.
  };
  /** @brief Result of incremental frame decoding. */
  enum class decode_result {
    incomplete,  ///< More bytes are needed.
    complete,  ///< A valid frame was decoded.
    invalid  ///< Protocol error.
  };  ///< Decoder progress or fatal protocol error.
  /**
   * @brief Decode one server frame without consuming incomplete input.
   * @param bytes Buffered stream bytes, bounded by buffer_limit.
   * @param frame Receives a complete frame.
   * @param consumed Receives the number of bytes consumed, or zero.
   * @return Decoder progress; invalid frames must close the connection.
   */
  decode_result decode_server_frame(const std::vector<std::uint8_t> &bytes, server_frame &frame, std::size_t &consumed);
  /**
   * @brief Encode a READY frame after validating its full viewport and identities.
   * @param announcement Most recent fresh WELCOME.
   * @param capture_generation Nonzero local capture instance.
   * @param area Full physical viewport.
   * @param bytes Receives the complete frame on success.
   * @return Whether the values are valid.
   */
  bool encode_ready(const welcome &announcement, std::uint64_t capture_generation, const viewport &area, std::vector<std::uint8_t> &bytes);
  /**
   * @brief Encode an event, validating wire bounds and clipping absolute coordinates.
   * @param identity Active identities including capture generation.
   * @param value Input event.
   * @param supported_key Exact shared Linux mapping membership predicate; required for keys.
   * @param bytes Receives the complete frame on success.
   * @return Whether the event is valid.
   */
  bool encode_event(const tokens &identity, const event &value, const std::function<bool(std::uint16_t)> &supported_key, std::vector<std::uint8_t> &bytes);
  /** @brief Client lifecycle states; closed is terminal. */
  enum class state {
    connecting,  ///< Socket connection pending.
    waiting_welcome,  ///< Awaiting cleanup acknowledgement.
    ready_to_send,  ///< Qualified capture may send READY.
    waiting_active,  ///< Awaiting matching ACTIVE.
    active,  ///< Events may be queued.
    closed  ///< Terminal connection state.
  };

  /** @brief Authenticated Linux peer credentials. */
  struct peer_credentials {
    pid_t pid = 0;  ///< Peer process ID in this namespace.
    uid_t uid = 0;  ///< Peer user ID in this namespace.
  };

  /**
   * @brief Nonblocking single-connection client. All operations belong to one dispatcher.
   *
   * Only the bounded nonblocking drive may run under the capture gate. The caller must
   * tag callbacks with its own local generation, compare the actual Wayland fd's
   * credentials, enforce the capture barrier and create a new client on failure.
   */
  class client {
  public:
    /** @brief Clock used for handshake and partial-frame deadlines. */
    using clock = std::chrono::steady_clock;
    /**
     * @brief Start one nonblocking pathname connection, authenticating before reads.
     * @param path Absolute Unix socket path.
     * @param expected_uid Required server UID.
     * @param supported_key Exact membership predicate for the shared evdev mapping.
     * @param now Current monotonic time.
     */
    client(const std::string &path, uid_t expected_uid, std::function<bool(std::uint16_t)> supported_key, clock::time_point now);
    /** @brief Close the connection. */
    ~client();
    /** @brief Clients are not copyable. */
    client(const client &) = delete;
    /** @brief Clients are not copy assignable. */
    client &operator=(const client &) = delete;
    /**
     * @brief Return the pollable descriptor, or -1 when closed.
     * @return The descriptor, or -1 when closed.
     */
    int fd() const;
    /**
     * @brief Return poll events currently required, including POLLIN/POLLOUT.
     * @return Required poll event mask.
     */
    short poll_events() const;
    /**
     * @brief Return lifecycle state.
     * @return Current lifecycle state.
     */
    state status() const;
    /**
     * @brief Return actionable terminal error, empty until failure.
     * @return Terminal error text, empty until failure.
     */
    const std::string &error() const;
    /**
     * @brief Return authenticated credentials after connection; pid is zero before authentication.
     * @return Peer credentials; pid is zero before authentication.
     */
    peer_credentials peer() const;
    /**
     * @brief Return the fresh WELCOME after status reaches ready_to_send.
     * @return The fresh WELCOME.
     */
    const welcome &announcement() const;
    /**
     * @brief Drive nonblocking I/O and deadlines using poll revents, including zero for timer ticks.
     * @param revents poll(2) result for fd().
     * @param now Current monotonic time.
     * @return False after any terminal failure or invalidation.
     */
    bool drive(short revents, clock::time_point now);
    /**
     * @brief Queue READY only after the caller has published a qualified frame.
     * @param capture_generation Nonzero capture generation for this connection.
     * @param area Full viewport matching WELCOME.
     * @param now Current monotonic time.
     * @return Whether READY was queued.
     */
    bool send_ready(std::uint64_t capture_generation, const viewport &area, clock::time_point now);
    /**
     * @brief Queue a strictly ordered event after matching ACTIVE.
     * @param value Validated input event.
     * @param now Current monotonic time.
     * @return False for invalid transition, protocol value or terminal queue exhaustion.
     */
    bool send_event(const event &value, clock::time_point now);
    /** @brief Retire this connection and clear local held state. */
    void close();

  private:
    /** @brief Fail closed with an actionable reason. */
    void fail(const std::string &reason);
    /** @brief Authenticate a completed socket connection. */
    bool authenticate();
    /** @brief Enqueue one complete frame. */
    bool queue(const std::vector<std::uint8_t> &bytes, clock::time_point now);
    /** @brief Process buffered complete server frames. */
    bool process(clock::time_point now);
    int fd_ = -1;  ///< Owned socket descriptor.
    uid_t expected_uid_;  ///< Required server UID.
    std::function<bool(std::uint16_t)> supported_key_;  ///< Shared key membership predicate.
    state state_ = state::closed;  ///< Current lifecycle state.
    std::string error_;  ///< Terminal error.
    peer_credentials peer_;  ///< Authenticated peer.
    welcome welcome_;  ///< Fresh cleanup acknowledgement.
    tokens active_;  ///< READY identity awaiting or after ACTIVE.
    std::vector<std::uint8_t> input_;  ///< Bounded inbound stream.
    std::vector<std::uint8_t> output_;  ///< Bounded outbound stream.
    std::set<std::uint16_t> keys_;  ///< Queued held keys.
    std::set<std::uint16_t> buttons_;  ///< Queued held buttons.
    clock::time_point handshake_deadline_;  ///< Connection through ACTIVE deadline.
    clock::time_point input_deadline_ {};  ///< Deadline for incomplete incoming frame.
    clock::time_point output_deadline_ {};  ///< Deadline for pending outgoing bytes.
  };
}  // namespace private_seat
