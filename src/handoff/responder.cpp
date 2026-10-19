/**
 * @file src/handoff/responder.cpp
 * @brief Implementation of `src/handoff/responder.h` (wave C).
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <limits>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>

// local includes
#include "responder.h"
#include "src/logging.h"
#include "src/process.h"
#include "src/rtsp.h"

namespace handoff {
  namespace {
    using clock_t_ = std::chrono::steady_clock;

    /**
     * @brief The production operations, bound to the RTSP server and the process object.
     *
     * @return The operations.
     */
    responder_ops_t production_ops() {
      responder_ops_t ops;
      ops.list_sessions = []() {
        std::vector<protocol::session_entry_t> entries;
        for (const auto &info : rtsp_stream::list_sessions()) {
          protocol::session_entry_t entry;
          entry.id = info.id;
          entry.started_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(info.started.time_since_epoch()).count();
          entry.transport = info.transport;
          entry.client_category = info.client_category;
          entries.push_back(std::move(entry));
        }
        return entries;
      };
      ops.close_session = [](std::uint32_t id) {
        switch (rtsp_stream::request_session_close(id)) {
          case rtsp_stream::close_result_e::closed:
            return close_outcome_e::closed;
          case rtsp_stream::close_result_e::already_stopping:
            return close_outcome_e::already_stopping;
          case rtsp_stream::close_result_e::not_found:
            break;
        }
        return close_outcome_e::not_found;
      };
      ops.close_cert = [](const std::string &hex) {
        const auto count = rtsp_stream::terminate_sessions_by_cert_hash(hex);
        return static_cast<std::uint32_t>(std::min<std::size_t>(count, std::numeric_limits<std::uint32_t>::max()));
      };
      ops.end_and_join = []() {
        rtsp_stream::terminate_sessions(stream::stop_reason_e::terminated);
        proc::proc.drop_released_app();
      };
      return ops;
    }

    /**
     * @brief Whether a string is exactly 64 lowercase hexadecimal characters.
     *
     * @param text String to check.
     * @return `true` when valid.
     */
    bool is_lower_hex64(const std::string &text) {
      if (text.size() != 64) {
        return false;
      }
      return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      });
    }
  }  // namespace

  drain_responder_t::drain_responder_t(channel_t &channel, end_all_machine_t &machine):
      _channel {channel},
      _machine {machine},
      _ops {production_ops()},
      _now {[]() {
        return clock_t_::now();
      }} {}

  drain_responder_t::~drain_responder_t() {
    stop();
  }

#ifdef SUNSHINE_TESTS
  drain_responder_t::drain_responder_t(
    channel_t &channel,
    end_all_machine_t &machine,
    responder_ops_t ops,
    std::function<std::chrono::steady_clock::time_point()> now
  ):
      _channel {channel},
      _machine {machine},
      _ops {std::move(ops)},
      _now {std::move(now)} {
    if (!_now) {
      _now = []() {
        return clock_t_::now();
      };
    }
  }

  std::size_t drain_responder_t::live_handlers_for_test() const {
    std::lock_guard lock {_mutex};
    return static_cast<std::size_t>(std::count_if(_handlers.begin(), _handlers.end(), [](const handler_t &h) {
      return !h.done->load();
    }));
  }
