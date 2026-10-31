/**
 * @file tests/unit/platform/linux/test_hdr_conversion_gpu.cpp
 * @brief Opt-in synthetic, selected-render-node HDR conversion tests.
 */
#if defined(__linux__) && defined(SUNSHINE_BUILD_DRM)
  #include "src/config.h"
  #include "src/platform/linux/graphics.h"
  #include "src/platform/linux/vaapi.h"

  #include <algorithm>
  #include <array>
  #include <cmath>
  #include <cstdint>
  #include <cstdlib>
  #include <cstring>
  #include <dlfcn.h>
  #include <fcntl.h>
  #include <functional>
  #include <gbm.h>
  #include <gtest/gtest.h>
  #include <memory>
  #include <set>
  #include <string>
  #include <unistd.h>

extern "C" {
  #include <libavutil/frame.h>
  #include <libavutil/hwcontext.h>
}

namespace {
  constexpr int width = 1024;
  constexpr int height = 128;

  /** @brief GBM functions loaded only after the explicit GPU opt-in. */
  struct gbm_api {
    void *lib = nullptr;
    decltype(&gbm_create_device) create = nullptr;
    decltype(&gbm_device_destroy) destroy_device = nullptr;
    decltype(&gbm_bo_create) create_bo = nullptr;
    decltype(&gbm_bo_destroy) destroy_bo = nullptr;
    decltype(&gbm_bo_map) map = nullptr;
    decltype(&gbm_bo_unmap) unmap = nullptr;
    decltype(&gbm_bo_get_fd) fd = nullptr;
    decltype(&gbm_bo_get_stride) stride = nullptr;
    decltype(&gbm_bo_get_modifier) modifier = nullptr;

    ~gbm_api() {
      if (lib) {
        dlclose(lib);
      }
    }

