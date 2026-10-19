/**
 * @file src/handoff/terminate.cpp
 * @brief Implementation of `src/handoff/terminate.h` (wave C).
 */
#include "terminate.h"

// standard includes
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

// local includes
#include "src/logging.h"
#include "src/process.h"

namespace handoff {
  using namespace std::literals;

  namespace {
    std::mutex terminate_mutex;  ///< Serialises request-path terminates; held across the whole `proc::proc.terminate()`.

#ifdef SUNSHINE_TESTS
    std::mutex test_hook_mutex;  ///< Guards `test_terminate`.
    std::function<void()> test_terminate;  ///< Replacement for `proc::proc.terminate()`, or empty.
    std::atomic<long long> test_request_budget_ms {0};  ///< Override of `request_budget()` in ms; 0 means none.
#endif

    /**
     * @brief The peer to ask, or null when this process must not ask.
     *
     * @return The peer; null when none exists or the mode is `new_pending` or `draining`.
     */
    std::shared_ptr<drain_peer_t> active_peer() {
      const auto mode = runtime().mode();
      if (mode == mode_e::new_pending || mode == mode_e::draining) {
        return nullptr;
      }
      return runtime().peer();
    }
  }  // namespace

  std::chrono::milliseconds request_budget() {
#ifdef SUNSHINE_TESTS
    const auto override_ms = test_request_budget_ms.load();
    if (override_ms > 0) {
      return std::chrono::milliseconds {override_ms};
    }
#endif
    return request_terminate_budget;
  }

  terminate_permission_e terminate_gate(std::chrono::milliseconds budget) {
    const auto peer = active_peer();
    if (!peer) {
      return terminate_permission_e::no_drain;
    }
    return await_terminate_permission(*peer, budget);
  }

  void terminate_app_permitted() {
    std::function<void()> replacement;
#ifdef SUNSHINE_TESTS
    {
      std::lock_guard hook_lock {test_hook_mutex};
      replacement = test_terminate;
    }
#endif

    std::lock_guard lock {terminate_mutex};
    if (replacement) {
      replacement();
    } else {
      proc::proc.terminate();
    }
  }

  terminate_permission_e terminate_app(std::chrono::milliseconds budget, std::string_view action, bool at_shutdown) {
    const auto permission = terminate_gate(budget);
    if (allows_terminate(permission)) {
      terminate_app_permitted();
      return permission;
    }

    // A shutdown never leaves behind an app this process started; an adopted app stays with the other process.
    if (at_shutdown && !proc::proc.adopted()) {
      BOOST_LOG(warning) << "The previous process did not release the app; terminating it anyway because this process launched it"sv;
      terminate_app_permitted();
    } else {
      BOOST_LOG(warning) << "Not doing "sv << action << ": the previous process did not release the app"sv;
    }
    return permission;
  }

  std::optional<std::size_t> draining_session_count(std::chrono::milliseconds timeout) {
    const auto peer = active_peer();
    if (!peer || peer->gone()) {
      return std::size_t {0};
    }

    const auto session_peer = std::dynamic_pointer_cast<session_peer_t>(peer);
    if (!session_peer) {
      return std::nullopt;
    }
    const auto sessions = session_peer->list(timeout);
    if (!sessions) {
      return std::nullopt;
    }
    return sessions->size();
  }

  std::string launch_refusal() {
    const auto count = draining_session_count();
    if (count && *count == 0) {
      return {};
    }
    return "The previous process still has streaming sessions"s;
  }

#ifdef SUNSHINE_TESTS
  void set_terminate_for_test(std::function<void()> terminate) {
    std::lock_guard hook_lock {test_hook_mutex};
    test_terminate = std::move(terminate);
  }

  void set_request_budget_for_test(std::chrono::milliseconds budget) {
    test_request_budget_ms.store(budget.count() > 0 ? budget.count() : 0);
  }
#endif
}  // namespace handoff
