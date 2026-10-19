/**
 * @file tests/unit/test_audio_handoff.cpp
 * @brief Blind tests for the drain-handoff rules in audio (`src/audio.h`): `reassert_virtual_sink()`, the restore skip
 * of `stop_audio_control` while draining, the `start_audio_control` and `stop_audio_control` test seams, and the
 * host sink passed in `SUNSHINE_HANDOFF_HOST_SINK`.
 *
 * Written from `docs/drain-handoff.md` ("Other resources", audio sinks), `src/audio.h`, `src/platform/common.h` and
 * `src/handoff/runtime.h` only. Every test installs its own fake audio controller and resets the handoff runtime, so no
 * state survives from one test to the next.
 *
 * Not covered here (no seam in the headers, so E2E only): the PulseAudio `server_t` destructor skipping `unload_null`,
 * and the backend's `sink_info()` that never reports a `sink-sunshine-*` name as the host sink.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// local includes
#include "src/audio.h"
#include "src/platform/common.h"

#ifdef __linux__
  #include "src/handoff/hooks.h"
  #include "src/handoff/runtime.h"
  #include "src/handoff/settings.h"
#endif

namespace {
  /**
   * @brief Fake platform audio controller that records every `set_sink` call.
   */
  class fake_audio_control_t: public platf::audio_control_t {
  public:
    /**
     * @brief Record the sink name and return the configured result.
     *
     * @param sink Sink name given by the caller.
     * @return `result`.
     */
    int set_sink(const std::string &sink) override {
      sinks.push_back(sink);
      return result;
    }

    /**
     * @brief Not used by these tests; always returns no microphone.
     *
     * @return `nullptr`.
     */
    std::unique_ptr<platf::mic_t> microphone(
      const std::uint8_t *mapping,
      int channels,
      std::uint32_t sample_rate,
      std::uint32_t frame_size,
      bool continuous,
      [[maybe_unused]] bool host_audio_enabled
    ) override {
      return nullptr;
    }

    /**
     * @brief Not used by these tests; always reports the sink as available.
     *
     * @param sink Sink name, ignored.
     * @return `true`.
     */
    bool is_sink_available(const std::string &sink) override {
      return true;
    }

    /**
     * @brief Not used by these tests; reports no sink information.
     *
     * @return `std::nullopt`.
     */
    std::optional<platf::sink_t> sink_info() override {
      return std::nullopt;
    }

    std::vector<std::string> sinks;  ///< Sink names passed to `set_sink`, in call order.
    int result {0};  ///< Value `set_sink` returns.
  };

  /**
   * @brief Build a test-owned audio context with a fake controller and a virtual sink name.
   *
   * The context is filled in place (an audio context holds a mutex, so it cannot be returned by value).
   *
   * @param ctx Context to fill.
   * @param control Fake controller; its ownership moves into `ctx`.
   * @param virtual_sink Virtual sink name recorded for the first session.
   * @param restore Value for `restore_sink`.
   * @param host Host playback sink name stored in `sink.host`.
   */
  void make_ctx(
    audio::audio_ctx_t &ctx,
    std::unique_ptr<platf::audio_control_t> control,
    const std::string &virtual_sink,
    bool restore,
    const std::string &host
  ) {
    ctx.sink_flag = std::make_unique<std::atomic_bool>(true);
    ctx.control = std::move(control);
    ctx.restore_sink = restore;
    ctx.sink.host = host;
    ctx.virtual_sink = virtual_sink;
  }

  /**
   * @brief Fixture that installs a factory for the audio controller and resets the handoff runtime.
   */
  class HandoffAudioTest: public ::testing::Test {
  protected:
    /**
     * @brief Reset the handoff runtime and clear the controller factory before each test.
     */
    void SetUp() override {
#ifdef __linux__
      handoff::runtime().reset_for_test();
#endif
      audio::testing::set_audio_control_factory_for_test({});
    }

    /**
     * @brief Restore the platform controller factory and reset the handoff runtime after each test.
     */
    void TearDown() override {
      audio::testing::set_audio_control_factory_for_test({});
#ifdef __linux__
      handoff::runtime().reset_for_test();
#endif
    }
  };

