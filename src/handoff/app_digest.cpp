/**
 * @file src/handoff/app_digest.cpp
 * @brief Definitions for `src/handoff/app_digest.h`.
 */
// standard includes
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// lib includes
#include <openssl/evp.h>

// local includes
#include "app_digest.h"

namespace handoff {
  /**
   * @brief Prefix of environment variables that are never part of the digest.
   */
  constexpr char HANDOFF_ENV_PREFIX[] = "SUNSHINE_HANDOFF_";

  /**
   * @brief Append an unsigned 64-bit value as 8 little-endian bytes.
   *
   * @param out Buffer to append to.
   * @param value Value to write.
   */
  static void append_u64(std::string &out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
  }

  /**
   * @brief Append a string as its byte length (8 bytes, little-endian) followed by its bytes.
   *
   * @param out Buffer to append to.
   * @param value String to write.
   */
  static void append_str(std::string &out, const std::string &value) {
    append_u64(out, static_cast<std::uint64_t>(value.size()));
    out.append(value);
  }

  std::string app_digest(const app_digest_input_t &input) {
    // Build the ordered, length-prefixed encoding first, then hash it in one call.
    std::string encoded;

    append_u64(encoded, static_cast<std::uint64_t>(input.prep.size()));
    for (const auto &entry : input.prep) {
      append_str(encoded, entry.do_cmd);
      append_str(encoded, entry.undo_cmd);
      append_u64(encoded, entry.elevated ? 1 : 0);
    }

    append_str(encoded, input.working_dir);
    append_str(encoded, input.output);
    append_u64(encoded, static_cast<std::uint64_t>(input.exit_timeout_s));

    // The base environment is sorted so its order does not matter, and handoff's own variables are left out.
    std::vector<std::pair<std::string, std::string>> env;
    for (const auto &item : input.base_env) {
      if (item.first.rfind(HANDOFF_ENV_PREFIX, 0) == 0) {
        continue;
      }
      env.push_back(item);
    }
    std::sort(env.begin(), env.end());

    append_u64(encoded, static_cast<std::uint64_t>(env.size()));
    for (const auto &item : env) {
      append_str(encoded, item.first);
      append_str(encoded, item.second);
    }

    // SHA-256 over the encoding, as lowercase hex.
    unsigned char digest[EVP_MAX_MD_SIZE] {};
    unsigned int digest_len = 0;
    if (EVP_Digest(encoded.data(), encoded.size(), digest, &digest_len, EVP_sha256(), nullptr) != 1) {
      // A failing SHA-256 cannot happen with the built-in provider; return an empty digest so it never matches.
      return {};
    }

    static constexpr char HEX[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest_len * 2);
    for (unsigned int i = 0; i < digest_len; ++i) {
      hex.push_back(HEX[digest[i] >> 4]);
      hex.push_back(HEX[digest[i] & 0x0F]);
    }
    return hex;
  }
}  // namespace handoff
