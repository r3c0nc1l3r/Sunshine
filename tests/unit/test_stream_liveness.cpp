/**
 * @file tests/unit/test_stream_liveness.cpp
 * @brief Tests for stream session stop reasons and liveness checks (src/stream_liveness.*).
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// lib includes
#include <enet/enet.h>
#ifndef _WIN32
  #include <netinet/in.h>
#endif

// local includes
#include <src/stream_liveness.h>

using namespace std::chrono_literals;
using clock_t_ = std::chrono::steady_clock;

namespace {
  /**
   * @brief Initialize ENet once for the whole test binary.
   * @return `true` when ENet is ready.
   */
  bool enet_ready() {
    static const bool ready = enet_initialize() == 0;
    return ready;
  }

  /**
   * @brief Fixed base time for injected clocks.
   * @return A time point far from the epoch.
   */
  clock_t_::time_point base_time() {
    return clock_t_::time_point {std::chrono::seconds {1000}};
  }
}  // namespace

// ---------------------------------------------------------------------------
// S2: stop reason names
// ---------------------------------------------------------------------------

/**
 * @brief Each stop reason enumerator maps to the exact name from the spec (S2).
 */
TEST(StreamStopReasonTest, EveryEnumeratorHasExactName) {
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::unknown), std::string_view {"unknown"});
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::disconnect), std::string_view {"disconnect"});
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::ping_timeout), std::string_view {"ping-timeout"});
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::media_ping_timeout), std::string_view {"media-ping-timeout"});
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::initial_ping_timeout), std::string_view {"initial-ping-timeout"});
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::protocol_error), std::string_view {"protocol-error"});
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::api_close), std::string_view {"api-close"});
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::stream_ended), std::string_view {"stream-ended"});
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::terminated), std::string_view {"terminated"});
}

/**
 * @brief Every enumerator has a distinct name (S2), so log lines can be told apart.
 */
TEST(StreamStopReasonTest, NamesAreDistinct) {
  std::set<std::string_view> names;
  for (int i = 0; i <= static_cast<int>(stream::stop_reason_e::terminated); ++i) {
    names.insert(stream::to_string(static_cast<stream::stop_reason_e>(i)));
  }
  EXPECT_EQ(names.size(), static_cast<std::size_t>(static_cast<int>(stream::stop_reason_e::terminated) + 1));
}

/**
 * @brief Out-of-range values map to `unknown` (S2).
 */
TEST(StreamStopReasonTest, OutOfRangeValueIsUnknown) {
  EXPECT_EQ(stream::to_string(static_cast<stream::stop_reason_e>(999)), std::string_view {"unknown"});
  EXPECT_EQ(stream::to_string(static_cast<stream::stop_reason_e>(-1)), std::string_view {"unknown"});
}

// ---------------------------------------------------------------------------
// S1a: media-ping liveness
// ---------------------------------------------------------------------------

/**
 * @brief Not armed and never expired before any ping, even far in the future (S1a).
 */
TEST(MediaLivenessTest, NotArmedAndNeverExpiredBeforeAnyPing) {
  stream::liveness::media_liveness_t media;
  EXPECT_FALSE(media.armed());
  EXPECT_FALSE(media.expired(base_time() + std::chrono::hours {24 * 365}, 1s));
  EXPECT_FALSE(media.expired(base_time(), 0ms));
}

/**
 * @brief Armed after the first ping (S1a).
 */
TEST(MediaLivenessTest, ArmedAfterFirstPing) {
  stream::liveness::media_liveness_t media;
  media.on_ping(base_time());
  EXPECT_TRUE(media.armed());
}

/**
 * @brief One millisecond below the timeout is not expired (S1a).
 */
TEST(MediaLivenessTest, NotExpiredJustBelowTimeout) {
  stream::liveness::media_liveness_t media;
  media.on_ping(base_time());
  EXPECT_FALSE(media.expired(base_time() + 4999ms, 5000ms));
}

/**
 * @brief Exactly at the timeout is expired (S1a: `now - last >= timeout`).
 */
TEST(MediaLivenessTest, ExpiredExactlyAtTimeout) {
  stream::liveness::media_liveness_t media;
  media.on_ping(base_time());
  EXPECT_TRUE(media.expired(base_time() + 5000ms, 5000ms));
}

/**
 * @brief Above the timeout is expired (S1a).
 */
