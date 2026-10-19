/**
 * @file src/handoff/old_side.cpp
 * @brief Definitions for `src/handoff/old_side.h`.
 *
 * Threading: `run()` executes the whole sequence on the calling thread. While it runs, two helper threads exist:
 * a reader thread that does nothing but `channel_t::receive()` and route (so a slow drain request can never delay
 * the routing of a `PONG`), and a worker thread that calls `on_drain_request`. The reader hands `HELLO`, `READY`,
 * `GO_ACK` and `PONG` to `run()` through a mutex-guarded queue and every other known message to the worker's queue.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>

// local includes
#include "old_side.h"
#include "ping_monitor.h"
#include "src/logging.h"

namespace handoff {
  namespace {
    using steady_t = std::chrono::steady_clock;

    /// Longest sleep of a wait loop; bounds the latency of noticing a child exit or a shutdown request.
    constexpr std::chrono::milliseconds wait_slice {20};
    /// Receive timeout of the reader thread; bounds the latency of `stop()`.
    constexpr std::chrono::milliseconds reader_slice {50};
    /// Most events (and, separately, drain requests) queued; later ones are dropped.
    constexpr std::size_t max_queued = 1024;

    /**
     * @brief One item for the `run()` thread: a control message or the final channel status.
     */
    struct event_t {
      bool closed {false};  ///< `true` for the final channel status.
      recv_status_e status {recv_status_e::eof};  ///< Final status when `closed`.
      protocol::message_t message;  ///< Message when not `closed`.
      steady_t::time_point at;  ///< When the reader thread read it.
    };

    /**
     * @brief Result kinds of one wait.
     */
    enum class wait_e {
      event,  ///< An event was taken from the queue.
      timeout,  ///< The deadline passed with nothing queued.
      shutdown,  ///< `request_shutdown()` was called.
      child_exited,  ///< The child has exited and nothing is queued.
    };

    /**
     * @brief Reader thread, drain-request worker and event queue of one `run()`.
     */
    class pump_t {
    public:
      /**
       * @brief Create the pump; nothing runs until `start()`.
       *
       * @param channel Channel.
       * @param on_drain_request Drain-request handler, may be empty.
       */
      pump_t(channel_t &channel, const std::function<void(received_t)> &on_drain_request):
          _channel(channel),
          _on_drain_request(on_drain_request) {
      }

      /**
       * @brief Stops both threads.
       */
      ~pump_t() {
        stop();
      }

      pump_t(const pump_t &) = delete;  ///< Not copyable.
      pump_t &operator=(const pump_t &) = delete;  ///< Not copyable.

      /**
       * @brief Start the reader and the worker.
       */
      void start() {
        _worker = std::thread([this]() {
          worker_loop();
        });
        _reader = std::thread([this]() {
          reader_loop();
        });
      }

      /**
       * @brief Stop the reader, let the worker finish what the reader already took, and join both.
       *
       * Messages still in the socket stay there for whoever reads the channel next.
       */
      void stop() {
        _stop_reader = true;
        if (_reader.joinable()) {
          _reader.join();
        }
        {
          std::lock_guard lock {_work_mutex};
          _stop_worker = true;
        }
        _work_cv.notify_all();
        if (_worker.joinable()) {
          _worker.join();
        }
      }

      /**
       * @brief Take the next queued event, waiting until the deadline.
       *
       * @param deadline Latest time to wait until.
       * @param child Child, polled for an exit while nothing is queued.
       * @param shutdown Shutdown flag.
       * @param out Receives the event when the result is `event`.
       * @return Result kind.
       */
      wait_e wait(steady_t::time_point deadline, child_ops_t &child, const std::atomic<bool> &shutdown, event_t &out) {
        for (;;) {
          {
            std::unique_lock lock {_event_mutex};
            if (!_events.empty()) {
              out = std::move(_events.front());
              _events.pop_front();
              return wait_e::event;
            }
            if (_closed_seen) {
              // Sticky: the channel stays closed.
              out = event_t {};
              out.closed = true;
              out.status = _closed_status;
              out.at = steady_t::now();
              return wait_e::event;
            }
          }
          if (shutdown.load()) {
            return wait_e::shutdown;
          }
          if (child.exited()) {
            // Re-check the queue once: the child may have sent its last message before it exited.
            std::lock_guard lock {_event_mutex};
            if (_events.empty() && !_closed_seen) {
              return wait_e::child_exited;
            }
            continue;
          }
          auto now = steady_t::now();
          if (now >= deadline) {
            return wait_e::timeout;
          }
          std::unique_lock lock {_event_mutex};
          if (_events.empty() && !_closed_seen) {
            _event_cv.wait_for(lock, std::min<steady_t::duration>(wait_slice, deadline - now));
          }
        }
      }

    private:
      /**
       * @brief Read the channel and route each message.
       */
      void reader_loop() {
        try {
          while (!_stop_reader) {
            received_t received;
            auto status = _channel.receive(received, reader_slice);
            if (status == recv_status_e::timeout) {
              continue;
            }
            if (status != recv_status_e::ok) {
              push_closed(status);
              return;
            }
            route(std::move(received));
          }
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Drain handoff: channel reader failed: " << e.what();
          push_closed(recv_status_e::error);
        } catch (...) {
          BOOST_LOG(error) << "Drain handoff: channel reader failed";
          push_closed(recv_status_e::error);
        }
      }

      /**
       * @brief Report the final channel status to `run()`.
       *
       * @param status Final status.
       */
      void push_closed(recv_status_e status) {
        event_t event;
        event.closed = true;
        event.status = status;
        event.at = steady_t::now();
        push_event(std::move(event));
      }

      /**
       * @brief Send a received message to `run()` or to the drain worker.
       *
       * @param received Message and fd.
       */
      void route(received_t received) {
        const auto &message = received.message;
        if (std::holds_alternative<protocol::hello_t>(message) || std::holds_alternative<protocol::ready_t>(message) || std::holds_alternative<protocol::go_ack_t>(message) || std::holds_alternative<protocol::pong_t>(message)) {
          if (received.fd >= 0) {
            ::close(received.fd);  // The child attaches no fd to these messages.
          }
          event_t event;
          event.message = std::move(received.message);
          event.at = steady_t::now();
          push_event(std::move(event));
          return;
        }
        // Only drain requests go to the handler; anything else is a protocol violation of the new process.
        bool drain_request = std::holds_alternative<protocol::end_all_t>(message) || std::holds_alternative<protocol::sessions_request_t>(message) || std::holds_alternative<protocol::close_request_t>(message) || std::holds_alternative<protocol::close_cert_request_t>(message);
        if (!drain_request || !_on_drain_request) {
          if (!drain_request) {
            BOOST_LOG(warning) << "Drain handoff: dropping unexpected " << protocol::type_name(message) << " from the new process";
          } else {
            BOOST_LOG(warning) << "Drain handoff: dropping " << protocol::type_name(message) << ": no drain request handler";
          }
          if (received.fd >= 0) {
            ::close(received.fd);
          }
          return;
        }
        bool queued = false;
        {
          std::lock_guard lock {_work_mutex};
          if (_work.size() < max_queued) {
            _work.push_back(std::move(received));
            queued = true;
          }
        }
        if (queued) {
          _work_cv.notify_one();
        } else {
          BOOST_LOG(warning) << "Drain handoff: drain request queue full; dropping a request";
          if (received.fd >= 0) {
            ::close(received.fd);
          }
        }
      }

      /**
       * @brief Append an event for `run()`.
       *
       * @param event Event.
       */
      void push_event(event_t event) {
        {
          std::lock_guard lock {_event_mutex};
          if (event.closed) {
            // Kept apart so that it is reported after every queued event, and again by later waits.
            _closed_seen = true;
            _closed_status = event.status;
          } else if (_events.size() < max_queued) {
            _events.push_back(std::move(event));
          } else {
            BOOST_LOG(warning) << "Drain handoff: event queue full; dropping a message";
          }
        }
        _event_cv.notify_all();
      }

      /**
       * @brief Call the drain-request handler for each queued message, in order.
       */
      void worker_loop() {
        for (;;) {
          received_t item;
          {
            std::unique_lock lock {_work_mutex};
            _work_cv.wait(lock, [this]() {
              return _stop_worker || !_work.empty();
            });
            if (_stop_worker) {
              // Stopping: finish only the call that is running; discard the rest.
              if (!_work.empty()) {
                BOOST_LOG(warning) << "Drain handoff: discarding " << _work.size() << " queued drain request(s) at the end of the handoff";
                for (auto &queued : _work) {
                  if (queued.fd >= 0) {
                    ::close(queued.fd);
                  }
                }
                _work.clear();
              }
              return;
            }
            item = std::move(_work.front());
            _work.pop_front();
          }
          try {
            _on_drain_request(std::move(item));
          } catch (const std::exception &e) {
            BOOST_LOG(error) << "Drain handoff: drain request handler threw: " << e.what();
          } catch (...) {
            BOOST_LOG(error) << "Drain handoff: drain request handler threw";
          }
        }
      }

      channel_t &_channel;  ///< Channel.
      const std::function<void(received_t)> &_on_drain_request;  ///< Drain-request handler.
      std::atomic<bool> _stop_reader {false};  ///< Asks the reader to end.
      std::mutex _event_mutex;  ///< Guards the event queue and the closed flag.
      std::condition_variable _event_cv;  ///< Signals new events.
      std::deque<event_t> _events;  ///< Events for `run()`.
      bool _closed_seen {false};  ///< The channel reported a final status (after all queued events).
      recv_status_e _closed_status {recv_status_e::eof};  ///< Final status.
      std::mutex _work_mutex;  ///< Guards the drain-request queue.
      std::condition_variable _work_cv;  ///< Signals the worker.
      std::deque<received_t> _work;  ///< Drain requests.
      bool _stop_worker {false};  ///< Asks the worker to end once its queue is empty.
      std::thread _reader;  ///< Reader thread.
      std::thread _worker;  ///< Worker thread.
    };

    /**
     * @brief The sequence of one handoff on the `run()` thread.
     */
    class driver_t {
    public:
      /**
       * @brief Bind the driver to the parts of one `old_side_t`.
       */
      driver_t(old_side_ops_t &ops, child_ops_t &child, channel_t &channel, end_all_machine_t &end_all, const old_side_timeouts_t &timeouts, int new_base, const std::function<void(const control_reply_t &)> &answer_helper, const std::atomic<bool> &shutdown, pump_t &pump):
          _ops(ops),
          _child(child),
          _channel(channel),
          _end_all(end_all),
          _t(timeouts),
          _new_base(new_base),
          _answer_helper(answer_helper),
          _shutdown(shutdown),
          _pump(pump) {
      }

      /**
       * @brief Run the whole sequence.
       *
       * @return Outcome.
       */
      old_outcome_e run() {
        // Step 3 start: HELLO with our pidfd, then HELLO and READY from the child.
        int pidfd = _ops.self_pidfd();
        if (pidfd < 0) {
          return fail("cannot open a pidfd of this process");
        }
        bool sent = _channel.send(_ops.hello(), pidfd);
        ::close(pidfd);
        if (!sent) {
          return fail("cannot send HELLO to the new process");
        }

        auto ready_deadline = steady_t::now() + _t.ready;
        if (auto early = expect_start(ready_deadline)) {
          return *early;
        }

        // Switch (step 5), substeps 1 to 9.
        _paused = true;
        _ops.pause_accept();
        _gate = true;
        _ops.raise_gate();
        if (!_ops.wait_in_flight(_t.in_flight)) {
          return fail("in-flight requests did not finish in time");
        }
        if (auto reason = _ops.recheck(_t.pairing); !reason.empty()) {
          return fail(reason);
        }
        if (!_ops.save_state()) {
          return fail("saving the state files failed");
        }
        _frozen = true;
        _ops.freeze();
        _withdrawn = true;
        _ops.withdraw_mdns(_t.mdns_withdraw);
        _ops.remove_tray();
        auto snapshot = _ops.take_snapshot();
        if (_ops.shutdown_requested() || _shutdown.load()) {
          return rollback("shutdown requested", true);
        }
        _released = true;
        _ops.set_released(true);
        if (_shutdown.load()) {
          // A shutdown in this gap is still before GO: kill-first rollback.
          return rollback("shutdown requested", true);
        }

        auto go_sent_at = steady_t::now();
        auto go_deadline = go_sent_at + _t.go_ack;
        _go_sent = true;
        if (!_channel.send(std::move(snapshot))) {
          return fail("cannot send GO to the new process");
        }

        // Substep 10: GO_ACK.
        for (;;) {
          event_t event;
          auto result = _pump.wait(go_deadline, _child, _shutdown, event);
          if (result == wait_e::shutdown) {
            return fail("shutdown requested");
          }
          if (result == wait_e::timeout) {
            return fail("timeout waiting for GO_ACK");
          }
          if (result == wait_e::child_exited) {
            return fail("the new process exited before GO_ACK");
          }
          if (event.closed) {
            return fail("channel lost before GO_ACK");
          }
          if (auto *ack = std::get_if<protocol::go_ack_t>(&event.message)) {
            if (event.at > go_deadline) {
              return fail("GO_ACK arrived after its deadline");
            }
            if (!ack->ok) {
              return fail("the new process refused GO: " + ack->reason);
            }
            break;
          }
          // HELLO, READY or PONG here carry nothing for us; do not let a stream of them starve the deadline.
          if (steady_t::now() > go_deadline) {
            return fail("timeout waiting for GO_ACK");
          }
        }
        if (steady_t::now() > go_deadline) {
          return fail("GO_ACK deadline passed before COMMIT");
        }
        if (_shutdown.load()) {
          return fail("shutdown requested");
        }
        if (!_channel.send(protocol::commit_t {})) {
          return fail("cannot send COMMIT to the new process");
        }
        _committed = true;
        auto commit_at = steady_t::now();
        _ops.set_role(role_e::draining);
        _ops.set_state(state_e::draining);
        _entered = true;
        _ops.enter_drain_mode();
        answer(control_reply_t {reply_kind_e::committed, {}, _child.pid(), _child.start_time()});
        BOOST_LOG(info) << "Drain handoff: committed, rollback window of " << _t.rollback_window.count() << " ms starts";

        return window(commit_at);
      }

    private:
      /**
       * @brief Wait for HELLO and READY (one deadline). On success returns nothing; otherwise the final outcome.
       *
       * @param deadline Deadline for both.
       * @return Outcome when the start failed.
       */
      std::optional<old_outcome_e> expect_start(steady_t::time_point deadline) {
        bool got_hello = false;
        for (;;) {
          event_t event;
          auto result = _pump.wait(deadline, _child, _shutdown, event);
          if (result == wait_e::shutdown) {
            return fail("shutdown requested");
          }
          if (result == wait_e::timeout) {
            return fail(got_hello ? "timeout waiting for READY" : "timeout waiting for HELLO");
          }
          if (result == wait_e::child_exited) {
            return fail("the new process exited during start");
          }
          if (event.closed) {
            return fail("channel lost during start");
          }
          if (auto *hello = std::get_if<protocol::hello_t>(&event.message)) {
            if (got_hello) {
              continue;
            }
            if (hello->major != protocol::version_major) {
              return fail("protocol major version mismatch");
            }
            got_hello = true;
          } else if (auto *ready = std::get_if<protocol::ready_t>(&event.message)) {
            if (!got_hello) {
              return fail("READY before HELLO");
            }
            if (!ready->ok) {
              return fail("the new process failed to start: " + ready->reason);
            }
            return std::nullopt;
          } else if (std::holds_alternative<protocol::go_ack_t>(event.message)) {
            return fail("GO_ACK before READY");
          }
          // PONG: ignored.
        }
      }

      /**
       * @brief The rollback window and the handover (steps 6 and 7).
       *
       * @param commit_at When COMMIT was sent.
       * @return Outcome.
       */
      old_outcome_e window(steady_t::time_point commit_at) {
        ping_monitor_t monitor {_t.misses_to_hang};
        auto next_ping = steady_t::now();
        auto next_handover = commit_at + _t.rollback_window;
        bool handover_failed = false;

        for (;;) {
          auto deadline = std::min(next_ping, next_handover);
          event_t event;
          auto result = _pump.wait(deadline, _child, _shutdown, event);
          if (result == wait_e::shutdown) {
            return finish_shutdown();
          }
          if (result == wait_e::child_exited) {
            return fail("the new process exited inside the rollback window");
          }
          if (result == wait_e::event) {
            if (event.closed) {
              return fail("channel lost inside the rollback window");
            }
            if (auto *pong = std::get_if<protocol::pong_t>(&event.message)) {
              monitor.on_pong(pong->seq);
            }
          }

          // Run the timed actions that are due, also after an event, so a stream of events cannot starve them.
          auto now = steady_t::now();
          if (now >= next_ping) {
            auto seq = monitor.next_ping();
            if (!_channel.send(protocol::ping_t {seq})) {
              BOOST_LOG(warning) << "Drain handoff: could not send PING " << seq;
            }
            next_ping = now + _t.ping_interval;
            if (monitor.hung()) {
              return fail("the new process missed " + std::to_string(monitor.misses()) + " PINGs in a row");
            }
          }
          if (now >= next_handover) {
            // Step 7.1: the liveness check, then the main-PID handover.
            if (_shutdown.load()) {
              return finish_shutdown();
            }
            if (_child.exited()) {
              return fail("the new process exited before the handover");
            }
            if (_ops.handover(_child.pid())) {
              if (_shutdown.load()) {
                return finish_shutdown();
              }
              if (handover_failed) {
                _ops.set_state(state_e::draining);
              }
              _ops.close_socket_copies();
              if (!_channel.send(protocol::main_t {})) {
                BOOST_LOG(warning) << "Drain handoff: could not send MAIN";
              }
              BOOST_LOG(info) << "Drain handoff: main PID handed over";
              return old_outcome_e::handed_over;
            }
            BOOST_LOG(error) << "Drain handoff: the main-PID handover failed; retrying in " << _t.handover_retry.count() << " ms";
            if (_shutdown.load()) {
              return finish_shutdown();
            }
            if (!handover_failed) {
              handover_failed = true;
              _ops.set_state(state_e::handover_failed);
            }
            next_handover = steady_t::now() + _t.handover_retry;
          }
        }
      }

      /**
       * @brief Answer the helper once.
       *
       * @param reply Answer.
       */
      void answer(const control_reply_t &reply) {
        if (_answered) {
          return;
        }
        _answered = true;
        if (_answer_helper) {
          _answer_helper(reply);
        }
      }

      /**
       * @brief End without a rollback because a shutdown arrived after `GO`.
       *
       * @return `shutdown`.
       */
      old_outcome_e finish_shutdown() {
        BOOST_LOG(warning) << "Drain handoff: shutting down during the handoff; no rollback";
        answer(control_reply_t {reply_kind_e::failed, "shutting down", 0, 0});
        return old_outcome_e::shutdown;
      }

      /**
       * @brief A failure: roll back, except after `GO` during a shutdown.
       *
       * @param reason One-line reason.
       * @return Outcome.
       */
      old_outcome_e fail(const std::string &reason) {
        if (_go_sent && _shutdown.load()) {
          return finish_shutdown();
        }
        return rollback(reason, false);
      }

      /**
       * @brief The rollback of the steps that happened (step 6), kill-first.
       *
       * @param reason One-line reason.
       * @param shutdown_cause Whether the rollback is the abort for a shutdown.
       * @return `shutdown` for a shutdown abort, else `rolled_back`.
       */
      old_outcome_e rollback(const std::string &reason, bool shutdown_cause) {
        BOOST_LOG(error) << "Drain handoff: rolling back: " << reason;
        auto app = _end_all.decide_rollback();

        // Kill-first: nothing below runs before the child is gone. This loop deliberately ignores a shutdown request:
        // the forced-shutdown limit of 10 s covers a stuck kill.
        bool kill_pending = false;
        for (;;) {
          auto reaped = _child.kill_and_reap(_t.reap);
          bool gone = reaped == reap_result_e::reaped || reaped == reap_result_e::reaped_elsewhere;
          if (reaped == reap_result_e::error && _child.exited()) {
            BOOST_LOG(error) << "Drain handoff: the new process's pidfd is invalid but the child has exited; treating it as gone";
            gone = true;
          }
          if (!gone) {
            BOOST_LOG(error) << "Drain handoff: the new process is not gone yet (kill pending)" << (kill_pending ? "; sending SIGKILL again" : "");
            if (!kill_pending) {
              kill_pending = true;
              _ops.set_state(state_e::kill_pending);
              answer(control_reply_t {reply_kind_e::kill_pending, "kill_pending", 0, 0});
            }
            if (reaped == reap_result_e::error) {
              // An error can occur while the child is alive: handle it like a timeout, at the reap interval.
              std::this_thread::sleep_for(_t.reap);
            }
            continue;
          }
          break;
        }
        answer(control_reply_t {reply_kind_e::failed, reason, 0, 0});

        if (_frozen) {
          // The unbounded wait only protects the reload and the app-list swap.
          _ops.wait_in_flight(std::chrono::milliseconds {-1});
          if (!_ops.reload_after_rollback()) {
            BOOST_LOG(error) << "Drain handoff: reload after the rollback failed; keeping the frozen state";
          }
          _ops.unfreeze();
        }
        if (_released) {
          if (app == rollback_app_e::take_back) {
            _ops.set_released(false);
          } else {
            BOOST_LOG(warning) << "Drain handoff: the app claim was given up (END_ALL); dropping the app state without undo";
            _ops.drop_app();
          }
        }
        _ops.remove_new_instance_file();
        if (_committed) {
          BOOST_LOG(warning) << "Drain handoff: apps the new process launched and commands it ran are not undone";
          _ops.start_upnp_cleanup(_new_base);
        }
        if (_gate) {
          _ops.lower_gate();
        }
        if (_paused) {
          _ops.resume_accept();
        }
        if (_withdrawn) {
          _ops.publish_mdns();
          _ops.restore_tray();
        }
        if (_entered) {
          _ops.leave_drain_mode();
          _ops.set_role(role_e::active);
        }
        _ops.set_state(state_e::none);
        BOOST_LOG(info) << "Drain handoff: rollback complete";
        return (shutdown_cause || _shutdown.load()) ? old_outcome_e::shutdown : old_outcome_e::rolled_back;
      }

      old_side_ops_t &_ops;  ///< Effects.
      child_ops_t &_child;  ///< Child.
      channel_t &_channel;  ///< Channel.
      end_all_machine_t &_end_all;  ///< `END_ALL` machine.
      const old_side_timeouts_t &_t;  ///< Timeouts.
      int _new_base;  ///< Child's UDP base.
      const std::function<void(const control_reply_t &)> &_answer_helper;  ///< Helper answer.
      const std::atomic<bool> &_shutdown;  ///< Shutdown requested.
      pump_t &_pump;  ///< Channel pump.
      bool _answered {false};  ///< The helper has been answered.
      bool _paused {false};  ///< Accept paused (or being paused).
      bool _gate {false};  ///< Gate raised.
      bool _frozen {false};  ///< State frozen.
      bool _withdrawn {false};  ///< mDNS withdrawn and tray removed.
      bool _released {false};  ///< App released.
      bool _go_sent {false};  ///< `GO` send attempted.
      bool _committed {false};  ///< `COMMIT` sent.
      bool _entered {false};  ///< Drain mode entered.
    };
  }  // namespace

  old_side_t::old_side_t(old_side_ops_t &ops, child_ops_t &child, channel_t &channel, end_all_machine_t &end_all, old_side_timeouts_t timeouts, int new_base, std::function<void(const control_reply_t &)> answer_helper):
      _ops(ops),
      _child(child),
      _channel(channel),
      _end_all(end_all),
      _timeouts(timeouts),
      _new_base(new_base),
      _answer_helper(std::move(answer_helper)) {
  }

  old_outcome_e old_side_t::run(const std::function<void(received_t)> &on_drain_request) {
    pump_t pump {_channel, on_drain_request};
    pump.start();
    driver_t driver {_ops, _child, _channel, _end_all, _timeouts, _new_base, _answer_helper, _shutdown, pump};
    auto outcome = driver.run();
    pump.stop();
    return outcome;
  }

  void old_side_t::request_shutdown() {
    _shutdown = true;
  }
}  // namespace handoff
