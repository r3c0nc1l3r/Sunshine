/**
 * @file src/platform/linux/wlgrab.cpp
 * @brief Definitions for wlgrab capture.
 */
// standard includes
#include <thread>

// local includes
#include "cuda.h"
#include "lanternbox_hdr.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"
#include "vaapi.h"
#include "wayland.h"
#include "wayland_stats.h"

using namespace std::literals;

namespace wl {
  static int env_width;
  static int env_height;

  bool use_vram_capture(platf::mem_type_e hwdevice_type) {
    if (hwdevice_type == platf::mem_type_e::vaapi) {
      return true;
    }

#ifdef SUNSHINE_BUILD_CUDA
    if (hwdevice_type == platf::mem_type_e::cuda) {
      return true;
    }
#endif

    return false;
  }

  /**
   * @brief Captured frame buffer shared between capture and encode stages.
   */
  struct img_t: public platf::img_t {
    /**
     * @brief Destroy the Wayland capture image.
     */
    ~img_t() override {
      delete[] data;
      data = nullptr;
    }
  };

  /**
   * @brief Wayland screencopy capture backend shared by RAM and VRAM paths.
   */
  class wlr_t: public platf::display_t {
  public:
    /**
     * @brief Initialize Wayland screencopy capture for the selected output.
     *
     * @param hwdevice_type Hardware device type requested for capture or encode.
     * @param display_name Display name.
     * @param config Configuration values to apply.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      // calculate frame interval we should capture at
      delay = ::video::capture_frame_interval(config);
      const AVRational fps = ::video::framerate_to_rational(config);
      if (fps.den != 1) {
        BOOST_LOG(info) << "[wlgrab] Requested frame rate [" << fps.num << "/" << fps.den << ", approx. " << av_q2d(fps) << " fps]";
      } else {
        BOOST_LOG(info) << "[wlgrab] Requested frame rate [" << fps.num << "fps]";
      }

      mem_type = hwdevice_type;

      if (display.init()) {
        return -1;
      }

      interface.listen(display.registry());

      display.roundtrip();

      if (!interface[wl::interface_t::XDG_OUTPUT]) {
        BOOST_LOG(error) << "[wlgrab] Missing Wayland wire for xdg_output"sv;
        return -1;
      }

      if (!interface[wl::interface_t::WLR_EXPORT_DMABUF]) {
        BOOST_LOG(error) << "[wlgrab] Missing Wayland wire for wlr-export-dmabuf"sv;
        return -1;
      }

      // Populate xdg_output info (name, viewport) for every monitor up
      // front so we can match by stable name in addition to index.
      for (auto &m : interface.monitors) {
        m->listen(interface.output_manager);
      }
      display.roundtrip();

      if (interface.monitors.empty()) {
        BOOST_LOG(error) << "[wlgrab] No Wayland output is available"sv;
        return -1;
      }

      auto monitor = interface.monitors[0].get();

      if (!display_name.empty()) {
        // Match by xdg_output name first (stable across hotplug, e.g.
        // "eDP-1", "HEADLESS-2"). Fall back to numeric index for
        // backward compatibility with existing configs.
        bool matched = false;
        for (auto &m : interface.monitors) {
          if (m->name == display_name) {
            monitor = m.get();
            matched = true;
            break;
          }
        }
        if (!matched) {
          auto streamedMonitor = util::from_view(display_name);
          if (streamedMonitor >= 0 && streamedMonitor < interface.monitors.size()) {
            monitor = interface.monitors[streamedMonitor].get();
          }
        }
      }

      output = monitor->output;

      if (config.dynamicRange > 0 && (interface.capture_manager || monitor->lanternbox)) {
#ifndef SUNSHINE_BUILD_VAAPI
        BOOST_LOG(error) << "[wlgrab] Lanternbox HDR requires a VAAPI-enabled build"sv;
        return -1;
#else
        if (hwdevice_type != platf::mem_type_e::vaapi || config.dynamicRange != 1 || config.chromaSamplingType != 0 || (config.videoFormat != 1 && config.videoFormat != 2)) {
          BOOST_LOG(error) << "[wlgrab] Lanternbox HDR requires VAAPI VRAM and 10-bit HEVC/AV1 4:2:0"sv;
          return -1;
        }
        if (!interface.capture_manager || !interface.dmabuf_interface || wl_proxy_get_version((wl_proxy *) interface.screencopy_manager) < 3) {
          BOOST_LOG(error) << "[wlgrab] Missing Lanternbox HDR contract manager, DMA-BUF or screencopy v3"sv;
          return -1;
        }

        /**
         * @brief Forward immutable contract events into the strict metadata collector.
         * @details Static callbacks reference display-owned state that outlives all capture requests.
         */
        static const lanternbox_capture_contract_v1_listener listener {
          .format = [](void *data, lanternbox_capture_contract_v1 *, uint32_t generation, uint32_t fourcc, uint32_t primaries, uint32_t transfer, uint32_t full_range) {
            static_cast<hdr10_metadata_t *>(data)->format({generation, fourcc, primaries, transfer, full_range});
          },
          .luminance = [](void *data, lanternbox_capture_contract_v1 *, uint32_t white, uint32_t minimum, uint32_t maximum, uint32_t cll, uint32_t fall) {
            static_cast<hdr10_metadata_t *>(data)->luminance({white, minimum, maximum, cll, fall});
          },
          .mastering_primaries = [](void *data, lanternbox_capture_contract_v1 *, uint32_t rx, uint32_t ry, uint32_t gx, uint32_t gy, uint32_t bx, uint32_t by, uint32_t wx, uint32_t wy) {
            static_cast<hdr10_metadata_t *>(data)->mastering_primaries({rx, ry, gx, gy, bx, by, wx, wy});
          },
          .done = [](void *data, lanternbox_capture_contract_v1 *) {
            static_cast<hdr10_metadata_t *>(data)->done();
          },
          .failed = [](void *data, lanternbox_capture_contract_v1 *, const char *) {
            static_cast<hdr10_metadata_t *>(data)->failed();
          },
        };
        hdr_contract.reset(lanternbox_capture_manager_v1_get_hdr10(interface.capture_manager.get(), output));
        if (!hdr_contract || lanternbox_capture_contract_v1_add_listener(hdr_contract.get(), &listener, &hdr10) < 0 || wl_display_roundtrip(display.get()) < 0 || !hdr10.valid()) {
          BOOST_LOG(error) << "[wlgrab] Lanternbox HDR contract failed, is incomplete, malformed or unsupported"sv;
          return -1;
        }
        dmabuf.required_fourcc = hdr10_metadata_t::fourcc;
#endif
      }