TEST(MediaLivenessTest, ExpiredAboveTimeout) {
  stream::liveness::media_liveness_t media;
  media.on_ping(base_time());
  EXPECT_TRUE(media.expired(base_time() + 5001ms, 5000ms));
}

/**
 * @brief A new ping resets the clock (S1a).
 */
TEST(MediaLivenessTest, NewPingResetsClock) {
  stream::liveness::media_liveness_t media;
  media.on_ping(base_time());
  media.on_ping(base_time() + 4s);
  EXPECT_FALSE(media.expired(base_time() + 8s, 5s));  // 4 s since the second ping
  EXPECT_TRUE(media.expired(base_time() + 9s, 5s));  // 5 s since the second ping
}

/**
 * @brief A client pinging every 500 ms is never expired for a 1 s timeout, and expires one timeout after its last ping (S1a).
 */
TEST(MediaLivenessTest, PeriodicPingsKeepAliveAndExpireAfterLast) {
  stream::liveness::media_liveness_t media;
  constexpr auto ping_gap = 500ms;
  constexpr auto timeout = 1000ms;
  constexpr int pings = 20;

  for (int i = 0; i < pings; ++i) {
    const auto t = base_time() + i * ping_gap;
    media.on_ping(t);
    EXPECT_FALSE(media.expired(t + ping_gap, timeout)) << "ping " << i;
  }

  const auto last = base_time() + (pings - 1) * ping_gap;
  EXPECT_FALSE(media.expired(last + 999ms, timeout));
  EXPECT_TRUE(media.expired(last + 1000ms, timeout));
}

// ---------------------------------------------------------------------------
// S1a registry: shared_media_liveness_t
// ---------------------------------------------------------------------------

/**
 * @brief Shared tracker is not armed and never expired before any ping (S1a registry).
 */
TEST(SharedMediaLivenessTest, NotArmedAndNeverExpiredBeforeAnyPing) {
  stream::liveness::shared_media_liveness_t media;
  EXPECT_FALSE(media.armed());
  EXPECT_FALSE(media.expired(base_time() + std::chrono::hours {24 * 365}, 1s));
  EXPECT_FALSE(media.expired(base_time(), 0ms));
}

/**
 * @brief Shared tracker is armed after the first ping (S1a registry).
 */
TEST(SharedMediaLivenessTest, ArmedAfterFirstPing) {
  stream::liveness::shared_media_liveness_t media;
  media.on_ping(base_time());
  EXPECT_TRUE(media.armed());
}

/**
 * @brief Shared tracker expires exactly at the timeout and not one millisecond before (S1a registry).
 */
TEST(SharedMediaLivenessTest, ExpiryBoundaryMatchesMediaLiveness) {
  stream::liveness::shared_media_liveness_t media;
  media.on_ping(base_time());
  EXPECT_FALSE(media.expired(base_time() + 4999ms, 5000ms));
  EXPECT_TRUE(media.expired(base_time() + 5000ms, 5000ms));
  EXPECT_TRUE(media.expired(base_time() + 5001ms, 5000ms));
}

/**
 * @brief A new ping on the shared tracker resets the clock (S1a registry).
 */
TEST(SharedMediaLivenessTest, NewPingResetsClock) {
  stream::liveness::shared_media_liveness_t media;
  media.on_ping(base_time());
  media.on_ping(base_time() + 4s);
  EXPECT_FALSE(media.expired(base_time() + 8s, 5s));
  EXPECT_TRUE(media.expired(base_time() + 9s, 5s));
}

/**
 * @brief Several threads pinging while another thread polls expired() does not crash, and the final state is armed
 *        and not expired relative to the last ping time (S1a registry, thread safety).
 */
TEST(SharedMediaLivenessTest, ConcurrentPingsAndExpiredAreSafe) {
  stream::liveness::shared_media_liveness_t media;
  constexpr int writers = 4;
  constexpr int pings_per_writer = 2000;

  std::atomic<bool> polling {true};
  std::vector<std::thread> threads;

  threads.emplace_back([&media, &polling] {
    while (polling.load()) {
      (void) media.expired(base_time() + 1ms, 1s);
      (void) media.armed();
    }
  });

  for (int w = 0; w < writers; ++w) {
    threads.emplace_back([&media] {
      for (int i = 0; i < pings_per_writer; ++i) {
        media.on_ping(base_time() + std::chrono::milliseconds {i});
      }
    });
  }

  for (std::size_t t = 1; t < threads.size(); ++t) {
    threads[t].join();
  }
  polling.store(false);
  threads[0].join();

  EXPECT_TRUE(media.armed());
  // Every recorded ping lies within [base, base + pings_per_writer ms), so at base + pings_per_writer ms the last ping
  // is less than one hour old, and one hour after that point it has expired.
  const auto newest = base_time() + std::chrono::milliseconds {pings_per_writer};
  EXPECT_FALSE(media.expired(newest, 1h));
  EXPECT_TRUE(media.expired(newest + 1h, 1h));
}

