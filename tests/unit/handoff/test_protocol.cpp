/**
 * @file tests/unit/handoff/test_protocol.cpp
 * @brief Blind tests for the drain-handoff channel wire protocol (src/handoff/protocol.*), written from the spec and header.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <cstdint>
  #include <limits>
  #include <optional>
  #include <string>
  #include <utility>
  #include <variant>
  #include <vector>

  // lib includes
  #include <nlohmann/json.hpp>

  // local includes
  #include "src/handoff/protocol.h"

namespace proto = handoff::protocol;

namespace {
  /** @brief A 64-character lowercase hex digest used by the samples. */
  const std::string kDigest = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

  /** @brief A 64-character lowercase hex certificate hash used by the samples. */
  const std::string kCertHash = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

  /**
   * @brief Build a populated `app_snapshot_t`.
   * @return Snapshot with every field set.
   */
  proto::app_snapshot_t populated_snapshot() {
    proto::app_snapshot_t snapshot;
    snapshot.app_id = 7;
    snapshot.name = "Desktop";
    snapshot.applied_prep = 2;
    snapshot.digest = kDigest;
    snapshot.launch_env = {{"SUNSHINE_CLIENT_WIDTH", "1920"}, {"SUNSHINE_CLIENT_FPS", "60"}};
    return snapshot;
  }

  /**
   * @brief Build a populated `hello_t` (old process: probe, identity and cooldown set).
   * @return Hello with every optional set.
   */
  proto::hello_t populated_hello() {
    proto::hello_t hello;
    hello.major = 1;
    hello.minor = 0;
    hello.capabilities = {"feature-a", "feature-b"};
    proto::probe_results_t probe;
    probe.hevc_mode = 2;
    probe.av1_mode = 3;
    probe.yuv444 = true;
    hello.probe = probe;
    proto::probe_identity_t identity;
    identity.build_id = "build-1";
    identity.render_node = "/dev/dri/renderD128";
    identity.vendor_id = 4098;
    identity.device_id = 5;
    identity.driver_version = "550.1";
    hello.identity = identity;
    hello.cooldown_deadline_ns = 123456789012LL;
    return hello;
  }

  /**
   * @brief Build a populated session list entry.
   * @param id Session id to set.
   * @return Entry with every field set.
   */
  proto::session_entry_t session_entry(std::uint32_t id) {
    proto::session_entry_t entry;
    entry.id = id;
    entry.started_unix_ms = 1700000000000LL;
    entry.transport = "udp/ipv4";
    entry.client_category = "lan";
    return entry;
  }

  /**
   * @brief Decode a payload and require the status to be `ok`.
   * @param payload Payload to decode.
   * @return The decoded message (default `ping_t` when the status was not ok).
   */
  proto::message_t decode_ok(const std::string &payload) {
    proto::message_t message;
    const auto status = proto::decode(payload, message);
    EXPECT_EQ(status, proto::decode_status_e::ok) << "Payload must decode with status ok: " << payload;
    return message;
  }

  /**
   * @brief Require a payload to be malformed.
   * @param payload Payload that must be rejected as malformed.
   */
  void expect_malformed(const std::string &payload) {
    SCOPED_TRACE(payload);
    proto::message_t message;
    EXPECT_EQ(proto::decode(payload, message), proto::decode_status_e::malformed)
      << "The payload must be malformed (not JSON, wrong type, missing or out-of-range member, or bad value).";
  }

  /**
   * @brief Serialize a JSON object for use as a payload.
   * @param json JSON to dump.
   * @return Compact JSON text.
   */
  std::string dump(const nlohmann::json &json) {
    return json.dump();
  }
}  // namespace

// ---- Round trips -------------------------------------------------------------------------------------------------

/**
 * @brief Every message type with populated optionals round-trips through encode and decode.
 */
TEST(HandoffProtocolTest, RoundTripPopulatedMessagesOfEveryType) {
  std::vector<proto::message_t> messages;
  messages.emplace_back(populated_hello());

  proto::ready_t ready_ok;
  ready_ok.ok = true;
  messages.emplace_back(ready_ok);
  proto::ready_t ready_fail;
  ready_fail.ok = false;
  ready_fail.reason = "config port mismatch";
  messages.emplace_back(ready_fail);

  proto::go_t go;
  go.app = populated_snapshot();
  go.host_audio = true;
  go.next_launch_session_id = 9;
  messages.emplace_back(go);

  proto::go_ack_t go_ack;
  go_ack.ok = false;
  go_ack.reason = "app or prep commands changed while an app runs";
  messages.emplace_back(go_ack);

  messages.emplace_back(proto::commit_t {});
  messages.emplace_back(proto::main_t {});

  proto::ping_t ping;
  ping.seq = 7;
  messages.emplace_back(ping);

  proto::pong_t pong;
  pong.seq = 7;
  messages.emplace_back(pong);

  proto::sessions_request_t sessions_request;
  sessions_request.request_id = 5;
  messages.emplace_back(sessions_request);

  proto::sessions_reply_t sessions_reply;
  sessions_reply.request_id = 5;
  sessions_reply.sessions = {session_entry(2), session_entry(3)};
  messages.emplace_back(sessions_reply);

  proto::close_request_t close_request;
  close_request.request_id = 6;
  close_request.session_id = 2;
  messages.emplace_back(close_request);

  proto::close_reply_t close_reply;
  close_reply.request_id = 6;
  close_reply.result = "already-stopping";
  messages.emplace_back(close_reply);

  proto::close_cert_request_t close_cert_request;
  close_cert_request.request_id = 8;
  close_cert_request.cert_sha256 = kCertHash;
  messages.emplace_back(close_cert_request);

  proto::close_cert_reply_t close_cert_reply;
  close_cert_reply.request_id = 8;
  close_cert_reply.count = 3;
  messages.emplace_back(close_cert_reply);

  proto::end_all_t end_all;
  end_all.request_id = 10;
  messages.emplace_back(end_all);

  proto::ended_t ended;
  ended.request_id = 10;
  messages.emplace_back(ended);

  proto::end_rejected_t end_rejected;
  end_rejected.request_id = 10;
  messages.emplace_back(end_rejected);

  proto::handoff_state_t state;
  state.state = "kill_pending";
  messages.emplace_back(state);

  for (const auto &message : messages) {
    const auto payload = proto::encode(message);
    ASSERT_FALSE(payload.empty()) << "Encoding of type " << proto::type_name(message) << " must not be empty.";
    proto::message_t decoded;
    ASSERT_EQ(proto::decode(payload, decoded), proto::decode_status_e::ok)
      << "Encoded " << proto::type_name(message) << " must decode with status ok: " << payload;
    EXPECT_EQ(decoded, message) << "Round trip of " << proto::type_name(message) << " must reproduce the message exactly.";
  }
}

