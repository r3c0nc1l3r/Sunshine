/**
 * @file tests/unit/test_upnp_handoff.cpp
 * @brief Tests for the drain-handoff UPnP rules (P11): the planned mappings per UDP base, the unmap scope of a
 *        draining process, and the background removal guards that keep the old process's own mappings.
 *
 * Only the pure functions of `src/upnp.h` and the port helpers of `src/handoff/settings.h` are used. Nothing starts
 * UPnP and no gateway is contacted. The expected ports follow the spec: TCP at `port - 5` (HTTPS), `port` (HTTP),
 * `port + 1` (web UI, only when reachable from the WAN) and `port + 21` (RTSP); UDP at `base + 9` (video), `base + 10`
 * (control) and `base + 11` (audio).
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

// local includes
#include <src/upnp.h>

#ifdef __linux__
  #include <src/handoff/settings.h>
#endif

namespace {
  using planned_t = upnp::planned_mapping_t;
  using port_set_t = std::set<std::uint16_t>;

  /**
   * @brief Ports of every entry of a plan.
   *
   * @param plan Plan to read.
   * @return The set of all ports in the plan, TCP and UDP.
   */
  port_set_t ports_of(const std::vector<planned_t> &plan) {
    port_set_t result;
    for (const auto &entry : plan) {
      result.insert(entry.port);
    }
    return result;
  }

  /**
   * @brief Ports of the UDP entries of a plan.
   *
   * @param plan Plan to read.
   * @return The set of UDP ports in the plan.
   */
  port_set_t udp_ports_of(const std::vector<planned_t> &plan) {
    port_set_t result;
    for (const auto &entry : plan) {
      if (entry.udp) {
        result.insert(entry.port);
      }
    }
    return result;
  }

  /**
   * @brief Ports of the TCP entries of a plan.
   *
   * @param plan Plan to read.
   * @return The set of TCP ports in the plan.
   */
  port_set_t tcp_ports_of(const std::vector<planned_t> &plan) {
    port_set_t result;
    for (const auto &entry : plan) {
      if (!entry.udp) {
        result.insert(entry.port);
      }
    }
    return result;
  }

  /**
   * @brief Text form of a port set, for assertion messages.
   *
   * @param ports Ports to format.
   * @return Comma-separated list in ascending order.
   */
  std::string describe(const port_set_t &ports) {
    std::string text;
    for (const auto port : ports) {
      if (!text.empty()) {
        text += ",";
      }
      text += std::to_string(port);
    }
    return "{" + text + "}";
  }

  /**
   * @brief The three UDP ports of a base, from the spec: video `base + 9`, control `base + 10`, audio `base + 11`.
   *
   * @param base UDP base.
   * @return The three ports.
   */
  port_set_t spec_udp(int base) {
    return {
      static_cast<std::uint16_t>(base + 9),
      static_cast<std::uint16_t>(base + 10),
      static_cast<std::uint16_t>(base + 11),
    };
  }

  /**
   * @brief The four TCP ports of a configured port, from the spec: `port - 5`, `port`, `port + 1` and `port + 21`.
   *
   * @param port Configured `port`.
   * @return The four ports.
   */
  port_set_t spec_tcp(int port) {
    return {
      static_cast<std::uint16_t>(port - 5),
      static_cast<std::uint16_t>(port),
      static_cast<std::uint16_t>(port + 1),
      static_cast<std::uint16_t>(port + 21),
    };
  }

  /**
   * @brief Union of two port sets.
   *
   * @param a First set.
   * @param b Second set.
   * @return Every port of both sets.
   */
  port_set_t unite(const port_set_t &a, const port_set_t &b) {
    port_set_t result = a;
    result.insert(b.begin(), b.end());
    return result;
  }

  /**
   * @brief Intersection of two port sets.
   *
   * @param a First set.
   * @param b Second set.
   * @return Ports that are in both sets.
   */
  port_set_t intersect(const port_set_t &a, const port_set_t &b) {
    port_set_t result;
    for (const auto port : a) {
      if (b.count(port) != 0) {
        result.insert(port);
      }
    }
    return result;
  }
}  // namespace

/**
 * @brief Test fixture for the unmap scope. Restores the scope to `all` before and after each test.
 */
