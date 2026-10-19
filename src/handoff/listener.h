/**
 * @file src/handoff/listener.h
 * @brief A Simple-Web-Server server that can adopt an existing listening socket and pause and resume accepting.
 *
 * See `docs/drain-handoff.md`, "Pausing accept" and switch step 5, and the plan decision D2. The third-party
 * submodule is not patched: `adoptable_server_t<Base>` derives from a Simple-Web-Server server class (`Base` is
 * `SimpleWeb::Server<SimpleWeb::HTTP>`, `SimpleWeb::Server<SimpleWeb::HTTPS>` or Sunshine's own `SunshineHTTPSServer`
 * in `nvhttp.cpp`) and uses only these members of `SimpleWeb::ServerBase` (all `protected` or `public`):
 * - `acceptor` (`std::unique_ptr<asio::ip::tcp::acceptor>`), `io_service` (`std::shared_ptr<io_context>`),
 *   `config` (`port`, `address`, `thread_pool_size`), `start_stop_mutex`, `internal_io_service` and `threads`;
 * - the virtual `accept()` (pure in `ServerBase`, overridden by every `Base`) and the virtual `after_bind()`;
 * - the non-virtual `start()` and `stop()`, which are called unchanged.
 *
 * Every `Base::accept()` has the same shape: it arms `acceptor->async_accept`, and the completion handler first calls
 * `this->accept()` again (a virtual call) unless the error is `operation_aborted`, then handles the connection. That
 * re-arm through the virtual is the only hook the pause needs.
 *
 * Linux only. `listener_fd()`, the `::dup` in `start_adopted` and the handoff itself exist on Linux only, so the
 * template is compiled out elsewhere and the servers keep the plain Simple-Web-Server types (see the aliases in
 * `nvhttp.h` and `confighttp.h`). Nothing in this file may be included from code that is built on other platforms
 * without an `#ifdef __linux__`.
 *
 * Threading model (required, checked by `start_adopted`): the server owns its io thread (the thread that runs
 * `start()` or `start_adopted()`), `config.thread_pool_size` is 1 and `io_service` is left unset so the server creates
 * its own. All completion handlers, including every request handler, therefore run on that one thread, which is what
 * makes `pause` race-free. A caller that needs another configuration gets `std::invalid_argument` from
 * `start_adopted`; `pause` and the other members are not specified for such a server.
 *
 * Work guard: `asio::io_context::run()` returns when no handler is pending. A paused server has none, and a plain
 * `start()` server has only the pending accept, which `pause` cancels. So `start_adopted` takes an
 * `executor_work_guard` on `*io_service` before it runs the loop, and `pause` takes one on the io thread (if none is
 * held yet) before it cancels the accept. The guard is released by the destructor. The loop still ends when `stop()`
 * is called (`stop()` calls `io_service->stop()` for an internal io_service), which is how Sunshine ends its servers.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>

#ifdef __linux__
  // lib includes
  #include <boost/asio/executor_work_guard.hpp>
  #include <boost/asio/io_context.hpp>

namespace handoff {
  /**
   * @brief A Simple-Web-Server server that can adopt a listening socket, pause accepting and resume it.
   *
   * @tparam Base A Simple-Web-Server server class: `SimpleWeb::Server<SimpleWeb::HTTP>`,
   *         `SimpleWeb::Server<SimpleWeb::HTTPS>` or `SunshineHTTPSServer`. It must be default or argument
   *         constructible through public constructors (they are inherited), must override `accept()` in the
   *         `ServerBase` way described in the file comment, and must not make `accept()` private.
   *
   * What stays as upstream: an unadopted `start()` (inherited, not hidden) behaves exactly as in the base class. The
   * only differences for a server that is never paused are that `accept()` first tests a flag that is false, and that
   * `listener_fd()` and `paused()` exist. `stop()` and the destructor are the base class's: `stop()` closes the
   * acceptor and every open connection and ends the loop of an internal io_service; the destructor stops the server.
   * As upstream, the owner must call `stop()` and join the io thread (the thread inside `start`/`start_adopted`)
   * before it destroys the server. `pause`, `resume`, `close_listener` and `listener_fd` must not run concurrently
   * with the destructor.
   *
   * In-flight connections: nothing here touches a connection that was accepted before a call. `pause` and
   * `close_listener` do not close, reset or time out any accepted connection; a connection that was accepted, or is in
   * its TLS handshake or in a keep-alive wait, continues to be served by this server's io thread, and the handoff's
   * 503 gate is what limits its requests. Connections that arrive while paused (or after `close_listener`) are not
   * accepted by this server: they stay in the kernel's listen queue of the shared socket, where the next process that
   * calls `accept` on it (the new process after `resume`, or this server after a rollback `resume`) takes them.
   *
   * Constructors: all constructors of `Base` are inherited. `pause`, `resume`, `close_listener`, `listener_fd` and
   * `paused` are inert (return false, -1 or false) before the first `start_adopted`/`start` has opened the acceptor
   * and after `stop()`.
   */
  template<class Base>
  class adoptable_server_t: public Base {
  public:
    using Base::Base;

    /**
     * @brief Start the server on a listening socket that already exists, and run its io loop on the calling thread.
     *
     * Same shape as `ServerBase::start` (it replaces `open`/`bind`/`listen`), under `start_stop_mutex`:
     * 1. Validate (below), then create the internal `io_service` and an unopened acceptor.
     * 2. `::dup(fd)` the socket and `acceptor->assign(protocol, dup)`. `protocol` is `tcp::v4()` or `tcp::v6()`, taken
     *    from `getsockname(fd)`'s address family. The caller's `fd` is never closed or modified by this call; the
     *    duplicate is owned by the acceptor and closed by `stop()` or the destructor, or by this function if a later
     *    step throws. The socket is already bound and listening, so `bind`, `listen` and `set_option(reuse_address)`
     *    are not called, and `config.address`/`config.port` are not used (the callback receives the port from
     *    `local_endpoint()`). The open file description, and so its non-blocking flag and its accept queue, is shared
     *    with every other holder of the socket.
     * 3. Call the virtual `after_bind()` (HTTPS verification setup), as `start()` does.
     * 4. Take the work guard, set the paused flag to `paused`, and, if `paused` is false, call `accept()` (the same
     *    call `start()` makes).
     * 5. Post `callback(port)` to the io thread (as `start()` does), so it runs once the loop runs. It is the
     *    "listening" signal for `READY`: for a paused start, accepting is armed only by `resume()`.
     * 6. Run `io_service->run()` on the calling thread. It returns only after `stop()` (or an exception thrown from a
     *    handler, as upstream), then the function returns.
     *
     * A start with `paused == true` accepts nothing and arms no accept; connections queue in the socket's backlog.
     *
     * @param fd A bound, listening, stream TCP socket (`AF_INET` or `AF_INET6`), for example one inherited from the
     *        old process.
     * @param paused Whether accepting starts paused (the new process before `COMMIT`).
     * @param callback Called once on the io thread with the local port. May be empty.
     *
     * @throws std::invalid_argument `fd < 0`; `io_service` was set by the caller; `config.thread_pool_size != 1`; or
     *         the server was already started and not stopped. Nothing was changed.
     * @throws SimpleWeb::system_error (`boost::system::system_error`, as `start()`): `fd` is not a socket
     *         (`ENOTSOCK`), not a listening stream socket (`EINVAL`/`EOPNOTSUPP`, checked with `SO_ACCEPTCONN` and
     *         `SO_TYPE`), has another address family (`EAFNOSUPPORT`), `::dup` fails, or `assign` fails. The caller
     *         treats it as in `nvhttp::start`/`confighttp::start`: log fatal, raise the shutdown event.
     *
     * Thread-safety: call once, on the dedicated server thread (the same thread that would call `start()`).
     */
    void start_adopted(int fd, bool paused, const std::function<void(unsigned short)> &callback = nullptr);

    /**
     * @brief Stop accepting new connections, and wait until that is true.
     *
     * Posts to the io thread: take the work guard if none is held, set the paused flag, call `acceptor->cancel()`,
     * then fulfil a promise. The caller waits for the promise. Because everything runs on the one io thread:
     * - a pending accept is cancelled and its `operation_aborted` completion does not re-arm (the base handler
     *   re-arms only when `ec != operation_aborted`) and is silent when `on_error` is unset (as in Sunshine);
     * - an accept that succeeded before the posted handler runs has its completion handler run first, which calls
     *   `this->accept()`; with the flag already false that call arms a new accept, which the `cancel()` that follows
     *   cancels in the same posted handler; and a success that is handled after the flag is set finds it true in the
     *   overridden `accept()` and does not re-arm. Either way, when this function returns `true` no accept is armed.
     *   The connection of that last success is served as any other accepted connection.
     *
     * Idempotent: pausing a paused server returns `true` (it still waits for the io thread, so it is a barrier).
     *
     * @param timeout Longest wait for the io thread to run the posted handler.
     * @return `true` when, on return, accepting is paused and no accept is armed in this process. `false` when the
     *         server is not running (not started, stopped, or `close_listener` done: there is nothing to pause) or the
     *         io thread did not run the handler within `timeout`; in the timeout case the pause may still take effect
     *         later, so the caller treats `false` as a failed pause and calls `resume()` on its rollback path.
     *
     * Thread-safety: any thread except this server's io thread (a request handler, the `callback`, `on_error`):
     * calling it there would wait for itself, so it returns `false` at once without posting. Takes no lock of its own
     * that a handler could hold. Must not be called under a lock a request handler of this server takes.
     */
    bool pause(std::chrono::milliseconds timeout = std::chrono::milliseconds {5000});

    /**
     * @brief Resume accepting.
     *
     * Posts to the io thread: clear the paused flag and call `accept()`. Connections that queued in the listen
     * backlog while paused are accepted: asio's epoll reactor tries the `accept` system call as soon as the operation
     * is started, before it waits for readiness, so a queued connection is taken without a new arrival (to be verified
     * on the build host by the test "pause, connect, resume"). Idempotent: resuming a server that is not paused
     * returns `true` and arms nothing (never a second accept).
     *
     * @param timeout Longest wait for the io thread to run the posted handler.
     * @return `true` when accepting is armed (or already was). `false` when the server is not running (not started,
     *         stopped, or the listener was closed) or the handler did not run within `timeout` (it may still run).
     *
     * Thread-safety: as `pause`.
     */
    bool resume(std::chrono::milliseconds timeout = std::chrono::milliseconds {5000});

    /**
     * @brief Close this process's copy of the listening socket without touching accepted connections.
     *
     * For switch substep 10's `close_socket_copies`: after the switch the draining process must not hold a listener.
     * Posts to the io thread: cancel and close the acceptor under `start_stop_mutex`, set the paused flag (so nothing
     * re-arms). Unlike `stop()`, it does not close any connection and does not stop the io loop: in-flight
     * connections are served until they end, then the loop idles on the work guard until `stop()`. Idempotent. After
     * it `listener_fd()` is -1 and `resume()` returns `false`. The listening socket itself stays open in every other
     * process that holds it.
     *
     * @param timeout Longest wait for the io thread.
     * @return `true` when the acceptor is closed (or was); `false` when not started or on timeout.
     *
     * Thread-safety: as `pause`.
     */
    bool close_listener(std::chrono::milliseconds timeout = std::chrono::milliseconds {5000});

    /**
     * @brief The listening socket, for the old process to pass to a new one.
     *
     * Works for an adopted start and for an unadopted `start()`. The descriptor is owned by the acceptor: it stays
     * valid until `stop()`, `close_listener()` or destruction, and the caller must not close it. To hand it to a
     * child the caller passes it to `spawn_child` (which dups it onto the child's descriptor numbers) or `::dup`s it.
     *
     * @return `acceptor->native_handle()` while the acceptor is open; -1 when not started, stopped or closed.
     *
     * Thread-safety: any thread. Takes `start_stop_mutex` briefly (held by `start()` only during setup and by
     * `stop()`), never across a blocking call; do not call it from a handler that runs while `stop()` waits.
     */
    int listener_fd();

    /**
     * @brief Whether accepting is paused (set by `pause`, `close_listener`, or a start with `paused == true`).
     *
     * @return The flag. Thread-safety: any thread (atomic).
     */
    bool paused() const noexcept;

  protected:
    /**
     * @brief The `ServerBase::accept()` hook: arm one accept unless paused.
     *
     * `if (paused_) return; Base::accept();`. Called by `start()`, by `Base`'s completion handler on every non-aborted
     * completion (the re-arm), and by `resume()`. Runs on the io thread, except for the first call in `start()`,
     * which runs on the server thread before the loop (same thread in practice).
     */
    void accept() override;

  private:
    std::atomic<bool> paused_ {false};  ///< True while accepting is paused. Written on the io thread; read anywhere.
    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;  ///< Keeps `run()` alive while paused. Created by `start_adopted` before the loop or by `pause` on the io thread; destroyed by the destructor after the io thread ended.
  };
}  // namespace handoff
#endif
