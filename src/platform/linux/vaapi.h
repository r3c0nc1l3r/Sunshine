/**
 * @file src/platform/linux/vaapi.h
 * @brief Declarations for VA-API hardware accelerated capture.
 */
#pragma once

// local includes
#include "misc.h"
#include "src/platform/common.h"

namespace egl {
  struct surface_descriptor_t;
}

namespace va {
  /**
   * @brief Create an opt-in VRAM converter for full-range BT.2020/PQ ABGR2101010 to P010.
   * @param width Source crop width.
   * @param height Source crop height.
   * @param offset_x Source crop X offset.
   * @param offset_y Source crop Y offset.
   * @return HDR-only encode device, or null on initialization failure.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_hdr10_encode_device(int width, int height, int offset_x, int offset_y);

  /**
   * @brief Create the HDR converter using only an explicitly supplied render device.
   * @param width Source crop width.
   * @param height Source crop height.
   * @param card Owned render-node descriptor; no adapter discovery or fallback is performed.
   * @param offset_x Source crop X offset.
   * @param offset_y Source crop Y offset.
   * @return HDR-only encode device, or null on initialization failure.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_hdr10_encode_device(int width, int height, file_t &&card, int offset_x, int offset_y);

  /**
   * @brief Initialize a VAAPI hardware context on the HDR converter's selected descriptor.
   * @param device HDR device returned by make_hdr10_encode_device; it must outlive its frames.
   * @param context Empty output reference, receiving an owned AVBufferRef on success.
   * @return Zero on success; negative on invalid arguments or VAAPI initialization failure.
   * @details Use av_hwframe_ctx_alloc on the returned buffer with VAAPI/P010; call init_hwframes before
   * av_hwframe_ctx_init, then allocate a VAAPI frame and transfer its ownership with set_frame.
   * The frame can be downloaded into a caller-owned P010 frame with av_hwframe_transfer_data.
   * Callers must not interpret the backend-specific data pointer.
   */
  int initialize_hdr10_hwdevice(platf::avcodec_encode_device_t &device, AVBufferRef **context);

  /**
   * Width --> Width of the image
   * Height --> Height of the image
   * offset_x --> Horizontal offset of the image in the texture
   * offset_y --> Vertical offset of the image in the texture
   * file_t card --> The file descriptor of the render device used for encoding
   *
   * @param width Frame or display width in pixels.
   * @param height Frame or display height in pixels.
   * @param vram Whether the image should use GPU memory instead of system memory.
   * @return Constructed AVCodec encode device object.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(int width, int height, bool vram);
  /**
   * @brief Create AVCodec encode device.
   *
   * @param width Frame or display width in pixels.
   * @param height Frame or display height in pixels.
   * @param offset_x Offset x.
   * @param offset_y Offset y.
   * @param vram Whether the image should use GPU memory instead of system memory.
   * @return Constructed AVCodec encode device object.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(int width, int height, int offset_x, int offset_y, bool vram);
  /**
   * @brief Create AVCodec encode device.
   *
   * @param width Frame or display width in pixels.
   * @param height Frame or display height in pixels.
   * @param card Video device path or render node used for VAAPI.
   * @param offset_x Offset x.
   * @param offset_y Offset y.
   * @param vram Whether the image should use GPU memory instead of system memory.
   * @return Constructed AVCodec encode device object.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(int width, int height, file_t &&card, int offset_x, int offset_y, bool vram);

  // Ensure the render device pointed to by fd is capable of encoding h264 with the hevc_mode configured
  /**
   * @brief Validate that the configured VAAPI device can be used.
   *
   * @param fd Native file descriptor to wrap or inspect.
   * @return True when the VAAPI device is usable for capture or encode.
   */
  bool validate(int fd);
}  // namespace va
