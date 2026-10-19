/**
 * @file src/handoff/types.h
 * @brief Small types shared by the handoff runtime and the platform-neutral hooks.
 */
#pragma once

namespace handoff {
  /**
   * @brief Why the process is shutting down, as far as session stop reasons are concerned.
   *
   * The shared code maps it to `stream::stop_reason_e` with `rtsp_stream::to_stop_reason`: `terminated` to
   * `terminated`, `stop` to `stop`. `drain_deadline` is not a shutdown reason: the deadline timer passes it directly.
   */
  enum class shutdown_reason_e {
    terminated,  ///< An ordinary shutdown: the sessions end with reason `terminated`.
    stop,  ///< A draining process was stopped: the sessions end with reason `stop`.
  };
}  // namespace handoff
