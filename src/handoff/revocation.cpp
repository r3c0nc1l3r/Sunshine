/**
 * @file src/handoff/revocation.cpp
 * @brief Definitions for `src/handoff/revocation.h`.
 */
// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "revocation.h"

namespace handoff {
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
      if (devices_it == root_it->end() || !devices_it->is_array()) {
        return std::nullopt;
      }

      std::set<std::string> disabled;
      for (const auto &device : *devices_it) {
        // Entries that are not objects, or have no string certificate, cannot be matched and are skipped.
        if (!device.is_object()) {
          continue;
        }

        const auto cert_it = device.find("cert");
        if (cert_it == device.end() || !cert_it->is_string()) {
          continue;
        }

        // A missing `enabled` means enabled; only an explicit `false` disables the client.
        const auto enabled_it = device.find("enabled");
        const bool enabled = enabled_it == device.end() || !enabled_it->is_boolean() || enabled_it->get<bool>();
        if (!enabled) {
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
