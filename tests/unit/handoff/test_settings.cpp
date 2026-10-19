/**
 * @file tests/unit/handoff/test_settings.cpp
 * @brief Blind tests for the drain-handoff settings module (src/handoff/settings.*), written from the spec and header.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <cstdint>
  #include <limits>
  #include <set>
  #include <string>

  // local includes
  #include "src/handoff/settings.h"

namespace {
  /**
   * @brief Settings with every documented default (the tests pass the configured port 47989 separately).
   * @return Default settings.
   */
  handoff::settings_t default_settings() {
    return handoff::settings_t {};
  }

  /**
   * @brief Host facts that allow the feature to enable.
   * @return Context with `capture = wlr` and no refusing condition.
   */
  handoff::enable_context_t good_context() {
    handoff::enable_context_t context;
    context.capture = "wlr";
    context.fresh_state = false;
    context.display_settings_manager = false;
    context.runtime_dir_available = true;
    return context;
  }
}  // namespace

/**
 * @brief Video, control and audio ports sit at base + 9, + 10 and + 11.
 */
TEST(HandoffSettingsTest, UdpPortsAreBasePlus9To11) {
  EXPECT_EQ(handoff::udp_ports(47989), (std::set<std::uint16_t> {47998, 47999, 48000}))
    << "udp_ports(47989) must be video 47998, control 47999 and audio 48000.";
  EXPECT_EQ(handoff::udp_ports(48089), (std::set<std::uint16_t> {48098, 48099, 48100}))
    << "udp_ports(48089) must be base+9, base+10 and base+11.";
}

/**
 * @brief The helper returns exactly three ports for any base.
 */
TEST(HandoffSettingsTest, UdpPortsReturnsThreeDistinctPorts) {
  EXPECT_EQ(handoff::udp_ports(2000).size(), 3U)
    << "udp_ports must return exactly three distinct UDP ports (video, control, audio).";
}

/**
 * @brief The four fixed TCP ports are port - 5, port, port + 1 and port + 21.
 */
TEST(HandoffSettingsTest, TcpPortsAreFixedOffsetsOfPort) {
  EXPECT_EQ(handoff::tcp_ports(47989), (std::set<std::uint16_t> {47984, 47989, 47990, 48010}))
    << "tcp_ports(47989) must be HTTPS 47984, HTTP 47989, web UI 47990 and RTSP 48010.";
}

/**
 * @brief Asking for the other base from the configured port gives port + offset.
 */
TEST(HandoffSettingsTest, OtherUdpBaseFromPortIsPortPlusOffset) {
  EXPECT_EQ(handoff::other_udp_base(47989, 100, 47989), 48089)
    << "When the spawning process uses the port base, the new one must use port + udp_offset.";
}

/**
 * @brief Asking for the other base from port + offset gives the port base.
 */
TEST(HandoffSettingsTest, OtherUdpBaseFromOffsetBaseIsPort) {
  EXPECT_EQ(handoff::other_udp_base(47989, 100, 48089), 47989)
    << "When the spawning process uses port + udp_offset, the new one must use the port base.";
}

/**
 * @brief A current base that is neither base still yields the configured port (header: "else port").
 */
TEST(HandoffSettingsTest, OtherUdpBaseWithForeignCurrentBaseIsPort) {
  EXPECT_EQ(handoff::other_udp_base(47989, 100, 12345), 47989)
    << "When current_base is neither port nor port + udp_offset, the result must be port.";
}

/**
 * @brief Two alternating bases give each other back, so a process always gets the other one.
 */
TEST(HandoffSettingsTest, OtherUdpBaseAlternatesBetweenTwoBases) {
  const int first = handoff::other_udp_base(47989, 100, 47989);
  EXPECT_EQ(handoff::other_udp_base(47989, 100, first), 47989)
    << "Applying other_udp_base twice must return to the starting base.";
}

/**
 * @brief Removing UPnP ports after a rollback removes the new base's UDP ports that the old process does not use.
 */
TEST(HandoffSettingsTest, UpnpRemovalKeepsOnlyNewBaseUdpPorts) {
  EXPECT_EQ(handoff::upnp_ports_to_remove(48089, 47989, 47989), (std::set<std::uint16_t> {48098, 48099, 48100}))
    << "With disjoint bases, every UDP port of the new base must be removed.";
}

