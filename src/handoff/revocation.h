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
   * Reads `root.named_devices[]` (each with `cert` (PEM) and `enabled`, which defaults to `true` when missing). Only
   * entries with `enabled` explicitly `false` are returned. A client missing from the file is not returned.
   *
   * @param state_json Content of `sunshine_state.json`.
   * @return The PEM certificates of disabled clients, or `std::nullopt` when the content is not valid JSON or has no
   *         parseable `root.named_devices` array (the caller then ends no session, logs a Warning and retries).
   */
  std::optional<std::set<std::string>> disabled_client_certs(std::string_view state_json);
}  // namespace handoff
