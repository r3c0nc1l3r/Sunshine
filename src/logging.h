/**
 * @file src/logging.h
 * @brief Declarations for logging related functions.
 */
#pragma once

// standard includes
#include <cstddef>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>

// lib includes
#include <boost/log/common.hpp>
#include <boost/log/sinks.hpp>

/**
 * @brief Boost.Log asynchronous text sink used by Sunshine logging.
 */
using text_sink = boost::log::sinks::asynchronous_sink<boost::log::sinks::text_ostream_backend>;

extern boost::log::sources::severity_logger<int> verbose;  ///< Verbose.
extern boost::log::sources::severity_logger<int> debug;  ///< Debug.
extern boost::log::sources::severity_logger<int> info;  ///< Info.
extern boost::log::sources::severity_logger<int> warning;  ///< Warning.
extern boost::log::sources::severity_logger<int> error;  ///< Error.
extern boost::log::sources::severity_logger<int> fatal;  ///< Fatal.
#ifdef SUNSHINE_TESTS
extern boost::log::sources::severity_logger<int> tests;
#endif

#include "config.h"
#include "stat_trackers.h"

/**
 * @brief Handles the initialization and deinitialization of the logging system.
 */
namespace logging {
  /**
   * @brief The number of previous log files retained during rotation.
   */
  inline constexpr std::size_t retained_log_file_count {5};

  /**
   * @brief Rotate a log file while retaining up to five previous logs.
   *
   * The current log is renamed with a `.1` suffix, existing rotated logs are
   * advanced by one generation, and the previous `.5` log is removed.
   *
   * @param log_file Path to the current log file.
   * @return An error code when rotation fails, or a clear error code on success.
   */
  inline std::error_code rotate_log_file(const std::filesystem::path &log_file) noexcept {
    const auto rotated_log_path = [&log_file](std::size_t generation) {
      auto rotated_path = log_file;
      rotated_path += std::format(".{}", generation);
      return rotated_path;
    };

    try {
      std::filesystem::remove(rotated_log_path(retained_log_file_count));

      for (auto generation = retained_log_file_count; generation > 1; --generation) {
        const auto previous_path = rotated_log_path(generation - 1);
        if (std::filesystem::exists(previous_path)) {
          std::filesystem::rename(previous_path, rotated_log_path(generation));
        }
      }

      if (std::filesystem::exists(log_file)) {
        std::filesystem::rename(log_file, rotated_log_path(1));
      }

      return {};
    } catch (const std::filesystem::filesystem_error &filesystem_error) {
      return filesystem_error.code();
    }
  }

  /**
   * @brief RAII helper that runs shutdown cleanup when destroyed.
   */
  class deinit_t {
  public:
    /**
     * @brief Restores logging state when the logging subsystem shuts down.
     */
    ~deinit_t();
  };

  /**
   * @brief Deinitialize the logging system.
   * @examples
   * deinit();
   * @examples_end
   */
  void deinit();

  /**
   * @brief Format a Boost.Log record for Sunshine log output.
   *
   * The line is `[<time>]: <Severity>: [<tag>] <message>` when a process tag is set (see @ref set_process_tag), for
   * example `[2026-01-01 12:00:00.000]: Info: [drain] Session ended`, and `[<time>]: <Severity>: <message>` when it
   * is not. The tag is read once per record.
   *
   * @param view Boost.Log record view being formatted.
   * @param os Boost.Log output stream receiving formatted text.
   */
  void formatter(const boost::log::record_view &view, boost::log::formatting_ostream &os);

  /**
   * @brief Set the process tag that every later log line carries right after the severity (`[drain] ` for a draining
   * handoff process).
   *
   * Thread-safe and callable from any thread at any time, also while other threads log; a record is formatted with
   * either the old or the new tag, never a torn one. The tag is stored as given, without the brackets (the formatter
   * adds `[` and `]` and one trailing space). Nothing is logged by this call.
   *
   * @param tag New tag, for example `drain`. An empty string clears the tag.
   */
  void set_process_tag(std::string tag);