      offset_x = monitor->viewport.offset_x;
      offset_y = monitor->viewport.offset_y;
      width = monitor->viewport.width;
      height = monitor->viewport.height;

      this->env_width = ::wl::env_width;
      this->env_height = ::wl::env_height;

      this->logical_width = monitor->viewport.logical_width;
      this->logical_height = monitor->viewport.logical_height;

      int desktop_logical_width = 0;
      int desktop_logical_height = 0;
      for (auto &monitor_entry : interface.monitors) {
        auto output_monitor = monitor_entry.get();
        desktop_logical_width = std::max(desktop_logical_width, output_monitor->viewport.offset_x + output_monitor->viewport.logical_width);
        desktop_logical_height = std::max(desktop_logical_height, output_monitor->viewport.offset_y + output_monitor->viewport.logical_height);
      }

      this->env_logical_width = desktop_logical_width;
      this->env_logical_height = desktop_logical_height;

      BOOST_LOG(info) << "[wlgrab] Selected monitor ["sv << monitor->description << "] for streaming"sv;
      BOOST_LOG(debug) << "[wlgrab] Offset: "sv << offset_x << 'x' << offset_y;
      BOOST_LOG(debug) << "[wlgrab] Resolution: "sv << width << 'x' << height;
      BOOST_LOG(debug) << "[wlgrab] Logical Resolution: "sv << logical_width << 'x' << logical_height;
      BOOST_LOG(debug) << "[wlgrab] Desktop Resolution: "sv << env_width << 'x' << env_height;
      BOOST_LOG(debug) << "[wlgrab] Logical Desktop Resolution: "sv << env_logical_width << 'x' << env_logical_height;