    bool load() {
      lib = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
      if (!lib) {
        return false;
      }
  #define LOAD(member, symbol) \
    member = reinterpret_cast<decltype(member)>(dlsym(lib, #symbol)); \
    if (!member) \
    return false
      LOAD(create, gbm_create_device);
      LOAD(destroy_device, gbm_device_destroy);
      LOAD(create_bo, gbm_bo_create);
      LOAD(destroy_bo, gbm_bo_destroy);
      LOAD(map, gbm_bo_map);
      LOAD(unmap, gbm_bo_unmap);
      LOAD(fd, gbm_bo_get_fd);
      LOAD(stride, gbm_bo_get_stride);
      LOAD(modifier, gbm_bo_get_modifier);
  #undef LOAD
      return true;
    }
  };

  /** @brief Own all resources allocated on the nominated render node. */
  class HdrGpu: public ::testing::Test {
  protected:
    gbm_api api;
    int card = -1;
    gbm_device *gbm = nullptr;
    gbm_bo *bo = nullptr;
    AVBufferRef *hw = nullptr;
    AVBufferRef *frames = nullptr;
    AVFrame *download = nullptr;
    std::unique_ptr<platf::avcodec_encode_device_t> device;
    egl::img_descriptor_t image {};
    std::string old_adapter;
    bool adapter_set = false;

    void SetUp() override {
      for (int &fd : image.sd.fds) {
        fd = -1;
      }
      const char *enabled = std::getenv("SUNSHINE_HDR_GPU_TEST");
      const char *node = std::getenv("SUNSHINE_HDR_TEST_RENDER_NODE");
      if (!enabled || std::strcmp(enabled, "1") != 0 || !node || !*node) {
        GTEST_SKIP() << "Set both SUNSHINE_HDR_GPU_TEST=1 and SUNSHINE_HDR_TEST_RENDER_NODE";
      }
      old_adapter = config::video.adapter_name;
      config::video.adapter_name = node;
      adapter_set = true;
      card = open(node, O_RDWR | O_CLOEXEC);
      ASSERT_GE(card, 0) << "Cannot open the explicitly supplied render node";
      ASSERT_NE(gladLoaderLoadEGL(EGL_NO_DISPLAY), 0);
      ASSERT_EQ(gbm::init(), 0);
      ASSERT_TRUE(api.load());
      gbm = api.create(card);
      ASSERT_NE(gbm, nullptr);
      int owned = dup(card);
      ASSERT_GE(owned, 0);
      device = va::make_hdr10_encode_device(width, height, file_t {owned}, 0, 0);
      ASSERT_NE(device, nullptr);
      ASSERT_EQ(va::initialize_hdr10_hwdevice(*device, &hw), 0);
      ASSERT_NE(hw, nullptr);
      frames = av_hwframe_ctx_alloc(hw);
      ASSERT_NE(frames, nullptr);
      auto *ctx = reinterpret_cast<AVHWFramesContext *>(frames->data);
      ctx->format = AV_PIX_FMT_VAAPI;
      ctx->sw_format = AV_PIX_FMT_P010LE;
      ctx->width = width;
      ctx->height = height;
      device->init_hwframes(ctx);
      ASSERT_EQ(av_hwframe_ctx_init(frames), 0);
      download = av_frame_alloc();
      ASSERT_NE(download, nullptr);
    }

    void TearDown() override {
      av_frame_free(&download);
      device.reset();
      av_buffer_unref(&frames);
      av_buffer_unref(&hw);
      if (bo) {
        api.destroy_bo(bo);
      }
      if (gbm) {
        api.destroy_device(gbm);
      }
      if (card >= 0) {
        close(card);
      }
      if (adapter_set) {
        config::video.adapter_name = old_adapter;
      }
    }

    void source(uint32_t format, const std::function<uint32_t(int, int)> &pixel) {
      bo = api.create_bo(gbm, width, height, format, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE);
      ASSERT_NE(bo, nullptr) << "Selected node cannot allocate the synthetic buffer";
      uint32_t map_stride = 0;
      void *map_data = nullptr;
      auto *mapped = static_cast<uint8_t *>(api.map(bo, 0, 0, width, height, GBM_BO_TRANSFER_WRITE, &map_stride, &map_data));
      ASSERT_NE(mapped, nullptr);
      for (int y = 0; y < height; ++y) {
        auto *row = reinterpret_cast<uint32_t *>(mapped + y * map_stride);
        for (int x = 0; x < width; ++x) {
          row[x] = pixel(x, y);
        }
      }
      api.unmap(bo, map_data);
      image.width = image.sd.width = width;
      image.height = image.sd.height = height;
      image.pixel_pitch = 4;
      image.row_pitch = api.stride(bo);
      image.sd.fourcc = format;
      image.sd.modifier = api.modifier(bo);
      image.sd.pitches[0] = api.stride(bo);
      image.sd.offsets[0] = 0;
      image.sd.fds[0] = api.fd(bo);
      ASSERT_GE(image.sd.fds[0], 0);
      image.sequence = 1;
    }

    void target(bool full) {
      device->colorspace = {video::colorspace_e::bt2020, full, 10};
      AVFrame *frame = av_frame_alloc();
      ASSERT_NE(frame, nullptr);
      if (av_hwframe_get_buffer(frames, frame, 0) < 0) {
        av_frame_free(&frame);
        FAIL() << "Unable to allocate VAAPI frame";
      }
      ASSERT_EQ(device->set_frame(frame, frames), 0);
      device->apply_colorspace();
    }

    void readback() {
      av_frame_unref(download);
      ASSERT_EQ(av_hwframe_transfer_data(download, device->frame, 0), 0);
      ASSERT_EQ(download->format, AV_PIX_FMT_P010LE);
      ASSERT_GE(download->width, width);
      ASSERT_GE(download->height, height);
    }

    uint16_t sample(int plane, int x, int y) const {
      const auto *row = reinterpret_cast<const uint16_t *>(download->data[plane] + y * download->linesize[plane]);
      return row[x];
    }

    void packed() const {
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          EXPECT_EQ(sample(0, x, y) & 63, 0);
        }
      }
      for (int y = 0; y < height / 2; ++y) {
        for (int x = 0; x < width; ++x) {
          EXPECT_EQ(sample(1, x, y) & 63, 0);
        }
      }
    }
  };

  /** @brief Pack logical RGB10 channels into DRM ABGR2101010. */
  uint32_t rgb(unsigned r, unsigned g, unsigned b) {
    return r | (g << 10) | (b << 20) | (3u << 30);
  }

  /** @brief Independent BT.2020 non-constant-luminance 10-bit reference. */
  std::array<int, 3> reference(unsigned r, unsigned g, unsigned b, bool full) {
    const double red = r / 1023.0, green = g / 1023.0, blue = b / 1023.0;
    const double y = 0.2627 * red + 0.6780 * green + 0.0593 * blue;
    const double cb = (blue - y) / (2.0 * (1.0 - 0.0593));
    const double cr = (red - y) / (2.0 * (1.0 - 0.2627));
    return {int(std::lround((full ? 0 : 64) + (full ? 1023 : 876) * y)), int(std::lround(512 + (full ? 1023 : 896) * cb)), int(std::lround(512 + (full ? 1023 : 896) * cr))};
  }
}  // namespace

