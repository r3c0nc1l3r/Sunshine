/**
 * @file src/audio.h
 * @brief Declarations for audio capture and encoding.
 */
#pragma once

// local includes
#include "platform/common.h"
#include "thread_safe.h"
#include "utility.h"

#include <bitset>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace audio {
  /**
   * @brief Supported Opus channel layouts advertised to Moonlight clients.
   */
  enum stream_config_e : int {
    STEREO,  ///< Stereo
    HIGH_STEREO,  ///< High stereo
    SURROUND51,  ///< Surround 5.1
    HIGH_SURROUND51,  ///< High surround 5.1
    SURROUND71,  ///< Surround 7.1
    HIGH_SURROUND71,  ///< High surround 7.1
    MAX_STREAM_CONFIG  ///< Maximum audio stream configuration
  };

  /**
   * @brief Static Opus encoder layout for one advertised audio mode.
   */
  struct opus_stream_config_t {
    std::int32_t sampleRate;  ///< Opus sample rate in hertz.
    int channelCount;  ///< Number of audio channels in the Opus layout.
    int streams;  ///< Number of Opus streams in the layout.
    int coupledStreams;  ///< Number of stereo-coupled Opus streams.
    const std::uint8_t *mapping;  ///< Channel mapping table passed to the Opus encoder.
    int bitrate;  ///< Target bitrate in bits per second.
  };

  /**
   * @brief Custom Opus channel layout supplied by configuration.
   */
  struct stream_params_t {
    int channelCount;  ///< Number of audio channels in the Opus layout.
    int streams;  ///< Number of Opus streams in the custom layout.
    int coupledStreams;  ///< Number of stereo-coupled Opus streams.
    std::uint8_t mapping[8];  ///< Channel mapping table for up to eight speakers.
  };

  extern opus_stream_config_t stream_configs[MAX_STREAM_CONFIG];

  /**
   * @brief Audio capture and encoder settings for a stream.
   */
  struct config_t {
    /**
     * @brief Boolean audio feature flags.
     */
    enum flags_e : int {
      HIGH_QUALITY,  ///< High quality audio
      HOST_AUDIO,  ///< Host audio
      CUSTOM_SURROUND_PARAMS,  ///< Custom surround parameters
      CONTINUOUS_AUDIO,  ///< Continuous audio
      MAX_FLAGS  ///< Maximum number of flags
    };

    int packetDuration;  ///< Packet duration in milliseconds requested by the client.
    int channels;  ///< Number of audio channels requested by the client.
    int mask;  ///< Speaker mask describing the requested channel layout.

    stream_params_t customStreamParams;  ///< Custom Opus layout used when CUSTOM_SURROUND_PARAMS is enabled.

    std::bitset<MAX_FLAGS> flags;  ///< Enabled audio feature flags.
  };

  /**
   * @brief Shared audio device state used while streams are active.
   *
   * Handoff rules (see `docs/drain-handoff.md`, "Other resources", audio sinks): the virtual sinks are owned by the
   * active process. A draining process never restores the default sink when its sessions end (`stop_audio_control`
   * skips the restore) and never unloads the virtual sink modules when it exits (the PulseAudio `server_t` destructor
   * skips `unload_null`). Both checks read `handoff::hooks::draining()`; nothing else changes for a normal start.
   */
  struct audio_ctx_t {
    // We want to change the sink for the first stream only
    std::unique_ptr<std::atomic_bool> sink_flag;  ///< Tracks whether the capture sink was already switched.

    std::unique_ptr<platf::audio_control_t> control;  ///< Platform audio-control implementation.

    bool restore_sink;  ///< Whether Sunshine should restore the original sink when capture ends.
    platf::sink_t sink;  ///< Original sink captured before Sunshine switched devices.

    std::mutex sink_mutex;  ///< Guards `virtual_sink` and the default-sink switch that `reassert_virtual_sink` makes.
    // Written only by the first-session sink switch in audio.cpp capture() (the sink_flag exchange), under sink_mutex,
    // and only when that switch selects a virtual (null) sink.
    std::string virtual_sink;  ///< Virtual sink the first session switched to; empty until then and when none was used.
  };

  /**
   * @brief Byte buffer used for encoded audio packet payloads.
   */
  using buffer_t = util::buffer_t<std::uint8_t>;
  /**
   * @brief Encoded audio packet paired with platform channel metadata.
   */
  using packet_t = std::pair<void *, buffer_t>;
  /**
   * @brief Shared mailbox reference to the global audio context.
   */
  using audio_ctx_ref_t = safe::shared_t<audio_ctx_t>::ptr_t;

  /**
   * @brief Capture, encode, and publish audio packets for a stream.
   *
   * @param mail Mailbox used to publish encoded audio packets.
   * @param config Audio capture and encoder settings.
   * @param channel_data Platform-specific capture channel pointer.
   */
  void capture(safe::mail_t mail, config_t config, void *channel_data);

  /**
   * @brief Get the reference to the audio context.
   * @returns A shared pointer reference to audio context.
   * @note Aside from the configuration purposes, it can be used to extend the
   *       audio sink lifetime to capture sink earlier and restore it later.
   *
   * @examples
   * audio_ctx_ref_t audio = get_audio_ctx_ref()
   * @examples_end
   */
  audio_ctx_ref_t get_audio_ctx_ref();

  /**
   * @brief Check if the audio sink held by audio context is available.
   * @returns True if available (and can probably be restored), false otherwise.
   * @note Useful for delaying the release of audio context shared pointer (which
   *       tries to restore original sink).
   *
   * @examples
   * audio_ctx_ref_t audio = get_audio_ctx_ref()
   * if (audio.get()) {
   *     return is_audio_ctx_sink_available(*audio.get());
   * }
   * return false;
   * @examples_end
   * @param ctx Native context object used by the operation or callback.
   */
  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx);

  /**
   * @brief Make this process's own virtual sink the default sink again, when this process has sessions.
   *
   * Used after a rollback: the draining process had changed the default sink, and the process that keeps serving
   * must select its virtual sink again so its streams are heard there. Nothing else is changed: `sink_flag` and
   * `restore_sink` keep their values, so the restore that ends the sessions still runs as usual.
   *
   * Rules, in order:
   * - `sessions <= 0`: returns `false` and does nothing;
   * - no `control`, or `virtual_sink` is empty (no session has switched to a virtual sink): `false`, nothing done;
   * - otherwise calls `control->set_sink(virtual_sink)` under `sink_mutex`; returns `true` when it returns 0, `false`
   *   when it fails (logged by the backend; the caller treats it as best effort).
   *
   * Thread-safety: takes `sink_mutex` for the duration of the switch. It may be called from any thread except one that
   * holds `sink_mutex` already. It must not be called under the slots lock or the clear lock (it talks to the
   * PulseAudio mainloop and can wait).
   *
   * @param ctx Audio context to act on: the process-wide one, dereferenced from `get_audio_ctx_ref()`.
   * @param sessions This process's own session count (`rtsp_stream::session_count()`), not the total.
   * @return `true` when the default sink was switched back to the virtual sink; otherwise `false`.
   */
  bool reassert_virtual_sink(audio_ctx_t &ctx, int sessions);

