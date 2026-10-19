/**
 * @file tests/unit/handoff/test_revocation.cpp
 * @brief Blind tests for the revocation backstop parser (src/handoff/revocation.*), written from the spec and header.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <optional>
  #include <set>
  #include <string>

  // local includes
  #include "src/handoff/revocation.h"

/**
 * @brief Only entries with `enabled` explicitly `false` are returned.
 */
TEST(HandoffRevocationTest, DisabledClientsAreReturned) {
  const std::string json = R"({
      "root": {
        "uniqueid": "host-unique-id",
        "named_devices": [
          {"name": "a", "cert": "cert-a", "uuid": "u1", "enabled": false},
          {"name": "b", "cert": "cert-b", "uuid": "u2", "enabled": true},
          {"name": "c", "cert": "cert-c", "uuid": "u3"},
          {"name": "d", "cert": "cert-d", "uuid": "u4", "enabled": false}
        ]
      }
    })";
  const auto result = handoff::disabled_client_certs(json);
  ASSERT_TRUE(result.has_value()) << "A valid state file with named_devices must return a value, not nullopt.";
  EXPECT_EQ(*result, (std::set<std::string> {"cert-a", "cert-d"}))
    << "Only the clients with enabled=false (a and d) must be returned; enabled=true (b) and a missing enabled (c) must not.";
}

/**
 * @brief An enabled client is not returned.
 */
TEST(HandoffRevocationTest, EnabledClientIsNotReturned) {
  const std::string json = R"({"root":{"named_devices":[{"name":"b","cert":"cert-b","uuid":"u","enabled":true}]}})";
  const auto result = handoff::disabled_client_certs(json);
  ASSERT_TRUE(result.has_value()) << "A valid state file must return a value.";
  EXPECT_TRUE(result->empty()) << "A client with enabled=true must not be returned.";
}

/**
 * @brief A client whose `enabled` member is missing is enabled by default, so it is not returned.
 */
TEST(HandoffRevocationTest, MissingEnabledIsNotReturned) {
  const std::string json = R"({"root":{"named_devices":[{"name":"c","cert":"cert-c","uuid":"u"}]}})";
  const auto result = handoff::disabled_client_certs(json);
  ASSERT_TRUE(result.has_value()) << "A valid state file must return a value.";
  EXPECT_TRUE(result->empty()) << "A client with no enabled member defaults to true and must not be returned.";
}

/**
 * @brief A client that is not in the file is not returned (the function only reports what the file disables).
 */
TEST(HandoffRevocationTest, ClientMissingFromFileIsNotReturned) {
  const std::string json = R"({"root":{"named_devices":[{"name":"a","cert":"cert-a","uuid":"u","enabled":false}]}})";
  const auto result = handoff::disabled_client_certs(json);
  ASSERT_TRUE(result.has_value()) << "A valid state file must return a value.";
  EXPECT_EQ(result->count("cert-never-listed"), 0U)
    << "A certificate that is not in the file must not appear in the result.";
  EXPECT_EQ(*result, (std::set<std::string> {"cert-a"}))
    << "Only the listed disabled client must appear.";
}

/**
 * @brief An empty client list is valid and gives an empty set, not nullopt.
 */
TEST(HandoffRevocationTest, EmptyNamedDevicesGivesEmptySet) {
  const std::string json = R"({"root":{"uniqueid":"x","named_devices":[]}})";
  const auto result = handoff::disabled_client_certs(json);
  ASSERT_TRUE(result.has_value()) << "An empty but parseable named_devices array must return a value.";
  EXPECT_TRUE(result->empty()) << "An empty client list must give an empty set.";
}

/**
 * @brief PEM certificates with JSON-escaped newlines come back as the decoded text.
 */
TEST(HandoffRevocationTest, PemCertificateIsReturnedDecoded) {
  const std::string json =
    R"({"root":{"named_devices":[{"name":"a","cert":"-----BEGIN CERTIFICATE-----\nFAKEDATA\n-----END CERTIFICATE-----\n","uuid":"u","enabled":false}]}})";
  const auto result = handoff::disabled_client_certs(json);
  ASSERT_TRUE(result.has_value()) << "A valid state file must return a value.";
  EXPECT_EQ(*result, (std::set<std::string> {"-----BEGIN CERTIFICATE-----\nFAKEDATA\n-----END CERTIFICATE-----\n"}))
    << "The returned certificate must be the decoded PEM string, with real newlines.";
}

/**
 * @brief Text that is not JSON gives nullopt.
 */
TEST(HandoffRevocationTest, InvalidJsonReturnsNullopt) {
  EXPECT_FALSE(handoff::disabled_client_certs("{not json").has_value())
    << "Invalid JSON must give nullopt.";
}

/**
 * @brief An empty file gives nullopt.
 */
TEST(HandoffRevocationTest, EmptyContentReturnsNullopt) {
  EXPECT_FALSE(handoff::disabled_client_certs("").has_value())
    << "An empty state file must give nullopt.";
}

/**
 * @brief A top-level JSON value that is not an object has no root and gives nullopt.
 */
TEST(HandoffRevocationTest, TopLevelArrayReturnsNullopt) {
  EXPECT_FALSE(handoff::disabled_client_certs("[1,2,3]").has_value())
    << "A top-level array has no root object and must give nullopt.";
}

/**
 * @brief A file without a `root` member gives nullopt.
 */
TEST(HandoffRevocationTest, MissingRootReturnsNullopt) {
  const std::string json = R"({"other":{"named_devices":[]}})";
  EXPECT_FALSE(handoff::disabled_client_certs(json).has_value())
    << "A state file with no root member must give nullopt.";
}

/**
 * @brief A `root` that is not an object gives nullopt.
 */
TEST(HandoffRevocationTest, RootThatIsNotObjectReturnsNullopt) {
  EXPECT_FALSE(handoff::disabled_client_certs(R"({"root":5})").has_value())
    << "A root member that is not an object must give nullopt.";
}

/**
 * @brief A root without `named_devices` gives nullopt.
 */
TEST(HandoffRevocationTest, MissingNamedDevicesReturnsNullopt) {
  const std::string json = R"({"root":{"uniqueid":"x"}})";
  EXPECT_FALSE(handoff::disabled_client_certs(json).has_value())
    << "A root with no named_devices must give nullopt (the caller retries later).";
}

/**
 * @brief `named_devices` that is an object rather than an array gives nullopt.
 */
TEST(HandoffRevocationTest, NamedDevicesObjectReturnsNullopt) {
  const std::string json = R"({"root":{"named_devices":{}}})";
  EXPECT_FALSE(handoff::disabled_client_certs(json).has_value())
    << "named_devices that is an object, not an array, must give nullopt.";
}

/**
 * @brief `named_devices` that is a string gives nullopt.
 */
TEST(HandoffRevocationTest, NamedDevicesStringReturnsNullopt) {
  const std::string json = R"({"root":{"named_devices":"cert-a"}})";
  EXPECT_FALSE(handoff::disabled_client_certs(json).has_value())
    << "named_devices that is a string, not an array, must give nullopt.";
}

/**
 * @brief `named_devices` that is null gives nullopt.
 */
TEST(HandoffRevocationTest, NamedDevicesNullReturnsNullopt) {
  const std::string json = R"({"root":{"named_devices":null}})";
  EXPECT_FALSE(handoff::disabled_client_certs(json).has_value())
    << "named_devices that is null must give nullopt.";
}
#endif