      return 0;
    }

    /**
     * @brief Report HDR only after a validated contract on this capture connection.
     * @return True while the immutable HDR contract is held and valid.
     */
    bool is_hdr() override {
      return hdr_contract && hdr10.valid();
    }

    /**
     * @brief Export the negotiated ST2086 metadata without changing its units.
     * @param metadata Destination metadata; cleared when no valid contract exists.
     * @return True when negotiated HDR metadata is available.
     */
    bool get_hdr_metadata(SS_HDR_METADATA &metadata) override {
      if (!is_hdr()) {
        metadata = {};
        return false;
      }
      return hdr10.get_metadata(metadata);
    }

    /**
     * @brief Prevent an HDR source from being encoded as SDR or by an incompatible codec.
     * @param name FFmpeg codec requested by the encoder.
     * @param config Requested stream configuration.
     * @return True for the existing SDR path or a compatible HDR VAAPI codec.
     */
    bool is_codec_supported(std::string_view name, const ::video::config_t &config) override {
      return !hdr_contract || (is_hdr() && config.dynamicRange == 1 && config.chromaSamplingType == 0 && (name == "hevc_vaapi"sv || name == "av1_vaapi"sv));
    }

    /**
     * @brief Populate a fallback image when real capture data is unavailable.
     *
     * @param img Image or frame object to read from or populate.
     * @return Capture status reported to the streaming pipeline.
     */
    int dummy_img(platf::img_t *img) override {
      return 0;
    }

    /**
     * @brief Capture a display frame into the provided image object.
     *
     * @param pull_free_image_cb Callback that provides an available image buffer.
     * @param img_out Captured wlroots image returned to the streaming pipeline.
     * @param timeout Maximum time to wait for the operation.
     * @param cursor Cursor image or visibility state to composite.
     * @return Capture status reported to the streaming pipeline.
     */
    inline platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
      auto status = request_frame(timeout, cursor);

      // Reinitializing destroys the display and with it every capture buffer
      const auto counters = wl::stats::snapshot();
      if (wl::stats::over_cap(counters)) {
        BOOST_LOG(warning) << "[wayland] Too many live capture buffers, reinitializing capture: "sv << wl::stats::format(counters, platf::count_open_fds());
        return platf::capture_e::reinit;
      }

      if (stats_gate.due(std::chrono::steady_clock::now())) {
        BOOST_LOG(info) << "[wayland] capture buffers: "sv << wl::stats::format(counters, platf::count_open_fds());
      }

      return status;
    }

    /**
     * @brief Request a screencopy frame and wait for it.
     *
     * @param timeout Maximum time to wait for the frame.
     * @param cursor Whether the compositor should include the cursor.
     * @return Capture status reported to the streaming pipeline.
     */
    platf::capture_e request_frame(std::chrono::milliseconds timeout, bool cursor) {
      if (hdr_contract && !hdr10.valid()) {
        BOOST_LOG(error) << "[wlgrab] Lanternbox HDR contract is no longer valid"sv;
        return platf::capture_e::error;
      }
      auto to = std::chrono::steady_clock::now() + timeout;

      // Dispatch events until we get a new frame or the timeout expires
      dmabuf.listen(interface.screencopy_manager, interface.dmabuf_interface, &interface.supported_modifiers, output, cursor);
      do {
        auto remaining_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(to - std::chrono::steady_clock::now());
        if (remaining_time_ms.count() < 0 || !display.dispatch(remaining_time_ms)) {
          return platf::capture_e::timeout;
        }
      } while (dmabuf.status == dmabuf_t::WAITING);

      if (dmabuf.status == dmabuf_t::ERROR || (hdr_contract && !hdr10.valid())) {
        return platf::capture_e::error;
      }
      auto current_frame = dmabuf.current_frame;

      if (
        dmabuf.status == dmabuf_t::REINIT ||
        current_frame->sd.width != width ||
        current_frame->sd.height != height
      ) {
        return platf::capture_e::reinit;
      }

      return platf::capture_e::ok;
    }

    platf::mem_type_e mem_type;  ///< Mem type.

    std::chrono::nanoseconds delay;  ///< Delay before the timer task becomes eligible to run.

    wl::display_t display;  ///< Wayland display connection used for capture.
    interface_t interface;  ///< Wayland registry interfaces required by screencopy.
    hdr10_metadata_t hdr10;  ///< Validated metadata for this connection and selected output.
    util::safe_ptr<lanternbox_capture_contract_v1, lanternbox_capture_contract_v1_destroy> hdr_contract;  ///< Destroyed after pending captures and before the connection.
    dmabuf_t dmabuf;  ///< DMA-BUF feedback and format state advertised by the compositor.

    wl_output *output;  ///< Wayland output selected for capture.
    wl::stats::periodic_gate_t stats_gate {60s};  ///< Limits the capture buffer statistics log line to about once a minute.
  };

  /**
   * @brief Wayland screencopy backend that copies frames into system memory.
   */
  class wlr_ram_t: public wlr_t {
  public:
    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      auto next_frame = std::chrono::steady_clock::now();

      sleep_overshoot_logger.reset();

      while (true) {
        platf::handle_pacing(next_frame, delay, sleep_overshoot_logger);

        std::shared_ptr<platf::img_t> img_out;
        auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor);
        switch (status) {
          case platf::capture_e::reinit:
          case platf::capture_e::error:
          case platf::capture_e::interrupted:
            return status;
          case platf::capture_e::timeout:
            if (!push_captured_image_cb(std::move(img_out), false)) {
              return platf::capture_e::ok;
            }
            break;
          case platf::capture_e::ok:
            if (!push_captured_image_cb(std::move(img_out), true)) {
              return platf::capture_e::ok;
            }
            break;
          default:
            BOOST_LOG(error) << "[wlgrab] Unrecognized capture status ["sv << std::to_underlying(status) << ']';
            return status;
        }
      }

      return platf::capture_e::ok;
    }

    /**
     * @brief Capture a display frame into the provided image object.
     *
     * @param pull_free_image_cb Callback that provides an available image buffer.
     * @param img_out Captured wlroots image returned to the streaming pipeline.
     * @param timeout Maximum time to wait for the operation.
     * @param cursor Cursor image or visibility state to composite.
     * @return Capture status reported to the streaming pipeline.
     */
    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
      auto status = wlr_t::snapshot(pull_free_image_cb, img_out, timeout, cursor);
      if (status != platf::capture_e::ok) {
        return status;
      }

      auto current_frame = dmabuf.current_frame;

      auto rgb_opt = egl::import_source(egl_display.get(), current_frame->sd);

      if (!rgb_opt) {
        return platf::capture_e::reinit;
      }

      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }

      gl::ctx.BindTexture(GL_TEXTURE_2D, (*rgb_opt)->tex[0]);

      // Don't remove these lines, see https://github.com/LizardByte/Sunshine/issues/453
      int h;
      int w;
      gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
      gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
      BOOST_LOG(debug) << "[wlgrab] width and height: w "sv << w << " h "sv << h;

      gl::ctx.GetTextureSubImage((*rgb_opt)->tex[0], 0, 0, 0, 0, width, height, 1, GL_BGRA, GL_UNSIGNED_BYTE, img_out->height * img_out->row_pitch, img_out->data);
      gl::ctx.BindTexture(GL_TEXTURE_2D, 0);

      img_out->frame_timestamp = current_frame->frame_timestamp;

      return platf::capture_e::ok;
    }

    /**
     * @brief Initialize Wayland capture that copies frames into system memory.
     *
     * @param hwdevice_type Hardware device type requested for capture or encode.
     * @param display_name Display name.
     * @param config Configuration values to apply.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      if (wlr_t::init(hwdevice_type, display_name, config)) {
        return -1;
      }

      egl_display = egl::make_display(display.get());
      if (!egl_display) {
        return -1;
      }

      auto ctx_opt = egl::make_ctx(egl_display.get());
      if (!ctx_opt) {
        return -1;
      }

      ctx = std::move(*ctx_opt);

      return 0;
    }

    /**
     * @brief Create AVCodec encode device.
     *
     * @param pix_fmt Sunshine pixel format to convert or allocate for.
     * @return Constructed AVCodec encode device object.
     */
    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
      if (hdr_contract) {
        BOOST_LOG(error) << "[wlgrab] Lanternbox HDR forbids RAM conversion"sv;
        return nullptr;
      }