/**
 * @brief Equal bases mean nothing is removed, because the old process maps the same ports.
 */
TEST(HandoffSettingsTest, UpnpRemovalWithEqualBasesIsEmpty) {
  EXPECT_TRUE(handoff::upnp_ports_to_remove(47989, 47989, 47989).empty())
    << "When new_base equals old_base, upnp_ports_to_remove must return an empty set.";
}

/**
 * @brief Overlapping bases: the removal never includes a UDP port of the old base.
 */
TEST(HandoffSettingsTest, UpnpRemovalNeverContainsOldBaseUdpPortsWhenBasesOverlap) {
  const auto removed = handoff::upnp_ports_to_remove(47990, 47989, 47989);
  const auto old_udp = handoff::udp_ports(47989);
  for (const auto port : removed) {
    EXPECT_EQ(old_udp.count(port), 0U)
      << "upnp_ports_to_remove must not return UDP port " << port << " that the old base maps.";
  }
  EXPECT_EQ(removed, (std::set<std::uint16_t> {48001}))
    << "new base 47990 has UDP ports 47999..48001; the old base keeps 47999 and 48000, so only 48001 is removed.";
}

/**
 * @brief The removal never includes a TCP port, even when a new base's UDP port number equals one.
 */
TEST(HandoffSettingsTest, UpnpRemovalNeverContainsTcpPorts) {
  // New base 48000 has UDP 48009, 48010, 48011. Port 48010 is the configured port's RTSP TCP port (47989 + 21).
  const auto removed = handoff::upnp_ports_to_remove(48000, 47989, 47989);
  const auto tcp = handoff::tcp_ports(47989);
  for (const auto port : removed) {
    EXPECT_EQ(tcp.count(port), 0U)
      << "upnp_ports_to_remove must never return TCP port " << port << " of the configured port.";
  }
  EXPECT_EQ(removed, (std::set<std::uint16_t> {48009, 48011}))
    << "The TCP port 48010 must be excluded from the removal; 48009 and 48011 remain.";
}

/**
 * @brief Equal bases never remove a port even when the bases differ only by overlap with TCP.
 */
TEST(HandoffSettingsTest, UpnpRemovalWithEqualBasesNeverReturnsTcpOrOldPorts) {
  const auto removed = handoff::upnp_ports_to_remove(48000, 48000, 47989);
  EXPECT_TRUE(removed.empty()) << "Equal bases must produce an empty removal even when TCP ports are near.";
}

/**
 * @brief Default settings with the configured port 47989 are valid.
 */
TEST(HandoffSettingsTest, DefaultSettingsAreValid) {
  EXPECT_EQ(handoff::validate(default_settings(), 47989), "")
    << "Default settings with port 47989 must pass validation.";
}

/**
 * @brief A deadline of 0 is outside 1 to 86400 and is rejected, naming the option.
 */
TEST(HandoffSettingsTest, DeadlineBelowOneIsRejected) {
  auto settings = default_settings();
  settings.deadline_s = 0;
  const auto reason = handoff::validate(settings, 47989);
  EXPECT_FALSE(reason.empty()) << "A deadline of 0 must fail validation.";
  EXPECT_NE(reason.find("drain_handoff_deadline"), std::string::npos)
    << "The reason must name drain_handoff_deadline, got: " << reason;
}

/**
 * @brief A deadline above 86400 is rejected.
 */
TEST(HandoffSettingsTest, DeadlineAboveMaximumIsRejected) {
  auto settings = default_settings();
  settings.deadline_s = 86401;
  EXPECT_FALSE(handoff::validate(settings, 47989).empty())
    << "A deadline of 86401 must fail validation (range 1 to 86400).";
}

/**
 * @brief The deadline bounds 1 and 86400 are accepted (with a rollback window that fits under them).
 */
TEST(HandoffSettingsTest, DeadlineBoundsAreAccepted) {
  auto maximum = default_settings();
  maximum.deadline_s = 86400;
  EXPECT_EQ(handoff::validate(maximum, 47989), "")
    << "A deadline of 86400 must be accepted.";

  auto minimum = default_settings();
  minimum.deadline_s = 1;
  minimum.rollback_window_s = 1;
  minimum.cooldown_s = 1;
  EXPECT_EQ(handoff::validate(minimum, 47989), "")
    << "A deadline of 1 must be accepted when the rollback window is also 1 and cooldown is 1.";
}

