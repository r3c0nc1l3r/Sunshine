/**
 * @file src/handoff/close_app.cpp
 * @brief Definitions for `src/handoff/close_app.h`.
 */
// local includes
#include "close_app.h"

#include "src/logging.h"
#include "src/process.h"
#include "terminate.h"

namespace handoff {
  control_reply_t close_app(pid_t peer_pid, const close_app_ops_t &ops) {
    BOOST_LOG(info) << "Control: close app requested by pid " << peer_pid;

    const bool app_running = (ops.running ? ops.running() : proc::proc.running()) > 0;
    const auto budget = ops.budget.count() > 0 ? ops.budget : request_budget();
    const auto permission = ops.terminate ?
                              ops.terminate(budget, "close app") :
                              terminate_app(budget, "close app");

    if (!allows_terminate(permission)) {
      BOOST_LOG(warning) << "Control: close app refused: previous process still holds the app";
      return control_reply_t {reply_kind_e::refused, "previous process still holds the app"};
    }

    BOOST_LOG(info) << "Control: close app done (" << (app_running ? "app" : "none") << ")";
    control_reply_t reply;
    reply.kind = reply_kind_e::closed;
    reply.closed_app = app_running;
    return reply;
  }
}  // namespace handoff
