/**
 * @file test_hdr_capture_encode.cpp
 * @brief Opt-in synthetic Wayland capture through Sunshine's HEVC encoder.
 * @note The explicitly selected Wayland socket must expose exactly one output,
 *       occupied by the authorized fullscreen synthetic pattern.
 */
#ifdef __linux__

  #include "src/config.h"
  #include "src/globals.h"
  #include "src/platform/common.h"
  #include "src/video.h"

  #include <algorithm>
  #include <array>
  #include <cerrno>
  #include <chrono>
  #include <cmath>
  #include <condition_variable>
  #include <cstdlib>
  #include <cstring>
  #include <fcntl.h>
  #include <filesystem>
  #include <gtest/gtest.h>
  #include <iostream>
  #include <memory>
  #include <mutex>
  #include <signal.h>
  #include <spawn.h>
  #include <sstream>
  #include <string>
  #include <sys/stat.h>
  #include <sys/wait.h>
  #include <thread>
  #include <unistd.h>
  #include <vector>
  #include <wayland-client.h>

extern char **environ;

namespace {
  using namespace std::chrono_literals;

  /** @brief Read an environment variable without accepting an empty value. */
  const char *env(const char *name) {
    const char *value = std::getenv(name);
    return value && *value ? value : nullptr;
  }

  /** @brief Require the caller to opt into capture of the dedicated synthetic display. */
  bool opted_in() {
    const char *capture = env("SUNSHINE_HDR_CAPTURE_TEST");
    const char *synthetic = env("SUNSHINE_HDR_SYNTHETIC_ONLY");
    return capture && synthetic && std::strcmp(capture, "1") == 0 && std::strcmp(synthetic, "1") == 0;
  }

  /** @brief Validate explicit device, socket and exclusive output location before capture. */
  bool valid_environment() {
    const char *display = env("WAYLAND_DISPLAY");
    const char *runtime = env("XDG_RUNTIME_DIR");
    const char *node = env("SUNSHINE_HDR_TEST_RENDER_NODE");
    const char *output = env("HDR_PROBE_OUTPUT");
    if (std::getenv("WAYLAND_SOCKET") != nullptr || !display || !runtime || !node || !output) {
      return false;
    }
    const std::string expected_runtime = "/run/user/" + std::to_string(getuid());
    const std::filesystem::path qa_dir = std::filesystem::path(expected_runtime) / "lanternbox-hdr-qa";
    const std::filesystem::path private_dir = qa_dir / "auto";
    const bool normal_fixture = runtime == expected_runtime && std::strcmp(display, "lanternbox-0") == 0;
    const bool private_fixture = runtime == private_dir.string() && std::strcmp(display, "qa-outer") == 0;
    if (!normal_fixture && !private_fixture) {
      return false;
    }
    if (private_fixture) {
      struct stat qa_stat {}, private_stat {};
      if (lstat(qa_dir.c_str(), &qa_stat) != 0 || !S_ISDIR(qa_stat.st_mode) || qa_stat.st_uid != getuid() || lstat(private_dir.c_str(), &private_stat) != 0 || !S_ISDIR(private_stat.st_mode) || private_stat.st_uid != getuid()) {
        return false;
      }
      std::error_code error;
      const auto canonical_runtime = std::filesystem::canonical(expected_runtime, error);
      if (error) {
        return false;
      }
      const auto canonical_qa = std::filesystem::canonical(qa_dir, error);
      if (error || canonical_qa.parent_path() != canonical_runtime) {
        return false;
      }
      const auto canonical_private = std::filesystem::canonical(private_dir, error);
      if (error || canonical_private.parent_path() != canonical_qa) {
        return false;
      }
    }
    const std::string node_path = node;
    constexpr char prefix[] = "/dev/dri/renderD";
    if (node_path.compare(0, sizeof(prefix) - 1, prefix) != 0 || node_path.size() == sizeof(prefix) - 1 || node_path.find_first_not_of("0123456789", sizeof(prefix) - 1) != std::string::npos) {
      return false;
    }
    struct stat st {};
    if (lstat(node, &st) != 0 || !S_ISCHR(st.st_mode)) {
      return false;
    }
    const std::filesystem::path socket = std::filesystem::path(runtime) / display;
    if (lstat(socket.c_str(), &st) != 0 || !S_ISSOCK(st.st_mode) || (private_fixture && st.st_uid != getuid())) {
      return false;
    }
    const std::filesystem::path destination(output);
    return destination.is_absolute() && destination.parent_path() == std::filesystem::path(expected_runtime) / "lanternbox-hdr-qa" &&
           !destination.filename().empty() && destination.filename() != "." && destination.filename() != "..";
  }

  /** @brief Count only public registry globals; no output names or contents are read. */
  struct registry_counts {
    unsigned outputs = 0;
    unsigned hdr_managers = 0;
  };

  /** @brief Count output and Lanternbox HDR manager advertisements. */
  void registry_global(void *data, wl_registry *, uint32_t, const char *interface, uint32_t) {
    auto &counts = *static_cast<registry_counts *>(data);
    if (std::strcmp(interface, "wl_output") == 0) {
      ++counts.outputs;
    } else if (std::strcmp(interface, "lanternbox_capture_manager_v1") == 0) {
      ++counts.hdr_managers;
    }
  }