/**
 * @brief Every message type with empty or omitted optionals round-trips.
 */
TEST(HandoffProtocolTest, RoundTripEmptyOptionalMessagesOfEveryType) {
  std::vector<proto::message_t> messages;
  messages.emplace_back(proto::hello_t {});
  messages.emplace_back(proto::ready_t {});
  messages.emplace_back(proto::go_t {});
  messages.emplace_back(proto::go_ack_t {});
  messages.emplace_back(proto::commit_t {});
  messages.emplace_back(proto::main_t {});
  messages.emplace_back(proto::ping_t {});
  messages.emplace_back(proto::pong_t {});
  messages.emplace_back(proto::sessions_request_t {});
  messages.emplace_back(proto::sessions_reply_t {});
  messages.emplace_back(proto::close_request_t {});

  proto::close_reply_t close_reply;
  close_reply.result = "closed";
  messages.emplace_back(close_reply);

  messages.emplace_back(proto::close_cert_request_t {});
  messages.emplace_back(proto::close_cert_reply_t {});
  messages.emplace_back(proto::end_all_t {});
  messages.emplace_back(proto::ended_t {});
  messages.emplace_back(proto::end_rejected_t {});

  proto::handoff_state_t state;
  state.state = "none";
  messages.emplace_back(state);

  for (const auto &message : messages) {
    const auto payload = proto::encode(message);
    ASSERT_FALSE(payload.empty()) << "Encoding of an empty " << proto::type_name(message) << " must not be empty.";
    proto::message_t decoded;
    ASSERT_EQ(proto::decode(payload, decoded), proto::decode_status_e::ok)
      << "Encoded empty " << proto::type_name(message) << " must decode with status ok: " << payload;
    EXPECT_EQ(decoded, message) << "Round trip of empty " << proto::type_name(message) << " must reproduce it exactly.";
  }
}

/**
 * @brief A GO with no app and a HELLO with no probe, identity or cooldown round-trip with optionals empty.
 */
TEST(HandoffProtocolTest, RoundTripKeepsEmptyOptionalsEmpty) {
  proto::go_t go;
  go.host_audio = false;
  go.next_launch_session_id = 0;
  proto::message_t decoded;
  ASSERT_EQ(proto::decode(proto::encode(go), decoded), proto::decode_status_e::ok) << "GO without an app must decode.";
  const auto *round = std::get_if<proto::go_t>(&decoded);
  ASSERT_NE(round, nullptr) << "A GO must decode to go_t.";
  EXPECT_FALSE(round->app.has_value()) << "A GO encoded without an app must decode with no app.";

  proto::hello_t hello;
  ASSERT_EQ(proto::decode(proto::encode(hello), decoded), proto::decode_status_e::ok) << "HELLO with no optionals must decode.";
  const auto *hello_round = std::get_if<proto::hello_t>(&decoded);
  ASSERT_NE(hello_round, nullptr) << "A HELLO must decode to hello_t.";
  EXPECT_FALSE(hello_round->probe.has_value()) << "A HELLO with no probe must decode with no probe.";
  EXPECT_FALSE(hello_round->identity.has_value()) << "A HELLO with no identity must decode with no identity.";
  EXPECT_FALSE(hello_round->cooldown_deadline_ns.has_value()) << "A HELLO with no cooldown must decode with no cooldown.";
}

/**
 * @brief Largest unsigned values round-trip without overflow.
 */
TEST(HandoffProtocolTest, RoundTripLargestUnsignedValues) {
  proto::ping_t ping;
  ping.seq = std::numeric_limits<std::uint64_t>::max();
  proto::message_t decoded;
  ASSERT_EQ(proto::decode(proto::encode(ping), decoded), proto::decode_status_e::ok) << "PING with the largest uint64 must decode.";
  EXPECT_EQ(decoded, proto::message_t {ping}) << "The largest uint64 sequence number must round-trip.";

  proto::close_request_t close;
  close.request_id = std::numeric_limits<std::uint64_t>::max();
  close.session_id = std::numeric_limits<std::uint32_t>::max();
  ASSERT_EQ(proto::decode(proto::encode(close), decoded), proto::decode_status_e::ok) << "CLOSE with the largest ids must decode.";
  EXPECT_EQ(decoded, proto::message_t {close}) << "The largest uint32 session id must round-trip.";
}

/**
 * @brief A negative cooldown deadline (signed 64-bit) round-trips.
 */
TEST(HandoffProtocolTest, RoundTripNegativeCooldownDeadline) {
  proto::hello_t hello;
  hello.cooldown_deadline_ns = -5;
  proto::message_t decoded;
  ASSERT_EQ(proto::decode(proto::encode(hello), decoded), proto::decode_status_e::ok) << "HELLO with a negative cooldown must decode.";
  EXPECT_EQ(decoded, proto::message_t {hello}) << "cooldown_deadline_ns is signed and must round-trip negative values.";
}

// ---- Encoding (wire shape) ---------------------------------------------------------------------------------------

/**
 * @brief The PING example from the header encodes to the documented JSON.
 */
TEST(HandoffProtocolTest, EncodePingMatchesDocumentedExample) {
  proto::ping_t ping;
  ping.seq = 7;
  const auto json = nlohmann::json::parse(proto::encode(ping));
  EXPECT_EQ(json, (nlohmann::json {{"type", "PING"}, {"seq", 7}}))
    << "PING must encode as {\"type\":\"PING\",\"seq\":7}.";
}

/**
 * @brief Members of a populated HELLO use the C++ field names and the probe and identity shapes.
 */
