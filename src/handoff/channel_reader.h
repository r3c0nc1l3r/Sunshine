/**
 * @file src/handoff/channel_reader.h
 * @brief Dedicated channel reader: answers `PING` inline and hands every other message to a worker thread.
 *
 * See `docs/drain-handoff.md`, step 6: the `PONG` path takes no lock and does no other work, so slow handlers never
 * delay a `PONG`.
 */
#pragma once

// standard includes
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

// local includes
#include "channel.h"

namespace handoff {
  /**
   * @brief Reader thread plus worker thread over one channel.
   */
  class channel_reader_t {
  public:
    /**
     * @brief Called on the worker thread for each message that is not an answered `PING`, in arrival order.
     *
     * The handler owns an attached fd. If it throws, the worker logs the exception and closes the attached fd, so a
     * handler must not throw after it has closed or adopted that fd.
     */
    using message_handler_t = std::function<void(received_t)>;

    /**
     * @brief Called once, on the worker thread, after the last message, when the channel reports `eof`, `error`,
     * `oversized` or `malformed`. Not called after `stop()`.
     */
    using close_handler_t = std::function<void(recv_status_e)>;

    /**
     * @brief Create a reader. Nothing runs until `start()`.
     *
     * @param channel Channel; must outlive the reader.
     * @param answer_pings When `true`, each `PING <seq>` is answered with `PONG <seq>` on the reader thread and not
     *        passed to the handler (the new process); when `false`, `PING` is passed on like any message.
     * @param on_message Message handler.
     * @param on_close Close handler.
     */
    channel_reader_t(channel_t &channel, bool answer_pings, message_handler_t on_message, close_handler_t on_close);

    /**
     * @brief Calls `stop()`.
     */
    ~channel_reader_t();

    /**
     * @brief Start both threads.
     */
    void start();

    /**
     * @brief Stop and join both threads; queued messages not yet handled are dropped (fds closed). Safe to call twice
     * and from a handler (then the join of the calling thread is skipped).
     */
    void stop();

  private:
    channel_t &_channel;  ///< Channel.
    bool _answer_pings;  ///< Answer `PING` inline.
    message_handler_t _on_message;  ///< Message handler.
    close_handler_t _on_close;  ///< Close handler.
    std::mutex _mutex;  ///< Guards the queue and flags.
    std::condition_variable _cv;  ///< Signals the worker.
    std::deque<received_t> _queue;  ///< Messages for the worker.
    bool _stopping {false};  ///< Set by `stop()`.
    bool _closed {false};  ///< The channel reported a final status.
    recv_status_e _close_status {recv_status_e::eof};  ///< Final status.
    std::thread _reader;  ///< Reader thread.
    std::thread _worker;  ///< Worker thread.
  };
}  // namespace handoff