class HandoffUpnpScopeTest: public BaseTest {  // NOSONAR(cpp:S3656): protected members are intentional for test fixture subclassing
protected:
  /**
   * @brief Start every test with the normal scope.
   */
  void SetUp() override {
    upnp::set_unmap_scope(upnp::unmap_scope_e::all);
    BaseTest::SetUp();
  }

  /**
   * @brief Put the scope back to `all` so later tests see the normal exit behaviour.
   */
  void TearDown() override {
    upnp::set_unmap_scope(upnp::unmap_scope_e::all);
    BaseTest::TearDown();
  }
};

// ---------------------------------------------------------------------------
// plan_mappings
// ---------------------------------------------------------------------------

/**
 * @brief Test: with equal bases and no web UI on the WAN, the plan has three TCP and three UDP mappings.
 */
TEST(HandoffUpnpTest, PlanWithEqualBasesAndNoWebUiHasSixMappings) {
  const auto plan = upnp::plan_mappings(47989, 47989, false);

  ASSERT_EQ(plan.size(), 6U) << "without the web UI on the WAN the plan must hold RTSP, video, audio, control, HTTP and HTTPS";
  EXPECT_EQ(tcp_ports_of(plan), (port_set_t {47984, 47989, 48010}))
    << "TCP ports must be port-5 (HTTPS), port (HTTP) and port+21 (RTSP), got " << describe(tcp_ports_of(plan));
  EXPECT_EQ(udp_ports_of(plan), (port_set_t {47998, 47999, 48000}))
    << "UDP ports must be base+9, base+10 and base+11 with base == port, got " << describe(udp_ports_of(plan));
}

/**
 * @brief Test: with `web_ui_on_wan` set, the plan also holds the web UI TCP port `port + 1`.
 */
TEST(HandoffUpnpTest, PlanWithWebUiOnWanAddsWebUiPort) {
  const auto plan = upnp::plan_mappings(47989, 47989, true);

  ASSERT_EQ(plan.size(), 7U) << "with the web UI reachable from the WAN the plan must hold seven mappings";
  EXPECT_TRUE(tcp_ports_of(plan).count(47990) == 1U)
    << "the web UI TCP port port+1 (47990) must be in the plan when web_ui_on_wan is set";
  EXPECT_EQ(tcp_ports_of(plan), spec_tcp(47989))
    << "with web_ui_on_wan the TCP ports must be exactly the four fixed ports, got " << describe(tcp_ports_of(plan));
}

/**
 * @brief Test: the entries come in the documented request order (RTSP, video, audio, control, HTTP, HTTPS, web UI).
 */
TEST(HandoffUpnpTest, PlanEntriesComeInDocumentedOrder) {
  const auto plan = upnp::plan_mappings(47989, 47989, true);

  ASSERT_EQ(plan.size(), 7U) << "setup: the plan must hold seven entries to check the order";
  const std::vector<std::pair<std::uint16_t, bool>> expected {
    {48010, false},  // RTSP, port + 21
    {47998, true},  // video, base + 9
    {48000, true},  // audio, base + 11
    {47999, true},  // control, base + 10
    {47989, false},  // client HTTP, port
    {47984, false},  // client HTTPS, port - 5
    {47990, false},  // web UI, port + 1
  };
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(plan[i].port, expected[i].first) << "entry " << i << " has the wrong port; the request order is RTSP, video, audio, control, HTTP, HTTPS, web UI";
    EXPECT_EQ(plan[i].udp, expected[i].second) << "entry " << i << " has the wrong protocol (udp flag); the request order is RTSP, video, audio, control, HTTP, HTTPS, web UI";
  }
}

/**
 * @brief Test: a UDP base different from `port` moves only the UDP ports; the TCP ports stay at `port`.
 */
TEST(HandoffUpnpTest, PlanWithOtherUdpBaseMovesOnlyUdpPorts) {
  const auto plan = upnp::plan_mappings(47989, 48089, true);

  EXPECT_EQ(udp_ports_of(plan), (port_set_t {48098, 48099, 48100}))
    << "UDP ports must follow the UDP base 48089 (base+9..base+11), got " << describe(udp_ports_of(plan));
  EXPECT_EQ(tcp_ports_of(plan), spec_tcp(47989))
    << "TCP ports must not follow the UDP base; they stay at the configured port, got " << describe(tcp_ports_of(plan));
}

