/**
 * @file src/handoff/gate.h
 * @brief The 503 gate and the in-flight route counters of the switch.
 *
 * See `docs/drain-handoff.md`, step 5, substeps 2 and 3. A counted handler first increments, then checks the gate;
 * the switcher first raises the gate, then waits for zero. All operations are sequentially consistent.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace handoff {
  /**
   * @brief Gate plus in-flight counter. Thread-safe.
   */
  class gate_t {
  public:
    /**
     * @brief RAII entry of one counted request.
     *
     * Constructed by `enter()`; when not admitted it has already decremented. Movable, not copyable; the destructor
     * of an admitted guard decrements once.
     */
    class guard_t {
    public:
      /**
       * @brief Empty, not admitted guard.
       */
      guard_t() = default;

      /**
       * @brief Move a guard; the source no longer decrements.
       *
       * @param other Source.
       */
      guard_t(guard_t &&other) noexcept;

      /**
       * @brief Move-assign; releases the current entry first.
       *
       * @param other Source.
       * @return This guard.
       */
      guard_t &operator=(guard_t &&other) noexcept;

      guard_t(const guard_t &) = delete;  ///< Not copyable.
      guard_t &operator=(const guard_t &) = delete;  ///< Not copyable.

      /**
       * @brief Decrement if admitted.
       */
      ~guard_t();

      /**
       * @brief Whether the request may proceed (the gate was down after the increment).
       *
       * @return `true` when admitted; `false` means answer 503 with `Connection: close`.
       */
      bool admitted() const;

    private:
      friend class gate_t;
      gate_t *_gate {nullptr};  ///< Owning gate while admitted.
    };

    /**
     * @brief Enter a counted route: increment, then check the gate; if raised, decrement and refuse.
     *
     * @return Guard; check `admitted()`.
     */
    guard_t enter();

    /**
     * @brief Whether a non-counted gated request (one that is not counted but still refused) must get 503.
     *
     * @return `true` while the gate is raised.
     */
    bool raised() const;

    /**
     * @brief Raise the gate.
     */
    void raise();

    /**
     * @brief Lower the gate.
     */
    void lower();

    /**
     * @brief Current number of admitted, unfinished requests.
     *
     * @return Count.
     */
    std::size_t in_flight() const;

    /**
     * @brief Wait until the counter is zero.
     *
     * @param timeout Longest wait; negative waits without limit.
     * @return `true` when zero was reached; `false` on timeout.
     */
    bool wait_idle(std::chrono::milliseconds timeout);

  private:
    /**
     * @brief Decrement and wake waiters at zero.
     */
    void leave();

    std::atomic<bool> _raised {false};  ///< Gate state.
    std::atomic<std::size_t> _in_flight {0};  ///< Admitted, unfinished requests.
    std::mutex _mutex;  ///< For `_cv`.
    std::condition_variable _cv;  ///< Signals zero.
  };

  /**
   * @brief The process-wide gate used by the HTTP, HTTPS and web UI handlers.
   *
   * @return The gate.
   */
  gate_t &route_gate();
}  // namespace handoff
