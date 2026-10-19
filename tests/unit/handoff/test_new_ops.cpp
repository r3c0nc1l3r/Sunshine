/**
 * @file tests/unit/handoff/test_new_ops.cpp
 * @brief Blind tests for the Sunshine new-side ops (src/handoff/new_ops.h): the deferral invariant, the UPnP unmap
 * scope order, the probe-override ordering, the milestone waiters, `exit_failure`, and the abort versus `COMMIT` rule.
 *
 * Written from `docs/drain-handoff.md` (steps 3 and 5, "Other resources"), `new_ops.h`, `new_side.h`, `runtime.h`,
 * `probe.h` and `hooks.h` only. Every platform call is replaced by a recording seam; nothing publishes, maps, shows a
 * tray or exits.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <cstdint>
  #include <filesystem>
  #include <functional>
  #include <future>
  #include <map>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <thread>
  #include <vector>

  // platform includes
  #include <cerrno>
  #include <fcntl.h>
  #include <sys/eventfd.h>
  #include <sys/socket.h>
  #include <unistd.h>

  // local includes
  #include "src/handoff/coordinator.h"
  #include "src/handoff/hooks.h"
  #include "src/handoff/new_ops.h"
  #include "src/handoff/runtime.h"
  #include "src/upnp.h"
  #include "src/video.h"

using namespace std::chrono_literals;

namespace {
  namespace protocol = handoff::protocol;
  using handoff::new_stage_e;
  using handoff::validation_e;

  /**
   * @brief Thread-safe ordered record of every seam call.
   */
  class recorder_t {
  public:
    /**
     * @brief Append an entry.
     *
     * @param entry Entry text.
     */
    void add(std::string entry) {
      std::lock_guard lock {_mutex};
      _entries.push_back(std::move(entry));
    }

    /**
     * @brief Copy of all entries.
     *
     * @return Entries in call order.
     */
    std::vector<std::string> all() const {
      std::lock_guard lock {_mutex};
      return _entries;
    }

    /**
     * @brief Forget all entries.
     */
    void clear() {
      std::lock_guard lock {_mutex};
      _entries.clear();
    }

    /**
     * @brief Number of entries that start with `prefix`.
     *
     * @param prefix Prefix.
     * @return Count.
     */
    int count(const std::string &prefix) const {
      std::lock_guard lock {_mutex};
      return static_cast<int>(std::count_if(_entries.begin(), _entries.end(), [&](const std::string &e) {
        return e.rfind(prefix, 0) == 0;
      }));
    }

    /**
     * @brief Index of the first entry that starts with `prefix`.
     *
     * @param prefix Prefix.
     * @return Index, or -1.
     */
    int index_of(const std::string &prefix) const {
      std::lock_guard lock {_mutex};
      for (std::size_t i = 0; i < _entries.size(); ++i) {
        if (_entries[i].rfind(prefix, 0) == 0) {
          return static_cast<int>(i);
        }
      }
      return -1;
    }

    /**
     * @brief The first entry that starts with `prefix`.
     *
     * @param prefix Prefix.
     * @return The entry, or an empty string.
     */
    std::string first(const std::string &prefix) const {
      std::lock_guard lock {_mutex};
      for (const auto &e : _entries) {
        if (e.rfind(prefix, 0) == 0) {
          return e;
        }
      }
      return {};
    }

  private:
    mutable std::mutex _mutex;  ///< Guards the entries.
    std::vector<std::string> _entries;  ///< Entries.
  };

  /**
   * @brief UPnP guard stand-in that counts its destruction.
   */
  class fake_guard_t: public platf::deinit_t {
  public:
    /**
     * @brief Create a guard.
     *
     * @param destroyed Counter incremented by the destructor.
     */
    explicit fake_guard_t(std::atomic<int> &destroyed):
        _destroyed(destroyed) {}

    /**
     * @brief Count the destruction.
     */
    ~fake_guard_t() override {
      ++_destroyed;
    }

  private:
    std::atomic<int> &_destroyed;  ///< Counter.
  };

  /**
   * @brief Poll a condition until it holds or the limit passes.
   *
   * @param pred Condition.
   * @param limit Longest wait.
   * @return `true` when the condition held.
   */
  bool wait_until(const std::function<bool()> &pred, std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
      if (pred()) {
        return true;
      }
      std::this_thread::sleep_for(5ms);
    }
    return pred();
  }

  /**
   * @brief Name of a stage for log entries.
   *
   * @param stage Stage.
   * @return Name.
   */
  std::string stage_name(new_stage_e stage) {
    switch (stage) {
      case new_stage_e::starting:
        return "starting";
      case new_stage_e::ready:
        return "ready";
      case new_stage_e::go:
        return "go";
      case new_stage_e::committing:
        return "committing";
      case new_stage_e::committed:
        return "committed";
      case new_stage_e::main:
        return "main";
    }
    return "?";
  }

  /**
   * @brief Name of an unmap scope for log entries.
   *
   * @param scope Scope.
   * @return Name.
   */
  std::string scope_name(upnp::unmap_scope_e scope) {
    switch (scope) {
      case upnp::unmap_scope_e::all:
        return "all";
      case upnp::unmap_scope_e::udp_only:
        return "udp_only";
      case upnp::unmap_scope_e::none:
        return "none";
    }
    return "?";
  }

  /**
   * @brief Current role of this process's instance file, as text.
   *
   * @return `pending`, `active`, `draining` or `none` (no writer).
   */
  std::string current_role() {
    const auto instance = handoff::runtime().instance_current();
    return instance ? std::string {handoff::to_string(instance->role)} : std::string {"none"};
  }
}  // namespace

/**
 * @brief Fixture: a handoff-started runtime (mode `new_pending`, instance file role `pending`), a channel pair and
 * ops whose every seam records into `rec`.
 */
class HandoffNewOpsTest: public ::testing::Test {
protected:
  /**
   * @brief Build the runtime, the channel and the ops with the default scenario.
   */
  void SetUp() override {
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    _dir = std::filesystem::temp_directory_path() / ("sunshine-t18b-newops-" + std::to_string(::getpid()) + "-" + info->name());
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
    std::filesystem::create_directories(_dir, ec);

    config.port = 47989;
    config.ipv6 = false;
    config.feature_enabled = true;
    config.tray_enabled = true;
    config.servers_listening_timeout = 2000ms;
    config.resume_timeout = 1234ms;

    seams.set_unmap_scope = [this](upnp::unmap_scope_e scope) {
      rec.add("set_unmap_scope(" + scope_name(scope) + ")");
    };
    seams.own_identity = [this] {
      return identity;
    };
    seams.set_probe_override = [this](const protocol::probe_results_t &results) {
      std::this_thread::sleep_for(probe_override_delay);
      {
        std::lock_guard lock {_state_mutex};
        probe_results = results;
      }
      rec.add("set_probe_override");
      probe_installed = true;
    };
    seams.set_cooldown_deadline_ns = [this](std::int64_t value) {
      rec.add("set_cooldown_deadline_ns(" + std::to_string(value) + ")");
    };
    seams.validate_listener = [this](int fd, int port, bool ipv6) {
      rec.add("validate_listener(" + std::to_string(fd) + "," + std::to_string(port) + "," + (ipv6 ? "v6" : "v4") + ")");
      return validate_fn ? validate_fn(fd, port, ipv6) : std::string {};
    };
    seams.reload_all = [this] {
      rec.add("reload_all:stage=" + stage_name(ops->stage()));
      return reload_reason;
    };
    seams.prepare_adoption = [this](const protocol::go_t &) {
      rec.add("prepare_adoption");
      return prepare_reason;
    };
    seams.install_adoption = [this] {
      rec.add(std::string {"install_adoption:mode="} + std::string {handoff::to_string(handoff::runtime().mode())});
    };
    seams.resume_servers = [this](std::chrono::milliseconds timeout) {
      rec.add("resume_servers(" + std::to_string(timeout.count()) + ")");
      return resume_result;
    };
    seams.publish_mdns = [this] {
      rec.add("publish_mdns:role=" + current_role());
      return true;
    };
    seams.start_upnp = [this]() -> std::unique_ptr<platf::deinit_t> {
      rec.add("start_upnp:role=" + current_role());
      if (upnp_returns_guard) {
        return std::make_unique<fake_guard_t>(guards_destroyed);
      }
      return nullptr;
    };
    seams.request_tray_start = [this] {
      rec.add("request_tray_start:role=" + current_role());
    };
    seams.enable_tray_entries = [this] {
      rec.add("enable_tray_entries");
    };
    seams.notify_ready = [this] {
      rec.add("notify_ready");
    };
    seams.start_control_server = [this] {
      rec.add("start_control_server");
    };
    seams.exit_process = [this](int code) {
      rec.add("exit_process(" + std::to_string(code) + ")");
    };
    seams.shutdown_channel = [this](handoff::channel_t &channel_ref) {
      rec.add("shutdown_channel");
      if (channel_ref.fd() != -1) {
        ::shutdown(channel_ref.fd(), SHUT_RDWR);
      }
    };

    rebuild();
  }