/**
 * @brief Test: the UDP ports of a plan are exactly the spec ports of its base, for an arbitrary base.
 */
TEST(HandoffUpnpTest, PlanUdpPortsMatchSpecForArbitraryBase) {
  const auto plan = upnp::plan_mappings(40000, 40100, false);

  EXPECT_EQ(udp_ports_of(plan), spec_udp(40100))
    << "UDP ports of base 40100 must be 40109, 40110 and 40111, got " << describe(udp_ports_of(plan));
  EXPECT_EQ(tcp_ports_of(plan), (port_set_t {39995, 40000, 40021}))
    << "without the web UI the TCP ports must be 39995, 40000 and 40021, got " << describe(tcp_ports_of(plan));
}

/**
 * @brief Test: every planned entry has a non-empty description for the gateway.
 */
TEST(HandoffUpnpTest, PlanEntriesHaveDescriptions) {
  const auto plan = upnp::plan_mappings(47989, 48089, true);

  for (std::size_t i = 0; i < plan.size(); ++i) {
    EXPECT_FALSE(plan[i].description.empty()) << "planned mapping " << i << " has no description for the gateway";
  }
}

/**
 * @brief Test: the plan is deterministic, so two calls with the same input give the same entries.
 */
TEST(HandoffUpnpTest, PlanIsRepeatable) {
  const auto first = upnp::plan_mappings(47989, 48089, true);
  const auto second = upnp::plan_mappings(47989, 48089, true);

  ASSERT_EQ(first.size(), second.size()) << "two calls with the same input returned plans of different sizes";
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(first[i].port, second[i].port) << "entry " << i << " changed port between two identical calls";
    EXPECT_EQ(first[i].udp, second[i].udp) << "entry " << i << " changed protocol between two identical calls";
  }
}

// ---------------------------------------------------------------------------
// select_for_scope
// ---------------------------------------------------------------------------

/**
 * @brief Test: scope `all` keeps every entry of the plan, in order.
 */
TEST(HandoffUpnpTest, ScopeAllKeepsEveryEntryInOrder) {
  const auto plan = upnp::plan_mappings(47989, 48089, true);

  const auto selected = upnp::select_for_scope(plan, upnp::unmap_scope_e::all);

  ASSERT_EQ(selected.size(), plan.size()) << "scope all must keep every entry of the plan";
  for (std::size_t i = 0; i < plan.size(); ++i) {
    EXPECT_EQ(selected[i].port, plan[i].port) << "scope all changed the order or ports at entry " << i;
    EXPECT_EQ(selected[i].udp, plan[i].udp) << "scope all changed the protocol at entry " << i;
  }
}

/**
 * @brief Test: scope `udp_only` keeps only the UDP entries, in their original order.
 */
TEST(HandoffUpnpTest, ScopeUdpOnlyKeepsOnlyUdpEntries) {
  const auto plan = upnp::plan_mappings(47989, 47989, true);

  const auto selected = upnp::select_for_scope(plan, upnp::unmap_scope_e::udp_only);

  ASSERT_EQ(selected.size(), 3U) << "scope udp_only must keep the three UDP entries of the base and no TCP entry";
  for (const auto &entry : selected) {
    EXPECT_TRUE(entry.udp) << "scope udp_only kept a TCP mapping on port " << entry.port << "; a draining process must never unmap TCP ports";
  }
  EXPECT_EQ(selected[0].port, 47998) << "the first kept UDP entry must be video (base+9)";
  EXPECT_EQ(selected[1].port, 48000) << "the second kept UDP entry must be audio (base+11), in the original request order";
  EXPECT_EQ(selected[2].port, 47999) << "the third kept UDP entry must be control (base+10), in the original request order";
}

/**
 * @brief Test: scope `none` keeps nothing, for every plan.
 */
TEST(HandoffUpnpTest, ScopeNoneKeepsNothing) {
  EXPECT_TRUE(upnp::select_for_scope(upnp::plan_mappings(47989, 47989, true), upnp::unmap_scope_e::none).empty())
    << "scope none must keep no entry when the bases are equal";
  EXPECT_TRUE(upnp::select_for_scope(upnp::plan_mappings(47989, 48089, true), upnp::unmap_scope_e::none).empty())
    << "scope none must keep no entry for any base";
}