#ifdef SUNSHINE_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        return va::make_avcodec_encode_device(width, height, false);
      }
#endif

#ifdef SUNSHINE_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        return cuda::make_avcodec_encode_device(width, height, false);
      }
#endif

      return std::make_unique<platf::avcodec_encode_device_t>();
    }

    /**
     * @brief Allocate an image buffer compatible with this display backend.
     *
     * @return Allocated img object, or null when unavailable.
     */
    std::shared_ptr<platf::img_t> alloc_img() override {
      auto img = std::make_shared<img_t>();
      img->width = width;
      img->height = height;
      img->pixel_pitch = 4;
      img->row_pitch = img->pixel_pitch * width;
      img->data = new std::uint8_t[height * img->row_pitch];

      return img;
    }

    egl::display_t egl_display;  ///< EGL display.
    egl::ctx_t ctx;  ///< EGL context used for wlroots capture conversion.
  };

  /**
   * @brief Wayland screencopy backend that exports frames as GPU resources.
   */
  class wlr_vram_t: public wlr_t {
  public:
    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      auto next_frame = std::chrono::steady_clock::now();

      sleep_overshoot_logger.reset();

      while (true) {
        platf::handle_pacing(next_frame, delay, sleep_overshoot_logger);

        std::shared_ptr<platf::img_t> img_out;
        auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor);
        switch (status) {
          case platf::capture_e::reinit:
          case platf::capture_e::error:
          case platf::capture_e::interrupted:
            return status;
          case platf::capture_e::timeout:
            if (!push_captured_image_cb(std::move(img_out), false)) {
              return platf::capture_e::ok;
            }
            break;
          case platf::capture_e::ok:
            if (!push_captured_image_cb(std::move(img_out), true)) {
              return platf::capture_e::ok;
            }
            break;
          default:
            BOOST_LOG(error) << "[wlgrab] Unrecognized capture status ["sv << std::to_underlying(status) << ']';
            return status;
        }
      }

      return platf::capture_e::ok;
    }

    /**
     * @brief Capture a display frame into the provided image object.
     *
     * @param pull_free_image_cb Callback that provides an available image buffer.
     * @param img_out Captured wlroots image returned to the streaming pipeline.
     * @param timeout Maximum time to wait for the operation.
     * @param cursor Cursor image or visibility state to composite.
     * @return Capture status reported to the streaming pipeline.
     */
    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
      auto status = wlr_t::snapshot(pull_free_image_cb, img_out, timeout, cursor);
      if (status != platf::capture_e::ok) {
        return status;
      }

      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }
      auto img = (egl::img_descriptor_t *) img_out.get();
      img->reset();

      auto current_frame = dmabuf.current_frame;

      ++sequence;
      img->sequence = sequence;

      img->sd = current_frame->sd;
      img->frame_timestamp = current_frame->frame_timestamp;

      // Prevent dmabuf from closing the file descriptors.
      std::fill_n(current_frame->sd.fds, 4, -1);

      return platf::capture_e::ok;
    }

    /**
     * @brief Allocate an image buffer compatible with this display backend.
     *
     * @return Allocated img object, or null when unavailable.
     */
    std::shared_ptr<platf::img_t> alloc_img() override {
      auto img = std::make_shared<egl::img_descriptor_t>();

      img->width = width;
      img->height = height;
      img->sequence = 0;
      img->serial = std::numeric_limits<decltype(img->serial)>::max();
      img->data = nullptr;

      // File descriptors aren't open
      std::fill_n(img->sd.fds, 4, -1);

      return img;
    }

    /**
     * @brief Create AVCodec encode device.
     *
     * @param pix_fmt Sunshine pixel format to convert or allocate for.
     * @return Constructed AVCodec encode device object.
     */
    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        if (hdr_contract) {
          if (!is_hdr() || pix_fmt != platf::pix_fmt_e::p010) {
            BOOST_LOG(error) << "[wlgrab] Lanternbox HDR conversion requires a valid contract and P010"sv;
            return nullptr;
          }
          return va::make_hdr10_encode_device(width, height, 0, 0);
        }
        return va::make_avcodec_encode_device(width, height, 0, 0, true);
      }