  /**
   * @brief Remove the instance file and the temporary directory and reset the process-wide state.
   */
  void TearDown() override {
    handoff::set_new_ops_coordinator(nullptr);
    ops.reset();
    peer.reset();
    channel.reset();
    handoff::runtime().instance_remove();
    handoff::runtime().reset_for_test();
    video::set_handoff_probe_override(std::nullopt);
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
  }

  /**
   * @brief Recreate the runtime (mode `new_pending`, role `pending`), the channel and the ops from the current
   * `config` and `seams`. Clears the record.
   */
  void rebuild() {
    ops.reset();
    peer.reset();
    channel.reset();
    upnp_slot.reset();
    handoff::runtime().instance_remove();
    handoff::runtime().reset_for_test();
    video::set_handoff_probe_override(std::nullopt);

    std::map<std::string, std::string> vars {
      {"SUNSHINE_HANDOFF_FDS", "https:3,http:4,webui:5,rtsp:6"},
      {"SUNSHINE_HANDOFF_CHANNEL", "7"},
      {"SUNSHINE_HANDOFF_UDP_BASE", "48100"},
      {"SUNSHINE_HANDOFF_GENERATION", "1"},
    };
    handoff::early_init_env_t env;
    env.getenv = [&vars](const char *name) -> const char * {
      auto it = vars.find(name);
      return it == vars.end() ? nullptr : it->second.c_str();
    };
    env.unsetenv = [&vars](const char *name) {
      vars.erase(name);
    };
    env.scrub_fds = [] {
    };
    ASSERT_EQ(handoff::runtime().early_init(env), "") << "setup: early_init with a valid fake handoff environment must succeed";

    handoff::instance_t initial;
    initial.pid = ::getpid();
    initial.start_time = 1;
    initial.role = handoff::role_e::pending;
    initial.udp_base = 48100;
    initial.sessions = 0;
    initial.handoff = handoff::state_e::none;
    ASSERT_TRUE(handoff::runtime().open_instance(_dir / "instances", initial)) << "setup: the instance file must be writable";

    const auto pair = handoff::make_channel_pair();
    ASSERT_TRUE(pair.has_value()) << "setup: socketpair";
    channel = std::make_shared<handoff::channel_t>(pair->first);
    peer = std::make_unique<handoff::channel_t>(pair->second);
    rec.clear();
    probe_installed = false;
    guards_destroyed = 0;
    ops = std::make_unique<handoff::sunshine_new_ops_t>(config, channel, upnp_slot, seams);
  }

  /**
   * @brief An old process `HELLO` whose probe identity equals `identity`.
   *
   * @return The message.
   */
  protocol::hello_t old_hello() const {
    protocol::hello_t hello;
    protocol::probe_results_t results;
    results.hevc_mode = 3;
    results.av1_mode = 2;
    results.yuv444_h264 = true;
    results.yuv444_hevc = false;
    results.yuv444_av1 = true;
    hello.probe = results;
    hello.identity = identity;
    return hello;
  }

  /**
   * @brief Make a pollable fd to stand for the old process's pidfd.
   *
   * @return The fd.
   */
  static int make_fake_pidfd() {
    return ::eventfd(0, EFD_CLOEXEC);
  }

  /**
   * @brief Drive the ops through everything before `COMMIT`, calling them directly in protocol order.
   */
  void run_pre_commit() {
    ops->hello();
    ops->accept_old_hello(old_hello(), make_fake_pidfd());
    EXPECT_EQ(ops->validate_listeners(), "") << "setup: validate_listeners must succeed";
    for (int i = 0; i < 4; ++i) {
      ops->server_listening();
    }
    EXPECT_EQ(ops->start_servers_paused(), "") << "setup: start_servers_paused must succeed";
    EXPECT_EQ(ops->reload_all(), "");
    EXPECT_EQ(ops->prepare_adoption(protocol::go_t {}), "");
  }

  /**
   * @brief Run the six `COMMIT` effects in the order `new_side.h` documents.
   */
  void run_commit() {
    ops->install_adoption();
    ops->resume_accept();
    ops->publish_mdns();
    ops->map_upnp();
    ops->start_tray();
    ops->become_active();
  }

  /**
   * @brief Whether any deferred seam (publish, UPnP start, tray start, unmap scope) was called.
   *
   * @return `true` when at least one was.
   */
  bool any_deferred_seam_called() const {
    return rec.count("publish_mdns") > 0 || rec.count("start_upnp") > 0 || rec.count("request_tray_start") > 0 || rec.count("set_unmap_scope") > 0;
  }

  handoff::new_ops_config_t config;  ///< Ops configuration.
  handoff::new_ops_seams_t seams;  ///< Ops seams (all recording).
  recorder_t rec;  ///< Record of seam calls.
  std::atomic<int> guards_destroyed {0};  ///< Fake UPnP guards destroyed.
  std::atomic<bool> probe_installed {false};  ///< The `set_probe_override` seam finished.
  std::chrono::milliseconds probe_override_delay {0};  ///< Delay inside `set_probe_override`.
  protocol::probe_results_t probe_results;  ///< Results given to `set_probe_override`.
  protocol::probe_identity_t identity {"build-1", "/dev/dri/renderD128", 0x1002, 0x73bf, "6.1"};  ///< This host's identity.
  std::function<std::string(int, int, bool)> validate_fn;  ///< Result of `validate_listener`; empty means valid.
  std::string reload_reason;  ///< Result of the `reload_all` seam.
  std::string prepare_reason;  ///< Result of the `prepare_adoption` seam.
  std::vector<std::string> resume_result;  ///< Result of the `resume_servers` seam.
  bool upnp_returns_guard {true};  ///< Whether `start_upnp` returns a guard.
  std::unique_ptr<platf::deinit_t> upnp_slot;  ///< The guard slot.
  std::shared_ptr<handoff::channel_t> channel;  ///< The ops' end of the channel.
  std::unique_ptr<handoff::channel_t> peer;  ///< The old process's end.
  std::unique_ptr<handoff::sunshine_new_ops_t> ops;  ///< Ops under test.

private:
  std::filesystem::path _dir;  ///< Temporary directory.
  std::mutex _state_mutex;  ///< Guards `probe_results`.
};

// ---------------------------------------------------------------------------------------------------------------------
// Deferral invariant
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffNewOpsTest, NothingDeferredIsCalledBeforeCommit) {
  run_pre_commit();
  EXPECT_FALSE(any_deferred_seam_called()) << "publish_mdns, start_upnp, request_tray_start and set_unmap_scope must not be called before COMMIT; log has publish=" << rec.count("publish_mdns") << " upnp=" << rec.count("start_upnp") << " tray=" << rec.count("request_tray_start") << " scope=" << rec.count("set_unmap_scope");
  EXPECT_EQ(rec.count("install_adoption"), 0) << "install_adoption must not run before COMMIT";
  EXPECT_EQ(rec.count("resume_servers"), 0) << "the servers must stay paused until COMMIT";
  EXPECT_EQ(current_role(), "pending") << "the instance file role must be pending before COMMIT";
  EXPECT_EQ(handoff::runtime().mode(), handoff::mode_e::new_pending) << "the runtime mode must be new_pending before COMMIT";
  EXPECT_EQ(rec.count("notify_ready"), 0) << "READY=1 must not be sent before MAIN";
}