/** @brief Solid neutral values and separate primaries agree with BT.2020 in both ranges. */
TEST_F(HdrGpu, SolidColorsAndPacking) {
  const std::array<std::array<unsigned, 3>, 6> colors {{{0, 0, 0}, {1023, 1023, 1023}, {512, 512, 512}, {1023, 0, 0}, {0, 1023, 0}, {0, 0, 1023}}};
  // Each color occupies a wide stripe so chroma sampling is checked away from boundaries.
  source(GBM_FORMAT_ABGR2101010, [&](int x, int) {
    auto c = colors[std::min(x / 160, 5)];
    return rgb(c[0], c[1], c[2]);
  });
  ASSERT_FALSE(HasFatalFailure());
  for (bool full : {false, true}) {
    SCOPED_TRACE(full);
    target(full);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_EQ(device->convert(image), 0);
    readback();
    ASSERT_FALSE(HasFatalFailure());
    packed();
    for (int i = 0; i < 6; ++i) {
      auto expected = reference(colors[i][0], colors[i][1], colors[i][2], full);
      int x = i * 160 + 80;
      EXPECT_NEAR(sample(0, x, 1) / 64, expected[0], 1);
      EXPECT_NEAR(sample(1, x, 0) / 64, expected[1], 1);
      EXPECT_NEAR(sample(1, x + 1, 0) / 64, expected[2], 1);
    }
    EXPECT_EQ(sample(0, 80, 1), full ? 0 : 4096);
    EXPECT_EQ(sample(0, 240, 1), full ? 65472 : 60160);
    EXPECT_EQ(sample(1, 80, 0), 32768);
    EXPECT_EQ(sample(1, 81, 0), 32768);
  }
}

/** @brief A 10-bit gray ramp retains more than 8 bits of distinct luma. */
TEST_F(HdrGpu, GrayRampRetainsTenBitPrecision) {
  source(GBM_FORMAT_ABGR2101010, [](int x, int) {
    return rgb(x, x, x);
  });
  ASSERT_FALSE(HasFatalFailure());
  for (bool full : {false, true}) {
    target(full);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_EQ(device->convert(image), 0);
    readback();
    ASSERT_FALSE(HasFatalFailure());
    packed();
    std::set<uint16_t> levels;
    for (int x = 0; x < width; ++x) {
      levels.insert(sample(0, x, 1));
    }
    EXPECT_GT(levels.size(), 256u);
  }
}

/** @brief RAM and RGB8 input, and a non-P010 target, are rejected. */
TEST_F(HdrGpu, RejectsUnsupportedFormats) {
  target(false);
  ASSERT_FALSE(HasFatalFailure());
  platf::img_t ram;
  std::array<uint8_t, 16> pixels {};
  ram.data = pixels.data();
  ram.width = ram.height = 2;
  ram.pixel_pitch = 4;
  ram.row_pitch = 8;
  EXPECT_NE(device->convert(ram), 0);
  source(GBM_FORMAT_ABGR8888, [](int, int) {
    return 0xffffffffu;
  });
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_NE(device->convert(image), 0);
  const auto unref = [](AVBufferRef *ref) {
    av_buffer_unref(&ref);
  };
  std::unique_ptr<AVBufferRef, decltype(unref)> bad_frames(av_hwframe_ctx_alloc(hw), unref);
  ASSERT_NE(bad_frames, nullptr);
  auto *ctx = reinterpret_cast<AVHWFramesContext *>(bad_frames->data);
  ctx->format = AV_PIX_FMT_VAAPI;
  ctx->sw_format = AV_PIX_FMT_NV12;
  ctx->width = width;
  ctx->height = height;
  ASSERT_EQ(av_hwframe_ctx_init(bad_frames.get()), 0);
  AVFrame *bad = av_frame_alloc();
  ASSERT_NE(bad, nullptr);
  if (av_hwframe_get_buffer(bad_frames.get(), bad, 0) < 0) {
    av_frame_free(&bad);
    FAIL() << "Unable to allocate unsupported NV12 VAAPI frame";
  }
  EXPECT_NE(device->set_frame(bad, bad_frames.get()), 0);  // Ownership transfers even on failure.
}
#endif
