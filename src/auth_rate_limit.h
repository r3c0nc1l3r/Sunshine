/**
 * @file src/auth_rate_limit.h
 * @brief Declarations for the failed-login rate limiter used by the Web UI.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace confighttp {

  /**
   * @brief Tracks failed authentication attempts per source address and locks out sources that fail too often.
   *
   * A source's failures are counted from its first failure in the current window. The failure that reaches
   * `max_failures` starts a lockout that lasts until `first_failure + window`. Once the window has passed, the
   * source's counter starts again. The table holds at most `max_sources` sources; when it is full, the source
   * with the oldest first failure is evicted. All members are thread-safe. The class is pure logic: it never
   * reads the clock, so callers pass the time point.
   */
  class auth_rate_limiter_t {
  public:
    /**
     * @brief Construct a limiter.
     *
     * @param max_failures Failures inside one window that start a lockout.
     * @param window Length of the failure window and of the lockout.
     * @param max_sources Maximum number of sources tracked at once (at least 1).
     */
    explicit auth_rate_limiter_t(std::size_t max_failures = 10, std::chrono::seconds window = std::chrono::minutes(5), std::size_t max_sources = 1024);

    /**
     * @brief Check whether a source is currently locked out.
     *
     * @param source Normalized source address.
     * @param now Current time point.
     * @return True while `source` is locked out at `now`.
     */
    bool blocked(std::string_view source, std::chrono::steady_clock::time_point now);

    /**
     * @brief Record one failed authentication for a source.
     *
     * @param source Normalized source address.
     * @param now Current time point.
     * @return True exactly when this failure starts a lockout for `source`.
     */
    bool on_failure(std::string_view source, std::chrono::steady_clock::time_point now);

    /**
     * @brief Forget every tracked source.
     */
    void clear();

    /**
     * @brief Count the tracked sources.
     *
     * @return Number of sources currently in the table, including ones whose window has expired but were not yet pruned.
     */
    std::size_t size() const;

  private:
    /**
     * @brief Failure state for one source.
     */
    struct entry_t {
      std::chrono::steady_clock::time_point first_failure;  ///< Time of the first failure in the current window.
      std::size_t failures;  ///< Failures recorded in the current window.
    };

    /**
     * @brief Check whether an entry's window has passed at `now`.
     *
     * @param entry Entry to check.
     * @param now Current time point.
     * @return True when `now` is at or after `first_failure + window`.
     */
    bool expired(const entry_t &entry, std::chrono::steady_clock::time_point now) const;

    const std::size_t max_failures;  ///< Failures inside one window that start a lockout.
    const std::chrono::seconds window;  ///< Length of the failure window and of the lockout.
    const std::size_t max_sources;  ///< Maximum number of sources tracked at once.

    mutable std::mutex mutex;  ///< Protects `entries`.
    std::map<std::string, entry_t, std::less<>> entries;  ///< Failure state keyed by source address.
  };
}  // namespace confighttp
