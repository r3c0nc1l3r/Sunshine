/**
 * @file src/handoff/instance_file.cpp
 * @brief Definitions for `src/handoff/instance_file.h`.
 */
// standard includes
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <unistd.h>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "instance_file.h"
#include "src/logging.h"

namespace handoff {
  namespace fs = std::filesystem;

  namespace {
    /**
     * @brief Outcome of reading a file.
     */
    enum class read_status_e {
      ok,  ///< The content was read.
      missing,  ///< The file does not exist (`ENOENT`).
      failed,  ///< Any other error.
    };

    /**
     * @brief Read a whole file with POSIX calls, so the errno of a failed open is known.
     *
     * @param path File to read.
     * @param content Receives the content; cleared first.
     * @return Outcome.
     */
    read_status_e read_whole_file(const fs::path &path, std::string &content) {
      constexpr std::size_t max_bytes = 64 * 1024;  // A larger file is treated as unreadable.
      content.clear();
      // O_NOFOLLOW makes a symlink (dangling or not) fail with ELOOP, which counts as failed, never missing. O_NONBLOCK
      // keeps a FIFO from blocking the scan; the fstat below rejects it anyway.
      const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
      if (fd < 0) {
        return errno == ENOENT ? read_status_e::missing : read_status_e::failed;
      }

      struct stat st {};
      if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return read_status_e::failed;
      }

