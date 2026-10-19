/**
 * @file src/handoff/protocol.h
 * @brief Messages of the drain-handoff channel and their wire encoding.
 *
 * See `docs/drain-handoff.md`, "The handoff channel". Each message is one datagram on a `SOCK_SEQPACKET`
 * socketpair. The payload is a JSON object with a string member `type` (the wire name from `type_name()`); every
 * other member has the name of the C++ field below. Nested structs are JSON objects, vectors are arrays,
 * `launch_env` is an array of two-string arrays `[name, value]`, and an empty `std::optional` is omitted (it may also
 * be `null` on decode). Decoding is strict: a known type with a missing, wrongly typed or out-of-range member is
 * malformed. Unknown types decode to `unknown_t` and are ignored by the receiver.
 *
 * Example: `{"type":"PING","seq":7}`.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace handoff::protocol {
  constexpr std::uint32_t version_major = 1;  ///< Protocol major version; a different major is a failed `READY`.
  constexpr std::uint32_t version_minor = 0;  ///< Protocol minor version; informational.
  constexpr std::size_t max_message_size = 64 * 1024;  ///< Largest accepted encoded message, in bytes.

  /**
   * @brief Identity of the encoder probe results carried in `HELLO`.
   *
   * The receiver uses the probe results only when every field equals its own identity.
   */
  struct probe_identity_t {
    std::string build_id;  ///< Build identifier of the Sunshine binary.
    std::string render_node;  ///< GPU render node path, for example `/dev/dri/renderD128`.
    std::uint32_t vendor_id {0};  ///< PCI vendor id of the GPU.
    std::uint32_t device_id {0};  ///< PCI device id of the GPU.
    std::string driver_version;  ///< Driver version string.

    /**
     * @brief Compare two identities field by field.
     *
     * @param other Identity to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const probe_identity_t &other) const = default;
  };

  /**
   * @brief Encoder capability flags that `/serverinfo` reports.
   */
  struct probe_results_t {
    int hevc_mode {0};  ///< HEVC mode as reported by `/serverinfo` (0 to 3).
    int av1_mode {0};  ///< AV1 mode as reported by `/serverinfo` (0 to 3).
    bool yuv444 {false};  ///< 4:4:4 support.

    /**
     * @brief Compare two result sets field by field.
     *
     * @param other Results to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const probe_results_t &other) const = default;
  };

  /**
   * @brief `HELLO`: first message in each direction.
   *
   * The old process's `HELLO` also carries its own pidfd as `SCM_RIGHTS` (see `channel_t`); that fd is not part of
   * the encoded payload.
   */
  struct hello_t {
    std::uint32_t major {version_major};  ///< Sender's protocol major version.
    std::uint32_t minor {version_minor};  ///< Sender's protocol minor version.
    std::vector<std::string> capabilities;  ///< Optional feature names; unknown names are ignored.
    std::optional<probe_results_t> probe;  ///< Old process only: its encoder probe results.
    std::optional<probe_identity_t> identity;  ///< Old process only: identity of `probe`.
    std::optional<std::int64_t> cooldown_deadline_ns;  ///< Old process only: cooldown deadline, `CLOCK_MONOTONIC` ns.

    /**
     * @brief Compare two messages field by field.
     *
     * @param other Message to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const hello_t &other) const = default;
  };

  /**
   * @brief `READY` from the new process: started and listening with accepting paused, or failed.
   */
  struct ready_t {
    bool ok {false};  ///< `true` when the new process is ready.
    std::string reason;  ///< Failure reason; empty when `ok`.

    /**
     * @brief Compare two messages field by field.
     *
     * @param other Message to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const ready_t &other) const = default;
  };

  /**
   * @brief The old process's running placebo app, as handed over in `GO`.
   */
  struct app_snapshot_t {
    int app_id {0};  ///< App id (`proc_t` app id).
    std::string name;  ///< App name.
    std::uint32_t applied_prep {0};  ///< Number of applied prep commands (`_app_prep_it - _app_prep_begin`).
    std::string digest;  ///< Lowercase hex SHA-256 from `handoff::app_digest()`.
    std::vector<std::pair<std::string, std::string>> launch_env;  ///< `SUNSHINE_*` launch environment, as data.

    /**
     * @brief Compare two snapshots field by field.
     *
     * @param other Snapshot to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const app_snapshot_t &other) const = default;
  };

  /**
   * @brief `GO` from the old process: the switch has happened on its side; carries the snapshot.
   */
  struct go_t {
    std::optional<app_snapshot_t> app;  ///< Running placebo app, or none.
    bool host_audio {false};  ///< Current `host_audio` value (the value `/resume` uses).
    std::uint32_t next_launch_session_id {0};  ///< Next launch session id to allocate.

    /**
     * @brief Compare two messages field by field.
     *
     * @param other Message to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const go_t &other) const = default;
  };

  /**
   * @brief `GO_ACK` from the new process.
   */
  struct go_ack_t {
    bool ok {false};  ///< `true` when the adoption is prepared and the reloads succeeded.
    std::string reason;  ///< Failure reason; empty when `ok`.

    /**
     * @brief Compare two messages field by field.
     *
     * @param other Message to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const go_ack_t &other) const = default;
  };

  /**
   * @brief `COMMIT` from the old process: the new process may install the adoption and start serving.
   */
  struct commit_t {
    /**
     * @brief Compare two messages.
     *
     * @param other Message to compare with.
     * @return Always `true`.
     */
    bool operator==(const commit_t &other) const = default;
  };

  /**
   * @brief `MAIN` from the old process: the main-PID handover is done.
   */
  struct main_t {
    /**
     * @brief Compare two messages.
     *
     * @param other Message to compare with.
     * @return Always `true`.
     */
    bool operator==(const main_t &other) const = default;
  };

  /**
   * @brief `PING <seq>` from the old process.
   */
  struct ping_t {
    std::uint64_t seq {0};  ///< Sequence number.

    /**
     * @brief Compare two messages field by field.
     *
     * @param other Message to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const ping_t &other) const = default;
  };

  /**
   * @brief `PONG <seq>` from the new process.
   */
  struct pong_t {
    std::uint64_t seq {0};  ///< Sequence number of the answered `PING`.

    /**
     * @brief Compare two messages field by field.
     *
     * @param other Message to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const pong_t &other) const = default;
  };

  /**
   * @brief One session of the draining process, as listed in `SESSIONS`.
   */
  struct session_entry_t {
    std::uint32_t id {0};  ///< Launch session id.
    std::int64_t started_unix_ms {0};  ///< Session start time, Unix epoch milliseconds.
    std::string transport;  ///< `udp/ipv4` or `udp/ipv6`.
    std::string client_category;  ///< `local`, `lan` or `wan`.

    /**
     * @brief Compare two entries field by field.
     *
     * @param other Entry to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const session_entry_t &other) const = default;
  };

  /**
   * @brief `SESSIONS?` request.
   */
  struct sessions_request_t {
    std::uint64_t request_id {0};  ///< Request id, echoed by the reply.

    /**
     * @brief Compare two messages field by field.
     *
     * @param other Message to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const sessions_request_t &other) const = default;
  };

  /**
   * @brief `SESSIONS` reply.
   */
  struct sessions_reply_t {
    std::uint64_t request_id {0};  ///< Echoed request id.
    std::vector<session_entry_t> sessions;  ///< Live sessions of the draining process.

    /**
     * @brief Compare two messages field by field.
     *
     * @param other Message to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const sessions_reply_t &other) const = default;
  };

  /**
   * @brief `CLOSE <id>` request.
   */
  struct close_request_t {
    std::uint64_t request_id {0};  ///< Request id, echoed by the reply.
    std::uint32_t session_id {0};  ///< Session to close.

    /**
     * @brief Compare two messages field by field.
     *
     * @param other Message to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const close_request_t &other) const = default;
  };

  /**
   * @brief `CLOSED <result>` reply to `CLOSE`.
   */
  struct close_reply_t {
    std::uint64_t request_id {0};  ///< Echoed request id.
    std::string result;  ///< `closed`, `already-stopping` or `not-found`.

    /**
     * @brief Compare two messages field by field.
     *
     * @param other Message to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const close_reply_t &other) const = default;
  };

  /**
   * @brief `CLOSE_CERT <hash>` request.
   */
  struct close_cert_request_t {
    std::uint64_t request_id {0};  ///< Request id, echoed by the reply.
    std::string cert_sha256;  ///< Lowercase hex SHA-256 of the client certificate (64 characters).

    /**
     * @brief Compare two messages field by field.
     *
     * @param other Message to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const close_cert_request_t &other) const = default;
  };

  /**
   * @brief `CLOSED <count>` reply to `CLOSE_CERT`.
   */
  struct close_cert_reply_t {
    std::uint64_t request_id {0};  ///< Echoed request id.
    std::uint32_t count {0};  ///< Number of sessions ended.

    /**
     * @brief Compare two messages field by field.
     *
     * @param other Message to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const close_cert_reply_t &other) const = default;
  };

  /**
   * @brief `END_ALL` request from the active process.
   */
  struct end_all_t {
    std::uint64_t request_id {0};  ///< Request id, echoed by the reply.

    /**
     * @brief Compare two messages field by field.
     *
     * @param other Message to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const end_all_t &other) const = default;
  };

  /**
   * @brief `ENDED` reply: the draining process ended and joined all its sessions and gave up its app claim.
   */
  struct ended_t {
    std::uint64_t request_id {0};  ///< Echoed request id.

    /**
     * @brief Compare two messages field by field.
     *
     * @param other Message to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const ended_t &other) const = default;
  };

  /**
   * @brief `END_REJECTED` reply: a rollback was decided; treated like a timeout.
   */
  struct end_rejected_t {
    std::uint64_t request_id {0};  ///< Echoed request id.

    /**
     * @brief Compare two messages field by field.
     *
     * @param other Message to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const end_rejected_t &other) const = default;
  };

  /**
   * @brief Unsolicited `HANDOFF_STATE <state>` from the draining process.
   */
  struct handoff_state_t {
    std::string state;  ///< `none`, `draining`, `handover_failed` or `kill_pending`.

    /**
     * @brief Compare two messages field by field.
     *
     * @param other Message to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const handoff_state_t &other) const = default;
  };

  /**
   * @brief A well-formed message of a type this version does not know; ignored by receivers.
   */
  struct unknown_t {
    std::string type;  ///< The `type` member as received.

    /**
     * @brief Compare two messages field by field.
     *
     * @param other Message to compare with.
     * @return `true` when every field is equal.
     */
    bool operator==(const unknown_t &other) const = default;
  };

  /**
   * @brief Any channel message.
   */
  using message_t = std::variant<
    hello_t,
    ready_t,
    go_t,
    go_ack_t,
    commit_t,
    main_t,
    ping_t,
    pong_t,
    sessions_request_t,
    sessions_reply_t,
    close_request_t,
    close_reply_t,
    close_cert_request_t,
    close_cert_reply_t,
    end_all_t,
    ended_t,
    end_rejected_t,
    handoff_state_t,
    unknown_t>;

  /**
   * @brief Wire name of a message's type.
   *
   * Names: `HELLO`, `READY`, `GO`, `GO_ACK`, `COMMIT`, `MAIN`, `PING`, `PONG`, `SESSIONS?`, `SESSIONS`, `CLOSE`,
   * `CLOSED`, `CLOSE_CERT`, `CLOSED_CERT`, `END_ALL`, `ENDED`, `END_REJECTED`, `HANDOFF_STATE`; for `unknown_t` its
   * stored type.
   *
   * @param message Message.
   * @return Wire name.
   */
  std::string type_name(const message_t &message);

  /**
   * @brief Encode a message as the JSON payload of one datagram.
   *
   * @param message Message to encode. `unknown_t` cannot be encoded.
   * @return Encoded payload; empty for `unknown_t` or when the encoding would exceed `max_message_size`.
   */
  std::string encode(const message_t &message);

  /**
   * @brief Outcome of decoding one datagram.
   */
  enum class decode_status_e {
    ok,  ///< Decoded; the message is set (possibly `unknown_t`).
    oversized,  ///< Longer than `max_message_size`.
    malformed,  ///< Not a JSON object, no string `type`, or a known type with invalid members.
  };

  /**
   * @brief Strictly decode one datagram.
   *
   * Rules for known types: every member listed for the type must be present with the right JSON type, no
   * unsigned value may be negative or exceed its C++ type, `cert_sha256` and `app_snapshot_t::digest` must be 64 lowercase hex characters (an empty digest, which
   * `app_digest()` returns on a hash failure, is malformed, so two failed digests can never compare equal),
   * `CLOSED.result` must be one of its three values and `HANDOFF_STATE.state` one of its four values. Members not
   * listed for the type are ignored.
   *
   * @param payload Datagram payload.
   * @param message Receives the message when the status is `ok`.
   * @return Decode status.
   */
  decode_status_e decode(std::string_view payload, message_t &message);
}  // namespace handoff::protocol
