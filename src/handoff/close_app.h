/**
 * @file src/handoff/close_app.h
 * @brief The `close-app` verb of the control socket: close the running app like `/api/apps/close`.
 *
 * See `docs/drain-handoff.md`, "The control socket".
 */
#pragma once

// standard includes
#include <chrono>
#include <functional>
#include <string_view>
#include <sys/types.h>

// local includes
#include "control.h"
#include "end_all.h"

namespace handoff {
  /**
   * @brief What `close_app()` calls; empty members use the real functions. Tests replace them.
   */
  struct close_app_ops_t {
    std::function<int()> running;  ///< The running app's id, 0 when none. Default: `proc::proc.running()`.
    std::function<terminate_permission_e(std::chrono::milliseconds budget, std::string_view action)>
      terminate;  ///< Ask and terminate. Default: `terminate_app(budget, action)` (`terminate.h`).
    std::chrono::milliseconds budget {0};  ///< Budget for `terminate`; 0 means `request_budget()`.
  };

  /**
   * @brief Handle one `close-app` request.
   *
   * Logs the Info line `Control: close app requested by pid <peer_pid>`. Reads `running()` once: an app is running when
   * it is above 0. Then calls `terminate(budget, "close app")` once in either case (the same call as `/api/apps/close`
   * on Linux, so with no app it is a no-op as there). When `allows_terminate()` holds for the result, it logs
   * `Control: close app done (app)` or `Control: close app done (none)` and returns `closed` with `closed_app` = whether
   * an app was running. Otherwise it logs the Warning `Control: close app refused: previous process still holds the
   * app` and returns `refused previous process still holds the app`. An exception from `running` or `terminate` is not
   * caught here (the control server answers `refused internal error`). No app or process names are logged.
   *
   * @param peer_pid Peer pid from `SO_PEERCRED` (for the log only).
   * @param ops Operations; default the real ones.
   * @return The reply to write.
   */
  control_reply_t close_app(pid_t peer_pid, const close_app_ops_t &ops = {});
}  // namespace handoff