/**
 * @brief Test: a draining process whose active peer uses another base, scope `udp_only`, selects only its own UDP base
 *        and no TCP port.
 */
TEST(HandoffUpnpTest, DrainingScopeUdpOnlySelectsOwnBaseOnly) {
  const auto old_plan = upnp::plan_mappings(47989, 47989, true);

  const auto selected = upnp::select_for_scope(old_plan, upnp::unmap_scope_e::udp_only);
  const auto selected_ports = ports_of(selected);

  EXPECT_EQ(selected_ports, spec_udp(47989))
    << "the draining process must select exactly its own UDP base for unmapping, got " << describe(selected_ports);
  EXPECT_TRUE(intersect(selected_ports, spec_tcp(47989)).empty())
    << "the draining process selected a shared TCP port for unmapping: " << describe(intersect(selected_ports, spec_tcp(47989)));
}

/**
 * @brief Test: every scope returns an empty selection for an empty plan.
 */
TEST(HandoffUpnpTest, EveryScopeOfEmptyPlanIsEmpty) {
  const std::vector<planned_t> empty_plan;

  EXPECT_TRUE(upnp::select_for_scope(empty_plan, upnp::unmap_scope_e::all).empty()) << "scope all of an empty plan must be empty";
  EXPECT_TRUE(upnp::select_for_scope(empty_plan, upnp::unmap_scope_e::udp_only).empty()) << "scope udp_only of an empty plan must be empty";
  EXPECT_TRUE(upnp::select_for_scope(empty_plan, upnp::unmap_scope_e::none).empty()) << "scope none of an empty plan must be empty";
}

// ---------------------------------------------------------------------------
// removal_plan: the second guard of remove_ports_async
// ---------------------------------------------------------------------------

/**
 * @brief Test: an empty request gives an empty removal plan.
 */
TEST(HandoffUpnpTest, RemovalPlanOfEmptyRequestIsEmpty) {
  const auto own = upnp::plan_mappings(47989, 47989, true);

  EXPECT_TRUE(upnp::removal_plan({}, own).empty()) << "removal_plan with no requested port must return nothing";
}

/**
 * @brief Test: ports that this process does not map come back as UDP entries, in ascending order.
 */
TEST(HandoffUpnpTest, RemovalPlanReturnsForeignPortsAscendingAsUdp) {
  const auto own = upnp::plan_mappings(47989, 47989, true);

  const auto removal = upnp::removal_plan({48100, 48098, 48099}, own);

  ASSERT_EQ(removal.size(), 3U) << "all three foreign UDP ports must be planned for removal";
  EXPECT_EQ(removal[0].port, 48098) << "removal entries must be in ascending port order (first)";
  EXPECT_EQ(removal[1].port, 48099) << "removal entries must be in ascending port order (second)";
  EXPECT_EQ(removal[2].port, 48100) << "removal entries must be in ascending port order (third)";
  for (const auto &entry : removal) {
    EXPECT_TRUE(entry.udp) << "removal planned a TCP mapping on port " << entry.port << "; the removal may delete only UDP entries";
  }
}

/**
 * @brief Test: UDP ports this process maps itself are never selected for removal, even when requested.
 */
TEST(HandoffUpnpTest, RemovalPlanDropsOwnUdpPorts) {
  const auto own = upnp::plan_mappings(47989, 47989, true);

  const auto removal = upnp::removal_plan({47998, 47999, 48000}, own);

  EXPECT_TRUE(removal.empty()) << "removal_plan selected UDP ports of this process's own base: " << describe(ports_of(removal));
}

/**
 * @brief Test: TCP ports this process maps itself (RTSP, HTTP, HTTPS, web UI) are never selected for removal.
 */
TEST(HandoffUpnpTest, RemovalPlanDropsOwnTcpPorts) {
  const auto own = upnp::plan_mappings(47989, 48089, true);

  const auto removal = upnp::removal_plan({47984, 47989, 47990, 48010}, own);

  EXPECT_TRUE(removal.empty()) << "removal_plan selected a shared TCP port of this process: " << describe(ports_of(removal));
}