// ---------------------------------------------------------------------------
// S1a registry: media_ping_registry_t
// ---------------------------------------------------------------------------

/**
 * @brief An empty registry has size 0 and an unknown key is not routed (S1a registry).
 */
TEST(MediaPingRegistryTest, EmptyRegistryHasNoKeys) {
  stream::liveness::media_ping_registry_t registry;
  EXPECT_EQ(registry.size(), 0u);
  EXPECT_FALSE(registry.on_ping("unknown", base_time()));
}

/**
 * @brief add() routes a ping to the tracker: on_ping returns true and arms the tracker (S1a registry).
 */
TEST(MediaPingRegistryTest, AddThenOnPingArmsTracker) {
  stream::liveness::media_ping_registry_t registry;
  auto tracker = std::make_shared<stream::liveness::shared_media_liveness_t>();

  registry.add("key-a", tracker);
  EXPECT_EQ(registry.size(), 1u);
  EXPECT_FALSE(tracker->armed());

  EXPECT_TRUE(registry.on_ping("key-a", base_time()));
  EXPECT_TRUE(tracker->armed());
  EXPECT_TRUE(tracker->expired(base_time() + 5s, 5s));
  EXPECT_FALSE(tracker->expired(base_time() + 4s, 5s));
}

/**
 * @brief on_ping with a key that was never added returns false and feeds no tracker (S1a registry).
 */
TEST(MediaPingRegistryTest, UnknownKeyReturnsFalseAndFeedsNothing) {
  stream::liveness::media_ping_registry_t registry;
  auto tracker = std::make_shared<stream::liveness::shared_media_liveness_t>();
  registry.add("key-a", tracker);

  EXPECT_FALSE(registry.on_ping("key-b", base_time()));
  EXPECT_FALSE(tracker->armed());
  EXPECT_EQ(registry.size(), 1u);
}

/**
 * @brief add() with a null tracker is ignored: the size does not change (S1a registry).
 */
TEST(MediaPingRegistryTest, AddNullTrackerIsIgnored) {
  stream::liveness::media_ping_registry_t registry;
  registry.add("key-a", nullptr);
  EXPECT_EQ(registry.size(), 0u);
  EXPECT_FALSE(registry.on_ping("key-a", base_time()));
}

/**
 * @brief add() with a null tracker does not remove or replace an existing entry for the same key (S1a registry).
 *
 * The spec only says a null tracker is ignored, so an existing route is kept.
 */
TEST(MediaPingRegistryTest, AddNullTrackerKeepsExistingRoute) {
  stream::liveness::media_ping_registry_t registry;
  auto tracker = std::make_shared<stream::liveness::shared_media_liveness_t>();
  registry.add("key-a", tracker);

  registry.add("key-a", nullptr);
  EXPECT_EQ(registry.size(), 1u);
  EXPECT_TRUE(registry.on_ping("key-a", base_time()));
  EXPECT_TRUE(tracker->armed());
}

/**
 * @brief Adding an existing key replaces its tracker: pings go to the new one and the old one is not fed (S1a registry).
 */
TEST(MediaPingRegistryTest, AddExistingKeyReplacesTracker) {
  stream::liveness::media_ping_registry_t registry;
  auto old_tracker = std::make_shared<stream::liveness::shared_media_liveness_t>();
  auto new_tracker = std::make_shared<stream::liveness::shared_media_liveness_t>();

  registry.add("key-a", old_tracker);
  registry.add("key-a", new_tracker);
  EXPECT_EQ(registry.size(), 1u);

  EXPECT_TRUE(registry.on_ping("key-a", base_time()));
  EXPECT_TRUE(new_tracker->armed());
  EXPECT_FALSE(old_tracker->armed());
}

/**
 * @brief remove() removes every key routed to the tracker and leaves keys of other trackers alone (S1a registry).
 */
