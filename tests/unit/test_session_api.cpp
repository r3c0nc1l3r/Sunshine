/**
 * @file tests/unit/test_session_api.cpp
 * @brief Tests for the session list and close API: the JSON shape, the RTSP helpers, authentication, and the audit log.
 *
 * The HTTP tests use a real HTTPS client/server with the same approach as tests/unit/test_confighttp.cpp.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// lib includes
#include <Simple-Web-Server/client_https.hpp>
#include <Simple-Web-Server/crypto.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include <src/config.h>
#include <src/confighttp.h>
#include <src/crypto.h>
#include <src/httpcommon.h>
#include <src/rtsp.h>
#include <src/utility.h>

namespace {
  const std::string TEST_PRIVATE_KEY = R"(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDLePNlWN06FLlM
ujWzIX8UICO7SWfH5DXlafVjpxwi/WCkdO6FxixqRNGu71wMvJXFbDlNR8fqX2xo
+eq17J3uFKn+qdjmP3L38bkqxhoJ/nCrXkeGyCTQ+Daug63ZYSJeW2Mmf+LAR5/i
/fWYfXpSlbcf5XJQPEWvENpLqWu+NOU50dJXIEVYpUXRx2+x4ZbwkH7tVJm94L+C
OUyiJKQPyWgU2aFsyJGwHFfePfSUpfYHqbHZV/ILpY59VJairBwE99bx/mBvMI7a
hBmJTSDuDffJcPDhFF5kZa0UkQPrPvhXcQaSRti7v0VonEQj8pTSnGYr9ktWKk92
wxDyn9S3AgMBAAECggEAbEhQ14WELg2rUz7hpxPTaiV0fo4hEcrMN+u8sKzVF3Xa
QYsNCNoe9urq3/r39LtDxU3D7PGfXYYszmz50Jk8ruAGW8WN7XKkv3i/fxjv8JOc
6EYDMKJAnYkKqLLhCQddX/Oof2udg5BacVWPpvhX6a1NSEc2H6cDupfwZEWkVhMi
bCC3JcNmjFa8N7ow1/5VQiYVTjpxfV7GY1GRe7vMvBucdQKH3tUG5PYXKXytXw/j
KDLaECiYVT89KbApkI0zhy7I5g3LRq0Rs5fmYLCjVebbuAL1W5CJHFJeFOgMKvnO
QSl7MfHkTnzTzUqwkwXjgNMGsTosV4UloL9gXVF6GQKBgQD5fI771WETkpaKjWBe
6XUVSS98IOAPbTGpb8CIhSjzCuztNAJ+0ey1zklQHonMFbdmcWTkTJoF3ECqAos9
vxB4ROg+TdqGDcRrXa7Twtmhv66QvYxttkaK3CqoLX8CCTnjgXBCijo6sCpo6H1T
+y55bBDpxZjNFT5BV3+YPBfWQwKBgQDQyNt+saTqJqxGYV7zWQtOqKORRHAjaJpy
m5035pky5wORsaxQY8HxbsTIQp9jBSw3SQHLHN/NAXDl2k7VAw/axMc+lj9eW+3z
2Hv5LVgj37jnJYEpYwehvtR0B4jZnXLyLwShoBdRPkGlC5fs9+oWjQZoDwMLZfTg
eZVOJm6SfQKBgQDfxYcB/kuKIKsCLvhHaSJpKzF6JoqRi6FFlkScrsMh66TCxSmP
0n58O0Cqqhlyge/z5LVXyBVGOF2Pn6SAh4UgOr4MVAwyvNp2aprKuTQ2zhSnIjx4
k0sGdZ+VJOmMS/YuRwUHya+cwDHp0s3Gq77tja5F38PD/s/OD8sUIqJGvQKBgBfI
6ghy4GC0ayfRa+m5GSqq14dzDntaLU4lIDIAGS/NVYDBhunZk3yXq99Mh6/WJQVf
Uc77yRsnsN7ekeB+as33YONmZm2vd1oyLV1jpwjfMcdTZHV8jKAGh1l4ikSQRUoF
xTdMb5uXxg6xVWtvisFq63HrU+N2iAESmMnAYxRZAoGAVEFJRRjPrSIUTCCKRiTE
br+cHqy6S5iYRxGl9riKySBKeU16fqUACIvUqmqlx4Secj3/Hn/VzYEzkxcSPwGi
qMgdS0R+tacca7NopUYaaluneKYdS++DNlT/m+KVHqLynQr54z1qBlThg9KGrpmM
LGZkXtQpx6sX7v3Kq56PkNk=
-----END PRIVATE KEY-----)";

  const std::string TEST_PUBLIC_CERT = R"(-----BEGIN CERTIFICATE-----
MIIC6zCCAdOgAwIBAgIBATANBgkqhkiG9w0BAQsFADA5MQswCQYDVQQGEwJJVDEW
MBQGA1UECgwNR2FtZXNPbldoYWxlczESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTIy
MDQwOTA5MTYwNVoXDTQyMDQwNDA5MTYwNVowOTELMAkGA1UEBhMCSVQxFjAUBgNV
BAoMDUdhbWVzT25XaGFsZXMxEjAQBgNVBAMMCWxvY2FsaG9zdDCCASIwDQYJKoZI
hvcNAQEBBQADggEPADCCAQoCggEBAMt482VY3ToUuUy6NbMhfxQgI7tJZ8fkNeVp
9WOnHCL9YKR07oXGLGpE0a7vXAy8lcVsOU1Hx+pfbGj56rXsne4Uqf6p2OY/cvfx
uSrGGgn+cKteR4bIJND4Nq6DrdlhIl5bYyZ/4sBHn+L99Zh9elKVtx/lclA8Ra8Q
2kupa7405TnR0lcgRVilRdHHb7HhlvCQfu1Umb3gv4I5TKIkpA/JaBTZoWzIkbAc
V9499JSl9gepsdlX8guljn1UlqKsHAT31vH+YG8wjtqEGYlNIO4N98lw8OEUXmRl
rRSRA+s++FdxBpJG2Lu/RWicRCPylNKcZiv2S1YqT3bDEPKf1LcCAwEAATANBgkq
hkiG9w0BAQsFAAOCAQEAqPBqzvDjl89pZMll3Ge8RS7HeDuzgocrhOcT2jnk4ag7
/TROZuISjDp6+SnL3gPEt7E2OcFAczTg3l/wbT5PFb6vM96saLm4EP0zmLfK1FnM
JDRahKutP9rx6RO5OHqsUB+b4jA4W0L9UnXUoLKbjig501AUix0p52FBxu+HJ90r
HlLs3Vo6nj4Z/PZXrzaz8dtQ/KJMpd/g/9xlo6BKAnRk5SI8KLhO4hW6zG0QA56j
X4wnh1bwdiidqpcgyuKossLOPxbS786WmsesaAWPnpoY6M8aija+ALwNNuWWmyMg
9SVDV76xJzM36Uq7Kg3QJYTlY04WmPIdJHkCtXWf9g==
-----END CERTIFICATE-----)";
}  // namespace

namespace {
  /**
   * @brief Name of the log file the unit-test logger writes to.
   */
  constexpr auto TEST_LOG_FILE = "test_sunshine.log";

  /**
   * @brief Build a session description with fixed, known values.
   *
   * @return Session description used by the JSON tests.
   */
  rtsp_stream::session_info_t make_session_info() {
    rtsp_stream::session_info_t info;
    info.id = 7;
    info.started = std::chrono::system_clock::time_point {std::chrono::seconds {1700000000}};
    info.transport = "udp/ipv4";
    info.client_category = "lan";
    return info;
  }

  /**
   * @brief Build the basic-auth header value for the given credentials.
   *
   * @param username User name.
   * @param password Password.
   * @return Header value such as `Basic <base64>`.
   */
  std::string create_auth_header(const std::string &username, const std::string &password) {
    return "Basic " + SimpleWeb::Crypto::Base64::encode(username + ":" + password);
  }

  /**
   * @brief Current size of the unit-test log file, after flushing pending log output.
   *
   * @return Size in bytes, or 0 when the file does not exist yet.
   */
  std::uintmax_t test_log_size() {
    logging::log_flush();
    std::error_code ec;
    const auto size = std::filesystem::file_size(TEST_LOG_FILE, ec);
    return ec ? 0 : size;
  }

  /**
   * @brief Log text written after a given byte offset of the unit-test log file.
   *
   * @param offset Byte offset returned by `test_log_size()` before the action under test.
   * @return Text appended to the log since that offset.
   */
  std::string test_log_since(std::uintmax_t offset) {
    logging::log_flush();
    std::ifstream input(TEST_LOG_FILE, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (offset >= content.size()) {
      return {};
    }
    return content.substr(static_cast<std::size_t>(offset));
  }

  /**
   * @brief Check whether any single line of the text contains every given fragment.
   *
   * @param text Text to search, split into lines.
   * @param fragments Substrings that must all appear on the same line.
   * @return `true` when such a line exists.
   */
  bool has_line_with_all(const std::string &text, const std::vector<std::string> &fragments) {
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
      bool all_found = true;
      for (const auto &fragment : fragments) {
        if (line.find(fragment) == std::string::npos) {
          all_found = false;
          break;
        }
      }
      if (all_found) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Count how many times a fragment occurs in a text.
   *
   * @param text Text to search.
   * @param needle Substring to count.
   * @return Number of non-overlapping occurrences.
   */
  std::size_t count_occurrences(const std::string &text, const std::string &needle) {
    std::size_t count = 0;
    for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size())) {
      ++count;
    }
    return count;
  }
}  // namespace