/**
 * @brief Test: a mixed request keeps only the ports the process does not map.
 */
TEST(HandoffUpnpTest, RemovalPlanOfMixedRequestKeepsOnlyForeignPorts) {
  const auto own = upnp::plan_mappings(47989, 47989, false);

  const auto removal = upnp::removal_plan({47998, 48098, 48010, 48099}, own);

  EXPECT_EQ(ports_of(removal), (port_set_t {48098, 48099}))
    << "only the ports this process does not map may be removed, got " << describe(ports_of(removal));
  for (const auto &entry : removal) {
    EXPECT_TRUE(entry.udp) << "mixed removal planned a TCP mapping on port " << entry.port;
  }
}

/**
 * @brief Test: with no own plan (`start()` never ran), every requested port is a candidate, as UDP.
 */
TEST(HandoffUpnpTest, RemovalPlanWithEmptyOwnPlanKeepsAllRequestedAsUdp) {
  const std::vector<planned_t> empty_own;

  const auto removal = upnp::removal_plan({48098}, empty_own);

  ASSERT_EQ(removal.size(), 1U) << "with no own mappings the requested foreign port must be kept";
  EXPECT_EQ(removal[0].port, 48098) << "the kept port must be the requested one";
  EXPECT_TRUE(removal[0].udp) << "the kept entry must be a UDP mapping";
}

// ---------------------------------------------------------------------------
// upnp_ports_to_remove (wave 1 helper, the set the rollback passes to remove_ports_async)
// ---------------------------------------------------------------------------

#ifdef __linux__
/**
 * @brief Test: the removal set of a rollback is the killed process's UDP base, when that base differs from the old one.
 */
TEST(HandoffUpnpTest, PortsToRemoveAreNewBaseUdpPorts) {
  EXPECT_EQ(handoff::upnp_ports_to_remove(48089, 47989, 47989), (port_set_t {48098, 48099, 48100}))
    << "the removal set must be the new base's UDP ports when they are not the old base's";
}

/**
 * @brief Test: equal bases give an empty removal set, so nothing the old process uses is ever removed.
 */
TEST(HandoffUpnpTest, PortsToRemoveAreEmptyForEqualBases) {
  EXPECT_TRUE(handoff::upnp_ports_to_remove(47989, 47989, 47989).empty())
    << "equal UDP bases must give an empty removal set";
  EXPECT_TRUE(handoff::upnp_ports_to_remove(48089, 48089, 47989).empty())
    << "equal UDP bases (both not the configured port) must give an empty removal set";
}

/**
 * @brief Test: overlapping bases still exclude every UDP port of the old process's own base.
 */
TEST(HandoffUpnpTest, PortsToRemoveExcludeOldBaseWhenBasesOverlap) {
  const auto removal = handoff::upnp_ports_to_remove(48001, 48000, 40000);

  EXPECT_EQ(removal, (port_set_t {48012}))
    << "with bases 48001 (new) and 48000 (old) only 48012 is new-only, got " << describe(removal);
  EXPECT_TRUE(intersect(removal, spec_udp(48000)).empty())
    << "the removal set contains a UDP port of the old base: " << describe(intersect(removal, spec_udp(48000)));
}

/**
 * @brief Test: a UDP port that coincides with a shared TCP port is never in the removal set.
 */
TEST(HandoffUpnpTest, PortsToRemoveExcludeSharedTcpPorts) {
  // New base 39992 maps UDP 40001 (also the web UI TCP port of configured port 40000), 40002 and 40003.
  const auto removal = handoff::upnp_ports_to_remove(39992, 40000, 40000);

  EXPECT_EQ(removal, (port_set_t {40002, 40003}))
    << "the removal set must drop the UDP port that is also a shared TCP port, got " << describe(removal);
  EXPECT_TRUE(intersect(removal, spec_tcp(40000)).empty())
    << "the removal set contains a shared TCP port: " << describe(intersect(removal, spec_tcp(40000)));
}
#endif

// ---------------------------------------------------------------------------
// Hard rule: cleanup never removes the old process's own mappings
// ---------------------------------------------------------------------------

#ifdef __linux__
/**
 * @brief Test: for several configurations, what the old process's removal guard would delete never contains a port the
 *        old process maps (its TCP ports, its UDP base), even after its own filter.
 */