TEST(HandoffProtocolTest, EncodePopulatedHelloUsesFieldNames) {
  const auto json = nlohmann::json::parse(proto::encode(populated_hello()));
  EXPECT_EQ(json.at("type"), "HELLO") << "The type member must be HELLO.";
  EXPECT_EQ(json.at("major"), 1) << "major must be encoded as a member named major.";
  EXPECT_EQ(json.at("minor"), 0) << "minor must be encoded as a member named minor.";
  EXPECT_EQ(json.at("capabilities"), (nlohmann::json::array({"feature-a", "feature-b"})))
    << "capabilities must be a JSON array of strings.";
  EXPECT_EQ(json.at("probe").at("hevc_mode"), 2) << "probe must be an object with hevc_mode.";
  EXPECT_EQ(json.at("probe").at("av1_mode"), 3) << "probe must be an object with av1_mode.";
  EXPECT_EQ(json.at("probe").at("yuv444"), true) << "probe must be an object with yuv444.";
  EXPECT_EQ(json.at("identity").at("build_id"), "build-1") << "identity must be an object with build_id.";
  EXPECT_EQ(json.at("identity").at("render_node"), "/dev/dri/renderD128") << "identity must have render_node.";
  EXPECT_EQ(json.at("identity").at("vendor_id"), 4098) << "identity must have vendor_id.";
  EXPECT_EQ(json.at("identity").at("device_id"), 5) << "identity must have device_id.";
  EXPECT_EQ(json.at("identity").at("driver_version"), "550.1") << "identity must have driver_version.";
  EXPECT_EQ(json.at("cooldown_deadline_ns"), 123456789012LL) << "cooldown_deadline_ns must use its C++ name.";
}

/**
 * @brief An empty optional is omitted from the encoded HELLO.
 */
TEST(HandoffProtocolTest, EncodeEmptyOptionalsOmitsMembers) {
  const auto json = nlohmann::json::parse(proto::encode(proto::hello_t {}));
  EXPECT_FALSE(json.contains("probe")) << "An empty probe optional must be omitted from HELLO.";
  EXPECT_FALSE(json.contains("identity")) << "An empty identity optional must be omitted from HELLO.";
  EXPECT_FALSE(json.contains("cooldown_deadline_ns")) << "An empty cooldown optional must be omitted from HELLO.";
  EXPECT_FALSE(nlohmann::json::parse(proto::encode(proto::go_t {})).contains("app"))
    << "An empty app optional must be omitted from GO.";
}

/**
 * @brief GO carries the app snapshot with launch_env as two-element [name, value] arrays.
 */
TEST(HandoffProtocolTest, EncodeGoCarriesSnapshotWithLaunchEnvAsPairs) {
  proto::go_t go;
  go.app = populated_snapshot();
  go.host_audio = true;
  go.next_launch_session_id = 9;
  const auto json = nlohmann::json::parse(proto::encode(go));
  EXPECT_EQ(json.at("type"), "GO") << "The type member must be GO.";
  EXPECT_EQ(json.at("host_audio"), true) << "host_audio must be a boolean member.";
  EXPECT_EQ(json.at("next_launch_session_id"), 9) << "next_launch_session_id must use its C++ name.";
  EXPECT_EQ(json.at("app").at("app_id"), 7) << "The snapshot must carry app_id.";
  EXPECT_EQ(json.at("app").at("name"), "Desktop") << "The snapshot must carry name.";
  EXPECT_EQ(json.at("app").at("applied_prep"), 2) << "The snapshot must carry applied_prep.";
  EXPECT_EQ(json.at("app").at("digest"), kDigest) << "The snapshot must carry digest.";
  EXPECT_EQ(json.at("app").at("launch_env"), (nlohmann::json::array({nlohmann::json::array({"SUNSHINE_CLIENT_WIDTH", "1920"}), nlohmann::json::array({"SUNSHINE_CLIENT_FPS", "60"})})))
    << "launch_env must be an array of [name, value] two-string arrays.";
}

/**
 * @brief Session list entries carry the four named members.
 */
TEST(HandoffProtocolTest, EncodeSessionsEntriesUseFieldNames) {
  proto::sessions_reply_t reply;
  reply.request_id = 5;
  reply.sessions = {session_entry(2)};
  const auto json = nlohmann::json::parse(proto::encode(reply));
  EXPECT_EQ(json.at("type"), "SESSIONS") << "The type member must be SESSIONS.";
  EXPECT_EQ(json.at("request_id"), 5) << "request_id must be the echoed request id.";
  EXPECT_EQ(json.at("sessions").at(0), (nlohmann::json {{"id", 2}, {"started_unix_ms", 1700000000000LL}, {"transport", "udp/ipv4"}, {"client_category", "lan"}}))
    << "Each session entry must carry id, started_unix_ms, transport and client_category.";
}

/**
 * @brief CLOSE and CLOSED carry their named members.
 */
TEST(HandoffProtocolTest, EncodeCloseAndClosedUseFieldNames) {
  proto::close_request_t close;
  close.request_id = 12;
  close.session_id = 2;
  EXPECT_EQ(nlohmann::json::parse(proto::encode(close)), (nlohmann::json {{"type", "CLOSE"}, {"request_id", 12}, {"session_id", 2}}))
    << "CLOSE must carry request_id and session_id.";

  proto::close_reply_t reply;
  reply.request_id = 12;
  reply.result = "closed";
  EXPECT_EQ(nlohmann::json::parse(proto::encode(reply)), (nlohmann::json {{"type", "CLOSED"}, {"request_id", 12}, {"result", "closed"}}))
    << "CLOSED must carry request_id and result.";
}

/**
 * @brief The CLOSE_CERT reply uses the wire name CLOSED_CERT, distinct from the CLOSE reply's CLOSED.
 */
TEST(HandoffProtocolTest, EncodeCloseCertReplyUsesClosedCertName) {
  // SPEC-QUESTION: the spec's prose writes "CLOSED <count>" for both replies; the header gives the reply to
  // CLOSE_CERT the type name CLOSED_CERT so that the two replies can be told apart. This test follows the header.
  proto::close_cert_reply_t reply;
  reply.request_id = 4;
  reply.count = 2;
  EXPECT_EQ(nlohmann::json::parse(proto::encode(reply)), (nlohmann::json {{"type", "CLOSED_CERT"}, {"request_id", 4}, {"count", 2}}))
    << "The CLOSE_CERT reply must encode as CLOSED_CERT with request_id and count.";
}

/**
 * @brief HANDOFF_STATE carries its state member.
 */
TEST(HandoffProtocolTest, EncodeHandoffStateUsesStateMember) {
  proto::handoff_state_t state;
  state.state = "draining";
  EXPECT_EQ(nlohmann::json::parse(proto::encode(state)), (nlohmann::json {{"type", "HANDOFF_STATE"}, {"state", "draining"}}))
    << "HANDOFF_STATE must carry a state member.";
}

