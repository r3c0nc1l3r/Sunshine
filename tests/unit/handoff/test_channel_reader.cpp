/**
 * @file tests/unit/handoff/test_channel_reader.cpp
 * @brief Tests for the drain-handoff channel reader (src/handoff/channel_reader.h).
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <condition_variable>
  #include <cstdint>
  #include <functional>
  #include <memory>
  #include <mutex>
  #include <string>
  #include <thread>
  #include <variant>
  #include <vector>

  // platform includes
  #include <sys/socket.h>
  #include <unistd.h>

  // local includes
  #include "src/handoff/channel_reader.h"

using namespace std::chrono_literals;

namespace {
  using clock_type = std::chrono::steady_clock;
  namespace proto = handoff::protocol;

  /**
   * @brief Thread-safe record of what the reader handed out. Shared so a hung reader cannot leave dangling captures.
   */
  struct recorder_t {
    std::mutex mutex;  ///< Guards the members.
    std::condition_variable cv;  ///< Signals changes.
    std::vector<proto::message_t> messages;  ///< Messages seen by the handler.
    std::vector<handoff::recv_status_e> closes;  ///< Close statuses seen.
    std::vector<std::string> events;  ///< "msg" / "close" in order.

    void on_message(handoff::received_t r) {
      if (r.fd >= 0) {
        ::close(r.fd);
      }
      {
        std::lock_guard lock(mutex);
        messages.push_back(std::move(r.message));
        events.emplace_back("msg");
      }
      cv.notify_all();
    }

    void on_close(handoff::recv_status_e s) {
      {
        std::lock_guard lock(mutex);
        closes.push_back(s);
        events.emplace_back("close");
      }
      cv.notify_all();
    }

    template<class Pred>
    bool wait_for(Pred pred, std::chrono::milliseconds timeout) {
      std::unique_lock lock(mutex);
      return cv.wait_for(lock, timeout, [&] {
        return pred(*this);
      });
    }

    std::size_t message_count() {
      std::lock_guard lock(mutex);
      return messages.size();
    }

    std::size_t close_count() {
      std::lock_guard lock(mutex);
      return closes.size();
    }
  };

  /**
   * @brief Channel pair plus a recorder; the reader runs on `a`, the test drives `b`.
   */
  struct fixture_t {
    std::unique_ptr<handoff::channel_t> a;  ///< Reader side.
    std::unique_ptr<handoff::channel_t> b;  ///< Test side.
    int raw_b {-1};  ///< Duplicate of b's fd for raw writes.
    std::shared_ptr<recorder_t> rec {std::make_shared<recorder_t>()};  ///< Recorder.
    std::unique_ptr<handoff::channel_reader_t> reader;  ///< Reader.

    bool init() {
      auto fds = handoff::make_channel_pair();
      if (!fds) {
        return false;
      }
      a = std::make_unique<handoff::channel_t>(fds->first);
      b = std::make_unique<handoff::channel_t>(fds->second);
      return true;
    }

    void make_reader(bool answer_pings) {
      auto r = rec;
      reader = std::make_unique<handoff::channel_reader_t>(
        *a,
        answer_pings,
        [r](handoff::received_t m) {
          r->on_message(std::move(m));
        },
        [r](handoff::recv_status_e s) {
          r->on_close(s);
        }
      );
    }

    ~fixture_t() {
      if (reader) {
        reader->stop();
      }
      if (raw_b >= 0) {
        ::close(raw_b);
      }
    }
  };

  proto::message_t ping(std::uint64_t seq) {
    proto::ping_t p;
    p.seq = seq;
    return p;
  }

  proto::message_t request(std::uint64_t id) {
    proto::sessions_request_t r;
    r.request_id = id;
    return r;
  }

  /**
   * @brief Send with retries (the socket buffer is small), bounded.
   */
  bool send_retry(handoff::channel_t &c, const proto::message_t &m) {
    auto deadline = clock_type::now() + 2s;
    while (!c.send(m)) {
      if (clock_type::now() > deadline) {
        return false;
      }
      std::this_thread::sleep_for(1ms);
    }
    return true;
  }

  /**
   * @brief Run a function on a detached thread and wait for it; false when it did not finish in time.
   */
  bool finishes_within(std::function<void()> fn, std::chrono::milliseconds limit) {
    struct state_t {
      std::mutex m;
      std::condition_variable cv;
      bool done {false};
    };

    auto st = std::make_shared<state_t>();
    std::thread([st, fn = std::move(fn)] {
      fn();
      {
        std::lock_guard lock(st->m);
        st->done = true;
      }
      st->cv.notify_all();
    }).detach();
    std::unique_lock lock(st->m);
    return st->cv.wait_for(lock, limit, [&] {
      return st->done;
    });
  }
}  // namespace