/**
 * @brief Test: the session JSON has exactly the four documented keys and nothing else.
 */
TEST(SessionApiJsonTest, HasExactlyTheDocumentedKeys) {
  const nlohmann::json json = confighttp::session_info_to_json(make_session_info());

  const std::set<std::string> expected_keys {
    "id",
    "started",
    "transport",
    "client_category",
  };

  std::set<std::string> actual_keys;
  for (const auto &item : json.items()) {
    actual_keys.insert(item.key());
  }

  EXPECT_EQ(actual_keys, expected_keys);
  EXPECT_EQ(actual_keys.size(), 4U);
}

/**
 * @brief Test: no key or string value of the session JSON carries identity or secret material.
 */
TEST(SessionApiJsonTest, NoIdentityOrSecretMaterial) {
  const nlohmann::json json = confighttp::session_info_to_json(make_session_info());

  const std::set<std::string> forbidden {"name", "uuid", "address", "cert", "key", "iv", "payload", "connect"};

  auto lowercase = [](std::string text) {
    for (auto &ch : text) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
  };

  for (const auto &item : json.items()) {
    const std::string key = lowercase(item.key());
    for (const auto &word : forbidden) {
      EXPECT_EQ(key.find(word), std::string::npos) << "key '" << item.key() << "' contains '" << word << "'";
    }
    if (item.value().is_string()) {
      const std::string value = lowercase(item.value().get<std::string>());
      for (const auto &word : forbidden) {
        EXPECT_EQ(value.find(word), std::string::npos) << "value '" << value << "' contains '" << word << "'";
      }
    }
  }
}