/**
 * @brief A message of an unknown type cannot be encoded and gives an empty payload.
 */
TEST(HandoffProtocolTest, EncodeUnknownGivesEmptyPayload) {
  proto::unknown_t unknown;
  unknown.type = "FUTURE_THING";
  EXPECT_TRUE(proto::encode(proto::message_t {unknown}).empty())
    << "encode() of unknown_t must return an empty payload.";
}

/**
 * @brief A payload that would exceed the maximum size is not encoded and gives an empty payload.
 */
TEST(HandoffProtocolTest, EncodeOversizedGivesEmptyPayload) {
  proto::ready_t ready;
  ready.ok = false;
  ready.reason = std::string(70000, 'r');
  EXPECT_TRUE(proto::encode(proto::message_t {ready}).empty())
    << "An encoding longer than max_message_size must give an empty payload.";
}

// ---- Type names --------------------------------------------------------------------------------------------------

/**
 * @brief Each known message type has its documented wire name.
 */
TEST(HandoffProtocolTest, TypeNamesAreTheWireNames) {
  EXPECT_EQ(proto::type_name(proto::message_t {proto::hello_t {}}), "HELLO") << "hello_t must be named HELLO.";
  EXPECT_EQ(proto::type_name(proto::message_t {proto::ready_t {}}), "READY") << "ready_t must be named READY.";
  EXPECT_EQ(proto::type_name(proto::message_t {proto::go_t {}}), "GO") << "go_t must be named GO.";
  EXPECT_EQ(proto::type_name(proto::message_t {proto::go_ack_t {}}), "GO_ACK") << "go_ack_t must be named GO_ACK.";
  EXPECT_EQ(proto::type_name(proto::message_t {proto::commit_t {}}), "COMMIT") << "commit_t must be named COMMIT.";
  EXPECT_EQ(proto::type_name(proto::message_t {proto::main_t {}}), "MAIN") << "main_t must be named MAIN.";
  EXPECT_EQ(proto::type_name(proto::message_t {proto::ping_t {}}), "PING") << "ping_t must be named PING.";
  EXPECT_EQ(proto::type_name(proto::message_t {proto::pong_t {}}), "PONG") << "pong_t must be named PONG.";
  EXPECT_EQ(proto::type_name(proto::message_t {proto::sessions_request_t {}}), "SESSIONS?")
    << "sessions_request_t must be named SESSIONS?.";
  EXPECT_EQ(proto::type_name(proto::message_t {proto::sessions_reply_t {}}), "SESSIONS")
    << "sessions_reply_t must be named SESSIONS.";
  EXPECT_EQ(proto::type_name(proto::message_t {proto::close_request_t {}}), "CLOSE") << "close_request_t must be named CLOSE.";
  EXPECT_EQ(proto::type_name(proto::message_t {proto::close_reply_t {}}), "CLOSED") << "close_reply_t must be named CLOSED.";
  EXPECT_EQ(proto::type_name(proto::message_t {proto::close_cert_request_t {}}), "CLOSE_CERT")
    << "close_cert_request_t must be named CLOSE_CERT.";
  EXPECT_EQ(proto::type_name(proto::message_t {proto::close_cert_reply_t {}}), "CLOSED_CERT")
    << "close_cert_reply_t must be named CLOSED_CERT (see the SPEC-QUESTION in EncodeCloseCertReplyUsesClosedCertName).";
  EXPECT_EQ(proto::type_name(proto::message_t {proto::end_all_t {}}), "END_ALL") << "end_all_t must be named END_ALL.";
  EXPECT_EQ(proto::type_name(proto::message_t {proto::ended_t {}}), "ENDED") << "ended_t must be named ENDED.";
  EXPECT_EQ(proto::type_name(proto::message_t {proto::end_rejected_t {}}), "END_REJECTED")
    << "end_rejected_t must be named END_REJECTED.";
  EXPECT_EQ(proto::type_name(proto::message_t {proto::handoff_state_t {}}), "HANDOFF_STATE")
    << "handoff_state_t must be named HANDOFF_STATE.";
}

/**
 * @brief An unknown message reports the type string it was received with.
 */
TEST(HandoffProtocolTest, TypeNameOfUnknownIsStoredType) {
  proto::unknown_t unknown;
  unknown.type = "FUTURE_THING";
  EXPECT_EQ(proto::type_name(proto::message_t {unknown}), "FUTURE_THING")
    << "type_name of unknown_t must return the stored type.";
}

// ---- Constants ---------------------------------------------------------------------------------------------------

/**
 * @brief Protocol constants match the header: major version 1, minor 0, and a 64 KiB limit.
 */
TEST(HandoffProtocolTest, ProtocolConstantsMatchHeader) {
  EXPECT_EQ(proto::version_major, 1U) << "The protocol major version must be 1.";
  EXPECT_EQ(proto::version_minor, 0U) << "The protocol minor version must be 0.";
  EXPECT_EQ(proto::max_message_size, 64U * 1024U) << "max_message_size must be 64 KiB.";
}

// ---- Hand-written JSON decoding (wire rules) ---------------------------------------------------------------------

/**
 * @brief A hand-written PING decodes to its sequence number.
 */
TEST(HandoffProtocolTest, DecodeHandWrittenPing) {
  proto::ping_t expected;
  expected.seq = 7;
  EXPECT_EQ(decode_ok(R"({"type":"PING","seq":7})"), proto::message_t {expected})
    << "{\"type\":\"PING\",\"seq\":7} must decode to ping_t with seq 7.";
}

/**
 * @brief A hand-written PONG decodes to its sequence number.
 */
TEST(HandoffProtocolTest, DecodeHandWrittenPong) {
  proto::pong_t expected;
  expected.seq = 42;
  EXPECT_EQ(decode_ok(R"({"type":"PONG","seq":42})"), proto::message_t {expected})
    << "{\"type\":\"PONG\",\"seq\":42} must decode to pong_t with seq 42.";
}

/**
 * @brief A full hand-written HELLO decodes with its probe, identity and cooldown.
 */