/**
 * @brief A ready timeout below 1 is rejected and 1 is accepted.
 */
TEST(HandoffSettingsTest, ReadyTimeoutBelowOneIsRejected) {
  auto settings = default_settings();
  settings.ready_timeout_s = 0;
  const auto reason = handoff::validate(settings, 47989);
  EXPECT_NE(reason.find("drain_handoff_ready_timeout"), std::string::npos)
    << "A ready timeout of 0 must be rejected naming drain_handoff_ready_timeout, got: " << reason;

  settings.ready_timeout_s = 1;
  EXPECT_EQ(handoff::validate(settings, 47989), "")
    << "A ready timeout of 1 must be accepted.";
}

/**
 * @brief A rollback window below 1 is rejected.
 */
TEST(HandoffSettingsTest, RollbackWindowBelowOneIsRejected) {
  auto settings = default_settings();
  settings.rollback_window_s = 0;
  const auto reason = handoff::validate(settings, 47989);
  EXPECT_NE(reason.find("drain_handoff_rollback_window"), std::string::npos)
    << "A rollback window of 0 must be rejected naming drain_handoff_rollback_window, got: " << reason;
}

/**
 * @brief A cooldown shorter than the rollback window is rejected; equal is accepted.
 */
TEST(HandoffSettingsTest, CooldownShorterThanRollbackWindowIsRejected) {
  auto settings = default_settings();
  settings.cooldown_s = 59;
  settings.rollback_window_s = 60;
  const auto reason = handoff::validate(settings, 47989);
  EXPECT_NE(reason.find("drain_handoff_cooldown"), std::string::npos)
    << "A cooldown of 59 with rollback window 60 must be rejected naming drain_handoff_cooldown, got: " << reason;

  settings.cooldown_s = 60;
  EXPECT_EQ(handoff::validate(settings, 47989), "")
    << "A cooldown equal to the rollback window must be accepted.";
}

/**
 * @brief A deadline shorter than the rollback window is rejected; equal is accepted.
 */
TEST(HandoffSettingsTest, DeadlineShorterThanRollbackWindowIsRejected) {
  auto settings = default_settings();
  settings.deadline_s = 59;
  settings.rollback_window_s = 60;
  const auto reason = handoff::validate(settings, 47989);
  EXPECT_NE(reason.find("drain_handoff_deadline"), std::string::npos)
    << "A deadline of 59 with rollback window 60 must be rejected naming drain_handoff_deadline, got: " << reason;

  settings.deadline_s = 60;
  EXPECT_EQ(handoff::validate(settings, 47989), "")
    << "A deadline equal to the rollback window must be accepted.";
}

/**
 * @brief An offset of 0 is rejected because the two bases would be equal.
 */
TEST(HandoffSettingsTest, ZeroOffsetIsRejected) {
  auto settings = default_settings();
  settings.udp_offset = 0;
  const auto reason = handoff::validate(settings, 47989);
  EXPECT_NE(reason.find("drain_handoff_udp_offset"), std::string::npos)
    << "An offset of 0 must be rejected naming drain_handoff_udp_offset, got: " << reason;
}

/**
 * @brief A UDP port below 1024 in either base is rejected.
 */
TEST(HandoffSettingsTest, UdpPortBelowRangeIsRejected) {
  auto settings = default_settings();
  // Base 1014: UDP ports 1023, 1024, 1025; 1023 is below 1024.
  settings.udp_offset = 1014 - 47989;
  const auto reason = handoff::validate(settings, 47989);
  EXPECT_NE(reason.find("drain_handoff_udp_offset"), std::string::npos)
    << "A base whose UDP ports include 1023 must be rejected naming drain_handoff_udp_offset, got: " << reason;
}

/**
 * @brief Lowest valid base: UDP ports 1024 to 1026 are accepted.
 */
TEST(HandoffSettingsTest, UdpPortAtLowerBoundIsAccepted) {
  auto settings = default_settings();
  settings.udp_offset = 1015 - 47989;  // UDP 1024, 1025, 1026
  EXPECT_EQ(handoff::validate(settings, 47989), "")
    << "UDP ports starting at 1024 must be accepted.";
}

