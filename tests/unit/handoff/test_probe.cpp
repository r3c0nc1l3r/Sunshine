/**
 * @file tests/unit/handoff/test_probe.cpp
 * @brief Blind tests for the encoder probe exchanged in `HELLO` (src/handoff/probe.h, src/video.h): `gather_probe()`
 * with an injected `probe_env_t`, `accepted_probe()` (identity match and mismatch, the 0 to 5 mode range), and the
 * `/serverinfo` override (`video::set_handoff_probe_override`, `video::handoff_probe_override`) and its mapping from
 * `probe_results_t`.
 *
 * Written from `docs/drain-handoff.md` ("3. New process start", "GPU"), `src/handoff/probe.h`, `src/handoff/protocol.h`
 * and `src/video.h` only. Every test resets the `/serverinfo` override, and the probe reads go through fakes, so no
 * test touches `/sys`, the GPU or the encoders.
 *
 * Not covered here (no documented seam in `nvhttp.h`): the `/serverinfo` response flags themselves. They are E2E only
 * (docs/drain-handoff.md, E2E 13 and 26).
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <array>
  #include <cstdint>
  #include <map>
  #include <optional>
  #include <string>
  #include <vector>

  // local includes
  #include "src/handoff/probe.h"
  #include "src/handoff/protocol.h"
  #include "src/video.h"

namespace {
  using handoff::probe_env_t;
  namespace protocol = handoff::protocol;

  /**
   * @brief Fake host for `gather_probe()`: text files, symbolic links, render nodes and encoder probe state.
   *
   * Keys of `text` and `links` are full paths, as `gather_probe()` builds them. `read_paths` records every path that
   * `read_text` was asked for, in call order.
   */
  struct fake_host_t {
    std::string build_id {"build-1"};  ///< Build id given in the environment.
    std::string configured_adapter;  ///< `config::video.adapter_name` (empty when not set).
    std::vector<std::string> render_nodes {"/dev/dri/renderD128"};  ///< Render nodes that exist, in order.
    std::map<std::string, std::string> text;  ///< Text file content by path.
    std::map<std::string, std::string> links;  ///< Symbolic link target basename by path.
    protocol::probe_results_t results {};  ///< Current encoder probe state.
    int list_calls {0};  ///< Number of `list_render_nodes` calls.
    std::vector<std::string> read_paths;  ///< Paths passed to `read_text`, in call order.

    /**
     * @brief Build the probe environment over this fake. The fake must outlive the returned environment.
     *
     * @return Environment whose readers use this object.
     */
    probe_env_t make() {
      probe_env_t env;
      env.build_id = build_id;
      env.configured_adapter = configured_adapter;
      env.read_text = [this](const std::string &path) -> std::optional<std::string> {
        read_paths.push_back(path);
        auto it = text.find(path);
        if (it == text.end()) {
          return std::nullopt;
        }
        return it->second;
      };
      env.read_link_basename = [this](const std::string &path) -> std::optional<std::string> {
        auto it = links.find(path);
        if (it == links.end()) {
          return std::nullopt;
        }
        return it->second;
      };
      env.list_render_nodes = [this]() {
        ++list_calls;
        return render_nodes;
      };
      env.current_results = [this]() {
        return results;
      };
      return env;
    }

    /**
     * @brief Fill the host with a complete, typical NVIDIA identity on `/dev/dri/renderD128`.
     */
    void fill_nvidia() {
      text["/sys/class/drm/renderD128/device/vendor"] = "0x10de";
      text["/sys/class/drm/renderD128/device/device"] = "0x2684";
      links["/sys/class/drm/renderD128/device/driver"] = "nvidia";
      text["/sys/module/nvidia/version"] = "550.54.14";
    }
  };

  /**
   * @brief Identity that matches the one `fake_host_t::fill_nvidia()` and the default `build_id` produce.
   *
   * @return The own identity used by the `accepted_probe` tests.
   */
  protocol::probe_identity_t own_identity() {
    protocol::probe_identity_t identity;
    identity.build_id = "build-1";
    identity.render_node = "/dev/dri/renderD128";
    identity.vendor_id = 0x10de;
    identity.device_id = 0x2684;
    identity.driver_version = "550.54.14";
    return identity;
  }

  /**
   * @brief Probe results with every field set to a non-default value.
   *
   * @return Results used by the `accepted_probe` tests.
   */
  protocol::probe_results_t sample_results() {
    protocol::probe_results_t results;
    results.hevc_mode = 3;
    results.av1_mode = 1;
    results.yuv444_h264 = false;
    results.yuv444_hevc = true;
    results.yuv444_av1 = false;
    return results;
  }

  /**
   * @brief Build a `HELLO` that carries the given probe results and identity.
   *
   * @param results Probe results to send.
   * @param identity Identity to send.
   * @return The `HELLO` message.
   */
  protocol::hello_t make_hello(const protocol::probe_results_t &results, const protocol::probe_identity_t &identity) {
    protocol::hello_t hello;
    hello.probe = results;
    hello.identity = identity;
    return hello;
  }

  /**
   * @brief Fixture that clears the `/serverinfo` override before and after each test.
   */
  class HandoffProbeOverrideTest: public ::testing::Test {
  protected:
    /**
     * @brief Clear the override before the test.
     */
    void SetUp() override {
      video::set_handoff_probe_override(std::nullopt);
    }

    /**
     * @brief Clear the override after the test.
     */
    void TearDown() override {
      video::set_handoff_probe_override(std::nullopt);
    }
  };
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// gather_probe()
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief The results come from `current_results()`, field for field.
 */
