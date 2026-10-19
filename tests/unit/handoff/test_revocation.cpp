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
  #include <utility>
  #include <vector>

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

/**
 * @brief The ptree string form `"enabled":"false"` is a disabled client.
 */
TEST(HandoffRevocationTest, StringFalseIsDisabled) {
  const std::string json = R"({"root":{"named_devices":[{"cert":"cert-s","enabled":"false"}]}})";
  const auto result = handoff::disabled_client_certs(json);
  ASSERT_TRUE(result.has_value()) << "A valid state file must return a value.";
  EXPECT_EQ(*result, (std::set<std::string> {"cert-s"}))
    << "enabled as the string \"false\" (boost ptree form) must count as disabled and be returned.";
}

/**
 * @brief The ptree string form `"enabled":"true"` is an enabled client and is not returned.
 */
TEST(HandoffRevocationTest, StringTrueIsEnabled) {
  const std::string json = R"({"root":{"named_devices":[{"cert":"cert-s","enabled":"true"}]}})";
  const auto result = handoff::disabled_client_certs(json);
  ASSERT_TRUE(result.has_value()) << "A valid state file must return a value.";
  EXPECT_TRUE(result->empty()) << "enabled as the string \"true\" must not be returned.";
}

/**
 * @brief The ptree string form `"enabled":"0"` is disabled.
 */
TEST(HandoffRevocationTest, StringZeroIsDisabled) {
  const std::string json = R"({"root":{"named_devices":[{"cert":"cert-z","enabled":"0"}]}})";
  const auto result = handoff::disabled_client_certs(json);
  ASSERT_TRUE(result.has_value()) << "A valid state file must return a value.";
  EXPECT_EQ(*result, (std::set<std::string> {"cert-z"})) << "enabled as the string \"0\" must count as disabled.";
}

/**
 * @brief The ptree string form `"enabled":"1"` is enabled and is not returned.
 */
TEST(HandoffRevocationTest, StringOneIsEnabled) {
  const std::string json = R"({"root":{"named_devices":[{"cert":"cert-o","enabled":"1"}]}})";
  const auto result = handoff::disabled_client_certs(json);
  ASSERT_TRUE(result.has_value()) << "A valid state file must return a value.";
  EXPECT_TRUE(result->empty()) << "enabled as the string \"1\" must not be returned.";
}

/**
 * @brief `named_devices` written as the empty ptree string is an empty set, not nullopt.
 */
TEST(HandoffRevocationTest, EmptyStringNamedDevicesGivesEmptySet) {
  const std::string json = R"({"root":{"uniqueid":"x","named_devices":""}})";
  const auto result = handoff::disabled_client_certs(json);
  ASSERT_TRUE(result.has_value()) << "named_devices written as \"\" must be read as an empty array, not nullopt.";
  EXPECT_TRUE(result->empty()) << "An empty ptree named_devices must give an empty set.";
}

/**
 * @brief Any `enabled` value other than the documented enabled forms fails closed: the client is returned.
 */
TEST(HandoffRevocationTest, UnrecognisedEnabledValuesFailClosed) {
  const std::vector<std::pair<std::string, std::string>> cases = {
    {R"("yes")", "string yes"},
    {"0", "number 0"},
    {"1", "number 1"},
    {"null", "null"},
    {"{}", "empty object"},
    {"[]", "empty array"},
  };
  for (const auto &[value, label] : cases) {
    SCOPED_TRACE(label);
    const std::string json = R"({"root":{"named_devices":[{"cert":"cert-f","enabled":)" + value + R"(}]}})";
    const auto result = handoff::disabled_client_certs(json);
    ASSERT_TRUE(result.has_value()) << "A valid state file must return a value.";
    EXPECT_EQ(result->count("cert-f"), 1U)
      << "enabled = " << label << " is not an enabled form, so the client must fail closed and be returned.";
  }
}

/**
 * @brief An entry without a string `cert` is skipped; other disabled entries still count.
 */
TEST(HandoffRevocationTest, EntryWithoutStringCertIsSkipped) {
  const std::string json = R"({"root":{"named_devices":[
      {"cert":123,"enabled":"false"},
      {"enabled":"false"},
      {"cert":"cert-ok","enabled":"false"}
    ]}})";
  const auto result = handoff::disabled_client_certs(json);
  ASSERT_TRUE(result.has_value()) << "A valid state file must return a value even when some entries lack a string cert.";
  EXPECT_EQ(*result, (std::set<std::string> {"cert-ok"}))
    << "Entries with a non-string or missing cert must be skipped, while the valid disabled entry is still returned.";
}

/**
 * @brief Boolean and string forms of `enabled` can be mixed in one file.
 */
TEST(HandoffRevocationTest, MixedBooleanAndStringFormsInOneFile) {
  const std::string json = R"({"root":{"named_devices":[
      {"cert":"c-bool-false","enabled":false},
      {"cert":"c-str-false","enabled":"false"},
      {"cert":"c-str-zero","enabled":"0"},
      {"cert":"c-bool-true","enabled":true},
      {"cert":"c-str-true","enabled":"true"},
      {"cert":"c-str-one","enabled":"1"},
      {"cert":"c-missing"}
    ]}})";
  const auto result = handoff::disabled_client_certs(json);
  ASSERT_TRUE(result.has_value()) << "A valid mixed-form state file must return a value.";
  EXPECT_EQ(*result, (std::set<std::string> {"c-bool-false", "c-str-false", "c-str-zero"}))
    << "Only the disabled forms (false, \"false\", \"0\") must be returned from a mixed file.";
}
#endif
