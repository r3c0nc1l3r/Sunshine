/**
 * @file src/platform/linux/wlgrab.cpp
 * @brief Definitions for wlgrab capture.
 */
// standard includes
#include <limits>
#include <thread>

// local includes
#include "cuda.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"
#include "vaapi.h"
#include "wayland.h"
#include "wayland_stats.h"
#ifndef __FreeBSD__
  #include "private_seat_input.h"
  #include "private_seat_wayland.h"
#endif

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
    /** @brief Retire private capture before the Wayland members are destroyed. */
    ~wlr_t() override {
      retire_private_capture();
    }

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

#ifndef __FreeBSD__
      if (config.private_seat_stream) {
        private_handle = input::private_seat_handle();
        transition = platf::private_seat_input::begin_capture_transition(private_handle, {});
        if (!transition) {
          BOOST_LOG(error) << "[wlgrab] Private-seat cleanup acknowledgement unavailable"sv;
          return -1;
        }
        private_started = true;
        capture_generation = platf::private_seat_input::allocate_capture_generation();
        if (!capture_generation || config.width <= 0 || config.height <= 0) {
          return -1;
        }
        gate = std::make_shared<platf::private_seat_input::capture_gate>();
        stream_width = config.width;
        stream_height = config.height;
      }
#endif
      if (display.init()) {
        return -1;
      }

#ifndef __FreeBSD__
      if (private_started) {
        interface.listen_capture(display.get());
      } else
#endif
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

#ifndef __FreeBSD__
      if (private_started && interface.monitors.size() != 1) {
        BOOST_LOG(error) << "[wlgrab] Private capture requires exactly one output"sv;
        return -1;
      }
#endif
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

#ifndef __FreeBSD__
      if (private_started) {
        private_seat::wayland_selection selection {monitor->output, monitor->name, monitor->global_id, {0, 0, static_cast<std::uint32_t>(monitor->viewport.width), static_cast<std::uint32_t>(monitor->viewport.height)}};
        std::string binding_error;
        binding = private_seat::wayland_binding::bind(display.get(), interface, selection, transition->peer, capture_generation, [weak = private_handle, owner_gate = gate, producer = transition->producer_generation](std::uint64_t, bool available) {
          if (available) {
            return;
          }
          owner_gate->invalidate();
          if (auto ctx = weak.lock()) {
            ctx->post([producer](auto &current) {
              if (current.generation() == producer) {
                current.reset();
              }
            });
          }
        },
                                                      binding_error);
        if (!binding) {
          BOOST_LOG(error) << "[wlgrab] Private output binding failed: "sv << binding_error;
          return -1;
        }
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!binding->snapshot() && !binding->invalid()) {
          auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
          if (remaining.count() <= 0 || !display.dispatch(remaining)) {
            break;
          }
        }
        auto snap = binding->snapshot();
        const auto &welcome = transition->welcome;
        if (!snap || binding->invalid() || snap->generation != capture_generation || snap->output_id != welcome.identity.output_id || snap->epoch != welcome.identity.epoch || snap->physical_width != welcome.physical_width || snap->physical_height != welcome.physical_height || snap->logical_width != welcome.logical_width || snap->logical_height != welcome.logical_height) {
          BOOST_LOG(error) << "[wlgrab] Private output snapshot does not match WELCOME"sv;
          return -1;
        }
        snapshot_proof.wayland_peer = snap->peer;
        snapshot_proof.producer_generation = transition->producer_generation;
        snapshot_proof.output_id = snap->output_id;
        snapshot_proof.epoch = snap->epoch;
        snapshot_proof.capture_generation = capture_generation;
        snapshot_proof.snapshot_sequence = 1;
        request_sequence = 1;
        snapshot_proof.physical_width = snap->physical_width;
        snapshot_proof.physical_height = snap->physical_height;
        snapshot_proof.logical_width = snap->logical_width;
        snapshot_proof.logical_height = snap->logical_height;
        snapshot_proof.stream_width = stream_width;
        snapshot_proof.stream_height = stream_height;
        snapshot_proof.sole_full_normal_origin = true;
      }
#endif
      output = monitor->output;

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
        invalidate_private_capture();
        return platf::capture_e::reinit;
      }

      if (stats_gate.due(std::chrono::steady_clock::now())) {
        BOOST_LOG(info) << "[wayland] capture buffers: "sv << wl::stats::format(counters, platf::count_open_fds());
      }

