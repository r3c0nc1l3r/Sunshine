/**
 * @file src/platform/linux/lanternbox_hdr.h
 * @brief Validation of immutable Lanternbox HDR10 capture metadata.
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
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

  /**
   * @brief Strict collector for the ordered v2 preparation and activation events.
   * @details The effective intent is immutable and any invalid event is terminal.
   */
  class capture_session_t {
  public:
    /** @brief Record the requested mode before receiving events. @param hdr Requested HDR intent. */
    explicit capture_session_t(bool hdr = false):
        requested_hdr {hdr} {}

    /** @brief Set the requested intent before any events arrive. @param value Requested HDR intent. */
    void request_hdr(bool value) {
      requested_hdr = value;
    }

    /** @brief Receive the effective mode. @param value Protocol mode value. */
    void mode(std::uint32_t value) {
      if (step != 0 || value > 1 || (value == 1 && !requested_hdr)) {
        return reject();
      }
      hdr = value == 1;
      step = 1;
    }

    /** @brief Receive HDR format. @param values Wire fields. */
    void format(const std::array<std::uint32_t, 5> &values) {
      if (!hdr || step != 1) {
        return reject();
      }
      metadata.format(values);
      step = 2;
    }

    /** @brief Receive HDR luminance. @param values Wire fields. */
    void luminance(const std::array<std::uint32_t, 5> &values) {
      if (!hdr || step != 2) {
        return reject();
      }
      metadata.luminance(values);
      step = 3;
    }

    /** @brief Receive HDR mastering chromaticities. @param values Wire fields. */
    void mastering_primaries(const std::array<std::uint32_t, 8> &values) {
      if (!hdr || step != 3) {
        return reject();
      }
      metadata.mastering_primaries(values);
      step = 4;
    }

    /** @brief Finish and validate preparation. */
    void prepared() {
      if (step != (hdr ? 4 : 1)) {
        return reject();
      }
      if (hdr) {
        metadata.done();
        if (!metadata.valid()) {
          return reject();
        }
      }
      step = 5;
    }

    /** @brief Record an activation request. @return Whether activation is permitted. */
    bool activating() {
      if (step != 5 || pending || epoch) {
        reject();
        return false;
      }
      pending = true;
      return true;
    }

    /** @brief Receive the activation barrier. @param value Nonzero color epoch. */
    void ready(std::uint32_t value) {
      if (step != 5 || !pending || !value) {
        return reject();
      }
      pending = false;
      epoch = value;
    }

    /** @brief Mark a terminal protocol or transport failure. */
    void failed() {
      reject();
    }

    /** @brief Check successful preparation. @return True when valid. */
    bool valid() const {
      return step == 5;
    }

    /** @brief Check whether a terminal failure was observed. @return True after rejection. */
    bool terminal() const {
      return step < 0;
    }

    /** @brief Check effective HDR intent. @return True for valid HDR. */
    bool is_hdr() const {
      return valid() && hdr;
    }

    /** @brief Check activation acknowledgement. @return Nonzero ready epoch, or zero. */
    std::uint32_t color_epoch() const {
      return valid() ? epoch : 0;
    }

    /** @brief Export validated HDR metadata. @param out Destination. @return True for HDR. */
    bool get_metadata(SS_HDR_METADATA &out) const {
      out = {};
      return is_hdr() && metadata.get_metadata(out);
    }

  private:
    void reject() {
      step = -1;
      pending = false;
      epoch = 0;
    }

    hdr10_metadata_t metadata;  ///< HDR field collector.
    std::atomic<int> step {0};  ///< Published preparation state and expected event sequence.
    bool requested_hdr {false};  ///< Whether HDR was requested.
    bool hdr {false};  ///< Effective immutable intent.
    bool pending {false};  ///< Activation awaiting acknowledgement.
    std::uint32_t epoch {0};  ///< Acknowledged producer epoch.
  };
}  // namespace wl