TEST(HandoffUpnpTest, CleanupNeverRemovesOldProcessMappings) {
  struct config_t {
    int port;
    int old_base;
    int new_base;
    bool web_ui;
  };

  const std::vector<config_t> configs {
    {47989, 47989, 48089, true},  // normal: old on the port, new on port + 100
    {47989, 48089, 47989, true},  // reversed roles
    {47989, 47989, 48089, false},  // web UI not on the WAN
    {40000, 48000, 48001, true},  // overlapping bases (misconfigured offset)
    {40000, 40000, 39992, true},  // new base maps a shared TCP port
    {47989, 47989, 47989, true},  // equal bases: nothing to remove
  };

  for (const auto &config : configs) {
    const auto own = upnp::plan_mappings(config.port, config.old_base, config.web_ui);
    const auto requested = handoff::upnp_ports_to_remove(config.new_base, config.old_base, config.port);
    const auto removal = upnp::removal_plan(requested, own);
    const auto removed = ports_of(removal);

    const auto old_mapped = unite(spec_udp(config.old_base), spec_tcp(config.port));
    EXPECT_TRUE(intersect(removed, old_mapped).empty())
      << "port " << config.port << ", old base " << config.old_base << ", new base " << config.new_base
      << ": the removal would delete the old process's own mappings " << describe(intersect(removed, old_mapped));
    EXPECT_TRUE(intersect(removed, ports_of(own)).empty())
      << "port " << config.port << ", old base " << config.old_base << ", new base " << config.new_base
      << ": the removal would delete a port of this process's own plan " << describe(intersect(removed, ports_of(own)));
    for (const auto &entry : removal) {
      EXPECT_TRUE(entry.udp) << "port " << config.port << ": the removal planned a TCP mapping on port " << entry.port;
    }
    EXPECT_EQ(removed, requested)
      << "port " << config.port << ", old base " << config.old_base << ", new base " << config.new_base
      << ": the wave 1 set already excludes the old process's ports, so the second guard must keep all of it";
  }
}
#endif

/**
 * @brief Test: a wrong removal set (the old base, the shared TCP ports and the new base together) still never removes
 *        a mapping of this process; only the new-only UDP ports remain.
 */
TEST(HandoffUpnpTest, MisconfiguredRemovalSetCannotRemoveOwnMappings) {
  const auto own = upnp::plan_mappings(47989, 47989, true);
  const port_set_t wrong = unite(unite(spec_udp(47989), spec_tcp(47989)), spec_udp(48089));

  const auto removal = upnp::removal_plan(wrong, own);

  EXPECT_EQ(ports_of(removal), spec_udp(48089))
    << "only the new-only UDP ports may survive the guard, got " << describe(ports_of(removal));
  EXPECT_TRUE(intersect(ports_of(removal), ports_of(own)).empty())
    << "a port of this process's own plan survived the guard: " << describe(intersect(ports_of(removal), ports_of(own)));
}

/**
 * @brief Test: with overlapping bases, a wrong removal set still never removes this process's own UDP base or TCP ports.
 */
TEST(HandoffUpnpTest, MisconfiguredRemovalSetWithOverlappingBasesKeepsOwnPorts) {
  const auto own = upnp::plan_mappings(40000, 48000, true);
  const port_set_t wrong = unite(unite(spec_udp(48000), spec_udp(48001)), spec_tcp(40000));

  const auto removal = upnp::removal_plan(wrong, own);

  EXPECT_EQ(ports_of(removal), (port_set_t {48012}))
    << "with overlapping bases only the port that is neither the own base nor a TCP port may survive, got " << describe(ports_of(removal));
}

/**
 * @brief Test: the `removal_plan` guard keeps the old process's ports even when the requested set repeats them.
 */
TEST(HandoffUpnpTest, RemovalPlanKeepsOwnPortsEvenWhenRepeatedInRequest) {
  const auto own = upnp::plan_mappings(47989, 47989, false);

  const auto removal = upnp::removal_plan({47998, 47998, 48098, 48098}, own);

  EXPECT_EQ(ports_of(removal), (port_set_t {48098}))
    << "repeated requested ports must not leak own mappings, got " << describe(ports_of(removal));
}