TEST_F(HandoffNewOpsTest, CommitCallsDeferredSeamsAfterInstallAdoption) {
  // Positive control for the deferral test above: the very same seams ARE called once COMMIT effects run.
  run_pre_commit();
  run_commit();
  const int install = rec.index_of("install_adoption");
  ASSERT_GE(install, 0) << "install_adoption seam was not called at COMMIT";
  EXPECT_EQ(rec.count("resume_servers"), 1) << "resume_servers must be called once at COMMIT";
  for (const char *name : {"publish_mdns", "start_upnp", "request_tray_start"}) {
    ASSERT_EQ(rec.count(name), 1) << name << " must be called exactly once at COMMIT";
    EXPECT_GT(rec.index_of(name), install) << name << " must be called after install_adoption";
    EXPECT_GT(rec.index_of(name), rec.index_of("resume_servers")) << name << " must be called after resume_servers";
  }
  EXPECT_LT(rec.index_of("publish_mdns"), rec.index_of("start_upnp")) << "COMMIT effect order: publish_mdns before map_upnp";
  EXPECT_LT(rec.index_of("start_upnp"), rec.index_of("request_tray_start")) << "COMMIT effect order: map_upnp before start_tray";
}

TEST_F(HandoffNewOpsTest, InstallAdoptionMovesModeToNewActiveBeforeTheSeam) {
  run_pre_commit();
  ops->install_adoption();
  EXPECT_EQ(rec.first("install_adoption"), "install_adoption:mode=new_active") << "the runtime mode must already be new_active when the install_adoption seam runs";
  EXPECT_EQ(handoff::runtime().mode(), handoff::mode_e::new_active) << "mode after install_adoption";
  EXPECT_EQ(ops->stage(), new_stage_e::committing) << "stage after install_adoption";
}

TEST_F(HandoffNewOpsTest, RoleStaysPendingForEveryEffectUntilBecomeActive) {
  run_pre_commit();
  run_commit();
  EXPECT_EQ(rec.first("publish_mdns"), "publish_mdns:role=pending") << "role must still be pending while publish_mdns runs";
  EXPECT_EQ(rec.first("start_upnp"), "start_upnp:role=pending") << "role must still be pending while start_upnp runs";
  EXPECT_EQ(rec.first("request_tray_start"), "request_tray_start:role=pending") << "role must still be pending while the tray starts";
  EXPECT_EQ(current_role(), "active") << "become_active must write role active";
  EXPECT_EQ(handoff::runtime().instance_current()->handoff, handoff::state_e::draining) << "become_active must write state draining";
  EXPECT_EQ(ops->stage(), new_stage_e::committed) << "stage after become_active";
}

TEST_F(HandoffNewOpsTest, RoleIsPendingAfterEveryEffectBeforeBecomeActive) {
  run_pre_commit();
  ops->install_adoption();
  ops->resume_accept();
  ops->publish_mdns();
  ops->map_upnp();
  ops->start_tray();
  EXPECT_EQ(current_role(), "pending") << "only become_active may write role active";
}

TEST_F(HandoffNewOpsTest, ExitFailureCallsNoDeferredSeam) {
  run_pre_commit();
  ops->exit_failure();
  EXPECT_FALSE(any_deferred_seam_called()) << "a process that exits before COMMIT must publish, map and show nothing";
  EXPECT_EQ(current_role(), "pending") << "a process that exits before COMMIT must leave role pending";
  EXPECT_EQ(rec.count("install_adoption"), 0) << "nothing may be installed when exit_failure runs";
}

// ---------------------------------------------------------------------------------------------------------------------
// UPnP unmap scope
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffNewOpsTest, UnmapScopeUdpOnlyIsSetBeforeStartUpnp) {
  run_pre_commit();
  run_commit();
  ASSERT_EQ(rec.count("set_unmap_scope(udp_only)"), 1) << "set_unmap_scope(udp_only) must be called exactly once at COMMIT";
  ASSERT_EQ(rec.count("start_upnp"), 1) << "start_upnp must be called at COMMIT";
  EXPECT_LT(rec.index_of("set_unmap_scope(udp_only)"), rec.index_of("start_upnp")) << "the unmap scope must be udp_only BEFORE start_upnp, so a graceful exit before MAIN never unmaps the shared TCP ports";
  EXPECT_EQ(rec.count("set_unmap_scope(all)"), 0) << "the scope must stay udp_only until MAIN";
}

TEST_F(HandoffNewOpsTest, UnmapScopeUdpOnlyIsSetEvenWhenUpnpIsOff) {
  upnp_returns_guard = false;
  rebuild();
  run_pre_commit();
  run_commit();
  EXPECT_EQ(rec.count("set_unmap_scope(udp_only)"), 1) << "the scope is set before start_upnp even when UPnP turns out to be off";
  EXPECT_EQ(rec.count("start_upnp"), 1) << "start_upnp is still called (it returns null when UPnP is off)";
  EXPECT_FALSE(upnp_slot) << "a null guard must leave the slot empty";
}

TEST_F(HandoffNewOpsTest, UpnpGuardGoesIntoTheSlot) {
  run_pre_commit();
  run_commit();
  EXPECT_TRUE(upnp_slot) << "the guard returned by start_upnp must be stored in the slot";
  EXPECT_EQ(guards_destroyed.load(), 0) << "the guard must stay alive after map_upnp (main destroys it at shutdown)";
  upnp_slot.reset();
  EXPECT_EQ(guards_destroyed.load(), 1) << "the slot must own the guard";
}

TEST_F(HandoffNewOpsTest, OnMainSetsUnmapScopeAll) {
  run_pre_commit();
  run_commit();
  ops->on_main();
  ASSERT_EQ(rec.count("set_unmap_scope(all)"), 1) << "on_main must call set_unmap_scope(all) once";
  EXPECT_GT(rec.index_of("set_unmap_scope(all)"), rec.index_of("start_upnp")) << "scope all must come after UPnP started";
  EXPECT_GT(rec.index_of("set_unmap_scope(all)"), rec.index_of("set_unmap_scope(udp_only)")) << "scope all must come after udp_only";
  EXPECT_EQ(rec.count("set_unmap_scope(none)"), 0) << "set_unmap_scope must never be called with none";
  EXPECT_EQ(rec.count("set_unmap_scope"), 2) << "set_unmap_scope is called exactly twice in a handoff-started process: udp_only at COMMIT, all at MAIN";
}

TEST_F(HandoffNewOpsTest, OnMainRunsItsStepsInTheDocumentedOrder) {
  run_pre_commit();
  run_commit();
  ops->on_main();
  const int scope = rec.index_of("set_unmap_scope(all)");
  const int tray = rec.index_of("enable_tray_entries");
  const int ready = rec.index_of("notify_ready");
  const int control = rec.index_of("start_control_server");
  ASSERT_GE(scope, 0) << "set_unmap_scope(all) not called";
  ASSERT_GE(tray, 0) << "enable_tray_entries not called";
  ASSERT_GE(ready, 0) << "notify_ready not called";
  ASSERT_GE(control, 0) << "start_control_server not called";
  EXPECT_LT(scope, tray) << "order: set_unmap_scope(all) before enable_tray_entries";
  EXPECT_LT(tray, ready) << "order: enable_tray_entries before notify_ready";
  EXPECT_LT(ready, control) << "order: notify_ready before start_control_server";
  EXPECT_EQ(handoff::runtime().mode(), handoff::mode_e::normal) << "MAIN moves new_active to normal";
  EXPECT_TRUE(handoff::runtime().is_main()) << "MAIN makes the process the main process";
  EXPECT_EQ(ops->stage(), new_stage_e::main) << "stage after on_main";
}

TEST_F(HandoffNewOpsTest, OnMainIsIdempotent) {
  run_pre_commit();
  run_commit();
  ops->on_main();
  const auto before = rec.all().size();
  ops->on_main();
  EXPECT_EQ(rec.all().size(), before) << "a second on_main must call no seam";
  EXPECT_EQ(rec.count("notify_ready"), 1) << "READY=1 must be sent exactly once";
}

