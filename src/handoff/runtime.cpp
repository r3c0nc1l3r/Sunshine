/**
 * @file src/handoff/runtime.cpp
 * @brief Definitions for `src/handoff/runtime.h`.
 */
// standard includes
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>

// lib includes
#include <sys/eventfd.h>
#include <unistd.h>

// local includes
#include "runtime.h"

namespace handoff {
  namespace {
    /**
     * @brief Largest UDP base accepted from the environment: base + 11 (the audio port) must stay within 65535.
     */
    constexpr unsigned MAX_UDP_BASE = 65524;

    /**
     * @brief Parse a strictly decimal unsigned number within a range.
     *
     * @param text Text; no sign, no spaces, no trailing characters.
     * @param min Smallest accepted value.
     * @param max Largest accepted value.
     * @return The value, or `std::nullopt` when it does not parse or is out of range.
     */
    std::optional<unsigned> parse_decimal(const std::string &text, unsigned min, unsigned max) {
      if (text.empty() || text.size() > 10) {
        return std::nullopt;
      }
      for (char c : text) {
        if (c < '0' || c > '9') {
          return std::nullopt;
        }
      }
      unsigned long long value = 0;
      auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (ec != std::errc() || end != text.data() + text.size() || value < min || value > max) {
        return std::nullopt;
      }
      return static_cast<unsigned>(value);
    }

    /**
     * @brief Read a variable into a copy, so a later `unsetenv` cannot invalidate it.
     *
     * @param env Environment.
     * @param name Variable name.
     * @return The value, or `std::nullopt` when unset.
     */
    std::optional<std::string> read_var(const early_init_env_t &env, const char *name) {
      const char *value = env.getenv(name);
      if (value == nullptr) {
        return std::nullopt;
      }
      return std::string(value);
    }
  }  // namespace

  std::string_view to_string(mode_e mode) {
    switch (mode) {
      case mode_e::disabled:
        return "disabled";
      case mode_e::normal:
        return "normal";
      case mode_e::new_pending:
        return "new_pending";
      case mode_e::new_active:
        return "new_active";
      case mode_e::draining:
        return "draining";
    }
    return "disabled";
  }

  early_init_env_t system_environment() {
    early_init_env_t env;
    env.getenv = [](const char *name) -> const char * {
      return ::getenv(name);
    };
    env.unsetenv = [](const char *name) {
      ::unsetenv(name);
    };
    env.scrub_fds = []() {
      scrub_inherited();
    };
    return env;
  }

  runtime_t::runtime_t():
      _wake_fd(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
  }

  runtime_t::~runtime_t() {
    if (_wake_fd >= 0) {
      ::close(_wake_fd);
    }
  }

  std::string runtime_t::early_init(const early_init_env_t &env) {
    // Copy every value first: getenv pointers die at unsetenv.
    auto fds_var = read_var(env, "SUNSHINE_HANDOFF_FDS");
    auto channel_var = read_var(env, "SUNSHINE_HANDOFF_CHANNEL");

    if (fds_var.has_value() != channel_var.has_value()) {
      return "SUNSHINE_HANDOFF_FDS and SUNSHINE_HANDOFF_CHANNEL must be set together";
    }

    if (!fds_var) {
      // A normal start (possibly an in-place restart that keeps its UDP base and generation).
      auto base_var = read_var(env, "SUNSHINE_UDP_BASE");
      auto generation_var = read_var(env, "SUNSHINE_GENERATION");

      if (base_var) {
        if (auto base = parse_decimal(*base_var, 1, MAX_UDP_BASE)) {
          _udp_base = static_cast<int>(*base);
        }
      }
      if (generation_var) {
        if (auto generation = parse_decimal(*generation_var, 0, 65535)) {
          _generation = *generation;
        }
      }

      env.scrub_fds();
      env.unsetenv("SUNSHINE_UDP_BASE");
      env.unsetenv("SUNSHINE_GENERATION");
      return {};
    }

    // A handoff start. Validate everything before storing anything.
    auto udp_base_var = read_var(env, "SUNSHINE_HANDOFF_UDP_BASE");
    auto generation_var = read_var(env, "SUNSHINE_HANDOFF_GENERATION");
    auto sink_var = read_var(env, "SUNSHINE_HANDOFF_HOST_SINK");

    std::string error;
    auto fds = parse_inherited_fds(*fds_var, *channel_var, &error);
    if (!fds) {
      return error.empty() ? std::string("invalid SUNSHINE_HANDOFF_FDS or SUNSHINE_HANDOFF_CHANNEL") : error;
    }

    std::optional<unsigned> base;
    if (udp_base_var) {
      base = parse_decimal(*udp_base_var, 1, MAX_UDP_BASE);
    }
    if (!base) {
      return "SUNSHINE_HANDOFF_UDP_BASE is missing or invalid";
    }

    std::optional<unsigned> generation;
    if (generation_var) {
      generation = parse_decimal(*generation_var, 1, 65535);
    }
    if (!generation) {
      return "SUNSHINE_HANDOFF_GENERATION is missing or invalid";
    }

    {
      std::lock_guard lock {_mutex};
      _inherited = *fds;
      if (sink_var && !sink_var->empty()) {
        _host_sink = std::move(*sink_var);
      } else {
        _host_sink.reset();
      }
    }
    _udp_base = static_cast<int>(*base);
    _generation = *generation;
    _started_by_handoff = true;
    _is_main = false;
    _mode = mode_e::new_pending;

    env.scrub_fds();
    env.unsetenv("SUNSHINE_UDP_BASE");
    env.unsetenv("SUNSHINE_GENERATION");
    return {};
  }