/**
 * @brief A UDP port above 65535 in either base is rejected.
 */
TEST(HandoffSettingsTest, UdpPortAboveRangeIsRejected) {
  auto settings = default_settings();
  settings.udp_offset = 65525 - 47989;  // UDP 65534, 65535, 65536; 65536 is out of range.
  const auto reason = handoff::validate(settings, 47989);
  EXPECT_NE(reason.find("drain_handoff_udp_offset"), std::string::npos)
    << "A base whose UDP ports include 65536 must be rejected naming drain_handoff_udp_offset, got: " << reason;
}

/**
 * @brief Highest valid base: UDP ports 65533 to 65535 are accepted.
 */
TEST(HandoffSettingsTest, UdpPortAtUpperBoundIsAccepted) {
  auto settings = default_settings();
  settings.udp_offset = 65524 - 47989;  // UDP 65533, 65534, 65535
  EXPECT_EQ(handoff::validate(settings, 47989), "")
    << "UDP ports ending at 65535 must be accepted.";
}

/**
 * @brief A second base whose UDP port equals a TCP port of the configured port is rejected.
 */
TEST(HandoffSettingsTest, SecondBaseUdpPortEqualToTcpPortIsRejected) {
  auto settings = default_settings();
  settings.udp_offset = 11;  // base 48000; UDP 48011, 48000+10=48010 is RTSP TCP.
  const auto reason = handoff::validate(settings, 47989);
  EXPECT_NE(reason.find("drain_handoff_udp_offset"), std::string::npos)
    << "A UDP port equal to the RTSP TCP port 48010 must be rejected naming drain_handoff_udp_offset, got: " << reason;
}

/**
 * @brief The second base is checked against TCP too: a base whose UDP ports hit the HTTP port is rejected.
 */
TEST(HandoffSettingsTest, BaseUdpPortEqualToHttpPortIsRejected) {
  auto settings = default_settings();
  settings.udp_offset = 47979 - 47989;  // base 47979; UDP 47988, 47989, 47990; 47989 is HTTP, 47990 web UI.
  const auto reason = handoff::validate(settings, 47989);
  EXPECT_NE(reason.find("drain_handoff_udp_offset"), std::string::npos)
    << "A UDP port equal to the HTTP port 47989 must be rejected naming drain_handoff_udp_offset, got: " << reason;
}

/**
 * @brief Two bases that share a UDP port are rejected.
 */
TEST(HandoffSettingsTest, BasesSharingUdpPortAreRejected) {
  auto settings = default_settings();
  settings.udp_offset = 1;  // base 47990; UDP 47999, 48000, 48001 overlap base 47989's 47999 and 48000.
  const auto reason = handoff::validate(settings, 47989);
  EXPECT_NE(reason.find("drain_handoff_udp_offset"), std::string::npos)
    << "Bases 47989 and 47990 share UDP ports and must be rejected naming drain_handoff_udp_offset, got: " << reason;
}

/**
 * @brief Failure reasons are one line.
 */
TEST(HandoffSettingsTest, FailureReasonIsOneLine) {
  auto settings = default_settings();
  settings.deadline_s = 0;
  EXPECT_EQ(handoff::validate(settings, 47989).find('\n'), std::string::npos)
    << "The validation reason must be a one-line string.";
}

/**
 * @brief Rule order: the deadline range is checked before the ready timeout.
 */
TEST(HandoffSettingsTest, OrderDeadlineRangeBeforeReadyTimeout) {
  auto deadline_only = default_settings();
  deadline_only.deadline_s = 0;
  auto both = deadline_only;
  both.ready_timeout_s = 0;
  EXPECT_EQ(handoff::validate(both, 47989), handoff::validate(deadline_only, 47989))
    << "With both deadline and ready timeout invalid, the reason must be the deadline one (first rule wins).";
}

/**
 * @brief Rule order: the ready timeout is checked before the rollback window.
 */
TEST(HandoffSettingsTest, OrderReadyTimeoutBeforeRollbackWindow) {
  auto ready_only = default_settings();
  ready_only.ready_timeout_s = 0;
  auto both = ready_only;
  both.rollback_window_s = 0;
  EXPECT_EQ(handoff::validate(both, 47989), handoff::validate(ready_only, 47989))
    << "With both ready timeout and rollback window invalid, the reason must be the ready timeout one.";
}