TEST(HandoffProbeGatherTest, ResultsComeFromCurrentResults) {
  fake_host_t host;
  host.fill_nvidia();
  host.results.hevc_mode = 4;
  host.results.av1_mode = 5;
  host.results.yuv444_h264 = true;
  host.results.yuv444_hevc = false;
  host.results.yuv444_av1 = true;
  auto env = host.make();

  const auto data = handoff::gather_probe(env);
  EXPECT_EQ(data.results.hevc_mode, 4) << "results.hevc_mode must be the current HEVC mode.";
  EXPECT_EQ(data.results.av1_mode, 5) << "results.av1_mode must be the current AV1 mode.";
  EXPECT_TRUE(data.results.yuv444_h264) << "results.yuv444_h264 must be the H.264 4:4:4 flag.";
  EXPECT_FALSE(data.results.yuv444_hevc) << "results.yuv444_hevc must be the HEVC 4:4:4 flag.";
  EXPECT_TRUE(data.results.yuv444_av1) << "results.yuv444_av1 must be the AV1 4:4:4 flag.";
}

/**
 * @brief The build id in the identity is the environment's build id, as given.
 */
TEST(HandoffProbeGatherTest, BuildIdComesFromEnvironment) {
  fake_host_t host;
  host.fill_nvidia();
  host.build_id = "2026.914.233613-lb.2";
  auto env = host.make();

  EXPECT_EQ(handoff::gather_probe(env).identity.build_id, "2026.914.233613-lb.2")
    << "identity.build_id must be the build id of the environment, unchanged.";
}

/**
 * @brief A configured adapter is the render node, and the configured list is not consulted.
 */
TEST(HandoffProbeGatherTest, ConfiguredAdapterIsUsedAsRenderNode) {
  fake_host_t host;
  host.configured_adapter = "/dev/dri/renderD129";
  host.render_nodes = {"/dev/dri/renderD128"};
  host.text["/sys/class/drm/renderD129/device/vendor"] = "0x1002";
  host.text["/sys/class/drm/renderD129/device/device"] = "0x7310";
  auto env = host.make();

  const auto data = handoff::gather_probe(env);
  EXPECT_EQ(data.identity.render_node, "/dev/dri/renderD129")
    << "A configured adapter must be the render node, got: " << data.identity.render_node;
  EXPECT_EQ(data.identity.vendor_id, 0x1002u) << "Vendor id must be read from the configured adapter's node.";
  EXPECT_EQ(data.identity.device_id, 0x7310u) << "Device id must be read from the configured adapter's node.";
  EXPECT_EQ(host.list_calls, 0) << "The render node list must not be consulted when an adapter is configured.";
}

