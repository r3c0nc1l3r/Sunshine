/**
 * @file tests/unit/handoff/test_listener.cpp
 * @brief Blind tests for the adoptable Simple-Web-Server (src/handoff/listener.h), written from the class
 *        declaration and its documentation. Real loopback sockets and plain HTTP.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <arpa/inet.h>
  #include <atomic>
  #include <cerrno>
  #include <chrono>
  #include <cstring>
  #include <exception>
  #include <fcntl.h>
  #include <functional>
  #include <future>
  #include <memory>
  #include <netinet/in.h>
  #include <poll.h>
  #include <stdexcept>
  #include <string>
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <thread>
  #include <unistd.h>
  #include <vector>

  // lib includes
  #include <boost/system/system_error.hpp>
  #include <Simple-Web-Server/server_http.hpp>

  // local includes
  #include "src/handoff/listener.h"

namespace {
  using namespace std::chrono_literals;
  using http_server_t = handoff::adoptable_server_t<SimpleWeb::Server<SimpleWeb::HTTP>>;

  /**
   * @brief A bound, listening loopback TCP socket on an ephemeral port.
   */
  struct listener_socket_t {
    int fd = -1;  ///< The socket.
    unsigned short port = 0;  ///< Its port.

    listener_socket_t() {
      fd = ::socket(AF_INET, SOCK_STREAM, 0);
      sockaddr_in addr {};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      addr.sin_port = 0;
      int one = 1;
      ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
      if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(fd, 64) != 0) {
        ::close(fd);
        fd = -1;
        return;
      }
      socklen_t len = sizeof(addr);
      ::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len);
      port = ntohs(addr.sin_port);
    }

    ~listener_socket_t() {
      if (fd >= 0) {
        ::close(fd);
      }
    }

    listener_socket_t(const listener_socket_t &) = delete;
    listener_socket_t &operator=(const listener_socket_t &) = delete;
  };

  /**
   * @brief The local port of a socket, or 0.
   */
  unsigned short port_of(int fd) {
    sockaddr_in addr {};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
      return 0;
    }
    return ntohs(addr.sin_port);
  }

  /**
   * @brief A plain blocking HTTP client with bounded reads.
   */
  class client_t {
  public:
    explicit client_t(unsigned short port) {
      fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
      sockaddr_in addr {};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      addr.sin_port = htons(port);
      connected_ = ::connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
    }

    ~client_t() {
      if (fd_ >= 0) {
        ::close(fd_);
      }
    }

    client_t(const client_t &) = delete;
    client_t &operator=(const client_t &) = delete;

    bool connected() const {
      return connected_;
    }

    void send_get(bool keep_alive) {
      const std::string req = std::string("GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: ") + (keep_alive ? "keep-alive" : "close") + "\r\n\r\n";
      ::send(fd_, req.data(), req.size(), MSG_NOSIGNAL);
    }

    /**
     * @brief Read until one complete response ("...\r\n\r\nok"), EOF or the limit; returns what arrived.
     */
    std::string read_response(std::chrono::milliseconds limit) {
      std::string data;
      const auto end = std::chrono::steady_clock::now() + limit;
      while (std::chrono::steady_clock::now() < end) {
        if (data.find("\r\n\r\nok") != std::string::npos) {
          break;
        }
        pollfd p {fd_, POLLIN, 0};
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
        if (::poll(&p, 1, static_cast<int>(left > 0 ? left : 0)) <= 0) {
          break;
        }
        char buf[1024];
        const auto n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n <= 0) {
          break;
        }
        data.append(buf, static_cast<std::size_t>(n));
      }
      return data;
    }

  private:
    int fd_ = -1;
    bool connected_ = false;
  };

  bool is_ok(const std::string &response) {
    return response.starts_with("HTTP/1.1 200") && response.find("\r\n\r\nok") != std::string::npos;
  }

  /**
   * @brief Runs a server's start or start_adopted on a thread and stops it reliably.
   */
  class server_runner_t {
  public:
    server_runner_t():
        server(std::make_unique<http_server_t>()) {
      server->default_resource["GET"] = [this](std::shared_ptr<http_server_t::Response> response, std::shared_ptr<http_server_t::Request>) {
        ++handled;
        response->write(SimpleWeb::StatusCode::success_ok, "ok");
      };
    }

    ~server_runner_t() {
      stop();
    }

    /**
     * @brief Run start_adopted; waits for the callback.
     */
    bool run_adopted(int fd, bool paused) {
      return launch([this, fd, paused] {
        server->start_adopted(fd, paused, [this](unsigned short port) {
          port_promise_.set_value(port);
        });
      });
    }

    /**
     * @brief Run the unadopted start on 127.0.0.1, port 0.
     */
    bool run_plain() {
      server->config.address = "127.0.0.1";
      server->config.port = 0;
      return launch([this] {
        server->start([this](unsigned short port) {
          port_promise_.set_value(port);
        });
      });
    }

    void stop() {
      if (!thread_.joinable()) {
        return;
      }
      server->stop();
      if (done_future_.wait_for(5s) == std::future_status::ready) {
        thread_.join();
      } else {
        ADD_FAILURE() << "the io thread did not return within 5 s of stop()";
        thread_.detach();
        // Leak the server on purpose: the thread may still use it.
        (void) server.release();
      }
    }

    bool finished(std::chrono::milliseconds limit) {
      return done_future_.valid() && done_future_.wait_for(limit) == std::future_status::ready;
    }

    std::unique_ptr<http_server_t> server;
    std::atomic<int> handled {0};
    unsigned short port = 0;
    std::exception_ptr error;

  private:
    bool launch(std::function<void()> body) {
      auto port_future = port_promise_.get_future();
      done_future_ = done_promise_.get_future();
      thread_ = std::thread([this, body = std::move(body)] {
        try {
          body();
        } catch (...) {
          error = std::current_exception();
          try {
            port_promise_.set_value(0);
          } catch (...) {}
        }
        done_promise_.set_value();
      });
      if (port_future.wait_for(3s) != std::future_status::ready) {
        return false;
      }
      port = port_future.get();
      return port != 0;
    }

    std::promise<unsigned short> port_promise_;
    std::promise<void> done_promise_;
    std::future<void> done_future_;
    std::thread thread_;
  };
}  // namespace