  void runtime_t::configure(const settings_t &settings) {
    {
      std::lock_guard lock {_mutex};
      _settings = settings;
    }

    // Only a normal start moves between disabled and normal.
    const mode_e target = settings.enabled ? mode_e::normal : mode_e::disabled;
    auto current = _mode.load();
    while (current == mode_e::disabled || current == mode_e::normal) {
      if (current == target || _mode.compare_exchange_weak(current, target)) {
        break;
      }
    }
  }

  bool runtime_t::disable() {
    // Hold the settings mutex across the mode change, so a settings() reader never sees enabled == true with the
    // mode already disabled.
    std::lock_guard lock {_mutex};
    auto current = _mode.load();
    while (current == mode_e::normal || current == mode_e::disabled) {
      if (_mode.compare_exchange_weak(current, mode_e::disabled)) {
        _settings.enabled = false;
        return true;
      }
    }
    return false;
  }

  mode_e runtime_t::mode() const {
    return _mode.load();
  }

  bool runtime_t::transition(mode_e from, mode_e to) {
    const bool legal =
      (from == mode_e::normal && to == mode_e::draining) ||
      (from == mode_e::draining && to == mode_e::normal) ||
      (from == mode_e::new_pending && to == mode_e::new_active) ||
      (from == mode_e::new_active && to == mode_e::normal);
    if (!legal) {
      return false;
    }
    return _mode.compare_exchange_strong(from, to);
  }

  bool runtime_t::enabled() const {
    return _mode.load() != mode_e::disabled;
  }

  bool runtime_t::started_by_handoff() const {
    return _started_by_handoff.load();
  }

  settings_t runtime_t::settings() const {
    std::lock_guard lock {_mutex};
    return _settings;
  }

  unsigned runtime_t::generation() const {
    return _generation.load();
  }

  std::optional<int> runtime_t::udp_base() const {
    int base = _udp_base.load();
    if (base == 0) {
      return std::nullopt;
    }
    return base;
  }

  int runtime_t::effective_udp_base(int configured_port) const {
    int base = _udp_base.load();
    return base != 0 ? base : configured_port;
  }

  bool runtime_t::is_main() const {
    return _is_main.load();
  }

  void runtime_t::set_main(bool value) {
    _is_main = value;
  }

  bool runtime_t::frozen() const {
    return _frozen.load();
  }

  void runtime_t::set_frozen(bool value) {
    _frozen = value;
  }

  bool runtime_t::handoff_in_progress() const {
    return _handoff_in_progress.load();
  }

  void runtime_t::set_handoff_in_progress(bool value) {
    _handoff_in_progress = value;
  }

  std::string runtime_t::process_tag() const {
    return _mode.load() == mode_e::draining ? "drain" : "";
  }