/**
 * @brief Without a configured adapter, the first existing render node is taken.
 */
TEST(HandoffProbeGatherTest, FirstRenderNodeIsUsedWhenNoAdapter) {
  fake_host_t host;
  host.render_nodes = {"/dev/dri/renderD128", "/dev/dri/renderD129"};
  host.fill_nvidia();
  auto env = host.make();

  const auto data = handoff::gather_probe(env);
  EXPECT_EQ(data.identity.render_node, "/dev/dri/renderD128")
    << "The first render node in ascending order must be taken, got: " << data.identity.render_node;
  EXPECT_EQ(host.list_calls, 1) << "The render node list must be consulted exactly once when no adapter is configured.";
}

/**
 * @brief No adapter and no render node: the render node is empty and the GPU fields are their neutral values.
 */
TEST(HandoffProbeGatherTest, NoRenderNodeGivesEmptyIdentityFields) {
  fake_host_t host;
  host.render_nodes = {};
  auto env = host.make();

  const auto data = handoff::gather_probe(env);
  EXPECT_EQ(data.identity.render_node, "") << "With no render node the render_node must be the empty string.";
  EXPECT_EQ(data.identity.vendor_id, 0u) << "With no render node vendor_id must be 0.";
  EXPECT_EQ(data.identity.device_id, 0u) << "With no render node device_id must be 0.";
  EXPECT_EQ(data.identity.driver_version, "") << "With no render node driver_version must be the empty string.";
}

/**
 * @brief Vendor and device ids are read as hexadecimal, with the `0x` prefix.
 */
TEST(HandoffProbeGatherTest, VendorAndDeviceIdsParseHexWithPrefix) {
  fake_host_t host;
  host.fill_nvidia();
  auto env = host.make();

  const auto data = handoff::gather_probe(env);
  EXPECT_EQ(data.identity.vendor_id, 0x10deu) << "The vendor id text 0x10de must parse to 0x10de.";
  EXPECT_EQ(data.identity.device_id, 0x2684u) << "The device id text 0x2684 must parse to 0x2684.";
}

/**
 * @brief Vendor and device ids also parse without the `0x` prefix.
 */
TEST(HandoffProbeGatherTest, VendorAndDeviceIdsParseWithoutPrefix) {
  fake_host_t host;
  host.fill_nvidia();
  host.text["/sys/class/drm/renderD128/device/vendor"] = "10de";
  host.text["/sys/class/drm/renderD128/device/device"] = "2684";
  auto env = host.make();

  const auto data = handoff::gather_probe(env);
  EXPECT_EQ(data.identity.vendor_id, 0x10deu) << "The vendor id text 10de (no prefix) must parse to 0x10de.";
  EXPECT_EQ(data.identity.device_id, 0x2684u) << "The device id text 2684 (no prefix) must parse to 0x2684.";
}

/**
 * @brief Unreadable id files give 0 for the vendor and the device.
 */
TEST(HandoffProbeGatherTest, UnreadableIdFilesGiveZero) {
  fake_host_t host;
  host.links["/sys/class/drm/renderD128/device/driver"] = "nvidia";
  host.text["/sys/module/nvidia/version"] = "550.54.14";
  auto env = host.make();

  const auto data = handoff::gather_probe(env);
  EXPECT_EQ(data.identity.vendor_id, 0u) << "An unreadable vendor file must give vendor_id 0.";
  EXPECT_EQ(data.identity.device_id, 0u) << "An unreadable device file must give device_id 0.";
}

/**
 * @brief Empty or unparsable id text gives 0.
 */
