/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "platform/common.h"
#include "thread_safe.h"

namespace input {
  struct input_t;

  /**
   * @brief Stable identifiers of the virtual input devices, with the handoff generation folded in.
   *
   * The two processes of a drain handoff must never create a device with the same stable id, so every id carries the
   * generation (`hooks::generation()`, which is 0 for a process that was never started by a handoff). Generation 0 gives
   * exactly the upstream strings, so a normal start is unchanged. These are pure functions: they touch no device and no
   * state, are safe from any thread, and are the single place where the format is defined. The platform backend
   * (`src/platform/virtualhid_input.cpp`) calls them.
   *
   * Formats (the generation is written in decimal in the id and in the MAC; `g` is a literal letter):
   * - keyboard: `sunshine-keyboard` at generation 0, `sunshine-keyboard-g<gen>` above;
   * - mouse: `sunshine-mouse` at 0, `sunshine-mouse-g<gen>` above;
   * - touchscreen: `sunshine-touchscreen` at 0, `sunshine-touchscreen-g<gen>` above;
   * - pen tablet: `sunshine-pen-tablet` at 0, `sunshine-pen-tablet-g<gen>` above;
   * - gamepad (non-MAC profiles): `sunshine-gamepad-<idx>` at 0, `sunshine-gamepad-<gen>-<idx>` above;
   * - gamepad MAC (DualShock 4 and DualSense profiles): `02:00:00:00:00:<idx>` (two-digit lowercase hex) at 0, and
   *   `02:00:<gen_hi>:<gen_lo>:00:<idx>` above, where `gen_hi` and `gen_lo` are the two bytes of the generation (both
   *   two-digit lowercase hex). The two bytes keep the MAC unique for every generation up to 65535.
   */
  namespace device_ids {
    /**
     * @brief Stable id of the virtual keyboard.
     *
     * @param generation Handoff generation; generation 0 gives the upstream id.
     * @return The id, in the format above.
     */
    std::string keyboard(unsigned generation);

    /**
     * @brief Stable id of the virtual mouse.
     *
     * @param generation Handoff generation; generation 0 gives the upstream id.
     * @return The id, in the format above.
     */
    std::string mouse(unsigned generation);

    /**
     * @brief Stable id of the virtual touchscreen (per client).
     *
     * @param generation Handoff generation; generation 0 gives the upstream id.
     * @return The id, in the format above.
     */
    std::string touchscreen(unsigned generation);

    /**
     * @brief Stable id of the virtual pen tablet (per client).
     *
     * @param generation Handoff generation; generation 0 gives the upstream id.
     * @return The id, in the format above.
     */
    std::string pen_tablet(unsigned generation);

    /**
     * @brief Stable id of a virtual gamepad whose profile is not a MAC-addressed one.
     *
     * @param generation Handoff generation.
     * @param global_index Global gamepad slot (`gamepad_id_t::globalIndex`); the caller passes it as given.
     * @return The id, in the format above. For generation 0 and index 3 this is `sunshine-gamepad-3`.
     */
    std::string gamepad(unsigned generation, int global_index);

    /**
     * @brief MAC address of a virtual gamepad whose profile is MAC-addressed (DualShock 4 and DualSense).
     *
     * The caller keeps its own rules: a random MAC (the `virtualhid_randomize_mac` option) or a slot outside 0 to 255
     * is handled before this function is called. This function formats only.
     *
     * @param generation Handoff generation. Only its low 16 bits are used.
     * @param global_index Global gamepad slot, 0 to 255; the low byte is written.
     * @return The MAC string, in the format above.
     */
    std::string gamepad_mac(unsigned generation, int global_index);
  }  // namespace device_ids

  /**
   * @brief Write a debug log representation of the input packet.
   *
   * @param input Raw input packet to format for logging.
   */
  void print(void *input);
  /**
   * @brief Reset stream input state after a client disconnect or shutdown.
   *
   * @param input Shared stream input state to reset.
   */
  void reset(std::shared_ptr<input_t> &input);