TEST(MediaPingRegistryTest, RemoveDropsOnlyThatTrackersKeys) {
  stream::liveness::media_ping_registry_t registry;
  auto tracker_a = std::make_shared<stream::liveness::shared_media_liveness_t>();
  auto tracker_b = std::make_shared<stream::liveness::shared_media_liveness_t>();

  registry.add("a-1", tracker_a);
  registry.add("a-2", tracker_a);
  registry.add("b-1", tracker_b);
  EXPECT_EQ(registry.size(), 3u);

  registry.remove(tracker_a);
  EXPECT_EQ(registry.size(), 1u);
  EXPECT_FALSE(registry.on_ping("a-1", base_time()));
  EXPECT_FALSE(registry.on_ping("a-2", base_time()));
  EXPECT_FALSE(tracker_a->armed());

  EXPECT_TRUE(registry.on_ping("b-1", base_time()));
  EXPECT_TRUE(tracker_b->armed());
}

/**
 * @brief remove() of a tracker that has no keys in the registry changes nothing (S1a registry).
 */
TEST(MediaPingRegistryTest, RemoveUnregisteredTrackerIsNoOp) {
  stream::liveness::media_ping_registry_t registry;
  auto registered = std::make_shared<stream::liveness::shared_media_liveness_t>();
  auto stranger = std::make_shared<stream::liveness::shared_media_liveness_t>();
  registry.add("key-a", registered);

  registry.remove(stranger);
  EXPECT_EQ(registry.size(), 1u);
  EXPECT_TRUE(registry.on_ping("key-a", base_time()));
}

/**
 * @brief A tracker whose key was taken over by a newer tracker owns no keys, so removing it keeps the newer route (S1a registry).
 */
TEST(MediaPingRegistryTest, RemoveReplacedTrackerKeepsNewerRoute) {
  stream::liveness::media_ping_registry_t registry;
  auto old_tracker = std::make_shared<stream::liveness::shared_media_liveness_t>();
  auto new_tracker = std::make_shared<stream::liveness::shared_media_liveness_t>();
  registry.add("key-a", old_tracker);
  registry.add("key-a", new_tracker);

  registry.remove(old_tracker);
  EXPECT_EQ(registry.size(), 1u);
  EXPECT_TRUE(registry.on_ping("key-a", base_time()));
  EXPECT_TRUE(new_tracker->armed());
}

/**
 * @brief size() counts keys, not trackers: several keys on one tracker count separately (S1a registry).
 */
TEST(MediaPingRegistryTest, SizeCountsKeys) {
  stream::liveness::media_ping_registry_t registry;
  auto tracker = std::make_shared<stream::liveness::shared_media_liveness_t>();
  registry.add("k1", tracker);
  registry.add("k2", tracker);
  registry.add("k3", tracker);
  EXPECT_EQ(registry.size(), 3u);
}

/**
 * @brief Concurrent add/remove of different keys while other threads ping and read size() is safe, and each
 *        thread's keys route correctly while they are registered (S1a registry, thread safety).
 */
TEST(MediaPingRegistryTest, ConcurrentAddRemoveAndPingAreSafe) {
  stream::liveness::media_ping_registry_t registry;
  constexpr int adders = 4;
  constexpr int rounds = 50;
  constexpr int keys_per_round = 16;

  std::atomic<int> routing_misses {0};
  std::atomic<bool> pinging {true};
  std::vector<std::thread> threads;

  for (int w = 0; w < adders; ++w) {
    threads.emplace_back([&registry, &routing_misses, w] {
      for (int r = 0; r < rounds; ++r) {
        auto tracker = std::make_shared<stream::liveness::shared_media_liveness_t>();
        std::vector<std::string> keys;
        for (int k = 0; k < keys_per_round; ++k) {
          keys.push_back("t" + std::to_string(w) + "-k" + std::to_string(k));
          registry.add(keys.back(), tracker);
        }
        // Only this thread touches these keys, so they must still route to this tracker.
        for (const auto &key : keys) {
          if (!registry.on_ping(key, base_time())) {
            ++routing_misses;
          }
        }
        registry.remove(tracker);
      }
    });
  }

  for (int p = 0; p < 2; ++p) {
    threads.emplace_back([&registry, &pinging] {
      int i = 0;
      while (pinging.load()) {
        const auto key = "t" + std::to_string(i % adders) + "-k" + std::to_string(i % keys_per_round);
        (void) registry.on_ping(key, base_time());
        ++i;
      }
    });
  }

  threads.emplace_back([&registry, &pinging] {
    while (pinging.load()) {
      (void) registry.size();
    }
  });

  for (int t = 0; t < adders; ++t) {
    threads[static_cast<std::size_t>(t)].join();
  }
  pinging.store(false);
  for (std::size_t t = static_cast<std::size_t>(adders); t < threads.size(); ++t) {
    threads[t].join();
  }

  EXPECT_EQ(routing_misses.load(), 0);
  EXPECT_EQ(registry.size(), 0u);
}

