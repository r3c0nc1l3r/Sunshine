// Contract tests for the standalone Sunshine client public API. Synthetic data only.
#include "private_seat_test_root.h"
#include "src/platform/linux/private_seat_client.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <limits.h>
#include <limits>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {
  using namespace private_seat;
  using Bytes = std::vector<std::uint8_t>;

  void u16(Bytes &b, std::uint16_t n) {
    for (int i = 0; i < 2; ++i) {
      b.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
    }
  }

  void u32(Bytes &b, std::uint32_t n) {
    for (int i = 0; i < 4; ++i) {
      b.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
    }
  }

  void u64(Bytes &b, std::uint64_t n) {
    for (int i = 0; i < 8; ++i) {
      b.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
    }
  }

  void f64(Bytes &b, double v) {
    std::uint64_t bits;
    static_assert(sizeof bits == sizeof v);
    std::memcpy(&bits, &v, sizeof bits);
    u64(b, bits);
  }

  Bytes header(std::uint16_t type, std::uint32_t size) {
    Bytes b {'L', 'B', 'S', '1'};
    u16(b, 1);
    u16(b, type);
    u32(b, size);
    return b;
  }

  Bytes welcome_wire() {
    Bytes b = header(1, 40);
    u64(b, 7);
    u64(b, 9);
    u64(b, 11);
    u32(b, 2560);
    u32(b, 1440);
    u32(b, 1280);
    u32(b, 720);
    return b;
  }

  Bytes active_wire() {
    Bytes b = header(3, 32);
    u64(b, 7);
    u64(b, 9);
    u64(b, 11);
    u64(b, 13);
    return b;
  }

  welcome announcement() {
    welcome w;
    w.identity.generation = 7;
    w.identity.epoch = 9;
    w.identity.output_id = 11;
    w.physical_width = 2560;
    w.physical_height = 1440;
    w.logical_width = 1280;
    w.logical_height = 720;
    return w;
  }

  tokens identity() {
    tokens t;
    t.generation = 7;
    t.epoch = 9;
    t.output_id = 11;
    t.capture_generation = 13;
    return t;
  }

  Bytes prefix(std::uint16_t type, std::uint32_t size) {
    Bytes b = header(type, size);
    u64(b, 7);
    u64(b, 9);
    u64(b, 11);
    u64(b, 13);
    return b;
  }

  auto supported = [](std::uint16_t code) {
    return code == 30 || code == 42;
  };

  TEST(PrivateSeatWire, FragmentedAndCoalescedServerFrames) {
    const auto w = welcome_wire();
    const auto a = active_wire();
    for (std::size_t n = 0; n < w.size(); ++n) {
      server_frame out;
      std::size_t used = 99;
      EXPECT_EQ(decode_server_frame(Bytes(w.begin(), w.begin() + n), out, used), decode_result::incomplete) << n;
      EXPECT_EQ(used, 0u);
    }
    Bytes both = w;
    both.insert(both.end(), a.begin(), a.end());
    server_frame out;
    std::size_t used = 0;
    ASSERT_EQ(decode_server_frame(both, out, used), decode_result::complete);
    EXPECT_EQ(used, w.size());
    EXPECT_EQ(out.type, 1);
    EXPECT_EQ(out.announcement.identity.generation, 7u);
    EXPECT_EQ(out.announcement.identity.epoch, 9u);
    EXPECT_EQ(out.announcement.identity.output_id, 11u);
    EXPECT_EQ(out.announcement.physical_width, 2560u);
    EXPECT_EQ(out.announcement.logical_height, 720u);
    both.erase(both.begin(), both.begin() + used);
    ASSERT_EQ(decode_server_frame(both, out, used), decode_result::complete);
    EXPECT_EQ(used, a.size());
    EXPECT_EQ(out.type, 3);
    EXPECT_EQ(out.identity.capture_generation, 13u);
  }

  TEST(PrivateSeatWire, RejectsInvalidHeadersAndInvalidWelcomeValues) {
    std::vector<Bytes> invalid;
    auto b = welcome_wire();
    b[0] = 'X';
    invalid.push_back(b);
    b = welcome_wire();
    b[4] = 2;
    invalid.push_back(b);
    b = welcome_wire();
    b[6] = 10;
    invalid.push_back(b);
    b = welcome_wire();
    b[8] = 41;
    invalid.push_back(b);
    b = header(1, 0xffffffffu);
    invalid.push_back(b);
    b = welcome_wire();
    for (int i = 12; i < 20; ++i) {
      b[i] = 0;
    }
    invalid.push_back(b);
    b = welcome_wire();
    for (int i = 36; i < 40; ++i) {
      b[i] = 0;
    }
    invalid.push_back(b);
    for (const auto &wire : invalid) {
      server_frame out;
      std::size_t used = 99;
      EXPECT_EQ(decode_server_frame(wire, out, used), decode_result::invalid);
      EXPECT_EQ(used, 0u);
    }
  }

  TEST(PrivateSeatWire, ReadyHasExactFullViewportAndIdentity) {
    auto w = announcement();
    viewport v {0, 0, 2560, 1440};
    Bytes got;
    ASSERT_TRUE(encode_ready(w, 13, v, got));
    auto expected = prefix(2, 48);
    u32(expected, 0);
    u32(expected, 0);
    u32(expected, 2560);
    u32(expected, 1440);
    EXPECT_EQ(got, expected);
    EXPECT_FALSE(encode_ready(w, 0, v, got));
    v.x = 1;
    EXPECT_FALSE(encode_ready(w, 13, v, got));
    v.x = 0;
    v.width = 2559;
    EXPECT_FALSE(encode_ready(w, 13, v, got));
    v.width = 2560;
    w.identity.output_id = 0;
    EXPECT_FALSE(encode_ready(w, 13, v, got));
  }

  TEST(PrivateSeatWire, ExactEventEncodingAndClippedAbsolute) {
    Bytes got;
    event e;
    e.type = event_type::key;
    e.code = 30;
    e.down = true;
    ASSERT_TRUE(encode_event(identity(), e, supported, got));
    auto expected = prefix(10, 35);
    u16(expected, 30);
    expected.push_back(1);
    EXPECT_EQ(got, expected);
    e.type = event_type::absolute;
    e.x = -1;
    e.y = 2;
    ASSERT_TRUE(encode_event(identity(), e, supported, got));
    expected = prefix(12, 48);
    f64(expected, 0);
    f64(expected, 1);
    EXPECT_EQ(got, expected);
    e.type = event_type::relative;
    e.x = -32768;
    e.y = 32768;
    ASSERT_TRUE(encode_event(identity(), e, supported, got));
    expected = prefix(11, 48);
    f64(expected, -32768);
    f64(expected, 32768);
    EXPECT_EQ(got, expected);
    e.type = event_type::axis;
    e.horizontal = 120;
    e.vertical = -120;
    ASSERT_TRUE(encode_event(identity(), e, supported, got));
    expected = prefix(14, 40);
    u32(expected, 120);
    u32(expected, static_cast<std::uint32_t>(-120));
    EXPECT_EQ(got, expected);
  }

  TEST(PrivateSeatWire, EventBoundsAndUnsupportedCodesFailClosed) {
    Bytes got;
    event e;
    auto id = identity();
    e.type = event_type::key;
    e.code = 31;
    e.down = true;
    EXPECT_FALSE(encode_event(id, e, supported, got));
    e.code = 0x110;
    EXPECT_FALSE(encode_event(id, e, supported, got));
    e.type = event_type::button;
    e.code = 0x115;
    EXPECT_FALSE(encode_event(id, e, supported, got));
    e.code = 0x110;
    EXPECT_TRUE(encode_event(id, e, supported, got));
    e.type = event_type::relative;
    e.x = 32769;
    e.y = 0;
    EXPECT_FALSE(encode_event(id, e, supported, got));
    e.x = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(encode_event(id, e, supported, got));
    e.type = event_type::absolute;
    e.x = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(encode_event(id, e, supported, got));
    e.x = -1.001;
    EXPECT_FALSE(encode_event(id, e, supported, got));
    e.x = 2.001;
    EXPECT_FALSE(encode_event(id, e, supported, got));
    e.type = event_type::axis;
    e.horizontal = 0;
    e.vertical = 0;
    EXPECT_FALSE(encode_event(id, e, supported, got));
    e.horizontal = 15361;
    EXPECT_FALSE(encode_event(id, e, supported, got));
    e.horizontal = 120;
    id.capture_generation = 0;
    EXPECT_FALSE(encode_event(id, e, supported, got));
  }

  // Real AF_UNIX listener. No host input, no Sunshine application, no threads.
  // The fixture leaves its unique socket and directory in TMPDIR for the lead's
  // scratch housekeeping; it never uses /tmp or /dev/shm and never unlinks a path.
  class PrivateSeatTransport: public ::testing::Test {
  protected:
    int listener = -1;
    int server = -1;
    std::string path;
    client::clock::time_point now = client::clock::now();
    std::unique_ptr<client> producer;

    void SetUp() override {
      std::string test_root, root;
      ASSERT_TRUE(private_seat_test::scratch_paths(test_root, root)) << "invalid private-seat scratch environment";
      std::string pattern = root + "/lbs-test-XXXXXX";
      std::vector<char> writable(pattern.begin(), pattern.end());
      writable.push_back('\0');
      char *dir = ::mkdtemp(writable.data());
      ASSERT_NE(dir, nullptr);
      path = std::string(dir) + "/s";
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      ASSERT_LT(path.size(), sizeof address.sun_path);
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
      listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      ASSERT_GE(listener, 0);
      ASSERT_EQ(::bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof address), 0);
      ASSERT_EQ(::listen(listener, 8), 0);
    }

    void TearDown() override {
      producer.reset();
      if (server >= 0) {
        ::close(server);
      }
      if (listener >= 0) {
        ::close(listener);
      }
    }

    bool readable(int fd, int timeout_ms = 1000) {
      pollfd p {fd, POLLIN, 0};
      return ::poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLIN);
    }

    bool start(uid_t expected) {
      producer = std::make_unique<client>(path, expected, supported, now);
      if (!readable(listener)) {
        return false;
      }
      server = ::accept(listener, nullptr, nullptr);
      return server >= 0;
    }

    bool drive_until(state desired) {
      for (int i = 0; i < 40; ++i) {
        if (producer->status() == desired) {
          return true;
        }
        if (producer->status() == state::closed) {
          return false;
        }
        pollfd p {producer->fd(), producer->poll_events(), 0};
        int rc = ::poll(&p, 1, 25);
        if (rc < 0) {
          return false;
        }
        producer->drive(rc ? p.revents : 0, now);
      }
      return producer->status() == desired;
    }

    bool drive_closed() {
      for (int i = 0; i < 40 && producer->status() != state::closed; ++i) {
        pollfd p {producer->fd(), producer->poll_events(), 0};
        int rc = ::poll(&p, 1, 25);
        if (rc < 0) {
          return false;
        }
        producer->drive(rc ? p.revents : 0, now);
      }
      return producer->status() == state::closed;
    }

    bool write_bytes(const Bytes &bytes) {
      std::size_t pos = 0;
      while (pos < bytes.size()) {
        ssize_t n = ::send(server, bytes.data() + pos, bytes.size() - pos, MSG_NOSIGNAL);
        if (n <= 0) {
          return false;
        }
        pos += static_cast<std::size_t>(n);
      }
      return true;
    }

    bool flush_producer() {
      // Run the public dispatcher, never bypass its outgoing buffer.
      for (int i = 0; i < 40; ++i) {
        if (readable(server, 0)) {
          return true;
        }
        pollfd p {producer->fd(), producer->poll_events(), 0};
        int rc = ::poll(&p, 1, 25);
        if (rc < 0) {
          return false;
        }
        if (!producer->drive(rc ? p.revents : 0, now)) {
          return false;
        }
      }
      return false;
    }

    bool read_exact(Bytes &bytes, std::size_t count) {
      bytes.clear();
      while (bytes.size() < count) {
        if (!readable(server)) {
          return false;
        }
        std::uint8_t buf[128];
        std::size_t want = std::min(count - bytes.size(), sizeof buf);
        ssize_t n = ::recv(server, buf, want, 0);
        if (n <= 0) {
          return false;
        }
        bytes.insert(bytes.end(), buf, buf + n);
      }
      return true;
    }

    bool handshake() {
      if (!start(::getuid()) || !write_bytes(welcome_wire()) || !drive_until(state::ready_to_send)) {
        return false;
      }
      viewport full {0, 0, 2560, 1440};
      if (!producer->send_ready(13, full, now) || !flush_producer()) {
        return false;
      }
      Bytes ready;
      if (!read_exact(ready, 60)) {
        return false;
      }
      Bytes expected = prefix(2, 48);
      u32(expected, 0);
      u32(expected, 0);
      u32(expected, 2560);
      u32(expected, 1440);
      if (ready != expected || !write_bytes(active_wire())) {
        return false;
      }
      return drive_until(state::active);
    }

    bool no_reconnect() {
      return !readable(listener, 200);
    }
  };

  TEST_F(PrivateSeatTransport, RejectsWrongServerUidBeforeAnyWelcome) {
    uid_t expected = ::getuid() == std::numeric_limits<uid_t>::max() ? 0 : ::getuid() + 1;
    ASSERT_TRUE(start(expected));
    ASSERT_TRUE(drive_closed());
    EXPECT_EQ(producer->peer().pid, 0);
    EXPECT_FALSE(producer->error().empty());
    EXPECT_EQ(producer->fd(), -1);
    EXPECT_TRUE(no_reconnect());
  }

  TEST_F(PrivateSeatTransport, FragmentedWelcomeAndActiveGateEvents) {
    ASSERT_TRUE(start(::getuid()));
    event key;
    key.type = event_type::key;
    key.code = 30;
    key.down = true;
    EXPECT_FALSE(producer->send_event(key, now));
    const Bytes w = welcome_wire();
    ASSERT_TRUE(write_bytes(Bytes(w.begin(), w.begin() + 7)));
    ASSERT_FALSE(drive_until(state::ready_to_send));
    EXPECT_NE(producer->status(), state::closed);
    ASSERT_TRUE(write_bytes(Bytes(w.begin() + 7, w.end())));
    ASSERT_TRUE(drive_until(state::ready_to_send));
    EXPECT_EQ(producer->peer().pid, ::getpid());
    EXPECT_EQ(producer->peer().uid, ::getuid());
    EXPECT_FALSE(producer->send_event(key, now));
    viewport full {0, 0, 2560, 1440};
    ASSERT_TRUE(producer->send_ready(13, full, now));
    ASSERT_TRUE(flush_producer());
    Bytes ready;
    ASSERT_TRUE(read_exact(ready, 60));
    Bytes expected = prefix(2, 48);
    u32(expected, 0);
    u32(expected, 0);
    u32(expected, 2560);
    u32(expected, 1440);
    EXPECT_EQ(ready, expected);
    const Bytes a = active_wire();
    ASSERT_TRUE(write_bytes(Bytes(a.begin(), a.begin() + 5)));
    ASSERT_FALSE(drive_until(state::active));
    EXPECT_NE(producer->status(), state::closed);
    EXPECT_FALSE(producer->send_event(key, now));
    ASSERT_TRUE(write_bytes(Bytes(a.begin() + 5, a.end())));
    ASSERT_TRUE(drive_until(state::active));
    EXPECT_TRUE(producer->send_event(key, now));
    EXPECT_FALSE(producer->send_event(key, now));
    key.down = false;
    EXPECT_TRUE(producer->send_event(key, now));
    EXPECT_FALSE(producer->send_event(key, now));
    ASSERT_TRUE(flush_producer());
    Bytes events;
    ASSERT_TRUE(read_exact(events, 94));
    Bytes wanted = prefix(10, 35);
    u16(wanted, 30);
    wanted.push_back(1);
    Bytes up = prefix(10, 35);
    u16(up, 30);
    up.push_back(0);
    wanted.insert(wanted.end(), up.begin(), up.end());
    EXPECT_EQ(events, wanted);
  }

  TEST_F(PrivateSeatTransport, ActiveThenCoalescedWelcomeRetiresOldTokens) {
    ASSERT_TRUE(start(::getuid()));
    ASSERT_TRUE(write_bytes(welcome_wire()));
    ASSERT_TRUE(drive_until(state::ready_to_send));
    viewport full {0, 0, 2560, 1440};
    ASSERT_TRUE(producer->send_ready(13, full, now));
    ASSERT_TRUE(flush_producer());
    Bytes ready;
    ASSERT_TRUE(read_exact(ready, 60));
    Bytes combined = active_wire();
    Bytes next = welcome_wire();
    next[12] = 8;
    combined.insert(combined.end(), next.begin(), next.end());
    ASSERT_TRUE(write_bytes(combined));
    ASSERT_TRUE(drive_closed());
    event key;
    key.type = event_type::key;
    key.code = 30;
    key.down = true;
    EXPECT_FALSE(producer->send_event(key, now));
    EXPECT_EQ(producer->fd(), -1);
    EXPECT_TRUE(no_reconnect());
  }

  TEST_F(PrivateSeatTransport, ActiveInvalidFrameAndEofCloseWithoutReconnect) {
    ASSERT_TRUE(handshake());
    ASSERT_TRUE(write_bytes(header(99, 0)));
    ASSERT_TRUE(drive_closed());
    EXPECT_FALSE(producer->error().empty());
    EXPECT_TRUE(no_reconnect());
  }

  TEST_F(PrivateSeatTransport, IncompleteFrameTimeoutCloses) {
    ASSERT_TRUE(handshake());
    ASSERT_TRUE(write_bytes(Bytes {'L', 'B', 'S', '1'}));
    // Consume the fragment while the injected monotonic clock is still at now.
    pollfd p {producer->fd(), producer->poll_events(), 0};
    ASSERT_GT(::poll(&p, 1, 1000), 0);
    ASSERT_TRUE(producer->drive(p.revents, now));
    EXPECT_FALSE(producer->drive(0, now + std::chrono::seconds(6)));
    EXPECT_EQ(producer->status(), state::closed);
    EXPECT_TRUE(no_reconnect());
  }

  TEST_F(PrivateSeatTransport, ActiveIdleHasNoTimeout) {
    ASSERT_TRUE(handshake());
    EXPECT_TRUE(producer->drive(0, now + std::chrono::seconds(30)));
    EXPECT_EQ(producer->status(), state::active);
  }

  TEST_F(PrivateSeatTransport, MismatchedActiveCannotOpenGate) {
    ASSERT_TRUE(start(::getuid()));
    ASSERT_TRUE(write_bytes(welcome_wire()));
    ASSERT_TRUE(drive_until(state::ready_to_send));
    viewport full {0, 0, 2560, 1440};
    ASSERT_TRUE(producer->send_ready(13, full, now));
    ASSERT_TRUE(flush_producer());
    Bytes ready;
    ASSERT_TRUE(read_exact(ready, 60));
    Bytes active = active_wire();
    active[36] = 14;  // Low byte of capture generation.
    ASSERT_TRUE(write_bytes(active));
    ASSERT_TRUE(drive_closed());
    event key;
    key.type = event_type::key;
    key.code = 30;
    key.down = true;
    EXPECT_FALSE(producer->send_event(key, now));
    EXPECT_TRUE(no_reconnect());
  }

  TEST_F(PrivateSeatTransport, EofClosesActiveConnection) {
    ASSERT_TRUE(handshake());
    ASSERT_EQ(::shutdown(server, SHUT_RDWR), 0);
    ::close(server);
    server = -1;
    ASSERT_TRUE(drive_closed());
    EXPECT_FALSE(producer->error().empty());
    EXPECT_TRUE(no_reconnect());
  }

  TEST_F(PrivateSeatTransport, UndrainedOutboundQueueFailsClosedAndDoesNotReconnect) {
    ASSERT_TRUE(handshake());
    event motion;
    motion.type = event_type::relative;
    motion.x = 1;
    motion.y = -1;
    std::size_t accepted = 0;
    while (accepted < 1000 && producer->send_event(motion, now)) {
      ++accepted;
    }
    // Each REL occupies 60 bytes, and the public outbound limit is 4096.
    EXPECT_GT(accepted, 0u);
    EXPECT_LE(accepted, buffer_limit / 60);
    EXPECT_EQ(producer->status(), state::closed);
    EXPECT_FALSE(producer->send_event(motion, now));
    EXPECT_EQ(producer->fd(), -1);
    EXPECT_TRUE(no_reconnect());
  }
}  // namespace