  /** @brief Ignore removal events during the initial registry enumeration. */
  void registry_remove(void *, wl_registry *, uint32_t) {}

  /** @brief Verify the explicit socket exposes only one output and the HDR manager. */
  bool valid_registry() {
    wl_display *display = wl_display_connect(env("WAYLAND_DISPLAY"));
    if (!display) {
      return false;
    }
    wl_registry *registry = wl_display_get_registry(display);
    if (!registry) {
      wl_display_disconnect(display);
      return false;
    }
    registry_counts counts;
    const wl_registry_listener listener {registry_global, registry_remove};
    wl_registry_add_listener(registry, &listener, &counts);
    const int result = wl_display_roundtrip(display);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return result >= 0 && counts.outputs == 1 && counts.hdr_managers == 1;
  }

  /** @brief Decode an IDR with the installed software FFmpeg and check synthetic PQ patches. */
  bool matches_pattern(video::packet_t &packet) {
    constexpr size_t frame_size = 1920u * 1080u * 3u;  // YUV420, 16 bits per sample
    if (!packet || !packet->data() || packet->data_size() == 0 || packet->data_size() > 4u * 1024u * 1024u) {
      return false;
    }
    int input[2];
    int output[2];
    if (pipe2(input, O_CLOEXEC) != 0) {
      return false;
    }
    if (pipe2(output, O_CLOEXEC) != 0) {
      close(input[0]);
      close(input[1]);
      return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, input[1]);
    posix_spawn_file_actions_addclose(&actions, output[0]);
    char executable[] = "/usr/bin/ffmpeg";
    char *args[] = {executable, const_cast<char *>("-nostdin"), const_cast<char *>("-v"), const_cast<char *>("error"), const_cast<char *>("-threads"), const_cast<char *>("1"), const_cast<char *>("-f"), const_cast<char *>("hevc"), const_cast<char *>("-i"), const_cast<char *>("pipe:0"), const_cast<char *>("-frames:v"), const_cast<char *>("1"), const_cast<char *>("-pix_fmt"), const_cast<char *>("+yuv420p10le"), const_cast<char *>("-strict"), const_cast<char *>("-1"), const_cast<char *>("-f"), const_cast<char *>("yuv4mpegpipe"), const_cast<char *>("pipe:1"), nullptr};
    pid_t decoder = -1;
    const int spawn_result = posix_spawn(&decoder, executable, &actions, nullptr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(input[0]);
    close(output[1]);
    if (spawn_result != 0) {
      close(input[1]);
      close(output[0]);
      return false;
    }
    bool wrote_all = false;
    std::thread writer([&] {
      size_t offset = 0;
      while (offset < packet->data_size()) {
        const ssize_t count = write(input[1], packet->data() + offset, packet->data_size() - offset);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          break;
        }
        offset += static_cast<size_t>(count);
      }
      wrote_all = offset == packet->data_size();
      close(input[1]);
    });
    std::vector<uint8_t> raw(frame_size + 4096);
    size_t size = 0;
    while (size < raw.size()) {
      const ssize_t count = read(output[0], raw.data() + size, raw.size() - size);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        break;
      }
      size += static_cast<size_t>(count);
    }
    close(output[0]);
    writer.join();
    int status = 0;
    const pid_t waited = waitpid(decoder, &status, 0);
    if (!wrote_all || waited != decoder || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      return false;
    }
    const auto header_end = std::find(raw.begin(), raw.begin() + size, '\n');
    if (header_end == raw.begin() + size) {
      return false;
    }
    std::string header(raw.begin(), header_end);
    std::istringstream fields(header);
    std::string field;
    bool width = false, height = false, format = false;
    if (!(fields >> field) || field != "YUV4MPEG2") {
      return false;
    }
    while (fields >> field) {
      width |= field == "W1920";
      height |= field == "H1080";
      format |= field == "C420p10";
    }
    const size_t header_size = static_cast<size_t>(header_end - raw.begin()) + 1;
    const auto frame_end = std::find(raw.begin() + header_size, raw.begin() + size, '\n');
    if (!width || !height || !format || frame_end == raw.begin() + size) {
      return false;
    }
    const std::string frame_header(raw.begin() + header_size, frame_end);
    if (frame_header != "FRAME" && frame_header.rfind("FRAME ", 0) != 0) {
      return false;
    }
    const size_t pixels = static_cast<size_t>(frame_end - raw.begin()) + 1;
    if (size - pixels != frame_size) {
      return false;
    }
    constexpr std::array<int, 4> expected {594, 668, 769, 923};
    for (size_t i = 0; i < expected.size(); ++i) {
      const size_t x = (2 * i + 1) * 1920 / 8;
      const size_t y = 1080 * 7 / 8;
      const size_t offset = (y * 1920 + x) * 2;
      const int sample = raw[pixels + offset] | (raw[pixels + offset + 1] << 8);
      if (std::abs(sample - expected[i]) > 32) {
        return false;
      }
    }
    return true;
  }

  /** @brief Decode the SDR synthetic pattern, or its 203-nit HDR rendering. */
  bool matches_sequence_pattern(video::packet_t &packet, bool hdr) {
    const size_t frame_size = 1920u * 1080u * 3u / 2u * (hdr ? 2u : 1u);
    if (!packet || !packet->data() || packet->data_size() == 0 || packet->data_size() > 4u * 1024u * 1024u) {
      return false;
    }
    int input[2];
    int output[2];
    if (pipe2(input, O_CLOEXEC) != 0) {
      return false;
    }
    if (pipe2(output, O_CLOEXEC) != 0) {
      close(input[0]);
      close(input[1]);
      return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, input[1]);
    posix_spawn_file_actions_addclose(&actions, output[0]);
    char executable[] = "/usr/bin/ffmpeg";
    char *args[] = {executable, const_cast<char *>("-nostdin"), const_cast<char *>("-v"), const_cast<char *>("error"), const_cast<char *>("-threads"), const_cast<char *>("1"), const_cast<char *>("-noauto_conversion_filters"), const_cast<char *>("-f"), const_cast<char *>("hevc"), const_cast<char *>("-i"), const_cast<char *>("pipe:0"), const_cast<char *>("-frames:v"), const_cast<char *>("1"), const_cast<char *>("-strict"), const_cast<char *>("-1"), const_cast<char *>("-f"), const_cast<char *>("yuv4mpegpipe"), const_cast<char *>("pipe:1"), nullptr};
    pid_t decoder = -1;
    const int spawn_result = posix_spawn(&decoder, executable, &actions, nullptr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(input[0]);
    close(output[1]);
    if (spawn_result != 0) {
      close(input[1]);
      close(output[0]);
      return false;
    }
    bool wrote_all = false;
    std::thread writer([&] {
      size_t offset = 0;
      while (offset < packet->data_size()) {
        const ssize_t count = write(input[1], packet->data() + offset, packet->data_size() - offset);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          break;
        }
        offset += static_cast<size_t>(count);
      }
      wrote_all = offset == packet->data_size();
      close(input[1]);
    });
    std::vector<uint8_t> raw(frame_size + 4096);
    size_t size = 0;
    while (size < raw.size()) {
      const ssize_t count = read(output[0], raw.data() + size, raw.size() - size);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        break;
      }
      size += static_cast<size_t>(count);
    }
    close(output[0]);
    writer.join();
    int status = 0;
    const pid_t waited = waitpid(decoder, &status, 0);
    if (!wrote_all || waited != decoder || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      std::cerr << "decode: expected_bits=" << (hdr ? 10 : 8)
                << " written=" << wrote_all << " waited=" << (waited == decoder)
                << " exit=" << (WIFEXITED(status) ? WEXITSTATUS(status) : -1)
                << " signal=" << (WIFSIGNALED(status) ? WTERMSIG(status) : 0) << std::endl;
      return false;
    }
    const auto header_end = std::find(raw.begin(), raw.begin() + size, '\n');
    if (header_end == raw.begin() + size) {
      return false;
    }
    std::string header(raw.begin(), header_end);
    std::istringstream fields(header);
    std::string field;
    bool width = false, height = false, format = false;
    int actual_bits = 0;
    if (!(fields >> field) || field != "YUV4MPEG2") {
      return false;
    }
    while (fields >> field) {
      width |= field == "W1920";
      height |= field == "H1080";
      if (field == "C420p10") {
        actual_bits = 10;
      }
      if (field == "C420jpeg" || field == "C420" || field == "C420mpeg2") {
        actual_bits = 8;
      }
      format = actual_bits == (hdr ? 10 : 8);
    }
    const size_t header_size = static_cast<size_t>(header_end - raw.begin()) + 1;
    const auto frame_end = std::find(raw.begin() + header_size, raw.begin() + size, '\n');
    if (!width || !height || !format || frame_end == raw.begin() + size) {
      std::cerr << "decode: actual_bits=" << actual_bits << " expected_bits=" << (hdr ? 10 : 8)
                << " width_ok=" << width << " height_ok=" << height
                << " frame_header=" << (frame_end != raw.begin() + size) << std::endl;
      return false;
    }
    const std::string frame_header(raw.begin() + header_size, frame_end);
    if (frame_header != "FRAME" && frame_header.rfind("FRAME ", 0) != 0) {
      return false;
    }
    const size_t pixels = static_cast<size_t>(frame_end - raw.begin()) + 1;
    if (size - pixels != frame_size) {
      std::cerr << "decode: bytes=" << (size - pixels) << " expected_bytes=" << frame_size << std::endl;
      return false;
    }
    auto luma = [&](size_t x, size_t y) {
      const size_t offset = pixels + (y * 1920 + x) * (hdr ? 2 : 1);
      return hdr ? (raw[offset] | (raw[offset + 1] << 8)) : raw[offset];
    };
    // The public SDR producer draws RGB/white quarters above a ramp and checker.
    const int red = luma(1920 / 8, 1080 / 4);
    const int green = luma(3 * 1920 / 8, 1080 / 4);
    const int blue = luma(5 * 1920 / 8, 1080 / 4);
    const int white = luma(7 * 1920 / 8, 1080 / 4);
    const int black = luma(0, 3 * 1080 / 4);
    // Full-range BT.601 coefficients for SDR, BT.2020 NCL for HDR.
    // HDR reference: convert BT.709 primaries to BT.2020 linear light,
    // apply ST 2084 at 203 cd/m², then form NCL Cb/Cr with 10-bit range.
    const size_t bytes = hdr ? 2 : 1;
    auto chroma = [&](size_t plane, size_t x) {
      const size_t index = (1080 / 8) * (1920 / 2) + x / 2;
      const size_t offset = pixels + (1920u * 1080u + plane * (1920u * 1080u / 4u) + index) * bytes;
      return hdr ? (raw[offset] | (raw[offset + 1] << 8)) : raw[offset];
    };
    const int ru = chroma(0, 1920 / 8), rv = chroma(1, 1920 / 8);
    const int gu = chroma(0, 3 * 1920 / 8), gv = chroma(1, 3 * 1920 / 8);
    const int bu = chroma(0, 5 * 1920 / 8), bv = chroma(1, 5 * 1920 / 8);
    const auto near = [](int actual, int expected, int tolerance) {
      return std::abs(actual - expected) <= tolerance;
    };
    const bool y_valid = hdr ? (near(white, 594, 35) && black < 100 && green > red + 15 && red > blue + 15 && white > green + 15) :
                               (white >= 235 && black <= 20 && red >= 35 && red <= 105 && green >= 125 && green <= 210 && blue >= 8 && blue <= 65 && green > red && red > blue);
    const bool chroma_valid = hdr ? (near(ru, 427, 45) && near(rv, 621, 45) && near(gu, 411, 45) && near(gv, 468, 45) && near(bu, 689, 45) && near(bv, 544, 45)) :
                                    (near(ru, 85, 20) && near(rv, 255, 20) && near(gu, 43, 20) && near(gv, 21, 20) && near(bu, 255, 20) && near(bv, 107, 20));
    if (!y_valid || !chroma_valid) {
      std::cerr << "pattern: bits=" << actual_bits << " y=" << red << "," << green << "," << blue << "," << white << "," << black
                << " red_uv=" << ru << "," << rv << " green_uv=" << gu << "," << gv << " blue_uv=" << bu << "," << bv
                << " expected_white=" << (hdr ? 594 : 255) << " chroma_tolerance=" << (hdr ? 45 : 20) << std::endl;
    }
    return y_valid && chroma_valid;
  }

  /** @brief Persist a single encoded packet without replacing an existing artifact. */
  bool save_packet(video::packet_t &packet) {
    const std::filesystem::path destination(env("HDR_PROBE_OUTPUT"));
    int dir = open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) {
      return false;
    }
    int fd = openat(dir, destination.filename().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    close(dir);
    if (fd < 0) {
      return false;
    }
    size_t offset = 0;
    while (offset < packet->data_size()) {
      const auto written = write(fd, packet->data() + offset, packet->data_size() - offset);
      if (written <= 0) {
        close(fd);
        return false;
      }
      offset += static_cast<size_t>(written);
    }
    return close(fd) == 0;
  }

  /** @brief Enforce a deadline even when a worker marker is inherited directly. */
  class worker_watchdog {
  public:
    explicit worker_watchdog(std::chrono::seconds timeout = 5s):
        thread([this, timeout] {
          std::unique_lock lock(mutex);
          if (!condition.wait_for(lock, timeout, [this] {
                return finished;
              })) {
            kill(-getpid(), SIGKILL);
            _exit(1);
          }
        }) {}

    ~worker_watchdog() {
      {
        std::lock_guard lock(mutex);
        finished = true;
      }
      condition.notify_one();
      thread.join();
    }

  private:
    std::mutex mutex;
    std::condition_variable condition;
    bool finished = false;
    std::thread thread;
  };

  /** @brief Run the public Sunshine capture and encoding pipeline inside the supervised process. */
  void capture_worker() {
    ASSERT_TRUE(valid_environment());
    ASSERT_TRUE(valid_registry()) << "Expected one output and the Lanternbox HDR manager";
    // Broken decoder pipes must fail the test instead of terminating the worker.
    signal(SIGPIPE, SIG_IGN);
    config::video.capture = "wlr";
    config::video.encoder = "vaapi";
    config::video.adapter_name = env("SUNSHINE_HDR_TEST_RENDER_NODE");
    config::video.output_name.clear();
    mail::man = std::make_shared<safe::mail_raw_t>();
    auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
    auto platform = platf::init();
    ASSERT_EQ(video::probe_encoders(), 0);

    video::config_t config {};
    config.width = 1920;
    config.height = 1080;
    config.framerate = 30;
    config.bitrate = 20000;
    config.slicesPerFrame = 1;
    config.numRefFrames = 1;
    config.videoFormat = 1;
    config.dynamicRange = 1;
    config.chromaSamplingType = 0;
    // Select full-range output through the public colorspace contract.
    config.encoderCscMode = 0;
    if (!video::colorspace_from_client_config(config, true).full_range) {
      config.encoderCscMode = 1;
    }
    ASSERT_TRUE(video::colorspace_from_client_config(config, true).full_range);
    auto session = std::make_shared<safe::mail_raw_t>();
    auto shutdown = session->event<bool>(mail::shutdown);
    auto hdr = session->event<video::hdr_info_t>(mail::hdr);
    auto idr = session->event<bool>(mail::idr);
    std::thread capture([&] {
      video::capture(session, config, nullptr);
    });

    video::packet_t packet;
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
      idr->raise(true);
      auto next = packets->pop(100ms);
      if (next && next->is_idr() && matches_pattern(next)) {
        packet = std::move(next);
        break;
      }
    }
    auto metadata = hdr->try_pop();
    shutdown->raise(true);
    capture.join();
    ASSERT_TRUE(packet) << "No IDR decoded to the synthetic HDR pattern";
    ASSERT_GT(packet->data_size(), 0u);
    ASSERT_LE(packet->data_size(), 4u * 1024u * 1024u);
    ASSERT_NE(packet->data(), nullptr);
    ASSERT_TRUE(metadata && metadata->enabled) << "No enabled HDR metadata message arrived";
    const auto &m = metadata->metadata;
    ASSERT_EQ(m.maxDisplayLuminance, 10000);
    std::cout << "HDR metadata: primaries=";
    for (const auto &primary : m.displayPrimaries) {
      std::cout << primary.x << "," << primary.y << " ";
    }
    std::cout << "white=" << m.whitePoint.x << "," << m.whitePoint.y
              << " max_display=" << m.maxDisplayLuminance << " min_display=" << m.minDisplayLuminance
              << " max_cll=" << m.maxContentLightLevel << " max_fall=" << m.maxFrameAverageLightLevel
              << " max_full_frame=" << m.maxFullFrameLuminance << std::endl;
    ::testing::Test::RecordProperty("hdr_metadata", "enabled");
    ASSERT_TRUE(save_packet(packet)) << "Could not exclusively create the packet output";
  }

  /** @brief Exclusively save one phase's packet in the designated QA directory. */
  bool save_sequence_packet(video::packet_t &packet, const std::filesystem::path &destination) {
    int dir = open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) {
      return false;
    }
    int fd = openat(dir, destination.filename().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    close(dir);
    if (fd < 0) {
      return false;
    }
    size_t offset = 0;
    while (offset < packet->data_size()) {
      const ssize_t n = write(fd, packet->data() + offset, packet->data_size() - offset);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        close(fd);
        return false;
      }
      offset += static_cast<size_t>(n);
    }
    return close(fd) == 0;
  }

  /** @brief Check encoded HEVC VUI using a separately supervised ffprobe process. */
  bool sequence_vui(const std::filesystem::path &path, bool hdr) {
    int out[2];
    if (pipe2(out, O_CLOEXEC) != 0) {
      return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    std::string filename = path.string();
    char executable[] = "/usr/bin/ffprobe";
    char *args[] = {executable, const_cast<char *>("-v"), const_cast<char *>("error"), const_cast<char *>("-f"), const_cast<char *>("hevc"), const_cast<char *>("-select_streams"), const_cast<char *>("v:0"), const_cast<char *>("-show_entries"), const_cast<char *>("stream=pix_fmt,color_range,color_space,color_transfer,color_primaries"), const_cast<char *>("-of"), const_cast<char *>("default=noprint_wrappers=1"), filename.data(), nullptr};
    pid_t child = -1;
    const int result = posix_spawn(&child, executable, &actions, nullptr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out[1]);
    if (result != 0) {
      close(out[0]);
      return false;
    }
    std::string fields;
    char buf[512];
    ssize_t n;
    while ((n = read(out[0], buf, sizeof(buf))) != 0) {
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      fields.append(buf, static_cast<size_t>(n));
      if (fields.size() > 4096) {
        break;
      }
    }
    close(out[0]);
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || fields.size() > 4096) {
      return false;
    }
    auto has = [&](const char *value) {
      return fields.find(std::string(value) + "\n") != std::string::npos;
    };
    if (!has("color_range=pc")) {
      return false;
    }
    if (hdr) {
      return has("pix_fmt=yuv420p10le") && has("color_space=bt2020nc") && has("color_transfer=smpte2084") && has("color_primaries=bt2020");
    }
    return (has("pix_fmt=yuv420p") || has("pix_fmt=yuvj420p")) &&
           has("color_space=smpte170m") && has("color_transfer=smpte170m") && has("color_primaries=smpte170m");
  }

  /** @brief Capture three independent sessions after a single platform and encoder probe. */
  void sequence_worker() {
    ASSERT_TRUE(valid_environment());
    ASSERT_TRUE(valid_registry());
    signal(SIGPIPE, SIG_IGN);
    config::video.capture = "wlr";
    config::video.encoder = "vaapi";
    config::video.adapter_name = env("SUNSHINE_HDR_TEST_RENDER_NODE");
    config::video.output_name.clear();
    mail::man = std::make_shared<safe::mail_raw_t>();
    auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
    auto platform = platf::init();
    ASSERT_EQ(video::probe_encoders(), 0);
    video::config_t config {};
    config.width = 1920;
    config.height = 1080;
    config.framerate = 30;
    config.bitrate = 20000;
    config.slicesPerFrame = 1;
    config.numRefFrames = 1;
    config.videoFormat = 1;
    config.chromaSamplingType = 0;
    config.encoderCscMode = 0;
    if (!video::colorspace_from_client_config(config, true).full_range) {
      config.encoderCscMode = 1;
    }
    ASSERT_TRUE(video::colorspace_from_client_config(config, true).full_range);
    ASSERT_EQ(video::colorspace_from_client_config(config, false).colorspace, video::colorspace_e::rec601);
    int channels[3] = {0, 1, 2};
    for (int phase = 0; phase < 3; ++phase) {
      const bool wants_hdr = phase == 1;
      config.dynamicRange = wants_hdr ? 1 : 0;
      // Dispose of packets left after the preceding capture thread joined.
      while (packets->pop(0ms)) {}
      auto session = std::make_shared<safe::mail_raw_t>();
      auto shutdown = session->event<bool>(mail::shutdown);
      auto hdr = session->event<video::hdr_info_t>(mail::hdr);
      auto idr = session->event<bool>(mail::idr);
      std::thread capture([&] {
        video::capture(session, config, &channels[phase]);
      });
      video::packet_t packet;
      bool wrong_channel = false;
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (std::chrono::steady_clock::now() < deadline) {
        idr->raise(true);
        auto next = packets->pop(100ms);
        if (next && next->channel_data != &channels[phase]) {
          wrong_channel = true;
          break;
        }
        if (next && next->is_idr() && matches_sequence_pattern(next, wants_hdr)) {
          packet = std::move(next);
          break;
        }
      }
      auto metadata = hdr->try_pop();
      shutdown->raise(true);
      capture.join();
      while (auto next = packets->pop(0ms)) {
        if (next->channel_data != &channels[phase]) {
          wrong_channel = true;
        }
      }
      ASSERT_FALSE(wrong_channel) << "phase " << phase << " received a packet from another channel";
      ASSERT_TRUE(packet) << "phase " << phase << " did not produce the decoded synthetic pattern";
      ASSERT_TRUE(metadata) << "phase " << phase << " did not emit HDR state";
      ASSERT_EQ(metadata->enabled, wants_hdr) << "phase " << phase;
      if (wants_hdr) {
        const auto &m = metadata->metadata;
        constexpr std::array<std::array<int, 2>, 3> rgb {{{35400, 14600}, {8500, 39850}, {6550, 2300}}};
        for (size_t i = 0; i < rgb.size(); ++i) {
          ASSERT_EQ(m.displayPrimaries[i].x, rgb[i][0]);
          ASSERT_EQ(m.displayPrimaries[i].y, rgb[i][1]);
        }
        ASSERT_EQ(m.whitePoint.x, 15635);
        ASSERT_EQ(m.whitePoint.y, 16450);
        ASSERT_EQ(m.minDisplayLuminance, 50);
        ASSERT_EQ(m.maxDisplayLuminance, 10000);
        ASSERT_EQ(m.maxContentLightLevel, 0);
        ASSERT_EQ(m.maxFrameAverageLightLevel, 0);
      }
      const std::filesystem::path path(std::string(env("HDR_PROBE_OUTPUT")) + "." + std::to_string(phase) + ".hevc");
      ASSERT_TRUE(save_sequence_packet(packet, path)) << "Could not exclusively create phase artifact";
      ASSERT_TRUE(sequence_vui(path, wants_hdr)) << "Wrong or missing bit depth/color VUI in phase " << phase;
      std::cout << "sequence phase=" << phase << " hdr=" << wants_hdr << " pattern and VUI passed" << std::endl;
    }
  }

  /** @brief Request HDR once and validate the unsupported-producer SDR fallback. */
  void fallback_worker() {
    ASSERT_TRUE(valid_environment());
    ASSERT_TRUE(valid_registry());
    signal(SIGPIPE, SIG_IGN);
    config::video.capture = "wlr";
    config::video.encoder = "vaapi";
    config::video.adapter_name = env("SUNSHINE_HDR_TEST_RENDER_NODE");
    config::video.output_name.clear();
    mail::man = std::make_shared<safe::mail_raw_t>();
    auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
    auto platform = platf::init();
    ASSERT_EQ(video::probe_encoders(), 0);
    video::config_t config {};
    config.width = 1920;
    config.height = 1080;
    config.framerate = 30;
    config.bitrate = 20000;
    config.slicesPerFrame = 1;
    config.numRefFrames = 1;
    config.videoFormat = 1;
    config.dynamicRange = 1;
    config.chromaSamplingType = 0;
    config.encoderCscMode = 0;
    if (!video::colorspace_from_client_config(config, true).full_range) {
      config.encoderCscMode = 1;
    }
    ASSERT_TRUE(video::colorspace_from_client_config(config, true).full_range);
    ASSERT_EQ(video::colorspace_from_client_config(config, false).colorspace, video::colorspace_e::rec601);
    auto session = std::make_shared<safe::mail_raw_t>();
    auto shutdown = session->event<bool>(mail::shutdown);
    auto hdr = session->event<video::hdr_info_t>(mail::hdr);
    auto idr = session->event<bool>(mail::idr);
    int channel = 0;
    std::thread capture([&] {
      video::capture(session, config, &channel);
    });
    video::packet_t packet;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      idr->raise(true);
      auto next = packets->pop(100ms);
      if (next && next->channel_data == &channel && next->is_idr() && matches_sequence_pattern(next, false)) {
        packet = std::move(next);
        break;
      }
    }
    auto metadata = hdr->try_pop();
    shutdown->raise(true);
    capture.join();
    ASSERT_TRUE(packet) << "HDR request did not yield a decoded synthetic SDR IDR";
    ASSERT_TRUE(metadata) << "No effective HDR state was reported";
    ASSERT_FALSE(metadata->enabled) << "HDR remained enabled despite unavailable producer";
    const std::filesystem::path path(std::string(env("HDR_PROBE_OUTPUT")) + ".fallback.hevc");
    ASSERT_TRUE(save_sequence_packet(packet, path)) << "Could not exclusively create fallback artifact";
    ASSERT_TRUE(sequence_vui(path, false)) << "Fallback packet is not SDR 8-bit with SDR VUI";
  }
}  // namespace

