/**
 * @file tests/unit/platform/linux/test_lanternbox_hdr.cpp
 * @brief Public HDR10 capture-contract collector tests.
 */
#ifdef __linux__
  #include "src/platform/linux/lanternbox_hdr.h"

  #include <array>
  #include <cstdint>
  #include <gtest/gtest.h>
  #include <limits>

namespace {
  using values5 = std::array<std::uint32_t, 5>;
  using values8 = std::array<std::uint32_t, 8>;
  constexpr values5 format {1, 0x30334241, 9, 16, 1};
  constexpr values5 luminance {203, 50, 1000, 800, 400};
  constexpr values8 primaries {35400, 14600, 8500, 39850, 6550, 2300, 15635, 16450};

  /** @brief Send all three required data events. */
  void data(wl::hdr10_metadata_t &collector, const values5 &f = format, const values5 &l = luminance, const values8 &p = primaries) {
    collector.format(f);
    collector.luminance(l);
    collector.mastering_primaries(p);
  }

  /** @brief Check that invalid retrieval clears every public metadata field. */
  void expect_unavailable(const wl::hdr10_metadata_t &collector) {
    SS_HDR_METADATA out {};
    for (auto &primary : out.displayPrimaries) {
      primary.x = primary.y = 123;
    }
    out.whitePoint.x = out.whitePoint.y = 123;
    out.maxDisplayLuminance = out.minDisplayLuminance = 123;
    out.maxContentLightLevel = out.maxFrameAverageLightLevel = 123;
    out.maxFullFrameLuminance = 123;
    EXPECT_FALSE(collector.valid());
    EXPECT_FALSE(collector.get_metadata(out));
    for (const auto &primary : out.displayPrimaries) {
      EXPECT_EQ(primary.x, 0);
      EXPECT_EQ(primary.y, 0);
    }
    EXPECT_EQ(out.whitePoint.x, 0);
    EXPECT_EQ(out.whitePoint.y, 0);
    EXPECT_EQ(out.maxDisplayLuminance, 0);
    EXPECT_EQ(out.minDisplayLuminance, 0);
    EXPECT_EQ(out.maxContentLightLevel, 0);
    EXPECT_EQ(out.maxFrameAverageLightLevel, 0);
    EXPECT_EQ(out.maxFullFrameLuminance, 0);
  }

  /** @brief Complete a contract and assert the result is rejected. */
  void expect_rejected(const values5 &f, const values5 &l, const values8 &p) {
    wl::hdr10_metadata_t collector;
    data(collector, f, l, p);
    collector.done();
    expect_unavailable(collector);
  }
}  // namespace

/** @brief All events are required and metadata is unavailable until done. */
TEST(LanternboxHdrCollector, RequiresEveryEventAndDone) {
  wl::hdr10_metadata_t pending;
  expect_unavailable(pending);
  data(pending);
  expect_unavailable(pending);
  pending.done();
  EXPECT_TRUE(pending.valid());

  for (int missing = 0; missing < 3; ++missing) {
    SCOPED_TRACE(missing);
    wl::hdr10_metadata_t collector;
    if (missing != 0) {
      collector.format(format);
    }
    if (missing != 1) {
      collector.luminance(luminance);
    }
    if (missing != 2) {
      collector.mastering_primaries(primaries);
    }
    collector.done();
    expect_unavailable(collector);
    data(collector);
    collector.done();
    expect_unavailable(collector);
  }
}

/** @brief Duplicate and post-completion events permanently poison the contract. */
TEST(LanternboxHdrCollector, DuplicateAndLateEventsPermanentlyInvalidate) {
  for (int event = 0; event < 3; ++event) {
    SCOPED_TRACE(event);
    wl::hdr10_metadata_t duplicate;
    data(duplicate);
    if (event == 0) {
      duplicate.format(format);
    }
    if (event == 1) {
      duplicate.luminance(luminance);
    }
    if (event == 2) {
      duplicate.mastering_primaries(primaries);
    }
    duplicate.done();
    expect_unavailable(duplicate);
    data(duplicate);
    duplicate.done();
    expect_unavailable(duplicate);
  }
  for (int event = 0; event < 5; ++event) {
    SCOPED_TRACE(event);
    wl::hdr10_metadata_t late;
    data(late);
    late.done();
    ASSERT_TRUE(late.valid());
    if (event == 0) {
      late.format(format);
    }
    if (event == 1) {
      late.luminance(luminance);
    }
    if (event == 2) {
      late.mastering_primaries(primaries);
    }
    if (event == 3) {
      late.done();
    }
    if (event == 4) {
      late.failed();
    }
    expect_unavailable(late);
    late.done();
    expect_unavailable(late);
  }
}

/** @brief Failure before or during event collection cannot be repaired. */
TEST(LanternboxHdrCollector, FailurePermanentlyInvalidates) {
  for (int phase = 0; phase < 3; ++phase) {
    SCOPED_TRACE(phase);
    wl::hdr10_metadata_t collector;
    if (phase > 0) {
      collector.format(format);
    }
    if (phase > 1) {
      collector.luminance(luminance);
    }
    collector.failed();
    expect_unavailable(collector);
    data(collector);
    collector.done();
    expect_unavailable(collector);
  }
}