      char buffer[4096];
      while (true) {
        const auto n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0) {
          if (errno == EINTR) {
            continue;
          }
          ::close(fd);
          return read_status_e::failed;
        }
        if (n == 0) {
          break;
        }
        content.append(buffer, static_cast<std::size_t>(n));
        if (content.size() > max_bytes) {
          ::close(fd);
          return read_status_e::failed;
        }
      }
      ::close(fd);
      return read_status_e::ok;
    }

    /**
     * @brief Read a whole file into a string.
     *
     * @param path File to read.
     * @return The content, or `std::nullopt` when the file cannot be opened or read.
     */
    std::optional<std::string> read_file(const fs::path &path) {
      std::string content;
      if (read_whole_file(path, content) != read_status_e::ok) {
        return std::nullopt;
      }
      return content;
    }

    /**
     * @brief Make sure the instances directory exists and is safe to write into.
     *
     * Missing directories, including missing parents, are created with mode 0700. An existing directory must be a real
     * directory (not a symlink), owned by the effective uid, and not writable by group or others.
     *
     * @param dir Instances directory.
     * @return `true` when the directory is usable; otherwise an Error is logged and `false` returned.
     */
    bool prepare_directory(const fs::path &dir) {
      // The runtime directory is assumed to be user-owned and private (systemd RuntimeDirectory or XDG_RUNTIME_DIR).
      // Collect the missing directories from the leaf upwards; they are created from the top down.
      std::vector<fs::path> missing;
      for (fs::path current = dir; !current.empty(); current = current.parent_path()) {
        struct stat st {};
        if (::lstat(current.c_str(), &st) == 0) {
          break;
        }
        if (errno != ENOENT) {
          BOOST_LOG(error) << "Drain handoff: cannot inspect instances directory: " << std::strerror(errno);
          return false;
        }
        missing.push_back(current);
        if (current == current.parent_path()) {
          break;
        }
      }

      for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        if (::mkdir(it->c_str(), 0700) == 0) {
          // mkdir applies the umask, which can only remove bits; chmod makes the mode exactly 0700.
          if (::chmod(it->c_str(), 0700) != 0) {
            BOOST_LOG(error) << "Drain handoff: cannot set mode of instances directory: " << std::strerror(errno);
            return false;
          }
        } else if (errno != EEXIST) {
          BOOST_LOG(error) << "Drain handoff: cannot create instances directory: " << std::strerror(errno);
          return false;
        }
      }

      struct stat st {};
      if (::lstat(dir.c_str(), &st) != 0) {
        BOOST_LOG(error) << "Drain handoff: cannot inspect instances directory: " << std::strerror(errno);
        return false;
      }
      if (!S_ISDIR(st.st_mode)) {
        BOOST_LOG(error) << "Drain handoff: instances path is not a real directory";
        return false;
      }
      if (st.st_uid != ::geteuid()) {
        BOOST_LOG(error) << "Drain handoff: instances directory is not owned by this user";
        return false;
      }
      if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        BOOST_LOG(error) << "Drain handoff: instances directory is writable by group or others";
        return false;
      }
      return true;
    }

    /**
     * @brief Write a file as the atomic replacement for an instance file: temporary file, fsync, rename.
     *
     * The caller holds the writer's mutex.
     *
     * @param dir Instances directory.
     * @param instance Content to write; its `pid` names the file.
     * @return `true` on success.
     */
    bool write_locked(const fs::path &dir, const instance_t &instance) {
      if (!prepare_directory(dir)) {
        return false;
      }

      const auto name = std::to_string(instance.pid) + ".json";
      const auto final_path = dir / name;
      const auto tmp_path = dir / (name + ".tmp");

      // A temporary file left by an earlier crash is removed, so the exclusive create below cannot fail on it.
      if (::unlink(tmp_path.c_str()) != 0 && errno != ENOENT) {
        BOOST_LOG(error) << "Drain handoff: cannot remove old temporary instance file: " << std::strerror(errno);
        return false;
      }

      const int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (fd < 0) {
        BOOST_LOG(error) << "Drain handoff: cannot open instance file for writing: " << std::strerror(errno);
        return false;
      }

      const auto body = to_json(instance);
      bool ok = true;
      std::size_t written = 0;
      while (ok && written < body.size()) {
        const auto n = ::write(fd, body.data() + written, body.size() - written);
        if (n < 0) {
          if (errno == EINTR) {
            continue;
          }
          ok = false;
        } else {
          written += static_cast<std::size_t>(n);
        }
      }
      if (ok && ::fsync(fd) != 0) {
        ok = false;
      }
      if (::close(fd) != 0) {
        ok = false;
      }
      if (ok && ::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        ok = false;
      }

      if (!ok) {
        BOOST_LOG(error) << "Drain handoff: cannot write instance file: " << std::strerror(errno);
        ::unlink(tmp_path.c_str());
      }
      return ok;
    }

    /**
     * @brief Read an integer member of a JSON object.
     *
     * @param doc JSON object.
     * @param key Member name.
     * @return The value, or `std::nullopt` when missing, not an integer or out of the `int64` range.
     */
    std::optional<std::int64_t> integer_member(const nlohmann::json &doc, const char *key) {
      const auto it = doc.find(key);
      if (it == doc.end()) {
        return std::nullopt;
      }
      if (it->is_number_unsigned()) {
        const auto value = it->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(INT64_MAX)) {
          return std::nullopt;
        }
        return static_cast<std::int64_t>(value);
      }
      if (it->is_number_integer()) {
        return it->get<std::int64_t>();
      }
      return std::nullopt;
    }

    /**
     * @brief Read a string member of a JSON object.
     *
     * @param doc JSON object.
     * @param key Member name.
     * @return The value, or `std::nullopt` when missing or not a string.
     */
    std::optional<std::string> string_member(const nlohmann::json &doc, const char *key) {
      const auto it = doc.find(key);
      if (it == doc.end() || !it->is_string()) {
        return std::nullopt;
      }
      return it->get<std::string>();
    }
  }  // namespace

  std::string_view to_string(role_e role) {
    switch (role) {
      case role_e::pending:
        return "pending";
      case role_e::active:
        return "active";
      case role_e::draining:
        return "draining";
    }
    return "active";
  }

  std::string_view to_string(state_e state) {
    switch (state) {
      case state_e::none:
        return "none";
      case state_e::draining:
        return "draining";
      case state_e::handover_failed:
        return "handover_failed";
      case state_e::kill_pending:
        return "kill_pending";
    }
    return "none";
  }

  std::optional<role_e> parse_role(std::string_view name) {
    if (name == "pending") {
      return role_e::pending;
    }
    if (name == "active") {
      return role_e::active;
    }
    if (name == "draining") {
      return role_e::draining;
    }
    return std::nullopt;
  }

  std::optional<state_e> parse_state(std::string_view name) {
    if (name == "none") {
      return state_e::none;
    }
    if (name == "draining") {
      return state_e::draining;
    }
    if (name == "handover_failed") {
      return state_e::handover_failed;
    }
    if (name == "kill_pending") {
      return state_e::kill_pending;
    }
    return std::nullopt;
  }

  std::string to_json(const instance_t &instance) {
    // Built by hand so the member order matches the spec; every value is a number or a fixed ASCII name.
    return "{\"pid\":" + std::to_string(instance.pid) +
           ",\"start_time\":" + std::to_string(instance.start_time) +
           ",\"role\":\"" + std::string(to_string(instance.role)) +
           "\",\"udp_base\":" + std::to_string(instance.udp_base) +
           ",\"sessions\":" + std::to_string(instance.sessions) +
           ",\"handoff\":\"" + std::string(to_string(instance.handoff)) + "\"}";
  }

  std::optional<instance_t> parse_instance(std::string_view json) {
    // The parser is called without exceptions: invalid input yields a discarded value.
    const auto doc = nlohmann::json::parse(std::string(json), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
      return std::nullopt;
    }

    const auto pid = integer_member(doc, "pid");
    const auto start_time = integer_member(doc, "start_time");
    const auto role_name = string_member(doc, "role");
    const auto udp_base = integer_member(doc, "udp_base");
    const auto sessions = integer_member(doc, "sessions");
    const auto handoff_name = string_member(doc, "handoff");
    if (!pid || !start_time || !role_name || !udp_base || !sessions || !handoff_name) {
      return std::nullopt;
    }

    const auto role = parse_role(*role_name);
    const auto handoff = parse_state(*handoff_name);
    if (!role || !handoff) {
      return std::nullopt;
    }

    // Range checks: a pid is positive and fits pid_t, counters are non-negative and fit int.
    if (*pid <= 0 || *pid > INT_MAX || *start_time < 0 || *udp_base < INT_MIN || *udp_base > INT_MAX || *sessions < 0 || *sessions > INT_MAX) {
      return std::nullopt;
    }

    instance_t instance;
    instance.pid = static_cast<pid_t>(*pid);
    instance.start_time = static_cast<std::uint64_t>(*start_time);
    instance.role = *role;
    instance.udp_base = static_cast<int>(*udp_base);
    instance.sessions = static_cast<int>(*sessions);
    instance.handoff = *handoff;
    return instance;
  }

  std::optional<std::uint64_t> parse_stat_start_time(std::string_view stat) {
    // The command name in field 2 may contain spaces and ')', so the fields are counted after the last ')'.
    const auto close = stat.rfind(')');
    if (close == std::string_view::npos) {
      return std::nullopt;
    }

    const auto rest = stat.substr(close + 1);
    std::size_t pos = 0;
    int field = 2;  // Field number of the last token consumed; the first token after ')' is field 3.
    while (pos < rest.size()) {
      while (pos < rest.size() && rest[pos] == ' ') {
        ++pos;
      }
      if (pos >= rest.size()) {
        break;
      }

      const auto token_start = pos;
      while (pos < rest.size() && rest[pos] != ' ') {
        ++pos;
      }
      ++field;

      if (field == 22) {
        const auto token = rest.substr(token_start, pos - token_start);
        std::uint64_t value = 0;
        const auto [end, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (ec != std::errc() || end != token.data() + token.size()) {
          return std::nullopt;
        }
        return value;
      }
    }
    return std::nullopt;
  }

  std::optional<std::uint64_t> read_start_time(pid_t pid) {
    if (pid <= 0) {
      return std::nullopt;
    }

    const auto stat = read_file(fs::path("/proc") / std::to_string(pid) / "stat");
    if (!stat) {
      return std::nullopt;
    }
    return parse_stat_start_time(*stat);
  }

  std::optional<fs::path> runtime_dir(std::string_view runtime_directory, std::string_view xdg_runtime_dir) {
    // systemd may list several directories separated by ':'; only the first one is used. A value that is not an
    // absolute path counts as unset.
    const auto first = fs::path(std::string(runtime_directory.substr(0, runtime_directory.find(':'))));
    if (!first.empty() && first.is_absolute()) {
      return first;
    }
    const auto xdg = fs::path(std::string(xdg_runtime_dir));
    if (!xdg.empty() && xdg.is_absolute()) {
      return xdg / "sunshine";
    }
    return std::nullopt;
  }

  instance_writer_t::instance_writer_t(fs::path dir, instance_t initial):
      _dir(std::move(dir)),
      _instance(initial) {}

  bool instance_writer_t::write() {
    std::lock_guard lock(_mutex);
    if (_removed) {
      return false;
    }
    return write_locked(_dir, _instance);
  }

  bool instance_writer_t::set_role(role_e role) {
    std::lock_guard lock(_mutex);
    if (_removed) {
      return false;
    }
    _instance.role = role;
    return write_locked(_dir, _instance);
  }

  bool instance_writer_t::set_state(state_e state) {
    std::lock_guard lock(_mutex);
    if (_removed) {
      return false;
    }
    _instance.handoff = state;
    return write_locked(_dir, _instance);
  }

  bool instance_writer_t::set_sessions(int sessions) {
    std::lock_guard lock(_mutex);
    if (_removed) {
      return false;
    }
    _instance.sessions = sessions;
    return write_locked(_dir, _instance);
  }

  instance_t instance_writer_t::current() const {
    std::lock_guard lock(_mutex);
    return _instance;
  }

  void instance_writer_t::remove() {
    std::lock_guard lock(_mutex);
    _removed = true;
    remove_instance_file(_dir, _instance.pid);
  }

  /**
   * @brief Remove the instance file of process `pid` from `dir`.
   */
  void remove_instance_file(const fs::path &dir, pid_t pid) {
    const auto path = dir / (std::to_string(pid) + ".json");
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
      BOOST_LOG(warning) << "Drain handoff: cannot remove instance file: " << std::strerror(errno);
    }
  }

  namespace {
    /**
     * @brief Classify one directory entry for `scan_instances()`.
     *
     * @param path Entry path.
     * @param result Scan result to update.
     * @param start_time_of Returns a pid's current start time.
     */
    void scan_entry(const fs::path &path, scan_result_t &result, const std::function<std::optional<std::uint64_t>(pid_t)> &start_time_of) {
      if (path.extension() != ".json") {
        return;
      }

      std::string content;
      const auto status = read_whole_file(path, content);
      if (status == read_status_e::missing) {
        // Removed by its process during the scan.
        return;
      }
      if (status == read_status_e::failed) {
        result.any_unreadable = true;
        return;
      }

      // The file name must name the pid inside the file.
      const auto instance = parse_instance(content);
      if (!instance || path.filename() != fs::path(std::to_string(instance->pid) + ".json")) {
        result.any_unreadable = true;
        return;
      }

      const auto current = start_time_of(instance->pid);
      if (current && *current == instance->start_time) {
        result.live.push_back(*instance);
      } else {
        result.stale.push_back(instance->pid);
      }
    }
  }  // namespace

  /**
   * @brief Read the instance files in `dir` and sort them into live and stale instances.
   */
  scan_result_t scan_instances(const fs::path &dir, const std::function<std::optional<std::uint64_t>(pid_t)> &start_time_of) {
    scan_result_t result;

    try {
      std::error_code ec;
      // Only a directory that does not exist is "missing". Any other failure to inspect it, or a path that is not a
      // directory, means the instances cannot be listed, so the scan fails closed.
      const auto status = fs::status(dir, ec);
      if (ec) {
        if (ec == std::errc::no_such_file_or_directory) {
          result.dir_missing = true;
        } else {
          result.any_unreadable = true;
        }
        return result;
      }
      if (status.type() == fs::file_type::not_found) {
        result.dir_missing = true;
        return result;
      }
      if (!fs::is_directory(status)) {
        result.any_unreadable = true;
        return result;
      }

      fs::directory_iterator it(dir, ec);
      if (ec) {
        result.any_unreadable = true;
        return result;
      }

      // Explicit increment, then check its error: a range-for or a for loop with `increment(ec)` in the step would
      // silently end the scan (libstdc++ sets the iterator to end on error), so the error must be read after each step.
      const fs::directory_iterator end;
      while (it != end) {
        const fs::path path = it->path();
        scan_entry(path, result, start_time_of);

        it.increment(ec);
        if (ec) {
          result.any_unreadable = true;
          break;
        }
      }
    } catch (...) {
      // Anything that throws (for example bad_alloc) leaves the listing incomplete: fail closed.
      result.any_unreadable = true;
    }
    return result;
  }

  bool consumer_busy(const scan_result_t &scan, const std::vector<pid_t> &running_sunshine_pids) {
    // A missing directory is only busy while a Sunshine process runs.
    if (scan.dir_missing && !running_sunshine_pids.empty()) {
      return true;
    }
    if (scan.any_unreadable) {
      return true;
    }

    // Every running Sunshine process needs a live file of its own.
    for (const auto pid : running_sunshine_pids) {
      const auto has_file = std::any_of(scan.live.begin(), scan.live.end(), [pid](const instance_t &instance) {
        return instance.pid == pid;
      });
      if (!has_file) {
        return true;
      }
    }

    // Any live file that shows a handoff in progress, or sessions, makes Sunshine busy.
    for (const auto &instance : scan.live) {
      if (instance.role == role_e::pending || instance.sessions > 0 || instance.handoff == state_e::kill_pending) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Wait until the instance file of process `pid` in `dir` is active, or the timeout expires.
   */
  bool wait_for_active(const fs::path &dir, pid_t pid, std::uint64_t start_time, std::chrono::milliseconds timeout) {
    using clock = std::chrono::steady_clock;
    constexpr auto poll_interval = std::chrono::milliseconds(100);

    const auto deadline = clock::now() + timeout;
    const auto path = dir / (std::to_string(pid) + ".json");
    while (true) {
      const auto content = read_file(path);
      const auto instance = content ? parse_instance(*content) : std::nullopt;
      if (instance && instance->pid == pid && instance->start_time == start_time && instance->role == role_e::active) {
        return true;
      }

      const auto now = clock::now();
      if (now >= deadline) {
        return false;
      }
      // Never sleep longer than the poll interval, and never past the deadline.
      std::this_thread::sleep_for(std::min<clock::duration>(poll_interval, deadline - now));
    }
  }
}  // namespace handoff