#ifndef __FreeBSD__
      if (private_started && status != platf::capture_e::ok) {
        invalidate_private_capture();
      }
#endif
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
      auto to = std::chrono::steady_clock::now() + timeout;

#ifndef __FreeBSD__
      if (private_started) {
        if (!binding || binding->invalid() || !binding->snapshot() || request_sequence == std::numeric_limits<std::uint64_t>::max()) {
          return platf::capture_e::reinit;
        }
        frame_proof = snapshot_proof;
        frame_proof.frame_output_id = snapshot_proof.output_id;
        frame_proof.frame_epoch = snapshot_proof.epoch;
        frame_proof.frame_capture_generation = capture_generation;
        frame_proof.frame_request_sequence = ++request_sequence;
      }
#endif
      // Dispatch events until we get a new frame or the timeout expires
      dmabuf.listen(interface.screencopy_manager, interface.dmabuf_interface, &interface.supported_modifiers, output, cursor);
      do {
        auto remaining_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(to - std::chrono::steady_clock::now());
        if (remaining_time_ms.count() < 0 || !display.dispatch(remaining_time_ms)) {
          return platf::capture_e::timeout;
        }
      } while (dmabuf.status == dmabuf_t::WAITING);

      auto current_frame = dmabuf.current_frame;

      if (
#ifndef __FreeBSD__
        (private_started && (!binding || binding->invalid() || !binding->snapshot() || binding->snapshot()->output_id != frame_proof.frame_output_id || binding->snapshot()->epoch != frame_proof.frame_epoch || binding->snapshot()->generation != frame_proof.frame_capture_generation)) ||
#endif
        dmabuf.status == dmabuf_t::REINIT ||
        current_frame->sd.width != width ||
        current_frame->sd.height != height
      ) {
        return platf::capture_e::reinit;
      }

#ifndef __FreeBSD__
      if (private_started) {
        frame_proof.frame_width = current_frame->sd.width;
        frame_proof.frame_height = current_frame->sd.height;
      }
#endif
      return platf::capture_e::ok;
    }

    /** @brief Retire the gate immediately and reset its producer asynchronously. */
    void invalidate_private_capture() {
#ifndef __FreeBSD__
      if (!private_started) {
        return;
      }
      if (gate) {
        gate->invalidate();
      }
      if (binding && !binding->invalid()) {
        binding->invalidate();
      } else if (auto ctx = private_handle.lock()) {
        const auto producer = transition->producer_generation;
        ctx->post([producer](auto &current) {
          if (current.generation() == producer) {
            current.reset();
          }
        });
      }
#endif
    }

    /**
     * @brief Close the gate and wait outside it for server-side cleanup before teardown.
     * @return True if cleanup is complete or unnecessary.
     */
    bool retire_private_capture() {
#ifndef __FreeBSD__
      if (!private_started || retired) {
        return true;
      }
      invalidate_private_capture();
      if (!qualification_posted) {
        // The initial WELCOME already acknowledged cleanup before display.init.
        // No READY could have been queued without a published frame, so this
        // inactive producer cannot own pressed state or a capture mapping.
        retired = true;
        return true;
      }
      auto replacement = platf::private_seat_input::begin_capture_transition(private_handle, gate);
      if (!replacement) {
        BOOST_LOG(error) << "[wlgrab] Private-seat teardown cleanup acknowledgement unavailable"sv;
        return false;
      } else if (auto ctx = private_handle.lock()) {
        const auto producer = replacement->producer_generation;
        ctx->post([producer](auto &current) {
          if (current.generation() == producer) {
            current.reset();
          }
        });
      }
      retired = true;
#endif
      return true;
    }

    /**
     * @brief Enforce the fallible cleanup barrier before the capture owner releases this display.
     * @return True if the cleanup barrier succeeds.
     */
    bool prepare_input_transition() override {
      return retire_private_capture();
    }

    /**
     * @brief Publish copied pixels into the encoder's bounded in-memory slot.
     * @param img Captured image associated with the publication.
     * @param publish Callback that publishes the image.
     * @return True if publication and any required qualification enqueue succeed.
     */
    bool publish_input_frame(const platf::img_t &img, const std::function<bool()> &publish) override {
#ifndef __FreeBSD__
      if (private_started) {
        bool enqueue_qualification = false;
        {
          std::lock_guard lock {gate->mutex};
          if (!gate->valid || !binding || binding->invalid() || !binding->snapshot() || img.width != frame_proof.frame_width || img.height != frame_proof.frame_height || frame_proof.frame_epoch != binding->snapshot()->epoch || frame_proof.frame_capture_generation != capture_generation || !publish()) {
            return false;
          }
          frame_proof.latest_published_sequence = frame_proof.frame_request_sequence;
          frame_proof.post_snapshot_frame_published = true;
          gate->latest = frame_proof;
          enqueue_qualification = !qualification_posted;
        }
        if (enqueue_qualification) {
          qualification_posted = platf::private_seat_input::post_capture_qualification(private_handle, gate);
          if (!qualification_posted) {
            invalidate_private_capture();
            return false;
          }
        }
        return true;
      }
#endif
      return publish();
    }

    /**
     * @brief Preserve the capture callback's continuation semantics.
     * @param push Callback accepting the captured image.
     * @param img Image to publish.
     * @return Timeout when the callback continues, or ok when it stops.
     */
    platf::capture_e publish_frame(const push_captured_image_cb_t &push, std::shared_ptr<platf::img_t> img) {
      if (push(std::move(img), true)) {
        return platf::capture_e::timeout;
      }
      invalidate_private_capture();
      return platf::capture_e::ok;
    }

    platf::mem_type_e mem_type;  ///< Mem type.

    std::chrono::nanoseconds delay;  ///< Delay before the timer task becomes eligible to run.

    wl::display_t display;  ///< Wayland display connection used for capture.
    interface_t interface;  ///< Wayland registry interfaces required by screencopy.
    dmabuf_t dmabuf;  ///< DMA-BUF feedback and format state advertised by the compositor.
