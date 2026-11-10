/**
 * @file test_private_seat_capture.cpp
 * @brief Opt-in paired synthetic wlr capture and private-seat integration test.
 */
#ifdef __linux__
  #include "../unit/platform/linux/private_seat_test_root.h"
  #include "src/config.h"
  #include "src/globals.h"
  #include "src/input.h"
  #include "src/platform/common.h"
  #include "src/platform/linux/private_seat_input.h"
  #include "src/video.h"
  #include "third-party/moonlight-common-c/src/Input.h"

  #include <algorithm>
  #include <atomic>
  #include <cerrno>
  #include <chrono>
  #include <cmath>
  #include <cstdlib>
  #include <cstring>
  #include <fcntl.h>
  #include <future>
  #include <gtest/gtest.h>
  #include <iostream>
  #include <limits.h>
  #include <linux/input-event-codes.h>
  #include <lizardbyte/common/env.h>
  #include <memory>
  #include <sstream>
  #include <string>
  #include <sys/stat.h>
  #include <thread>
  #include <unistd.h>
  #include <vector>

namespace {
  using namespace std::chrono_literals;
  namespace seat = platf::private_seat_input;
  constexpr char fixture_output[] = "LANTERN-1";

  bool env_is(const char *name, const char *value) {
    const char *actual = std::getenv(name);
    return actual && std::strcmp(actual, value) == 0;
  }

  bool canonical(const std::string &path, std::string &result) {
    char resolved[PATH_MAX];
    if (!::realpath(path.c_str(), resolved)) {
      return false;
    }
    result = resolved;
    return true;
  }