TEST_F(HandoffNewOpsTest, OnMainBeforeCommittedIsIgnored) {
  run_pre_commit();
  ops->on_main();
  EXPECT_EQ(rec.all().size(), 0u) << "on_main before COMMIT must call no seam (a confused peer must not make this the main process)";
  EXPECT_FALSE(handoff::runtime().is_main()) << "on_main before COMMIT must not set the main flag";
  EXPECT_EQ(handoff::runtime().mode(), handoff::mode_e::new_pending) << "mode must not change";
  EXPECT_EQ(ops->stage(), new_stage_e::go) << "stage must not change";
}

TEST_F(HandoffNewOpsTest, OnMainBetweenCommitEffectsIsIgnored) {
  run_pre_commit();
  ops->install_adoption();
  rec.clear();
  ops->on_main();
  EXPECT_EQ(rec.count("notify_ready"), 0) << "on_main while the stage is committing must not send READY=1";
  EXPECT_EQ(rec.count("set_unmap_scope"), 0) << "on_main while the stage is committing must not change the scope";
  EXPECT_FALSE(handoff::runtime().is_main()) << "not main yet";
}

TEST_F(HandoffNewOpsTest, ReadyAndControlAreNotSentBeforeMain) {
  run_pre_commit();
  run_commit();
  EXPECT_EQ(rec.count("notify_ready"), 0) << "no READY=1 between COMMIT and MAIN";
  EXPECT_EQ(rec.count("start_control_server"), 0) << "no control socket before MAIN";
  EXPECT_EQ(rec.count("enable_tray_entries"), 0) << "tray Quit/Restart stay disabled before MAIN";
  EXPECT_FALSE(handoff::runtime().is_main()) << "not main before MAIN";
}

// ---------------------------------------------------------------------------------------------------------------------
// Failed resume
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffNewOpsTest, ResumeIsCalledWithTheConfiguredTimeout) {
  run_pre_commit();
  run_commit();
  EXPECT_EQ(rec.count("resume_servers(1234)"), 1) << "resume_servers must get config.resume_timeout (1234 ms) exactly once; log entry: " << rec.first("resume_servers");
}

TEST_F(HandoffNewOpsTest, FailedResumeSkipsPublishUpnpAndTray) {
  resume_result = {"https", "rtsp"};
  rebuild();
  run_pre_commit();
  run_commit();
  EXPECT_TRUE(ops->commit_failed()) << "a non-empty resume answer must set commit_failed";
  EXPECT_EQ(rec.count("publish_mdns"), 0) << "a server that does not accept must not be announced";
  EXPECT_EQ(rec.count("start_upnp"), 0) << "no UPnP after a failed resume";
  EXPECT_EQ(rec.count("request_tray_start"), 0) << "no tray after a failed resume";
  EXPECT_EQ(rec.count("set_unmap_scope"), 0) << "set_unmap_scope is not called when map_upnp does nothing";
  EXPECT_EQ(current_role(), "pending") << "the role stays pending after a failed resume";
  EXPECT_EQ(ops->stage(), new_stage_e::committed) << "become_active still sets the stage";
  EXPECT_EQ(rec.count("exit_process"), 0) << "the ops do not exit by themselves after a failed resume (entry does, after the wiring)";
}

TEST_F(HandoffNewOpsTest, CommitFailedIsFalseAfterASuccessfulCommit) {
  EXPECT_FALSE(ops->commit_failed()) << "commit_failed before COMMIT";
  run_pre_commit();
  run_commit();
  EXPECT_FALSE(ops->commit_failed()) << "commit_failed after a successful COMMIT";
  EXPECT_EQ(current_role(), "active") << "control: a successful COMMIT writes active";
}

// ---------------------------------------------------------------------------------------------------------------------
// exit_failure
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffNewOpsTest, ExitFailureExitsWithCodeOneThroughTheSeam) {
  ops->exit_failure();
  EXPECT_EQ(rec.count("exit_process(1)"), 1) << "exit_failure must call exit_process(1) exactly once";
  EXPECT_EQ(rec.count("exit_process"), 1) << "exit_failure must call exit_process with no other code";
}

TEST_F(HandoffNewOpsTest, ExitFailureTwiceExitsOnce) {
  ops->exit_failure();
  ops->exit_failure();
  EXPECT_EQ(rec.count("exit_process"), 1) << "a second exit_failure must do nothing";
}

TEST_F(HandoffNewOpsTest, ExitFailureReleasesTheProbeWaiter) {
  ops->exit_failure();
  EXPECT_TRUE(ops->wait_probe_decision(500ms)) << "exit_failure calls mark_finished, so the probe decision is final";
  EXPECT_FALSE(probe_installed.load()) << "no override was ever installed";
}

TEST_F(HandoffNewOpsTest, NotCallingExitFailureNeverExits) {
  run_pre_commit();
  run_commit();
  ops->on_main();
  EXPECT_EQ(rec.count("exit_process"), 0) << "no seam call exits the process on its own";
}

// ---------------------------------------------------------------------------------------------------------------------
// Probe override ordering
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffNewOpsTest, ProbeOverrideIsInstalledBeforeTheWaiterReturns) {
  probe_override_delay = 150ms;
  rebuild();
  std::atomic<bool> installed_when_returned {false};
  std::atomic<bool> result {false};
  std::atomic<bool> waiting {false};
  std::thread waiter([&] {
    waiting = true;
    result = ops->wait_probe_decision(3000ms);
    installed_when_returned = probe_installed.load();
  });
  ASSERT_TRUE(wait_until([&] {
    return waiting.load();
  },
                         1000ms));
  std::this_thread::sleep_for(30ms);
  ops->accept_old_hello(old_hello(), make_fake_pidfd());
  waiter.join();
  EXPECT_TRUE(result.load()) << "wait_probe_decision must return true once the HELLO was processed";
  EXPECT_TRUE(installed_when_returned.load()) << "the probe override must already be installed when wait_probe_decision returns true";
}

TEST_F(HandoffNewOpsTest, MatchingIdentityInstallsTheOverrideOnce) {
  ops->accept_old_hello(old_hello(), make_fake_pidfd());
  EXPECT_EQ(rec.count("set_probe_override"), 1) << "an accepted probe must be installed exactly once";
  EXPECT_EQ(probe_results.hevc_mode, 3) << "hevc_mode passed to the override";
  EXPECT_EQ(probe_results.av1_mode, 2) << "av1_mode passed to the override";
  EXPECT_TRUE(probe_results.yuv444_h264) << "yuv444_h264 passed to the override";
  EXPECT_FALSE(probe_results.yuv444_hevc) << "yuv444_hevc passed to the override";
  EXPECT_TRUE(probe_results.yuv444_av1) << "yuv444_av1 passed to the override";
  EXPECT_TRUE(ops->wait_probe_decision(100ms)) << "decided after accept_old_hello";
}

TEST_F(HandoffNewOpsTest, MismatchedIdentityInstallsNoOverrideButReleasesTheWaiter) {
  auto hello = old_hello();
  hello.identity->driver_version = "other";
  ops->accept_old_hello(hello, make_fake_pidfd());
  EXPECT_EQ(rec.count("set_probe_override"), 0) << "a rejected probe identity must leave the override unset, so the startup probe runs";
  EXPECT_TRUE(ops->wait_probe_decision(100ms)) << "the decision is final even when the identity was rejected";
}

TEST_F(HandoffNewOpsTest, HelloWithoutProbeInstallsNoOverride) {
  protocol::hello_t hello;
  ops->accept_old_hello(hello, make_fake_pidfd());
  EXPECT_EQ(rec.count("set_probe_override"), 0) << "no probe data, no override";
  EXPECT_TRUE(ops->wait_probe_decision(100ms)) << "decided";
}

TEST_F(HandoffNewOpsTest, OutOfRangeModeInstallsNoOverride) {
  auto hello = old_hello();
  hello.probe->hevc_mode = 6;
  ops->accept_old_hello(hello, make_fake_pidfd());
  EXPECT_EQ(rec.count("set_probe_override"), 0) << "a mode outside 0 to 5 must be rejected";
}

TEST_F(HandoffNewOpsTest, ModesFourAndFiveAreAccepted) {
  auto hello = old_hello();
  hello.probe->hevc_mode = 4;
  hello.probe->av1_mode = 5;
  ops->accept_old_hello(hello, make_fake_pidfd());
  EXPECT_EQ(rec.count("set_probe_override"), 1) << "modes 4 and 5 (the 4:4:4 variants) are within the accepted range";
}

