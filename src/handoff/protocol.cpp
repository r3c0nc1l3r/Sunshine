/**
 * @file src/handoff/protocol.cpp
 * @brief Definitions for `src/handoff/protocol.h`.
 */
// standard includes
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "protocol.h"

namespace handoff::protocol {
  namespace {
    using json = nlohmann::json;

    /**
     * @brief Look up a member of a JSON object.
     *
     * @param object JSON value, expected to be an object.
     * @param name Member name.
     * @return Pointer to the member, or `nullptr` when `object` is not an object or the member is absent.
     */
    const json *find_member(const json &object, const char *name) {
      if (!object.is_object()) {
        return nullptr;
      }
      const auto it = object.find(name);
      return it == object.end() ? nullptr : &*it;
    }

    /**
     * @brief Read a boolean value.
     *
     * @param value JSON value.
     * @param out Receives the value.
     * @return `true` when `value` is a boolean.
     */
    bool read(const json &value, bool &out) {
      if (!value.is_boolean()) {
        return false;
      }
      out = value.get<bool>();
      return true;
    }

    /**
     * @brief Read a string value.
     *
     * @param value JSON value.
     * @param out Receives the value.
     * @return `true` when `value` is a string.
     */
    bool read(const json &value, std::string &out) {
      if (!value.is_string()) {
        return false;
      }
      out = value.get<std::string>();
      return true;
    }