#ifdef __linux__
  /**
   * @brief Map-backed fake process environment for `runtime_t::early_init()`.
   */
  struct fake_env_t {
    std::map<std::string, std::string> vars;  ///< Variables that are set.

    /**
     * @brief Build the `early_init` callbacks over this object.
     *
     * @return Callbacks that read and change `vars`.
     */
    handoff::early_init_env_t make() {
      handoff::early_init_env_t env;
      env.getenv = [this](const char *name) -> const char * {
        auto it = vars.find(name);
        return it == vars.end() ? nullptr : it->second.c_str();
      };
      env.unsetenv = [this](const char *name) {
        vars.erase(name);
      };
      env.scrub_fds = [] {
      };
      return env;
    }
  };

  /**
   * @brief Put the process-wide runtime into `draining` through the documented steps (`configure`, then the
   * `normal` to `draining` transition).
   *
   * @return `true` when `hooks::draining()` reports `true` afterwards.
   */
  bool make_draining() {
    handoff::settings_t settings;
    settings.enabled = true;
    handoff::runtime().configure(settings);
    if (!handoff::runtime().transition(handoff::mode_e::normal, handoff::mode_e::draining)) {
      return false;
    }
    return handoff::hooks::draining();
  }
#endif
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// `reassert_virtual_sink()`
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Zero sessions: nothing is switched and the result is `false`.
 */
TEST_F(HandoffAudioTest, ReassertWithZeroSessionsDoesNothing) {
  audio::audio_ctx_t ctx;
  auto control = std::make_unique<fake_audio_control_t>();
  auto *view = control.get();
  make_ctx(ctx, std::move(control), "sink-sunshine-stereo", true, "host");

  EXPECT_FALSE(audio::reassert_virtual_sink(ctx, 0)) << "With zero sessions reassert_virtual_sink must return false.";
  EXPECT_TRUE(view->sinks.empty()) << "With zero sessions reassert_virtual_sink must not call set_sink.";
}

/**
 * @brief A negative session count is treated like zero: nothing is switched.
 */
TEST_F(HandoffAudioTest, ReassertWithNegativeSessionsDoesNothing) {
  audio::audio_ctx_t ctx;
  auto control = std::make_unique<fake_audio_control_t>();
  auto *view = control.get();
  make_ctx(ctx, std::move(control), "sink-sunshine-stereo", true, "host");

  EXPECT_FALSE(audio::reassert_virtual_sink(ctx, -1)) << "With a negative session count reassert_virtual_sink must return false.";
  EXPECT_TRUE(view->sinks.empty()) << "With a negative session count reassert_virtual_sink must not call set_sink.";
}

/**
 * @brief No controller: the result is `false` and nothing is done.
 */
TEST_F(HandoffAudioTest, ReassertWithoutControlReturnsFalse) {
  audio::audio_ctx_t ctx;
  ctx.sink_flag = std::make_unique<std::atomic_bool>(true);
  ctx.virtual_sink = "sink-sunshine-stereo";
  ctx.restore_sink = true;

  EXPECT_FALSE(audio::reassert_virtual_sink(ctx, 2)) << "Without a controller reassert_virtual_sink must return false.";
}

/**
 * @brief No virtual sink recorded (no session switched to one): the result is `false` and no sink is set.
 */
TEST_F(HandoffAudioTest, ReassertWithEmptyVirtualSinkDoesNothing) {
  audio::audio_ctx_t ctx;
  auto control = std::make_unique<fake_audio_control_t>();
  auto *view = control.get();
  make_ctx(ctx, std::move(control), "", true, "host");

  EXPECT_FALSE(audio::reassert_virtual_sink(ctx, 2))
    << "With an empty virtual_sink reassert_virtual_sink must return false.";
  EXPECT_TRUE(view->sinks.empty()) << "With an empty virtual_sink reassert_virtual_sink must not call set_sink.";
}