#ifdef SUNSHINE_TESTS
  namespace testing {
    /**
     * @brief Replace the factory that `start_audio_control` calls to create the platform audio controller.
     *
     * Tests install a fake `platf::audio_control_t` that records `set_sink` calls. An empty function restores
     * `platf::audio_control()`. Not thread-safe: call it when no stream runs.
     *
     * @param factory Fake factory, or empty for the platform default.
     */
    void set_audio_control_factory_for_test(std::function<std::unique_ptr<platf::audio_control_t>()> factory);

    /**
     * @brief Run `stop_audio_control` on a context a test built, without the process-wide context.
     *
     * When `handoff::hooks::draining()` is false, a context with `restore_sink` true and a host sink set calls
     * `set_sink(host)` once. When it is true (the test puts `handoff::runtime()` into mode `draining` first), it calls
     * nothing.
     *
     * @param ctx Test-owned audio context.
     */
    void stop_audio_control_for_test(audio_ctx_t &ctx);

    /**
     * @brief Run `start_audio_control` on a context a test built.
     *
     * @param ctx Test-owned audio context.
     * @return `0` (the value `start_audio_control` returns; a missing controller is not an error here).
     */
    int start_audio_control_for_test(audio_ctx_t &ctx);
  }  // namespace testing
#endif
}  // namespace audio
