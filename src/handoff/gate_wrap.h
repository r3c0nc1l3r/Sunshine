/**
 * @file src/handoff/gate_wrap.h
 * @brief Wrappers that put Simple-Web-Server route handlers behind the handoff 503 gate.
 *
 * See `docs/drain-handoff.md`, step 5, substeps 2 and 3. Templates need their bodies here; they are kept minimal.
 *
 * Handler shape: every Sunshine route handler is callable as `handler(response, request)` where both are the
 * `std::shared_ptr` types of a Simple-Web-Server `ServerBase<...>::Response` and `::Request` (see `resp_https_t`,
 * `req_https_t`, `resp_http_t`, `req_http_t` in `nvhttp.cpp` and `confighttp.cpp`). The wrappers are templates over
 * those two pointer types, so one implementation serves the HTTP, HTTPS and web UI servers. They need only these
 * members of the response: `close_connection_after_response` (a `bool` member) and
 * `write(SimpleWeb::StatusCode, const SimpleWeb::CaseInsensitiveMultimap &)`, so a test passes a fake response type.
 *
 * Refusal: when the gate refuses, the wrapper does not call the handler. It sets
 * `response->close_connection_after_response = true` and writes status 503 (`Service Unavailable`) with the single
 * header `Connection: close` and no body. It never reads the request.
 *
 * Asynchronous handlers: a counted guard covers the handler body only, from the wrapper's entry to the handler's
 * return, including every return path and an exception. Sunshine has one handler that answers after it returns: the
 * pairing request `/pair` (phase `getservercert`) stores its response in the pairing session and completes it after
 * the PIN is approved (`pair_session_t::async_insert_pin`). Such a pending response is not covered by the counter;
 * the handoff covers it separately by refusing and waiting on `nvhttp::pairing_session_count()` (spec step 1 and
 * switch substep 4). The web UI's `/api/pin` POST waits inside its handler, so the guard covers it. No other handler
 * stores its response.
 *
 * On non-Linux builds the handoff sources are not compiled, so the wrappers return the handler unchanged and
 * `wrap_resources()` does nothing.
 */
#pragma once

// standard includes
#include <functional>
#include <memory>
#include <string>
#include <utility>

#ifdef __linux__
  // lib includes
  #include <Simple-Web-Server/status_code.hpp>

  // local includes
  #include "gate.h"
#endif

namespace handoff {
#ifdef __linux__
  /**
   * @brief Answer a refused request: 503 with `Connection: close`, and close the connection after the response.
   *
   * @tparam Resp Response pointer type.
   * @param response Response to answer.
   */
  template<class Resp>
  void refuse_503(const Resp &response) {
    response->close_connection_after_response = true;
    response->write(SimpleWeb::StatusCode::server_error_service_unavailable, SimpleWeb::CaseInsensitiveMultimap {{"Connection", "close"}});
  }

  /**
   * @brief Wrap a handler of a counted route (a state-changing route, or `/applist` and `/appasset`).
   *
   * The returned callable does, in this order: `auto guard = gate.enter();` (increment, then check the gate); if the
   * guard is not admitted, `refuse_503(response)` and return, with the handler not called and the counter already
   * backed out; otherwise call `handler(response, request)` with the guard alive, so the in-flight counter drops
   * only after the handler has returned (also on an exception, which propagates unchanged).
   *
   * @tparam Resp Response pointer type.
   * @tparam Req Request pointer type.
   * @tparam Handler Callable invocable as `handler(Resp, Req)`; copied into the result.
   * @param handler Route handler.
   * @param gate Gate to use; defaults to the process-wide `route_gate()`. Tests pass their own.
   * @return A `std::function<void(Resp, Req)>` that can be assigned to a Simple-Web-Server resource. It holds a
   *         reference to `gate`, which must outlive it.
   */
  template<class Resp, class Req, class Handler>
  std::function<void(Resp, Req)> counted(Handler handler, gate_t &gate = route_gate()) {
    return [handler = std::move(handler), &gate](Resp response, Req request) mutable {
      auto guard = gate.enter();
      if (!guard.admitted()) {
        refuse_503(response);
        return;
      }
      handler(std::move(response), std::move(request));
    };
  }

