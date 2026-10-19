/**
 * @file src/handoff/app_digest.h
 * @brief Digest of a running app's merged prep list and launch-relevant settings, for adoption checks.
 *
 * See `docs/drain-handoff.md`, "App ownership and prep commands".
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace handoff {
  /**
   * @brief One merged prep command.
   */
  struct prep_entry_t {
    std::string do_cmd;  ///< `do` command.
    std::string undo_cmd;  ///< `undo` command.
    bool elevated {false};  ///< `elevated` flag.
  };

  /**
   * @brief Inputs of the digest.
   */
  struct app_digest_input_t {
    std::vector<prep_entry_t> prep;  ///< Merged prep list (global first unless excluded), in order.
    std::string working_dir;  ///< App `working-dir`.
    std::string output;  ///< App `output`.
    std::int64_t exit_timeout_s {0};  ///< App `exit-timeout`, seconds.
    std::vector<std::pair<std::string, std::string>> base_env;  ///< Base environment, any order.
  };

  /**
   * @brief Lowercase hex SHA-256 over an ordered, length-prefixed encoding of the inputs.
   *
   * Encoding: every string is written as its byte length (8 bytes, little-endian) followed by its bytes; booleans and
   * integers as 8-byte little-endian values. Order: the number of prep entries, then each entry's `do`, `undo`,
   * `elevated`; then `working_dir`, `output`, `exit_timeout_s`; then the base environment sorted by name (byte-wise),
   * without names starting with `SUNSHINE_HANDOFF_`, as the count followed by each name and value. So `("a","bc")`
   * and `("ab","c")` give different digests, and the environment's order does not matter.
   *
   * @param input Inputs.
   * @return 64 lowercase hex characters.
   */
  std::string app_digest(const app_digest_input_t &input);
}  // namespace handoff