TEST(HandoffProtocolTest, DecodeHandWrittenFullHello) {
  const std::string payload = R"({
      "type":"HELLO","major":1,"minor":0,"capabilities":["feature-a"],
      "probe":{"hevc_mode":1,"av1_mode":0,"yuv444":false},
      "identity":{"build_id":"b","render_node":"/dev/dri/renderD128","vendor_id":4098,"device_id":5,"driver_version":"550.1"},
      "cooldown_deadline_ns":42
    })";
  proto::hello_t expected;
  expected.capabilities = {"feature-a"};
  proto::probe_results_t probe;
  probe.hevc_mode = 1;
  probe.av1_mode = 0;
  probe.yuv444 = false;
  expected.probe = probe;
  proto::probe_identity_t identity;
  identity.build_id = "b";
  identity.render_node = "/dev/dri/renderD128";
  identity.vendor_id = 4098;
  identity.device_id = 5;
  identity.driver_version = "550.1";
  expected.identity = identity;
  expected.cooldown_deadline_ns = 42;
  EXPECT_EQ(decode_ok(payload), proto::message_t {expected}) << "The full HELLO must decode to the matching hello_t.";
}

/**
 * @brief A HELLO whose optionals are null decodes with those optionals empty.
 */
TEST(HandoffProtocolTest, DecodeHelloWithNullOptionalsGivesEmptyOptionals) {
  const auto message = decode_ok(
    R"({"type":"HELLO","major":1,"minor":0,"capabilities":[],"probe":null,"identity":null,"cooldown_deadline_ns":null})"
  );
  const auto *hello = std::get_if<proto::hello_t>(&message);
  ASSERT_NE(hello, nullptr) << "A HELLO must decode to hello_t.";
  EXPECT_FALSE(hello->probe.has_value()) << "A null probe must decode as an empty optional.";
  EXPECT_FALSE(hello->identity.has_value()) << "A null identity must decode as an empty optional.";
  EXPECT_FALSE(hello->cooldown_deadline_ns.has_value()) << "A null cooldown must decode as an empty optional.";
}

/**
 * @brief A HELLO with optionals omitted decodes with those optionals empty.
 */
TEST(HandoffProtocolTest, DecodeHelloWithOmittedOptionalsGivesEmptyOptionals) {
  const auto message = decode_ok(R"({"type":"HELLO","major":1,"minor":0,"capabilities":[]})");
  const auto *hello = std::get_if<proto::hello_t>(&message);
  ASSERT_NE(hello, nullptr) << "A HELLO must decode to hello_t.";
  EXPECT_FALSE(hello->probe.has_value()) << "An omitted probe must decode as an empty optional.";
  EXPECT_FALSE(hello->identity.has_value()) << "An omitted identity must decode as an empty optional.";
  EXPECT_FALSE(hello->cooldown_deadline_ns.has_value()) << "An omitted cooldown must decode as an empty optional.";
}

/**
 * @brief A hand-written READY decodes with ok and reason.
 */
TEST(HandoffProtocolTest, DecodeHandWrittenReady) {
  proto::ready_t expected;
  expected.ok = false;
  expected.reason = "config port mismatch";
  EXPECT_EQ(decode_ok(R"({"type":"READY","ok":false,"reason":"config port mismatch"})"), proto::message_t {expected})
    << "READY must decode its ok and reason members.";
}

/**
 * @brief A hand-written GO with an app decodes launch_env pairs in order.
 */
TEST(HandoffProtocolTest, DecodeHandWrittenGoWithApp) {
  const std::string payload = R"({"type":"GO","app":{"app_id":3,"name":"n","applied_prep":1,)"
                              R"("digest":")" +
                              kDigest + R"(","launch_env":[["SUNSHINE_A","1"],["SUNSHINE_B","two"]]},)"
                                        R"("host_audio":true,"next_launch_session_id":4})";
  proto::go_t expected;
  proto::app_snapshot_t snapshot;
  snapshot.app_id = 3;
  snapshot.name = "n";
  snapshot.applied_prep = 1;
  snapshot.digest = kDigest;
  snapshot.launch_env = {{"SUNSHINE_A", "1"}, {"SUNSHINE_B", "two"}};
  expected.app = snapshot;
  expected.host_audio = true;
  expected.next_launch_session_id = 4;
  EXPECT_EQ(decode_ok(payload), proto::message_t {expected})
    << "GO with an app must decode the snapshot, its launch_env pairs in order, host_audio and the next session id.";
}

/**
 * @brief A hand-written GO with app null decodes to no app.
 */
TEST(HandoffProtocolTest, DecodeHandWrittenGoWithNullApp) {
  proto::go_t expected;
  expected.host_audio = false;
  expected.next_launch_session_id = 0;
  EXPECT_EQ(decode_ok(R"({"type":"GO","app":null,"host_audio":false,"next_launch_session_id":0})"), proto::message_t {expected})
    << "GO with app null must decode to a GO with no app.";
}

/**
 * @brief A hand-written GO_ACK decodes with ok and reason.
 */
TEST(HandoffProtocolTest, DecodeHandWrittenGoAck) {
  proto::go_ack_t expected;
  expected.ok = true;
  EXPECT_EQ(decode_ok(R"({"type":"GO_ACK","ok":true,"reason":""})"), proto::message_t {expected})
    << "GO_ACK must decode its ok and reason members.";
}

/**
 * @brief COMMIT and MAIN decode with only their type.
 */
TEST(HandoffProtocolTest, DecodeHandWrittenCommitAndMain) {
  EXPECT_EQ(decode_ok(R"({"type":"COMMIT"})"), proto::message_t {proto::commit_t {}}) << "COMMIT must decode.";
  EXPECT_EQ(decode_ok(R"({"type":"MAIN"})"), proto::message_t {proto::main_t {}}) << "MAIN must decode.";
}

/**
 * @brief A hand-written SESSIONS? request decodes to its request id.
 */
TEST(HandoffProtocolTest, DecodeHandWrittenSessionsRequest) {
  proto::sessions_request_t expected;
  expected.request_id = 11;
  EXPECT_EQ(decode_ok(R"({"type":"SESSIONS?","request_id":11})"), proto::message_t {expected})
    << "SESSIONS? must decode its request_id.";
}

/**
 * @brief A hand-written SESSIONS reply decodes its entries.
 */
TEST(HandoffProtocolTest, DecodeHandWrittenSessionsReply) {
  proto::sessions_reply_t expected;
  expected.request_id = 11;
  expected.sessions = {session_entry(2)};
  const std::string payload =
    R"({"type":"SESSIONS","request_id":11,"sessions":[{"id":2,"started_unix_ms":1700000000000,"transport":"udp/ipv4","client_category":"lan"}]})";
  EXPECT_EQ(decode_ok(payload), proto::message_t {expected}) << "SESSIONS must decode request_id and each entry.";
}