/**
 * @brief Rule order: the cooldown rule is checked before the deadline-versus-rollback rule.
 */
TEST(HandoffSettingsTest, OrderCooldownBeforeDeadlineVersusRollback) {
  auto cooldown_only = default_settings();
  cooldown_only.cooldown_s = 59;
  auto both = cooldown_only;
  both.deadline_s = 59;
  EXPECT_EQ(handoff::validate(both, 47989), handoff::validate(cooldown_only, 47989))
    << "With cooldown and deadline both shorter than the rollback window, the cooldown reason must win.";
}

/**
 * @brief Rule order: the offset-zero rule is checked after the deadline rules.
 */
TEST(HandoffSettingsTest, OrderDeadlineBeforeOffset) {
  auto deadline_only = default_settings();
  deadline_only.deadline_s = 59;
  auto both = deadline_only;
  both.udp_offset = 0;
  EXPECT_EQ(handoff::validate(both, 47989), handoff::validate(deadline_only, 47989))
    << "With deadline shorter than the rollback window and offset 0, the deadline reason must win.";
}

/**
 * @brief Enable check: the configured `wlr` capture with no refusing condition is allowed.
 */
TEST(HandoffSettingsTest, CheckEnableAllowsWlrWithNoRefusingCondition) {
  EXPECT_EQ(handoff::check_enable(good_context()), "")
    << "capture = wlr with no FRESH_STATE, no display settings manager and a runtime dir must enable.";
}

/**
 * @brief Enable check: every capture backend other than exactly `wlr` is refused with the documented text.
 */
TEST(HandoffSettingsTest, CheckEnableRefusesOtherCaptureBackendsWithDocumentedReason) {
  for (const std::string capture : {"kms", "kwin", "nvfbc", "portal", "x11", "WLR"}) {
    auto context = good_context();
    context.capture = capture;
    EXPECT_EQ(handoff::check_enable(context), "drain handoff is supported only with capture = wlr (capture = " + capture + ")")
      << "capture = " << capture << " must refuse with the documented reason text.";
  }
}

/**
 * @brief Enable check: an empty capture is refused and shown as `<empty>` in the reason.
 */
TEST(HandoffSettingsTest, CheckEnableRefusesEmptyCaptureShownAsEmptyMarker) {
  auto context = good_context();
  context.capture = "";
  EXPECT_EQ(handoff::check_enable(context), "drain handoff is supported only with capture = wlr (capture = <empty>)")
    << "An empty capture must be refused and shown as <empty> in the reason.";
}

/**
 * @brief Enable check: FRESH_STATE alone refuses.
 */
TEST(HandoffSettingsTest, CheckEnableRefusesFreshState) {
  auto context = good_context();
  context.fresh_state = true;
  EXPECT_FALSE(handoff::check_enable(context).empty())
    << "FRESH_STATE set must refuse to enable.";
}

/**
 * @brief Enable check: a display settings manager alone refuses.
 */
TEST(HandoffSettingsTest, CheckEnableRefusesDisplaySettingsManager) {
  auto context = good_context();
  context.display_settings_manager = true;
  EXPECT_FALSE(handoff::check_enable(context).empty())
    << "A display settings manager must refuse to enable.";
}

/**
 * @brief Enable check: no runtime directory alone refuses.
 */
TEST(HandoffSettingsTest, CheckEnableRefusesWithoutRuntimeDirectory) {
  auto context = good_context();
  context.runtime_dir_available = false;
  EXPECT_FALSE(handoff::check_enable(context).empty())
    << "Without RUNTIME_DIRECTORY or XDG_RUNTIME_DIR the feature must refuse to enable.";
}

/**
 * @brief Enable check: the four refusal conditions give four different reasons, so the order checks below are meaningful.
 */
TEST(HandoffSettingsTest, CheckEnableRefusalReasonsAreDistinct) {
  auto capture_bad = good_context();
  capture_bad.capture = "kms";
  auto fresh = good_context();
  fresh.fresh_state = true;
  auto manager = good_context();
  manager.display_settings_manager = true;
  auto runtime = good_context();
  runtime.runtime_dir_available = false;

  const auto capture_reason = handoff::check_enable(capture_bad);
  const auto fresh_reason = handoff::check_enable(fresh);
  const auto manager_reason = handoff::check_enable(manager);
  const auto runtime_reason = handoff::check_enable(runtime);
  EXPECT_NE(capture_reason, fresh_reason) << "Capture and FRESH_STATE refusals must have different reasons.";
  EXPECT_NE(fresh_reason, manager_reason) << "FRESH_STATE and display settings manager refusals must differ.";
  EXPECT_NE(manager_reason, runtime_reason) << "Display settings manager and runtime dir refusals must differ.";
}