TEST(HandoffChannelReaderTest, AnswersPingWithPongOfSameSeq) {
  fixture_t f;
  ASSERT_TRUE(f.init());
  f.make_reader(true);
  f.reader->start();

  ASSERT_TRUE(f.b->send(ping(77)));
  handoff::received_t r;
  ASSERT_EQ(handoff::recv_status_e::ok, f.b->receive(r, 500ms)) << "no PONG came back for a PING";
  auto *pong = std::get_if<proto::pong_t>(&r.message);
  ASSERT_NE(nullptr, pong) << "the answer to PING must be PONG";
  EXPECT_EQ(77u, pong->seq) << "PONG must carry the PING seq";
}

TEST(HandoffChannelReaderTest, AnsweredPingNeverReachesTheHandler) {
  fixture_t f;
  ASSERT_TRUE(f.init());
  f.make_reader(true);
  f.reader->start();

  ASSERT_TRUE(f.b->send(ping(1)));
  ASSERT_TRUE(f.b->send(request(5)));
  ASSERT_TRUE(f.rec->wait_for(
    [](recorder_t &r) {
      return !r.messages.empty();
    },
    1000ms
  ));
  std::this_thread::sleep_for(50ms);
  std::lock_guard lock(f.rec->mutex);
  ASSERT_EQ(1u, f.rec->messages.size()) << "only the non-PING message may reach the handler";
  EXPECT_TRUE(std::holds_alternative<proto::sessions_request_t>(f.rec->messages[0]))
    << "the handler must not see PING when answer_pings is true";
}

TEST(HandoffChannelReaderTest, ManyPingsGetManyPongsInOrder) {
  fixture_t f;
  ASSERT_TRUE(f.init());
  f.make_reader(true);
  f.reader->start();
  for (std::uint64_t i = 1; i <= 6; ++i) {
    ASSERT_TRUE(send_retry(*f.b, ping(i)));
  }
  for (std::uint64_t i = 1; i <= 6; ++i) {
    handoff::received_t r;
    ASSERT_EQ(handoff::recv_status_e::ok, f.b->receive(r, 500ms));
    auto *pong = std::get_if<proto::pong_t>(&r.message);
    ASSERT_NE(nullptr, pong);
    EXPECT_EQ(i, pong->seq);
  }
}

TEST(HandoffChannelReaderTest, PingReachesHandlerWhenNotAnswering) {
  fixture_t f;
  ASSERT_TRUE(f.init());
  f.make_reader(false);
  f.reader->start();

  ASSERT_TRUE(f.b->send(ping(9)));
  ASSERT_TRUE(f.rec->wait_for(
    [](recorder_t &r) {
      return !r.messages.empty();
    },
    1000ms
  )) << "PING must be passed to the handler when answer_pings is false";
  {
    std::lock_guard lock(f.rec->mutex);
    EXPECT_TRUE(f.rec->messages[0] == ping(9));
  }
  handoff::received_t r;
  EXPECT_EQ(handoff::recv_status_e::timeout, f.b->receive(r, 50ms)) << "no PONG may be sent when answer_pings is false";
}