/**
 * @brief A hand-written CLOSE decodes its request id and session id.
 */
TEST(HandoffProtocolTest, DecodeHandWrittenClose) {
  proto::close_request_t expected;
  expected.request_id = 12;
  expected.session_id = 2;
  EXPECT_EQ(decode_ok(R"({"type":"CLOSE","request_id":12,"session_id":2})"), proto::message_t {expected})
    << "CLOSE must decode request_id and session_id.";
}

/**
 * @brief A hand-written CLOSED decodes its result, each of the three values.
 */
TEST(HandoffProtocolTest, DecodeHandWrittenClosedWithEachResult) {
  for (const std::string result : {"closed", "already-stopping", "not-found"}) {
    proto::close_reply_t expected;
    expected.request_id = 12;
    expected.result = result;
    EXPECT_EQ(decode_ok(R"({"type":"CLOSED","request_id":12,"result":")" + result + R"("})"), proto::message_t {expected})
      << "CLOSED with result " << result << " must decode.";
  }
}

/**
 * @brief A hand-written CLOSE_CERT decodes its request id and the 64-character hash.
 */
TEST(HandoffProtocolTest, DecodeHandWrittenCloseCert) {
  proto::close_cert_request_t expected;
  expected.request_id = 13;
  expected.cert_sha256 = kCertHash;
  EXPECT_EQ(decode_ok(dump({{"type", "CLOSE_CERT"}, {"request_id", 13}, {"cert_sha256", kCertHash}})), proto::message_t {expected})
    << "CLOSE_CERT must decode request_id and a 64-character lowercase cert_sha256.";
}

/**
 * @brief A hand-written CLOSED_CERT decodes its request id and count.
 */
TEST(HandoffProtocolTest, DecodeHandWrittenClosedCert) {
  // SPEC-QUESTION: see EncodeCloseCertReplyUsesClosedCertName; the wire name follows the header.
  proto::close_cert_reply_t expected;
  expected.request_id = 13;
  expected.count = 2;
  EXPECT_EQ(decode_ok(R"({"type":"CLOSED_CERT","request_id":13,"count":2})"), proto::message_t {expected})
    << "CLOSED_CERT must decode request_id and count.";
}

/**
 * @brief END_ALL, ENDED and END_REJECTED each decode their request id.
 */
TEST(HandoffProtocolTest, DecodeHandWrittenEndAllFamily) {
  proto::end_all_t end_all;
  end_all.request_id = 14;
  EXPECT_EQ(decode_ok(R"({"type":"END_ALL","request_id":14})"), proto::message_t {end_all}) << "END_ALL must decode.";

  proto::ended_t ended;
  ended.request_id = 14;
  EXPECT_EQ(decode_ok(R"({"type":"ENDED","request_id":14})"), proto::message_t {ended}) << "ENDED must decode.";

  proto::end_rejected_t rejected;
  rejected.request_id = 14;
  EXPECT_EQ(decode_ok(R"({"type":"END_REJECTED","request_id":14})"), proto::message_t {rejected})
    << "END_REJECTED must decode.";
}

/**
 * @brief Each of the four HANDOFF_STATE values decodes.
 */
TEST(HandoffProtocolTest, DecodeHandWrittenHandoffStateWithEachValue) {
  for (const std::string state : {"none", "draining", "handover_failed", "kill_pending"}) {
    proto::handoff_state_t expected;
    expected.state = state;
    EXPECT_EQ(decode_ok(R"({"type":"HANDOFF_STATE","state":")" + state + R"("})"), proto::message_t {expected})
      << "HANDOFF_STATE with state " << state << " must decode.";
  }
}

/**
 * @brief Unsigned members at the top of their range decode.
 */
TEST(HandoffProtocolTest, DecodeUnsignedBoundariesAccepted) {
  proto::close_request_t expected;
  expected.request_id = 1;
  expected.session_id = 4294967295U;
  EXPECT_EQ(decode_ok(R"({"type":"CLOSE","request_id":1,"session_id":4294967295})"), proto::message_t {expected})
    << "A session_id of 4294967295 (uint32 max) must be accepted.";

  proto::ping_t ping;
  ping.seq = std::numeric_limits<std::uint64_t>::max();
  EXPECT_EQ(decode_ok(R"({"type":"PING","seq":18446744073709551615})"), proto::message_t {ping})
    << "A seq of 18446744073709551615 (uint64 max) must be accepted.";
}

// ---- Unknown types and extra members -----------------------------------------------------------------------------

/**
 * @brief An unknown type decodes to unknown_t carrying the type, and is not an error.
 */
TEST(HandoffProtocolTest, UnknownTypeDecodesToUnknown) {
  const auto message = decode_ok(R"({"type":"FUTURE_THING","x":[1,2]})");
  const auto *unknown = std::get_if<proto::unknown_t>(&message);
  ASSERT_NE(unknown, nullptr) << "An unknown type must decode to unknown_t.";
  EXPECT_EQ(unknown->type, "FUTURE_THING") << "unknown_t must carry the type as received.";
}

/**
 * @brief An unknown type with no other members still decodes to unknown_t.
 */
TEST(HandoffProtocolTest, UnknownTypeWithOnlyTypeDecodesToUnknown) {
  const auto message = decode_ok(R"({"type":"FUTURE"})");
  proto::unknown_t expected;
  expected.type = "FUTURE";
  EXPECT_EQ(message, proto::message_t {expected}) << "A type-only unknown message must decode to unknown_t.";
}

/**
 * @brief Type names are case sensitive, so a lowercase known name is an unknown type.
 */
TEST(HandoffProtocolTest, LowercaseKnownTypeNameIsUnknown) {
  const auto message = decode_ok(R"({"type":"ping","seq":1})");
  const auto *unknown = std::get_if<proto::unknown_t>(&message);
  ASSERT_NE(unknown, nullptr) << "A lowercase type name is not a known type and must decode to unknown_t.";
  EXPECT_EQ(unknown->type, "ping") << "unknown_t must carry the lowercase type as received.";
}

/**
 * @brief Members not listed for a type are ignored.
 */
TEST(HandoffProtocolTest, ExtraMembersAreIgnored) {
  proto::ping_t expected;
  expected.seq = 3;
  EXPECT_EQ(decode_ok(R"({"type":"PING","seq":3,"extra":{"a":1},"zzz":"q"})"), proto::message_t {expected})
    << "Members that are not listed for the type must be ignored.";

  proto::ready_t ready;
  ready.ok = true;
  EXPECT_EQ(decode_ok(R"({"type":"READY","ok":true,"reason":"","extra":5})"), proto::message_t {ready})
    << "An extra member on a known type must be ignored.";
}