  /**
   * @brief Wrap a handler of a gated but not counted route (a web UI page, a read-only API, an asset).
   *
   * The returned callable checks `gate.raised()` first; if raised, `refuse_503(response)` and return without calling
   * the handler; otherwise call `handler(response, request)`. Nothing is counted, so the switcher does not wait for
   * these handlers (they do not read state the switch changes).
   *
   * @tparam Resp Response pointer type.
   * @tparam Req Request pointer type.
   * @tparam Handler Callable invocable as `handler(Resp, Req)`; copied into the result.
   * @param handler Route handler.
   * @param gate Gate to use; defaults to the process-wide `route_gate()`.
   * @return A `std::function<void(Resp, Req)>`; it holds a reference to `gate`, which must outlive it.
   */
  template<class Resp, class Req, class Handler>
  std::function<void(Resp, Req)> gated(Handler handler, gate_t &gate = route_gate()) {
    return [handler = std::move(handler), &gate](Resp response, Req request) mutable {
      if (gate.raised()) {
        refuse_503(response);
        return;
      }
      handler(std::move(response), std::move(request));
    };
  }

  /**
   * @brief Wrap every handler of a Simple-Web-Server `resource` map in place (the web UI server).
   *
   * For each regular expression and each method: `POST`, `DELETE`, `PUT` and `PATCH` handlers become `counted`,
   * every other method (`GET`, `HEAD`, `OPTIONS`, ...) becomes `gated`. Call it once, after all routes are
   * registered and before the server starts; it is not safe to call while the server runs. `default_resource` is not
   * touched.
   *
   * @tparam Resp Response pointer type.
   * @tparam Req Request pointer type.
   * @tparam Resources Type of `server.resource`: a map from regular expression to a map from method name
   *         (`std::string`) to `std::function<void(Resp, Req)>`.
   * @param resources The resource map to rewrite.
   * @param gate Gate to use; defaults to the process-wide `route_gate()`.
   */
  template<class Resp, class Req, class Resources>
  void wrap_resources(Resources &resources, gate_t &gate = route_gate()) {
    for (auto &route : resources) {
      for (auto &method : route.second) {
        const auto &name = method.first;
        const bool state_changing = name == "POST" || name == "DELETE" || name == "PUT" || name == "PATCH";
        method.second = state_changing ? counted<Resp, Req>(std::move(method.second), gate) : gated<Resp, Req>(std::move(method.second), gate);
      }
    }
  }
#else
  /**
   * @brief Non-Linux: returns the handler unchanged.
   *
   * @tparam Resp Response pointer type.
   * @tparam Req Request pointer type.
   * @tparam Handler Callable invocable as `handler(Resp, Req)`.
   * @param handler Route handler.
   * @return The handler as a `std::function<void(Resp, Req)>`.
   */
  template<class Resp, class Req, class Handler>
  std::function<void(Resp, Req)> counted(Handler handler) {
    return handler;
  }

  /**
   * @brief Non-Linux: returns the handler unchanged.
   *
   * @tparam Resp Response pointer type.
   * @tparam Req Request pointer type.
   * @tparam Handler Callable invocable as `handler(Resp, Req)`.
   * @param handler Route handler.
   * @return The handler as a `std::function<void(Resp, Req)>`.
   */
  template<class Resp, class Req, class Handler>
  std::function<void(Resp, Req)> gated(Handler handler) {
    return handler;
  }

  /**
   * @brief Non-Linux: does nothing.
   *
   * @tparam Resp Response pointer type.
   * @tparam Req Request pointer type.
   * @tparam Resources Type of the resource map.
   * @param resources The resource map (unchanged).
   */
  template<class Resp, class Req, class Resources>
  void wrap_resources(Resources &resources) {
    (void) resources;
  }
#endif
}  // namespace handoff