#endif

      if (hdr_contract) {
        BOOST_LOG(error) << "[wlgrab] Lanternbox HDR forbids non-VAAPI conversion fallback"sv;
        return nullptr;
      }

#ifdef SUNSHINE_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        return cuda::make_avcodec_gl_encode_device(width, height, 0, 0);
      }
#endif

      return std::make_unique<platf::avcodec_encode_device_t>();
    }

    /**
     * @brief Populate a fallback image when real capture data is unavailable.
     *
     * @param img Image or frame object to read from or populate.
     * @return Capture status reported to the streaming pipeline.
     */
    int dummy_img(platf::img_t *img) override {
      // Empty images are recognized as dummies by the zero sequence number
      return 0;
    }

    std::uint64_t sequence {};  ///< Monotonic capture sequence assigned to Wayland frames.
  };

}  // namespace wl

namespace platf {
  /**
   * @brief Create a Wayland capture backend for the requested memory type.
   */
  std::shared_ptr<display_t> wl_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type != platf::mem_type_e::system && hwdevice_type != platf::mem_type_e::vaapi && hwdevice_type != platf::mem_type_e::cuda) {
      BOOST_LOG(error) << "[wlgrab] Could not initialize display with the given hw device type."sv;
      return nullptr;
    }

    if (wl::use_vram_capture(hwdevice_type)) {
      auto wlr = std::make_shared<wl::wlr_vram_t>();
      if (wlr->init(hwdevice_type, display_name, config)) {
        return nullptr;
      }

      return wlr;
    }