  /**
   * @brief Destroy every retained virtual gamepad session.
   *
   * Retained gamepads survive a paused transport connection so they can be reused on resume. Call this when the
   * streamed application or all streaming sessions are explicitly terminated.
   */
  void terminate_gamepads();

  /**
   * @brief Destroy virtual gamepads retained for one paired client.
   *
   * @param session_id Stable paired-client identity used by alloc().
   */
  void terminate_gamepads(std::string_view session_id);

  /**
   * @brief Queue a raw input message for platform passthrough.
   */
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data);

  /**
   * @brief Initialize global input resources and platform backends.
   *
   * Normal start (`hooks::started_by_handoff()` is false): creates the platform input backend now, exactly as before
   * (through `ensure_platform_input()`), so the virtual devices exist from startup.
   *
   * Handoff start: creates nothing. The platform backend, and with it the keyboard and mouse devices, is created by
   * the first `ensure_platform_input()` call, which `alloc()` makes. The returned guard still destroys the backend at
   * process exit (a draining process therefore destroys its devices before it exits, as the spec requires).
   *
   * Thread-safety: call once, from `main()`, before any stream starts. Probing gamepads never needs the backend (see
   * `platf::supported_gamepads`, which accepts a null input).
   *
   * @return Cleanup handle for initialized input resources; never null (the guard is also returned in handoff mode).
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Create the platform input backend if it does not exist yet, exactly once for the process.
   *
   * Guarded by a `std::call_once`: the platform factory (`platf::input()`, or the test factory) runs at most once,
   * even when several threads call this at the same time. The first caller creates the backend and every later call
   * returns at once.
   *
   * Lock order: `alloc()` calls this before it takes the retained-input lock, because creating the backend can take a
   * long time (the libvirtualhid runtime). This function never takes the retained-input lock, the slots lock or the
   * clear lock. Callers must not hold any of those when they call it.
   *
   * After the backend exists, a later `init()` or `ensure_platform_input()` call does nothing. The deinit guard of
   * `init()` resets the backend at exit and does not reset the once-flag, so the backend is not recreated after exit.
   */
  void ensure_platform_input();

  /**
   * @brief Probe whether the platform can create virtual gamepads.
   *
   * @return True when at least one configured gamepad backend is available.
   */
  bool probe_gamepads();

  /**
   * @brief Recreate shared libvirtualhid keyboard and mouse devices after a license-state change.
   *
   * The work is serialized with streamed input so both backends can switch
   * safely between the Windows HID and SendInput paths.
   */
  void refresh_virtual_input();

  /**
   * @brief Allocate and initialize platform input state for a stream.
   *
   * @param mail Mailbox used to exchange messages with worker threads.
   * @param session_id Stable paired-client identity shared by launch and resume connections.
   * @return Shared input state bound to the stream mailbox.
   */
  std::shared_ptr<input_t> alloc(safe::mail_t mail, std::string session_id);