TEST(HandoffListenerTest, InertBeforeStart) {
  http_server_t server;
  EXPECT_EQ(server.listener_fd(), -1) << "listener_fd must be -1 before start";
  EXPECT_FALSE(server.paused()) << "a new server is not paused";
  EXPECT_FALSE(server.pause(100ms)) << "pause before start must return false";
  EXPECT_FALSE(server.resume(100ms)) << "resume before start must return false";
  EXPECT_FALSE(server.close_listener(100ms)) << "close_listener before start must return false";
}

TEST(HandoffListenerTest, AdoptedServerServesARequest) {
  listener_socket_t ls;
  ASSERT_GE(ls.fd, 0);
  server_runner_t r;
  ASSERT_TRUE(r.run_adopted(ls.fd, false)) << "start_adopted must call the callback with a port";
  EXPECT_EQ(r.port, ls.port) << "the callback must receive the adopted socket's port";
  client_t c {ls.port};
  ASSERT_TRUE(c.connected());
  c.send_get(false);
  const auto response = c.read_response(2s);
  EXPECT_TRUE(is_ok(response)) << "an adopted, unpaused server must serve a request, got: " << response;
  EXPECT_EQ(r.handled.load(), 1);
}

TEST(HandoffListenerTest, AdoptedFdStaysOpenAndOwnedByCaller) {
  listener_socket_t ls;
  ASSERT_GE(ls.fd, 0);
  {
    server_runner_t r;
    ASSERT_TRUE(r.run_adopted(ls.fd, false));
    const int own = r.server->listener_fd();
    EXPECT_GE(own, 0) << "listener_fd must be valid while running";
    EXPECT_NE(own, ls.fd) << "the server must work on a duplicate of the caller's fd";
    EXPECT_EQ(port_of(own), ls.port) << "listener_fd must be the same socket";
    r.stop();
    EXPECT_EQ(r.server->listener_fd(), -1) << "listener_fd must be -1 after stop";
  }
  EXPECT_NE(::fcntl(ls.fd, F_GETFD), -1) << "the caller's fd must never be closed by the server";
  // The caller's socket still listens: a connection can be made.
  client_t c {ls.port};
  EXPECT_TRUE(c.connected()) << "the caller's listening socket must still work after the server stopped";
}