#endif

  bool drain_responder_t::spawn(const char *what, bool counted_against_cap, std::function<void()> work) {
    try {
      // Reap finished threads first. They are joined with no lock held.
      std::vector<handler_t> finished;
      {
        std::lock_guard lock {_mutex};
        if (_stopping) {
          return false;
        }
        for (auto it = _handlers.begin(); it != _handlers.end();) {
          if (it->done->load()) {
            finished.push_back(std::move(*it));
            it = _handlers.erase(it);
          } else {
            ++it;
          }
        }
      }
      for (auto &h : finished) {
        if (h.thread.joinable()) {
          h.thread.join();
        }
      }

      std::lock_guard lock {_mutex};
      if (_stopping) {
        return false;
      }
      if (counted_against_cap) {
        // The END_ALL runner is not counted. Its done flag is set in the critical section that clears the running
        // flag, so while the flag is set exactly one live handler is the runner.
        std::size_t alive = 0;
        for (const auto &h : _handlers) {
          if (!h.done->load()) {
            ++alive;
          }
        }
        if (_end_all_running && alive > 0) {
          --alive;
        }
        if (alive >= max_close_cert_threads) {
          BOOST_LOG(warning) << "Drain handoff: too many " << what << " handlers, request dropped";
          return false;
        }
      }

      _handlers.reserve(_handlers.size() + 1);
      auto done = std::make_shared<std::atomic<bool>>(false);
      std::thread thread {[work = std::move(work), done, what]() {
        try {
          work();
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Drain handoff: " << what << " handler failed: " << e.what();
        } catch (...) {
          BOOST_LOG(error) << "Drain handoff: " << what << " handler failed";
        }
        done->store(true);
      }};
      _handlers.push_back(handler_t {std::move(thread), std::move(done)});
      return true;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Drain handoff: cannot start the " << what << " handler: " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Drain handoff: cannot start the " << what << " handler";
    }
    return false;
  }

  void drain_responder_t::handle(received_t message) {
    // The responder never keeps an fd: close it before looking at the message.
    if (message.fd >= 0) {
      ::close(message.fd);
      message.fd = -1;
    }

    try {
      {
        std::lock_guard lock {_mutex};
        if (_stopping) {
          return;
        }
      }

      const auto send = [this](const protocol::message_t &reply, const char *what) {
        if (!_channel.send(reply)) {
          BOOST_LOG(warning) << "Drain handoff: cannot send the " << what << " reply";
        }
      };

      if (const auto *sreq = std::get_if<protocol::sessions_request_t>(&message.message)) {
        protocol::sessions_reply_t reply;
        reply.request_id = sreq->request_id;
        try {
          reply.sessions = _ops.list_sessions();
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Drain handoff: listing sessions failed: " << e.what();
          return;
        }
        send(reply, "SESSIONS");
      } else if (const auto *creq = std::get_if<protocol::close_request_t>(&message.message)) {
        protocol::close_reply_t reply;
        reply.request_id = creq->request_id;
        try {
          switch (_ops.close_session(creq->session_id)) {
            case close_outcome_e::closed:
              reply.result = "closed";
              break;
            case close_outcome_e::already_stopping:
              reply.result = "already-stopping";
              break;
            case close_outcome_e::not_found:
              reply.result = "not-found";
              break;
          }
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Drain handoff: closing a session failed: " << e.what();
          return;
        }
        send(reply, "CLOSE");
      } else if (const auto *ccreq = std::get_if<protocol::close_cert_request_t>(&message.message)) {
        const std::uint64_t request_id = ccreq->request_id;
        if (!is_lower_hex64(ccreq->cert_sha256)) {
          BOOST_LOG(warning) << "Drain handoff: CLOSE_CERT with an invalid hash, answered 0";
          protocol::close_cert_reply_t reply;
          reply.request_id = request_id;
          reply.count = 0;
          send(reply, "CLOSE_CERT");
          return;
        }
        spawn("CLOSE_CERT", true, [this, request_id, hash = ccreq->cert_sha256]() {
          const auto started = _now();
          protocol::close_cert_reply_t reply;
          reply.request_id = request_id;
          try {
            reply.count = _ops.close_cert(hash);
          } catch (const std::exception &e) {
            BOOST_LOG(error) << "Drain handoff: CLOSE_CERT failed: " << e.what();
            return;
          } catch (...) {
            BOOST_LOG(error) << "Drain handoff: CLOSE_CERT failed";
            return;
          }
          {
            std::lock_guard lock {_mutex};
            if (_stopping) {
              return;
            }
          }
          if (_now() - started > close_cert_late_after) {
            BOOST_LOG(warning) << "Drain handoff: late CLOSE_CERT reply";
          }
          if (!_channel.send(reply)) {
            BOOST_LOG(warning) << "Drain handoff: cannot send the CLOSE_CERT reply";
          }
        });
      } else if (const auto *ereq = std::get_if<protocol::end_all_t>(&message.message)) {
        bool start_runner = false;
        {
          std::lock_guard lock {_mutex};
          if (_stopping) {
            return;
          }
          _end_all_waiters.push_back(ereq->request_id);
          if (!_end_all_running) {
            _end_all_running = true;
            start_runner = true;
          }
        }
        if (!start_runner) {
          return;  // The running round answers this request.
        }

        const bool started = spawn("END_ALL", false, [this]() {
          const auto began = _now();
          end_all_answer_e answer = end_all_answer_e::rejected;
          try {
            answer = _machine.handle_end_all(_ops.end_and_join);
          } catch (const std::exception &e) {
            BOOST_LOG(error) << "Drain handoff: END_ALL failed: " << e.what();
          } catch (...) {
            BOOST_LOG(error) << "Drain handoff: END_ALL failed";
          }

          // Take the waiters and clear the running flag in one critical section. The thread marks itself done here
          // too, so that it is not counted against the CLOSE_CERT cap while it sends the replies (the cap count depends on
          // this). A reap may therefore join a runner that is still sending; that is brief, because sends do not block.
          std::vector<std::uint64_t> waiters;
          bool stopping = false;
          {
            std::lock_guard lock {_mutex};
            waiters.swap(_end_all_waiters);
            _end_all_running = false;
            stopping = _stopping;
            const auto self = std::this_thread::get_id();
            for (auto &h : _handlers) {
              if (h.thread.get_id() == self) {
                h.done->store(true);
              }
            }
          }
          if (stopping) {
            return;
          }
          if (_now() - began > end_all_late_after) {
            BOOST_LOG(warning) << "Drain handoff: late END_ALL reply";
          }
          for (const auto id : waiters) {
            try {
              const bool sent = answer == end_all_answer_e::ended ? _channel.send(protocol::ended_t {id}) : _channel.send(protocol::end_rejected_t {id});
              if (!sent) {
                BOOST_LOG(warning) << "Drain handoff: cannot send the END_ALL reply";
              }
            } catch (const std::exception &e) {
              BOOST_LOG(error) << "Drain handoff: sending the END_ALL reply failed: " << e.what();
            } catch (...) {
              BOOST_LOG(error) << "Drain handoff: sending the END_ALL reply failed";
            }
          }
        });

        if (!started) {
          // No runner: answer only where the final answer is already known.
          std::vector<std::uint64_t> waiters;
          bool stopping = false;
          {
            std::lock_guard lock {_mutex};
            waiters.swap(_end_all_waiters);
            _end_all_running = false;
            stopping = _stopping;
          }
          if (stopping) {
            return;
          }
          const auto state = _machine.state();
          if (state == end_state_e::ended || state == end_state_e::rolling_back) {
            for (const auto id : waiters) {
              const bool sent = state == end_state_e::ended ? _channel.send(protocol::ended_t {id}) : _channel.send(protocol::end_rejected_t {id});
              if (!sent) {
                BOOST_LOG(warning) << "Drain handoff: cannot send the END_ALL reply";
              }
            }
          } else {
            BOOST_LOG(error) << "Drain handoff: END_ALL not started and not decided, no reply";
          }
        }
      } else {
        BOOST_LOG(debug) << "Drain handoff: responder ignores " << protocol::type_name(message.message);
      }
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Drain handoff: responder failed: " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Drain handoff: responder failed";
    }
  }

  void drain_responder_t::stop() {
    std::vector<handler_t> to_join;
    {
      std::unique_lock lock {_mutex};
      if (_stopping) {
        _stop_cv.wait(lock, [this]() {
          return _stopped;
        });
        return;
      }
      _stopping = true;
      to_join = std::move(_handlers);
      _handlers.clear();
      _end_all_waiters.clear();
    }
    for (auto &h : to_join) {
      if (h.thread.joinable()) {
        h.thread.join();
      }
    }
    {
      std::lock_guard lock {_mutex};
      _stopped = true;
    }
    _stop_cv.notify_all();
  }
}  // namespace handoff