// ---------------------------------------------------------------------------
// S1b: configure_peer / tick with a null peer
// ---------------------------------------------------------------------------

/**
 * @brief configure_peer with a null peer does nothing and does not crash (S1b).
 */
TEST(EnetLivenessNullPeerTest, ConfigurePeerNullPeerIsNoOp) {
  EXPECT_NO_FATAL_FAILURE(stream::liveness::configure_peer(nullptr, 2s));
  EXPECT_NO_FATAL_FAILURE(stream::liveness::configure_peer(nullptr, 0ms));
}

/**
 * @brief tick with a null peer returns false and leaves last_ping unchanged (S1b).
 */
TEST(EnetLivenessNullPeerTest, TickNullPeerReturnsFalseAndKeepsLastPing) {
  auto last = base_time();
  EXPECT_FALSE(stream::liveness::tick(nullptr, base_time() + 10s, last, 1s));
  EXPECT_EQ(last, base_time());
}

/**
 * @brief tick with a null peer keeps a default (never) last_ping as default (S1b).
 */
TEST(EnetLivenessNullPeerTest, TickNullPeerKeepsDefaultLastPing) {
  clock_t_::time_point last {};
  EXPECT_FALSE(stream::liveness::tick(nullptr, base_time(), last, 1s));
  EXPECT_EQ(last, clock_t_::time_point {});
}

// ---------------------------------------------------------------------------
// S1b: real loopback ENet pair
// ---------------------------------------------------------------------------

namespace {
  /**
   * @brief Owns a loopback server host and client host and tears them down.
   */
  class EnetLivenessTest: public ::testing::Test {
  protected:
    void SetUp() override {
      ASSERT_TRUE(enet_ready());
    }

    void TearDown() override {
      if (client_ != nullptr) {
        enet_host_destroy(client_);
        client_ = nullptr;
      }
      if (server_ != nullptr) {
        enet_host_destroy(server_);
        server_ = nullptr;
      }
    }

    /**
     * @brief Create a server bound to an ephemeral loopback port and a client, then connect them.
     * @return `true` when both sides saw CONNECT.
     */
    bool connect_pair() {
      // moonlight-common-c's ENet fork: sockaddr-based ENetAddress and an address-family argument
      ENetAddress server_addr {};
      if (enet_address_set_host(&server_addr, "127.0.0.1") != 0 || enet_address_set_port(&server_addr, 0) != 0) {
        return false;
      }
      server_ = enet_host_create(AF_INET, &server_addr, 8, 2, 0, 0);
      if (server_ == nullptr) {
        return false;
      }

      // enet_host_create() stores the bound address (getsockname) in host->address
      const auto *bound = reinterpret_cast<const sockaddr_in *>(&server_->address.address);
      const std::uint16_t port = ntohs(bound->sin_port);
      if (port == 0) {
        return false;
      }

      client_ = enet_host_create(AF_INET, nullptr, 1, 2, 0, 0);
      if (client_ == nullptr) {
        return false;
      }

      ENetAddress target {};
      if (enet_address_set_host(&target, "127.0.0.1") != 0 || enet_address_set_port(&target, port) != 0) {
        return false;
      }
      client_peer_ = enet_host_connect(client_, &target, 2, 0);
      if (client_peer_ == nullptr) {
        return false;
      }

      const auto deadline = clock_t_::now() + 5s;
      while (clock_t_::now() < deadline && (server_peer_ == nullptr || !client_connected_)) {
        ENetEvent ev {};
        if (enet_host_service(server_, &ev, 10) > 0 && ev.type == ENET_EVENT_TYPE_CONNECT && server_peer_ == nullptr) {
          server_peer_ = ev.peer;
        }
        if (enet_host_service(client_, &ev, 10) > 0 && ev.type == ENET_EVENT_TYPE_CONNECT) {
          client_connected_ = true;
        }
      }
      return server_peer_ != nullptr && client_connected_;
    }