#ifndef SUNSHINE_BUILD_CUDA
    if (hwdevice_type == platf::mem_type_e::cuda) {
      BOOST_LOG(warning) << "This build does not include CUDA support. Falling back to GPU -> RAM -> GPU for NVENC."sv;
    }
#endif

    auto wlr = std::make_shared<wl::wlr_ram_t>();
    if (wlr->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return wlr;
  }

  /**
   * @brief Enumerate capture display names reported by the Wayland compositor.
   */
  std::vector<std::string> wl_display_names() {
    std::vector<std::string> display_names;

    wl::display_t display;
    if (display.init()) {
      return {};
    }

    wl::interface_t interface;
    interface.listen(display.registry());

    display.roundtrip();

    if (!interface[wl::interface_t::XDG_OUTPUT]) {
      BOOST_LOG(warning) << "[wlgrab] Missing Wayland wire for xdg_output"sv;
      return {};
    }

    if (!interface[wl::interface_t::WLR_EXPORT_DMABUF]) {
      BOOST_LOG(warning) << "[wlgrab] Missing Wayland wire for wlr-export-dmabuf"sv;
      return {};
    }

    wl::env_width = 0;
    wl::env_height = 0;

    for (auto &monitor : interface.monitors) {
      monitor->listen(interface.output_manager);
    }

    display.roundtrip();

    BOOST_LOG(info) << "[wlgrab] -------- Start of Wayland monitor list --------"sv;

    for (int x = 0; x < interface.monitors.size(); ++x) {
      auto monitor = interface.monitors[x].get();

      wl::env_width = std::max(wl::env_width, monitor->viewport.offset_x + monitor->viewport.width);
      wl::env_height = std::max(wl::env_height, monitor->viewport.offset_y + monitor->viewport.height);

      BOOST_LOG(info) << "[wlgrab] Monitor " << x << " is "sv << monitor->name << ": "sv << monitor->description;

      display_names.emplace_back(monitor->name.empty() ? std::to_string(x) : monitor->name);
    }

    BOOST_LOG(info) << "[wlgrab] --------- End of Wayland monitor list ---------"sv;

    return display_names;
  }

}  // namespace platf