TEST(HandoffChannelReaderTest, HandlerOrderIsPreserved) {
  fixture_t f;
  ASSERT_TRUE(f.init());
  f.make_reader(false);
  f.reader->start();
  constexpr std::uint64_t n = 25;
  for (std::uint64_t i = 1; i <= n; ++i) {
    ASSERT_TRUE(send_retry(*f.b, request(i))) << "could not send message " << i;
  }
  ASSERT_TRUE(f.rec->wait_for(
    [](recorder_t &r) {
      return r.messages.size() >= n;
    },
    2000ms
  ));
  std::lock_guard lock(f.rec->mutex);
  for (std::uint64_t i = 1; i <= n; ++i) {
    EXPECT_TRUE(f.rec->messages[i - 1] == request(i)) << "message " << i << " was delivered out of order";
  }
}

TEST(HandoffChannelReaderTest, AttachedFdIsPassedToTheHandler) {
  fixture_t f;
  ASSERT_TRUE(f.init());
  std::mutex m;
  std::condition_variable cv;
  int got_fd = -2;
  auto reader = std::make_unique<handoff::channel_reader_t>(
    *f.a,
    false,
    [&](handoff::received_t r) {
      std::lock_guard lock(m);
      got_fd = r.fd;
      cv.notify_all();
    },
    [](handoff::recv_status_e) {
    }
  );
  reader->start();
  int p[2];
  ASSERT_EQ(0, ::pipe(p));
  ASSERT_TRUE(f.b->send(proto::hello_t {}, p[0]));
  {
    std::unique_lock lock(m);
    ASSERT_TRUE(cv.wait_for(lock, 1000ms, [&] {
      return got_fd != -2;
    }));
  }
  reader->stop();
  EXPECT_GE(got_fd, 0) << "the fd attached to a message must reach the handler";
  if (got_fd >= 0) {
    ::close(got_fd);
  }
  ::close(p[0]);
  ::close(p[1]);
}