#ifdef SUNSHINE_TESTS
  namespace testing {
    /**
     * @brief Replace the global platform input backend for a unit test.
     *
     * Counts as the once-created backend: afterwards `ensure_platform_input()` does not call the factory.
     *
     * @param input Test-owned platform input backend.
     */
    void set_platform_input(platf::input_t input);

    /**
     * @brief Replace the factory that `ensure_platform_input()` calls, and rearm its once-flag.
     *
     * A test passes a fake whose call count it can read. Installing a factory drops any existing backend and makes the
     * next `ensure_platform_input()` call the first one again. Passing an empty function restores `platf::input()`.
     * Not thread-safe: call it when no other thread uses input.
     *
     * @param factory Fake factory, or empty for the platform default.
     */
    void set_platform_input_factory_for_test(std::function<platf::input_t()> factory);

    /**
     * @brief Whether the platform input backend has been created (by the factory, or by `set_platform_input`).
     *
     * @return `true` when `ensure_platform_input()` or `set_platform_input()` has produced a backend and it is not
     *         reset.
     */
    bool platform_input_created_for_test();

    /**
     * @brief Allocate a gamepad directly in retained input state for a unit test.
     *
     * @param input Retained input state.
     * @param client_index Client-relative controller index.
     * @param metadata Client-reported controller metadata.
     * @return Assigned global gamepad slot, or -1 on failure.
     */
    int alloc_gamepad(std::shared_ptr<input_t> &input, std::uint8_t client_index, const platf::gamepad_arrival_t &metadata);

    /**
     * @brief Return the global gamepad slot stored for a test controller.
     *
     * @param input Retained input state.
     * @param client_index Client-relative controller index.
     * @return Assigned global gamepad slot, or -1 when unallocated.
     */
    int gamepad_id(const std::shared_ptr<input_t> &input, std::uint8_t client_index);

    /**
     * @brief Keyboard event Sunshine emitted toward the platform backend.
     */
    struct keyboard_event_t {
      std::uint16_t key_code;  ///< Platform keycode after the configured keybinding remap.
      bool release;  ///< Whether the event releases the key.
      std::uint8_t flags;  ///< Bit flags carried by the client keyboard packet.
    };

    /**
     * @brief Redirect keyboard output away from the host operating system.
     *
     * Tests must install a sink before emitting keys, otherwise the events are typed into the
     * machine running the test suite.
     *
     * @param sink Recorder invoked in place of platf::keyboard_update, or empty to restore
     *             delivery to the platform backend.
     */
    void set_keyboard_sink(std::function<void(const keyboard_event_t &)> sink);

    /**
     * @brief Process one client keyboard packet on the calling thread.
     *
     * @param input Retained input state.
     * @param key_code Windows virtual-key code sent by the client.
     * @param modifiers Client modifier bitmask carried by the packet.
     * @param flags Bit flags carried by the client keyboard packet.
     * @param release Whether the packet releases the key.
     */
    void send_keyboard_packet(std::shared_ptr<input_t> &input, std::uint16_t key_code, std::uint8_t modifiers, std::uint8_t flags, bool release);

    /**
     * @brief Forget every key Sunshine tracks as pressed and cancel any pending key repeat.
     */
    void reset_keyboard_state();

    /**
     * @brief Release every key Sunshine tracks as pressed, as a disconnect does.
     */
    void release_held_keys();

    /**
     * @brief Validate raw protocol input bytes for a unit test.
     *
     * @param packet Raw packet bytes.
     * @return True when the packet is safe for typed processing.
     */
    bool is_valid_input_packet(std::span<const std::uint8_t> packet);

    /**
     * @brief Return the number of validated packets waiting in a test input queue.
     *
     * @param input Shared stream input state.
     * @return Number of queued packets, or zero for an empty input pointer.
     */
    std::size_t queued_input_packet_count(const std::shared_ptr<input_t> &input);
  }  // namespace testing
#endif

  /**
   * @brief Touchscreen coordinate bounds used to scale absolute input.
   */
  struct touch_port_t: public platf::touch_port_t {
    int env_width;  ///< Width of the full capture environment in physical pixels.
    int env_height;  ///< Height of the full capture environment in physical pixels.

    // Offset x and y coordinates of the client
    float client_offsetX;  ///< Horizontal client viewport offset used when scaling touch input.
    float client_offsetY;  ///< Vertical client viewport offset used when scaling touch input.

    float scalar_inv;  ///< Inverse scale factor from client coordinates to display coordinates.
    float scalar_tpcoords;  ///< Scale factor from client coordinates to touch-port coordinates.

    int env_logical_width;  ///< Width of the full capture environment after display scaling.
    int env_logical_height;  ///< Height of the full capture environment after display scaling.

    /**
     * @brief Check whether the touch-port bounds are initialized.
     */
    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);
}  // namespace input
