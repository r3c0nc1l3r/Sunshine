/**
 * @file src/handoff/revocation.cpp
 * @brief Definitions for `src/handoff/revocation.h`.
 */
// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "revocation.h"
#include "src/logging.h"

namespace handoff {
  /**
   * @brief Interpret the `enabled` value of one named device.
   *
   * Missing, `true`, `"true"` or `"1"` mean enabled. `false`, `"false"` or `"0"` mean disabled. Any other value is
   * treated as disabled (fail closed) and logged as a Warning that names only the JSON type.
   *
   * @param device The named device object (already known to hold a string `cert`).
   * @return `true` when the client is enabled.
   */
  static bool is_enabled(const nlohmann::json &device) {
    const auto enabled_it = device.find("enabled");
    if (enabled_it == device.end()) {
      return true;
    }

    const auto &value = *enabled_it;
    if (value.is_boolean()) {
      return value.get<bool>();
    }
    if (value.is_string()) {
      const auto text = value.get<std::string>();
      if (text == "true" || text == "1") {
        return true;
      }
      if (text == "false" || text == "0") {
        return false;
      }
    }

    // Fail closed: an unknown value disables the client. Log the type only, never the certificate.
    BOOST_LOG(warning) << "Drain handoff: client enabled value of JSON type " << value.type_name() << " treated as disabled";
    return false;
  }

  std::optional<std::set<std::string>> disabled_client_certs(std::string_view state_json) {
    // The parser must never throw out of this function, so every failure becomes std::nullopt.
    try {
      // Parse without exceptions; a parse error yields a discarded value instead of throwing.
      auto root = nlohmann::json::parse(std::string(state_json), nullptr, false);
      if (root.is_discarded() || !root.is_object()) {
        return std::nullopt;
      }

      const auto root_it = root.find("root");
      if (root_it == root.end() || !root_it->is_object()) {
        return std::nullopt;
      }

      const auto devices_it = root_it->find("named_devices");
      if (devices_it == root_it->end()) {
        return std::nullopt;
      }

      // boost ptree::write_json writes an empty array as the empty string, which is an empty list.
      if (devices_it->is_string() && devices_it->get<std::string>().empty()) {
        return std::set<std::string> {};
      }
      if (!devices_it->is_array()) {
        return std::nullopt;
      }

      std::set<std::string> disabled;
      for (const auto &device : *devices_it) {
        // An entry without a string certificate cannot be matched; skip it. Log the type only.
        const auto cert_it = device.is_object() ? device.find("cert") : device.end();
        if (cert_it == device.end() || !cert_it->is_string()) {
          BOOST_LOG(warning) << "Drain handoff: skipped a named device without a string certificate";
          continue;
        }

        if (!is_enabled(device)) {
          disabled.insert(cert_it->get<std::string>());
        }
      }

      return disabled;
    } catch (...) {
      // Any other failure (for example an allocation error) also counts as an unreadable file.
      return std::nullopt;
    }
  }
}  // namespace handoff
