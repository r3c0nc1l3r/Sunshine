// Blind contract tests: synthetic Unix peer only.
#include "private_seat_test_root.h"
#include "src/config.h"
#include "src/platform/linux/private_seat_input.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {
  using namespace std::chrono_literals;
  namespace psi = platf::private_seat_input;
  using bytes = std::vector<std::uint8_t>;

  void le(bytes &b, std::uint64_t value, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      b.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
  }

  bytes frame(unsigned type, const bytes &payload) {
    bytes b {'L', 'B', 'S', '1'};
    le(b, 1, 2);
    le(b, type, 2);
    le(b, payload.size(), 4);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
  }

  bytes welcome(std::uint64_t generation) {
    bytes b;
    le(b, generation, 8);
    le(b, 7, 8);
    le(b, 9, 8);
    le(b, 2560, 4);
    le(b, 1440, 4);
    le(b, 2560, 4);
    le(b, 1440, 4);
    return frame(1, b);
  }

  bytes active(std::uint64_t generation, std::uint64_t capture) {
    bytes b;
    le(b, generation, 8);
    le(b, 7, 8);
    le(b, 9, 8);
    le(b, capture, 8);
    return frame(3, b);
  }

  bool write_all(int fd, const bytes &b) {
    std::size_t n = 0;
    while (n < b.size()) {
      pollfd p {fd, POLLOUT, 0};
      if (poll(&p, 1, 1000) != 1) {
        return false;
      }
      auto wrote = send(fd, b.data() + n, b.size() - n, MSG_NOSIGNAL);
      if (wrote <= 0) {
        return false;
      }
      n += static_cast<std::size_t>(wrote);
    }
    return true;
  }

  bool read_exact(int fd, bytes &out, std::size_t size) {
    out.clear();
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (out.size() < size && std::chrono::steady_clock::now() < deadline) {
      pollfd p {fd, POLLIN, 0};
      if (poll(&p, 1, 20) <= 0) {
        continue;
      }
      std::array<std::uint8_t, 128> buf {};
      auto n = recv(fd, buf.data(), std::min(buf.size(), size - out.size()), 0);
      if (n <= 0) {
        return false;
      }
      out.insert(out.end(), buf.begin(), buf.begin() + n);
    }
    return out.size() == size;
  }

  class GateTest: public ::testing::Test {
  protected:
    std::shared_ptr<psi::context> ctx;
    std::shared_ptr<psi::capture_gate> gate;
    int listener = -1, server = -1;
    config::input_t saved_input;
    std::mutex queue_mutex;
    std::condition_variable queued;
    std::deque<std::function<void()>> queue;
    std::string socket_path;
    std::uint64_t capture_generation = 0;

    void SetUp() override {
      saved_input = config::input;
      capture_generation = psi::allocate_capture_generation();
      ASSERT_NE(capture_generation, 0u);
      std::string test_root, tmp;
      ASSERT_TRUE(private_seat_test::scratch_paths(test_root, tmp)) << "invalid private-seat scratch environment";
      std::string dir = tmp + "/seat-gate-XXXXXX";
      std::vector<char> name(dir.begin(), dir.end());
      name.push_back('\0');
      ASSERT_NE(mkdtemp(name.data()), nullptr);
      socket_path = std::string(name.data()) + "/s";
      ASSERT_LT(socket_path.size(), sizeof(sockaddr_un::sun_path));
      listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      ASSERT_GE(listener, 0);
      sockaddr_un addr {};
      addr.sun_family = AF_UNIX;
      std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);
      ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
      ASSERT_EQ(listen(listener, 8), 0);
      config::input.backend = "lanternbox-seat";
      config::input.lanternbox_input_socket = socket_path;
      config::input.lanternbox_compositor_uid = getuid();
      ctx = std::make_shared<psi::context>();
      ctx->set_dispatcher([this](std::function<void()> f) {
        {
          std::lock_guard<std::mutex> lock(queue_mutex);
          queue.push_back(std::move(f));
        }
        queued.notify_one();
        return true;
      },
                          [](std::weak_ptr<psi::context>, std::uint64_t) {
                          });
      gate = std::make_shared<psi::capture_gate>();
    }

    void TearDown() override {
      if (ctx) {
        ctx->reset();
      }
      ctx.reset();
      if (server >= 0) {
        close(server);
      }
      if (listener >= 0) {
        close(listener);
      }
      config::input = saved_input;
      // The operator owns cleanup of the dedicated TMPDIR.
    }

    bool dispatch_one() {
      std::function<void()> f;
      {
        std::unique_lock<std::mutex> lock(queue_mutex);
        if (!queued.wait_for(lock, 1s, [this] {
              return !queue.empty();
            })) {
          return false;
        }
        f = std::move(queue.front());
        queue.pop_front();
      }
      f();
      return true;
    }

    bool connect_and_welcome(std::uint64_t server_generation) {
      if (!ctx->begin_transition()) {
        return false;
      }
      pollfd lp {listener, POLLIN, 0};
      if (poll(&lp, 1, 1000) != 1) {
        return false;
      }
      if (server >= 0) {
        close(server);
      }
      server = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
      if (server < 0 || !write_all(server, welcome(server_generation))) {
        return false;
      }
      const auto deadline = std::chrono::steady_clock::now() + 1s;
      while (!ctx->welcome_cleanup_ack() && std::chrono::steady_clock::now() < deadline) {
        pollfd p {ctx->fd(), ctx->poll_events(), 0};
        poll(&p, 1, 20);
        if (!ctx->drive(p.revents, private_seat::client::clock::now())) {
          return false;
        }
      }
      return ctx->welcome_cleanup_ack();
    }

    psi::capture_proof proof() {
      psi::capture_proof p;
      p.wayland_peer = ctx->peer();
      p.producer_generation = ctx->generation();
      p.output_id = p.frame_output_id = 9;
      p.epoch = p.frame_epoch = 7;
      p.capture_generation = p.frame_capture_generation = capture_generation;
      p.snapshot_sequence = 20;
      p.frame_request_sequence = p.latest_published_sequence = 21;
      p.physical_width = p.logical_width = p.frame_width = p.stream_width = 2560;
      p.physical_height = p.logical_height = p.frame_height = p.stream_height = 1440;
      p.sole_full_normal_origin = p.post_snapshot_frame_published = true;
      return p;
    }

    bool drive_out() {
      pollfd p {ctx->fd(), ctx->poll_events(), 0};
      if (poll(&p, 1, 1000) < 0) {
        return false;
      }
      return ctx->drive(p.revents, private_seat::client::clock::now());
    }

    bool activate() {
      if (!connect_and_welcome(3)) {
        return false;
      }
      {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->latest = proof();
      }
      if (!ctx->qualify_capture(gate) || !drive_out()) {
        return false;
      }
      bytes got;
      if (!read_exact(server, got, 60)) {
        return false;
      }
      bytes expected;
      le(expected, 3, 8);
      le(expected, 7, 8);
      le(expected, 9, 8);
      le(expected, capture_generation, 8);
      le(expected, 0, 4);
      le(expected, 0, 4);
      le(expected, 2560, 4);
      le(expected, 1440, 4);
      if (got != frame(2, expected) || !write_all(server, active(3, capture_generation))) {
        return false;
      }
      const auto deadline = std::chrono::steady_clock::now() + 1s;
      while (!ctx->active() && std::chrono::steady_clock::now() < deadline) {
        if (!drive_out()) {
          return false;
        }
      }
      return ctx->active();
    }
  };

  TEST_F(GateTest, InvalidationDiscardsAlreadyQueuedEventBeforeDrive) {
    ASSERT_TRUE(activate());
    ASSERT_TRUE(ctx->send_key(30, true));
    gate->invalidate();
    ctx->drive(POLLOUT, private_seat::client::clock::now());
    pollfd p {server, POLLIN, 0};
    int rc = poll(&p, 1, 100);
    ASSERT_GE(rc, 0);
    if (rc > 0) {
      std::array<char, 64> data {};
      EXPECT_EQ(recv(server, data.data(), data.size(), MSG_DONTWAIT), 0) << "no event bytes may escape the retired gate";
    }
    EXPECT_FALSE(ctx->active());
    EXPECT_FALSE(ctx->qualify_capture(gate));
  }

  TEST_F(GateTest, QueuedQualificationReadsLatestProofAtDispatchTime) {
    ASSERT_TRUE(connect_and_welcome(3));
    auto p = proof();
    p.frame_request_sequence = 21;
    p.latest_published_sequence = 22;  // stale proof
    {
      std::lock_guard<std::mutex> lock(gate->mutex);
      gate->latest = p;
    }
    ASSERT_TRUE(psi::post_capture_qualification(ctx, gate));
    p.frame_request_sequence = p.latest_published_sequence;
    {
      std::lock_guard<std::mutex> lock(gate->mutex);
      gate->latest = p;
    }
    ASSERT_TRUE(dispatch_one());
    ASSERT_TRUE(drive_out());
    bytes got;
    ASSERT_TRUE(read_exact(server, got, 60));
    bytes expected;
    le(expected, 3, 8);
    le(expected, 7, 8);
    le(expected, 9, 8);
    le(expected, capture_generation, 8);
    le(expected, 0, 4);
    le(expected, 0, 4);
    le(expected, 2560, 4);
    le(expected, 1440, 4);
    EXPECT_EQ(got, frame(2, expected));
  }

  TEST_F(GateTest, InvalidatedGateCannotQualify) {
    ASSERT_TRUE(connect_and_welcome(3));
    {
      std::lock_guard<std::mutex> lock(gate->mutex);
      gate->latest = proof();
    }
    gate->invalidate();
    EXPECT_FALSE(ctx->qualify_capture(gate));
    EXPECT_FALSE(ctx->active());
    pollfd p {server, POLLIN, 0};
    EXPECT_EQ(poll(&p, 1, 100), 0);
  }

  TEST_F(GateTest, ReplacementWelcomeWaitsForDispatcherResetCompletion) {
    ASSERT_TRUE(activate());
    const auto old = ctx->generation();
    std::uint64_t reset_generation = 0;
    ctx->on_reset([&](std::uint64_t g) {
      reset_generation = g;
    });
    bool callback_called = false;
    bool callback_ok = false;
    ASSERT_TRUE(ctx->begin_transition([&](bool ok, std::uint64_t, private_seat::welcome, private_seat::peer_credentials) {
      callback_called = true;
      callback_ok = ok;
    }));
    ASSERT_EQ(reset_generation, ctx->generation());
    ASSERT_GT(reset_generation, old);
    pollfd lp {listener, POLLIN, 0};
    ASSERT_EQ(poll(&lp, 1, 1000), 1);
    int replacement = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    ASSERT_GE(replacement, 0);
    ASSERT_TRUE(write_all(replacement, welcome(4)));
    for (int i = 0; i < 4; ++i) {
      pollfd p {ctx->fd(), ctx->poll_events(), 0};
      poll(&p, 1, 20);
      ASSERT_TRUE(ctx->drive(p.revents, private_seat::client::clock::now()));
    }
    EXPECT_FALSE(callback_called);
    EXPECT_FALSE(ctx->welcome_cleanup_ack());
    ctx->reset_complete(reset_generation);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!callback_called && std::chrono::steady_clock::now() < deadline) {
      pollfd p {ctx->fd(), ctx->poll_events(), 0};
      poll(&p, 1, 20);
      if (!ctx->drive(p.revents, private_seat::client::clock::now())) {
        break;
      }
    }
    EXPECT_TRUE(callback_called);
    EXPECT_TRUE(callback_ok);
    EXPECT_TRUE(ctx->welcome_cleanup_ack());
    close(replacement);
  }

  TEST_F(GateTest, OldPostedOperationCannotRetireReplacement) {
    ASSERT_TRUE(activate());
    auto old = ctx->generation();
    ASSERT_TRUE(ctx->post([](psi::context &c) {
      c.reset();
    }));
    ctx->reset();
    ctx->reset_complete(old);
    ASSERT_TRUE(connect_and_welcome(4));
    auto replacement = ctx->generation();
    ASSERT_NE(old, replacement);
    ASSERT_TRUE(dispatch_one());
    EXPECT_EQ(ctx->generation(), replacement);
    EXPECT_TRUE(ctx->welcome_cleanup_ack());
    EXPECT_GE(ctx->fd(), 0);
    ctx->reset_complete(old);
    EXPECT_EQ(ctx->generation(), replacement);
    EXPECT_TRUE(ctx->welcome_cleanup_ack());
  }

  TEST(PrivateSeatKeyTracker, AliasAndSyntheticModifierProduceBalancedBoundaries) {
    psi::key_tracker tracker;
    std::vector<std::pair<std::uint16_t, bool>> events;
    auto sink = [&events](std::uint16_t code, bool down) {
      events.emplace_back(code, down);
      return true;
    };
    ASSERT_TRUE(tracker.down(1, 30, {42}, sink));
    ASSERT_TRUE(tracker.down(2, 30, {}, sink));
    ASSERT_TRUE(tracker.up(1, sink));
    ASSERT_TRUE(tracker.up(2, sink));
    const std::vector<std::pair<std::uint16_t, bool>> expected {{42, true}, {30, true}, {42, false}, {30, false}};
    EXPECT_EQ(events, expected);
  }

  TEST(PrivateSeatTransition, MissingContextRetiresGateAndFailsClosed) {
    auto gate = std::make_shared<psi::capture_gate>();
    EXPECT_FALSE(psi::begin_capture_transition({}, gate).has_value());
    std::lock_guard<std::mutex> lock(gate->mutex);
    EXPECT_FALSE(gate->valid);
    EXPECT_FALSE(gate->latest.has_value());
  }
}  // namespace