TEST(HandoffChannelReaderTest, SlowHandlerDoesNotDelayPong) {
  fixture_t f;
  ASSERT_TRUE(f.init());
  std::atomic<bool> in_handler {false};
  auto rec = f.rec;
  f.reader = std::make_unique<handoff::channel_reader_t>(
    *f.a,
    true,
    [&in_handler](handoff::received_t) {
      in_handler = true;
      std::this_thread::sleep_for(300ms);
    },
    [rec](handoff::recv_status_e s) {
      rec->on_close(s);
    }
  );
  f.reader->start();

  ASSERT_TRUE(f.b->send(request(1)));
  auto deadline = clock_type::now() + 1s;
  while (!in_handler && clock_type::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_TRUE(in_handler) << "the handler never started";

  for (std::uint64_t seq = 1; seq <= 3; ++seq) {
    auto start = clock_type::now();
    ASSERT_TRUE(f.b->send(ping(seq)));
    handoff::received_t r;
    ASSERT_EQ(handoff::recv_status_e::ok, f.b->receive(r, 250ms)) << "PONG delayed by a slow handler";
    auto latency = clock_type::now() - start;
    EXPECT_LT(latency, 100ms) << "PONG latency must not depend on the handler (PING " << seq << ")";
    auto *pong = std::get_if<proto::pong_t>(&r.message);
    ASSERT_NE(nullptr, pong);
    EXPECT_EQ(seq, pong->seq);
  }
}

TEST(HandoffChannelReaderTest, OnCloseCalledOnceWithEofWhenPeerCloses) {
  fixture_t f;
  ASSERT_TRUE(f.init());
  f.make_reader(true);
  f.reader->start();
  f.b->close();
  ASSERT_TRUE(f.rec->wait_for(
    [](recorder_t &r) {
      return !r.closes.empty();
    },
    1000ms
  )) << "on_close was not called when the peer closed";
  std::this_thread::sleep_for(100ms);
  std::lock_guard lock(f.rec->mutex);
  ASSERT_EQ(1u, f.rec->closes.size()) << "on_close must be called exactly once";
  EXPECT_EQ(handoff::recv_status_e::eof, f.rec->closes[0]);
}

TEST(HandoffChannelReaderTest, OnCloseComesAfterTheLastMessage) {
  fixture_t f;
  ASSERT_TRUE(f.init());
  f.make_reader(false);
  f.reader->start();
  ASSERT_TRUE(f.b->send(request(1)));
  ASSERT_TRUE(f.b->send(request(2)));
  f.b->close();
  ASSERT_TRUE(f.rec->wait_for(
    [](recorder_t &r) {
      return !r.closes.empty();
    },
    1000ms
  ));
  std::lock_guard lock(f.rec->mutex);
  ASSERT_EQ(3u, f.rec->events.size());
  EXPECT_EQ("msg", f.rec->events[0]);
  EXPECT_EQ("msg", f.rec->events[1]);
  EXPECT_EQ("close", f.rec->events[2]) << "on_close must come after the last message";
}

TEST(HandoffChannelReaderTest, OnCloseWithMalformedForGarbageDatagram) {
  fixture_t f;
  ASSERT_TRUE(f.init());
  f.make_reader(true);
  f.reader->start();
  ASSERT_EQ(5, ::send(f.b->fd(), "oops!", 5, MSG_NOSIGNAL));
  ASSERT_TRUE(f.rec->wait_for(
    [](recorder_t &r) {
      return !r.closes.empty();
    },
    1000ms
  )) << "on_close was not called for a malformed datagram";
  std::lock_guard lock(f.rec->mutex);
  EXPECT_EQ(handoff::recv_status_e::malformed, f.rec->closes[0]);
  EXPECT_EQ(1u, f.rec->closes.size());
}

TEST(HandoffChannelReaderTest, OnCloseWithOversizedForHugeDatagram) {
  fixture_t f;
  ASSERT_TRUE(f.init());
  f.make_reader(true);
  f.reader->start();
  std::string big(proto::max_message_size + 500, 'x');
  ASSERT_EQ(static_cast<ssize_t>(big.size()), ::send(f.b->fd(), big.data(), big.size(), MSG_NOSIGNAL));
  ASSERT_TRUE(f.rec->wait_for(
    [](recorder_t &r) {
      return !r.closes.empty();
    },
    1000ms
  ));
  std::lock_guard lock(f.rec->mutex);
  EXPECT_EQ(handoff::recv_status_e::oversized, f.rec->closes[0]);
}

TEST(HandoffChannelReaderTest, UnknownMessageTypeIsPassedOn) {
  fixture_t f;
  ASSERT_TRUE(f.init());
  f.make_reader(true);
  f.reader->start();
  const std::string payload = R"({"type":"FUTURE_THING"})";
  ASSERT_EQ(static_cast<ssize_t>(payload.size()), ::send(f.b->fd(), payload.data(), payload.size(), MSG_NOSIGNAL));
  ASSERT_TRUE(f.rec->wait_for(
    [](recorder_t &r) {
      return !r.messages.empty() || !r.closes.empty();
    },
    1000ms
  ));
  std::lock_guard lock(f.rec->mutex);
  EXPECT_EQ(0u, f.rec->closes.size()) << "an unknown message type is not a close";
  ASSERT_EQ(1u, f.rec->messages.size());
  EXPECT_TRUE(std::holds_alternative<proto::unknown_t>(f.rec->messages[0]));
}

TEST(HandoffChannelReaderTest, StopTwiceIsSafe) {
  fixture_t f;
  ASSERT_TRUE(f.init());
  f.make_reader(true);
  f.reader->start();
  auto reader = std::move(f.reader);
  EXPECT_TRUE(finishes_within(
    [r = std::shared_ptr<handoff::channel_reader_t>(std::move(reader))] {
      r->stop();
      r->stop();
    },
    2000ms
  )) << "stop() twice hangs";
}

TEST(HandoffChannelReaderTest, StopDoesNotCallOnClose) {
  fixture_t f;
  ASSERT_TRUE(f.init());
  f.make_reader(true);
  f.reader->start();
  f.reader->stop();
  f.b->close();
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(0u, f.rec->close_count()) << "on_close must not be called after stop()";
}

TEST(HandoffChannelReaderTest, StopStopsAnsweringPings) {
  fixture_t f;
  ASSERT_TRUE(f.init());
  f.make_reader(true);
  f.reader->start();
  f.reader->stop();
  f.b->send(ping(1));
  handoff::received_t r;
  EXPECT_EQ(handoff::recv_status_e::timeout, f.b->receive(r, 80ms)) << "no PONG after stop()";
}

TEST(HandoffChannelReaderTest, DestructorStopsTheReader) {
  fixture_t f;
  ASSERT_TRUE(f.init());
  f.make_reader(true);
  f.reader->start();
  EXPECT_TRUE(finishes_within(
    [r = std::shared_ptr<handoff::channel_reader_t>(std::move(f.reader))]() mutable {
      r.reset();
    },
    2000ms
  )) << "destroying a started reader hangs";
}

TEST(HandoffChannelReaderTest, StopFromInsideHandlerDoesNotDeadlock) {
  auto fds = handoff::make_channel_pair();
  ASSERT_TRUE(fds.has_value());
  // Heap objects, leaked on failure so that a hung thread never touches freed memory.
  auto *a = new handoff::channel_t(fds->first);
  auto *b = new handoff::channel_t(fds->second);
  auto stopped = std::make_shared<std::atomic<bool>>(false);
  auto reader_slot = std::make_shared<std::atomic<handoff::channel_reader_t *>>(nullptr);
  auto *reader = new handoff::channel_reader_t(
    *a,
    false,
    [stopped, reader_slot](handoff::received_t) {
      reader_slot->load()->stop();
      *stopped = true;
    },
    [](handoff::recv_status_e) {
    }
  );
  reader_slot->store(reader);
  reader->start();
  ASSERT_TRUE(b->send(request(1)));

  auto deadline = clock_type::now() + 2s;
  while (!*stopped && clock_type::now() < deadline) {
    std::this_thread::sleep_for(5ms);
  }
  ASSERT_TRUE(*stopped) << "stop() called from inside a message handler deadlocked";

  bool destroyed = finishes_within(
    [reader] {
      reader->stop();
      delete reader;
    },
    2000ms
  );
  EXPECT_TRUE(destroyed) << "stop() or destroying the reader after a stop from a handler hangs";
  if (destroyed) {
    delete a;
    delete b;
  }
}

TEST(HandoffChannelReaderTest, StopFromInsideCloseHandlerDoesNotDeadlock) {
  auto fds = handoff::make_channel_pair();
  ASSERT_TRUE(fds.has_value());
  auto *a = new handoff::channel_t(fds->first);
  auto *b = new handoff::channel_t(fds->second);
  auto stopped = std::make_shared<std::atomic<bool>>(false);
  auto reader_slot = std::make_shared<std::atomic<handoff::channel_reader_t *>>(nullptr);
  auto *reader = new handoff::channel_reader_t(
    *a,
    false,
    [](handoff::received_t) {
    },
    [stopped, reader_slot](handoff::recv_status_e) {
      reader_slot->load()->stop();
      *stopped = true;
    }
  );
  reader_slot->store(reader);
  reader->start();
  b->close();

  auto deadline = clock_type::now() + 2s;
  while (!*stopped && clock_type::now() < deadline) {
    std::this_thread::sleep_for(5ms);
  }
  ASSERT_TRUE(*stopped) << "stop() called from inside the close handler deadlocked";
  bool destroyed = finishes_within(
    [reader] {
      delete reader;
    },
    2000ms
  );
  EXPECT_TRUE(destroyed);
  if (destroyed) {
    delete a;
    delete b;
  }
}

#endif  // __linux__