  /**
   * @brief The current process tag.
   *
   * @return The tag without brackets; empty when none is set.
   */
  std::string process_tag();

  /**
   * @brief Rotate the current log file (unless told not to) and initialize the logging system.
   *
   * The log file is always opened for appending (`std::ios::app`, so `O_APPEND`), in every mode, so two processes that
   * write the same file (a draining process and its successor) never overwrite each other's data. The sink flushes
   * after every record (`auto_flush(true)`), so each record normally reaches the file in one `write`; records can
   * interleave between the processes, but only at record granularity, except that a record larger than the stream
   * buffer may be split into several writes and then be interleaved inside. A normal start
   * rotates first (`rotate_log_file` renames the old file away), so it appends to a fresh, empty file. If rotation
   * fails, a message is written to `stderr`, initialization continues, and the file is opened for appending, so the
   * old content is kept (earlier versions would have truncated it).
   *
   * @param min_log_level The minimum log level to output.
   * @param log_file The log file to write to.
   * @param append_no_rotate When `true` (a drain-handoff child process), rotation is skipped, so the running
   *        process's log is not renamed away. It means only "do not rotate": appending is the behaviour in every mode.
   *        The default `false` rotates as before.
   * @return An object that will deinitialize the logging system when it goes out of scope.
   * @examples
   * log_init(2, "sunshine.log");
   * @examples_end
   */
  [[nodiscard]] std::unique_ptr<deinit_t> init(int min_log_level, const std::string &log_file, bool append_no_rotate = false);

  /**
   * @brief Setup AV logging.
   * @param min_log_level The log level.
   */
  void setup_av_logging(int min_log_level);

  /**
   * @brief Setup logging for libdisplaydevice.
   * @param min_log_level The log level.
   */
  void setup_libdisplaydevice_logging(int min_log_level);

  /**
   * @brief Flush the log.
   * @examples
   * log_flush();
   * @examples_end
   */
  void log_flush();

  /**
   * @brief Print help to stdout.
   * @param name The name of the program.
   * @examples
   * print_help("sunshine");
   * @examples_end
   */
  void print_help(const char *name);

  /**
   * @brief A helper class for tracking and logging numerical values across a period of time
   * @examples
   * min_max_avg_periodic_logger<int> logger(debug, "Test time value", "ms", 5s);
   * logger.collect_and_log(1);
   * // ...
   * logger.collect_and_log(2);
   * // after 5 seconds
   * logger.collect_and_log(3);
   * // In the log:
   * // [2024:01:01:12:00:00]: Debug: Test time value (min/max/avg): 1ms/3ms/2.00ms
   * @examples_end
   */
  template<typename T>
  class min_max_avg_periodic_logger {
  public:
    /**
     * @brief Construct a periodic logger that reports min, max, and average samples.
     *
     * @param severity Severity level associated with the log message.
     * @param message Message text to log or report.
     * @param units Unit label appended to tracked numeric values.
     * @param interval_in_seconds Interval in seconds.
     */
    min_max_avg_periodic_logger(boost::log::sources::severity_logger<int> &severity, std::string_view message, std::string_view units, std::chrono::seconds interval_in_seconds = std::chrono::seconds(20)):
        severity(severity),
        message(message),
        units(units),
        interval(interval_in_seconds),
        enabled(config::sunshine.min_log_level <= severity.default_severity()) {
    }

    /**
     * @brief Collect a metric sample and write it to the periodic log when due.
     *
     * @param value Numeric sample to include in the next periodic aggregate.
     */
    void collect_and_log(const T &value) {
      if (enabled) {
        auto print_info = [&](const T &min_value, const T &max_value, double avg_value) {
          auto f = stat_trackers::two_digits_after_decimal();
          if constexpr (std::is_floating_point_v<T>) {
            BOOST_LOG(severity.get()) << message << " (min/max/avg): " << f % min_value << units << "/" << f % max_value << units << "/" << f % avg_value << units;
          } else {
            BOOST_LOG(severity.get()) << message << " (min/max/avg): " << min_value << units << "/" << max_value << units << "/" << f % avg_value << units;
          }
        };
        tracker.collect_and_callback_on_interval(value, print_info, interval);
      }
    }