/**
 * @brief Sessions and a recorded virtual sink: the default sink is set back to the virtual sink and `true` is returned.
 */
TEST_F(HandoffAudioTest, ReassertSelectsVirtualSinkWhenSessionsExist) {
  audio::audio_ctx_t ctx;
  auto control = std::make_unique<fake_audio_control_t>();
  auto *view = control.get();
  make_ctx(ctx, std::move(control), "sink-sunshine-stereo", true, "host");

  EXPECT_TRUE(audio::reassert_virtual_sink(ctx, 2)) << "With sessions and a virtual sink, reassert_virtual_sink must return true.";
  ASSERT_EQ(view->sinks.size(), 1u) << "reassert_virtual_sink must call set_sink exactly once.";
  EXPECT_EQ(view->sinks.front(), "sink-sunshine-stereo")
    << "reassert_virtual_sink must select the recorded virtual sink, got: " << view->sinks.front();
}

/**
 * @brief A failing `set_sink` makes the result `false`; the call was still made once.
 */
TEST_F(HandoffAudioTest, ReassertReturnsFalseWhenSetSinkFails) {
  audio::audio_ctx_t ctx;
  auto control = std::make_unique<fake_audio_control_t>();
  control->result = -1;
  auto *view = control.get();
  make_ctx(ctx, std::move(control), "sink-sunshine-stereo", true, "host");

  EXPECT_FALSE(audio::reassert_virtual_sink(ctx, 1)) << "When set_sink fails, reassert_virtual_sink must return false.";
  EXPECT_EQ(view->sinks.size(), 1u) << "reassert_virtual_sink must still attempt set_sink once.";
}

/**
 * @brief Reassert changes nothing else: `restore_sink`, the session flag, the host sink and `virtual_sink` keep their values.
 */
TEST_F(HandoffAudioTest, ReassertLeavesRestoreStateUnchanged) {
  audio::audio_ctx_t ctx;
  auto control = std::make_unique<fake_audio_control_t>();
  make_ctx(ctx, std::move(control), "sink-sunshine-surround51", true, "alsa_output.host");

  ASSERT_TRUE(audio::reassert_virtual_sink(ctx, 1)) << "Precondition: reassert_virtual_sink must succeed.";
  EXPECT_TRUE(ctx.restore_sink) << "reassert_virtual_sink must not change restore_sink.";
  ASSERT_NE(ctx.sink_flag, nullptr) << "reassert_virtual_sink must not clear the session flag.";
  EXPECT_TRUE(ctx.sink_flag->load()) << "reassert_virtual_sink must not change the session flag.";
  EXPECT_EQ(ctx.sink.host, "alsa_output.host") << "reassert_virtual_sink must not change the recorded host sink.";
  EXPECT_EQ(ctx.virtual_sink, "sink-sunshine-surround51") << "reassert_virtual_sink must not change virtual_sink.";
}

/**
 * @brief The sink mutex is released when `reassert_virtual_sink` returns, so a caller can take it again.
 */
TEST_F(HandoffAudioTest, ReassertReleasesSinkMutex) {
  audio::audio_ctx_t ctx;
  make_ctx(ctx, std::make_unique<fake_audio_control_t>(), "sink-sunshine-stereo", true, "host");

  ASSERT_TRUE(audio::reassert_virtual_sink(ctx, 1)) << "Precondition: reassert_virtual_sink must succeed.";
  std::unique_lock<std::mutex> lock(ctx.sink_mutex, std::try_to_lock);
  EXPECT_TRUE(lock.owns_lock()) << "reassert_virtual_sink must release sink_mutex before it returns.";
}

/**
 * @brief Each call switches again: two calls with sessions give two `set_sink` calls with the same name.
 */