TEST(HandoffListenerTest, StopEndsTheIoThread) {
  listener_socket_t ls;
  server_runner_t r;
  ASSERT_TRUE(r.run_adopted(ls.fd, false));
  r.server->stop();
  EXPECT_TRUE(r.finished(3s)) << "start_adopted must return after stop()";
  EXPECT_FALSE(r.error) << "start_adopted must not throw on a normal stop";
  EXPECT_FALSE(r.server->pause(100ms)) << "pause after stop must return false";
  EXPECT_FALSE(r.server->resume(100ms)) << "resume after stop must return false";
  r.stop();
}

TEST(HandoffListenerTest, PausedStartAcceptsNothingUntilResume) {
  listener_socket_t ls;
  server_runner_t r;
  ASSERT_TRUE(r.run_adopted(ls.fd, true)) << "the callback must run for a paused start too";
  EXPECT_TRUE(r.server->paused()) << "paused() must be true after a paused start";
  client_t c {ls.port};
  ASSERT_TRUE(c.connected()) << "the kernel queues connections while the server is paused";
  c.send_get(false);
  EXPECT_TRUE(c.read_response(300ms).empty()) << "a paused server must not answer";
  EXPECT_EQ(r.handled.load(), 0);
  ASSERT_TRUE(r.server->resume(2000ms)) << "resume must return true on a running server";
  EXPECT_FALSE(r.server->paused());
  const auto response = c.read_response(2s);
  EXPECT_TRUE(is_ok(response)) << "the connection queued while paused must be accepted after resume, got: " << response;
}

TEST(HandoffListenerTest, PauseConnectResumeAcceptsQueuedConnection) {
  listener_socket_t ls;
  server_runner_t r;
  ASSERT_TRUE(r.run_adopted(ls.fd, false));
  ASSERT_TRUE(r.server->pause(2000ms)) << "pause must return true on a running server";
  EXPECT_TRUE(r.server->paused());
  client_t c {ls.port};
  ASSERT_TRUE(c.connected());
  c.send_get(false);
  EXPECT_TRUE(c.read_response(300ms).empty()) << "no accept may be armed after pause returned true";
  ASSERT_TRUE(r.server->resume(2000ms));
  const auto response = c.read_response(2s);
  EXPECT_TRUE(is_ok(response)) << "the queued connection must be accepted after resume without a new arrival, got: " << response;
}

TEST(HandoffListenerTest, PauseIsIdempotentAndResumeIsIdempotent) {
  listener_socket_t ls;
  server_runner_t r;
  ASSERT_TRUE(r.run_adopted(ls.fd, false));
  EXPECT_TRUE(r.server->resume(2000ms)) << "resume on a server that is not paused returns true";
  EXPECT_TRUE(r.server->pause(2000ms));
  EXPECT_TRUE(r.server->pause(2000ms)) << "pausing a paused server returns true";
  EXPECT_TRUE(r.server->paused());
  EXPECT_TRUE(r.server->resume(2000ms));
  EXPECT_TRUE(r.server->resume(2000ms)) << "resuming twice returns true";
  EXPECT_FALSE(r.server->paused());
  // One accept only: two sequential clients are both served exactly once.
  for (int i = 0; i < 2; ++i) {
    client_t c {ls.port};
    ASSERT_TRUE(c.connected());
    c.send_get(false);
    EXPECT_TRUE(is_ok(c.read_response(2s))) << "request " << i << " not served after pause/resume cycles";
  }
  EXPECT_EQ(r.handled.load(), 2);
}

TEST(HandoffListenerTest, RepeatedPauseResumeCyclesKeepServing) {
  listener_socket_t ls;
  server_runner_t r;
  ASSERT_TRUE(r.run_adopted(ls.fd, false));
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(r.server->pause(2000ms)) << "pause failed in cycle " << i;
    ASSERT_TRUE(r.server->resume(2000ms)) << "resume failed in cycle " << i;
    client_t c {ls.port};
    ASSERT_TRUE(c.connected());
    c.send_get(false);
    EXPECT_TRUE(is_ok(c.read_response(2s))) << "request not served after cycle " << i;
  }
  EXPECT_EQ(r.handled.load(), 5);
}