/** @brief Run capture only by explicit opt-in, with a process-level timeout. */
TEST(HdrCaptureEncode, SyntheticWaylandHevcMain10) {
  if (!opted_in()) {
    GTEST_SKIP() << "Synthetic HDR capture is opt-in";
  }
  ASSERT_TRUE(valid_environment()) << "Explicit synthetic display, render node and QA output are required";
  if (env("SUNSHINE_HDR_CAPTURE_WORKER")) {
    if (getpgrp() != getpid()) {
      ASSERT_EQ(setpgid(0, 0), 0);
    }
    ASSERT_EQ(getpgrp(), getpid());
    ASSERT_NE(getpgid(getppid()), getpgrp()) << "Worker must not share the parent process group";
    worker_watchdog watchdog;
    capture_worker();
    return;
  }

  struct stat existing {};
  ASSERT_EQ(lstat(env("HDR_PROBE_OUTPUT"), &existing), -1);
  ASSERT_EQ(errno, ENOENT) << "Output must not already exist";
  std::vector<std::string> entries;
  for (char **entry = environ; *entry; ++entry) {
    if (std::strncmp(*entry, "SUNSHINE_HDR_CAPTURE_WORKER=", 28) != 0 && std::strncmp(*entry, "GTEST_TOTAL_SHARDS=", 19) != 0 && std::strncmp(*entry, "GTEST_SHARD_INDEX=", 18) != 0 && std::strncmp(*entry, "GTEST_SHARD_STATUS_FILE=", 24) != 0) {
      entries.emplace_back(*entry);
    }
  }
  entries.emplace_back("SUNSHINE_HDR_CAPTURE_WORKER=1");
  std::vector<char *> child_env;
  for (auto &entry : entries) {
    child_env.push_back(entry.data());
  }
  child_env.push_back(nullptr);
  char program[] = "/proc/self/exe";
  char filter[] = "--gtest_filter=HdrCaptureEncode.SyntheticWaylandHevcMain10";
  char *argv[] = {program, filter, nullptr};
  pid_t child = -1;
  posix_spawnattr_t attributes;
  ASSERT_EQ(posix_spawnattr_init(&attributes), 0);
  ASSERT_EQ(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP), 0);
  ASSERT_EQ(posix_spawnattr_setpgroup(&attributes, 0), 0);
  const int spawn_result = posix_spawn(&child, program, nullptr, &attributes, argv, child_env.data());
  posix_spawnattr_destroy(&attributes);
  ASSERT_EQ(spawn_result, 0);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  int status = 0;
  pid_t result = 0;
  while (std::chrono::steady_clock::now() < deadline && (result = waitpid(child, &status, WNOHANG)) == 0) {
    std::this_thread::sleep_for(20ms);
  }
  if (result == 0) {
    kill(-child, SIGKILL);
    waitpid(child, &status, 0);
    FAIL() << "Capture worker exceeded five seconds";
  }
  ASSERT_EQ(result, child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
  int output = open(env("HDR_PROBE_OUTPUT"), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  ASSERT_GE(output, 0) << "Worker did not create a packet artifact";
  struct stat artifact {};
  const int stat_result = fstat(output, &artifact);
  close(output);
  ASSERT_EQ(stat_result, 0);
  ASSERT_TRUE(S_ISREG(artifact.st_mode));
  ASSERT_GT(artifact.st_size, 0);
  ASSERT_LE(artifact.st_size, 4 * 1024 * 1024);
}

/**
 * @brief Exercise SDR to HDR to SDR in one process on an isolated synthetic output.
 * @note Requires the opt-in patched canonical SDR KWin producer described in
 *       Lanternbox docs/hdr-session-v2.md, including KWIN_WAYLAND_CANONICAL_SDR=1.
 *       Start sdr-parity --producer-only on that nested socket before this test.
 *       The selected outer output must be 1920x1080 at scale 1 with no overlays.
 */
TEST(HdrCaptureEncode, SyntheticSessionSequence) {
  if (!opted_in()) {
    GTEST_SKIP() << "Synthetic HDR capture is opt-in";
  }
  ASSERT_TRUE(valid_environment()) << "Explicit synthetic display, render node and QA output are required";
  if (env("SUNSHINE_HDR_SEQUENCE_WORKER")) {
    if (getpgrp() != getpid()) {
      ASSERT_EQ(setpgid(0, 0), 0);
    }
    ASSERT_EQ(getpgrp(), getpid());
    ASSERT_NE(getpgid(getppid()), getpgrp());
    worker_watchdog watchdog(20s);
    sequence_worker();
    return;
  }
  for (int i = 0; i < 3; ++i) {
    const std::string path = std::string(env("HDR_PROBE_OUTPUT")) + "." + std::to_string(i) + ".hevc";
    struct stat st {};
    ASSERT_EQ(lstat(path.c_str(), &st), -1);
    ASSERT_EQ(errno, ENOENT) << "Phase artifact already exists";
  }
  std::vector<std::string> entries;
  for (char **entry = environ; *entry; ++entry) {
    if (std::strncmp(*entry, "SUNSHINE_HDR_SEQUENCE_WORKER=", 29) != 0 && std::strncmp(*entry, "GTEST_TOTAL_SHARDS=", 19) != 0 && std::strncmp(*entry, "GTEST_SHARD_INDEX=", 18) != 0 && std::strncmp(*entry, "GTEST_SHARD_STATUS_FILE=", 24) != 0) {
      entries.emplace_back(*entry);
    }
  }
  entries.emplace_back("SUNSHINE_HDR_SEQUENCE_WORKER=1");
  std::vector<char *> child_env;
  for (auto &entry : entries) {
    child_env.push_back(entry.data());
  }
  child_env.push_back(nullptr);
  char program[] = "/proc/self/exe";
  char filter[] = "--gtest_filter=HdrCaptureEncode.SyntheticSessionSequence";
  char *argv[] = {program, filter, nullptr};
  posix_spawnattr_t attr;
  ASSERT_EQ(posix_spawnattr_init(&attr), 0);
  ASSERT_EQ(posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP), 0);
  ASSERT_EQ(posix_spawnattr_setpgroup(&attr, 0), 0);
  pid_t child = -1;
  const int spawned = posix_spawn(&child, program, nullptr, &attr, argv, child_env.data());
  posix_spawnattr_destroy(&attr);
  ASSERT_EQ(spawned, 0);
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  int status = 0;
  pid_t result = 0;
  while (std::chrono::steady_clock::now() < deadline && (result = waitpid(child, &status, WNOHANG)) == 0) {
    std::this_thread::sleep_for(20ms);
  }
  if (result == 0) {
    kill(-child, SIGKILL);
    waitpid(child, &status, 0);
    FAIL() << "Sequence worker exceeded 20 seconds";
  }
  ASSERT_EQ(result, child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
}

/**
 * @brief Request HDR with an unmarked synthetic SDR producer and require SDR fallback.
 * @note Requires a private synthetic fixture with auto mode and no HDR-capable
 *       producer; SUNSHINE_HDR_FALLBACK_TEST=1 acknowledges this setup.
 *       First establish fallback with session-probe --synthetic-only
 *       --hdr-unsupported in the same isolated fixture.
 */
TEST(HdrCaptureEncode, UnavailableHdrUsesSdr) {
  const char *fallback = env("SUNSHINE_HDR_FALLBACK_TEST");
  if (!opted_in() || !fallback || std::strcmp(fallback, "1") != 0) {
    GTEST_SKIP() << "Synthetic unavailable-HDR fallback is opt-in";
  }
  ASSERT_TRUE(valid_environment());
  if (env("SUNSHINE_HDR_FALLBACK_WORKER")) {
    if (getpgrp() != getpid()) {
      ASSERT_EQ(setpgid(0, 0), 0);
    }
    ASSERT_EQ(getpgrp(), getpid());
    ASSERT_NE(getpgid(getppid()), getpgrp());
    worker_watchdog watchdog(20s);
    fallback_worker();
    return;
  }
  const std::string path = std::string(env("HDR_PROBE_OUTPUT")) + ".fallback.hevc";
  struct stat st {};
  ASSERT_EQ(lstat(path.c_str(), &st), -1);
  ASSERT_EQ(errno, ENOENT) << "Fallback artifact already exists";
  std::vector<std::string> entries;
  for (char **entry = environ; *entry; ++entry) {
    if (std::strncmp(*entry, "SUNSHINE_HDR_FALLBACK_WORKER=", 29) != 0 && std::strncmp(*entry, "GTEST_TOTAL_SHARDS=", 19) != 0 && std::strncmp(*entry, "GTEST_SHARD_INDEX=", 18) != 0 && std::strncmp(*entry, "GTEST_SHARD_STATUS_FILE=", 24) != 0) {
      entries.emplace_back(*entry);
    }
  }
  entries.emplace_back("SUNSHINE_HDR_FALLBACK_WORKER=1");
  std::vector<char *> child_env;
  for (auto &entry : entries) {
    child_env.push_back(entry.data());
  }
  child_env.push_back(nullptr);
  char program[] = "/proc/self/exe";
  char filter[] = "--gtest_filter=HdrCaptureEncode.UnavailableHdrUsesSdr";
  char *argv[] = {program, filter, nullptr};
  posix_spawnattr_t attr;
  ASSERT_EQ(posix_spawnattr_init(&attr), 0);
  ASSERT_EQ(posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP), 0);
  ASSERT_EQ(posix_spawnattr_setpgroup(&attr, 0), 0);
  pid_t child = -1;
  const int spawned = posix_spawn(&child, program, nullptr, &attr, argv, child_env.data());
  posix_spawnattr_destroy(&attr);
  ASSERT_EQ(spawned, 0);
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  int status = 0;
  pid_t result = 0;
  while (std::chrono::steady_clock::now() < deadline && (result = waitpid(child, &status, WNOHANG)) == 0) {
    std::this_thread::sleep_for(20ms);
  }
  if (result == 0) {
    kill(-child, SIGKILL);
    waitpid(child, &status, 0);
    FAIL() << "Fallback worker exceeded 20 seconds";
  }
  ASSERT_EQ(result, child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
}

#endif
