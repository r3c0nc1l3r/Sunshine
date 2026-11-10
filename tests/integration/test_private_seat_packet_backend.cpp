/**
 * @file test_private_seat_packet_backend.cpp
 * @brief Opt-in packet/backend integration test; the capture proof is
 * synthetic, not a capture test.
 */
#ifdef __linux__
  #include "../unit/platform/linux/private_seat_test_root.h"
  #include "src/config.h"
  #include "src/globals.h"
  #include "src/input.h"
  #include "src/platform/linux/private_seat_input.h"
  #include "third-party/moonlight-common-c/src/Input.h"

  #include <algorithm>
  #include <arpa/inet.h>
  #include <atomic>
  #include <chrono>
  #include <cmath>
  #include <cstdlib>
  #include <cstring>
  #include <future>
  #include <gtest/gtest.h>
  #include <iomanip>
  #include <limits.h>
  #include <linux/input-event-codes.h>
  #include <mutex>
  #include <poll.h>
  #include <sstream>
  #include <string>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <thread>
  #include <unistd.h>
  #include <vector>

namespace {
  using Bytes = std::vector<std::uint8_t>;
  using namespace std::chrono_literals;
  namespace seat = platf::private_seat_input;

  void le16(Bytes &b, std::uint16_t n) {
    b.push_back(n);
    b.push_back(n >> 8);
  }

  void le32(Bytes &b, std::uint32_t n) {
    for (int i = 0; i < 4; ++i) {
      b.push_back(n >> (8 * i));
    }
  }

  void le64(Bytes &b, std::uint64_t n) {
    for (int i = 0; i < 8; ++i) {
      b.push_back(n >> (8 * i));
    }
  }

  void be16(Bytes &b, std::int16_t n) {
    auto v = static_cast<std::uint16_t>(n);
    b.push_back(v >> 8);
    b.push_back(v);
  }

  Bytes frame(std::uint16_t type, std::uint32_t size) {
    Bytes b {'L', 'B', 'S', '1'};
    le16(b, 1);
    le16(b, type);
    le32(b, size);
    return b;
  }

  Bytes welcome() {
    auto b = frame(1, 40);
    le64(b, 7);
    le64(b, 9);
    le64(b, 11);
    le32(b, 2560);
    le32(b, 1440);
    le32(b, 1280);
    le32(b, 720);
    return b;
  }

  Bytes active(std::uint64_t capture_generation) {
    auto b = frame(3, 32);
    le64(b, 7);
    le64(b, 9);
    le64(b, 11);
    le64(b, capture_generation);
    return b;
  }

  std::uint32_t read32(const std::uint8_t *p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
  }

