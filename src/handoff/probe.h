/**
 * @file src/handoff/probe.h
 * @brief Encoder probe data exchanged in `HELLO`: what the draining process reports and how the new process accepts it.
 *
 * See `docs/drain-handoff.md`, "3. New process start" (the `HELLO` identity rule) and "GPU" (the probe results feed
 * `/serverinfo` only). `protocol.h` owns the wire structs (`probe_results_t`, `probe_identity_t`); this header owns
 * the functions that fill them and decide whether they are used. Linux-only: it is included only by the handoff glue
 * (the old and new side), never by shared code, so it needs no `#ifdef`.
 *
 * Thread-safety: all functions are pure apart from the reads done through `probe_env_t`. `gather_probe()` reads
 * `video::active_hevc_mode`, `video::active_av1_mode` and `video::last_encoder_probe_supported_yuv444_for_codec`
 * without a lock, so it must run where no encoder probe runs at the same time (the old side's control thread, before
 * it answers `HELLO`; the new side's startup, before `HELLO` is sent). It never writes encoder state.
 */
#pragma once

// standard includes
#include <functional>
#include <optional>
#include <string>
#include <vector>

// local includes
#include "hooks.h"
#include "protocol.h"

namespace handoff {
  /**
   * @brief Encoder probe results together with the identity of the host that produced them.
   *
   * The old side sends this as `HELLO`'s `probe` and `identity`. The new side builds one for itself with
   * `gather_probe()` and only compares the identity; its `results` are not used.
   */
  struct probe_data_t {
    protocol::probe_results_t results;  ///< Active HEVC and AV1 modes and per-codec 4:4:4 support.
    protocol::probe_identity_t identity;  ///< Build, GPU and driver identity the results belong to.
  };

  /**
   * @brief Environment that `gather_probe()` reads. Production uses `system_probe_env()`; tests inject fakes.
   *
   * Every function is called on the thread that calls `gather_probe()`. None of them may block for long or take the
   * slots, clear or channel locks. A `std::nullopt` from a reader means "not readable" and is treated as an empty
   * value by `gather_probe()` (see its rules), never as an error.
   */
  struct probe_env_t {
    std::string build_id;  ///< Build identifier (`PROJECT_VERSION_COMMIT`).
    std::string configured_adapter;  ///< `config::video.adapter_name`; empty when not set.

    /**
     * @brief Read a small text file and strip trailing whitespace and newlines.
     *
     * Used for `/sys/class/drm/<node>/device/{vendor,device}` and `/sys/module/<driver>/version`.
     */
    std::function<std::optional<std::string>(const std::string &path)> read_text;

    /**
     * @brief Final path component of a symbolic link's target.
     *
     * Used for `/sys/class/drm/<node>/device/driver`, whose target ends in the driver name.
     */
    std::function<std::optional<std::string>(const std::string &path)> read_link_basename;

    /**
     * @brief Render node paths that exist, in ascending name order, for example `/dev/dri/renderD128`.
     *
     * Used only when `configured_adapter` is empty. The first entry is taken.
     */
    std::function<std::vector<std::string>()> list_render_nodes;

    /**
     * @brief Current encoder probe state from `video::`.
     *
     * Production reads `video::active_hevc_mode`, `video::active_av1_mode` and
     * `video::last_encoder_probe_supported_yuv444_for_codec`; the result maps to `results` one to one.
     */
    std::function<protocol::probe_results_t()> current_results;
  };

  /**
   * @brief The production environment: real files, `config::video.adapter_name`, `PROJECT_VERSION_COMMIT` and
   * `video::` state. Every reader is a thin wrapper over the POSIX or Sunshine call of the same meaning.
   *
   * @return Environment for `gather_probe()`.
   */
  probe_env_t system_probe_env();

  /**
   * @brief Build the probe results and identity of this host, for `HELLO`.
   *
   * Rules:
   * - `results` come from `env.current_results()`. A process that has not probed yet reports the globals' values (0 and
   *   `false`); the caller must not send such a `HELLO` before the probe has run (the old side always has).
   * - `identity.build_id` is `env.build_id` as given.
   * - `identity.render_node`: `env.configured_adapter` when it is not empty. Otherwise the first entry of
   *   `env.list_render_nodes()`. When neither exists, the empty string.
   * - `vendor_id` and `device_id`: the text of `/sys/class/drm/<basename of render_node>/device/vendor` and
   *   `.../device` (for example `0x10de`, with or without the `0x`) parsed as a hexadecimal 32-bit unsigned value.
   *   Unreadable, empty or unparsable text gives 0.
   * - `driver_version`: the basename of the link `/sys/class/drm/<node>/device/driver` gives the driver name `<drv>`;
   *   the content of `/sys/module/<drv>/version` is the version. When either is unreadable the version is the empty
   *   string (the driver name alone is not part of the identity).
   * - The function never fails; it does no logging. It never writes encoder state.
   *
   * @param env Environment to read (tests pass fakes).
   * @return Results and identity for `HELLO`.
   */
  probe_data_t gather_probe(const probe_env_t &env = system_probe_env());

  /**
   * @brief The probe results from a received `HELLO`, but only when they may be used for `/serverinfo`.
   *
   * Returns the `hello.probe` value when all of the following hold, otherwise `std::nullopt`:
   * - `hello.probe` and `hello.identity` are both present;
   * - `*hello.identity == own` (every identity field equal, including the driver version);
   * - `hevc_mode` and `av1_mode` are both within 0 to 5 (a value outside that range is rejected, as a defensive
   *   check).
   *
   * The accepted range is 0 to 5, not the 0 to 3 that the config options allow (`config.cpp`, `hevc_mode` and
   * `av1_mode` use `int_between_f` with `{0, 3}`). The encoder probe can raise the active mode to 4 or 5 (the 4:4:4
   * variants), so a probe result can be higher than the configured value.
   *
   * Mapping onto the override (`video.h`, `handoff_probe_override_t`), field for field:
   * - `results.hevc_mode` to `hevc_mode`;
   * - `results.av1_mode` to `av1_mode`;
   * - `results.yuv444_h264` to `yuv444[0]`, `results.yuv444_hevc` to `yuv444[1]`, `results.yuv444_av1` to `yuv444[2]`.
   *
   * A mismatch, a missing field or an out-of-range mode leaves `/serverinfo` on this process's own probe state; the
   * function does not log. The caller is the new side's `accept_old_hello`.
   *
   * @param hello Old process's `HELLO`.
   * @param own Identity of this host, from `gather_probe(env).identity`.
   * @return The results to publish with `video::set_handoff_probe_override` (converted with the mapping above), or
   *         `std::nullopt`. The startup probe skip is not decided here. `main.cpp` gates its startup
   *         `video::probe_encoders` on `handoff::hooks::startup_probe_skipped()` (added to `hooks.h` on the other
   *         contract branch): true only when the process was started by a handoff AND
   *         `video::handoff_probe_override()` holds a value, so a child whose `HELLO` was rejected still probes.
   */
  std::optional<protocol::probe_results_t> accepted_probe(
    const protocol::hello_t &hello,
    const protocol::probe_identity_t &own
  );
}  // namespace handoff