TEST_F(HandoffNewOpsTest, WaitProbeDecisionTimesOutWithoutAHello) {
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(ops->wait_probe_decision(80ms)) << "no HELLO and no finish: wait_probe_decision must time out with false";
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_GE(elapsed, 60ms) << "it must wait for about the timeout";
  EXPECT_LT(elapsed, 1500ms) << "it must not wait much longer than the timeout";
  EXPECT_FALSE(handoff::hooks::startup_probe_skipped()) << "after a timeout the startup probe runs";
}

TEST_F(HandoffNewOpsTest, MarkFinishedReleasesTheProbeWaiterWithoutAnOverride) {
  std::atomic<bool> result {false};
  std::thread waiter([&] {
    result = ops->wait_probe_decision(3000ms);
  });
  std::this_thread::sleep_for(30ms);
  ops->mark_finished();
  waiter.join();
  EXPECT_TRUE(result.load()) << "mark_finished must release wait_probe_decision with true";
  EXPECT_EQ(rec.count("set_probe_override"), 0) << "no HELLO, no override";
  EXPECT_FALSE(handoff::hooks::startup_probe_skipped()) << "no override, so the startup probe runs";
}

TEST_F(HandoffNewOpsTest, ShutdownRequestEndsTheProbeWaitAndTheProbeRuns) {
  // SPEC-QUESTION: new_ops.h does not say that wait_probe_decision returns early on a shutdown request (entry.h step 11
  // does: "it returns early on a shutdown request"). The return value is not asserted, only the promptness and that the
  // probe is not skipped.
  handoff::runtime().request_shutdown();
  const auto start = std::chrono::steady_clock::now();
  (void) ops->wait_probe_decision(1500ms);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1200ms) << "wait_probe_decision must return early on a shutdown request";
  EXPECT_FALSE(handoff::hooks::startup_probe_skipped()) << "on shutdown the probe is not skipped";
}

TEST_F(HandoffNewOpsTest, DefaultOverrideSeamMakesTheStartupProbeSkipped) {
  seams.set_probe_override = nullptr;
  rebuild();
  EXPECT_FALSE(handoff::hooks::startup_probe_skipped()) << "control: before the HELLO the probe is not skipped";
  ops->accept_old_hello(old_hello(), make_fake_pidfd());
  const auto override_value = video::handoff_probe_override();
  ASSERT_TRUE(override_value.has_value()) << "the production seam must call video::set_handoff_probe_override";
  EXPECT_EQ(override_value->hevc_mode, 3) << "hevc_mode mapping";
  EXPECT_EQ(override_value->av1_mode, 2) << "av1_mode mapping";
  EXPECT_TRUE(override_value->yuv444[0]) << "yuv444_h264 maps to yuv444[0]";
  EXPECT_FALSE(override_value->yuv444[1]) << "yuv444_hevc maps to yuv444[1]";
  EXPECT_TRUE(override_value->yuv444[2]) << "yuv444_av1 maps to yuv444[2]";
  EXPECT_TRUE(handoff::hooks::startup_probe_skipped()) << "an accepted override in a handoff-started process skips the startup probe";
}

TEST_F(HandoffNewOpsTest, DefaultOverrideSeamIsNotUsedForARejectedIdentity) {
  seams.set_probe_override = nullptr;
  rebuild();
  auto hello = old_hello();
  hello.identity->build_id = "different";
  ops->accept_old_hello(hello, make_fake_pidfd());
  EXPECT_FALSE(video::handoff_probe_override().has_value()) << "a rejected identity must install nothing";
  EXPECT_FALSE(handoff::hooks::startup_probe_skipped()) << "so the startup probe runs";
}

// ---------------------------------------------------------------------------------------------------------------------
// accept_old_hello
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffNewOpsTest, AcceptOldHelloStoresTheCooldownDeadline) {
  auto hello = old_hello();
  hello.cooldown_deadline_ns = 123456789;
  ops->accept_old_hello(hello, make_fake_pidfd());
  EXPECT_EQ(rec.count("set_cooldown_deadline_ns(123456789)"), 1) << "the cooldown deadline from HELLO must be stored";
}

TEST_F(HandoffNewOpsTest, AcceptOldHelloWithoutCooldownStoresNone) {
  ops->accept_old_hello(old_hello(), make_fake_pidfd());
  EXPECT_EQ(rec.count("set_cooldown_deadline_ns"), 0) << "no cooldown in HELLO, no call";
}

TEST_F(HandoffNewOpsTest, AcceptOldHelloStoresTheChannelInTheRuntime) {
  ops->accept_old_hello(old_hello(), make_fake_pidfd());
  EXPECT_EQ(handoff::runtime().channel(), channel) << "accept_old_hello must call runtime().set_channel(channel)";
}

TEST_F(HandoffNewOpsTest, TakeOldPidfdHandsOverTheFdOnce) {
  const int fd = make_fake_pidfd();
  ASSERT_GE(fd, 0);
  ops->accept_old_hello(old_hello(), fd);
  EXPECT_EQ(ops->take_old_pidfd(), fd) << "take_old_pidfd must return the pidfd given to accept_old_hello";
  EXPECT_EQ(ops->take_old_pidfd(), -1) << "a second take returns -1";
  EXPECT_NE(::fcntl(fd, F_GETFD), -1) << "the destructor must not close a pidfd that was taken";
  ops.reset();
  EXPECT_NE(::fcntl(fd, F_GETFD), -1) << "the caller owns a taken pidfd";
  ::close(fd);
}

TEST_F(HandoffNewOpsTest, TakeOldPidfdBeforeAnyHelloReturnsMinusOne) {
  EXPECT_EQ(ops->take_old_pidfd(), -1) << "no HELLO yet, no pidfd";
}

TEST_F(HandoffNewOpsTest, DestructorClosesAnUntakenPidfd) {
  const int fd = make_fake_pidfd();
  ASSERT_GE(fd, 0);
  ops->accept_old_hello(old_hello(), fd);
  ops.reset();
  errno = 0;
  EXPECT_EQ(::fcntl(fd, F_GETFD), -1) << "the destructor must close the pidfd if take_old_pidfd did not take it";
  EXPECT_EQ(errno, EBADF) << "errno";
}

TEST_F(HandoffNewOpsTest, HelloIsTheDefaultMessageAndSetsStageStarting) {
  const auto hello = ops->hello();
  const protocol::hello_t expected {};
  EXPECT_EQ(hello.major, expected.major) << "major version";
  EXPECT_EQ(hello.minor, expected.minor) << "minor version";
  EXPECT_TRUE(hello.capabilities.empty()) << "no capabilities required";
  EXPECT_FALSE(hello.probe.has_value()) << "the new side sends no probe results";
  EXPECT_FALSE(hello.identity.has_value()) << "the new side sends no identity";
  EXPECT_FALSE(hello.cooldown_deadline_ns.has_value()) << "the new side sends no cooldown";
  EXPECT_EQ(ops->stage(), new_stage_e::starting) << "stage after hello()";
}

// ---------------------------------------------------------------------------------------------------------------------
// validate_listeners and the validation waiter
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffNewOpsTest, ValidateListenersChecksFourServersInOrderWithTheirPorts) {
  EXPECT_EQ(ops->validate_listeners(), "") << "valid listeners";
  const auto log = rec.all();
  ASSERT_EQ(log.size(), 4u) << "validate_listener must be called once per server";
  EXPECT_EQ(log[0], "validate_listener(3,47984,v4)") << "HTTPS: fd 3, port - 5";
  EXPECT_EQ(log[1], "validate_listener(4,47989,v4)") << "HTTP: fd 4, port";
  EXPECT_EQ(log[2], "validate_listener(5,47990,v4)") << "web UI: fd 5, port + 1";
  EXPECT_EQ(log[3], "validate_listener(6,48010,v4)") << "RTSP: fd 6, port + 21";
}

TEST_F(HandoffNewOpsTest, ValidateListenersPassesTheIpv6Flag) {
  config.ipv6 = true;
  rebuild();
  EXPECT_EQ(ops->validate_listeners(), "");
  EXPECT_EQ(rec.count("validate_listener(3,47984,v6)"), 1) << "the ipv6 flag of the configuration must be passed through";
}

