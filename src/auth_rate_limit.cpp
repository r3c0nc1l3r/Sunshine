/**
 * @file src/auth_rate_limit.cpp
 * @brief Definitions for the failed-login rate limiter used by the Web UI.
 */

// standard includes
#include <algorithm>
#include <mutex>

// local includes
#include "auth_rate_limit.h"

namespace confighttp {

  /**
   * @brief Construct a limiter.
   */
  auth_rate_limiter_t::auth_rate_limiter_t(std::size_t max_failures, std::chrono::seconds window, std::size_t max_sources):
      max_failures(max_failures),
      window(window),
      max_sources(std::max<std::size_t>(max_sources, 1)) {}

  /**
   * @brief Check whether a source is currently locked out.
   */
  bool auth_rate_limiter_t::blocked(std::string_view source, std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock(mutex);

    const auto it = entries.find(source);
    if (it == entries.end()) {
      return false;
    }

    if (expired(it->second, now)) {
      entries.erase(it);
      return false;
    }

    return it->second.failures >= max_failures;
  }

  /**
   * @brief Record one failed authentication for a source.
   */
  bool auth_rate_limiter_t::on_failure(std::string_view source, std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock(mutex);

    auto it = entries.find(source);
    if (it == entries.end()) {
      if (entries.size() >= max_sources) {
        const auto oldest = std::ranges::min_element(entries, {}, [](const auto &entry) {
          return entry.second.first_failure;
        });
        entries.erase(oldest);
      }
      it = entries.emplace(std::string(source), entry_t {now, 0}).first;
    } else if (expired(it->second, now)) {
      it->second = entry_t {now, 0};
    }

    ++it->second.failures;
    return it->second.failures == max_failures;
  }

  /**
   * @brief Forget every tracked source.
   */
  void auth_rate_limiter_t::clear() {
    std::scoped_lock lock(mutex);
    entries.clear();
  }

  /**
   * @brief Count the tracked sources.
   */
  std::size_t auth_rate_limiter_t::size() const {
    std::scoped_lock lock(mutex);
    return entries.size();
  }

  /**
   * @brief Check whether an entry's window has passed at `now`.
   */
  bool auth_rate_limiter_t::expired(const entry_t &entry, std::chrono::steady_clock::time_point now) const {
    return now - entry.first_failure >= window;
  }
}  // namespace confighttp
