/**
 * @file src/confighttp.h
 * @brief Declarations for the Web UI Config HTTP server.
 */
#pragma once

// standard includes
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

// lib includes
#include <libvirtualhid/license.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#ifdef __linux__
  #include "handoff/listener.h"
#endif
#include "rtsp.h"
#include "thread_safe.h"

/**
 * @def WEB_DIR
 * @brief Macro for WEB DIR.
 */
#define WEB_DIR SUNSHINE_ASSETS_DIR "/web/"

namespace confighttp {
  constexpr auto PORT_HTTPS = 1;  ///< GameStream port offset for port https.

#ifdef __linux__
  /**
   * @brief HTTPS server type of the Web UI, as the handoff needs it (Linux).
   *
   * `handoff::adoptable_server_t<SimpleWeb::Server<SimpleWeb::HTTPS>>` (see `handoff/listener.h`). Contract of the
   * swap: `https_server_t` below becomes this type on Linux and the duplicate alias in `confighttp.cpp` is removed.
   * `resp_https_t` and `req_https_t` go through `SimpleWeb::ServerBase<SimpleWeb::HTTPS>` and do not change, and
   * `register_session_routes(https_server_t &)` keeps its signature. An unadopted `start()` is unchanged.
   */
  using adoptable_https_server_t = handoff::adoptable_server_t<SimpleWeb::Server<SimpleWeb::HTTPS>>;
#else
  /**
   * @brief HTTPS server type of the Web UI (non-Linux builds keep the upstream type).
   */
  using adoptable_https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;
#endif

  // Type aliases for HTTPS server components
  using https_server_t = adoptable_https_server_t;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  // Main server start function
  void start();

  void print_req(const req_https_t &request);
  void send_response(const resp_https_t &response, const nlohmann::json &output_tree);
  void send_unauthorized(const resp_https_t &response, const req_https_t &request);
  void send_redirect(const resp_https_t &response, const req_https_t &request, const char *path);
  /**
   * @brief Authenticate the admin user with HTTP Basic credentials.
   *
   * A malformed or non-Basic `Authorization` header, or a wrong credential, is a failed attempt. Failed attempts
   * are counted per source address; a source over the limit gets 429 without an authentication check. A request
   * without an `Authorization` header is answered with 401 but not counted.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   * @return True if the user is authenticated, false otherwise.
   */
  bool authenticate(const resp_https_t &response, const req_https_t &request);
  void not_found(const resp_https_t &response, const req_https_t &request, const std::string &error_message = "Not Found");
  void bad_request(const resp_https_t &response, const req_https_t &request, const std::string &error_message = "Bad Request");
  /**
   * @brief Check content type.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   * @param contentType Expected HTTP content type.
   * @return True when the request passes validation and processing may continue.
   */
  bool check_content_type(const resp_https_t &response, const req_https_t &request, const std::string_view &contentType);
  std::string generate_csrf_token(const std::string &client_id);
  /**
   * @brief Validate CSRF token.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   * @param client_id Client identifier used to look up the CSRF token.
   * @return True when the request passes validation and processing may continue.
   */
  bool validate_csrf_token(const resp_https_t &response, const req_https_t &request, const std::string &client_id);
  std::string get_client_id(const req_https_t &request);
  /**
   * @brief Check app index.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   * @param index Zero-based index of the item being addressed.
   * @return True when the request passes validation and processing may continue.
   */
  bool check_app_index(const resp_https_t &response, const req_https_t &request, int index);
  void getPage(const resp_https_t &response, const req_https_t &request, const char *html_file, bool require_auth = true, bool redirect_if_username = false);
  void getAsset(const resp_https_t &response, const req_https_t &request);
  void browseDirectory(const resp_https_t &response, const req_https_t &request);
  void getLocale(const resp_https_t &response, const req_https_t &request);
  void getCSRFToken(const resp_https_t &response, const req_https_t &request);

  /**
   * @brief List the active streaming sessions.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void getSessions(const resp_https_t &response, const req_https_t &request);

  /**
   * @brief Close one streaming session by id (`POST /api/sessions/<id>/close`).
   *
   * Requires admin authentication and a CSRF token. Every authenticated request is logged at Info with the
   * session id, the authenticated user, the source address and the result (`closed`, `already-stopping`,
   * `not-found`, `csrf-rejected` or `bad-id`). A session that is already stopping, or whose stop another request
   * just won, gets 200 like `closed`.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client; `path_match[1]` is the session id.
   */
  void closeSession(const resp_https_t &response, const req_https_t &request);

  /**
   * @brief Register the session API routes (`GET /api/sessions`, `POST /api/sessions/<id>/close`) on the admin server.
   *
   * `start()` uses this; tests use it to exercise the production route table.
   *
   * @param server Admin web UI HTTPS server.
   */
  void register_session_routes(https_server_t &server);

  /**
   * @brief Convert a session description to the JSON object returned by `GET /api/sessions`.
   *
   * @param info Session description.
   * @return Object with exactly `id`, `started` (Unix seconds), `transport` and `client_category`.
   */
  nlohmann::json session_info_to_json(const rtsp_stream::session_info_t &info);

  /**
   * @brief List authenticated pairing requests awaiting operator approval.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void getPendingPairings(const resp_https_t &response, const req_https_t &request);

  /**
   * @brief Cancel an explicitly selected pairing request.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void cancelPairing(const resp_https_t &response, const req_https_t &request);

  /**
   * @brief Apply a PIN to an explicitly selected pairing request.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void savePin(const resp_https_t &response, const req_https_t &request);

  /**
   * @brief Check whether a detected driver version identifies a development build.
   *
   * Numeric versions beginning with `0.0` and containing at least three
   * components are development builds.
   *
   * @param version Detected driver version.
   * @return True when the version identifies a development build.
   */
  bool is_driver_version_development(std::string_view version);

