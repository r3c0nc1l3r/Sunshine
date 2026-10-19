/**
 * @file src/handoff/new_side.h
 * @brief The new process's start in handoff mode, up to `COMMIT`.
 *
 * See `docs/drain-handoff.md`, steps 3 and 5. Before `COMMIT` the new process adopts nothing, accepts nothing,
 * publishes no mDNS, maps no UPnP port, shows no tray icon and keeps role `pending`.
 */
#pragma once

// standard includes
#include <chrono>
#include <string>

// local includes
#include "channel.h"
#include "protocol.h"

namespace handoff {
  /**
   * @brief Timeouts of the new side; tests shorten them.
   */
  struct new_side_timeouts_t {
    std::chrono::milliseconds hello {30000};  ///< Wait for the old process's `HELLO`.
    std::chrono::milliseconds go {35000};  ///< From the `READY` send to `GO` (`ping_timeout` plus 25 s).
    std::chrono::milliseconds commit {5000};  ///< From the `GO_ACK` send to `COMMIT`.
  };

  /**
   * @brief Effects of the new side. Implemented by the Sunshine glue; faked in tests.
   */
  class new_side_ops_t {
  public:
    /**
     * @brief Virtual destructor.
     */
    virtual ~new_side_ops_t() = default;

    /**
     * @brief Own `HELLO` (no probe results, no identity, no cooldown).
     *
     * @return Message to send.
     */
    virtual protocol::hello_t hello() = 0;

    /**
     * @brief Take the old process's `HELLO` and pidfd (probe results for `/serverinfo` if the identity matches,
     * cooldown deadline). The ops own the pidfd from here on.
     *
     * @param hello Old process's `HELLO`.
     * @param old_pidfd Old process's pidfd (always valid here).
     */
    virtual void accept_old_hello(const protocol::hello_t &hello, int old_pidfd) = 0;

    /**
     * @brief Validate the inherited listeners against the config.
     *
     * @return Empty when valid; otherwise the reason (sent in a failed `READY`).
     */
    virtual std::string validate_listeners() = 0;

    /**
     * @brief Adopt the listeners and start the servers with accepting paused.
     *
     * @return Empty on success; otherwise the reason (sent in a failed `READY`).
     */
    virtual std::string start_servers_paused() = 0;

    /**
     * @brief On `GO`: reload `sunshine_state.json`, the credentials file and `apps.json` under `client_auth_mutex`.
     *
     * @return Empty on success; otherwise the reason (sent in a failed `GO_ACK`).
     */
    virtual std::string reload_all() = 0;

    /**
     * @brief On `GO`: check the snapshot against the reloaded app list and prepare the adoption without installing
     * it.
     *
     * @param go The `GO` message.
     * @return Empty on success; otherwise the reason (sent in a failed `GO_ACK`).
     */
    virtual std::string prepare_adoption(const protocol::go_t &go) = 0;

    /**
     * @brief On `COMMIT`, in this order with the calls below: install the prepared adoption (app, `host_audio`,
     * session id counter).
     */
    virtual void install_adoption() = 0;

    virtual void resume_accept() = 0;  ///< On `COMMIT`: resume accepting on all four sockets.
    virtual void publish_mdns() = 0;  ///< On `COMMIT`: start mDNS (`platf::publish::start()`).
    virtual void map_upnp() = 0;  ///< On `COMMIT`: start UPnP (`upnp::start()`).
    virtual void start_tray() = 0;  ///< On `COMMIT`: start the tray with Quit and restart disabled until `MAIN`.
    virtual void become_active() = 0;  ///< On `COMMIT`, last: write role `active` to the instance file.
  };

  /**
   * @brief Outcome of `run_new_side()`.
   */
  enum class new_outcome_e {
    committed,  ///< `COMMIT` received in time and every `COMMIT` effect ran; continue serving.
    exit_failure,  ///< Exit with status 1 now; nothing was adopted, accepted, published, mapped or shown.
  };

  /**
   * @brief Drive the new side up to `COMMIT`.
   *
   * Order: wait for the old `HELLO` (`hello` timeout; it must carry a pidfd and the same major version, else a failed
   * `READY` and `exit_failure`) and send own `HELLO`; `validate_listeners()`; `start_servers_paused()`; send `READY`
   * (ok, or failed with the first reason, then `exit_failure`). Wait for `GO` until `go` after the `READY` send;
   * `reload_all()` then `prepare_adoption()`; send `GO_ACK` (ok, or failed with the reason, then `exit_failure`).
   * Wait for `COMMIT` until `commit` after the `GO_ACK` send (monotonic; a `COMMIT` read later is ignored and gives
   * `exit_failure`). On `COMMIT`: `install_adoption`, `resume_accept`, `publish_mdns`, `map_upnp`, `start_tray`,
   * `become_active`, in this order, and `committed`. Any timeout, `eof`, channel error, malformed or oversized message
   * before `COMMIT` gives `exit_failure`. `PING` before `COMMIT` is answered with `PONG`; unknown messages are
   * ignored.
   *
   * @param ops Effects.
   * @param channel New process's end of the channel.
   * @param timeouts Timeouts.
   * @return Outcome.
   */
  new_outcome_e run_new_side(new_side_ops_t &ops, channel_t &channel, const new_side_timeouts_t &timeouts);
}  // namespace handoff