    /**
     * @brief Collect a metric sample and write it to the periodic log when due.
     *
     * @param func Callable used to calculate the tracked sample value.
     */
    void collect_and_log(std::function<T()> func) {
      if (enabled) {
        collect_and_log(func());
      }
    }

    /**
     * @brief Reset the object to its initial empty state.
     */
    void reset() {
      if (enabled) {
        tracker.reset();
      }
    }

    /**
     * @brief Check whether this periodic logger should emit at the configured severity.
     *
     * @return True when the logger severity is enabled and sampling should continue.
     */
    bool is_enabled() const {
      return enabled;
    }

  private:
    std::reference_wrapper<boost::log::sources::severity_logger<int>> severity;
    std::string message;
    std::string units;
    std::chrono::seconds interval;
    bool enabled;
    stat_trackers::min_max_avg_tracker<T> tracker;
  };

  /**
   * @brief A helper class for tracking and logging short time intervals across a period of time
   * @examples
   * time_delta_periodic_logger logger(debug, "Test duration", 5s);
   * logger.first_point_now();
   * // ...
   * logger.second_point_now_and_log();
   * // after 5 seconds
   * logger.first_point_now();
   * // ...
   * logger.second_point_now_and_log();
   * // In the log:
   * // [2024:01:01:12:00:00]: Debug: Test duration (min/max/avg): 1.23ms/3.21ms/2.31ms
   * @examples_end
   */
  class time_delta_periodic_logger {
  public:
    /**
     * @brief Construct a periodic logger for elapsed-time samples.
     *
     * @param severity Severity level associated with the log message.
     * @param message Message text to log or report.
     * @param interval_in_seconds Interval in seconds.
     */
    time_delta_periodic_logger(boost::log::sources::severity_logger<int> &severity, std::string_view message, std::chrono::seconds interval_in_seconds = std::chrono::seconds(20)):
        logger(severity, message, "ms", interval_in_seconds) {
    }

    /**
     * @brief Store the first timestamp for a measured interval.
     *
     * @param point Time point used for elapsed-time calculations.
     */
    void first_point(const std::chrono::steady_clock::time_point &point) {
      if (logger.is_enabled()) {
        point1 = point;
      }
    }

    /**
     * @brief Store the current time as the first timestamp.
     */
    void first_point_now() {
      if (logger.is_enabled()) {
        first_point(std::chrono::steady_clock::now());
      }
    }

    /**
     * @brief Store the second timestamp and log the elapsed interval.
     *
     * @param point Time point used for elapsed-time calculations.
     */
    void second_point_and_log(const std::chrono::steady_clock::time_point &point) {
      if (logger.is_enabled()) {
        logger.collect_and_log(std::chrono::duration<double, std::milli>(point - point1).count());
      }
    }

    /**
     * @brief Store the current time as the second timestamp and log the elapsed interval.
     */
    void second_point_now_and_log() {
      if (logger.is_enabled()) {
        second_point_and_log(std::chrono::steady_clock::now());
      }
    }

    /**
     * @brief Reset the object to its initial empty state.
     */
    void reset() {
      if (logger.is_enabled()) {
        logger.reset();
      }
    }

    /**
     * @brief Check whether this periodic logger should emit at the configured severity.
     *
     * @return True when the logger severity is enabled and sampling should continue.
     */
    bool is_enabled() const {
      return logger.is_enabled();
    }

  private:
    std::chrono::steady_clock::time_point point1 = std::chrono::steady_clock::now();
    min_max_avg_periodic_logger<double> logger;
  };

  /**
   * @brief Enclose string in square brackets.
   * @param input Input string.
   * @return Enclosed string.
   */
  std::string bracket(const std::string &input);

  /**
   * @brief Enclose string in square brackets.
   * @param input Input string.
   * @return Enclosed string.
   */
  std::wstring bracket(const std::wstring &input);

}  // namespace logging