  /**
   * @brief Check whether a detected driver version satisfies a minimum version.
   *
   * Empty minimum versions accept any detected version. Non-empty minimum versions
   * require a fully numeric dotted version string. Development versions beginning
   * with `0.0` are always accepted.
   *
   * @param version Detected driver version.
   * @param minimum_version Minimum supported driver version, or empty for any version.
   * @return True when the driver version is supported.
   */
  bool is_driver_version_supported(std::string_view version, std::string_view minimum_version);

  /**
   * @brief Build a standard driver status response.
   *
   * @param installed Whether the driver was detected.
   * @param version Detected driver version.
   * @param minimum_version Minimum supported driver version, or empty for any version.
   * @return Driver status JSON object.
   */
  nlohmann::json build_driver_status(bool installed, const std::string &version, std::string_view minimum_version);

  /**
   * @brief Convert a libvirtualhid license result into a Web UI response.
   *
   * @param result License operation result.
   * @return License status JSON object without the submitted license key.
   */
  nlohmann::json build_virtualhid_license_status(const lvh::LicenseResult &result);

  /**
   * @brief Build libvirtualhid driver version and installation status.
   *
   * @return libvirtualhid driver status JSON.
   */
  nlohmann::json get_virtualhid_driver_status();

  /**
   * @brief Build ViGEmBus fallback driver version and installation status.
   *
   * @return ViGEmBus fallback driver status JSON.
   */
  nlohmann::json get_vigembus_driver_status();

  void getVirtualInputStatus(const resp_https_t &response, const req_https_t &request);

  void getVirtualInputLicense(const resp_https_t &response, const req_https_t &request);

  void updateVirtualInputLicense(const resp_https_t &response, const req_https_t &request);

  void resetPortalToken(const resp_https_t &response, const req_https_t &request);

#ifdef SUNSHINE_TESTS
  using virtual_input_license_status_provider_t = std::function<lvh::LicenseResult()>;  ///< Test provider for current libvirtualhid license status.
  using portal_token_path_provider_t = std::function<std::filesystem::path()>;  ///< Test provider for the XDG Portal token path.

  /**
   * @brief Replace the virtual-input license status provider for unit tests.
   *
   * @param status_provider Provider returning the license status for the response.
   */
  void set_virtual_input_license_status_provider_for_testing(virtual_input_license_status_provider_t status_provider);

  /**
   * @brief Restore the production virtual-input license status provider after a unit test.
   */
  void reset_virtual_input_license_status_provider_for_testing();

  /**
   * @brief Replace the XDG Portal token path provider for unit tests.
   *
   * @param path_provider Provider returning the token path used by the reset endpoint.
   */
  void set_portal_token_path_provider_for_testing(portal_token_path_provider_t path_provider);

  /**
   * @brief Restore the production XDG Portal token path provider after a unit test.
   */
  void reset_portal_token_path_provider_for_testing();

  /**
   * @brief Exercise request-local sensitive string clearing for unit tests.
   *
   * @param value Mutable sensitive string to overwrite and clear.
   */
  void clear_sensitive_string_for_testing(std::string &value);

  namespace testing {
    /**
     * @brief Enable or disable the failed-login rate limit used by `authenticate()`.
     *
     * The limit starts disabled in test builds so that tests with wrong credentials do not lock each other out.
     * Enabling or disabling it also clears every tracked source.
     *
     * @param enabled True to enforce the failed-login rate limit, false to skip it.
     */
    void set_auth_rate_limit_enabled(bool enabled);
  }  // namespace testing
#endif

  // Browse helper functions (also exposed for unit testing)
  /**
   * @brief Checks whether a directory entry qualifies as an executable file.
   * @param entry The directory entry to check.
   * @param status The cached file status for the entry.
   * @return True if the file should be included in an executable-type listing.
   */
  bool is_browsable_executable(const std::filesystem::directory_entry &entry, const std::filesystem::file_status &status);

  /**
   * @brief Lists, filters, and sorts the entries of a directory for the browse API.
   * @param dir_path The directory to list.
   * @param type_str Filter type: "directory", "executable", "file", or "any".
   * @return Sorted JSON array of entry objects with name/type/path fields.
   */
  nlohmann::json build_browse_entries(const std::filesystem::path &dir_path, const std::string &type_str);

#ifdef _WIN32
  /**
   * @brief Builds a JSON array of available Windows drive letters.
   * @return JSON array of drive-letter entries.
   */
  nlohmann::json get_windows_drives();
#endif
}  // namespace confighttp

// mime types map
/**
 * @brief File-extension to MIME-type mapping used when serving the Web UI.
 */
const std::map<std::string, std::string> mime_types = {
  {"css", "text/css"},
  {"gif", "image/gif"},
  {"htm", "text/html"},
  {"html", "text/html"},
  {"ico", "image/x-icon"},
  {"jpeg", "image/jpeg"},
  {"jpg", "image/jpeg"},
  {"js", "application/javascript"},
  {"json", "application/json"},
  {"png", "image/png"},
  {"svg", "image/svg+xml"},
  {"ttf", "font/ttf"},
  {"txt", "text/plain"},
  {"woff2", "font/woff2"},
  {"xml", "text/xml"},
};