// ---------------------------------------------------------------------------
// Unmap scope: set_unmap_scope / unmap_scope
// ---------------------------------------------------------------------------

/**
 * @brief Test: after the fixture resets it, the unmap scope reads back as `all`, the normal exit behaviour.
 */
TEST_F(HandoffUpnpScopeTest, ScopeReadsBackAllAfterReset) {
  EXPECT_EQ(upnp::unmap_scope(), upnp::unmap_scope_e::all)
    << "the normal scope must be all";
}

/**
 * @brief Test: `set_unmap_scope(udp_only)` is read back by `unmap_scope()`.
 */
TEST_F(HandoffUpnpScopeTest, SetUdpOnlyIsReadBack) {
  upnp::set_unmap_scope(upnp::unmap_scope_e::udp_only);

  EXPECT_EQ(upnp::unmap_scope(), upnp::unmap_scope_e::udp_only)
    << "unmap_scope() must return the value last set to udp_only";
}

/**
 * @brief Test: `set_unmap_scope(none)` is read back by `unmap_scope()`.
 */
TEST_F(HandoffUpnpScopeTest, SetNoneIsReadBack) {
  upnp::set_unmap_scope(upnp::unmap_scope_e::none);

  EXPECT_EQ(upnp::unmap_scope(), upnp::unmap_scope_e::none)
    << "unmap_scope() must return the value last set to none";
}

/**
 * @brief Test: setting the scope back to `all` (a rollback made this process active again) is read back as `all`.
 */
TEST_F(HandoffUpnpScopeTest, SetBackToAllIsReadBack) {
  upnp::set_unmap_scope(upnp::unmap_scope_e::none);
  upnp::set_unmap_scope(upnp::unmap_scope_e::all);

  EXPECT_EQ(upnp::unmap_scope(), upnp::unmap_scope_e::all)
    << "after a rollback sets all, unmap_scope() must return all";
}

/**
 * @brief Test: the last of several scope changes is the one that is read back.
 */
TEST_F(HandoffUpnpScopeTest, LastSetWins) {
  upnp::set_unmap_scope(upnp::unmap_scope_e::udp_only);
  upnp::set_unmap_scope(upnp::unmap_scope_e::none);
  upnp::set_unmap_scope(upnp::unmap_scope_e::udp_only);

  EXPECT_EQ(upnp::unmap_scope(), upnp::unmap_scope_e::udp_only)
    << "unmap_scope() must return the most recently set scope";
}

/**
 * @brief Test: a draining process with equal bases uses scope `none`, so its selection for unmapping is empty.
 */
TEST_F(HandoffUpnpScopeTest, DrainingWithEqualBasesSelectsNothingUnderNone) {
  upnp::set_unmap_scope(upnp::unmap_scope_e::none);

  const auto selected = upnp::select_for_scope(upnp::plan_mappings(47989, 47989, true), upnp::unmap_scope());

  EXPECT_TRUE(selected.empty())
    << "a draining process whose base equals the active base must unmap nothing at exit";
}

// ---------------------------------------------------------------------------
// remove_ports_async without start()
// ---------------------------------------------------------------------------

/**
 * @brief Test: `remove_ports_async` does nothing and returns at once when `start()` was not called in this process.
 *
 * The call must not wait for gateway discovery (which takes about 2 s), so the bound is far below that.
 */
TEST(HandoffUpnpTest, RemovePortsAsyncWithoutStartReturnsPromptly) {
  const auto begin = std::chrono::steady_clock::now();

  upnp::remove_ports_async({48098, 48099, 48100});

  const auto elapsed = std::chrono::steady_clock::now() - begin;
  EXPECT_LT(elapsed, std::chrono::milliseconds {500})
    << "remove_ports_async must not wait for gateway discovery or the worker; it took too long";
}

/**
 * @brief Test: `remove_ports_async` with an empty set returns at once and does nothing.
 */
TEST(HandoffUpnpTest, RemovePortsAsyncWithEmptySetReturnsPromptly) {
  const auto begin = std::chrono::steady_clock::now();

  upnp::remove_ports_async({});

  const auto elapsed = std::chrono::steady_clock::now() - begin;
  EXPECT_LT(elapsed, std::chrono::milliseconds {500})
    << "remove_ports_async with an empty set must return at once";
}