TEST_F(HandoffNewOpsTest, ValidateListenersPrefixesTheServerName) {
  validate_fn = [](int, int port, bool) -> std::string {
    return port == 47990 ? "wrong port" : "";
  };
  EXPECT_EQ(ops->validate_listeners(), "webui: wrong port") << "the reason of the failing server, prefixed by its name";
}

TEST_F(HandoffNewOpsTest, ValidateListenersNamesEachServer) {
  const std::pair<int, std::string> servers[] = {{47984, "https"}, {47989, "http"}, {47990, "webui"}, {48010, "rtsp"}};
  for (const auto &[port, name] : servers) {
    rebuild();
    validate_fn = [port = port](int, int p, bool) -> std::string {
      return p == port ? "bad" : "";
    };
    EXPECT_EQ(ops->validate_listeners(), name + ": bad") << "server name for port " << port;
  }
}

TEST_F(HandoffNewOpsTest, ValidateListenersReportsTheFirstFailureInOrder) {
  validate_fn = [](int, int port, bool) -> std::string {
    return port == 47989 || port == 48010 ? "bad" : "";
  };
  EXPECT_EQ(ops->validate_listeners(), "http: bad") << "HTTP precedes RTSP, so the HTTP failure is the one reported";
}

TEST_F(HandoffNewOpsTest, ValidateListenersReturnsTheStartupRefusalFirst) {
  config.startup_refusal = "drain handoff is not available with privileged execution";
  rebuild();
  EXPECT_EQ(ops->validate_listeners(), "drain handoff is not available with privileged execution") << "the startup refusal is returned as is";
  EXPECT_EQ(rec.count("validate_listener"), 0) << "no listener is checked when the startup refused";
}

TEST_F(HandoffNewOpsTest, ValidateListenersFailsWhenTheFeatureIsDisabled) {
  config.feature_enabled = false;
  rebuild();
  EXPECT_EQ(ops->validate_listeners(), "drain handoff is disabled in the new process's configuration") << "disabled-feature reason";
  EXPECT_EQ(rec.count("validate_listener"), 0) << "no listener is checked when the feature is disabled";
}

TEST_F(HandoffNewOpsTest, StartupRefusalWinsOverDisabledFeature) {
  config.startup_refusal = "refused";
  config.feature_enabled = false;
  rebuild();
  EXPECT_EQ(ops->validate_listeners(), "refused") << "startup_refusal is checked before feature_enabled";
}

TEST_F(HandoffNewOpsTest, ValidateListenersInAProcessNotStartedByHandoffFails) {
  handoff::runtime().reset_for_test();
  EXPECT_EQ(ops->validate_listeners(), "not started by a handoff") << "no inherited fds";
  EXPECT_EQ(rec.count("validate_listener"), 0) << "no listener is checked";
}

TEST_F(HandoffNewOpsTest, WaitListenersValidatedReturnsOkAfterValidation) {
  EXPECT_EQ(ops->validate_listeners(), "");
  EXPECT_EQ(ops->wait_listeners_validated(100ms), validation_e::ok) << "validation succeeded";
}

TEST_F(HandoffNewOpsTest, WaitListenersValidatedReturnsFailedAfterFailedValidation) {
  config.feature_enabled = false;
  rebuild();
  EXPECT_NE(ops->validate_listeners(), "");
  EXPECT_EQ(ops->wait_listeners_validated(100ms), validation_e::failed) << "validation failed";
}

TEST_F(HandoffNewOpsTest, WaitListenersValidatedTimesOutBeforeValidation) {
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(ops->wait_listeners_validated(80ms), validation_e::timeout) << "neither validation nor finish happened";
  EXPECT_GE(std::chrono::steady_clock::now() - start, 60ms) << "must wait about the timeout";
}

TEST_F(HandoffNewOpsTest, WaitListenersValidatedIsReleasedByAValidationInAnotherThread) {
  std::atomic<validation_e> result {validation_e::timeout};
  std::thread waiter([&] {
    result = ops->wait_listeners_validated(3000ms);
  });
  std::this_thread::sleep_for(30ms);
  EXPECT_EQ(ops->validate_listeners(), "");
  waiter.join();
  EXPECT_EQ(result.load(), validation_e::ok) << "the waiter must be released with ok";
}

TEST_F(HandoffNewOpsTest, MarkFinishedBeforeValidationGivesFailed) {
  ops->mark_finished();
  EXPECT_EQ(ops->wait_listeners_validated(100ms), validation_e::failed) << "the new side ended without validating";
}

TEST_F(HandoffNewOpsTest, MarkFinishedAfterValidationKeepsOk) {
  EXPECT_EQ(ops->validate_listeners(), "");
  ops->mark_finished();
  EXPECT_EQ(ops->wait_listeners_validated(100ms), validation_e::ok) << "an earlier ok result must not turn into failed";
}

TEST_F(HandoffNewOpsTest, ServersFailedReleasesTheValidationWaiterWithFailed) {
  std::atomic<validation_e> result {validation_e::ok};
  std::thread waiter([&] {
    result = ops->wait_listeners_validated(3000ms);
  });
  std::this_thread::sleep_for(30ms);
  ops->servers_failed("HTTP interface failed to initialize");
  waiter.join();
  EXPECT_EQ(result.load(), validation_e::failed) << "servers_failed must wake a waiting wait_listeners_validated with failed";
}

// ---------------------------------------------------------------------------------------------------------------------
// start_servers_paused
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffNewOpsTest, StartServersPausedReturnsEmptyAfterFourListeningCalls) {
  std::promise<std::string> result;
  auto future = result.get_future();
  std::thread worker([&] {
    result.set_value(ops->start_servers_paused());
  });
  for (int i = 0; i < 4; ++i) {
    std::this_thread::sleep_for(10ms);
    ops->server_listening();
  }
  ASSERT_EQ(future.wait_for(1500ms), std::future_status::ready) << "start_servers_paused must return after the fourth server_listening";
  worker.join();
  EXPECT_EQ(future.get(), "") << "success is the empty string";
  EXPECT_EQ(ops->stage(), new_stage_e::ready) << "stage ready after success";
}

TEST_F(HandoffNewOpsTest, StartServersPausedIgnoresExtraListeningCalls) {
  for (int i = 0; i < 7; ++i) {
    ops->server_listening();
  }
  EXPECT_EQ(ops->start_servers_paused(), "") << "extra server_listening calls are ignored and do not break the wait";
}

TEST_F(HandoffNewOpsTest, StartServersPausedTimesOutNamingTheCount) {
  config.servers_listening_timeout = 200ms;
  rebuild();
  for (int i = 0; i < 3; ++i) {
    ops->server_listening();
  }
  const std::string reason = ops->start_servers_paused();
  EXPECT_NE(reason.find("servers did not start listening within"), std::string::npos) << "reason was: " << reason;
  EXPECT_NE(reason.find("3 of 4"), std::string::npos) << "reason must say how many servers listen; was: " << reason;
  EXPECT_NE(ops->stage(), new_stage_e::ready) << "a failed wait must not set stage ready";
}

TEST_F(HandoffNewOpsTest, ServersFailedWakesStartServersPausedWithTheReason) {
  std::promise<std::string> result;
  auto future = result.get_future();
  std::thread worker([&] {
    result.set_value(ops->start_servers_paused());
  });
  std::this_thread::sleep_for(30ms);
  ops->servers_failed("HTTP interface failed to initialize");
  ASSERT_EQ(future.wait_for(1500ms), std::future_status::ready) << "servers_failed must wake start_servers_paused";
  worker.join();
  EXPECT_EQ(future.get(), "HTTP interface failed to initialize") << "the reason given to servers_failed";
}

TEST_F(HandoffNewOpsTest, ServersFailedFirstReasonWins) {
  ops->servers_failed("first");
  ops->servers_failed("second");
  EXPECT_EQ(ops->start_servers_paused(), "first") << "the first reason wins";
}

TEST_F(HandoffNewOpsTest, StartServersPausedFailsOnShutdownRequest) {
  config.servers_listening_timeout = 10000ms;
  rebuild();
  handoff::runtime().request_shutdown();
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(ops->start_servers_paused(), "shutdown requested") << "a shutdown request ends the wait with this reason";
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1500ms) << "the shutdown flag is polled about every 100 ms";
}