    ENetHost *server_ {nullptr};
    ENetHost *client_ {nullptr};
    ENetPeer *server_peer_ {nullptr};
    ENetPeer *client_peer_ {nullptr};
    bool client_connected_ {false};
  };
}  // namespace

/**
 * @brief tick on a connected peer sends a ping the first time and records last_ping (S1b).
 */
TEST_F(EnetLivenessTest, TickFirstCallSendsPingAndSetsLastPing) {
  ASSERT_TRUE(connect_pair());
  clock_t_::time_point last {};
  EXPECT_TRUE(stream::liveness::tick(server_peer_, base_time(), last, 1s));
  EXPECT_EQ(last, base_time());
}

/**
 * @brief tick before the interval has elapsed returns false and leaves last_ping unchanged (S1b).
 */
TEST_F(EnetLivenessTest, TickBeforeIntervalReturnsFalse) {
  ASSERT_TRUE(connect_pair());
  clock_t_::time_point last {};
  ASSERT_TRUE(stream::liveness::tick(server_peer_, base_time(), last, 1s));
  EXPECT_FALSE(stream::liveness::tick(server_peer_, base_time() + 999ms, last, 1s));
  EXPECT_EQ(last, base_time());
}

/**
 * @brief tick exactly at the interval returns true and updates last_ping (S1b).
 */
TEST_F(EnetLivenessTest, TickAtIntervalReturnsTrue) {
  ASSERT_TRUE(connect_pair());
  clock_t_::time_point last {};
  ASSERT_TRUE(stream::liveness::tick(server_peer_, base_time(), last, 1s));
  EXPECT_TRUE(stream::liveness::tick(server_peer_, base_time() + 1s, last, 1s));
  EXPECT_EQ(last, base_time() + 1s);
}

/**
 * @brief tick after the interval returns true and updates last_ping (S1b).
 */
TEST_F(EnetLivenessTest, TickAfterIntervalReturnsTrue) {
  ASSERT_TRUE(connect_pair());
  clock_t_::time_point last {};
  ASSERT_TRUE(stream::liveness::tick(server_peer_, base_time(), last, 1s));
  EXPECT_TRUE(stream::liveness::tick(server_peer_, base_time() + 3s, last, 1s));
  EXPECT_EQ(last, base_time() + 3s);
}

/**
 * @brief tick on a peer that is no longer connected returns false and keeps last_ping (S1b).
 */
TEST_F(EnetLivenessTest, TickDisconnectedPeerReturnsFalse) {
  ASSERT_TRUE(connect_pair());
  enet_peer_disconnect_now(client_peer_, 0);
  client_peer_ = nullptr;

  // Service the server until it has seen the DISCONNECT, so the server peer is no longer connected.
  bool saw_disconnect = false;
  const auto deadline = clock_t_::now() + 2s;
  while (!saw_disconnect && clock_t_::now() < deadline) {
    ENetEvent ev {};
    while (enet_host_service(server_, &ev, 10) > 0) {
      if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
        enet_packet_destroy(ev.packet);
      } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT && ev.peer == server_peer_) {
        saw_disconnect = true;
      }
    }
  }
  ASSERT_TRUE(saw_disconnect);

  clock_t_::time_point last = base_time();
  EXPECT_FALSE(stream::liveness::tick(server_peer_, base_time() + 10s, last, 1s));
  EXPECT_EQ(last, base_time());
}

/**
 * @brief tick on a peer slot in a fresh host (never connected) returns false (S1b).
 */
TEST_F(EnetLivenessTest, TickUnconnectedPeerSlotReturnsFalse) {
  ENetAddress addr {};
  ASSERT_EQ(enet_address_set_host(&addr, "127.0.0.1"), 0);
  ASSERT_EQ(enet_address_set_port(&addr, 0), 0);
  server_ = enet_host_create(AF_INET, &addr, 1, 1, 0, 0);
  ASSERT_NE(server_, nullptr);
  ASSERT_NE(server_->peers, nullptr);

  clock_t_::time_point last {};
  EXPECT_FALSE(stream::liveness::tick(&server_->peers[0], base_time(), last, 1s));
  EXPECT_EQ(last, clock_t_::time_point {});
}

/**
 * @brief configure_peer with a timeout below 1 ms leaves the peer's ENet timeout fields unchanged (S1b).
 */