/**
 * @brief Enable check order: a bad capture is reported before FRESH_STATE.
 */
TEST(HandoffSettingsTest, CheckEnableOrderCaptureBeforeFreshState) {
  auto capture_only = good_context();
  capture_only.capture = "kms";
  auto both = capture_only;
  both.fresh_state = true;
  EXPECT_EQ(handoff::check_enable(both), handoff::check_enable(capture_only))
    << "With a bad capture and FRESH_STATE, the capture reason must be returned (first rule wins).";
}

/**
 * @brief Enable check order: FRESH_STATE is reported before the display settings manager.
 */
TEST(HandoffSettingsTest, CheckEnableOrderFreshStateBeforeDisplayManager) {
  auto fresh_only = good_context();
  fresh_only.fresh_state = true;
  auto both = fresh_only;
  both.display_settings_manager = true;
  EXPECT_EQ(handoff::check_enable(both), handoff::check_enable(fresh_only))
    << "With FRESH_STATE and a display settings manager, the FRESH_STATE reason must be returned.";
}

/**
 * @brief Enable check order: the display settings manager is reported before the missing runtime directory.
 */
TEST(HandoffSettingsTest, CheckEnableOrderDisplayManagerBeforeRuntimeDirectory) {
  auto manager_only = good_context();
  manager_only.display_settings_manager = true;
  auto both = manager_only;
  both.runtime_dir_available = false;
  EXPECT_EQ(handoff::check_enable(both), handoff::check_enable(manager_only))
    << "With a display settings manager and no runtime dir, the manager reason must be returned.";
}

/**
 * @brief An offset near INT_MAX is rejected with the range error, without overflow.
 */
TEST(HandoffSettingsTest, HugeOffsetGivesRangeErrorWithoutOverflow) {
  auto settings = default_settings();
  settings.udp_offset = std::numeric_limits<int>::max();
  const auto reason = handoff::validate(settings, 47989);
  EXPECT_NE(reason.find("drain_handoff_udp_offset"), std::string::npos)
    << "An offset of INT_MAX puts the second base far outside 1024-65535; it must be rejected naming drain_handoff_udp_offset, got: "
    << reason;
}

/**
 * @brief A large negative offset is rejected with the range error, without overflow.
 */
TEST(HandoffSettingsTest, HugeNegativeOffsetGivesRangeErrorWithoutOverflow) {
  auto settings = default_settings();
  settings.udp_offset = std::numeric_limits<int>::min();
  const auto reason = handoff::validate(settings, 47989);
  EXPECT_NE(reason.find("drain_handoff_udp_offset"), std::string::npos)
    << "An offset of INT_MIN puts the second base far below 1024; it must be rejected naming drain_handoff_udp_offset, got: "
    << reason;
}

/**
 * @brief With an out-of-range UDP port in the second base and a TCP collision in the same base, the range rule
 * decides first.
 *
 * The configured port's own base can never collide with its TCP ports (P+9..P+11 against P-5, P, P+1, P+21), so
 * the collision is put in the second base here: port 1030 with offset -16 gives base 1014, whose UDP ports
 * 1023, 1024, 1025 include 1023 (outside 1024-65535) and 1025 (RTSP TCP of port 1030 is 1051, HTTP 1030, web UI 1031,
 * HTTPS 1025 which collides).
 */
TEST(HandoffSettingsTest, RangeRuleWinsOverTcpCollisionInSameBase) {
  auto settings = default_settings();
  settings.udp_offset = 1014 - 1030;
  const auto reason = handoff::validate(settings, 1030);
  EXPECT_NE(reason.find("drain_handoff_udp_offset"), std::string::npos)
    << "The range rule comes before the TCP collision rule; the reason must name drain_handoff_udp_offset, got: " << reason;
  EXPECT_FALSE(reason.empty()) << "The configuration must be rejected.";
}

#endif