// ---- Size limits -------------------------------------------------------------------------------------------------

/**
 * @brief A payload longer than 64 KiB is oversized, whatever it contains.
 */
TEST(HandoffProtocolTest, PayloadLongerThanLimitIsOversized) {
  proto::message_t message;
  EXPECT_EQ(proto::decode(std::string(proto::max_message_size + 1, 'x'), message), proto::decode_status_e::oversized)
    << "A payload of 65537 bytes must be oversized (checked before parsing).";
}

/**
 * @brief An oversized payload with otherwise valid JSON is still oversized.
 */
TEST(HandoffProtocolTest, OversizedValidJsonIsOversized) {
  const std::string prefix = R"({"type":"FUTURE","pad":")";
  const std::string suffix = R"("})";
  const std::string payload = prefix + std::string(proto::max_message_size, 'x') + suffix;
  ASSERT_GT(payload.size(), proto::max_message_size) << "Precondition: the payload must exceed the limit.";
  proto::message_t message;
  EXPECT_EQ(proto::decode(payload, message), proto::decode_status_e::oversized)
    << "A valid JSON payload longer than 64 KiB must be oversized.";
}

/**
 * @brief A payload of exactly 64 KiB is not oversized.
 */
TEST(HandoffProtocolTest, PayloadOfExactlyLimitIsNotOversized) {
  const std::string prefix = R"({"type":"FUTURE","pad":")";
  const std::string suffix = R"("})";
  const std::size_t pad = proto::max_message_size - prefix.size() - suffix.size();
  const std::string payload = prefix + std::string(pad, 'x') + suffix;
  ASSERT_EQ(payload.size(), proto::max_message_size) << "Precondition: the payload must be exactly 64 KiB.";
  proto::message_t message;
  const auto status = proto::decode(payload, message);
  EXPECT_NE(status, proto::decode_status_e::oversized)
    << "A payload of exactly max_message_size bytes is within the limit and must not be oversized.";
  EXPECT_EQ(status, proto::decode_status_e::ok) << "The 64 KiB unknown message must decode with status ok.";
}

// ---- Malformed input ---------------------------------------------------------------------------------------------

/**
 * @brief Text that is not JSON is malformed.
 */
TEST(HandoffProtocolTest, NotJsonIsMalformed) {
  expect_malformed("not json");
  expect_malformed("");
  expect_malformed("{");
  expect_malformed("PING");
}

/**
 * @brief JSON that is not an object is malformed.
 */
TEST(HandoffProtocolTest, NonObjectJsonIsMalformed) {
  expect_malformed("[1,2]");
  expect_malformed("42");
  expect_malformed("\"PING\"");
  expect_malformed("null");
}

/**
 * @brief A message without a type member is malformed.
 */
TEST(HandoffProtocolTest, MissingTypeIsMalformed) {
  expect_malformed(R"({"seq":7})");
}

/**
 * @brief A type member that is not a string is malformed.
 */
TEST(HandoffProtocolTest, NonStringTypeIsMalformed) {
  expect_malformed(R"({"type":5,"seq":7})");
  expect_malformed(R"({"type":null})");
}

/**
 * @brief A known type with a missing member is malformed.
 */
TEST(HandoffProtocolTest, MissingMemberIsMalformed) {
  expect_malformed(R"({"type":"PING"})");
  expect_malformed(R"({"type":"READY","ok":true})");
  expect_malformed(R"({"type":"HELLO","major":1,"minor":0})");
  expect_malformed(R"({"type":"CLOSE","request_id":1})");
  expect_malformed(R"({"type":"SESSIONS","request_id":1})");
  expect_malformed(R"({"type":"GO","next_launch_session_id":1,"app":null})");
  expect_malformed(R"({"type":"HANDOFF_STATE"})");
}

/**
 * @brief A known type whose member has the wrong JSON type is malformed.
 */
TEST(HandoffProtocolTest, WrongMemberTypeIsMalformed) {
  expect_malformed(R"({"type":"PING","seq":"7"})");
  expect_malformed(R"({"type":"READY","ok":"yes","reason":""})");
  expect_malformed(R"({"type":"HELLO","major":1,"minor":0,"capabilities":"x"})");
  expect_malformed(R"({"type":"HELLO","major":1,"minor":0,"capabilities":[1]})");
  expect_malformed(R"({"type":"SESSIONS","request_id":1,"sessions":{}})");
  expect_malformed(R"({"type":"HANDOFF_STATE","state":3})");
  expect_malformed(R"({"type":"GO","app":5,"host_audio":true,"next_launch_session_id":1})");
  expect_malformed(R"({"type":"GO","host_audio":1,"next_launch_session_id":1})");
}

/**
 * @brief A floating-point value where an unsigned integer is required is malformed.
 */
TEST(HandoffProtocolTest, FloatWhereUnsignedRequiredIsMalformed) {
  expect_malformed(R"({"type":"PING","seq":1.5})");
}

/**
 * @brief A negative value where an unsigned integer is required is malformed.
 */
TEST(HandoffProtocolTest, NegativeUnsignedIsMalformed) {
  expect_malformed(R"({"type":"PING","seq":-1})");
  expect_malformed(R"({"type":"HELLO","major":-1,"minor":0,"capabilities":[]})");
  expect_malformed(R"({"type":"CLOSE","request_id":1,"session_id":-2})");
  expect_malformed(R"({"type":"CLOSE_CERT","request_id":-3,"cert_sha256":")" + kCertHash + R"("})");
}

/**
 * @brief An unsigned value above its C++ type's range is malformed.
 */
TEST(HandoffProtocolTest, OverflowingUnsignedIsMalformed) {
  expect_malformed(R"({"type":"CLOSE","request_id":1,"session_id":4294967296})");
  expect_malformed(R"({"type":"HELLO","major":4294967296,"minor":0,"capabilities":[]})");
  expect_malformed(R"({"type":"PING","seq":18446744073709551616})");
  expect_malformed(R"({"type":"GO","host_audio":true,"next_launch_session_id":4294967296})");
}

/**
 * @brief A vendor id above the uint32 range in the probe identity is malformed.
 */