TEST(HandoffProbeGatherTest, EmptyOrUnparsableIdTextGivesZero) {
  fake_host_t host;
  host.fill_nvidia();
  host.text["/sys/class/drm/renderD128/device/vendor"] = "";
  host.text["/sys/class/drm/renderD128/device/device"] = "not-hex";
  auto env = host.make();

  const auto data = handoff::gather_probe(env);
  EXPECT_EQ(data.identity.vendor_id, 0u) << "Empty vendor text must give vendor_id 0.";
  EXPECT_EQ(data.identity.device_id, 0u) << "Unparsable device text must give device_id 0.";
}

/**
 * @brief The driver version is the content of `/sys/module/<driver>/version`, where `<driver>` is the link basename.
 */
TEST(HandoffProbeGatherTest, DriverVersionComesFromModuleVersion) {
  fake_host_t host;
  host.fill_nvidia();
  auto env = host.make();

  const auto data = handoff::gather_probe(env);
  EXPECT_EQ(data.identity.driver_version, "550.54.14")
    << "driver_version must be the content of /sys/module/nvidia/version, got: " << data.identity.driver_version;
}

/**
 * @brief An unreadable driver link gives an empty driver version, even when the module version file exists.
 */
TEST(HandoffProbeGatherTest, UnreadableDriverLinkGivesEmptyVersion) {
  fake_host_t host;
  host.fill_nvidia();
  host.links.clear();
  auto env = host.make();

  const auto data = handoff::gather_probe(env);
  EXPECT_EQ(data.identity.driver_version, "") << "An unreadable driver link must give an empty driver_version.";
}

/**
 * @brief A missing module version file gives an empty driver version, even when the driver link is readable.
 */
TEST(HandoffProbeGatherTest, MissingModuleVersionGivesEmptyVersion) {
  fake_host_t host;
  host.fill_nvidia();
  host.text.erase("/sys/module/nvidia/version");
  auto env = host.make();

  const auto data = handoff::gather_probe(env);
  EXPECT_EQ(data.identity.driver_version, "") << "A missing /sys/module/<driver>/version must give an empty driver_version.";
}

/**
 * @brief The files read follow the documented paths: the node's vendor and device, the driver link and the module version.
 */
TEST(HandoffProbeGatherTest, ReadsDocumentedSysPaths) {
  fake_host_t host;
  host.fill_nvidia();
  auto env = host.make();

  (void) handoff::gather_probe(env);
  auto has = [&host](const std::string &path) {
    for (const auto &read : host.read_paths) {
      if (read == path) {
        return true;
      }
    }
    return false;
  };
  EXPECT_TRUE(has("/sys/class/drm/renderD128/device/vendor")) << "gather_probe must read /sys/class/drm/<node>/device/vendor.";
  EXPECT_TRUE(has("/sys/class/drm/renderD128/device/device")) << "gather_probe must read /sys/class/drm/<node>/device/device.";
  EXPECT_TRUE(has("/sys/module/nvidia/version")) << "gather_probe must read /sys/module/<driver>/version.";
}

/**
 * @brief Gathering reads encoder state but never changes it.
 */
TEST(HandoffProbeGatherTest, GatherDoesNotChangeEncoderGlobals) {
  const int hevc_before = video::active_hevc_mode;
  const int av1_before = video::active_av1_mode;
  const auto yuv_before = video::last_encoder_probe_supported_yuv444_for_codec;

  fake_host_t host;
  host.fill_nvidia();
  host.results.hevc_mode = 5;
  auto env = host.make();
  (void) handoff::gather_probe(env);

  EXPECT_EQ(video::active_hevc_mode, hevc_before) << "gather_probe must not change video::active_hevc_mode.";
  EXPECT_EQ(video::active_av1_mode, av1_before) << "gather_probe must not change video::active_av1_mode.";
  EXPECT_EQ(video::last_encoder_probe_supported_yuv444_for_codec, yuv_before)
    << "gather_probe must not change the 4:4:4 array.";
}

// ---------------------------------------------------------------------------------------------------------------------
// accepted_probe()
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief An identical identity is accepted and the sender's probe results are returned.
 */
