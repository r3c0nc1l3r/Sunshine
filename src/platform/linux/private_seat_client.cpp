/**
 * @file src/platform/linux/private_seat_client.cpp
 * @brief Bounded, nonblocking LBS1 client implementation.
 */
#include "private_seat_client.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace private_seat {
  namespace {
    /** @brief Maximum time for handshake and incomplete I/O. */
    constexpr auto timeout = std::chrono::seconds(5);
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559, "LBS1 requires IEEE-754 binary64");

    /** @brief Append a little-endian integer. */
    void put(std::vector<std::uint8_t> &out, std::uint64_t value, unsigned n) {
      for (unsigned i = 0; i < n; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
      }
    }

    /** @brief Read a little-endian integer. */
    std::uint64_t get(const std::uint8_t *p, unsigned n) {
      std::uint64_t value = 0;
      for (unsigned i = 0; i < n; ++i) {
        value |= std::uint64_t(p[i]) << (8 * i);
      }
      return value;
    }

    /** @brief Start a frame with its fixed header. */
    void header(std::vector<std::uint8_t> &out, std::uint16_t type, std::uint32_t size) {
      out.clear();
      out.insert(out.end(), {'L', 'B', 'S', '1'});
      put(out, 1, 2);
      put(out, type, 2);
      put(out, size, 4);
    }

    /** @brief Validate nonzero identity fields. */
    bool valid_tokens(const tokens &t, bool capture) {
      return t.generation && t.epoch && t.output_id && (!capture || t.capture_generation);
    }

    /** @brief Validate a dimension pair and its area. */
    bool dimensions(std::uint32_t w, std::uint32_t h) {
      return w && h && w <= 8192 && h <= 8192 && std::uint64_t(w) * h <= std::uint64_t(7680) * 4320;
    }

    /** @brief Validate all WELCOME fields. */
    bool valid_welcome(const welcome &w) {
      return valid_tokens(w.identity, false) && w.identity.capture_generation == 0 && dimensions(w.physical_width, w.physical_height) && dimensions(w.logical_width, w.logical_height);
    }

    /** @brief Append the active identity prefix. */
    void put_tokens(std::vector<std::uint8_t> &out, const tokens &t) {
      put(out, t.generation, 8);
      put(out, t.epoch, 8);
      put(out, t.output_id, 8);
      put(out, t.capture_generation, 8);
    }

    /** @brief Decode identity fields from a frame. */
    tokens read_tokens(const std::uint8_t *p, bool capture) {
      tokens t;
      t.generation = get(p, 8);
      t.epoch = get(p + 8, 8);
      t.output_id = get(p + 16, 8);
      if (capture) {
        t.capture_generation = get(p + 24, 8);
      }
      return t;
    }

    /** @brief Compare all identity fields. */
    bool equal(const tokens &a, const tokens &b) {
      return a.generation == b.generation && a.epoch == b.epoch && a.output_id == b.output_id && a.capture_generation == b.capture_generation;
    }

    /** @brief Append a binary64 value in little-endian order. */
    void put_double(std::vector<std::uint8_t> &out, double value) {
      std::uint64_t bits;
      std::memcpy(&bits, &value, sizeof(bits));
      put(out, bits, 8);
    }
  }  // namespace

  decode_result decode_server_frame(const std::vector<std::uint8_t> &bytes, server_frame &frame, std::size_t &consumed) {
    consumed = 0;
    if (bytes.size() > buffer_limit) {
      return decode_result::invalid;
    }
    if (bytes.size() < 12) {
      return decode_result::incomplete;
    }
    const auto *p = bytes.data();
    if (std::memcmp(p, "LBS1", 4) != 0 || get(p + 4, 2) != 1) {
      return decode_result::invalid;
    }
    const auto type = get(p + 6, 2);
    const auto size = get(p + 8, 4);
    if ((type != 1 && type != 3) || size > 64 || (type == 1 && size != 40) || (type == 3 && size != 32)) {
      return decode_result::invalid;
    }
    if (bytes.size() < 12 + size) {
      return decode_result::incomplete;
    }
    server_frame result;
    result.type = static_cast<std::uint16_t>(type);
    p += 12;
    if (type == 1) {
      result.announcement.identity = read_tokens(p, false);
      result.announcement.physical_width = static_cast<std::uint32_t>(get(p + 24, 4));
      result.announcement.physical_height = static_cast<std::uint32_t>(get(p + 28, 4));
      result.announcement.logical_width = static_cast<std::uint32_t>(get(p + 32, 4));
      result.announcement.logical_height = static_cast<std::uint32_t>(get(p + 36, 4));
      if (!valid_welcome(result.announcement)) {
        return decode_result::invalid;
      }
    } else {
      result.identity = read_tokens(p, true);
      if (!valid_tokens(result.identity, true)) {
        return decode_result::invalid;
      }
    }
    frame = result;
    consumed = 12 + size;
    return decode_result::complete;
  }

  bool encode_ready(const welcome &w, std::uint64_t capture_generation, const viewport &area, std::vector<std::uint8_t> &bytes) {
    bytes.clear();
    if (!valid_welcome(w) || !capture_generation || area.x || area.y || area.width != w.physical_width || area.height != w.physical_height) {
      return false;
    }
    header(bytes, 2, 48);
    tokens t = w.identity;
    t.capture_generation = capture_generation;
    put_tokens(bytes, t);
    put(bytes, area.x, 4);
    put(bytes, area.y, 4);
    put(bytes, area.width, 4);
    put(bytes, area.height, 4);
    return true;
  }

  bool encode_event(const tokens &identity, const event &value, const std::function<bool(std::uint16_t)> &supported_key, std::vector<std::uint8_t> &bytes) {
    bytes.clear();
    if (!valid_tokens(identity, true)) {
      return false;
    }
    std::uint32_t size;
    switch (value.type) {
      case event_type::key:
        if (!value.code || value.code > 0x2ff || (value.code >= 0x100 && value.code <= 0x15f) || !supported_key || !supported_key(value.code)) {
          return false;
        }
        size = 35;
        break;
      case event_type::button:
        if (value.code < 0x110 || value.code > 0x114) {
          return false;
        }
        size = 35;
        break;
      case event_type::relative:
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || std::abs(value.x) > 32768 || std::abs(value.y) > 32768) {
          return false;
        }
        size = 48;
        break;
      case event_type::absolute:
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || value.x < -1 || value.x > 2 || value.y < -1 || value.y > 2) {
          return false;
        }
        size = 48;
        break;
      case event_type::axis:
        if (value.horizontal < -15360 || value.horizontal > 15360 || value.vertical < -15360 || value.vertical > 15360 || (!value.horizontal && !value.vertical)) {
          return false;
        }
        size = 40;
        break;
      default:
        return false;
    }
    header(bytes, static_cast<std::uint16_t>(value.type), size);
    put_tokens(bytes, identity);
    switch (value.type) {
      case event_type::key:
      case event_type::button:
        put(bytes, value.code, 2);
        put(bytes, value.down ? 1 : 0, 1);
        break;
      case event_type::relative:
      case event_type::absolute:
        put_double(bytes, value.type == event_type::absolute ? std::clamp(value.x, 0.0, 1.0) : value.x);
        put_double(bytes, value.type == event_type::absolute ? std::clamp(value.y, 0.0, 1.0) : value.y);
        break;
      case event_type::axis:
        put(bytes, static_cast<std::uint32_t>(value.horizontal), 4);
        put(bytes, static_cast<std::uint32_t>(value.vertical), 4);
        break;
    }
    return true;
  }

  client::client(const std::string &path, uid_t expected_uid, std::function<bool(std::uint16_t)> supported_key, clock::time_point now):
      expected_uid_(expected_uid),
      supported_key_(std::move(supported_key)),
      handshake_deadline_(now + timeout) {
    sockaddr_un address {};
    if (path.empty() || path[0] != '/' || path.find('\0') != std::string::npos || path.size() >= sizeof(address.sun_path)) {
      fail("Invalid private-seat socket path");
      return;
    }
    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
      fail(std::string("Creating private-seat socket: ") + std::strerror(errno));
      return;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    int result = ::connect(fd_, reinterpret_cast<const sockaddr *>(&address), static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1));
    if (result == 0) {
      state_ = state::connecting;
      authenticate();
    } else if (errno == EINPROGRESS) {
      state_ = state::connecting;
    } else {
      fail(std::string("Connecting private-seat socket: ") + std::strerror(errno));
    }
  }

  client::~client() {
    close();
  }

  int client::fd() const {
    return fd_;
  }

  short client::poll_events() const {
    if (state_ == state::closed) {
      return 0;
    }
    return static_cast<short>(POLLIN | ((state_ == state::connecting || !output_.empty()) ? POLLOUT : 0));
  }

  state client::status() const {
    return state_;
  }

  const std::string &client::error() const {
    return error_;
  }

  peer_credentials client::peer() const {
    return peer_;
  }

  const welcome &client::announcement() const {
    return welcome_;
  }

  void client::close() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = -1;
    state_ = state::closed;
    input_.clear();
    output_.clear();
    keys_.clear();
    buttons_.clear();
  }

  void client::fail(const std::string &reason) {
    error_ = reason;
    close();
  }

  bool client::authenticate() {
    struct ucred credentials {};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd_, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 || length != sizeof(credentials) || credentials.pid <= 0 || credentials.uid != expected_uid_) {
      fail("Private-seat server credentials unavailable or UID mismatch");
      return false;
    }
    peer_ = {credentials.pid, credentials.uid};
    state_ = state::waiting_welcome;
    return true;
  }

  bool client::queue(const std::vector<std::uint8_t> &bytes, clock::time_point now) {
    if (bytes.size() > buffer_limit - output_.size()) {
      fail("Private-seat outbound buffer exhausted");
      return false;
    }
    if (output_.empty()) {
      output_deadline_ = now + timeout;
    }
    output_.insert(output_.end(), bytes.begin(), bytes.end());
    return true;
  }

  bool client::send_ready(std::uint64_t capture_generation, const viewport &area, clock::time_point now) {
    std::vector<std::uint8_t> bytes;
    if (state_ != state::ready_to_send || now >= handshake_deadline_ || !encode_ready(welcome_, capture_generation, area, bytes)) {
      return false;
    }
    if (!queue(bytes, now)) {
      return false;
    }
    active_ = welcome_.identity;
    active_.capture_generation = capture_generation;
    state_ = state::waiting_active;
    return true;
  }

  bool client::send_event(const event &value, clock::time_point now) {
    std::vector<std::uint8_t> bytes;
    if (state_ != state::active || !encode_event(active_, value, supported_key_, bytes)) {
      return false;
    }
    if (value.type == event_type::key || value.type == event_type::button) {
      auto &held = value.type == event_type::key ? keys_ : buttons_;
      bool present = held.count(value.code) != 0;
      if (present == value.down) {
        return false;
      }
      if (!queue(bytes, now)) {
        return false;
      }
      if (value.down) {
        held.insert(value.code);
      } else {
        held.erase(value.code);
      }
      return true;
    }
    return queue(bytes, now);
  }

  bool client::process(clock::time_point now) {
    while (!input_.empty()) {
      server_frame frame;
      std::size_t consumed = 0;
      auto result = decode_server_frame(input_, frame, consumed);
      if (result == decode_result::invalid) {
        fail("Invalid private-seat server frame");
        return false;
      }
      if (result == decode_result::incomplete) {
        if (input_deadline_ == clock::time_point {}) {
          input_deadline_ = now + timeout;
        }
        return true;
      }
      input_.erase(input_.begin(), input_.begin() + consumed);
      input_deadline_ = input_.empty() ? clock::time_point {} : now + timeout;
      if (frame.type == 1 && state_ == state::waiting_welcome) {
        welcome_ = frame.announcement;
        state_ = state::ready_to_send;
      } else if (frame.type == 3 && state_ == state::waiting_active && equal(frame.identity, active_)) {
        state_ = state::active;
      } else {
        fail(frame.type == 1 ? "Private-seat output invalidated; replacement connection required" : "Unexpected or mismatched private-seat ACTIVE");
        return false;
      }
    }
    return true;
  }

  bool client::drive(short revents, clock::time_point now) {
    if (state_ == state::closed) {
      return false;
    }
    if ((state_ != state::active && now >= handshake_deadline_) || (input_deadline_ != clock::time_point {} && now >= input_deadline_) || (!output_.empty() && now >= output_deadline_)) {
      fail("Private-seat handshake or stream deadline expired");
      return false;
    }
    if (revents & POLLNVAL) {
      fail("Invalid private-seat socket descriptor");
      return false;
    }
    if (state_ == state::connecting && (revents & (POLLOUT | POLLIN | POLLERR | POLLHUP))) {
      int error = 0;
      socklen_t length = sizeof(error);
      if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
        fail("Private-seat connection failed");
        return false;
      }
      if (!authenticate()) {
        return false;
      }
    }
    if (!output_.empty() && (revents & POLLOUT)) {
      ssize_t n = ::send(fd_, output_.data(), output_.size(), MSG_NOSIGNAL);
      if (n > 0) {
        output_.erase(output_.begin(), output_.begin() + n);
        if (output_.empty()) {
          output_deadline_ = {};
        }
      } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        fail("Private-seat socket write failed");
        return false;
      }
    }
    if (revents & (POLLIN | POLLHUP | POLLERR)) {
      std::size_t budget = buffer_limit;
      unsigned attempts = 0;
      while (budget && attempts++ < 64) {
        if (input_.size() == buffer_limit) {
          fail("Private-seat inbound buffer exhausted");
          return false;
        }
        std::uint8_t buf[4096];
        ssize_t n = ::recv(fd_, buf, std::min({sizeof(buf), buffer_limit - input_.size(), budget}), 0);
        if (n > 0) {
          budget -= static_cast<std::size_t>(n);
          input_.insert(input_.end(), buf, buf + n);
          if (!process(now)) {
            return false;
          }
        } else if (n == 0) {
          fail(input_.empty() ? "Private-seat server disconnected" : "Truncated private-seat frame at EOF");
          return false;
        } else if (errno == EINTR) {
          continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        } else {
          fail("Private-seat socket read failed");
          return false;
        }
      }
      if (revents & (POLLERR | POLLHUP)) {
        fail("Private-seat server disconnected");
        return false;
      }
    }
    return state_ != state::closed;
  }
}  // namespace private_seat