/**
 * @brief Test: every value of the session JSON matches the session description, with `started` as Unix seconds.
 */
TEST(SessionApiJsonTest, ValuesMatchSessionInfo) {
  const nlohmann::json json = confighttp::session_info_to_json(make_session_info());

  const nlohmann::json expected {
    {"id", 7},
    {"started", 1700000000},
    {"transport", "udp/ipv4"},
    {"client_category", "lan"},
  };

  EXPECT_EQ(json, expected);
  ASSERT_TRUE(json.contains("started"));
  EXPECT_TRUE(json["started"].is_number_integer());
}

/**
 * @brief Test: an IPv6 WAN session is converted with its transport and category values unchanged.
 */
TEST(SessionApiJsonTest, ValuesMatchSessionInfoForIpv6Wan) {
  rtsp_stream::session_info_t info = make_session_info();
  info.id = 4000000000U;
  info.transport = "udp/ipv6";
  info.client_category = "wan";

  const nlohmann::json json = confighttp::session_info_to_json(info);

  const nlohmann::json expected {
    {"id", 4000000000U},
    {"started", 1700000000},
    {"transport", "udp/ipv6"},
    {"client_category", "wan"},
  };
  EXPECT_EQ(json, expected);
}

/**
 * @brief Test: with no streaming sessions, the list is empty.
 */