TEST(HandoffListenerTest, PauseAndResumeReturnFalseOnTheIoThread) {
  listener_socket_t ls;
  server_runner_t r;
  std::atomic<int> state {0};  // 0 = not run, 1 = done
  std::atomic<bool> pause_result {true};
  std::atomic<bool> resume_result {true};
  std::atomic<bool> close_result {true};
  std::atomic<long long> elapsed_ms {-1};
  r.server->default_resource["GET"] = [&](std::shared_ptr<http_server_t::Response> response, std::shared_ptr<http_server_t::Request>) {
    const auto t0 = std::chrono::steady_clock::now();
    pause_result = r.server->pause(3000ms);
    resume_result = r.server->resume(3000ms);
    close_result = r.server->close_listener(3000ms);
    elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    state = 1;
    response->write(SimpleWeb::StatusCode::success_ok, "ok");
  };
  ASSERT_TRUE(r.run_adopted(ls.fd, false));
  client_t c {ls.port};
  ASSERT_TRUE(c.connected());
  c.send_get(false);
  EXPECT_TRUE(is_ok(c.read_response(3s))) << "the handler must complete (it must not wait for itself)";
  EXPECT_EQ(state.load(), 1);
  EXPECT_FALSE(pause_result.load()) << "pause called on the io thread must return false";
  EXPECT_FALSE(resume_result.load()) << "resume called on the io thread must return false";
  EXPECT_FALSE(close_result.load()) << "close_listener called on the io thread must return false";
  EXPECT_LT(elapsed_ms.load(), 1000) << "the calls on the io thread must return at once";
  EXPECT_FALSE(r.server->paused()) << "a refused pause must not change the flag";
}

TEST(HandoffListenerTest, PauseRacingAcceptsLeavesNoAcceptArmed) {
  listener_socket_t ls;
  server_runner_t r;
  ASSERT_TRUE(r.run_adopted(ls.fd, false));
  std::atomic<bool> stop_clients {false};
  std::vector<std::thread> clients;
  for (int t = 0; t < 3; ++t) {
    clients.emplace_back([&] {
      while (!stop_clients.load()) {
        client_t c {ls.port};
        if (c.connected()) {
          c.send_get(false);
          (void) c.read_response(200ms);
        }
      }
    });
  }
  std::this_thread::sleep_for(60ms);
  const bool paused = r.server->pause(3000ms);
  stop_clients = true;
  for (auto &t : clients) {
    t.join();
  }
  ASSERT_TRUE(paused) << "pause must return true while connections arrive";
  std::this_thread::sleep_for(150ms);  // let handlers of connections accepted before the pause finish
  const int before = r.handled.load();
  client_t late {ls.port};
  ASSERT_TRUE(late.connected());
  late.send_get(false);
  EXPECT_TRUE(late.read_response(300ms).empty()) << "a connection made after pause returned true must not be served (an accept was re-armed)";
  EXPECT_EQ(r.handled.load(), before) << "no request may be handled after pause returned true and in-flight work ended";
  ASSERT_TRUE(r.server->resume(2000ms));
  EXPECT_TRUE(is_ok(late.read_response(2s))) << "the late connection must be served after resume";
}