TEST_F(HandoffAudioTest, ReassertCallsSetSinkOnEachCall) {
  audio::audio_ctx_t ctx;
  auto control = std::make_unique<fake_audio_control_t>();
  auto *view = control.get();
  make_ctx(ctx, std::move(control), "sink-sunshine-stereo", true, "host");

  EXPECT_TRUE(audio::reassert_virtual_sink(ctx, 1)) << "First reassert_virtual_sink call must succeed.";
  EXPECT_TRUE(audio::reassert_virtual_sink(ctx, 3)) << "Second reassert_virtual_sink call must succeed.";
  ASSERT_EQ(view->sinks.size(), 2u) << "Two calls must give two set_sink calls.";
  EXPECT_EQ(view->sinks[1], "sink-sunshine-stereo") << "The second call must select the same virtual sink.";
}

// ---------------------------------------------------------------------------------------------------------------------
// `start_audio_control_for_test()` and `stop_audio_control_for_test()`
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief `start_audio_control` returns 0 when the factory gives no controller (not an error in the test seam).
 */
TEST_F(HandoffAudioTest, StartReturnsZeroWithoutController) {
  int calls = 0;
  audio::testing::set_audio_control_factory_for_test([&calls]() {
    ++calls;
    return nullptr;
  });

  audio::audio_ctx_t ctx;
  ctx.sink_flag = std::make_unique<std::atomic_bool>(false);
  ctx.restore_sink = false;
  EXPECT_EQ(audio::testing::start_audio_control_for_test(ctx), 0)
    << "start_audio_control_for_test must return 0 when no controller is created.";
  EXPECT_GE(calls, 1) << "start_audio_control_for_test must call the installed controller factory.";
}

/**
 * @brief `start_audio_control` returns 0 with a fake controller and calls the installed factory.
 */
TEST_F(HandoffAudioTest, StartReturnsZeroWithFakeController) {
  int calls = 0;
  audio::testing::set_audio_control_factory_for_test([&calls]() {
    ++calls;
    return std::make_unique<fake_audio_control_t>();
  });

  audio::audio_ctx_t ctx;
  ctx.sink_flag = std::make_unique<std::atomic_bool>(false);
  ctx.restore_sink = false;
  EXPECT_EQ(audio::testing::start_audio_control_for_test(ctx), 0)
    << "start_audio_control_for_test must return 0 with a fake controller.";
  EXPECT_GE(calls, 1) << "start_audio_control_for_test must call the installed controller factory.";
}

/**
 * @brief A normal (non-draining) stop restores the host sink once when `restore_sink` is set.
 */
TEST_F(HandoffAudioTest, StopRestoresHostSinkWhenNotDraining) {
  audio::audio_ctx_t ctx;
  auto control = std::make_unique<fake_audio_control_t>();
  auto *view = control.get();
  make_ctx(ctx, std::move(control), "sink-sunshine-stereo", true, "alsa_output.host");

  audio::testing::stop_audio_control_for_test(ctx);
  ASSERT_EQ(view->sinks.size(), 1u) << "A normal stop with restore_sink and a host sink must call set_sink once.";
  EXPECT_EQ(view->sinks.front(), "alsa_output.host") << "A normal stop must restore the host sink.";
}

/**
 * @brief A stop without `restore_sink` never changes the default sink.
 */
TEST_F(HandoffAudioTest, StopWithoutRestoreFlagDoesNotRestore) {
  audio::audio_ctx_t ctx;
  auto control = std::make_unique<fake_audio_control_t>();
  auto *view = control.get();
  make_ctx(ctx, std::move(control), "sink-sunshine-stereo", false, "alsa_output.host");

  audio::testing::stop_audio_control_for_test(ctx);
  EXPECT_TRUE(view->sinks.empty()) << "A stop with restore_sink false must not call set_sink.";
}

#ifdef __linux__
/**
 * @brief While draining, a stop does not restore the default sink, even with `restore_sink` and a host sink set.
 */
