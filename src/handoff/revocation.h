/**
 * @file src/handoff/revocation.h
 * @brief The draining process's revocation backstop: disabled clients in a rewritten `sunshine_state.json`.
 *
 * See `docs/drain-handoff.md`, "Visibility and control" (revoking a client).
 */
#pragma once

// standard includes
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace handoff {
  /**
   * @brief Certificates of clients disabled in a state file.
   *
   * Reads `root.named_devices[]` (each with `cert` (PEM) and `enabled`). Sunshine writes the file with boost
   * `ptree::write_json`, which stores every scalar as a string, so `enabled` is normally `"true"` or `"false"`, and an
   * empty `named_devices` is written as `""`, which is read as an empty array (an empty set, no Warning).
   *
   * - Enabled: `enabled` missing, `true`, `"true"` or `"1"`.
   * - Disabled (returned): `false`, `"false"` or `"0"`.
   * - Any other value (another string, a number, `null`, an object or array): disabled, fail closed, with a Warning
   *   naming the value's JSON type (never the certificate).
   *
   * A client missing from the file is not returned. An entry without a string `cert` is skipped with a Warning.
   *
   * @param state_json Content of `sunshine_state.json`.
   * @return The PEM certificates of disabled clients, or `std::nullopt` when the content is not valid JSON or has no
   *         parseable `root.named_devices` array (the caller then ends no session, logs a Warning and retries).
   */
  std::optional<std::set<std::string>> disabled_client_certs(std::string_view state_json);
}  // namespace handoff
