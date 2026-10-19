/**
 * @file src/handoff/gate.cpp
 * @brief Definitions for `src/handoff/gate.h`.
 */
// standard includes
#include <utility>

// local includes
#include "gate.h"

namespace handoff {
  gate_t::guard_t::guard_t(guard_t &&other) noexcept:
      _gate(std::exchange(other._gate, nullptr)) {
  }

  gate_t::guard_t &gate_t::guard_t::operator=(guard_t &&other) noexcept {
    if (this != &other) {
      if (_gate != nullptr) {
        _gate->leave();
      }
      _gate = std::exchange(other._gate, nullptr);
    }
    return *this;
  }

  gate_t::guard_t::~guard_t() {
    if (_gate != nullptr) {
      _gate->leave();
    }
  }

  bool gate_t::guard_t::admitted() const {
    return _gate != nullptr;
  }

  gate_t::guard_t gate_t::enter() {
    // Increment first, then check the gate (both sequentially consistent). The switcher raises the gate first and
    // then reads the counter, so at least one side sees the other.
    _in_flight.fetch_add(1, std::memory_order_seq_cst);
    guard_t guard;
    if (_raised.load(std::memory_order_seq_cst)) {
      leave();
      return guard;
    }
    guard._gate = this;
    return guard;
  }

  bool gate_t::raised() const {
    return _raised.load(std::memory_order_seq_cst);
  }

  void gate_t::raise() {
    _raised.store(true, std::memory_order_seq_cst);
  }

  void gate_t::lower() {
    _raised.store(false, std::memory_order_seq_cst);
  }

  std::size_t gate_t::in_flight() const {
    return _in_flight.load(std::memory_order_seq_cst);
  }

  bool gate_t::wait_idle(std::chrono::milliseconds timeout) {
    std::unique_lock lock {_mutex};
    // The predicate is evaluated under the mutex, and `leave()` notifies under the mutex, so a decrement to zero
    // cannot slip between the check and the wait.
    auto idle = [this]() {
      return _in_flight.load(std::memory_order_seq_cst) == 0;
    };
    if (timeout.count() < 0) {
      _cv.wait(lock, idle);
      return true;
    }
    return _cv.wait_for(lock, timeout, idle);
  }

  void gate_t::leave() {
    if (_in_flight.fetch_sub(1, std::memory_order_seq_cst) == 1) {
      std::lock_guard lock {_mutex};
      _cv.notify_all();
    }
  }

  gate_t &route_gate() {
    static gate_t gate;
    return gate;
  }
}  // namespace handoff
