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
   * @brief Get the time left in a source's lockout.
   */
  std::optional<std::chrono::seconds> auth_rate_limiter_t::retry_after(std::string_view source, std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock(mutex);

    const auto it = entries.find(source);
    if (it == entries.end() || expired(it->second, now) || it->second.failures < max_failures) {
      return std::nullopt;
    }

    const auto remaining = std::chrono::ceil<std::chrono::seconds>(it->second.first_failure + window - now);
    return std::max(remaining, std::chrono::seconds(1));
  }

  /**
   * @brief Record one failed authentication for a source.
   */
  bool auth_rate_limiter_t::on_failure(std::string_view source, std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock(mutex);

    auto it = entries.find(source);
    if (it == entries.end()) {
      if (entries.size() >= max_sources) {
        make_room(now);
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

  /**
   * @brief Make room in a full table for a new source. The caller must hold `mutex`.
   */
  void auth_rate_limiter_t::make_room(std::chrono::steady_clock::time_point now) {
    const auto dropped = std::erase_if(entries, [&](const auto &item) {
      return expired(item.second, now);
    });
    if (dropped > 0) {
      return;
    }

    // Every remaining entry is live. Prefer the oldest entry that is not locked out.
    auto victim = entries.end();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
      if (it->second.failures >= max_failures) {
        continue;
      }
      if (victim == entries.end() || it->second.first_failure < victim->second.first_failure) {
        victim = it;
      }
    }

    if (victim == entries.end()) {
      victim = std::ranges::min_element(entries, {}, [](const auto &item) {
        return item.second.first_failure;
      });
    }
    entries.erase(victim);
  }
}  // namespace confighttp
