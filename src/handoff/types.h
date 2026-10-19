/**
 * @file src/handoff/types.h
 * @brief Small types shared by the handoff runtime and the platform-neutral hooks.
 */
#pragma once

namespace handoff {
  /**
   * @brief Why the process is shutting down, as far as session stop reasons are concerned.
   *
   * The shared code maps it to `stream::stop_reason_e`: `terminated` to `terminated`, `stop` to `stop`.
   */
  enum class shutdown_reason_e {
    terminated,  ///< An ordinary shutdown: the sessions end with reason `terminated`.
    stop,  ///< A draining process was stopped: the sessions end with reason `stop`.
  };
}  // namespace handoff
