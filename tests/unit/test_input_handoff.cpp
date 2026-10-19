/**
 * @file tests/unit/test_input_handoff.cpp
 * @brief Blind tests for the drain-handoff parts of input: the stable virtual device ids per generation
 * (`input::device_ids::*`), and the lazy creation of the platform input backend (`input::ensure_platform_input()`,
 * `input::init()`) through the `set_platform_input_factory_for_test` seam.
 *
 * Written from `docs/drain-handoff.md` ("Other resources", "Virtual input devices"), `src/input.h` and
 * `src/handoff/runtime.h` only. Every test resets the platform input seam, and the handoff tests reset the process-wide
 * handoff runtime, so no state survives from one test to the next.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "src/input.h"
#include "src/platform/common.h"

#ifdef __linux__
  #include "src/handoff/hooks.h"
  #include "src/handoff/runtime.h"
#endif

namespace {
  /**
   * @brief Fixture that installs a counting fake for the platform input factory.
   *
   * The fake returns an empty backend, so no virtual device is created. `calls` counts factory invocations.
   */
  class HandoffInputFactoryTest: public ::testing::Test {
  protected:
    /**
     * @brief Install the counting factory before each test.
     */
    void SetUp() override {
#ifdef __linux__
      handoff::runtime().reset_for_test();
#endif
      calls = 0;
      input::testing::set_platform_input_factory_for_test([this]() {
        ++calls;
        return platf::input_t {};
      });
    }

    /**
     * @brief Restore the platform default factory and drop the fake backend after each test.
     */
    void TearDown() override {
      input::testing::set_platform_input_factory_for_test({});
#ifdef __linux__
      handoff::runtime().reset_for_test();
#endif
    }

    std::atomic<int> calls {0};  ///< Number of times the fake factory ran.
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
   * @brief Fixture for tests that make the process-wide handoff runtime a handoff child (reset by the base fixture).
   */
  class HandoffInputRuntimeTest: public HandoffInputFactoryTest {
  protected:
    /**
     * @brief Make the process a handoff child with the given generation, through `early_init`.
     *
     * @param generation Value for `SUNSHINE_HANDOFF_GENERATION`.
     * @return `true` when `early_init` accepted the variables (an empty error string).
     */
    bool start_as_handoff_child(unsigned generation) {
      fake_env_t fake;
      fake.vars["SUNSHINE_HANDOFF_FDS"] = "https:3,http:4,webui:5,rtsp:6";
      fake.vars["SUNSHINE_HANDOFF_CHANNEL"] = "7";
      fake.vars["SUNSHINE_HANDOFF_UDP_BASE"] = "48100";
      fake.vars["SUNSHINE_HANDOFF_GENERATION"] = std::to_string(generation);
      auto env = fake.make();
      return handoff::runtime().early_init(env).empty();
    }
  };
#endif
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Stable device ids (`input::device_ids::*`). Pure functions: no runtime, no device.
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Generation 0 gives exactly the upstream ids for keyboard, mouse, touchscreen and pen tablet.
 */
TEST(HandoffInputDeviceIdsTest, GenerationZeroGivesUpstreamIds) {
  EXPECT_EQ(input::device_ids::keyboard(0), "sunshine-keyboard") << "Generation 0 keyboard id must be the upstream id.";
  EXPECT_EQ(input::device_ids::mouse(0), "sunshine-mouse") << "Generation 0 mouse id must be the upstream id.";
  EXPECT_EQ(input::device_ids::touchscreen(0), "sunshine-touchscreen")
    << "Generation 0 touchscreen id must be the upstream id.";
  EXPECT_EQ(input::device_ids::pen_tablet(0), "sunshine-pen-tablet") << "Generation 0 pen tablet id must be the upstream id.";
}

/**
 * @brief Generation N appends `-g<N>` to keyboard, mouse, touchscreen and pen tablet ids.
 */
TEST(HandoffInputDeviceIdsTest, GenerationNAppendsGSuffix) {
  EXPECT_EQ(input::device_ids::keyboard(7), "sunshine-keyboard-g7") << "Generation 7 keyboard id has the wrong format.";
  EXPECT_EQ(input::device_ids::mouse(7), "sunshine-mouse-g7") << "Generation 7 mouse id has the wrong format.";
  EXPECT_EQ(input::device_ids::touchscreen(7), "sunshine-touchscreen-g7")
    << "Generation 7 touchscreen id has the wrong format.";
  EXPECT_EQ(input::device_ids::pen_tablet(7), "sunshine-pen-tablet-g7")
    << "Generation 7 pen tablet id has the wrong format.";
}

/**
 * @brief The generation is written in decimal, also above 65535 (no wrap in the id text).
 */
TEST(HandoffInputDeviceIdsTest, GenerationIsDecimalAboveSixteenBits) {
  EXPECT_EQ(input::device_ids::keyboard(1234), "sunshine-keyboard-g1234") << "Generation must be written in decimal.";
  EXPECT_EQ(input::device_ids::keyboard(65536), "sunshine-keyboard-g65536")
    << "Keyboard ids must keep the full generation above 65535; only MAC addresses wrap.";
  EXPECT_EQ(input::device_ids::mouse(65535), "sunshine-mouse-g65535") << "Mouse id at generation 65535 has the wrong format.";
}

/**
 * @brief Two different generations never give the same keyboard, mouse, touchscreen or pen tablet id.
 */
TEST(HandoffInputDeviceIdsTest, DifferentGenerationsGiveDifferentIds) {
  EXPECT_NE(input::device_ids::keyboard(1), input::device_ids::keyboard(2))
    << "Keyboard ids of generations 1 and 2 must differ.";
  EXPECT_NE(input::device_ids::keyboard(1), input::device_ids::keyboard(0))
    << "Keyboard id of generation 1 must differ from the upstream id.";
  EXPECT_NE(input::device_ids::mouse(1), input::device_ids::mouse(0)) << "Mouse id of generation 1 must differ from upstream.";
  EXPECT_NE(input::device_ids::touchscreen(1), input::device_ids::touchscreen(2))
    << "Touchscreen ids of generations 1 and 2 must differ.";
  EXPECT_NE(input::device_ids::pen_tablet(1), input::device_ids::pen_tablet(2))
    << "Pen tablet ids of generations 1 and 2 must differ.";
}

/**
 * @brief Generation 0 gamepad ids are `sunshine-gamepad-<idx>` with the decimal global index.
 */
TEST(HandoffInputDeviceIdsTest, GamepadGenerationZeroGivesUpstreamIds) {
  EXPECT_EQ(input::device_ids::gamepad(0, 3), "sunshine-gamepad-3") << "Generation 0 gamepad id for index 3 is wrong.";
  EXPECT_EQ(input::device_ids::gamepad(0, 0), "sunshine-gamepad-0") << "Generation 0 gamepad id for index 0 is wrong.";
  EXPECT_EQ(input::device_ids::gamepad(0, 12), "sunshine-gamepad-12")
    << "Generation 0 gamepad id for index 12 must use the decimal index without padding.";
}

/**
 * @brief Generation N gives `sunshine-gamepad-<gen>-<idx>` for non-MAC gamepads.
 */
TEST(HandoffInputDeviceIdsTest, GamepadGenerationNIncludesGeneration) {
  EXPECT_EQ(input::device_ids::gamepad(7, 3), "sunshine-gamepad-7-3") << "Generation 7 gamepad id for index 3 is wrong.";
  EXPECT_EQ(input::device_ids::gamepad(65536, 3), "sunshine-gamepad-65536-3")
    << "Gamepad ids must keep the full decimal generation above 65535.";
  EXPECT_NE(input::device_ids::gamepad(1, 3), input::device_ids::gamepad(2, 3))
    << "Gamepad ids of generations 1 and 2 at the same index must differ.";
}

/**
 * @brief Generation 0 gamepad MAC addresses are `02:00:00:00:00:<idx>` with a two-digit lowercase hex index.
 */
TEST(HandoffInputDeviceIdsTest, GamepadMacGenerationZeroUsesUpstreamBytes) {
  EXPECT_EQ(input::device_ids::gamepad_mac(0, 3), "02:00:00:00:00:03") << "Generation 0 MAC for slot 3 is wrong.";
  EXPECT_EQ(input::device_ids::gamepad_mac(0, 0), "02:00:00:00:00:00") << "Generation 0 MAC for slot 0 is wrong.";
  EXPECT_EQ(input::device_ids::gamepad_mac(0, 10), "02:00:00:00:00:0a")
    << "Slot 10 must be written as two-digit lowercase hex (0a).";
  EXPECT_EQ(input::device_ids::gamepad_mac(0, 255), "02:00:00:00:00:ff")
    << "Slot 255 must be written as two-digit lowercase hex (ff).";
}

/**
 * @brief Generation N puts its two bytes into MAC bytes 3 and 4, as two-digit lowercase hex.
 */
TEST(HandoffInputDeviceIdsTest, GamepadMacGenerationNCarriesBothBytes) {
  EXPECT_EQ(input::device_ids::gamepad_mac(7, 3), "02:00:00:07:00:03")
    << "Generation 7 must set the low byte (07) in MAC byte 4 and leave the high byte (00) in byte 3.";
  EXPECT_EQ(input::device_ids::gamepad_mac(0x1234, 3), "02:00:12:34:00:03")
    << "Generation 0x1234 must write high byte 12 and low byte 34.";
  EXPECT_EQ(input::device_ids::gamepad_mac(0xABCD, 0x0A), "02:00:ab:cd:00:0a")
    << "Generation bytes must be lowercase hex and the slot byte must be 0a.";
  EXPECT_EQ(input::device_ids::gamepad_mac(0x0102, 3), "02:00:01:02:00:03")
    << "Generation 0x0102 must write high byte 01 and low byte 02.";
}

/**
 * @brief The MAC keeps its format: 17 characters, six colon-separated pairs, the locally administered 02 prefix.
 */
TEST(HandoffInputDeviceIdsTest, GamepadMacHasSixPairsWithLocalPrefix) {
  const std::string mac = input::device_ids::gamepad_mac(0xBEEF, 200);
  ASSERT_EQ(mac.size(), 17u) << "A MAC string must be 17 characters (six pairs and five colons), got: " << mac;
  EXPECT_EQ(mac.substr(0, 6), "02:00:") << "The MAC must start with the locally administered prefix 02:00, got: " << mac;
  for (std::size_t pos : {2u, 5u, 8u, 11u, 14u}) {
    EXPECT_EQ(mac[pos], ':') << "A colon must sit at position " << pos << " of the MAC, got: " << mac;
  }
  EXPECT_EQ(mac, "02:00:be:ef:00:c8") << "Generation 0xBEEF slot 200 has the wrong MAC.";
}

/**
 * @brief Generations above 65535 wrap in the MAC (only the low 16 bits are used), so 65536 + n reuses the MAC of n.
 */
TEST(HandoffInputDeviceIdsTest, GamepadMacWrapsAboveSixteenBits) {
  EXPECT_EQ(input::device_ids::gamepad_mac(65536, 3), input::device_ids::gamepad_mac(0, 3))
    << "Generation 65536 must give the MAC of generation 0 (only the low 16 bits are used).";
  EXPECT_EQ(input::device_ids::gamepad_mac(65536 + 7, 3), "02:00:00:07:00:03")
    << "Generation 65543 must give the MAC of generation 7.";
  EXPECT_EQ(input::device_ids::gamepad_mac(65535, 3), "02:00:ff:ff:00:03")
    << "Generation 65535 is the largest generation that keeps its own MAC bytes.";
}

/**
 * @brief Calling an id function twice with the same input gives the same string (pure functions).
 */
TEST(HandoffInputDeviceIdsTest, IdsArePureAndRepeatable) {
  EXPECT_EQ(input::device_ids::keyboard(9), input::device_ids::keyboard(9))
    << "keyboard() must be a pure function of its generation.";
  EXPECT_EQ(input::device_ids::gamepad_mac(9, 4), input::device_ids::gamepad_mac(9, 4))
    << "gamepad_mac() must be a pure function of its inputs.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Lazy platform input backend (`input::ensure_platform_input()`, `input::init()`) with the factory seam.
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Installing a factory does not create the backend; the factory has not run yet.
 */
TEST_F(HandoffInputFactoryTest, FactoryIsNotCalledByInstallation) {
  EXPECT_EQ(calls.load(), 0) << "Installing the factory must not call it.";
  EXPECT_FALSE(input::testing::platform_input_created_for_test())
    << "No backend must exist right after the factory is installed.";
}

/**
 * @brief The first `ensure_platform_input()` creates the backend through the factory, exactly once.
 */
TEST_F(HandoffInputFactoryTest, EnsureCreatesBackendOnce) {
  input::ensure_platform_input();
  EXPECT_EQ(calls.load(), 1) << "The first ensure_platform_input() call must run the factory exactly once.";

  input::ensure_platform_input();
  input::ensure_platform_input();
  EXPECT_EQ(calls.load(), 1) << "Later ensure_platform_input() calls must not run the factory again.";
}

/**
 * @brief Many threads calling `ensure_platform_input()` at the same time still run the factory once.
 */
TEST_F(HandoffInputFactoryTest, ConcurrentEnsureCreatesBackendOnce) {
  // The factory sleeps so that the callers overlap inside call_once.
  input::testing::set_platform_input_factory_for_test([this]() {
    ++calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return platf::input_t {};
  });

  constexpr int thread_count = 8;
  std::atomic<bool> go {false};
  std::vector<std::thread> threads;
  threads.reserve(thread_count);
  for (int i = 0; i < thread_count; ++i) {
    threads.emplace_back([&go]() {
      while (!go.load()) {
        std::this_thread::yield();
      }
      input::ensure_platform_input();
    });
  }
  go = true;
  for (auto &thread : threads) {
    thread.join();
  }

  EXPECT_EQ(calls.load(), 1) << "Eight concurrent callers must create the backend with one factory call, got " << calls.load();
  EXPECT_TRUE(input::testing::platform_input_created_for_test())
    << "After concurrent ensure_platform_input() calls the backend must be marked as created.";
}

/**
 * @brief `set_platform_input` counts as the once-created backend: a later ensure does not run the factory.
 */
TEST_F(HandoffInputFactoryTest, SetPlatformInputCountsAsCreated) {
  input::testing::set_platform_input({});
  input::ensure_platform_input();
  EXPECT_EQ(calls.load(), 0) << "After set_platform_input() the factory must not run on ensure_platform_input().";
}

/**
 * @brief Installing a new factory rearms the once-flag: the next ensure calls the new factory.
 */
TEST_F(HandoffInputFactoryTest, InstallingFactoryRearmsOnceFlag) {
  input::ensure_platform_input();
  ASSERT_EQ(calls.load(), 1) << "Precondition: the first factory must have run once.";

  std::atomic<int> second_calls {0};
  input::testing::set_platform_input_factory_for_test([&second_calls]() {
    ++second_calls;
    return platf::input_t {};
  });
  EXPECT_FALSE(input::testing::platform_input_created_for_test())
    << "Installing a new factory must drop the existing backend.";

  input::ensure_platform_input();
  EXPECT_EQ(second_calls.load(), 1) << "After rearming, the next ensure must call the new factory once.";
  EXPECT_EQ(calls.load(), 1) << "The replaced factory must not run again.";
}

/**
 * @brief A normal start (no handoff) makes `init()` create the backend now, through the factory.
 */
TEST_F(HandoffInputFactoryTest, NormalStartInitCreatesBackend) {
  auto guard = input::init();
  EXPECT_NE(guard.get(), nullptr) << "init() must return a cleanup handle even when nothing else is required.";
  EXPECT_EQ(calls.load(), 1) << "A normal start must create the platform input backend during init().";
  guard.reset();
}

/**
 * @brief The cleanup handle from `init()` does not rearm the once-flag: after it runs, no new backend is created.
 */
TEST_F(HandoffInputFactoryTest, InitGuardDoesNotRecreateBackendAfterExit) {
  auto guard = input::init();
  ASSERT_EQ(calls.load(), 1) << "Precondition: init() must have created the backend once.";
  guard.reset();

  input::ensure_platform_input();
  EXPECT_EQ(calls.load(), 1) << "After the init() guard has run, ensure_platform_input() must not recreate the backend.";
}

#ifdef __linux__
/**
 * @brief Handoff start: `init()` creates nothing; the backend appears at the first `ensure_platform_input()`.
 */
TEST_F(HandoffInputRuntimeTest, HandoffStartInitCreatesNothingUntilFirstEnsure) {
  ASSERT_TRUE(start_as_handoff_child(1)) << "Precondition: the handoff start variables must be accepted.";
  ASSERT_TRUE(handoff::hooks::started_by_handoff()) << "Precondition: the process must report a handoff start.";

  auto guard = input::init();
  EXPECT_NE(guard.get(), nullptr) << "init() must return a cleanup handle in handoff mode too.";
  EXPECT_EQ(calls.load(), 0) << "A handoff start must not create the platform input backend in init().";
  EXPECT_FALSE(input::testing::platform_input_created_for_test())
    << "A handoff start must have no backend before its first session.";

  input::ensure_platform_input();
  EXPECT_EQ(calls.load(), 1) << "The first ensure_platform_input() of a handoff start must create the backend once.";
  guard.reset();
}

/**
 * @brief Handoff start: many callers of `ensure_platform_input()` at the first session create the backend once.
 */
TEST_F(HandoffInputRuntimeTest, HandoffStartConcurrentFirstSessionsCreateOnce) {
  ASSERT_TRUE(start_as_handoff_child(2)) << "Precondition: the handoff start variables must be accepted.";
  input::testing::set_platform_input_factory_for_test([this]() {
    ++calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return platf::input_t {};
  });

  std::atomic<bool> go {false};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&go]() {
      while (!go.load()) {
        std::this_thread::yield();
      }
      input::ensure_platform_input();
    });
  }
  go = true;
  for (auto &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(calls.load(), 1) << "Concurrent first sessions of a handoff start must run the factory once, got " << calls.load();
}