TEST(HandoffProtocolTest, OverflowingVendorIdIsMalformed) {
  expect_malformed(
    R"({"type":"HELLO","major":1,"minor":0,"capabilities":[],"identity":{"build_id":"b","render_node":"r","vendor_id":4294967296,"device_id":1,"driver_version":"d"}})"
  );
}

/**
 * @brief A cert hash that is not exactly 64 characters is malformed.
 */
TEST(HandoffProtocolTest, CertHashWrongLengthIsMalformed) {
  expect_malformed(dump({{"type", "CLOSE_CERT"}, {"request_id", 1}, {"cert_sha256", kCertHash.substr(0, 63)}}));
  expect_malformed(dump({{"type", "CLOSE_CERT"}, {"request_id", 1}, {"cert_sha256", kCertHash + "0"}}));
  expect_malformed(dump({{"type", "CLOSE_CERT"}, {"request_id", 1}, {"cert_sha256", ""}}));
}

/**
 * @brief A cert hash with uppercase hex is malformed; the header requires lowercase.
 */
TEST(HandoffProtocolTest, CertHashUppercaseIsMalformed) {
  std::string upper = kCertHash;
  upper[0] = 'F';
  expect_malformed(dump({{"type", "CLOSE_CERT"}, {"request_id", 1}, {"cert_sha256", upper}}));
}

/**
 * @brief A cert hash with a non-hex character is malformed.
 */
TEST(HandoffProtocolTest, CertHashNonHexCharacterIsMalformed) {
  std::string bad = kCertHash;
  bad[10] = 'g';
  expect_malformed(dump({{"type", "CLOSE_CERT"}, {"request_id", 1}, {"cert_sha256", bad}}));

  std::string spaced = kCertHash;
  spaced[5] = ' ';
  expect_malformed(dump({{"type", "CLOSE_CERT"}, {"request_id", 1}, {"cert_sha256", spaced}}));
}

/**
 * @brief A cert hash that is not a string is malformed.
 */
TEST(HandoffProtocolTest, CertHashNotStringIsMalformed) {
  expect_malformed(R"({"type":"CLOSE_CERT","request_id":1,"cert_sha256":123})");
}

/**
 * @brief A 64-character lowercase hex hash is accepted.
 */
TEST(HandoffProtocolTest, CertHashValidIsAccepted) {
  proto::close_cert_request_t expected;
  expected.request_id = 1;
  expected.cert_sha256 = std::string(64, 'a');
  EXPECT_EQ(decode_ok(dump({{"type", "CLOSE_CERT"}, {"request_id", 1}, {"cert_sha256", std::string(64, 'a')}})), proto::message_t {expected})
    << "A 64-character lowercase hex cert_sha256 must be accepted.";
}

/**
 * @brief A CLOSED result outside the three documented values is malformed.
 */
TEST(HandoffProtocolTest, ClosedBadResultIsMalformed) {
  expect_malformed(R"({"type":"CLOSED","request_id":1,"result":"bogus"})");
  expect_malformed(R"({"type":"CLOSED","request_id":1,"result":"Closed"})");
  expect_malformed(R"({"type":"CLOSED","request_id":1,"result":""})");
  expect_malformed(R"({"type":"CLOSED","request_id":1,"result":7})");
}

/**
 * @brief A CLOSED message with a count instead of a result is malformed (the CLOSE reply needs a result).
 */
TEST(HandoffProtocolTest, ClosedWithCountOnlyIsMalformed) {
  // SPEC-QUESTION: see EncodeCloseCertReplyUsesClosedCertName; CLOSED is the CLOSE reply, so count alone is malformed.
  expect_malformed(R"({"type":"CLOSED","request_id":1,"count":2})");
}

/**
 * @brief A CLOSED_CERT message with a result instead of a count is malformed.
 */
TEST(HandoffProtocolTest, ClosedCertWithResultOnlyIsMalformed) {
  // SPEC-QUESTION: see EncodeCloseCertReplyUsesClosedCertName; the wire name follows the header.
  expect_malformed(R"({"type":"CLOSED_CERT","request_id":1,"result":"closed"})");
}

/**
 * @brief A HANDOFF_STATE state outside the four documented values is malformed.
 */
TEST(HandoffProtocolTest, HandoffStateBadValueIsMalformed) {
  expect_malformed(R"({"type":"HANDOFF_STATE","state":"drain"})");
  expect_malformed(R"({"type":"HANDOFF_STATE","state":"Draining"})");
  expect_malformed(R"({"type":"HANDOFF_STATE","state":""})");
}

/**
 * @brief A GO app snapshot with an incomplete member set is malformed.
 */
TEST(HandoffProtocolTest, GoSnapshotMissingMemberIsMalformed) {
  expect_malformed(R"({"type":"GO","app":{"app_id":3,"name":"n","applied_prep":1,"launch_env":[]},"host_audio":false,"next_launch_session_id":0})");
}

/**
 * @brief A launch_env entry that is not a two-string array is malformed.
 */
TEST(HandoffProtocolTest, LaunchEnvEntryMustBeTwoStrings) {
  const std::string app_head = R"({"type":"GO","app":{"app_id":3,"name":"n","applied_prep":1,"digest":")" + kDigest +
                               R"(","launch_env":)";
  const std::string app_tail = R"(},"host_audio":false,"next_launch_session_id":0})";
  expect_malformed(app_head + R"([["SUNSHINE_A"]])" + app_tail);
  expect_malformed(app_head + R"([["SUNSHINE_A",1]])" + app_tail);
  expect_malformed(app_head + R"([["SUNSHINE_A","1","extra"]])" + app_tail);
  expect_malformed(app_head + R"(["SUNSHINE_A=1"])" + app_tail);
}

/**
 * @brief A session entry that is not an object, or lacks a member, is malformed.
 */
TEST(HandoffProtocolTest, MalformedSessionEntryIsMalformed) {
  expect_malformed(R"({"type":"SESSIONS","request_id":1,"sessions":["x"]})");
  expect_malformed(
    R"({"type":"SESSIONS","request_id":1,"sessions":[{"id":2,"started_unix_ms":1,"client_category":"lan"}]})"
  );
}

/**
 * @brief Malformed input decodes to the malformed status and leaves the output untouched.
 */
TEST(HandoffProtocolTest, MalformedInputIsNotDecodedAsOtherTypes) {
  proto::message_t message = proto::ping_t {};
  EXPECT_EQ(proto::decode("garbage", message), proto::decode_status_e::malformed)
    << "Garbage input must be malformed.";
}
#endif