  bool fixture_socket(const std::string &path, uid_t owner) {
    struct stat st {};
    std::string resolved;
    return ::lstat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) &&
           st.st_uid == owner && canonical(path, resolved) && resolved == path;
  }

  // Wire encoding follows the existing public packet fixture. No seat-protocol
  // messages or capture evidence are synthesized here.
  using Bytes = std::vector<std::uint8_t>;

  void le16(Bytes &b, std::uint16_t n) {
    b.push_back(n);
    b.push_back(n >> 8);
  }

  void le32(Bytes &b, std::uint32_t n) {
    for (int i = 0; i < 4; ++i) {
      b.push_back(n >> (8 * i));
    }
  }

  void be16(Bytes &b, int n) {
    auto v = static_cast<std::uint16_t>(n);
    b.push_back(v >> 8);
    b.push_back(v);
  }

  Bytes packet(std::uint32_t magic, std::size_t payload) {
    Bytes b;
    const auto size = static_cast<std::uint32_t>(payload + 4);
    b.push_back(size >> 24);
    b.push_back(size >> 16);
    b.push_back(size >> 8);
    b.push_back(size);
    le32(b, magic);
    return b;
  }

  Bytes key(std::uint16_t vk, bool down) {
    auto b = packet(down ? KEY_DOWN_EVENT_MAGIC : KEY_UP_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET) - 8);
    b.push_back(0);
    le16(b, vk);
    b.push_back(0);
    le16(b, 0);
    return b;
  }

  Bytes relative(int x, int y) {
    auto b = packet(MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET) - 8);
    be16(b, x);
    be16(b, y);
    return b;
  }

  Bytes absolute(int x, int y) {
    auto b = packet(MOUSE_MOVE_ABS_MAGIC, sizeof(NV_ABS_MOUSE_MOVE_PACKET) - 8);
    be16(b, x);
    be16(b, y);
    be16(b, 0);
    be16(b, 2559);
    be16(b, 1439);
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

  // Read only complete numeric lines, incrementally, from the already-open file.
  // Per-event deadlines and the external process-group watchdog bound all waits.
  class EventLog {
    int fd = -1;
    off_t offset = 0;
    std::string pending;

  public:
    ~EventLog() {
      if (fd >= 0) {
        ::close(fd);
      }
    }

    bool open_fixture(const std::string &root) {
      const char *path = std::getenv("SUNSHINE_PRIVATE_SEAT_EVENT_LOG");
      std::string resolved;
      struct stat st {}, lst {};
      if (!path || path[0] != '/' || !canonical(path, resolved) || resolved != path || resolved == root || !private_seat_test::within(resolved, root) || ::lstat(path, &lst) || !S_ISREG(lst.st_mode) || (lst.st_uid != 0 && lst.st_uid != 1002) || lst.st_nlink != 1) {
        return false;
      }
      fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
      if (fd < 0 || ::fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_dev != lst.st_dev || st.st_ino != lst.st_ino || st.st_uid != lst.st_uid) {
        return false;
      }
      offset = st.st_size;  // Ignore startup and pre-existing events.
      return true;
    }

    bool wait_releases() {
      bool key_up = false, button_up = false;
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (std::chrono::steady_clock::now() < deadline) {
        char buf[4096];
        auto n = ::pread(fd, buf, sizeof(buf), offset);
        if (n < 0 && errno != EINTR) {
          return false;
        }
        if (n > 0) {
          offset += n;
          pending.append(buf, static_cast<std::size_t>(n));
        }
        if (pending.size() > 65536) {
          return false;
        }
        std::size_t end;
        while ((end = pending.find('\n')) != std::string::npos) {
          std::string line = pending.substr(0, end);
          pending.erase(0, end + 1);
          std::istringstream in(line);
          std::vector<double> v;
          double x;
          while (in >> x) {
            if (!std::isfinite(x)) {
              return false;
            }
            v.push_back(x);
          }
          if (!in.eof() || v.empty()) {
            return false;
          }
          if (v == std::vector<double> {1, KEY_B, 0}) {
            key_up = true;
          }
          if (v == std::vector<double> {5, BTN_RIGHT, 0}) {
            button_up = true;
          }
        }
        if (key_up && button_up) {
          return true;
        }
        std::this_thread::sleep_for(20ms);
      }
      return false;
    }

    bool wait_for(std::initializer_list<double> expected, double tolerance = 0.0) {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (std::chrono::steady_clock::now() < deadline) {
        char buf[4096];
        const auto n = ::pread(fd, buf, sizeof(buf), offset);
        if (n < 0 && errno != EINTR) {
          return false;
        }
        if (n > 0) {
          offset += n;
          pending.append(buf, static_cast<std::size_t>(n));
        }
        if (pending.size() > 65536) {
          return false;
        }
        std::size_t end;
        while ((end = pending.find('\n')) != std::string::npos) {
          std::string line = pending.substr(0, end);
          pending.erase(0, end + 1);
          std::istringstream in(line);
          std::vector<double> values;
          double v;
          while (in >> v) {
            if (!std::isfinite(v)) {
              return false;
            }
            values.push_back(v);
          }
          if (!in.eof() || values.empty()) {
            return false;
          }
          if (values.size() != expected.size()) {
            continue;
          }
          bool matches = true;
          std::size_t i = 0;
          for (double want : expected) {
            if (std::abs(values[i++] - want) > tolerance) {
              matches = false;
            }
          }
          if (matches) {
            return true;
          }
        }
        std::this_thread::sleep_for(20ms);
      }
      return false;
    }
  };

  /** @brief Poll dispatcher-owned state without accessing it from the capture thread. */
  bool query_active(const std::weak_ptr<seat::context> &weak, bool &active) {
    auto ctx = weak.lock();
    auto promise = std::make_shared<std::promise<bool>>();
    auto result = promise->get_future();
    if (!ctx || !ctx->post_transition([promise](seat::context &c) {
          promise->set_value(c.active());
        })) {
      return false;
    }
    if (result.wait_for(500ms) != std::future_status::ready) {
      return false;
    }
    active = result.get();
    return true;
  }

  /** @brief Runs only against an explicitly named, synthetic local compositor. */
  class PrivateSeatCapture: public ::testing::Test {
  protected:
    config::input_t saved_input = config::input;
    config::video_t saved_video = config::video;
    safe::mail_t saved_man = mail::man;
    safe::mail_t stream_mail;
    std::shared_ptr<input::input_t> stream;
    std::unique_ptr<platf::deinit_t> platform_initialized;
    std::unique_ptr<platf::deinit_t> initialized;
    std::thread capture_thread;
    std::atomic<bool> stop {false};
    std::atomic<bool> finished {false};
    std::atomic<unsigned> captured {0};
    std::atomic<unsigned> published {0};
    std::atomic<bool> bad_image {false};
    std::atomic<bool> allocation_failed {false};
    bool display_created = false;
    bool cleanup_ok = false;
    platf::capture_e result = platf::capture_e::error;
    bool started_pool = false;
    EventLog events;

    void SetUp() override {
      if (!env_is("SUNSHINE_PRIVATE_SEAT_CAPTURE_TEST", "1") || !env_is("SYNTHETIC_ONLY", "1")) {
        GTEST_SKIP() << "both synthetic opt-ins required";
      }
      ASSERT_EQ(::geteuid(), 0u) << "fixture expects synthetic producer UID 0";
      ASSERT_TRUE(env_is("WAYLAND_DISPLAY", "qa-private"));
      ASSERT_EQ(std::getenv("WAYLAND_SOCKET"), nullptr);
      ASSERT_EQ(std::getenv("DISPLAY"), nullptr);
      std::string test_root, tmp;
      ASSERT_TRUE(private_seat_test::scratch_paths(test_root, tmp)) << "invalid private-seat scratch environment";
      ASSERT_TRUE(events.open_fixture(test_root)) << "invalid synthetic event log";
      const char *runtime_env = std::getenv("XDG_RUNTIME_DIR");
      ASSERT_NE(runtime_env, nullptr);
      std::string runtime;
      ASSERT_TRUE(canonical(runtime_env, runtime));
      ASSERT_EQ(runtime, runtime_env) << "runtime must be canonical, without symlinks";
      ASSERT_TRUE(runtime != test_root && private_seat_test::within(runtime, test_root));
      struct stat runtime_stat {}, node_stat {};
      ASSERT_EQ(::stat(runtime.c_str(), &runtime_stat), 0);
      ASSERT_TRUE(S_ISDIR(runtime_stat.st_mode));
      ASSERT_EQ(runtime_stat.st_uid, 1002u);
      ASSERT_TRUE(fixture_socket(runtime + "/qa-private", 1002));
      ASSERT_TRUE(fixture_socket(runtime + "/private-seat/seat.sock", 1002));
      ASSERT_EQ(::stat("/dev/dri/renderD128", &node_stat), 0);
      ASSERT_TRUE(S_ISCHR(node_stat.st_mode));
      ASSERT_FALSE(task_pool.running()) << "run the single filter in a fresh process";

      config::input.backend = "lanternbox-seat";
      config::input.lanternbox_input_socket = runtime + "/private-seat/seat.sock";
      config::input.lanternbox_compositor_uid = 1002;
      config::input.keyboard = true;
      config::input.mouse = true;
      config::input.controller = false;
      config::video.capture = "wlr";
      config::video.adapter_name = "/dev/dri/renderD128";
      config::video.output_name = fixture_output;
      mail::man = std::make_shared<safe::mail_raw_t>();

      // Compare the public helper with the already-validated process environment.
      // Report only booleans and counts; do not print paths or environment values.
      std::string helper_wayland, helper_runtime;
      const bool helper_wayland_present = lizardbyte::common::get_env("WAYLAND_DISPLAY", helper_wayland);
      const bool helper_runtime_present = lizardbyte::common::get_env("XDG_RUNTIME_DIR", helper_runtime);
      std::cout << "private-seat fixture: process_wayland_present=" << (std::getenv("WAYLAND_DISPLAY") != nullptr)
                << " helper_wayland_present=" << helper_wayland_present
                << " helper_wayland_matches=" << (helper_wayland == "qa-private")
                << " process_runtime_present=" << (runtime_env != nullptr)
                << " helper_runtime_present=" << helper_runtime_present
                << " helper_runtime_matches=" << (helper_runtime == runtime)
                << " wayland_socket_type=" << static_cast<unsigned>(S_IFSOCK)
                << " render_node_type=" << static_cast<unsigned>(node_stat.st_mode & S_IFMT)
                << " capture_wlr=" << (config::video.capture == "wlr") << std::endl;
      ASSERT_TRUE(helper_wayland_present && helper_wayland == "qa-private");
      ASSERT_TRUE(helper_runtime_present && helper_runtime == runtime);

      platform_initialized = platf::init();
      std::cout << "private-seat fixture: platform_initialized=" << static_cast<bool>(platform_initialized) << std::endl;
      ASSERT_TRUE(platform_initialized) << "platform initialization failed";
      ASSERT_TRUE(env_is("WAYLAND_DISPLAY", "qa-private"));
      ASSERT_TRUE(env_is("XDG_RUNTIME_DIR", runtime.c_str()));
      task_pool.start(1);
      started_pool = true;
      initialized = input::init();
      ASSERT_FALSE(input::private_seat_handle().expired());
      stream_mail = std::make_shared<safe::mail_raw_t>();
      stream = input::alloc(stream_mail, "synthetic-capture");
      ASSERT_TRUE(stream);
      ASSERT_TRUE(input::claim_private_stream(stream_mail));
    }

    void TearDown() override {
      stop = true;
      if (capture_thread.joinable()) {
        capture_thread.join();
      }
      if (stream) {
        input::reset(stream);
      }
      if (started_pool) {
        input::terminate_gamepads();
        initialized.reset();
        task_pool.stop();
        task_pool.join();
      }
      stream.reset();
      stream_mail.reset();
      platform_initialized.reset();
      mail::man = saved_man;
      config::input = saved_input;
      config::video = saved_video;
    }
  };

  TEST_F(PrivateSeatCapture, SyntheticOutput) {
    video::config_t cfg {};
    cfg.width = 2560;
    cfg.height = 1440;
    cfg.framerate = 60;
    cfg.private_seat_stream = true;
    const auto names = platf::display_names(platf::mem_type_e::system);
    const bool selected_output_present = std::find(names.begin(), names.end(), fixture_output) != names.end();
    std::cout << "private-seat fixture: output_count=" << names.size()
              << " fixture_output_present=" << selected_output_present << std::endl;
    ASSERT_EQ(names.size(), 1u) << "expected exactly one synthetic output";
    ASSERT_TRUE(selected_output_present) << "explicit fixture output LANTERN-1 is not advertised";
    auto weak = input::private_seat_handle();
    capture_thread = std::thread([this, cfg] {
      // EGL and every image remain owned and destroyed on this thread.
      auto display = platf::display(platf::mem_type_e::system, fixture_output, cfg);
      if (!display) {
        finished = true;
        return;
      }
      display_created = true;
      bool cursor = false;
      std::shared_ptr<platf::img_t> slot;
      result = display->capture(
        [this, &slot, &display](std::shared_ptr<platf::img_t> &&image, bool frame_captured) {
          if (stop) {
            return false;
          }
          if (!frame_captured) {
            return true;
          }
          if (!image || !image->data || image->width != 2560 || image->height != 1440 || image->pixel_pitch <= 0 || image->row_pitch <= 0) {
            bad_image = true;
            return false;
          }
          ++captured;
          if (display->publish_input_frame(*image, [&slot, &image] {
                slot = std::move(image);
                return true;
              })) {
            ++published;
          }
          slot.reset();
          return !stop;
        },
        [this, &display](std::shared_ptr<platf::img_t> &image) {
          // Let the push callback observe stop and end capture gracefully.
          image = display->alloc_img();
          if (!image) {
            allocation_failed = true;
          }
          return static_cast<bool>(image);
        },
        &cursor
      );
      slot.reset();
      cleanup_ok = display->prepare_input_transition();
      if (!cleanup_ok) {
        // The API requires retaining the display when the barrier fails. The
        // external process-group watchdog terminates this failed, stuck fixture.
        std::cerr << "private-seat fixture: input cleanup barrier failed; retaining display" << std::endl;
        std::promise<void> never;
        never.get_future().wait();
      }
      display.reset();
      finished = true;
    });

    bool active = false;
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline && !finished) {
      if (query_active(weak, active) && active && published > 0) {
        break;
      }
      std::this_thread::sleep_for(20ms);
    }
    EXPECT_TRUE(active) << "capture did not automatically qualify READY/ACTIVE";
    EXPECT_GT(captured.load(), 0u);
    EXPECT_GT(published.load(), 0u);
    EXPECT_FALSE(bad_image);
    EXPECT_FALSE(allocation_failed);
    ASSERT_TRUE(active && captured > 0 && published > 0 && !bad_image && !allocation_failed);
    auto send = [this](Bytes b) {
      input::passthrough(stream, std::move(b));
    };
    // Wait after each packet so a prior or coalesced event cannot satisfy a later assertion.
    send(absolute(640, 360));
    ASSERT_TRUE(events.wait_for({3, 640, 360}, 1.5)) << "absolute motion";
    send(relative(17, -9));
    ASSERT_TRUE(events.wait_for({4, 17, -9}, 0.01)) << "relative motion";
    send(key(0x41, true));
    ASSERT_TRUE(events.wait_for({1, KEY_A, 1})) << "A down";
    send(key(0x41, false));
    ASSERT_TRUE(events.wait_for({1, KEY_A, 0})) << "A up";
    send(button(1, true));
    ASSERT_TRUE(events.wait_for({5, BTN_LEFT, 1})) << "left down";
    send(button(1, false));
    ASSERT_TRUE(events.wait_for({5, BTN_LEFT, 0})) << "left up";
    send(wheel(120, false));
    ASSERT_TRUE(events.wait_for({8, 0, -120})) << "vertical positive packet";
    send(wheel(-120, false));
    ASSERT_TRUE(events.wait_for({8, 0, 120})) << "vertical negative packet";
    send(wheel(120, true));
    ASSERT_TRUE(events.wait_for({8, 1, 120})) << "horizontal positive packet";
    send(wheel(-120, true));
    ASSERT_TRUE(events.wait_for({8, 1, -120})) << "horizontal negative packet";
    send(key(0x42, true));
    ASSERT_TRUE(events.wait_for({1, KEY_B, 1})) << "held B down";
    send(button(3, true));
    ASSERT_TRUE(events.wait_for({5, BTN_RIGHT, 1})) << "held right down";
    stop = true;
    capture_thread.join();  // The external process-group timeout bounds a stuck backend.
    ASSERT_TRUE(display_created) << "could not create real wlr capture";
    EXPECT_FALSE(allocation_failed) << "capture image allocation failed";
    EXPECT_EQ(result, platf::capture_e::ok);
    ASSERT_TRUE(cleanup_ok);
    // Cleanup can emit either release first, so consume both in either order.
    // Bounded separate readers would discard the other release; record both together.
    EXPECT_TRUE(events.wait_releases()) << "held input was not released on capture teardown";
    bool after = true;
    ASSERT_TRUE(query_active(weak, after));
    EXPECT_FALSE(after) << "input remained active after capture cleanup";
  }
}  // namespace
#endif
