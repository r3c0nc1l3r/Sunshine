/**
 * @file src/handoff/listener_impl.h
 * @brief Definitions of the `handoff::adoptable_server_t` member functions declared in `handoff/listener.h`.
 *
 * Included at the end of `handoff/listener.h`; do not include it directly. See that file for the contract.
 */
#pragma once

#ifdef __linux__
  // standard includes
  #include <cerrno>
  #include <future>
  #include <memory>
  #include <mutex>
  #include <stdexcept>
  #include <utility>

  // lib includes
  #include <boost/asio/ip/tcp.hpp>
  #include <boost/asio/post.hpp>
  #include <boost/system/error_code.hpp>
  #include <boost/system/system_error.hpp>

  // platform includes
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <unistd.h>

namespace handoff {
  namespace detail {
    /**
     * @brief Throw a `boost::system::system_error` for an errno value.
     *
     * @param err The errno value.
     * @param what The message prefix.
     */
    [[noreturn]] inline void throw_errno(int err, const char *what) {
      throw boost::system::system_error(boost::system::error_code(err, boost::system::system_category()), what);
    }

    /**
     * @brief Run a function on an io_context's thread and wait for its result.
     *
     * The function runs inside a handler posted to `ios`. If the wait times out the handler may still run later; it
     * owns its shared state, so a late completion is harmless.
     *
     * @tparam F A callable returning `bool`.
     * @param ios The io_context whose thread runs the function.
     * @param timeout Longest wait.
     * @param fn The function.
     * @return The function's result, or `false` on timeout.
     */
    template<class F>
    bool run_on_io_thread(const std::shared_ptr<boost::asio::io_context> &ios, std::chrono::milliseconds timeout, F fn) {
      auto promise = std::make_shared<std::promise<bool>>();
      auto future = promise->get_future();
      boost::asio::post(*ios, [promise, fn = std::move(fn)]() mutable {
        try {
          promise->set_value(fn());
        } catch (...) {
          promise->set_value(false);
        }
      });
      if (future.wait_for(timeout) != std::future_status::ready) {
        return false;
      }
      return future.get();
    }
  }  // namespace detail