/** @brief Only the version-one RGB10 BT.2020 PQ full-range format is accepted. */
TEST(LanternboxHdrCollector, RejectsUnsupportedFormatFields) {
  for (std::size_t i = 0; i < format.size(); ++i) {
    SCOPED_TRACE(i);
    for (auto value : {0U, std::numeric_limits<std::uint32_t>::max()}) {
      auto bad = format;
      bad[i] = value;
      expect_rejected(bad, luminance, primaries);
    }
  }
  auto bad = format;
  bad[0] = 2;
  expect_rejected(bad, luminance, primaries);
  bad = format;
  bad[1] = 0x30335258;
  expect_rejected(bad, luminance, primaries);
  bad = format;
  bad[2] = 1;
  expect_rejected(bad, luminance, primaries);
  bad = format;
  bad[3] = 18;
  expect_rejected(bad, luminance, primaries);
  bad = format;
  bad[4] = 2;
  expect_rejected(bad, luminance, primaries);
}

/** @brief Each mastering chromaticity must exactly match BT.2020 D65. */
TEST(LanternboxHdrCollector, RequiresExactMasteringPrimaries) {
  for (std::size_t i = 0; i < primaries.size(); ++i) {
    SCOPED_TRACE(i);
    for (int delta : {-1, 1}) {
      auto bad = primaries;
      bad[i] = static_cast<std::uint32_t>(static_cast<int>(bad[i]) + delta);
      expect_rejected(format, luminance, bad);
    }
    auto bad = primaries;
    bad[i] = 65536;
    expect_rejected(format, luminance, bad);
  }
}

/** @brief Transport the protocol units and RGB order without substitution. */
TEST(LanternboxHdrCollector, PreservesMetadataUnitsAndUnknownContent) {
  wl::hdr10_metadata_t collector;
  data(collector, format, {203, 1234, 1000, 0, 0}, primaries);
  collector.done();
  ASSERT_TRUE(collector.valid());
  SS_HDR_METADATA out {};
  ASSERT_TRUE(collector.get_metadata(out));
  EXPECT_EQ(out.displayPrimaries[0].x, 35400);
  EXPECT_EQ(out.displayPrimaries[0].y, 14600);
  EXPECT_EQ(out.displayPrimaries[1].x, 8500);
  EXPECT_EQ(out.displayPrimaries[1].y, 39850);
  EXPECT_EQ(out.displayPrimaries[2].x, 6550);
  EXPECT_EQ(out.displayPrimaries[2].y, 2300);
  EXPECT_EQ(out.whitePoint.x, 15635);
  EXPECT_EQ(out.whitePoint.y, 16450);
  EXPECT_EQ(out.minDisplayLuminance, 1234);
  EXPECT_EQ(out.maxDisplayLuminance, 1000);
  EXPECT_EQ(out.maxContentLightLevel, 0);
  EXPECT_EQ(out.maxFrameAverageLightLevel, 0);
  EXPECT_EQ(out.maxFullFrameLuminance, 0);
}

/** @brief Known content light levels and the maximum representable black survive. */
TEST(LanternboxHdrCollector, AcceptsLuminanceBoundariesAndKnownContent) {
  for (const values5 l : {values5 {1, 0, 1, 0, 0}, values5 {7, 65535, 10000, 10000, 10000}, values5 {203, 1, 1000, 0, 500}, values5 {203, 1, 1000, 500, 0}}) {
    SCOPED_TRACE(l[0]);
    wl::hdr10_metadata_t collector;
    data(collector, format, l, primaries);
    collector.done();
    ASSERT_TRUE(collector.valid());
    SS_HDR_METADATA out {};
    ASSERT_TRUE(collector.get_metadata(out));
    EXPECT_EQ(out.minDisplayLuminance, l[1]);
    EXPECT_EQ(out.maxDisplayLuminance, l[2]);
    EXPECT_EQ(out.maxContentLightLevel, l[3]);
    EXPECT_EQ(out.maxFrameAverageLightLevel, l[4]);
    EXPECT_EQ(out.maxFullFrameLuminance, 0);
  }
}

/** @brief Reject invalid reference, peak, black, content levels and field overflow. */
TEST(LanternboxHdrCollector, RejectsLuminanceOutsideContract) {
  const values5 cases[] {
    {0, 0, 1000, 0, 0},
    {1001, 0, 1000, 0, 0},
    {1, 10000, 1000, 0, 0},
    {1, 10001, 1000, 0, 0},
    {1, 0, 0, 0, 0},
    {1, 0, 10001, 0, 0},
    {203, 65536, 1000, 0, 0},
    {203, 0, 1000, 10001, 0},
    {203, 0, 1000, 0, 10001},
    {203, 0, 1000, 400, 401},
    {203, 0, 1000, 65536, 0},
    {203, 0, 1000, 0, 65536},
    {203, 0, 65536, 0, 0},
  };
  for (const auto &l : cases) {
    SCOPED_TRACE(::testing::Message() << l[0] << ',' << l[1] << ',' << l[2] << ',' << l[3] << ',' << l[4]);
    expect_rejected(format, l, primaries);
  }
}
#endif