TEST(HandoffProbeAcceptTest, IdenticalIdentityIsAccepted) {
  const auto hello = make_hello(sample_results(), own_identity());

  const auto accepted = handoff::accepted_probe(hello, own_identity());
  ASSERT_TRUE(accepted.has_value()) << "A HELLO whose identity equals the own identity must be accepted.";
  EXPECT_EQ(accepted->hevc_mode, 3) << "The accepted results must carry the sender's hevc_mode.";
  EXPECT_EQ(accepted->av1_mode, 1) << "The accepted results must carry the sender's av1_mode.";
  EXPECT_FALSE(accepted->yuv444_h264) << "The accepted results must carry the sender's yuv444_h264.";
  EXPECT_TRUE(accepted->yuv444_hevc) << "The accepted results must carry the sender's yuv444_hevc.";
  EXPECT_FALSE(accepted->yuv444_av1) << "The accepted results must carry the sender's yuv444_av1.";
}

/**
 * @brief A different build id is a mismatch: nothing is accepted.
 */
TEST(HandoffProbeAcceptTest, BuildIdMismatchIsRejected) {
  auto identity = own_identity();
  identity.build_id = "build-2";
  const auto hello = make_hello(sample_results(), identity);

  EXPECT_FALSE(handoff::accepted_probe(hello, own_identity()).has_value())
    << "A different build id must be rejected.";
}

/**
 * @brief A different render node is a mismatch.
 */
TEST(HandoffProbeAcceptTest, RenderNodeMismatchIsRejected) {
  auto identity = own_identity();
  identity.render_node = "/dev/dri/renderD129";
  const auto hello = make_hello(sample_results(), identity);

  EXPECT_FALSE(handoff::accepted_probe(hello, own_identity()).has_value()) << "A different render node must be rejected.";
}

/**
 * @brief A different vendor id is a mismatch.
 */
TEST(HandoffProbeAcceptTest, VendorMismatchIsRejected) {
  auto identity = own_identity();
  identity.vendor_id = 0x1002;
  const auto hello = make_hello(sample_results(), identity);

  EXPECT_FALSE(handoff::accepted_probe(hello, own_identity()).has_value()) << "A different vendor id must be rejected.";
}

/**
 * @brief A different device id is a mismatch.
 */
TEST(HandoffProbeAcceptTest, DeviceMismatchIsRejected) {
  auto identity = own_identity();
  identity.device_id = 0x2204;
  const auto hello = make_hello(sample_results(), identity);

  EXPECT_FALSE(handoff::accepted_probe(hello, own_identity()).has_value()) << "A different device id must be rejected.";
}

/**
 * @brief A different driver version is a mismatch.
 */
TEST(HandoffProbeAcceptTest, DriverVersionMismatchIsRejected) {
  auto identity = own_identity();
  identity.driver_version = "560.35.03";
  const auto hello = make_hello(sample_results(), identity);

  EXPECT_FALSE(handoff::accepted_probe(hello, own_identity()).has_value())
    << "A different driver version must be rejected.";
}

/**
 * @brief A `HELLO` without probe results is rejected.
 */
TEST(HandoffProbeAcceptTest, MissingProbeIsRejected) {
  protocol::hello_t hello;
  hello.identity = own_identity();
  hello.probe = std::nullopt;

  EXPECT_FALSE(handoff::accepted_probe(hello, own_identity()).has_value()) << "A HELLO without probe results must be rejected.";
}

/**
 * @brief A `HELLO` without an identity is rejected.
 */
TEST(HandoffProbeAcceptTest, MissingIdentityIsRejected) {
  protocol::hello_t hello;
  hello.probe = sample_results();
  hello.identity = std::nullopt;

  EXPECT_FALSE(handoff::accepted_probe(hello, own_identity()).has_value())
    << "A HELLO without an identity must be rejected.";
}

/**
 * @brief HEVC mode above 5 is rejected as a defensive check.
 */
TEST(HandoffProbeAcceptTest, HevcModeAboveFiveIsRejected) {
  auto results = sample_results();
  results.hevc_mode = 6;
  const auto hello = make_hello(results, own_identity());

  EXPECT_FALSE(handoff::accepted_probe(hello, own_identity()).has_value()) << "hevc_mode 6 is outside 0 to 5 and must be rejected.";
}

