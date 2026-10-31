/**
 * @file src/platform/linux/lanternbox_hdr.h
 * @brief Validation of immutable Lanternbox HDR10 capture metadata.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <limits>

// local includes
#include "src/platform/common.h"

namespace wl {
  /**
   * @brief Collect exactly one complete, supported HDR10 contract before exposing metadata.
   * @details All values retain their protocol units. Duplicate, late, incomplete or failed
   * contracts are permanently invalid; the collector never substitutes content light levels.
   */
  class hdr10_metadata_t {
  public:
    static constexpr std::uint32_t fourcc = 0x30334241;  ///< DRM_FORMAT_ABGR2101010 (AB30).

    /**
     * @brief Receive generation, DRM format, H.273 primaries/transfer and full RGB range.
     * @param values Format event fields in wire order.
     */
    void format(const std::array<std::uint32_t, 5> &values) {
      if (accept(1)) {
        encoding = values;
      }
    }

    /**
     * @brief Receive reference white, mastering minimum/maximum and content light levels.
     * @param values Wire fields: cd/m², 0.0001 cd/m², cd/m², cd/m², cd/m².
     */
    void luminance(const std::array<std::uint32_t, 5> &values) {
      if (accept(2)) {
        light = values;
      }
    }

    /**
     * @brief Receive red, green, blue and white CIE xy pairs in units of 1/50000.
     * @param values Mastering primaries event fields in wire order.
     */
    void mastering_primaries(const std::array<std::uint32_t, 8> &values) {
      if (accept(4)) {
        primaries = values;
      }
    }

    /**
     * @brief Finish the contract; repeated completion permanently invalidates it.
     */
    void done() {
      rejected = rejected || finished;
      finished = true;
    }

    /**
     * @brief Permanently reject the contract.
     */
    void failed() {
      rejected = true;
    }

    /**
     * @brief Validate the complete version-1 RGB10 BT.2020/PQ contract and metadata bounds.
     * @return True only after all required events and a single completion.
     */
    bool valid() const {
      return finished && !rejected && events == 7 &&
             encoding == std::array<std::uint32_t, 5> {1, fourcc, 9, 16, 1} &&
             primaries == std::array<std::uint32_t, 8> {35400, 14600, 8500, 39850, 6550, 2300, 15635, 16450} &&
             light[0] > 0 && light[0] <= light[2] && light[2] <= 10000 &&
             light[1] < std::uint64_t {light[0]} * 10000 &&
             light[1] <= std::numeric_limits<decltype(SS_HDR_METADATA {}.minDisplayLuminance)>::max() &&
             light[3] <= 10000 && light[4] <= 10000 &&
             (light[3] == 0 || light[4] == 0 || light[4] <= light[3]);
    }

    /**
     * @brief Export validated metadata in Moonlight's matching ST2086 units.
     * @param metadata Destination, cleared even when the contract is invalid.
     * @return True if complete, validated metadata was exported.
     */
    bool get_metadata(SS_HDR_METADATA &metadata) const {
      metadata = {};
      if (!valid()) {
        return false;
      }
      for (std::size_t i = 0; i < 3; ++i) {
        metadata.displayPrimaries[i].x = primaries[i * 2];
        metadata.displayPrimaries[i].y = primaries[i * 2 + 1];
      }
      metadata.whitePoint.x = primaries[6];
      metadata.whitePoint.y = primaries[7];
      metadata.minDisplayLuminance = light[1];
      metadata.maxDisplayLuminance = light[2];
      metadata.maxContentLightLevel = light[3];
      metadata.maxFrameAverageLightLevel = light[4];
      return true;
    }

  private:
    /**
     * @brief Accept a required event exactly once before completion or failure.
     * @param event Bit identifying the received event.
     * @return True when the event may populate the contract.
     */
    bool accept(unsigned event) {
      if (finished || rejected || (events & event)) {
        rejected = true;
        return false;
      }
      events |= event;
      return true;
    }

    std::array<std::uint32_t, 5> encoding {};  ///< Generation, pixel format and RGB encoding.
    std::array<std::uint32_t, 5> light {};  ///< Luminance fields in their original wire units.
    std::array<std::uint32_t, 8> primaries {};  ///< Mastering chromaticities in units of 1/50000.
    unsigned events {0};  ///< Bit mask of required metadata events already received.
    bool finished {false};  ///< Whether completion has been received.
    bool rejected {false};  ///< Sticky rejection of failed or malformed event sequences.
  };
}  // namespace wl