TEST(HandoffListenerTest, CloseListenerKeepsAcceptedConnectionsAndClosesTheAcceptor) {
  listener_socket_t ls;
  server_runner_t r;
  ASSERT_TRUE(r.run_adopted(ls.fd, false));
  client_t keep {ls.port};
  ASSERT_TRUE(keep.connected());
  keep.send_get(true);
  ASSERT_TRUE(is_ok(keep.read_response(2s)));
  ASSERT_GE(r.server->listener_fd(), 0);
  EXPECT_TRUE(r.server->close_listener(2000ms)) << "close_listener must return true on a running server";
  EXPECT_EQ(r.server->listener_fd(), -1) << "listener_fd must be -1 after close_listener";
  EXPECT_TRUE(r.server->paused()) << "close_listener sets the paused flag";
  EXPECT_TRUE(r.server->close_listener(2000ms)) << "close_listener is idempotent";
  EXPECT_FALSE(r.server->resume(500ms)) << "resume after close_listener must return false";
  EXPECT_FALSE(r.server->pause(500ms)) << "pause after close_listener must return false (nothing to pause)";
  // The accepted keep-alive connection is still served by the same server.
  keep.send_get(false);
  const auto second = keep.read_response(2s);
  EXPECT_TRUE(is_ok(second)) << "an accepted connection must survive close_listener, got: " << second;
  // New connections are not accepted here (the caller's socket still queues them).
  client_t late {ls.port};
  ASSERT_TRUE(late.connected()) << "the caller's own copy of the socket still listens";
  late.send_get(false);
  EXPECT_TRUE(late.read_response(300ms).empty()) << "after close_listener this server must not accept";
  EXPECT_FALSE(r.finished(50ms)) << "close_listener must not stop the io loop";
}

TEST(HandoffListenerTest, StopAfterCloseListenerEndsTheLoop) {
  listener_socket_t ls;
  server_runner_t r;
  ASSERT_TRUE(r.run_adopted(ls.fd, false));
  ASSERT_TRUE(r.server->close_listener(2000ms));
  r.server->stop();
  EXPECT_TRUE(r.finished(3s)) << "stop() must end the loop that idles on the work guard";
  r.stop();
}

TEST(HandoffListenerTest, PausedServerStaysAliveAndStopEndsIt) {
  listener_socket_t ls;
  server_runner_t r;
  ASSERT_TRUE(r.run_adopted(ls.fd, true));
  std::this_thread::sleep_for(150ms);
  EXPECT_FALSE(r.finished(50ms)) << "a paused server holds the io loop open (work guard)";
  r.server->stop();
  EXPECT_TRUE(r.finished(3s)) << "stop() must end a paused server";
}

TEST(HandoffListenerTest, StartAdoptedRejectsOtherThreadPoolSizes) {
  listener_socket_t ls;
  http_server_t server;
  server.config.thread_pool_size = 2;
  EXPECT_THROW(server.start_adopted(ls.fd, false), std::invalid_argument) << "thread_pool_size != 1 must throw std::invalid_argument";
  server.config.thread_pool_size = 0;
  EXPECT_THROW(server.start_adopted(ls.fd, false), std::invalid_argument) << "thread_pool_size 0 must throw std::invalid_argument";
  EXPECT_EQ(server.listener_fd(), -1) << "a rejected start changes nothing";
}

TEST(HandoffListenerTest, RejectedStartLeavesServerStartable) {
  listener_socket_t ls;
  server_runner_t r;
  r.server->config.thread_pool_size = 3;
  EXPECT_THROW(r.server->start_adopted(ls.fd, false), std::invalid_argument);
  r.server->config.thread_pool_size = 1;
  ASSERT_TRUE(r.run_adopted(ls.fd, false)) << "after a rejected start the server must start normally";
  client_t c {ls.port};
  c.send_get(false);
  EXPECT_TRUE(is_ok(c.read_response(2s)));
}

TEST(HandoffListenerTest, StartAdoptedRejectsNegativeFd) {
  http_server_t server;
  EXPECT_THROW(server.start_adopted(-1, false), std::invalid_argument) << "fd < 0 must throw std::invalid_argument";
}

TEST(HandoffListenerTest, StartAdoptedRejectsNonSocket) {
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);
  http_server_t server;
  EXPECT_THROW(server.start_adopted(fds[0], false), boost::system::system_error) << "a pipe is not a socket (ENOTSOCK)";
  ::close(fds[0]);
  ::close(fds[1]);
}

TEST(HandoffListenerTest, StartAdoptedRejectsNonListeningSocket) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  http_server_t server;
  EXPECT_THROW(server.start_adopted(fd, false), boost::system::system_error) << "a socket that does not listen must be rejected";
  EXPECT_NE(::fcntl(fd, F_GETFD), -1) << "the caller's fd must not be closed by a failed start";
  ::close(fd);
}