TEST_F(HandoffNewOpsTest, AbortWakesStartServersPaused) {
  config.servers_listening_timeout = 10000ms;
  rebuild();
  std::promise<std::string> result;
  auto future = result.get_future();
  std::thread worker([&] {
    result.set_value(ops->start_servers_paused());
  });
  std::this_thread::sleep_for(30ms);
  ops->abort();
  ASSERT_EQ(future.wait_for(1500ms), std::future_status::ready) << "abort() must end the wait for the servers";
  worker.join();
  EXPECT_NE(future.get(), "") << "an aborted wait is a failure (non-empty reason)";
}

TEST_F(HandoffNewOpsTest, MarkFinishedWakesStartServersPaused) {
  config.servers_listening_timeout = 10000ms;
  rebuild();
  std::promise<std::string> result;
  auto future = result.get_future();
  std::thread worker([&] {
    result.set_value(ops->start_servers_paused());
  });
  std::this_thread::sleep_for(30ms);
  ops->mark_finished();
  ASSERT_EQ(future.wait_for(1500ms), std::future_status::ready) << "mark_finished must wake start_servers_paused";
  worker.join();
}

// ---------------------------------------------------------------------------------------------------------------------
// GO ops and stages
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffNewOpsTest, ReloadAllSetsStageGoFirstAndReturnsTheReason) {
  reload_reason = "sunshine_state.json: no_uniqueid";
  rebuild();
  EXPECT_EQ(ops->reload_all(), "sunshine_state.json: no_uniqueid") << "the seam's reason is returned";
  EXPECT_EQ(rec.first("reload_all"), "reload_all:stage=go") << "the stage must be go while the seam runs";
}

TEST_F(HandoffNewOpsTest, PrepareAdoptionCallsTheSeamAndInstallsNothing) {
  prepare_reason = "app or prep commands changed while an app runs; cancel the app first";
  rebuild();
  EXPECT_EQ(ops->prepare_adoption(protocol::go_t {}), prepare_reason) << "the seam's reason is returned";
  EXPECT_EQ(rec.count("prepare_adoption"), 1) << "the seam must be called once";
  EXPECT_EQ(rec.count("install_adoption"), 0) << "prepare_adoption installs nothing";
  EXPECT_EQ(handoff::runtime().mode(), handoff::mode_e::new_pending) << "mode unchanged";
}

TEST_F(HandoffNewOpsTest, StagesOnlyMoveForwardThroughTheProtocol) {
  EXPECT_EQ(ops->stage(), new_stage_e::starting) << "initial";
  ops->hello();
  EXPECT_EQ(ops->stage(), new_stage_e::starting) << "after hello";
  ops->accept_old_hello(old_hello(), make_fake_pidfd());
  EXPECT_EQ(ops->validate_listeners(), "");
  for (int i = 0; i < 4; ++i) {
    ops->server_listening();
  }
  EXPECT_EQ(ops->start_servers_paused(), "");
  EXPECT_EQ(ops->stage(), new_stage_e::ready) << "after start_servers_paused";
  ops->reload_all();
  EXPECT_EQ(ops->stage(), new_stage_e::go) << "after reload_all";
  ops->prepare_adoption(protocol::go_t {});
  ops->install_adoption();
  EXPECT_EQ(ops->stage(), new_stage_e::committing) << "after install_adoption";
  ops->resume_accept();
  ops->publish_mdns();
  ops->map_upnp();
  ops->start_tray();
  ops->become_active();
  EXPECT_EQ(ops->stage(), new_stage_e::committed) << "after become_active";
  ops->on_main();
  EXPECT_EQ(ops->stage(), new_stage_e::main) << "after on_main";
}

// ---------------------------------------------------------------------------------------------------------------------
// abort versus COMMIT
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffNewOpsTest, AbortHalfClosesTheChannelAndDoesNotExit) {
  ops->abort();
  EXPECT_EQ(rec.count("shutdown_channel"), 1) << "abort before COMMIT must half-close the channel through the seam";
  EXPECT_EQ(rec.count("exit_process"), 0) << "abort() does not call exit_process";
  handoff::received_t received;
  EXPECT_EQ(peer->receive(received, 500ms), handoff::recv_status_e::eof) << "the peer must see the channel end after the half-close";
}

TEST_F(HandoffNewOpsTest, AbortTwiceHalfClosesOnce) {
  ops->abort();
  ops->abort();
  EXPECT_EQ(rec.count("shutdown_channel"), 1) << "a second abort is a no-op";
}

TEST_F(HandoffNewOpsTest, AbortIsEffectiveInEveryStageBeforeCommitting) {
  run_pre_commit();
  ASSERT_EQ(ops->stage(), new_stage_e::go);
  ops->abort();
  EXPECT_EQ(rec.count("shutdown_channel"), 1) << "abort at stage go must half-close";
}

TEST_F(HandoffNewOpsTest, AbortAfterInstallAdoptionKeepsTheChannelOpen) {
  run_pre_commit();
  ops->install_adoption();
  ops->abort();
  EXPECT_EQ(rec.count("shutdown_channel"), 0) << "from committing on the channel is needed by END_ALL; abort must be a no-op";
  EXPECT_EQ(ops->stage(), new_stage_e::committing) << "stage unchanged";
  ASSERT_TRUE(peer->send(protocol::ping_t {7})) << "the peer can still send";
  handoff::received_t received;
  EXPECT_EQ(channel->receive(received, 500ms), handoff::recv_status_e::ok) << "the channel must still be open";
}

TEST_F(HandoffNewOpsTest, AbortBeforeInstallAdoptionInstallsNothingAndSetsCommitFailed) {
  run_pre_commit();
  ops->abort();
  ops->install_adoption();
  EXPECT_EQ(rec.count("install_adoption"), 0) << "after an abort the install_adoption seam must not run";
  EXPECT_TRUE(ops->commit_failed()) << "commit_failed must be set so entry exits with status 1";
  EXPECT_EQ(handoff::runtime().mode(), handoff::mode_e::new_pending) << "nothing was installed, so the mode stays new_pending";
  EXPECT_NE(ops->stage(), new_stage_e::committing) << "the stage must not reach committing";
}

TEST_F(HandoffNewOpsTest, AfterAnAbortedInstallEveryLaterEffectDoesNothing) {
  run_pre_commit();
  ops->abort();
  ops->install_adoption();
  ops->resume_accept();
  ops->publish_mdns();
  ops->map_upnp();
  ops->start_tray();
  ops->become_active();
  EXPECT_EQ(rec.count("resume_servers"), 0) << "no resume after an aborted install";
  EXPECT_EQ(rec.count("publish_mdns"), 0) << "no publish after an aborted install";
  EXPECT_EQ(rec.count("start_upnp"), 0) << "no UPnP after an aborted install";
  EXPECT_EQ(rec.count("request_tray_start"), 0) << "no tray after an aborted install";
  EXPECT_EQ(rec.count("set_unmap_scope"), 0) << "no scope change after an aborted install";
  EXPECT_EQ(current_role(), "pending") << "the role stays pending";
  // SPEC-QUESTION: whether become_active() still sets the stage `committed` after an aborted install is not stated;
  // the stage is not asserted here.
}

TEST_F(HandoffNewOpsTest, AbortVersusCommitIsAllOrNothing) {
  // The abort and the install race 40 times. Either the adoption is installed and the channel stays open, or
  // nothing is installed and commit_failed is set.
  int installed_count = 0;
  int aborted_count = 0;
  for (int i = 0; i < 40; ++i) {
    rebuild();
    std::atomic<bool> go {false};
    std::thread aborter([&] {
      while (!go.load()) {
      }
      ops->abort();
    });
    std::thread committer([&] {
      while (!go.load()) {
      }
      ops->install_adoption();
    });
    go = true;
    aborter.join();
    committer.join();
    const bool installed = rec.count("install_adoption") > 0;
    const bool closed = rec.count("shutdown_channel") > 0;
    if (installed) {
      ++installed_count;
      EXPECT_FALSE(closed) << "iteration " << i << ": the adoption was installed, so the channel must stay open";
      EXPECT_FALSE(ops->commit_failed()) << "iteration " << i << ": installed, so commit_failed must be false";
      EXPECT_EQ(ops->stage(), new_stage_e::committing) << "iteration " << i;
    } else {
      ++aborted_count;
      EXPECT_TRUE(ops->commit_failed()) << "iteration " << i << ": nothing installed, so commit_failed must be set";
      EXPECT_TRUE(closed) << "iteration " << i << ": the abort must have half-closed the channel";
      EXPECT_EQ(handoff::runtime().mode(), handoff::mode_e::new_pending) << "iteration " << i << ": nothing installed, mode unchanged";
    }
  }
  EXPECT_EQ(installed_count + aborted_count, 40) << "every iteration must be one of the two outcomes";
}