/**
 * @brief A negative HEVC mode is rejected.
 */
TEST(HandoffProbeAcceptTest, NegativeHevcModeIsRejected) {
  auto results = sample_results();
  results.hevc_mode = -1;
  const auto hello = make_hello(results, own_identity());

  EXPECT_FALSE(handoff::accepted_probe(hello, own_identity()).has_value()) << "A negative hevc_mode must be rejected.";
}

/**
 * @brief AV1 mode above 5 is rejected.
 */
TEST(HandoffProbeAcceptTest, Av1ModeAboveFiveIsRejected) {
  auto results = sample_results();
  results.av1_mode = 6;
  const auto hello = make_hello(results, own_identity());

  EXPECT_FALSE(handoff::accepted_probe(hello, own_identity()).has_value()) << "av1_mode 6 is outside 0 to 5 and must be rejected.";
}

/**
 * @brief A negative AV1 mode is rejected.
 */
TEST(HandoffProbeAcceptTest, NegativeAv1ModeIsRejected) {
  auto results = sample_results();
  results.av1_mode = -1;
  const auto hello = make_hello(results, own_identity());

  EXPECT_FALSE(handoff::accepted_probe(hello, own_identity()).has_value()) << "A negative av1_mode must be rejected.";
}

/**
 * @brief The whole accepted range 0 to 5 is accepted, including the 4:4:4 modes 4 and 5 that the config options do not allow.
 */
