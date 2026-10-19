/**
 * @file src/handoff/channel_reader.cpp
 * @brief Definitions for `src/handoff/channel_reader.h`.
 */
// standard includes
#include <exception>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>

// local includes
#include "channel_reader.h"
#include "src/logging.h"

namespace handoff {
  namespace {
    constexpr std::chrono::milliseconds receive_slice {100};  ///< Receive timeout, so `stop()` is responsive.
  }  // namespace

  channel_reader_t::channel_reader_t(channel_t &channel, bool answer_pings, message_handler_t on_message, close_handler_t on_close):
      _channel(channel),
      _answer_pings(answer_pings),
      _on_message(std::move(on_message)),
      _on_close(std::move(on_close)) {
  }

  channel_reader_t::~channel_reader_t() {
    stop();
  }

  void channel_reader_t::start() {
    std::lock_guard lock {_mutex};
    if (_reader.joinable() || _worker.joinable() || _stopping) {
      return;
    }
    _worker = std::thread([this]() {
      // Worker: handles queued messages in order, then reports the close once.
      std::unique_lock lock {_mutex};
      while (true) {
        _cv.wait(lock, [this]() {
          return _stopping || _closed || !_queue.empty();
        });
        if (_stopping) {
          return;
        }
        if (!_queue.empty()) {
          received_t item = std::move(_queue.front());
          _queue.pop_front();
          lock.unlock();
          try {
            _on_message(std::move(item));
          } catch (const std::exception &e) {
            BOOST_LOG(error) << "Handoff channel handler failed: " << e.what();
          } catch (...) {
            BOOST_LOG(error) << "Handoff channel handler failed";
          }
          lock.lock();
          continue;
        }
        // Closed and the queue is drained.
        const auto status = _close_status;
        lock.unlock();
        try {
          if (_on_close) {
            _on_close(status);
          }
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Handoff channel close handler failed: " << e.what();
        } catch (...) {
          BOOST_LOG(error) << "Handoff channel close handler failed";
        }
        return;
      }
    });
    _reader = std::thread([this]() {
      // Reader: nothing but receive, answer PING inline and queue the rest.
      while (true) {
        {
          std::lock_guard lock {_mutex};
          if (_stopping) {
            return;
          }
        }
        received_t item;
        const auto status = _channel.receive(item, receive_slice);
        if (status == recv_status_e::timeout) {
          continue;
        }
        if (status != recv_status_e::ok) {
          std::lock_guard lock {_mutex};
          if (!_stopping) {
            _closed = true;
            _close_status = status;
          }
          _cv.notify_all();
          return;
        }
        if (_answer_pings) {
          if (const auto *ping = std::get_if<protocol::ping_t>(&item.message)) {
            // No locks other than the channel's send mutex. A failed send is a lost PONG, which the peer counts as a miss.
            _channel.send(protocol::pong_t {ping->seq});
            if (item.fd >= 0) {
              ::close(item.fd);
            }
            continue;
          }
        }
        {
          std::lock_guard lock {_mutex};
          if (!_stopping) {
            _queue.push_back(std::move(item));
            item.fd = -1;
            _cv.notify_all();
            continue;
          }
        }
        if (item.fd >= 0) {
          ::close(item.fd);
        }
        return;
      }
    });
  }

  void channel_reader_t::stop() {
    std::thread reader;
    std::thread worker;
    {
      std::lock_guard lock {_mutex};
      _stopping = true;
      _cv.notify_all();
      // Drop queued messages, closing their fds.
      for (auto &item : _queue) {
        if (item.fd >= 0) {
          ::close(item.fd);
        }
      }
      _queue.clear();
      // Take the threads out so concurrent stops cannot join one thread twice. A handler that calls `stop()` runs on
      // the worker: it leaves the worker for a later `stop()` (the destructor) to join.
      if (_reader.joinable() && _reader.get_id() != std::this_thread::get_id()) {
        reader = std::move(_reader);
      }
      if (_worker.joinable() && _worker.get_id() != std::this_thread::get_id()) {
        worker = std::move(_worker);
      }
    }
    if (reader.joinable()) {
      reader.join();
    }
    if (worker.joinable()) {
      worker.join();
    }
  }
}  // namespace handoff