// ---------------------------------------------------------------------------------------------------------------------
// Default seams that must not be needed
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffNewOpsTest, ServerListeningNeverBlocksOnAnIdleOps) {
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) {
    ops->server_listening();
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms) << "server_listening must never block";
}

// ---------------------------------------------------------------------------------------------------------------------
// Production default seams and the coordinator hook (set_new_ops_coordinator)
// ---------------------------------------------------------------------------------------------------------------------

namespace {
  /**
   * @brief Sets the coordinator hook to null on destruction (declared after the coordinator, so it runs first).
   */
  struct hook_reset_t {
    /**
     * @brief Clear the hook.
     */
    ~hook_reset_t() {
      handoff::set_new_ops_coordinator(nullptr);
    }
  };

  /**
   * @brief Set `RUNTIME_DIRECTORY` for the life of the object.
   */
  struct runtime_dir_env_t {
    /**
     * @brief Set the variable.
     *
     * @param value New value.
     */
    explicit runtime_dir_env_t(const std::string &value) {
      if (const char *old = ::getenv("RUNTIME_DIRECTORY")) {
        had = true;
        previous = old;
      }
      ::setenv("RUNTIME_DIRECTORY", value.c_str(), 1);
    }

    /**
     * @brief Restore the variable.
     */
    ~runtime_dir_env_t() {
      if (had) {
        ::setenv("RUNTIME_DIRECTORY", previous.c_str(), 1);
      } else {
        ::unsetenv("RUNTIME_DIRECTORY");
      }
    }

    bool had {false};  ///< The variable was set before.
    std::string previous;  ///< Its old value.
  };
}  // namespace

/**
 * @brief Fixture for the default `set_cooldown_deadline_ns` and `start_control_server` seams.
 */
class HandoffNewOpsCoordinatorHookTest: public HandoffNewOpsTest {
protected:
  /**
   * @brief Use the production cooldown and control seams, then rebuild.
   */
  void SetUp() override {
    HandoffNewOpsTest::SetUp();
    seams.set_cooldown_deadline_ns = nullptr;
    seams.start_control_server = nullptr;
    rebuild();
    _tmp = std::filesystem::temp_directory_path() / ("sunshine-t18b-hook-" + std::to_string(::getpid()) + "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::error_code ec;
    std::filesystem::remove_all(_tmp, ec);
    std::filesystem::create_directories(_tmp, ec);
  }

  /**
   * @brief Remove the temporary directory.
   */
  void TearDown() override {
    HandoffNewOpsTest::TearDown();
    std::error_code ec;
    std::filesystem::remove_all(_tmp, ec);
  }

  /**
   * @brief Deliver an old `HELLO` that carries a cooldown deadline.
   *
   * @param ahead How far ahead the deadline is.
   * @return The deadline as a steady-clock time point.
   */
  std::chrono::steady_clock::time_point deliver_deadline(std::chrono::seconds ahead) {
    const auto deadline = std::chrono::steady_clock::now() + ahead;
    auto hello = old_hello();
    hello.cooldown_deadline_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count();
    ops->accept_old_hello(hello, make_fake_pidfd());
    return deadline;
  }

  /**
   * @brief Whether the coordinator's cooldown is active until about `deadline`.
   *
   * @param coordinator Coordinator.
   * @param deadline Expected deadline.
   * @return `true` when `cooldown_until()` is within 5 ms of it.
   */
  static bool cooldown_matches(const handoff::coordinator_t &coordinator, std::chrono::steady_clock::time_point deadline) {
    const auto diff = coordinator.cooldown_until() - deadline;
    return diff < 5ms && diff > -5ms;
  }

  std::filesystem::path _tmp;  ///< Runtime directory for the control socket.
};

TEST_F(HandoffNewOpsCoordinatorHookTest, DeadlineDeliveredBeforeTheCoordinatorIsAppliedWhenItIsSet) {
  handoff::control_server_t control;
  handoff::coordinator_t coordinator {control};
  hook_reset_t reset;
  EXPECT_EQ(coordinator.cooldown_until(), std::chrono::steady_clock::time_point::min()) << "control: no cooldown before anything is delivered";
  const auto deadline = deliver_deadline(30s);
  EXPECT_EQ(coordinator.cooldown_until(), std::chrono::steady_clock::time_point::min()) << "a null hook must not reach the coordinator yet";
  handoff::set_new_ops_coordinator(&coordinator);
  EXPECT_TRUE(cooldown_matches(coordinator, deadline)) << "set_new_ops_coordinator(non-null) must apply the stored deadline before it returns";
  EXPECT_GT(coordinator.cooldown_until(), std::chrono::steady_clock::now()) << "the cooldown is active";
}

TEST_F(HandoffNewOpsCoordinatorHookTest, DeadlineDeliveredAfterTheCoordinatorIsSetIsApplied) {
  handoff::control_server_t control;
  handoff::coordinator_t coordinator {control};
  hook_reset_t reset;
  handoff::set_new_ops_coordinator(&coordinator);
  const auto deadline = deliver_deadline(30s);
  EXPECT_TRUE(cooldown_matches(coordinator, deadline)) << "with the coordinator set, the default seam applies the deadline at once";
}

TEST_F(HandoffNewOpsCoordinatorHookTest, SettingNullDiscardsAStoredDeadline) {
  handoff::control_server_t control;
  handoff::coordinator_t coordinator {control};
  hook_reset_t reset;
  deliver_deadline(30s);
  handoff::set_new_ops_coordinator(nullptr);
  handoff::set_new_ops_coordinator(&coordinator);
  EXPECT_EQ(coordinator.cooldown_until(), std::chrono::steady_clock::time_point::min()) << "the deadline was discarded by set_new_ops_coordinator(nullptr), so no cooldown";
}

TEST_F(HandoffNewOpsCoordinatorHookTest, ASetDeadlineIsAppliedOnlyOnce) {
  handoff::control_server_t control;
  handoff::coordinator_t coordinator {control};
  hook_reset_t reset;
  deliver_deadline(30s);
  handoff::set_new_ops_coordinator(&coordinator);
  handoff::set_new_ops_coordinator(nullptr);
  handoff::set_new_ops_coordinator(&coordinator);
  EXPECT_GT(coordinator.cooldown_until(), std::chrono::steady_clock::now()) << "the earlier application stays (cooldown never lowers)";
}

TEST_F(HandoffNewOpsCoordinatorHookTest, DefaultControlServerWithoutACoordinatorDoesNothing) {
  const runtime_dir_env_t env {_tmp.string()};
  run_pre_commit();
  run_commit();
  ops->on_main();
  EXPECT_EQ(ops->stage(), new_stage_e::main) << "on_main completes even though the control server cannot start";
  EXPECT_FALSE(std::filesystem::exists(handoff::control_socket_path(_tmp, ::getpid()))) << "no control socket may appear without a coordinator";
}

TEST_F(HandoffNewOpsCoordinatorHookTest, DefaultControlServerWithACoordinatorStartsTheControlSocket) {
  const runtime_dir_env_t env {_tmp.string()};
  handoff::control_server_t control;
  handoff::coordinator_t coordinator {control};
  hook_reset_t reset;
  handoff::set_new_ops_coordinator(&coordinator);
  run_pre_commit();
  run_commit();
  ops->on_main();
  EXPECT_TRUE(std::filesystem::exists(handoff::control_socket_path(_tmp, ::getpid()))) << "on_main must start the control server through the coordinator";
  coordinator.request_shutdown();
  handoff::set_new_ops_coordinator(nullptr);
  coordinator.stop_control();
}

#endif  // __linux__