TEST(HandoffProbeAcceptTest, ModesZeroToFiveAreAccepted) {
  for (int mode = 0; mode <= 5; ++mode) {
    auto results = sample_results();
    results.hevc_mode = mode;
    results.av1_mode = 5 - mode;
    const auto hello = make_hello(results, own_identity());

    const auto accepted = handoff::accepted_probe(hello, own_identity());
    ASSERT_TRUE(accepted.has_value()) << "Modes hevc " << mode << " and av1 " << (5 - mode) << " are inside 0 to 5 and must be accepted.";
    EXPECT_EQ(accepted->hevc_mode, mode) << "The accepted hevc_mode must equal the sent value " << mode << ".";
    EXPECT_EQ(accepted->av1_mode, 5 - mode) << "The accepted av1_mode must equal the sent value " << (5 - mode) << ".";
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// `/serverinfo` override: `video::set_handoff_probe_override` and `video::handoff_probe_override`
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Without an installed override, the query returns no value.
 */
TEST_F(HandoffProbeOverrideTest, FreshProcessHasNoOverride) {
  EXPECT_FALSE(video::handoff_probe_override().has_value()) << "With no override installed the query must return nullopt.";
}

/**
 * @brief A value set is read back with the same fields.
 */
TEST_F(HandoffProbeOverrideTest, SetThenGetRoundTrips) {
  video::handoff_probe_override_t value;
  value.hevc_mode = 4;
  value.av1_mode = 5;
  value.yuv444 = {true, false, true};
  video::set_handoff_probe_override(value);

  const auto read = video::handoff_probe_override();
  ASSERT_TRUE(read.has_value()) << "An installed override must be returned.";
  EXPECT_EQ(read->hevc_mode, 4) << "The override must return the hevc_mode that was set.";
  EXPECT_EQ(read->av1_mode, 5) << "The override must return the av1_mode that was set.";
  EXPECT_EQ(read->yuv444, (std::array<bool, 3> {true, false, true})) << "The override must return the yuv444 array that was set.";
}

/**
 * @brief Setting `std::nullopt` clears an installed override.
 */
TEST_F(HandoffProbeOverrideTest, SettingNulloptClears) {
  video::handoff_probe_override_t value;
  value.hevc_mode = 2;
  video::set_handoff_probe_override(value);
  ASSERT_TRUE(video::handoff_probe_override().has_value()) << "Precondition: the override must be installed.";

  video::set_handoff_probe_override(std::nullopt);
  EXPECT_FALSE(video::handoff_probe_override().has_value()) << "Setting nullopt must clear the override.";
}

/**
 * @brief Clearing a process that never had an override changes nothing.
 */
TEST_F(HandoffProbeOverrideTest, ClearingWithoutOverrideIsNoop) {
  video::set_handoff_probe_override(std::nullopt);
  EXPECT_FALSE(video::handoff_probe_override().has_value()) << "Clearing when nothing is installed must leave no override.";
}

/**
 * @brief A second set replaces the first value.
 */
TEST_F(HandoffProbeOverrideTest, SecondSetReplacesFirst) {
  video::handoff_probe_override_t first;
  first.hevc_mode = 1;
  video::set_handoff_probe_override(first);

  video::handoff_probe_override_t second;
  second.hevc_mode = 3;
  second.av1_mode = 2;
  video::set_handoff_probe_override(second);

  const auto read = video::handoff_probe_override();
  ASSERT_TRUE(read.has_value()) << "The second override must be installed.";
  EXPECT_EQ(read->hevc_mode, 3) << "The second set must replace the first hevc_mode.";
  EXPECT_EQ(read->av1_mode, 2) << "The second set must replace the first av1_mode.";
}

/**
 * @brief The override never changes the encoder probe globals.
 */
TEST_F(HandoffProbeOverrideTest, OverrideDoesNotChangeEncoderGlobals) {
  const int hevc_before = video::active_hevc_mode;
  const int av1_before = video::active_av1_mode;
  const auto yuv_before = video::last_encoder_probe_supported_yuv444_for_codec;

  video::handoff_probe_override_t value;
  value.hevc_mode = 5;
  value.av1_mode = 5;
  value.yuv444 = {true, true, true};
  video::set_handoff_probe_override(value);

  EXPECT_EQ(video::active_hevc_mode, hevc_before) << "Installing the override must not change active_hevc_mode.";
  EXPECT_EQ(video::active_av1_mode, av1_before) << "Installing the override must not change active_av1_mode.";
  EXPECT_EQ(video::last_encoder_probe_supported_yuv444_for_codec, yuv_before)
    << "Installing the override must not change the 4:4:4 array.";
}

/**
 * @brief The accepted probe maps onto the override field for field: hevc, av1, and yuv444 H.264, HEVC, AV1 in order.
 */
TEST_F(HandoffProbeOverrideTest, AcceptedProbeMapsOntoOverride) {
  protocol::probe_results_t results;
  results.hevc_mode = 4;
  results.av1_mode = 2;
  results.yuv444_h264 = false;
  results.yuv444_hevc = true;
  results.yuv444_av1 = true;
  const auto hello = make_hello(results, own_identity());

  const auto accepted = handoff::accepted_probe(hello, own_identity());
  ASSERT_TRUE(accepted.has_value()) << "Precondition: the matching HELLO must be accepted.";

  // The mapping documented in src/video.h, applied by the caller.
  video::handoff_probe_override_t value;
  value.hevc_mode = accepted->hevc_mode;
  value.av1_mode = accepted->av1_mode;
  value.yuv444 = {accepted->yuv444_h264, accepted->yuv444_hevc, accepted->yuv444_av1};
  video::set_handoff_probe_override(value);

  const auto read = video::handoff_probe_override();
  ASSERT_TRUE(read.has_value()) << "The mapped override must be installed.";
  EXPECT_EQ(read->hevc_mode, 4) << "results.hevc_mode must land in hevc_mode.";
  EXPECT_EQ(read->av1_mode, 2) << "results.av1_mode must land in av1_mode.";
  EXPECT_EQ(read->yuv444[0], false) << "results.yuv444_h264 must land in yuv444[0] (H.264).";
  EXPECT_EQ(read->yuv444[1], true) << "results.yuv444_hevc must land in yuv444[1] (HEVC).";
  EXPECT_EQ(read->yuv444[2], true) << "results.yuv444_av1 must land in yuv444[2] (AV1).";
}

#endif