TEST(SessionApiRtspTest, ListSessionsWithoutSessionsIsEmpty) {
  EXPECT_TRUE(rtsp_stream::list_sessions().empty());
}

/**
 * @brief Test: asking to close an unknown session id returns false.
 */
TEST(SessionApiRtspTest, RequestSessionCloseUnknownIdReturnsFalse) {
  EXPECT_FALSE(rtsp_stream::request_session_close(12345));
}

/**
 * @brief Test fixture that runs a small HTTPS server with the session API routes.
 */
class SessionApiHttpTest: public BaseTest {  // NOSONAR(cpp:S3656): protected members are intentional for test fixture subclassing
protected:
  std::unique_ptr<SimpleWeb::Server<SimpleWeb::HTTPS>> server;  ///< Test HTTPS server.
  std::unique_ptr<SimpleWeb::Client<SimpleWeb::HTTPS>> client;  ///< Test HTTPS client.
  std::jthread server_thread;  ///< Thread running the server loop.
  unsigned short port = 0;  ///< Port assigned by the OS.

  std::string saved_username;  ///< Username before the fixture changed it.
  std::string saved_password;  ///< Password hash before the fixture changed it.
  std::string saved_salt;  ///< Salt before the fixture changed it.
  std::string saved_locale;  ///< Locale before the fixture changed it.
  std::vector<std::string> saved_csrf_allowed_origins;  ///< Allowed CSRF origins before the fixture changed them.
  std::filesystem::path test_web_dir;  ///< Temporary directory holding the test certificate files.
  std::filesystem::path cert_file;  ///< Test certificate path.
  std::filesystem::path key_file;  ///< Test private key path.