    /**
     * @brief Read a non-negative integer that fits in an unsigned type.
     *
     * Rejects negative values, floating-point values and values above the range of `T`.
     *
     * @tparam T Unsigned integer type.
     * @param value JSON value.
     * @param out Receives the value.
     * @return `true` when `value` is an integer within the range of `T`.
     */
    template<typename T>
    bool read_unsigned(const json &value, T &out) {
      std::uint64_t wide = 0;
      if (value.is_number_unsigned()) {
        wide = value.get<std::uint64_t>();
      } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
          return false;
        }
        wide = static_cast<std::uint64_t>(signed_value);
      } else {
        return false;
      }
      if (wide > std::numeric_limits<T>::max()) {
        return false;
      }
      out = static_cast<T>(wide);
      return true;
    }

    /**
     * @brief Read a 32-bit unsigned integer.
     *
     * @param value JSON value.
     * @param out Receives the value.
     * @return `true` when `value` is an integer in `[0, UINT32_MAX]`.
     */
    bool read(const json &value, std::uint32_t &out) {
      return read_unsigned(value, out);
    }

    /**
     * @brief Read a 64-bit unsigned integer.
     *
     * @param value JSON value.
     * @param out Receives the value.
     * @return `true` when `value` is an integer in `[0, UINT64_MAX]`.
     */
    bool read(const json &value, std::uint64_t &out) {
      return read_unsigned(value, out);
    }

    /**
     * @brief Read a signed 64-bit integer.
     *
     * @param value JSON value.
     * @param out Receives the value.
     * @return `true` when `value` is an integer within the range of `std::int64_t`.
     */
    bool read(const json &value, std::int64_t &out) {
      if (value.is_number_unsigned()) {
        const auto unsigned_value = value.get<std::uint64_t>();
        if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          return false;
        }
        out = static_cast<std::int64_t>(unsigned_value);
        return true;
      }
      if (value.is_number_integer()) {
        out = value.get<std::int64_t>();
        return true;
      }
      return false;
    }

    /**
     * @brief Read a JSON integer that must fit in a range of `int`.
     *
     * @param value JSON value.
     * @param min_value Smallest accepted value.
     * @param max_value Largest accepted value.
     * @param out Receives the value.
     * @return `true` when `value` is an integer within `[min_value, max_value]`.
     */
    bool read_int(const json &value, std::int64_t min_value, std::int64_t max_value, int &out) {
      std::int64_t wide = 0;
      if (!read(value, wide) || wide < min_value || wide > max_value) {
        return false;
      }
      out = static_cast<int>(wide);
      return true;
    }

    /**
     * @brief Read a required member with a generic reader.
     *
     * @tparam T Type of the field; `read(const json &, T &)` must exist.
     * @param object JSON object.
     * @param name Member name.
     * @param out Receives the value.
     * @return `true` when the member exists and reads successfully.
     */
    template<typename T>
    bool field(const json &object, const char *name, T &out) {
      const json *value = find_member(object, name);
      return value != nullptr && read(*value, out);
    }

    /**
     * @brief Read a required integer member that must be within a range of `int`.
     *
     * @param object JSON object.
     * @param name Member name.
     * @param min_value Smallest accepted value.
     * @param max_value Largest accepted value.
     * @param out Receives the value.
     * @return `true` when the member exists and is within the range.
     */
    bool int_field(const json &object, const char *name, std::int64_t min_value, std::int64_t max_value, int &out) {
      const json *value = find_member(object, name);
      return value != nullptr && read_int(*value, min_value, max_value, out);
    }

    // Forward declarations: optional_field() below reads these struct types, and their readers are defined later.
    bool read(const json &object, probe_results_t &out);
    bool read(const json &object, probe_identity_t &out);
    bool read(const json &object, app_snapshot_t &out);

    /**
     * @brief Read an optional member. An absent or `null` member leaves `out` empty.
     *
     * @tparam T Type of the field; `read(const json &, T &)` must exist.
     * @param object JSON object.
     * @param name Member name.
     * @param out Receives the value, or is reset when the member is absent or `null`.
     * @return `true` when the member is absent, `null`, or reads successfully.
     */
    template<typename T>
    bool optional_field(const json &object, const char *name, std::optional<T> &out) {
      out.reset();
      const json *value = find_member(object, name);
      if (value == nullptr || value->is_null()) {
        return true;
      }
      T tmp {};
      if (!read(*value, tmp)) {
        return false;
      }
      out = std::move(tmp);
      return true;
    }

    /**
     * @brief Check that a string is a lowercase hex SHA-256 digest of 64 characters.
     *
     * @param text Candidate string.
     * @return `true` when `text` is exactly 64 characters of `[0-9a-f]`.
     */
    bool is_lower_hex_sha256(const std::string &text) {
      if (text.size() != 64) {
        return false;
      }
      for (const char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool letter = c >= 'a' && c <= 'f';
        if (!digit && !letter) {
          return false;
        }
      }
      return true;
    }

    /**
     * @brief Read a required string member that must equal one of a set of values.
     *
     * @param object JSON object.
     * @param name Member name.
     * @param allowed Accepted values.
     * @param out Receives the value.
     * @return `true` when the member exists and equals one of `allowed`.
     */
    bool enum_field(const json &object, const char *name, const std::vector<std::string_view> &allowed, std::string &out) {
      if (!field(object, name, out)) {
        return false;
      }
      for (const auto candidate : allowed) {
        if (out == candidate) {
          return true;
        }
      }
      return false;
    }

    /**
     * @brief Write a probe result set as a JSON object.
     *
     * @param value Probe results.
     * @return JSON object with `hevc_mode`, `av1_mode` and `yuv444`.
     */
    json to_json(const probe_results_t &value) {
      json object = json::object();
      object["hevc_mode"] = value.hevc_mode;
      object["av1_mode"] = value.av1_mode;
      object["yuv444"] = value.yuv444;
      return object;
    }

    /**
     * @brief Read a probe result set from a JSON object.
     *
     * @param object JSON object with `hevc_mode` and `av1_mode` in 0 to 3 and `yuv444`.
     * @param out Receives the probe results.
     * @return `true` when every member is present and valid.
     */
    bool read(const json &object, probe_results_t &out) {
      return object.is_object() &&
             int_field(object, "hevc_mode", 0, 3, out.hevc_mode) &&
             int_field(object, "av1_mode", 0, 3, out.av1_mode) &&
             field(object, "yuv444", out.yuv444);
    }

    /**
     * @brief Write a probe identity as a JSON object.
     *
     * @param value Probe identity.
     * @return JSON object with the identity members.
     */
    json to_json(const probe_identity_t &value) {
      json object = json::object();
      object["build_id"] = value.build_id;
      object["render_node"] = value.render_node;
      object["vendor_id"] = value.vendor_id;
      object["device_id"] = value.device_id;
      object["driver_version"] = value.driver_version;
      return object;
    }

    /**
     * @brief Read a probe identity from a JSON object.
     *
     * @param object JSON object.
     * @param out Receives the identity.
     * @return `true` when every member is present and valid.
     */
    bool read(const json &object, probe_identity_t &out) {
      return object.is_object() &&
             field(object, "build_id", out.build_id) &&
             field(object, "render_node", out.render_node) &&
             field(object, "vendor_id", out.vendor_id) &&
             field(object, "device_id", out.device_id) &&
             field(object, "driver_version", out.driver_version);
    }

    /**
     * @brief Write an app snapshot as a JSON object.
     *
     * @param value App snapshot.
     * @return JSON object; `launch_env` is an array of `[name, value]` arrays.
     */
    json to_json(const app_snapshot_t &value) {
      json object = json::object();
      object["app_id"] = value.app_id;
      object["name"] = value.name;
      object["applied_prep"] = value.applied_prep;
      object["digest"] = value.digest;
      json env = json::array();
      for (const auto &[name, env_value] : value.launch_env) {
        env.push_back(json::array({name, env_value}));
      }
      object["launch_env"] = std::move(env);
      return object;
    }

    /**
     * @brief Read an app snapshot from a JSON object.
     *
     * @param object JSON object.
     * @param out Receives the snapshot.
     * @return `true` when every member is present and valid; `launch_env` entries must be two-string arrays.
     */
    bool read(const json &object, app_snapshot_t &out) {
      if (!object.is_object() || !int_field(object, "app_id", std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), out.app_id) || !field(object, "name", out.name) || !field(object, "applied_prep", out.applied_prep) || !field(object, "digest", out.digest) || !is_lower_hex_sha256(out.digest)) {
        return false;
      }
      const json *env = find_member(object, "launch_env");
      if (env == nullptr || !env->is_array()) {
        return false;
      }
      out.launch_env.clear();
      for (const auto &pair : *env) {
        if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string() || !pair[1].is_string()) {
          return false;
        }
        out.launch_env.emplace_back(pair[0].get<std::string>(), pair[1].get<std::string>());
      }
      return true;
    }

    /**
     * @brief Write one session entry as a JSON object.
     *
     * @param value Session entry.
     * @return JSON object with the entry members.
     */
    json to_json(const session_entry_t &value) {
      json object = json::object();
      object["id"] = value.id;
      object["started_unix_ms"] = value.started_unix_ms;
      object["transport"] = value.transport;
      object["client_category"] = value.client_category;
      return object;
    }

    /**
     * @brief Read one session entry from a JSON object.
     *
     * @param object JSON object.
     * @param out Receives the entry.
     * @return `true` when every member is present and valid.
     */
    bool read(const json &object, session_entry_t &out) {
      return object.is_object() &&
             field(object, "id", out.id) &&
             field(object, "started_unix_ms", out.started_unix_ms) &&
             field(object, "transport", out.transport) &&
             field(object, "client_category", out.client_category);
    }

    /**
     * @brief Write the `HELLO` members.
     *
     * @param message Message.
     * @return JSON object with the message members.
     */
    json to_json(const hello_t &message) {
      json object = json::object();
      object["major"] = message.major;
      object["minor"] = message.minor;
      object["capabilities"] = message.capabilities;
      if (message.probe) {
        object["probe"] = to_json(*message.probe);
      }
      if (message.identity) {
        object["identity"] = to_json(*message.identity);
      }
      if (message.cooldown_deadline_ns) {
        object["cooldown_deadline_ns"] = *message.cooldown_deadline_ns;
      }
      return object;
    }

    /**
     * @brief Read the `HELLO` members.
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when every required member is valid and the optional ones are absent, `null` or valid.
     */
    bool read(const json &object, hello_t &out) {
      if (!object.is_object() || !field(object, "major", out.major) || !field(object, "minor", out.minor)) {
        return false;
      }
      const json *capabilities = find_member(object, "capabilities");
      if (capabilities == nullptr || !capabilities->is_array()) {
        return false;
      }
      out.capabilities.clear();
      for (const auto &capability : *capabilities) {
        std::string name;
        if (!read(capability, name)) {
          return false;
        }
        out.capabilities.push_back(std::move(name));
      }
      return optional_field(object, "probe", out.probe) &&
             optional_field(object, "identity", out.identity) &&
             optional_field(object, "cooldown_deadline_ns", out.cooldown_deadline_ns);
    }

    /**
     * @brief Write the `READY` members.
     *
     * @param message Message.
     * @return JSON object with the message members.
     */
    json to_json(const ready_t &message) {
      json object = json::object();
      object["ok"] = message.ok;
      object["reason"] = message.reason;
      return object;
    }

    /**
     * @brief Read the `READY` members.
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when every member is present and valid.
     */
    bool read(const json &object, ready_t &out) {
      return object.is_object() && field(object, "ok", out.ok) && field(object, "reason", out.reason);
    }

    /**
     * @brief Write the `GO` members.
     *
     * @param message Message.
     * @return JSON object with the message members; `app` is omitted when there is no app.
     */
    json to_json(const go_t &message) {
      json object = json::object();
      if (message.app) {
        object["app"] = to_json(*message.app);
      }
      object["host_audio"] = message.host_audio;
      object["next_launch_session_id"] = message.next_launch_session_id;
      return object;
    }

    /**
     * @brief Read the `GO` members.
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when every required member is valid and `app` is absent, `null` or valid.
     */
    bool read(const json &object, go_t &out) {
      return object.is_object() &&
             optional_field(object, "app", out.app) &&
             field(object, "host_audio", out.host_audio) &&
             field(object, "next_launch_session_id", out.next_launch_session_id);
    }

    /**
     * @brief Write the `GO_ACK` members.
     *
     * @param message Message.
     * @return JSON object with the message members.
     */
    json to_json(const go_ack_t &message) {
      json object = json::object();
      object["ok"] = message.ok;
      object["reason"] = message.reason;
      return object;
    }

    /**
     * @brief Read the `GO_ACK` members.
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when every member is present and valid.
     */
    bool read(const json &object, go_ack_t &out) {
      return object.is_object() && field(object, "ok", out.ok) && field(object, "reason", out.reason);
    }

    /**
     * @brief Write the `COMMIT` members (none).
     *
     * @param message Message.
     * @return Empty JSON object.
     */
    json to_json(const commit_t &message) {
      static_cast<void>(message);
      return json::object();
    }

    /**
     * @brief Read the `COMMIT` members (none).
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when `object` is an object.
     */
    bool read(const json &object, commit_t &out) {
      static_cast<void>(out);
      return object.is_object();
    }

    /**
     * @brief Write the `MAIN` members (none).
     *
     * @param message Message.
     * @return Empty JSON object.
     */
    json to_json(const main_t &message) {
      static_cast<void>(message);
      return json::object();
    }

    /**
     * @brief Read the `MAIN` members (none).
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when `object` is an object.
     */
    bool read(const json &object, main_t &out) {
      static_cast<void>(out);
      return object.is_object();
    }

    /**
     * @brief Write the `PING` members.
     *
     * @param message Message.
     * @return JSON object with `seq`.
     */
    json to_json(const ping_t &message) {
      json object = json::object();
      object["seq"] = message.seq;
      return object;
    }

    /**
     * @brief Read the `PING` members.
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when `seq` is present and valid.
     */
    bool read(const json &object, ping_t &out) {
      return object.is_object() && field(object, "seq", out.seq);
    }

    /**
     * @brief Write the `PONG` members.
     *
     * @param message Message.
     * @return JSON object with `seq`.
     */
    json to_json(const pong_t &message) {
      json object = json::object();
      object["seq"] = message.seq;
      return object;
    }

    /**
     * @brief Read the `PONG` members.
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when `seq` is present and valid.
     */
    bool read(const json &object, pong_t &out) {
      return object.is_object() && field(object, "seq", out.seq);
    }

    /**
     * @brief Write the `SESSIONS?` members.
     *
     * @param message Message.
     * @return JSON object with `request_id`.
     */
    json to_json(const sessions_request_t &message) {
      json object = json::object();
      object["request_id"] = message.request_id;
      return object;
    }

    /**
     * @brief Read the `SESSIONS?` members.
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when `request_id` is present and valid.
     */
    bool read(const json &object, sessions_request_t &out) {
      return object.is_object() && field(object, "request_id", out.request_id);
    }

    /**
     * @brief Write the `SESSIONS` members.
     *
     * @param message Message.
     * @return JSON object with `request_id` and the `sessions` array.
     */
    json to_json(const sessions_reply_t &message) {
      json object = json::object();
      object["request_id"] = message.request_id;
      json sessions = json::array();
      for (const auto &entry : message.sessions) {
        sessions.push_back(to_json(entry));
      }
      object["sessions"] = std::move(sessions);
      return object;
    }

    /**
     * @brief Read the `SESSIONS` members.
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when `request_id` and every session entry are valid.
     */
    bool read(const json &object, sessions_reply_t &out) {
      if (!object.is_object() || !field(object, "request_id", out.request_id)) {
        return false;
      }
      const json *sessions = find_member(object, "sessions");
      if (sessions == nullptr || !sessions->is_array()) {
        return false;
      }
      out.sessions.clear();
      for (const auto &item : *sessions) {
        session_entry_t entry;
        if (!read(item, entry)) {
          return false;
        }
        out.sessions.push_back(std::move(entry));
      }
      return true;
    }

    /**
     * @brief Write the `CLOSE` members.
     *
     * @param message Message.
     * @return JSON object with `request_id` and `session_id`.
     */
    json to_json(const close_request_t &message) {
      json object = json::object();
      object["request_id"] = message.request_id;
      object["session_id"] = message.session_id;
      return object;
    }

    /**
     * @brief Read the `CLOSE` members.
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when every member is present and valid.
     */
    bool read(const json &object, close_request_t &out) {
      return object.is_object() &&
             field(object, "request_id", out.request_id) &&
             field(object, "session_id", out.session_id);
    }

    /**
     * @brief Write the `CLOSED` members.
     *
     * @param message Message.
     * @return JSON object with `request_id` and `result`.
     */
    json to_json(const close_reply_t &message) {
      json object = json::object();
      object["request_id"] = message.request_id;
      object["result"] = message.result;
      return object;
    }

    /**
     * @brief Read the `CLOSED` members.
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when `request_id` is valid and `result` is `closed`, `already-stopping` or `not-found`.
     */
    bool read(const json &object, close_reply_t &out) {
      return object.is_object() &&
             field(object, "request_id", out.request_id) &&
             enum_field(object, "result", {"closed", "already-stopping", "not-found"}, out.result);
    }

    /**
     * @brief Write the `CLOSE_CERT` members.
     *
     * @param message Message.
     * @return JSON object with `request_id` and `cert_sha256`.
     */
    json to_json(const close_cert_request_t &message) {
      json object = json::object();
      object["request_id"] = message.request_id;
      object["cert_sha256"] = message.cert_sha256;
      return object;
    }

    /**
     * @brief Read the `CLOSE_CERT` members.
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when `request_id` is valid and `cert_sha256` is 64 lowercase hex characters.
     */
    bool read(const json &object, close_cert_request_t &out) {
      return object.is_object() &&
             field(object, "request_id", out.request_id) &&
             field(object, "cert_sha256", out.cert_sha256) &&
             is_lower_hex_sha256(out.cert_sha256);
    }

    /**
     * @brief Write the `CLOSED_CERT` members.
     *
     * @param message Message.
     * @return JSON object with `request_id` and `count`.
     */
    json to_json(const close_cert_reply_t &message) {
      json object = json::object();
      object["request_id"] = message.request_id;
      object["count"] = message.count;
      return object;
    }

    /**
     * @brief Read the `CLOSED_CERT` members.
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when every member is present and valid.
     */
    bool read(const json &object, close_cert_reply_t &out) {
      return object.is_object() && field(object, "request_id", out.request_id) && field(object, "count", out.count);
    }

    /**
     * @brief Write a request id only message (`END_ALL`, `ENDED`, `END_REJECTED`).
     *
     * @param request_id Request id.
     * @return JSON object with `request_id`.
     */
    json request_id_only(std::uint64_t request_id) {
      json object = json::object();
      object["request_id"] = request_id;
      return object;
    }

    /**
     * @brief Write the `END_ALL` members.
     *
     * @param message Message.
     * @return JSON object with `request_id`.
     */
    json to_json(const end_all_t &message) {
      return request_id_only(message.request_id);
    }

    /**
     * @brief Read the `END_ALL` members.
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when `request_id` is present and valid.
     */
    bool read(const json &object, end_all_t &out) {
      return object.is_object() && field(object, "request_id", out.request_id);
    }

    /**
     * @brief Write the `ENDED` members.
     *
     * @param message Message.
     * @return JSON object with `request_id`.
     */
    json to_json(const ended_t &message) {
      return request_id_only(message.request_id);
    }

    /**
     * @brief Read the `ENDED` members.
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when `request_id` is present and valid.
     */
    bool read(const json &object, ended_t &out) {
      return object.is_object() && field(object, "request_id", out.request_id);
    }

    /**
     * @brief Write the `END_REJECTED` members.
     *
     * @param message Message.
     * @return JSON object with `request_id`.
     */
    json to_json(const end_rejected_t &message) {
      return request_id_only(message.request_id);
    }

    /**
     * @brief Read the `END_REJECTED` members.
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when `request_id` is present and valid.
     */
    bool read(const json &object, end_rejected_t &out) {
      return object.is_object() && field(object, "request_id", out.request_id);
    }

    /**
     * @brief Write the `HANDOFF_STATE` members.
     *
     * @param message Message.
     * @return JSON object with `state`.
     */
    json to_json(const handoff_state_t &message) {
      json object = json::object();
      object["state"] = message.state;
      return object;
    }

    /**
     * @brief Read the `HANDOFF_STATE` members.
     *
     * @param object JSON object.
     * @param out Receives the message.
     * @return `true` when `state` is `none`, `draining`, `handover_failed` or `kill_pending`.
     */
    bool read(const json &object, handoff_state_t &out) {
      return object.is_object() &&
             enum_field(object, "state", {"none", "draining", "handover_failed", "kill_pending"}, out.state);
    }

    /**
     * @brief Visitor that writes the members of each known message type.
     *
     * `unknown_t` is never encoded; the encoder checks for it before visiting.
     */
    struct member_writer {
      /**
       * @brief Write the members of a message.
       *
       * @tparam T Message type.
       * @param message Message.
       * @return JSON object with the members of `message`.
       */
      template<typename T>
      json operator()(const T &message) const {
        return to_json(message);
      }

      /**
       * @brief Unknown messages have no encoding.
       *
       * @param message Unknown message, ignored.
       * @return Empty JSON object; never used by `encode()`.
       */
      json operator()(const unknown_t &message) const {
        static_cast<void>(message);
        return json::object();
      }
    };

    /**
     * @brief Decode a message of a known type from its JSON object.
     *
     * @tparam T Message type.
     * @param object JSON object.
     * @param message Receives the message when it reads successfully.
     * @return `ok` when every member is valid, otherwise `malformed`.
     */
    template<typename T>
    decode_status_e decode_as(const json &object, message_t &message) {
      T value {};
      if (!read(object, value)) {
        return decode_status_e::malformed;
      }
      message = std::move(value);
      return decode_status_e::ok;
    }
  }  // namespace

  std::string type_name(const message_t &message) {
    if (std::holds_alternative<hello_t>(message)) {
      return "HELLO";
    }
    if (std::holds_alternative<ready_t>(message)) {
      return "READY";
    }
    if (std::holds_alternative<go_t>(message)) {
      return "GO";
    }
    if (std::holds_alternative<go_ack_t>(message)) {
      return "GO_ACK";
    }
    if (std::holds_alternative<commit_t>(message)) {
      return "COMMIT";
    }
    if (std::holds_alternative<main_t>(message)) {
      return "MAIN";
    }
    if (std::holds_alternative<ping_t>(message)) {
      return "PING";
    }
    if (std::holds_alternative<pong_t>(message)) {
      return "PONG";
    }
    if (std::holds_alternative<sessions_request_t>(message)) {
      return "SESSIONS?";
    }
    if (std::holds_alternative<sessions_reply_t>(message)) {
      return "SESSIONS";
    }
    if (std::holds_alternative<close_request_t>(message)) {
      return "CLOSE";
    }
    if (std::holds_alternative<close_reply_t>(message)) {
      return "CLOSED";
    }
    if (std::holds_alternative<close_cert_request_t>(message)) {
      return "CLOSE_CERT";
    }
    if (std::holds_alternative<close_cert_reply_t>(message)) {
      return "CLOSED_CERT";
    }
    if (std::holds_alternative<end_all_t>(message)) {
      return "END_ALL";
    }
    if (std::holds_alternative<ended_t>(message)) {
      return "ENDED";
    }
    if (std::holds_alternative<end_rejected_t>(message)) {
      return "END_REJECTED";
    }
    if (std::holds_alternative<handoff_state_t>(message)) {
      return "HANDOFF_STATE";
    }
    if (const auto *unknown = std::get_if<unknown_t>(&message)) {
      return unknown->type;
    }
    return {};
  }

  std::string encode(const message_t &message) {
    if (std::holds_alternative<unknown_t>(message)) {
      return {};
    }

    try {
      json object = std::visit(member_writer {}, message);
      object["type"] = type_name(message);
      std::string payload = object.dump();
      if (payload.size() > max_message_size) {
        return {};
      }
      return payload;
    } catch (...) {
      // A string that is not valid UTF-8 makes dump() throw; the message cannot be encoded.
      return {};
    }
  }

  decode_status_e decode(std::string_view payload, message_t &message) {
    if (payload.size() > max_message_size) {
      return decode_status_e::oversized;
    }

    try {
      json object = json::parse(std::string(payload), nullptr, false);
      if (object.is_discarded() || !object.is_object()) {
        return decode_status_e::malformed;
      }

      std::string type;
      if (!field(object, "type", type)) {
        return decode_status_e::malformed;
      }

      if (type == "HELLO") {
        return decode_as<hello_t>(object, message);
      }
      if (type == "READY") {
        return decode_as<ready_t>(object, message);
      }
      if (type == "GO") {
        return decode_as<go_t>(object, message);
      }
      if (type == "GO_ACK") {
        return decode_as<go_ack_t>(object, message);
      }
      if (type == "COMMIT") {
        return decode_as<commit_t>(object, message);
      }
      if (type == "MAIN") {
        return decode_as<main_t>(object, message);
      }
      if (type == "PING") {
        return decode_as<ping_t>(object, message);
      }
      if (type == "PONG") {
        return decode_as<pong_t>(object, message);
      }
      if (type == "SESSIONS?") {
        return decode_as<sessions_request_t>(object, message);
      }
      if (type == "SESSIONS") {
        return decode_as<sessions_reply_t>(object, message);
      }
      if (type == "CLOSE") {
        return decode_as<close_request_t>(object, message);
      }
      if (type == "CLOSED") {
        return decode_as<close_reply_t>(object, message);
      }
      if (type == "CLOSE_CERT") {
        return decode_as<close_cert_request_t>(object, message);
      }
      if (type == "CLOSED_CERT") {
        return decode_as<close_cert_reply_t>(object, message);
      }
      if (type == "END_ALL") {
        return decode_as<end_all_t>(object, message);
      }
      if (type == "ENDED") {
        return decode_as<ended_t>(object, message);
      }
      if (type == "END_REJECTED") {
        return decode_as<end_rejected_t>(object, message);
      }
      if (type == "HANDOFF_STATE") {
        return decode_as<handoff_state_t>(object, message);
      }

      message = unknown_t {std::move(type)};
      return decode_status_e::ok;
    } catch (...) {
      // Defensive: nothing above should throw, but no exception may leave the channel code.
      return decode_status_e::malformed;
    }
  }
}  // namespace handoff::protocol