#ifndef __FreeBSD__
    std::weak_ptr<platf::private_seat_input::context> private_handle;  ///< Dispatcher context.
    std::optional<platf::private_seat_input::capture_transition> transition;  ///< Fresh cleanup acknowledgement.
    std::shared_ptr<platf::private_seat_input::capture_gate> gate;  ///< Capture/input publication barrier.
    std::unique_ptr<private_seat::wayland_binding> binding;  ///< Destroyed before the interface and display.
    platf::private_seat_input::capture_proof snapshot_proof {};  ///< Immutable capture snapshot evidence.
    platf::private_seat_input::capture_proof frame_proof {};  ///< Current request evidence.
    std::uint64_t capture_generation = 0;  ///< Process-unique capture instance.
    std::uint64_t request_sequence = 0;  ///< Checked request counter.
    /** @var wl::wlr_t::stream_width
     * @brief Configured encoder width.
     */
    std::uint32_t stream_width = 0, stream_height = 0;  ///< Configured encoder height.
    bool private_started = false;  ///< Fresh producer was acquired.
    bool retired = false;  ///< Teardown has already run.
    bool qualification_posted = false;  ///< One bounded dispatcher qualification per capture instance.
#endif

    wl_output *output;  ///< Wayland output selected for capture.
    wl::stats::periodic_gate_t stats_gate {60s};  ///< Limits the capture buffer statistics log line to about once a minute.
  };

  /**
   * @brief Wayland screencopy backend that copies frames into system memory.
   */
  class wlr_ram_t: public wlr_t {
  public:
    /** @brief Retire private capture before derived resources are destroyed. */
    ~wlr_ram_t() override {
      retire_private_capture();
    }

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
            invalidate_private_capture();
            return status;
          case platf::capture_e::timeout:
#ifndef __FreeBSD__
            if (private_started) {
              return platf::capture_e::reinit;
            }
#endif
            if (!push_captured_image_cb(std::move(img_out), false)) {
              return platf::capture_e::ok;
            }
            break;
          case platf::capture_e::ok:
            {
              auto result = publish_frame(push_captured_image_cb, std::move(img_out));
              if (result != platf::capture_e::timeout) {
                return result;
              }
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
    /** @brief Retire private capture before derived resources are destroyed. */
    ~wlr_vram_t() override {
      retire_private_capture();
    }

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
            invalidate_private_capture();
            return status;
          case platf::capture_e::timeout:
#ifndef __FreeBSD__
            if (private_started) {
              return platf::capture_e::reinit;
            }
#endif
            if (!push_captured_image_cb(std::move(img_out), false)) {
              return platf::capture_e::ok;
            }
            break;
          case platf::capture_e::ok:
            {
              auto result = publish_frame(push_captured_image_cb, std::move(img_out));
              if (result != platf::capture_e::timeout) {
                return result;
              }
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
        return va::make_avcodec_encode_device(width, height, 0, 0, true);
      }
#endif

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