  template<class Base>
  void adoptable_server_t<Base>::start_adopted(int fd, bool paused, const std::function<void(unsigned short)> &callback) {
    std::unique_lock<std::mutex> lock(this->start_stop_mutex);

    // 1. Validate; nothing is changed by these errors.
    if (fd < 0) {
      throw std::invalid_argument("start_adopted: invalid file descriptor");
    }
    if (this->io_service && !this->internal_io_service) {
      throw std::invalid_argument("start_adopted: io_service must be left unset");
    }
    if (this->config.thread_pool_size != 1) {
      throw std::invalid_argument("start_adopted: thread_pool_size must be 1");
    }
    if (this->acceptor && this->acceptor->is_open()) {
      throw std::invalid_argument("start_adopted: server already started");
    }

    // The descriptor must be a listening stream socket of a supported family.
    int sock_type = 0;
    socklen_t opt_len = sizeof(sock_type);
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &sock_type, &opt_len) != 0) {
      detail::throw_errno(errno, "start_adopted: getsockopt(SO_TYPE)");
    }
    if (sock_type != SOCK_STREAM) {
      detail::throw_errno(EINVAL, "start_adopted: not a stream socket");
    }
    int listening = 0;
    opt_len = sizeof(listening);
    if (::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &opt_len) != 0) {
      detail::throw_errno(errno, "start_adopted: getsockopt(SO_ACCEPTCONN)");
    }
    if (listening == 0) {
      detail::throw_errno(EINVAL, "start_adopted: not a listening socket");
    }
    sockaddr_storage addr {};
    socklen_t addr_len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &addr_len) != 0) {
      detail::throw_errno(errno, "start_adopted: getsockname");
    }
    boost::asio::ip::tcp protocol = boost::asio::ip::tcp::v4();
    if (addr.ss_family == AF_INET6) {
      protocol = boost::asio::ip::tcp::v6();
    } else if (addr.ss_family != AF_INET) {
      detail::throw_errno(EAFNOSUPPORT, "start_adopted: unsupported address family");
    }

    // 2. Create the internal io_service and an unopened acceptor, then take a duplicate of the socket.
    if (!this->io_service) {
      this->io_service = std::make_shared<boost::asio::io_context>();
      this->internal_io_service = true;
    }
    const bool created_acceptor = !this->acceptor;
    if (created_acceptor) {
      this->acceptor = std::make_unique<boost::asio::ip::tcp::acceptor>(*this->io_service);
    }

    const int dup_fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0) {
      if (created_acceptor) {
        this->acceptor.reset();  // never opened: close_listener() must see "not started"
      }
      detail::throw_errno(errno, "start_adopted: dup");
    }
    boost::system::error_code ec;
    this->acceptor->assign(protocol, dup_fd, ec);
    if (ec) {
      ::close(dup_fd);
      if (created_acceptor) {
        this->acceptor.reset();
      }
      throw boost::system::system_error(ec, "start_adopted: assign");
    }

    unsigned short port = 0;
    try {
      // 3. HTTPS verification setup, as start() does.
      this->after_bind();
      port = this->acceptor->local_endpoint().port();

      if (this->internal_io_service && this->io_service->stopped()) {
        this->io_service->restart();
      }

      // 4. Keep run() alive while paused, then arm the first accept unless starting paused.
      work_guard_.reset();
      work_guard_.emplace(boost::asio::make_work_guard(*this->io_service));
      paused_.store(paused);
      if (!paused) {
        this->accept();
      }

      // 5. The "listening" signal.
      if (callback) {
        boost::asio::post(*this->io_service, [callback, port] {
          callback(port);
        });
      }
    } catch (...) {
      boost::system::error_code close_ec;
      this->acceptor->close(close_ec);
      work_guard_.reset();
      if (created_acceptor) {
        this->acceptor.reset();
      }
      throw;
    }

    // 6. Run the loop on this thread without the mutex, as start() does.
    this->threads.clear();
    lock.unlock();
    this->io_service->run();
    lock.lock();
  }

  template<class Base>
  bool adoptable_server_t<Base>::pause(std::chrono::milliseconds timeout) {
    std::shared_ptr<boost::asio::io_context> ios;
    {
      std::lock_guard<std::mutex> lock(this->start_stop_mutex);
      if (!(this->acceptor && this->acceptor->is_open()) || !this->io_service) {
        return false;
      }
      ios = this->io_service;
    }
    if (ios->get_executor().running_in_this_thread()) {
      return false;
    }

    return detail::run_on_io_thread(ios, timeout, [this]() {
      if (!work_guard_) {
        work_guard_.emplace(boost::asio::make_work_guard(*this->io_service));
      }
      paused_.store(true);
      std::lock_guard<std::mutex> lock(this->start_stop_mutex);
      if (!(this->acceptor && this->acceptor->is_open())) {
        return false;
      }
      boost::system::error_code ec;
      this->acceptor->cancel(ec);
      return true;
    });
  }

  template<class Base>
  bool adoptable_server_t<Base>::resume(std::chrono::milliseconds timeout) {
    std::shared_ptr<boost::asio::io_context> ios;
    {
      std::lock_guard<std::mutex> lock(this->start_stop_mutex);
      if (!(this->acceptor && this->acceptor->is_open()) || !this->io_service) {
        return false;
      }
      ios = this->io_service;
    }
    if (ios->get_executor().running_in_this_thread()) {
      return false;
    }

    return detail::run_on_io_thread(ios, timeout, [this]() {
      // Hold the mutex across the check and the arm so that a concurrent stop() cannot close the acceptor between them.
      std::lock_guard<std::mutex> lock(this->start_stop_mutex);
      if (!(this->acceptor && this->acceptor->is_open())) {
        return false;
      }
      if (paused_.exchange(false)) {
        this->accept();
      }
      return true;
    });
  }

  template<class Base>
  bool adoptable_server_t<Base>::close_listener(std::chrono::milliseconds timeout) {
    std::shared_ptr<boost::asio::io_context> ios;
    bool already_closed = false;
    {
      std::lock_guard<std::mutex> lock(this->start_stop_mutex);
      if (!this->acceptor || !this->io_service) {
        return false;
      }
      ios = this->io_service;
      already_closed = !this->acceptor->is_open();
    }
    if (ios->get_executor().running_in_this_thread()) {
      return false;
    }
    if (already_closed) {
      // Nothing to close, and after stop() the loop may not run a posted handler.
      paused_.store(true);
      return true;
    }

    return detail::run_on_io_thread(ios, timeout, [this]() {
      // Keep the loop alive after the acceptor is gone, until stop().
      if (!work_guard_) {
        work_guard_.emplace(boost::asio::make_work_guard(*this->io_service));
      }
      std::lock_guard<std::mutex> lock(this->start_stop_mutex);
      paused_.store(true);
      if (this->acceptor && this->acceptor->is_open()) {
        boost::system::error_code ec;
        this->acceptor->cancel(ec);
        this->acceptor->close(ec);
      }
      return true;
    });
  }

  template<class Base>
  int adoptable_server_t<Base>::listener_fd() {
    std::lock_guard<std::mutex> lock(this->start_stop_mutex);
    if (this->acceptor && this->acceptor->is_open()) {
      return static_cast<int>(this->acceptor->native_handle());
    }
    return -1;
  }

  template<class Base>
  bool adoptable_server_t<Base>::paused() const noexcept {
    return paused_.load();
  }

  template<class Base>
  void adoptable_server_t<Base>::accept() {
    // Note: `paused_` is not cleared by an unadopted `start()`. A restart after `pause()` or `close_listener()` and
    // `stop()` needs the flag cleared first (`start_adopted` sets it from its `paused` argument).
    if (paused_.load()) {
      return;
    }
    Base::accept();
  }
}  // namespace handoff
#endif