TEST(HandoffListenerTest, StartAdoptedRejectsDatagramSocket) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(fd, 0);
  http_server_t server;
  EXPECT_THROW(server.start_adopted(fd, false), boost::system::system_error) << "a UDP socket must be rejected";
  ::close(fd);
}

TEST(HandoffListenerTest, StartAdoptedRejectsCallerSetIoService) {
  listener_socket_t ls;
  http_server_t server;
  server.io_service = std::make_shared<boost::asio::io_context>();
  EXPECT_THROW(server.start_adopted(ls.fd, false), std::invalid_argument) << "a caller-provided io_service must be rejected";
}

TEST(HandoffListenerTest, StartAdoptedRejectsSecondStart) {
  listener_socket_t ls;
  server_runner_t r;
  ASSERT_TRUE(r.run_adopted(ls.fd, false));
  EXPECT_THROW(r.server->start_adopted(ls.fd, false), std::invalid_argument) << "starting a running server must throw std::invalid_argument";
  client_t c {ls.port};
  c.send_get(false);
  EXPECT_TRUE(is_ok(c.read_response(2s))) << "the running server must be unaffected by a rejected second start";
}

TEST(HandoffListenerTest, EmptyCallbackIsAccepted) {
  listener_socket_t ls;
  http_server_t server;
  std::promise<void> done;
  auto done_future = done.get_future();
  std::thread t([&] {
    try {
      server.start_adopted(ls.fd, false);
    } catch (...) {
      ADD_FAILURE() << "start_adopted with no callback must not throw";
    }
    done.set_value();
  });
  bool listening = false;
  for (int i = 0; i < 400 && !listening; ++i) {
    listening = server.listener_fd() >= 0;
    if (!listening) {
      std::this_thread::sleep_for(5ms);
    }
  }
  EXPECT_TRUE(listening) << "the server must start listening without a callback";
  server.stop();
  const bool ok = done_future.wait_for(3s) == std::future_status::ready;
  if (ok) {
    t.join();
  } else {
    t.detach();
  }
  EXPECT_TRUE(ok) << "the loop must end after stop()";
}

TEST(HandoffListenerTest, UnadoptedStartStillWorks) {
  server_runner_t r;
  ASSERT_TRUE(r.run_plain()) << "an unadopted start must call the callback";
  client_t c {r.port};
  ASSERT_TRUE(c.connected());
  c.send_get(false);
  EXPECT_TRUE(is_ok(c.read_response(2s))) << "an unadopted start must serve requests as the base class";
  const int fd = r.server->listener_fd();
  EXPECT_GE(fd, 0) << "listener_fd must work for an unadopted start";
  EXPECT_EQ(port_of(fd), r.port);
  r.stop();
  EXPECT_EQ(r.server->listener_fd(), -1);
}

TEST(HandoffListenerTest, UnadoptedServerSupportsPauseResume) {
  server_runner_t r;
  ASSERT_TRUE(r.run_plain());
  ASSERT_TRUE(r.server->pause(2000ms)) << "pause must work for an unadopted start";
  client_t c {r.port};
  ASSERT_TRUE(c.connected());
  c.send_get(false);
  EXPECT_TRUE(c.read_response(300ms).empty()) << "a paused unadopted server must not accept";
  ASSERT_TRUE(r.server->resume(2000ms));
  EXPECT_TRUE(is_ok(c.read_response(2s))) << "resume must accept the queued connection";
  r.stop();
  EXPECT_TRUE(r.finished(1s));
}

TEST(HandoffListenerTest, UnadoptedServerEndsOnStopAfterPause) {
  server_runner_t r;
  ASSERT_TRUE(r.run_plain());
  ASSERT_TRUE(r.server->pause(2000ms));
  r.server->stop();
  EXPECT_TRUE(r.finished(3s)) << "stop() must end the loop of a paused unadopted server";
}

TEST(HandoffListenerTest, UnadoptedServerCloseListener) {
  server_runner_t r;
  ASSERT_TRUE(r.run_plain());
  EXPECT_TRUE(r.server->close_listener(2000ms));
  EXPECT_EQ(r.server->listener_fd(), -1);
  EXPECT_FALSE(r.server->resume(300ms));
}
#endif
