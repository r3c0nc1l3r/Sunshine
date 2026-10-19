/**
 * @file src/handoff/ping_monitor.h
 * @brief The old process's `PING`/`PONG` miss rule for the rollback window.
 *
 * See `docs/drain-handoff.md`, step 6. A miss is a `PING` with no `PONG` of the same sequence number before the next
 * `PING` is due. Five misses in a row mean a hung new process. Any `PONG` for a sent sequence number, even late,
 * resets the miss count.
 */
#pragma once

// standard includes
#include <cstdint>

namespace handoff {
  /**
   * @brief Pure miss counter; the caller sends a `PING` every 2 s. Not thread-safe.
   */
  class ping_monitor_t {
  public:
    /**
     * @brief Create a monitor.
     *
     * @param misses_to_hang Consecutive misses that mean a hung process (default 5; values below 1 count as 1).
     */
    explicit ping_monitor_t(int misses_to_hang = 5);

    /**
     * @brief Record that the next `PING` is due and get its sequence number.
     *
     * If the previous `PING` has had no `PONG` with its sequence number, that counts as one miss first.
     *
     * @return Sequence number for the new `PING`; the first is 1, then 2, 3, ...
     */
    std::uint64_t next_ping();

    /**
     * @brief Record a received `PONG`.
     *
     * A sequence number of a `PING` that was sent (the latest or an earlier one) resets the miss count to 0 and, if it
     * is the latest, marks that `PING` answered. A sequence number that was never sent (0 or above the latest) is
     * ignored.
     *
     * @param seq Sequence number in the `PONG`.
     */
    void on_pong(std::uint64_t seq);

    /**
     * @brief Consecutive misses so far.
     *
     * @return Miss count.
     */
    int misses() const;

    /**
     * @brief Whether the miss count has reached the threshold.
     *
     * @return `true` when the new process counts as hung.
     */
    bool hung() const;

  private:
    int _misses_to_hang;  ///< Threshold.
    int _misses {0};  ///< Consecutive misses.
    std::uint64_t _last_sent {0};  ///< Latest sequence number sent, 0 before the first.
    bool _last_answered {true};  ///< Whether the latest `PING` got its `PONG`.
  };
}  // namespace handoff