TEST_F(EnetLivenessTest, ConfigurePeerBelowOneMsLeavesTimeoutUnchanged) {
  ASSERT_TRUE(connect_pair());
  const auto limit = server_peer_->timeoutLimit;
  const auto minimum = server_peer_->timeoutMinimum;
  const auto maximum = server_peer_->timeoutMaximum;

  stream::liveness::configure_peer(server_peer_, 0ms);

  EXPECT_EQ(server_peer_->timeoutLimit, limit);
  EXPECT_EQ(server_peer_->timeoutMinimum, minimum);
  EXPECT_EQ(server_peer_->timeoutMaximum, maximum);
}

/**
 * @brief A client that stops servicing its ENet host (never ACKs) but keeps sending reliable packets is disconnected
 *        by the server within timeout + 1.5 s, while the server still receives the client's packets (S1b).
 */
TEST_F(EnetLivenessTest, HalfDeadClientIsDisconnectedWithinTimeout) {
  ASSERT_TRUE(connect_pair());
  constexpr auto timeout = 2000ms;
  stream::liveness::configure_peer(server_peer_, timeout);

  clock_t_::time_point last_ping {};
  const auto silence_start = clock_t_::now();
  const auto deadline = silence_start + timeout + 1500ms;
  auto next_send = silence_start;

  bool disconnected = false;
  int received = 0;
  while (!disconnected && clock_t_::now() < deadline) {
    if (clock_t_::now() >= next_send) {
      // The client does not service its host, so it never ACKs. It still sends and flushes.
      ENetPacket *packet = enet_packet_create("client-data", 11, ENET_PACKET_FLAG_RELIABLE);
      if (enet_peer_send(client_peer_, 0, packet) == 0) {
        enet_host_flush(client_);
      }
      next_send += 100ms;
    }

    ENetEvent ev {};
    int rc = enet_host_service(server_, &ev, 10);
    while (rc > 0 && !disconnected) {
      if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
        ++received;
        enet_packet_destroy(ev.packet);
      } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT && ev.peer == server_peer_) {
        disconnected = true;
      }
      rc = enet_host_service(server_, &ev, 0);
    }

    stream::liveness::tick(server_peer_, clock_t_::now(), last_ping);
  }

  EXPECT_TRUE(disconnected) << "server never disconnected the silent client";
  EXPECT_GT(received, 0) << "server should still have received the client's packets while it was silent";
}

/**
 * @brief A client that services its host normally, with the same server setup, is not disconnected over about 4 s (S1b control).
 */
TEST_F(EnetLivenessTest, HealthyClientIsNotDisconnected) {
  ASSERT_TRUE(connect_pair());
  constexpr auto timeout = 2000ms;
  stream::liveness::configure_peer(server_peer_, timeout);

  clock_t_::time_point last_ping {};
  const auto start = clock_t_::now();
  const auto end = start + 4s;
  auto next_send = start;

  bool server_disconnected = false;
  bool client_disconnected = false;
  while (clock_t_::now() < end) {
    if (clock_t_::now() >= next_send) {
      ENetPacket *packet = enet_packet_create("client-data", 11, ENET_PACKET_FLAG_RELIABLE);
      if (enet_peer_send(client_peer_, 0, packet) == 0) {
        enet_host_flush(client_);
      }
      next_send += 100ms;
    }

    ENetEvent cev {};
    while (enet_host_service(client_, &cev, 0) > 0) {
      if (cev.type == ENET_EVENT_TYPE_RECEIVE) {
        enet_packet_destroy(cev.packet);
      } else if (cev.type == ENET_EVENT_TYPE_DISCONNECT) {
        client_disconnected = true;
      }
    }

    ENetEvent sev {};
    int rc = enet_host_service(server_, &sev, 10);
    while (rc > 0) {
      if (sev.type == ENET_EVENT_TYPE_RECEIVE) {
        enet_packet_destroy(sev.packet);
      } else if (sev.type == ENET_EVENT_TYPE_DISCONNECT && sev.peer == server_peer_) {
        server_disconnected = true;
      }
      rc = enet_host_service(server_, &sev, 0);
    }

    stream::liveness::tick(server_peer_, clock_t_::now(), last_ping);
  }

  EXPECT_FALSE(server_disconnected);
  EXPECT_FALSE(client_disconnected);
  EXPECT_EQ(server_peer_->state, ENET_PEER_STATE_CONNECTED);
}
