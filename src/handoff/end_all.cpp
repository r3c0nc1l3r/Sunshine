/**
 * @file src/handoff/end_all.cpp
 * @brief Definitions for `src/handoff/end_all.h`.
 */
// standard includes
#include <algorithm>
#include <thread>

// local includes
#include "end_all.h"

namespace handoff {
  end_all_answer_e end_all_machine_t::handle_end_all(const std::function<void()> &end_and_join) {
    std::unique_lock lock {_mutex};
    switch (_state) {
      case end_state_e::ended:
        return end_all_answer_e::ended;
      case end_state_e::rolling_back:
        return end_all_answer_e::rejected;
      case end_state_e::ending:
        {
          // Wait for the running call. A rollback decided meanwhile also wakes us, and then the answer is rejected.
          _cv.wait(lock, [this]() {
            return _state != end_state_e::ending;
          });
          return _state == end_state_e::ended ? end_all_answer_e::ended : end_all_answer_e::rejected;
        }
      case end_state_e::idle:
        break;
    }

    _state = end_state_e::ending;
    _ending_rejected = false;

    // Run without the mutex: ending and joining sessions can take long and a rollback must be able to decide.
    lock.unlock();
    try {
      end_and_join();
    } catch (...) {
      lock.lock();
      if (_state == end_state_e::ending) {
        // The sessions may not have ended: this is not an `ENDED`. Go back to idle so a later attempt can retry.
        _state = end_state_e::idle;
      }
      _cv.notify_all();
      throw;
    }
    lock.lock();

    end_all_answer_e answer;
    if (_state == end_state_e::ending) {
      _state = end_state_e::ended;
      answer = end_all_answer_e::ended;
    } else {
      // A rollback was decided meanwhile: the state stays `rolling_back`.
      _ending_rejected = true;
      answer = end_all_answer_e::rejected;
    }
    _cv.notify_all();
    return answer;
  }

  rollback_app_e end_all_machine_t::decide_rollback() {
    std::lock_guard lock {_mutex};
    switch (_state) {
      case end_state_e::idle:
        _state = end_state_e::rolling_back;
        _rollback_takes_back = true;
        return rollback_app_e::take_back;
      case end_state_e::ending:
        _state = end_state_e::rolling_back;
        _rollback_takes_back = false;
        _ending_rejected = true;
        _cv.notify_all();
        return rollback_app_e::drop;
      case end_state_e::ended:
        _state = end_state_e::rolling_back;
        _rollback_takes_back = false;
        return rollback_app_e::drop;
      case end_state_e::rolling_back:
        break;
    }
    return _rollback_takes_back ? rollback_app_e::take_back : rollback_app_e::drop;
  }

  end_state_e end_all_machine_t::state() const {
    std::lock_guard lock {_mutex};
    return _state;
  }

  void end_all_machine_t::reset() {
    std::lock_guard lock {_mutex};
    _state = end_state_e::idle;
    _rollback_takes_back = false;
    _ending_rejected = false;
    _cv.notify_all();
  }

  bool allows_terminate(terminate_permission_e permission) {
    return permission == terminate_permission_e::no_drain || permission == terminate_permission_e::ended ||
           permission == terminate_permission_e::peer_gone;
  }

  terminate_permission_e await_terminate_permission(drain_peer_t &peer, std::chrono::milliseconds budget) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + budget;

    if (!peer.exists()) {
      return terminate_permission_e::no_drain;
    }
    if (peer.gone()) {
      return terminate_permission_e::peer_gone;
    }

    switch (peer.end_all(budget)) {
      case drain_peer_t::reply_e::ended:
        return terminate_permission_e::ended;
      case drain_peer_t::reply_e::rejected:
        return peer.gone() ? terminate_permission_e::peer_gone : terminate_permission_e::refused;
      case drain_peer_t::reply_e::no_reply:
        break;
    }

    // No answer: the peer may still exit within the budget (for example both processes got SIGTERM). Poll its pidfd
    // until the budget is used up; never return `refused` early.
    constexpr auto poll_step = std::chrono::milliseconds {20};
    while (true) {
      if (peer.gone()) {
        return terminate_permission_e::peer_gone;
      }
      const auto now = clock::now();
      if (now >= deadline) {
        return terminate_permission_e::refused;
      }
      std::this_thread::sleep_for(std::min<clock::duration>(poll_step, deadline - now));
    }
  }
}  // namespace handoff
