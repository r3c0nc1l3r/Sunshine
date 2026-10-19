/**
 * @file src/video_retry_backoff.h
 * @brief Declarations for the backoff between failed encode runs.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>

namespace video {
  /**
   * @brief Exponential backoff for an encode loop that restarts after a failed run.
   *
   * A run that failed quickly (shorter than `healthy_run`) increases the delay: `initial`, `2 * initial`, ...
   * capped at `max`. A successful run, or a run that lasted at least `healthy_run`, resets it.
   */
  class retry_backoff_t {
  public:
    /**
     * @brief Create a backoff.
     *
     * @param initial Delay after the first quick failure.
     * @param max Maximum delay.
     * @param healthy_run Run duration from which a failure no longer counts as quick.
     */
    retry_backoff_t(std::chrono::milliseconds initial, std::chrono::milliseconds max, std::chrono::milliseconds healthy_run);

    /**
     * @brief Record the end of a run and get the delay before the next one.
     *
     * @param run_duration How long the run lasted.
     * @param failed Whether the run ended with an error.
     * @return Delay to wait before the next run; zero after a success or a healthy run.
     */
    std::chrono::milliseconds on_run_end(std::chrono::milliseconds run_duration, bool failed);

    /**
     * @brief Number of consecutive quick failures.
     *
     * @return Count since the last reset.
     */
    std::uint32_t consecutive_failures() const;

    /**
     * @brief Forget earlier failures.
     */
    void reset();

  private:
    std::chrono::milliseconds _initial;  ///< Delay after the first quick failure.
    std::chrono::milliseconds _max;  ///< Maximum delay.
    std::chrono::milliseconds _healthy_run;  ///< Run duration from which a failure is not quick.
    std::uint32_t _failures {0};  ///< Consecutive quick failures.
  };
}  // namespace video