/**
 * @brief The generation of a handoff child reaches the device ids through the hooks.
 */
TEST_F(HandoffInputRuntimeTest, HandoffGenerationReachesDeviceIds) {
  ASSERT_TRUE(start_as_handoff_child(7)) << "Precondition: the handoff start variables must be accepted.";
  EXPECT_EQ(handoff::hooks::generation(), 7u) << "A handoff child with SUNSHINE_HANDOFF_GENERATION=7 must report generation 7.";
  EXPECT_EQ(input::device_ids::keyboard(handoff::hooks::generation()), "sunshine-keyboard-g7")
    << "The keyboard id of a generation 7 handoff child must carry the generation.";
  EXPECT_EQ(input::device_ids::gamepad_mac(handoff::hooks::generation(), 1), "02:00:00:07:00:01")
    << "The gamepad MAC of a generation 7 handoff child must carry the generation.";
}

/**
 * @brief A normal start has generation 0, so the hooks give the upstream ids.
 */
TEST_F(HandoffInputRuntimeTest, NormalStartGenerationGivesUpstreamIds) {
  EXPECT_EQ(handoff::hooks::generation(), 0u) << "A process that was never started by a handoff must have generation 0.";
  EXPECT_EQ(input::device_ids::keyboard(handoff::hooks::generation()), "sunshine-keyboard")
    << "A normal start must keep the upstream keyboard id.";
}
#endif
