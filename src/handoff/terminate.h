/**
 * @file src/handoff/terminate.h
 * @brief The terminate rule at every caller of `proc::proc.terminate()`, the draining-session count, and the `/launch`
 * guard.
 *
 * See `docs/drain-handoff.md`, "App ownership and prep commands" (Terminate rule) and "Visibility and control". Linux
 * only: shared code (`nvhttp.cpp`, `confighttp.cpp`, `process.cpp`, `rtsp.cpp`) includes it behind `#ifdef __linux__`.
 * On other platforms those callers keep today's code unchanged.
 *
 * ## Caller rules (the contract for T7)
 *
 * Common to all callers: ask first, terminate only if `allows_terminate(permission)`; a refusal logs one Warning (generic
 * text: the action, and that the draining process did not release the app) and never runs an `undo` command. With no
 * draining process the permission is `no_drain` and every path behaves exactly as before (the only cost is a null check).
 * Every caller uses `terminate_app` or, for `/cancel`, `terminate_gate` followed by `terminate_app_permitted`, so request
 * paths terminate through one place and never run `proc::proc.terminate()` concurrently with each other.
 *
 * | Caller | Budget | When to ask | Refused |
 * |---|---|---|---|
 * | `nvhttp::cancel` (`/cancel`) | 15 s | First thing, before any session is ended, even when no app runs and the process has no sessions (`END_ALL` also ends the draining process's sessions). | Nothing is ended or terminated, own sessions included, and the display configuration is not reverted. XML reply: `root.cancel` 0, `status_code` 503, `status_message` says the previous process still holds the app; `Connection: close` as today. |
 * | `confighttp::closeApp` | 15 s | Always, before `terminate()`. | HTTP 503, JSON `{"status": false, "status_code": 503, "error": <text>}` with the headers of `bad_request`; the app keeps running. |
 * | `confighttp::updateClient` (disable) | 15 s | Only after the client's own sessions were ended and the peer was told (below), only when `rtsp_stream::total_session_count() == 0` and `proc::proc.running() > 0`. | The app keeps running; the reply is unchanged (`status` as before, 200). |
 * | `confighttp::unpair` | 15 s | Only when the client was removed and no client remains. | The client stays removed, the app keeps running, reply unchanged. |
 * | `confighttp::unpairAll` | 15 s | After all clients were erased. | Clients stay removed, the app keeps running, reply unchanged. |
 * | `proc` deinit guard (`process.cpp`, shutdown) | 5 s | Only when `proc::proc.running() > 0`; with no running app the guard calls `proc::proc.terminate()` as before, without asking. | Depends on who started the app (see "Shutdown and adopted apps" below): an ADOPTED app is not terminated and no `undo` runs (logged as a Warning); an app this process launched itself is terminated exactly as upstream does, whatever the gate said. |
 *
 * `updateClient` order when a client is disabled: (1) `rtsp_stream::terminate_sessions_by_cert(cert)` for the own
 * sessions; (2) when `cert` is not empty, `session_peer_t::close_cert(cert_sha256_hex(cert), ...)` on the peer
 * (best effort: a `std::nullopt` is logged as a Warning and ends nothing else); (3) the terminate decision above. The
 * response never depends on the peer.
 *
 * ## Shutdown and adopted apps
 *
 * A shutdown must never leave behind an app this process started. So on the shutdown path (and only there) a refused
 * permission skips `terminate()` only for an ADOPTED app, the one this process took over from the draining process
 * (`proc::proc.adopted()`, see below): it has no process group here, and its `undo` commands must not run while the
 * other process may still stream. For an app this process launched itself, the guard terminates it as upstream does
 * (process group terminated, `undo` commands run), whatever `terminate_gate` returned; the gate is still asked first, so
 * the draining process gets its `END_ALL`, but its answer does not hold back the terminate. Request paths (the other rows)
 * keep the plain rule: refused means nothing is terminated.
 *
 * How the module knows "adopted": the wave 2A process contract has no accessor for it (`install_adoption` sets
 * placebo mode with process group 0, which an app with an empty command launched by this process also has, so those
 * atomics cannot tell the two apart). This contract therefore needs one new accessor, `bool proc::proc_t::adopted()
 * const`: true from `install_adoption` with an app until `terminate()` or `drop_released_app()` clears the app; false for
 * every app that `execute()` started; thread-safe (an atomic flag next to `released`). The exact diff is in the wave C
 * report; the shutdown guard in `process.cpp` calls it.
 *
 * Internal terminates (`execute()` at its start and in its failure guard, `running()` when the app's process exited)
 * never ask: they never act on an adopted app.
 *
 * ## `/launch` guard (`nvhttp::launch`)
 *
 * After the argument check and the "app already running" check (so their answers keep priority), and before anything is
 * changed (`set_host_audio`, launch-session creation, display configuration, encoder probe, `proc::proc.execute`), call
 * `launch_refusal()`. When it returns a non-empty text, answer: XML `root.gamesession` 0, `status_code` 503,
 * `status_message` the text, `Connection: close`; start nothing. `/resume` is not guarded.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// local includes
#include "end_all.h"
#include "peer_link.h"
#include "runtime.h"

namespace handoff {
  constexpr std::chrono::milliseconds request_terminate_budget {15000};  ///< Budget of the request paths (`/cancel`, close app, update client, unpair, unpair-all).
  constexpr std::chrono::milliseconds shutdown_terminate_budget {5000};  ///< Budget of the shutdown path (inside the 10 s forced-shutdown limit).

  /**
   * @brief Ask for permission to terminate the app.
   *
   * Wraps `await_terminate_permission(*runtime().peer(), budget)`:
   * - `no_drain`, with nothing sent, when `runtime().peer()` is null (feature off, no handoff, peer dropped), and when
   *   the process mode is `new_pending` (before `COMMIT` this process holds no app and must not end the old process's
   *   sessions) or `draining`;
   * - otherwise the result of `await_terminate_permission` (`peer_gone`, `ended`, or `refused` at the end of the budget;
   *   an `END_REJECTED` is treated as a timeout there).
   *
   * Overlap: concurrent callers each send their own `END_ALL` (distinct request ids) and each gets its own answer. The
   * peer is read once per call, so a peer replaced or dropped meanwhile does not change a running call.
   *
   * Thread-safety: any thread; blocks up to `budget`. Holds no lock while it waits. Must not be called while holding the
   * RTSP slots lock, the clear lock, a `proc` lock, `client_auth_mutex`, or any lock the channel reader's handler takes.
   * A counted request keeps its in-flight guard while it waits; that is safe because a draining process exists only when
   * this process is not starting a handoff.
   *
   * @param budget `request_terminate_budget` or `shutdown_terminate_budget`.
   * @return Permission; see `terminate_permission_e` and `allows_terminate`.
   */
  terminate_permission_e terminate_gate(std::chrono::milliseconds budget);

  /**
   * @brief Terminate the app after permission was granted, through the single serialised place.
   *
   * Calls `proc::proc.terminate()` while holding one process-wide mutex that only this function and `terminate_app` use,
   * so concurrent request-path terminates run one after another (the second finds no app and does nothing). The mutex is
   * never held while waiting for permission.
   *
   * It terminates unconditionally; the caller decides whether it may be called. On the shutdown path, an app that this
   * process launched itself (`!proc::proc.adopted()`) is terminated through this function even after a refusal; an
   * adopted app is not (see "Shutdown and adopted apps" above).
   */
  void terminate_app_permitted();

  /**
   * @brief Ask for permission and, if granted, terminate the app.
   *
   * `terminate_gate(budget)`, then `terminate_app_permitted()` when `allows_terminate`. On a refusal it logs one Warning
   * naming `action` and does not terminate, except when `budget` is `shutdown_terminate_budget` and the app is not
   * adopted (`!proc::proc.adopted()`): then, after the same Warning, it terminates anyway, so a shutdown never leaves an
   * app this process started. The return value is still the gate's permission.
   *
   * @param budget Budget for the gate.
   * @param action Short generic name of the caller for the Warning, for example `"close app"`.
   * @return The permission, so the caller can tell a refusal.
   */
  terminate_permission_e terminate_app(std::chrono::milliseconds budget, std::string_view action);

  /**
   * @brief Number of sessions the draining process reports.
   *
   * @param timeout Longest wait for the `SESSIONS?` reply.
   * @return 0 with nothing sent when no peer exists or the peer is gone; the number of entries of the reply; and
   *         `std::nullopt` (unknown) when a peer exists that is not gone and gave no answer in time, or the peer has no
   *         session knowledge (it is not a `session_peer_t`). Thread-safety: any thread; blocks up to `timeout`.
   */
  std::optional<std::size_t> draining_session_count(std::chrono::milliseconds timeout = peer_request_timeout);

  /**
   * @brief The `/launch` guard.
   *
   * @return Empty when `/launch` may proceed: no peer, or the peer reports no sessions. A non-empty one-line text (no
   *         identities) when `draining_session_count()` is above 0 or unknown (a timed-out `SESSIONS?` counts as non-zero).
   *         Thread-safety: any thread; blocks up to 500 ms.
   */
  std::string launch_refusal();

#ifdef SUNSHINE_TESTS
  /**
   * @brief Test-only: replace the call of `proc::proc.terminate()` made by `terminate_app_permitted` (and so by
   * `terminate_app`) with `terminate`. The serialising mutex still applies.
   *
   * @param terminate The function to call; an empty function restores the real call.
   */
  void set_terminate_for_test(std::function<void()> terminate);
#endif
}  // namespace handoff