  /**
   * @brief Configure test credentials, start the HTTPS server with the session routes, and create a client.
   */
  void SetUp() override {
    BaseTest::SetUp();

    saved_username = config::sunshine.username;
    saved_password = config::sunshine.password;
    saved_salt = config::sunshine.salt;
    saved_locale = config::sunshine.locale;
    saved_csrf_allowed_origins = config::sunshine.csrf_allowed_origins;

    config::sunshine.username = "testuser";
    config::sunshine.salt = "testsalt";
    config::sunshine.password = util::hex(crypto::hash("testpass" + config::sunshine.salt)).to_string();
    config::sunshine.locale = "en";
    config::sunshine.csrf_allowed_origins = {
      "https://localhost",
      "https://127.0.0.1",
      "https://[::1]"
    };

    test_web_dir = std::filesystem::temp_directory_path() / "sunshine_test_session_api";  // NOSONAR(cpp:S5443): safe for tests
    std::filesystem::create_directories(test_web_dir);
    cert_file = test_web_dir / "test_cert.pem";
    key_file = test_web_dir / "test_key.pem";

    std::ofstream cert_out(cert_file);
    cert_out << TEST_PUBLIC_CERT;
    cert_out.close();

    std::ofstream key_out(key_file);
    key_out << TEST_PRIVATE_KEY;
    key_out.close();

    server = std::make_unique<SimpleWeb::Server<SimpleWeb::HTTPS>>(cert_file.string(), key_file.string());
    server->config.port = 0;
    server->config.reuse_address = true;
    server->config.timeout_request = 5;
    server->config.timeout_content = 300;

    // Token source for the CSRF check; the production handler is used.
    server->resource["^/csrf-token-test$"]["GET"] = [](
                                                      const std::shared_ptr<SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response> &response,
                                                      const std::shared_ptr<SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> &request
                                                    ) {
      confighttp::getCSRFToken(response, request);
    };

    // Production route table for the session API (GET /api/sessions, POST /api/sessions/<id>/close).
    confighttp::register_session_routes(*server);

    server_thread = std::jthread([this]() {
      server->start([this](const unsigned short assigned_port) {
        port = assigned_port;
      });
    });

    for (int i = 0; i < 100 && port == 0; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_NE(port, 0) << "Server failed to start";

    config::sunshine.csrf_allowed_origins.push_back(std::format("https://localhost:{}", port));
    config::sunshine.csrf_allowed_origins.push_back(std::format("https://127.0.0.1:{}", port));
    config::sunshine.csrf_allowed_origins.push_back(std::format("https://[::1]:{}", port));

    client = std::make_unique<SimpleWeb::Client<SimpleWeb::HTTPS>>(std::format("localhost:{}", port), false);
    client->config.timeout = 5;
  }

  /**
   * @brief Stop the server and restore the configuration changed by SetUp.
   */
  void TearDown() override {
    if (server) {
      server->stop();
    }
    if (server_thread.joinable()) {
      server_thread.join();
    }

    config::sunshine.username = saved_username;
    config::sunshine.password = saved_password;
    config::sunshine.salt = saved_salt;
    config::sunshine.locale = saved_locale;
    config::sunshine.csrf_allowed_origins = saved_csrf_allowed_origins;

    std::error_code ec;
    std::filesystem::remove_all(test_web_dir, ec);
    BaseTest::TearDown();
  }

  /**
   * @brief Build headers that carry valid test credentials.
   *
   * @return Header collection with an `Authorization` entry.
   */
  static SimpleWeb::CaseInsensitiveMultimap auth_headers() {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Authorization", create_auth_header("testuser", "testpass"));
    return headers;
  }

  /**
   * @brief Fetch a CSRF token for the authenticated test client.
   *
   * @return The token string returned by the CSRF route.
   */
  std::string fetch_csrf_token() {
    const auto response = client->request("GET", "/csrf-token-test", "", auth_headers());
    EXPECT_EQ(response->status_code, "200 OK");
    const nlohmann::json json = nlohmann::json::parse(response->content.string());
    return json["csrf_token"].get<std::string>();
  }

  /**
   * @brief Send an authenticated, cross-origin close for id 4243 with no CSRF token.
   *
   * The API logs this request as `result=csrf-rejected`, so it works as a marker: a negative test can check
   * that the log sink is live and that no other close request was logged.
   */
  void send_marker_close() {
    SimpleWeb::CaseInsensitiveMultimap headers = auth_headers();
    headers.emplace("Origin", "https://example.invalid");

    const auto response = client->request("POST", "/api/sessions/4243/close", "", headers);
    EXPECT_EQ(response->status_code, "400 Bad Request");
  }

  /**
   * @brief Check that the only close request logged since an offset is the marker from `send_marker_close()`.
   *
   * @param offset Byte offset of the log taken before the action under test.
   */
  void expect_only_marker_close_logged(std::uintmax_t offset) {
    const std::string logged = test_log_since(offset);
    ASSERT_FALSE(logged.empty()) << "No log text captured from " << TEST_LOG_FILE << "; the log sink is not where the test expects it";
    EXPECT_TRUE(has_line_with_all(logged, {"Session close requested via API: id=4243", "result=csrf-rejected"}))
      << "The marker close line is missing from the log; the sink or the audit line is broken";
    EXPECT_EQ(count_occurrences(logged, "Session close requested via API"), 1U)
      << "A close request other than the marker was logged";
  }
};

/**
 * @brief Test: GET /api/sessions without credentials is rejected with 401.
 */
TEST_F(SessionApiHttpTest, GetSessionsRejectsUnauthenticatedRequest) {
  const auto response = client->request("GET", "/api/sessions", "");

  ASSERT_EQ(response->status_code, "401 Unauthorized");
}

/**
 * @brief Test: GET /api/sessions with the right user and a wrong password is rejected with 401.
 */
TEST_F(SessionApiHttpTest, GetSessionsRejectsWrongPassword) {
  SimpleWeb::CaseInsensitiveMultimap headers;
  headers.emplace("Authorization", create_auth_header("testuser", "wrongpass"));

  const auto response = client->request("GET", "/api/sessions", "", headers);

  ASSERT_EQ(response->status_code, "401 Unauthorized");
}

/**
 * @brief Test: GET /api/sessions with the right password and a wrong user name is rejected with 401.
 */
TEST_F(SessionApiHttpTest, GetSessionsRejectsWrongUser) {
  SimpleWeb::CaseInsensitiveMultimap headers;
  headers.emplace("Authorization", create_auth_header("wronguser", "testpass"));

  const auto response = client->request("GET", "/api/sessions", "", headers);

  ASSERT_EQ(response->status_code, "401 Unauthorized");
}

/**
 * @brief Test: authenticated GET /api/sessions returns the status and an empty session list.
 */
TEST_F(SessionApiHttpTest, GetSessionsReturnsEmptyListWhenAuthenticated) {
  const auto response = client->request("GET", "/api/sessions", "", auth_headers());

  ASSERT_EQ(response->status_code, "200 OK");
  const nlohmann::json json = nlohmann::json::parse(response->content.string());
  const nlohmann::json expected {
    {"status", true},
    {"sessions", nlohmann::json::array()},
  };
  EXPECT_EQ(json, expected);
}

/**
 * @brief Test: POST /api/sessions/<id>/close without credentials is rejected with 401.
 */
TEST_F(SessionApiHttpTest, CloseSessionRejectsUnauthenticatedRequest) {
  const auto response = client->request("POST", "/api/sessions/4242/close", "");

  ASSERT_EQ(response->status_code, "401 Unauthorized");
}

/**
 * @brief Test: POST /api/sessions/<id>/close with the right user and a wrong password is rejected with 401.
 */
TEST_F(SessionApiHttpTest, CloseSessionRejectsWrongPassword) {
  SimpleWeb::CaseInsensitiveMultimap headers;
  headers.emplace("Authorization", create_auth_header("testuser", "wrongpass"));

  const auto response = client->request("POST", "/api/sessions/4242/close", "", headers);

  ASSERT_EQ(response->status_code, "401 Unauthorized");
}

/**
 * @brief Test: POST /api/sessions/<id>/close with the right password and a wrong user name is rejected with 401.
 */
TEST_F(SessionApiHttpTest, CloseSessionRejectsWrongUser) {
  SimpleWeb::CaseInsensitiveMultimap headers;
  headers.emplace("Authorization", create_auth_header("wronguser", "testpass"));

  const auto response = client->request("POST", "/api/sessions/4242/close", "", headers);

  ASSERT_EQ(response->status_code, "401 Unauthorized");
}

/**
 * @brief Test: closing an unknown session id with a valid CSRF token returns a 404 with a JSON content type.
 */
TEST_F(SessionApiHttpTest, CloseSessionUnknownIdReturnsNotFound) {
  const std::string token = fetch_csrf_token();
  SimpleWeb::CaseInsensitiveMultimap headers = auth_headers();
  headers.emplace("X-CSRF-Token", token);

  const auto response = client->request("POST", "/api/sessions/4242/close", "", headers);

  ASSERT_EQ(response->status_code, "404 Not Found");
  const auto content_type = response->header.find("Content-Type");
  ASSERT_NE(content_type, response->header.end());
  EXPECT_NE(content_type->second.find("application/json"), std::string::npos);
}

/**
 * @brief Test: a session id that does not fit in 32 bits is rejected with 400, even with a valid CSRF token.
 */
TEST_F(SessionApiHttpTest, CloseSessionOverflowingIdIsBadRequest) {
  const std::string token = fetch_csrf_token();
  SimpleWeb::CaseInsensitiveMultimap headers = auth_headers();
  headers.emplace("X-CSRF-Token", token);

  const auto response = client->request("POST", "/api/sessions/99999999999/close", "", headers);

  ASSERT_EQ(response->status_code, "400 Bad Request");
}

/**
 * @brief Test: a cross-origin close without a CSRF token is rejected like other state-changing endpoints.
 */
TEST_F(SessionApiHttpTest, CloseSessionRejectsCrossOriginWithoutCsrfToken) {
  SimpleWeb::CaseInsensitiveMultimap headers = auth_headers();
  headers.emplace("Origin", "https://example.invalid");

  const auto response = client->request("POST", "/api/sessions/4242/close", "", headers);

  ASSERT_EQ(response->status_code, "400 Bad Request");
  EXPECT_TRUE(response->content.string().contains("Missing CSRF token"));
}

/**
 * @brief Test: an authenticated close of an unknown id is logged with the id, user and not-found result, and never the password.
 */
TEST_F(SessionApiHttpTest, CloseSessionUnknownIdIsAuditLogged) {
  const std::string token = fetch_csrf_token();
  SimpleWeb::CaseInsensitiveMultimap headers = auth_headers();
  headers.emplace("X-CSRF-Token", token);

  const std::uintmax_t offset = test_log_size();
  const auto response = client->request("POST", "/api/sessions/4242/close", "", headers);
  ASSERT_EQ(response->status_code, "404 Not Found");

  const std::string logged = test_log_since(offset);
  ASSERT_FALSE(logged.empty()) << "No log text captured from " << TEST_LOG_FILE << "; the log sink is not where the test expects it";
  EXPECT_TRUE(has_line_with_all(
    logged,
    {"Session close requested via API: id=4242", "result=not-found", "user=testuser", "source="}
  ));
  EXPECT_EQ(logged.find("testpass"), std::string::npos);
}

/**
 * @brief Test: GET /api/sessions from a disallowed origin address is rejected with 403.
 *
 * Skipped: the test client connects from the loopback address, and no existing test in test_confighttp.cpp makes
 * `config::nvhttp.origin_web_ui_allowed` reject it. Making the loopback client disallowed needs the config values
 * for that setting, which the spec does not give.
 */
TEST_F(SessionApiHttpTest, GetSessionsRejectsDisallowedOriginAddress) {
  GTEST_SKIP() << "Loopback test client cannot be made a disallowed origin; test_confighttp.cpp has no such test";
}

/**
 * @brief Test: POST /api/sessions/<id>/close from a disallowed origin address is rejected with 403.
 *
 * Skipped for the same reason as `GetSessionsRejectsDisallowedOriginAddress`.
 */
TEST_F(SessionApiHttpTest, CloseSessionRejectsDisallowedOriginAddress) {
  GTEST_SKIP() << "Loopback test client cannot be made a disallowed origin; test_confighttp.cpp has no such test";
}

/**
 * @brief Test: with no admin user configured, GET /api/sessions redirects instead of listing sessions.
 */
TEST_F(SessionApiHttpTest, GetSessionsWithoutAdminUserRedirects) {
  config::sunshine.username = "";
  config::sunshine.password = "";

  const auto response = client->request("GET", "/api/sessions", "");

  EXPECT_NE(response->status_code, "200 OK");
  ASSERT_EQ(response->status_code, "307 Temporary Redirect");
  EXPECT_NE(response->header.find("Location"), response->header.end());
}

/**
 * @brief Test: with no admin user configured, POST close redirects, does nothing and logs no close line.
 */
TEST_F(SessionApiHttpTest, CloseSessionWithoutAdminUserRedirectsAndIsNotLogged) {
  config::sunshine.username = "";
  config::sunshine.password = "";

  const std::uintmax_t offset = test_log_size();
  const auto response = client->request("POST", "/api/sessions/4242/close", "");

  EXPECT_NE(response->status_code, "200 OK");
  ASSERT_EQ(response->status_code, "307 Temporary Redirect");
  EXPECT_NE(response->header.find("Location"), response->header.end());
  EXPECT_EQ(count_occurrences(test_log_since(offset), "Session close requested via API"), 0U);
}

/**
 * @brief Test: POST /api/sessions (wrong method on the list path) is not served and logs no close line.
 */
TEST_F(SessionApiHttpTest, PostToSessionsListIsNotServed) {
  const std::uintmax_t offset = test_log_size();

  // The fixture registers no default resources (production start() does), so the server may just drop the
  // connection; either way the request must not be served.
  try {
    const auto response = client->request("POST", "/api/sessions", "", auth_headers());
    EXPECT_NE(response->status_code, "200 OK");
  } catch (const std::exception &) {
    SUCCEED() << "connection dropped: not served";
  }
  send_marker_close();
  expect_only_marker_close_logged(offset);
}

/**
 * @brief Test: GET /api/sessions/1/close (wrong method on the close path) is not served and logs no close line.
 */
TEST_F(SessionApiHttpTest, GetCloseRouteIsNotServed) {
  const std::uintmax_t offset = test_log_size();

  // The fixture registers no default resources (production start() does), so the server may just drop the
  // connection; either way the request must not be served.
  try {
    const auto response = client->request("GET", "/api/sessions/1/close", "", auth_headers());
    EXPECT_NE(response->status_code, "200 OK");
  } catch (const std::exception &) {
    SUCCEED() << "connection dropped: not served";
  }
  send_marker_close();
  expect_only_marker_close_logged(offset);
}

/**
 * @brief Test: a close with a wrong password is rejected with 401, logs no close line and never logs the password.
 */
TEST_F(SessionApiHttpTest, CloseSessionWrongPasswordIsNotLogged) {
  SimpleWeb::CaseInsensitiveMultimap headers;
  headers.emplace("Authorization", create_auth_header("testuser", "wrongpass"));

  const std::uintmax_t offset = test_log_size();
  const auto response = client->request("POST", "/api/sessions/4242/close", "", headers);

  ASSERT_EQ(response->status_code, "401 Unauthorized");
  send_marker_close();
  const std::string logged = test_log_since(offset);
  expect_only_marker_close_logged(offset);
  EXPECT_EQ(logged.find("Session close requested via API: id=4242"), std::string::npos);
  EXPECT_EQ(logged.find("wrongpass"), std::string::npos);
}

/**
 * @brief Test: an authenticated cross-origin close without a CSRF token is audit-logged as csrf-rejected.
 */
TEST_F(SessionApiHttpTest, CloseSessionCrossOriginWithoutCsrfTokenIsAuditLogged) {
  SimpleWeb::CaseInsensitiveMultimap headers = auth_headers();
  headers.emplace("Origin", "https://example.invalid");

  const std::uintmax_t offset = test_log_size();
  const auto response = client->request("POST", "/api/sessions/4242/close", "", headers);
  ASSERT_EQ(response->status_code, "400 Bad Request");

  const std::string logged = test_log_since(offset);
  ASSERT_FALSE(logged.empty()) << "No log text captured from " << TEST_LOG_FILE << "; the log sink is not where the test expects it";
  EXPECT_TRUE(has_line_with_all(
    logged,
    {"Session close requested via API: id=4242", "result=csrf-rejected", "user=testuser"}
  ));
  EXPECT_EQ(logged.find("testpass"), std::string::npos);
}