TEST_F(HandoffAudioTest, StopSkipsRestoreWhileDraining) {
  ASSERT_TRUE(make_draining()) << "Precondition: the runtime must be in mode draining.";

  audio::audio_ctx_t ctx;
  auto control = std::make_unique<fake_audio_control_t>();
  auto *view = control.get();
  make_ctx(ctx, std::move(control), "sink-sunshine-stereo", true, "alsa_output.host");

  audio::testing::stop_audio_control_for_test(ctx);
  EXPECT_TRUE(view->sinks.empty()) << "A draining process must not restore the default sink when its sessions end.";
}

/**
 * @brief A stop while draining leaves the controller and the context fields as they were.
 */
TEST_F(HandoffAudioTest, StopWhileDrainingKeepsContextState) {
  ASSERT_TRUE(make_draining()) << "Precondition: the runtime must be in mode draining.";

  audio::audio_ctx_t ctx;
  make_ctx(ctx, std::make_unique<fake_audio_control_t>(), "sink-sunshine-stereo", true, "alsa_output.host");

  audio::testing::stop_audio_control_for_test(ctx);
  EXPECT_TRUE(ctx.restore_sink) << "A draining stop must not change restore_sink.";
  EXPECT_EQ(ctx.virtual_sink, "sink-sunshine-stereo") << "A draining stop must not change virtual_sink.";
  EXPECT_EQ(ctx.sink.host, "alsa_output.host") << "A draining stop must not change the host sink.";
}

/**
 * @brief The handoff host sink from `SUNSHINE_HANDOFF_HOST_SINK` is kept verbatim for `hooks::host_sink()`.
 */
TEST_F(HandoffAudioTest, HandoffHostSinkIsKeptFromEnvironment) {
  fake_env_t fake;
  fake.vars["SUNSHINE_HANDOFF_FDS"] = "https:3,http:4,webui:5,rtsp:6";
  fake.vars["SUNSHINE_HANDOFF_CHANNEL"] = "7";
  fake.vars["SUNSHINE_HANDOFF_UDP_BASE"] = "48100";
  fake.vars["SUNSHINE_HANDOFF_GENERATION"] = "1";
  fake.vars["SUNSHINE_HANDOFF_HOST_SINK"] = "alsa_output.host_card";
  auto env = fake.make();
  ASSERT_EQ(handoff::runtime().early_init(env), "") << "Precondition: the handoff start variables must be accepted.";

  EXPECT_EQ(handoff::hooks::host_sink(), std::optional<std::string> {"alsa_output.host_card"})
    << "The host sink from SUNSHINE_HANDOFF_HOST_SINK must be returned unchanged.";
}

/**
 * @brief An empty `SUNSHINE_HANDOFF_HOST_SINK` means no host sink.
 */
TEST_F(HandoffAudioTest, EmptyHandoffHostSinkMeansNone) {
  fake_env_t fake;
  fake.vars["SUNSHINE_HANDOFF_FDS"] = "https:3,http:4,webui:5,rtsp:6";
  fake.vars["SUNSHINE_HANDOFF_CHANNEL"] = "7";
  fake.vars["SUNSHINE_HANDOFF_UDP_BASE"] = "48100";
  fake.vars["SUNSHINE_HANDOFF_GENERATION"] = "1";
  fake.vars["SUNSHINE_HANDOFF_HOST_SINK"] = "";
  auto env = fake.make();
  ASSERT_EQ(handoff::runtime().early_init(env), "") << "Precondition: the handoff start variables must be accepted.";

  EXPECT_FALSE(handoff::hooks::host_sink().has_value()) << "An empty SUNSHINE_HANDOFF_HOST_SINK must mean no host sink.";
}

/**
 * @brief A normal start has no handoff host sink.
 */
TEST_F(HandoffAudioTest, NormalStartHasNoHandoffHostSink) {
  EXPECT_FALSE(handoff::hooks::host_sink().has_value())
    << "A process that was never started by a handoff has no host sink from the handoff.";
}
#endif