  std::int64_t read_signed_le32(const std::uint8_t *p) {
    const std::uint32_t value = read32(p);
    return value < 0x80000000u ? static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value) - 0x100000000LL;
  }

  // ABS event values are two little-endian IEEE-754 doubles after the header and
  // tokens.
  double read_double_le(const std::uint8_t *p) {
    std::uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) {
      bits |= std::uint64_t(p[i]) << (8 * i);
    }
    double value;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  int read_signed_be16(const std::uint8_t *p) {
    const unsigned value = (unsigned(p[0]) << 8) | unsigned(p[1]);
    return value < 0x8000 ? static_cast<int>(value) : static_cast<int>(value) - 65536;
  }

  Bytes packet(std::uint32_t magic, std::size_t payload) {
    Bytes b;
    auto size = static_cast<std::uint32_t>(payload + 4);
    b.push_back(size >> 24);
    b.push_back(size >> 16);
    b.push_back(size >> 8);
    b.push_back(size);
    le32(b, magic);
    return b;
  }

  Bytes key(std::uint16_t vk, bool down, std::uint8_t modifiers = 0) {
    auto b = packet(down ? KEY_DOWN_EVENT_MAGIC : KEY_UP_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET) - 8);
    b.push_back(0);
    le16(b, vk);
    b.push_back(modifiers);
    le16(b, 0);
    return b;
  }

  Bytes relative(int x, int y) {
    auto b =
      packet(MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET) - 8);
    be16(b, x);
    be16(b, y);
    return b;
  }

  Bytes absolute(int x, int y, int w, int h) {
    auto b = packet(MOUSE_MOVE_ABS_MAGIC, sizeof(NV_ABS_MOUSE_MOVE_PACKET) - 8);
    be16(b, x);
    be16(b, y);
    be16(b, 0);
    be16(b, w - 1);
    be16(b, h - 1);
    return b;
  }

  Bytes button(int n, bool down) {
    auto b = packet(down ? MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5 : MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5, sizeof(NV_MOUSE_BUTTON_PACKET) - 8);
    b.push_back(n);
    return b;
  }

  Bytes wheel(int amount, bool horizontal) {
    auto b = packet(horizontal ? SS_HSCROLL_MAGIC : SCROLL_MAGIC_GEN5, horizontal ? sizeof(SS_HSCROLL_PACKET) - 8 : sizeof(NV_SCROLL_PACKET) - 8);
    be16(b, amount);
    if (!horizontal) {
      be16(b, amount);
      be16(b, 0);
    }
    return b;
  }

  /** @brief Bounded fake compositor; it handles at most two connections and never
   * opens a device. */
  class FakeCompositor {
  public:
    int listener = -1;
    std::atomic<bool> stop {false};
    std::thread worker;
    std::mutex mutex;
    std::vector<Bytes> events;
    std::atomic<int> connections {0};
    std::atomic<std::uint64_t> capture_generation {0};
    std::atomic<bool> failed {false};
    std::atomic<unsigned> ready_frames {0}, received_bytes {0},
      closed_connections {0}, last_frame_type {0};

    bool start(const std::string &path) {
      sockaddr_un addr {};
      addr.sun_family = AF_UNIX;
      if (path.size() >= sizeof(addr.sun_path)) {
        return false;
      }
      std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
      listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (listener < 0 || ::bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) || ::listen(listener, 2)) {
        return false;
      }
      worker = std::thread([this] {
        run();
      });
      return true;
    }

    ~FakeCompositor() {
      stop = true;
      if (worker.joinable()) {
        worker.join();
      }
      if (listener >= 0) {
        ::close(listener);
      }
    }

    bool send_all(int fd, const Bytes &b) {
      std::size_t off = 0;
      while (off < b.size() && !stop) {
        pollfd p {fd, POLLOUT, 0};
        if (::poll(&p, 1, 50) <= 0) {
          continue;
        }
        auto n = ::send(fd, b.data() + off, b.size() - off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n <= 0) {
          return false;
        }
        off += static_cast<std::size_t>(n);
      }
      return off == b.size();
    }

    void run() {
      while (!stop && connections < 2) {
        pollfd lp {listener, POLLIN, 0};
        if (::poll(&lp, 1, 50) <= 0) {
          continue;
        }
        int fd =
          ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
          failed = true;
          return;
        }
        ++connections;
        if (!send_all(fd, welcome())) {
          ::close(fd);
          failed = true;
          return;
        }
        Bytes buf;
        while (!stop) {
          pollfd p {fd, POLLIN, 0};
          int rc = ::poll(&p, 1, 50);
          if (rc < 0) {
            failed = true;
            break;
          }
          if (!rc) {
            continue;
          }
          std::uint8_t chunk[512];
          auto n = ::recv(fd, chunk, sizeof(chunk), 0);
          if (n <= 0) {
            break;
          }
          received_bytes += static_cast<unsigned>(n);
          buf.insert(buf.end(), chunk, chunk + n);
          while (buf.size() >= 12) {
            auto len = read32(buf.data() + 8);
            if (len > 128) {
              failed = true;
              break;
            }
            if (buf.size() < 12 + len) {
              break;
            }
            Bytes f(buf.begin(), buf.begin() + 12 + len);
            buf.erase(buf.begin(), buf.begin() + 12 + len);
            const auto type =
              static_cast<unsigned>(f[6]) | (static_cast<unsigned>(f[7]) << 8);
            last_frame_type = type;
            if (type == 2) {
              ++ready_frames;
              private_seat::welcome announcement;
              announcement.identity = {7, 9, 11, 0};
              announcement.physical_width = 2560;
              announcement.physical_height = 1440;
              announcement.logical_width = 1280;
              announcement.logical_height = 720;
              Bytes expected_ready;
              const auto generation = capture_generation.load();
              if (!private_seat::encode_ready(announcement, generation, {0, 0, 2560, 1440}, expected_ready) || f != expected_ready || !send_all(fd, active(generation))) {
                failed = true;
              }
            } else {
              std::lock_guard lock(mutex);
              events.push_back(std::move(f));
            }
          }
          if (failed) {
            break;
          }
        }
        ::close(fd);
        ++closed_connections;
      }
    }

    std::vector<Bytes> snapshot() {
      std::lock_guard lock(mutex);
      return events;
    }
  };

  /** @brief Private backend integration with manually published capture evidence.
   */
  class PrivateSeatPacketBackend: public ::testing::Test {
  protected:
    config::input_t saved = config::input;
    std::unique_ptr<FakeCompositor> compositor;
    std::unique_ptr<platf::deinit_t> initialized;
    safe::mail_t owner_mail, other_mail;
    std::shared_ptr<input::input_t> owner, other;
    std::shared_ptr<seat::capture_gate> gate;
    bool started_pool = false;
    Bytes last_packet;
    std::uint64_t capture_generation = 0;

    void SetUp() override {
      const char *enabled = std::getenv("SUNSHINE_PRIVATE_SEAT_PACKET_TEST");
      if (!enabled || std::strcmp(enabled, "1")) {
        GTEST_SKIP() << "explicit opt-in required";
      }
      std::string test_root, root;
      ASSERT_TRUE(private_seat_test::scratch_paths(test_root, root)) << "invalid private-seat scratch environment";
      ASSERT_FALSE(task_pool.running()) << "run this filter in a fresh process";
      std::string pattern = root + "/sunshine-seat-packet-XXXXXX";
      std::vector<char> name(pattern.begin(), pattern.end());
      name.push_back('\0');
      char *dir = ::mkdtemp(name.data());
      ASSERT_NE(dir, nullptr);
      std::string path = std::string(dir) + "/s";
      compositor = std::make_unique<FakeCompositor>();
      ASSERT_TRUE(compositor->start(path));
      config::input.backend = "lanternbox-seat";
      config::input.lanternbox_input_socket = path;
      config::input.lanternbox_compositor_uid = ::getuid();
      config::input.keyboard = true;
      config::input.mouse = true;
      config::input.controller = true;
      config::input.keybindings.clear();
      config::input.key_repeat_delay = std::chrono::hours(1);
      config::input.high_resolution_scrolling = true;
      task_pool.start(1);
      started_pool = true;
      initialized = input::init();
      ASSERT_FALSE(input::private_seat_handle().expired());
      owner_mail = std::make_shared<safe::mail_raw_t>();
      other_mail = std::make_shared<safe::mail_raw_t>();
      owner = input::alloc(owner_mail, "synthetic-owner");
      other = input::alloc(other_mail, "synthetic-other");
      ASSERT_TRUE(owner);
      ASSERT_TRUE(other);
      ASSERT_TRUE(input::claim_private_stream(owner_mail));
      ASSERT_FALSE(input::claim_private_stream(other_mail));
    }

    void TearDown() override {
      if (gate) {
        gate->invalidate();
      }
      if (other) {
        input::reset(other);
      }
      if (owner) {
        input::reset(owner);
      }
      if (started_pool) {
        input::terminate_gamepads();
        initialized.reset();
        task_pool.stop();
        task_pool.join();
      }
      owner.reset();
      other.reset();
      owner_mail.reset();
      other_mail.reset();
      compositor.reset();
      config::input = saved;
    }

    bool activate() {
      auto weak = input::private_seat_handle();
      auto transition = seat::begin_capture_transition(weak, gate);
      if (!transition) {
        return false;
      }
      capture_generation = seat::allocate_capture_generation();
      if (!capture_generation) {
        return false;
      }
      compositor->capture_generation = capture_generation;
      gate = std::make_shared<seat::capture_gate>();
      seat::capture_proof p;
      p.wayland_peer = transition->peer;
      p.producer_generation = transition->producer_generation;
      p.output_id = p.frame_output_id = transition->welcome.identity.output_id;
      p.epoch = p.frame_epoch = transition->welcome.identity.epoch;
      p.capture_generation = p.frame_capture_generation = capture_generation;
      p.snapshot_sequence = 1;
      p.frame_request_sequence = p.latest_published_sequence = 2;
      p.physical_width = p.frame_width = 2560;
      p.physical_height = p.frame_height = 1440;
      p.logical_width = 1280;
      p.logical_height = 720;
      p.stream_width = 1920;
      p.stream_height = 1200;
      p.sole_full_normal_origin = true;
      p.post_snapshot_frame_published = true;
      {
        std::lock_guard lock(gate->mutex);
        gate->latest = p;
      }
      if (!seat::post_capture_qualification(weak, gate)) {
        return false;
      }
      for (int i = 0; i < 100; ++i) {
        auto promise = std::make_shared<std::promise<bool>>();
        auto result = promise->get_future();
        auto ctx = weak.lock();
        if (!ctx || !ctx->post([promise](seat::context &c) {
              promise->set_value(c.active());
            })) {
          return false;
        }
        if (result.wait_for(100ms) != std::future_status::ready) {
          return false;
        }
        if (result.get()) {
          return true;
        }
        std::this_thread::sleep_for(20ms);
      }
      return false;
    }

    bool wait_events(std::size_t count) {
      for (int i = 0; i < 100; ++i) {
        if (compositor->snapshot().size() >= count) {
          return true;
        }
        std::this_thread::sleep_for(20ms);
      }
      return false;
    }

    void send(std::shared_ptr<input::input_t> &who, Bytes b) {
      last_packet = b;
      input::passthrough(who, std::move(b));
    }

    std::string diagnostics() {
      std::ostringstream out;
      out << " capture_generation=" << capture_generation
          << " keyboard=" << static_cast<int>(config::input.keyboard)
          << " mouse=" << static_cast<int>(config::input.mouse)
          << " packet_bytes=" << last_packet.size() << " packet_valid="
          << static_cast<int>(input::testing::is_valid_input_packet(last_packet));
      if (last_packet.size() >= 8) {
        out << " packet_magic=" << read32(last_packet.data() + 4);
      }
      if (compositor) {
        out << " connections=" << compositor->connections.load()
            << " closed=" << compositor->closed_connections.load()
            << " ready_frames=" << compositor->ready_frames.load()
            << " peer_bytes=" << compositor->received_bytes.load()
            << " last_frame_type=" << compositor->last_frame_type.load()
            << " events=" << compositor->snapshot().size()
            << " peer_failed=" << static_cast<int>(compositor->failed.load());
      }
      auto ctx = input::private_seat_handle().lock();
      if (!ctx) {
        out << " context=0";
        return out.str();
      }
      auto promise =
        std::make_shared<std::promise<std::pair<std::uint64_t, bool>>>();
      auto future = promise->get_future();
      if (!ctx->post([promise](seat::context &c) {
            promise->set_value({c.generation(), c.active()});
          })) {
        out << " context=1 post=0";
      } else if (future.wait_for(250ms) != std::future_status::ready) {
        out << " context=1 post=1 reply=0";
      } else {
        auto [generation, active] = future.get();
        out << " context=1 post=1 reply=1 generation=" << generation
            << " active=" << static_cast<int>(active);
      }
      return out.str();
    }

    Bytes expected(private_seat::event e) {
      private_seat::tokens id {7, 9, 11, capture_generation};
      Bytes b;
      EXPECT_TRUE(private_seat::encode_event(id, e, [](std::uint16_t) {
        return true;
      },
                                             b));
      return b;
    }

    ::testing::AssertionResult expect_event(std::size_t n, private_seat::event e) {
      if (!wait_events(n + 1)) {
        return ::testing::AssertionFailure()
               << "Timed out waiting for event " << n << diagnostics();
      }
      auto got = compositor->snapshot();
      auto want = expected(e);
      if (e.type == private_seat::event_type::absolute) {
        constexpr std::size_t prefix_size = 12 + 32;
        constexpr std::size_t absolute_size = prefix_size + 16;
        constexpr double tolerance = 1e-12;
        std::ostringstream details;
        details << std::setprecision(17) << " expected_x=" << e.x
                << " expected_y=" << e.y;
        if (last_packet.size() == sizeof(NV_ABS_MOUSE_MOVE_PACKET)) {
          details << " raw_x=" << read_signed_be16(last_packet.data() + 8)
                  << " raw_y=" << read_signed_be16(last_packet.data() + 10)
                  << " raw_width=" << read_signed_be16(last_packet.data() + 14)
                  << " raw_height=" << read_signed_be16(last_packet.data() + 16);
        }
        const auto &actual = got[n];
        if (want.size() != absolute_size || actual.size() != absolute_size) {
          return ::testing::AssertionFailure()
                 << "ABS event " << n << " wrong size expected=" << want.size()
                 << " got=" << actual.size() << details.str() << diagnostics();
        }
        const double expected_wire_x = read_double_le(want.data() + prefix_size);
        const double expected_wire_y =
          read_double_le(want.data() + prefix_size + 8);
        const double x = read_double_le(actual.data() + prefix_size);
        const double y = read_double_le(actual.data() + prefix_size + 8);
        details << " expected_wire_x=" << expected_wire_x
                << " expected_wire_y=" << expected_wire_y << " got_x=" << x
                << " got_y=" << y << " delta_x=" << (x - e.x)
                << " delta_y=" << (y - e.y);
        if (!std::equal(want.begin(), want.begin() + prefix_size, actual.begin()) || !std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1 || std::abs(x - e.x) > tolerance || std::abs(y - e.y) > tolerance) {
          return ::testing::AssertionFailure()
                 << "ABS event " << n << " differs; prefix_exact="
                 << std::equal(want.begin(), want.begin() + prefix_size, actual.begin())
                 << details.str() << diagnostics();
        }
        return ::testing::AssertionSuccess();
      }
      if (e.type == private_seat::event_type::axis && got[n] != want) {
        constexpr std::size_t prefix_size = 12 + 32;
        constexpr std::size_t axis_size = prefix_size + 8;
        std::ostringstream details;
        details << " expected_horizontal=" << e.horizontal
                << " expected_vertical=" << e.vertical
                << " expected_size=" << want.size()
                << " got_size=" << got[n].size();
        if (want.size() == axis_size) {
          details << " expected_wire_horizontal="
                  << read_signed_le32(want.data() + prefix_size)
                  << " expected_wire_vertical="
                  << read_signed_le32(want.data() + prefix_size + 4);
        }
        if (got[n].size() == axis_size && want.size() == axis_size) {
          details << " got_horizontal="
                  << read_signed_le32(got[n].data() + prefix_size)
                  << " got_vertical="
                  << read_signed_le32(got[n].data() + prefix_size + 4)
                  << " prefix_exact="
                  << std::equal(want.begin(), want.begin() + prefix_size, got[n].begin());
        }
        if (last_packet.size() >= 10) {
          details << " raw_amount=" << read_signed_be16(last_packet.data() + 8);
        }
        return ::testing::AssertionFailure() << "Axis event " << n << " differs;"
                                             << details.str() << diagnostics();
      }
      if (got[n] != want) {
        return ::testing::AssertionFailure()
               << "Event " << n << " differs; expected "
               << ::testing::PrintToString(want) << " got "
               << ::testing::PrintToString(got[n]) << diagnostics();
      }
      return ::testing::AssertionSuccess();
    }
  };

  TEST_F(PrivateSeatPacketBackend, RawPacketsOwnershipAndLifecycle) {
    ASSERT_TRUE(activate()) << "synthetic capture proof did not activate backend"
                            << diagnostics();
    EXPECT_TRUE(input::probe_gamepads());
    input::refresh_virtual_input();
    std::size_t n = 0;
    private_seat::event e;
    e.type = private_seat::event_type::key;
    e.code = KEY_LEFTSHIFT;
    e.down = true;
    send(owner, key(0x10, true));
    ASSERT_TRUE(expect_event(n++, e));
    send(owner, key(0xa0, true));
    send(owner, key(0x10, false));
    e.code = KEY_A;
    send(owner, key(0x41, true, 1));
    ASSERT_TRUE(expect_event(n++, e));
    e.down = false;
    send(owner, key(0x41, false, 1));
    ASSERT_TRUE(expect_event(n++, e));
    e.code = KEY_LEFTSHIFT;
    send(owner, key(0xa0, false));
    ASSERT_TRUE(expect_event(n++, e));
    e.type = private_seat::event_type::relative;
    e.x = 17;
    e.y = -9;
    send(owner, relative(17, -9));
    ASSERT_TRUE(expect_event(n++, e));
    // The 2560x1440 output fits the 1920x1200 encoder stream at 1920x1080,
    // leaving 60 encoder pixels of padding at the top and bottom. The packet
    // reference plane is also 1920x1200, so scaling it into the stream is 1:1.
    // Thus (960,600) maps to (0.5,0.5), (960,60) to (0.5,0), and
    // positions in the padding clip to the corresponding output edge.
    e.type = private_seat::event_type::absolute;
    e.x = 0.5;
    e.y = 0.5;
    send(owner, absolute(960, 600, 1921, 1201));
    ASSERT_TRUE(expect_event(n++, e));
    e.y = 0;
    send(owner, absolute(960, 60, 1921, 1201));
    ASSERT_TRUE(expect_event(n++, e));
    send(owner, absolute(960, 0, 1921, 1201));
    ASSERT_TRUE(expect_event(n++, e));
    e.y = 1;
    send(owner, absolute(960, 1200, 1921, 1201));
    ASSERT_TRUE(expect_event(n++, e));
    const int codes[] = {BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE, BTN_EXTRA};
    e.type = private_seat::event_type::button;
    for (int i = 0; i < 5; ++i) {
      e.code = codes[i];
      e.down = true;
      send(owner, button(i + 1, true));
      ASSERT_TRUE(expect_event(n++, e));
      e.down = false;
      send(owner, button(i + 1, false));
      ASSERT_TRUE(expect_event(n++, e));
    }
    e.type = private_seat::event_type::axis;
    e.horizontal = 0;
    // Private wire v120 is positive for UP; Wayland sign conversion is
    // server-side.
    e.vertical = 120;
    send(owner, wheel(120, false));
    ASSERT_TRUE(expect_event(n++, e));
    e.horizontal = -60;
    e.vertical = 0;
    send(owner, wheel(-60, true));
    ASSERT_TRUE(expect_event(n++, e));
    send(other, key(0x41, true));
    input::reset(other);
    auto barrier = std::make_shared<std::promise<void>>();
    auto done = barrier->get_future();
    task_pool.push([barrier] {
      barrier->set_value();
    });
    ASSERT_EQ(done.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(compositor->snapshot().size(), n);
    e.type = private_seat::event_type::key;
    e.code = KEY_A;
    e.down = true;
    send(owner, key(0x41, true));
    ASSERT_TRUE(expect_event(n++, e));
    e.down = false;
    send(owner, key(0x41, false));
    ASSERT_TRUE(expect_event(n++, e));
    EXPECT_EQ(compositor->snapshot().size(), n);
    input::reset(owner);
    input::terminate_gamepads("synthetic-owner");
    owner = input::alloc(owner_mail, "synthetic-owner");
    ASSERT_TRUE(owner);
    ASSERT_TRUE(input::claim_private_stream(owner_mail));
    ASSERT_TRUE(activate()) << diagnostics();
    e.down = true;
    send(owner, key(0x41, true));
    ASSERT_TRUE(expect_event(n++, e));
    e.down = false;
    send(owner, key(0x41, false));
    ASSERT_TRUE(expect_event(n++, e));
    EXPECT_FALSE(compositor->failed);
  }
}  // namespace
#endif