  void runtime_t::note_sigterm() {
    // Async-signal-safe: atomic operations and one write(2) only. Keep errno intact for the interrupted code.
    const int saved_errno = errno;
    int expected = -1;
    const int reason = static_cast<int>(_mode.load() == mode_e::draining ? shutdown_reason_e::stop : shutdown_reason_e::terminated);
    _latched_reason.compare_exchange_strong(expected, reason);
    _shutdown_requested = true;
    if (_wake_fd >= 0) {
      const std::uint64_t one = 1;
      ssize_t ignored = ::write(_wake_fd, &one, sizeof(one));
      (void) ignored;
    }
    errno = saved_errno;
  }

  void runtime_t::request_shutdown() {
    note_sigterm();
  }

  bool runtime_t::shutdown_requested() const {
    return _shutdown_requested.load();
  }

  shutdown_reason_e runtime_t::shutdown_reason() const {
    const int latched = _latched_reason.load();
    if (latched >= 0) {
      return static_cast<shutdown_reason_e>(latched);
    }
    return _mode.load() == mode_e::draining ? shutdown_reason_e::stop : shutdown_reason_e::terminated;
  }

  int runtime_t::shutdown_wake_fd() const {
    return _wake_fd;
  }

  std::optional<inherited_fds_t> runtime_t::inherited() const {
    std::lock_guard lock {_mutex};
    return _inherited;
  }

  std::optional<std::string> runtime_t::host_sink() const {
    std::lock_guard lock {_mutex};
    return _host_sink;
  }

  bool runtime_t::open_instance(std::filesystem::path dir, instance_t initial) {
    auto writer = std::make_shared<instance_writer_t>(std::move(dir), initial);
    {
      std::lock_guard lock {_mutex};
      _instance = writer;
    }
    // The first write runs outside the mutex.
    return writer->write();
  }

  bool runtime_t::instance_set_role(role_e role) {
    std::shared_ptr<instance_writer_t> writer;
    {
      std::lock_guard lock {_mutex};
      writer = _instance;
    }
    return writer && writer->set_role(role);
  }

  bool runtime_t::instance_set_state(state_e state) {
    std::shared_ptr<instance_writer_t> writer;
    {
      std::lock_guard lock {_mutex};
      writer = _instance;
    }
    return writer && writer->set_state(state);
  }

  bool runtime_t::instance_set_sessions(int sessions) {
    std::shared_ptr<instance_writer_t> writer;
    {
      std::lock_guard lock {_mutex};
      writer = _instance;
    }
    return writer && writer->set_sessions(sessions);
  }

  std::optional<instance_t> runtime_t::instance_current() const {
    std::shared_ptr<instance_writer_t> writer;
    {
      std::lock_guard lock {_mutex};
      writer = _instance;
    }
    if (!writer) {
      return std::nullopt;
    }
    return writer->current();
  }

  void runtime_t::instance_remove() {
    std::shared_ptr<instance_writer_t> writer;
    {
      std::lock_guard lock {_mutex};
      writer = std::move(_instance);
      _instance.reset();
    }
    if (writer) {
      writer->remove();
    }
  }

  void runtime_t::set_channel(std::shared_ptr<channel_t> channel) {
    std::lock_guard lock {_mutex};
    _channel = std::move(channel);
  }

  std::shared_ptr<channel_t> runtime_t::channel() const {
    std::lock_guard lock {_mutex};
    return _channel;
  }

  void runtime_t::set_peer(std::shared_ptr<drain_peer_t> peer) {
    std::lock_guard lock {_mutex};
    _peer = std::move(peer);
  }

  std::shared_ptr<drain_peer_t> runtime_t::peer() const {
    std::lock_guard lock {_mutex};
    return _peer;
  }

#ifdef SUNSHINE_TESTS
  void runtime_t::set_udp_base_for_test(int base) {
    _udp_base = base;
  }

  void runtime_t::reset_for_test() {
    _mode = mode_e::disabled;
    _started_by_handoff = false;
    _generation = 0;
    _udp_base = 0;
    _is_main = true;
    _frozen = false;
    _handoff_in_progress = false;
    _shutdown_requested = false;
    _latched_reason = -1;
    if (_wake_fd >= 0) {
      std::uint64_t value = 0;
      while (::read(_wake_fd, &value, sizeof(value)) > 0) {
      }
    }
    std::lock_guard lock {_mutex};
    _settings = settings_t {};
    _inherited.reset();
    _host_sink.reset();
    _instance.reset();
    _channel.reset();
    _peer.reset();
  }
#endif

  runtime_t &runtime() {
    static runtime_t instance;
    return instance;
  }
}  // namespace handoff
