/**
 * @file tests/unit/handoff/test_channel.cpp
 * @brief Tests for the drain-handoff channel transport and request tracker (src/handoff/channel.h).
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <atomic>
  #include <chrono>
  #include <cstdint>
  #include <cstring>
  #include <filesystem>
  #include <functional>
  #include <future>
  #include <memory>
  #include <string>
  #include <thread>
  #include <variant>
  #include <vector>

  // platform includes
  #include <fcntl.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <unistd.h>

  // local includes
  #include "src/handoff/channel.h"

using namespace std::chrono_literals;

namespace {
  using clock_type = std::chrono::steady_clock;

  /**
   * @brief Closes an fd on scope exit.
   */
  struct fd_closer_t {
    int fd;  ///< Owned fd.

    explicit fd_closer_t(int f):
        fd(f) {}

    ~fd_closer_t() {
      if (fd >= 0) {
        ::close(fd);
      }
    }

    fd_closer_t(const fd_closer_t &) = delete;
    fd_closer_t &operator=(const fd_closer_t &) = delete;
  };

  /**
   * @brief A channel on one end and the raw fd of the other end.
   */
  struct raw_pair_t {
    handoff::channel_t channel;  ///< The channel end.
    fd_closer_t raw;  ///< The raw peer fd.

    raw_pair_t(int a, int b):
        channel(a),
        raw(b) {}
  };

  /**
   * @brief Make a raw pair; fails the test when the socketpair cannot be created.
   */
  std::unique_ptr<raw_pair_t> make_raw_pair() {
    auto fds = handoff::make_channel_pair();
    if (!fds) {
      return nullptr;
    }
    return std::make_unique<raw_pair_t>(fds->first, fds->second);
  }

  /**
   * @brief Two channels over one socketpair.
   */
  struct chan_pair_t {
    handoff::channel_t a;  ///< First end.
    handoff::channel_t b;  ///< Second end.

    chan_pair_t(int x, int y):
        a(x),
        b(y) {}
  };

  std::unique_ptr<chan_pair_t> make_chan_pair() {
    auto fds = handoff::make_channel_pair();
    if (!fds) {
      return nullptr;
    }
    return std::make_unique<chan_pair_t>(fds->first, fds->second);
  }

  handoff::protocol::message_t ping(std::uint64_t seq) {
    handoff::protocol::ping_t p;
    p.seq = seq;
    return p;
  }

  /**
   * @brief Send a raw datagram with an optional fd attached.
   */
  ssize_t raw_send(int sock, const std::string &payload, int attach_fd = -1) {
    iovec iov {};
    iov.iov_base = const_cast<char *>(payload.data());
    iov.iov_len = payload.size();
    msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] {};
    if (attach_fd >= 0) {
      msg.msg_control = control;
      msg.msg_controllen = sizeof(control);
      cmsghdr *c = CMSG_FIRSTHDR(&msg);
      c->cmsg_level = SOL_SOCKET;
      c->cmsg_type = SCM_RIGHTS;
      c->cmsg_len = CMSG_LEN(sizeof(int));
      std::memcpy(CMSG_DATA(c), &attach_fd, sizeof(int));
    }
    return ::sendmsg(sock, &msg, MSG_NOSIGNAL);
  }
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// make_channel_pair, send, receive
// ---------------------------------------------------------------------------------------------------------------------

TEST(HandoffChannelTest, PairEndsAreSeqpacketAndCloseOnExec) {
  auto fds = handoff::make_channel_pair();
  ASSERT_TRUE(fds.has_value()) << "make_channel_pair() failed";
  fd_closer_t c1(fds->first);
  fd_closer_t c2(fds->second);
  EXPECT_NE(fds->first, fds->second);
  for (int fd : {fds->first, fds->second}) {
    int type = 0;
    socklen_t len = sizeof(type);
    ASSERT_EQ(0, ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len));
    EXPECT_EQ(SOCK_SEQPACKET, type) << "channel ends must be SOCK_SEQPACKET";
    int flags = ::fcntl(fd, F_GETFD);
    ASSERT_GE(flags, 0);
    EXPECT_TRUE(flags & FD_CLOEXEC) << "channel ends must be close-on-exec";
  }
}

TEST(HandoffChannelTest, RoundTripOfSeveralMessageTypes) {
  auto p = make_chan_pair();
  ASSERT_TRUE(p);

  std::vector<handoff::protocol::message_t> messages;
  handoff::protocol::hello_t hello;
  hello.capabilities = {"a", "b"};
  hello.probe = handoff::protocol::probe_results_t {2, 1, true};
  handoff::protocol::probe_identity_t identity;
  identity.build_id = "build";
  identity.render_node = "/dev/dri/renderD128";
  identity.vendor_id = 4098;
  identity.device_id = 7;
  identity.driver_version = "1.2.3";
  hello.identity = identity;
  hello.cooldown_deadline_ns = 123456789;
  messages.emplace_back(hello);
  messages.emplace_back(handoff::protocol::ready_t {true, ""});
  handoff::protocol::go_t go;
  go.host_audio = true;
  go.next_launch_session_id = 9;
  handoff::protocol::app_snapshot_t app;
  app.app_id = 5;
  app.name = "placebo";
  app.applied_prep = 2;
  app.digest = std::string(64, 'a');
  app.launch_env = {{"SUNSHINE_X", "1"}};
  go.app = app;
  messages.emplace_back(go);
  messages.emplace_back(handoff::protocol::commit_t {});
  messages.emplace_back(ping(42));
  handoff::protocol::sessions_reply_t sr;
  sr.request_id = 3;
  sr.sessions.push_back({1, 1000, "udp/ipv4", "lan"});
  messages.emplace_back(sr);
  handoff::protocol::close_cert_request_t cc;
  cc.request_id = 4;
  cc.cert_sha256 = std::string(64, 'b');
  messages.emplace_back(cc);
  messages.emplace_back(handoff::protocol::handoff_state_t {"draining"});
  messages.emplace_back(handoff::protocol::end_all_t {11});

  for (const auto &m : messages) {
    ASSERT_TRUE(p->a.send(m)) << "send of " << handoff::protocol::type_name(m) << " failed";
    handoff::received_t r;
    ASSERT_EQ(handoff::recv_status_e::ok, p->b.receive(r, 500ms)) << "receive of " << handoff::protocol::type_name(m);
    EXPECT_TRUE(r.message == m) << "message " << handoff::protocol::type_name(m) << " changed on the way";
    EXPECT_EQ(-1, r.fd) << "no fd was attached";
  }
}

TEST(HandoffChannelTest, SendsInBothDirections) {
  auto p = make_chan_pair();
  ASSERT_TRUE(p);
  ASSERT_TRUE(p->a.send(ping(1)));
  ASSERT_TRUE(p->b.send(ping(2)));
  handoff::received_t r;
  ASSERT_EQ(handoff::recv_status_e::ok, p->b.receive(r, 500ms));
  EXPECT_TRUE(r.message == ping(1));
  ASSERT_EQ(handoff::recv_status_e::ok, p->a.receive(r, 500ms));
  EXPECT_TRUE(r.message == ping(2));
}

TEST(HandoffChannelTest, MessagesKeepTheirBoundariesAndOrder) {
  auto p = make_chan_pair();
  ASSERT_TRUE(p);
  for (std::uint64_t i = 1; i <= 5; ++i) {
    ASSERT_TRUE(p->a.send(ping(i)));
  }
  for (std::uint64_t i = 1; i <= 5; ++i) {
    handoff::received_t r;
    ASSERT_EQ(handoff::recv_status_e::ok, p->b.receive(r, 500ms));
    EXPECT_TRUE(r.message == ping(i)) << "message " << i << " out of order or merged";
  }
}

TEST(HandoffChannelTest, FdPassingKeepsTheFileAndSetsCloseOnExec) {
  auto p = make_chan_pair();
  ASSERT_TRUE(p);
  int pipefd[2];
  ASSERT_EQ(0, ::pipe(pipefd));
  fd_closer_t rd(pipefd[0]);
  fd_closer_t wr(pipefd[1]);

  ASSERT_TRUE(p->a.send(ping(1), pipefd[0])) << "send with an attached fd failed";
  handoff::received_t r;
  ASSERT_EQ(handoff::recv_status_e::ok, p->b.receive(r, 500ms));
  ASSERT_GE(r.fd, 0) << "the attached fd was not received";
  fd_closer_t received(r.fd);
  EXPECT_NE(r.fd, pipefd[0]) << "the received fd is a new descriptor";
  int flags = ::fcntl(r.fd, F_GETFD);
  ASSERT_GE(flags, 0);
  EXPECT_TRUE(flags & FD_CLOEXEC) << "a received fd must be close-on-exec (MSG_CMSG_CLOEXEC)";

  ASSERT_EQ(1, ::write(pipefd[1], "x", 1));
  char c = 0;
  pollfd pfd {r.fd, POLLIN, 0};
  ASSERT_EQ(1, ::poll(&pfd, 1, 500));
  ASSERT_EQ(1, ::read(r.fd, &c, 1));
  EXPECT_EQ('x', c) << "the received fd must refer to the same pipe";
  EXPECT_GE(::fcntl(pipefd[0], F_GETFD), 0) << "the sender keeps ownership of the attached fd";
}

TEST(HandoffChannelTest, ReceiveZeroTimeoutPollsOnce) {
  auto p = make_chan_pair();
  ASSERT_TRUE(p);
  handoff::received_t r;
  auto start = clock_type::now();
  EXPECT_EQ(handoff::recv_status_e::timeout, p->b.receive(r, 0ms)) << "empty channel with zero timeout";
  EXPECT_LT(clock_type::now() - start, 200ms) << "zero timeout must not wait";
  ASSERT_TRUE(p->a.send(ping(1)));
  EXPECT_EQ(handoff::recv_status_e::ok, p->b.receive(r, 0ms)) << "zero timeout must still return a queued message";
}

TEST(HandoffChannelTest, ReceiveTimesOutAfterTheTimeout) {
  auto p = make_chan_pair();
  ASSERT_TRUE(p);
  handoff::received_t r;
  auto start = clock_type::now();
  EXPECT_EQ(handoff::recv_status_e::timeout, p->b.receive(r, 60ms));
  auto elapsed = clock_type::now() - start;
  EXPECT_GE(elapsed, 45ms) << "receive returned before the timeout";
  EXPECT_LT(elapsed, 1000ms) << "receive waited far longer than the timeout";
}

TEST(HandoffChannelTest, ReceiveWakesWhenMessageArrivesDuringWait) {
  auto p = make_chan_pair();
  ASSERT_TRUE(p);
  std::thread sender([&] {
    std::this_thread::sleep_for(50ms);
    p->a.send(ping(8));
  });
  handoff::received_t r;
  auto start = clock_type::now();
  auto status = p->b.receive(r, 1500ms);
  auto elapsed = clock_type::now() - start;
  sender.join();
  ASSERT_EQ(handoff::recv_status_e::ok, status);
  EXPECT_LT(elapsed, 1000ms) << "receive must return when the message arrives, not at the timeout";
}

TEST(HandoffChannelTest, EofAfterPeerCloses) {
  auto p = make_chan_pair();
  ASSERT_TRUE(p);
  p->a.close();
  handoff::received_t r;
  EXPECT_EQ(handoff::recv_status_e::eof, p->b.receive(r, 500ms));
}

TEST(HandoffChannelTest, QueuedMessageIsDeliveredBeforeEof) {
  auto p = make_chan_pair();
  ASSERT_TRUE(p);
  ASSERT_TRUE(p->a.send(ping(3)));
  p->a.close();
  handoff::received_t r;
  ASSERT_EQ(handoff::recv_status_e::ok, p->b.receive(r, 500ms)) << "a message sent before the close must arrive";
  EXPECT_TRUE(r.message == ping(3));
  EXPECT_EQ(handoff::recv_status_e::eof, p->b.receive(r, 500ms));
}

TEST(HandoffChannelTest, SendAfterPeerCloseFailsWithoutSignal) {
  auto p = make_chan_pair();
  ASSERT_TRUE(p);
  p->b.close();
  EXPECT_FALSE(p->a.send(ping(1))) << "send to a closed peer must return false (and must not raise SIGPIPE)";
}

TEST(HandoffChannelTest, SendAfterOwnCloseFails) {
  auto p = make_chan_pair();
  ASSERT_TRUE(p);
  p->a.close();
  EXPECT_FALSE(p->a.send(ping(1))) << "send on a closed channel must fail";
  EXPECT_EQ(-1, p->a.fd()) << "fd() of a closed channel is -1";
}

TEST(HandoffChannelTest, ReceiveOnClosedChannelReportsEof) {
  auto p = make_chan_pair();
  ASSERT_TRUE(p);
  p->a.close();
  handoff::received_t r;
  EXPECT_EQ(handoff::recv_status_e::eof, p->a.receive(r, 0ms)) << "later receives on a closed channel report eof";
}

TEST(HandoffChannelTest, CloseTwiceIsSafe) {
  auto p = make_chan_pair();
  ASSERT_TRUE(p);
  p->a.close();
  p->a.close();
  EXPECT_EQ(-1, p->a.fd());
}

TEST(HandoffChannelTest, ChannelFromMinusOneIsClosed) {
  handoff::channel_t c(-1);
  EXPECT_EQ(-1, c.fd());
  EXPECT_FALSE(c.send(ping(1)));
  handoff::received_t r;
  EXPECT_EQ(handoff::recv_status_e::eof, c.receive(r, 0ms));
}

TEST(HandoffChannelTest, FdReportsOwnedFd) {
  auto fds = handoff::make_channel_pair();
  ASSERT_TRUE(fds.has_value());
  handoff::channel_t c(fds->first);
  fd_closer_t other(fds->second);
  EXPECT_EQ(fds->first, c.fd());
}

TEST(HandoffChannelTest, DestructorClosesTheFd) {
  auto fds = handoff::make_channel_pair();
  ASSERT_TRUE(fds.has_value());
  fd_closer_t other(fds->second);
  {
    handoff::channel_t c(fds->first);
  }
  // The other end must now see eof.
  handoff::channel_t peer(::dup(other.fd));
  handoff::received_t r;
  // other.fd still open (and dup'd); the destroyed end is gone, so a read sees eof.
  EXPECT_EQ(handoff::recv_status_e::eof, peer.receive(r, 500ms)) << "the destructor must close the channel fd";
}

TEST(HandoffChannelTest, OversizedDatagramIsReported) {
  auto p = make_raw_pair();
  ASSERT_TRUE(p);
  std::string big(handoff::protocol::max_message_size + 1000, 'x');
  ASSERT_EQ(static_cast<ssize_t>(big.size()), raw_send(p->raw.fd, big));
  handoff::received_t r;
  EXPECT_EQ(handoff::recv_status_e::oversized, p->channel.receive(r, 500ms));
}

TEST(HandoffChannelTest, OversizedDatagramClosesItsAttachedFd) {
  auto p = make_raw_pair();
  ASSERT_TRUE(p);
  int pipefd[2];
  ASSERT_EQ(0, ::pipe(pipefd));
  fd_closer_t rd(pipefd[0]);
  fd_closer_t wr(pipefd[1]);
  std::string big(handoff::protocol::max_message_size + 1000, 'x');
  ASSERT_EQ(static_cast<ssize_t>(big.size()), raw_send(p->raw.fd, big, pipefd[0]));
  handoff::received_t r;
  ASSERT_EQ(handoff::recv_status_e::oversized, p->channel.receive(r, 500ms));
  EXPECT_EQ(-1, r.fd) << "no fd is handed out with an oversized datagram";
  // Drop our own read end. If the channel closed its received copy, the pipe has no reader left.
  ::close(rd.fd);
  rd.fd = -1;
  pollfd pfd {wr.fd, 0, 0};
  ASSERT_EQ(1, ::poll(&pfd, 1, 200));
  EXPECT_TRUE(pfd.revents & POLLERR) << "an fd that arrived with an oversized datagram must be closed (leak)";
}

namespace {
  std::size_t open_fd_count() {
    std::size_t n = 0;
    for (const auto &e : std::filesystem::directory_iterator("/proc/self/fd")) {
      (void) e;
      ++n;
    }
    return n;
  }
}  // namespace

TEST(HandoffChannelTest, TooManyAttachedFdsIsMalformedAndLeaksNothing) {
  auto p = make_raw_pair();
  ASSERT_TRUE(p);
  int null = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  ASSERT_GE(null, 0);
  fd_closer_t null_closer(null);
  constexpr int count = 16;
  int fds[count];
  for (int &f : fds) {
    f = null;
  }
  const std::string payload = R"({"type":"PING","seq":1})";
  iovec iov {};
  iov.iov_base = const_cast<char *>(payload.data());
  iov.iov_len = payload.size();
  msghdr msg {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * count)] {};
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  cmsghdr *c = CMSG_FIRSTHDR(&msg);
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  c->cmsg_len = CMSG_LEN(sizeof(int) * count);
  std::memcpy(CMSG_DATA(c), fds, sizeof(int) * count);
  ASSERT_EQ(static_cast<ssize_t>(payload.size()), ::sendmsg(p->raw.fd, &msg, MSG_NOSIGNAL));

  auto before = open_fd_count();
  handoff::received_t r;
  EXPECT_EQ(handoff::recv_status_e::malformed, p->channel.receive(r, 500ms))
    << "a datagram whose control data was truncated (MSG_CTRUNC) is malformed";
  EXPECT_EQ(-1, r.fd);
  EXPECT_EQ(before, open_fd_count()) << "fds received with a truncated control message must not stay open (leak)";
}

TEST(HandoffChannelTest, CloseWakesReceiveBlockedWithoutTimeout) {
  auto fds = handoff::make_channel_pair();
  ASSERT_TRUE(fds.has_value());
  auto ch = std::make_shared<handoff::channel_t>(fds->first);
  fd_closer_t peer(fds->second);
  auto status = std::make_shared<std::atomic<int>>(-1);
  std::promise<void> done;
  auto fut = done.get_future();
  std::thread t([&, ch, status] {
    handoff::received_t r;
    *status = static_cast<int>(ch->receive(r, -1ms));
    done.set_value();
  });
  std::this_thread::sleep_for(60ms);
  ch->close();
  bool woke = fut.wait_for(1500ms) == std::future_status::ready;
  if (woke) {
    t.join();
    // SPEC-QUESTION: the status of a receive interrupted by close() is not named; eof is expected.
    EXPECT_EQ(static_cast<int>(handoff::recv_status_e::eof), status->load());
  } else {
    t.detach();
    ADD_FAILURE() << "close() did not wake a receive blocked with a negative timeout";
  }
}

TEST(HandoffChannelTest, MalformedDatagramIsReported) {
  auto p = make_raw_pair();
  ASSERT_TRUE(p);
  ASSERT_EQ(11, raw_send(p->raw.fd, "not json at"));
  handoff::received_t r;
  EXPECT_EQ(handoff::recv_status_e::malformed, p->channel.receive(r, 500ms));
}

TEST(HandoffChannelTest, MalformedDatagramClosesItsAttachedFd) {
  auto p = make_raw_pair();
  ASSERT_TRUE(p);
  int pipefd[2];
  ASSERT_EQ(0, ::pipe(pipefd));
  fd_closer_t rd(pipefd[0]);
  fd_closer_t wr(pipefd[1]);
  ASSERT_EQ(8, raw_send(p->raw.fd, "garbage!", pipefd[0]));
  handoff::received_t r;
  ASSERT_EQ(handoff::recv_status_e::malformed, p->channel.receive(r, 500ms));
  EXPECT_EQ(-1, r.fd);
  ::close(rd.fd);
  rd.fd = -1;
  pollfd pfd {wr.fd, 0, 0};
  ASSERT_EQ(1, ::poll(&pfd, 1, 200));
  EXPECT_TRUE(pfd.revents & POLLERR) << "an fd that arrived with a malformed datagram must be closed (leak)";
}

TEST(HandoffChannelTest, UnknownTypeIsDeliveredAsUnknown) {
  auto p = make_raw_pair();
  ASSERT_TRUE(p);
  const std::string payload = R"({"type":"FUTURE_THING","x":1})";
  ASSERT_EQ(static_cast<ssize_t>(payload.size()), raw_send(p->raw.fd, payload));
  handoff::received_t r;
  ASSERT_EQ(handoff::recv_status_e::ok, p->channel.receive(r, 500ms));
  ASSERT_TRUE(std::holds_alternative<handoff::protocol::unknown_t>(r.message));
  EXPECT_EQ("FUTURE_THING", std::get<handoff::protocol::unknown_t>(r.message).type);
}

TEST(HandoffChannelTest, ValidRawDatagramDecodes) {
  auto p = make_raw_pair();
  ASSERT_TRUE(p);
  const std::string payload = R"({"type":"PING","seq":7})";
  ASSERT_EQ(static_cast<ssize_t>(payload.size()), raw_send(p->raw.fd, payload));
  handoff::received_t r;
  ASSERT_EQ(handoff::recv_status_e::ok, p->channel.receive(r, 500ms));
  EXPECT_TRUE(r.message == ping(7));
}

TEST(HandoffChannelTest, SendNeverBlocksWhenPeerBufferIsFull) {
  auto p = make_chan_pair();
  ASSERT_TRUE(p);
  auto start = clock_type::now();
  bool refused = false;
  int sent = 0;
  for (int i = 0; i < 200000 && clock_type::now() - start < 3s; ++i) {
    if (!p->a.send(ping(i))) {
      refused = true;
      break;
    }
    ++sent;
  }
  auto elapsed = clock_type::now() - start;
  EXPECT_TRUE(refused) << "send never reported a full buffer after " << sent << " messages";
  EXPECT_LT(elapsed, 2000ms) << "send must not block when the peer does not read";
  // Draining makes room again.
  handoff::received_t r;
  ASSERT_EQ(handoff::recv_status_e::ok, p->b.receive(r, 500ms));
  EXPECT_TRUE(r.message == ping(0)) << "the first queued message must still be intact";
  bool again = false;
  for (int i = 0; i < 100 && !again; ++i) {
    again = p->a.send(ping(999));
    if (!again) {
      handoff::received_t d;
      p->b.receive(d, 0ms);
    }
  }
  EXPECT_TRUE(again) << "send works again after the peer reads";
}

TEST(HandoffChannelTest, ConcurrentSendersDeliverWholeMessages) {
  auto p = make_chan_pair();
  ASSERT_TRUE(p);
  constexpr int per_thread = 80;
  std::atomic<int> failed {0};
  auto sender = [&](std::uint64_t base) {
    for (int i = 0; i < per_thread; ++i) {
      handoff::protocol::sessions_reply_t sr;
      sr.request_id = base * 1000 + i;
      for (int k = 0; k < 20; ++k) {
        sr.sessions.push_back({static_cast<std::uint32_t>(sr.request_id), 1000, "udp/ipv4", "lan"});
      }
      auto deadline = clock_type::now() + 3s;
      while (!p->a.send(sr)) {
        if (clock_type::now() > deadline) {
          ++failed;
          return;
        }
        std::this_thread::yield();
      }
    }
  };
  std::thread t1(sender, 1);
  std::thread t2(sender, 2);

  int got1 = 0;
  int got2 = 0;
  int bad = 0;
  for (int i = 0; i < 2 * per_thread; ++i) {
    handoff::received_t r;
    if (p->b.receive(r, 2000ms) != handoff::recv_status_e::ok) {
      ++bad;
      break;
    }
    auto *sr = std::get_if<handoff::protocol::sessions_reply_t>(&r.message);
    if (!sr || sr->sessions.size() != 20) {
      ++bad;
      continue;
    }
    for (const auto &s : sr->sessions) {
      if (s.id != static_cast<std::uint32_t>(sr->request_id)) {
        ++bad;
        break;
      }
    }
    (sr->request_id / 1000 == 1 ? got1 : got2)++;
  }
  t1.join();
  t2.join();
  EXPECT_EQ(0, failed.load()) << "a sender could not send within 3 s";
  EXPECT_EQ(0, bad) << "a message from concurrent senders was lost, mixed or malformed";
  EXPECT_EQ(per_thread, got1);
  EXPECT_EQ(per_thread, got2);
}

TEST(HandoffChannelTest, SendAndReceiveRunConcurrently) {
  auto p = make_chan_pair();
  ASSERT_TRUE(p);
  std::atomic<bool> got {false};
  std::thread receiver([&] {
    handoff::received_t r;
    got = p->a.receive(r, 1500ms) == handoff::recv_status_e::ok;
  });
  std::this_thread::sleep_for(30ms);
  EXPECT_TRUE(p->a.send(ping(1))) << "send must not wait for a receive in progress on the same channel";
  ASSERT_TRUE(p->b.send(ping(2)));
  receiver.join();
  EXPECT_TRUE(got.load());
}

// ---------------------------------------------------------------------------------------------------------------------
// request_tracker_t
// ---------------------------------------------------------------------------------------------------------------------

TEST(HandoffRequestTrackerTest, IdsStartAtOneAndIncrease) {
  handoff::request_tracker_t t;
  auto a = t.begin();
  auto b = t.begin();
  auto c = t.begin();
  EXPECT_EQ(1u, a) << "request ids start at 1";
  EXPECT_LT(a, b);
  EXPECT_LT(b, c);
}

TEST(HandoffRequestTrackerTest, DeliverBeforeWaitIsCollected) {
  handoff::request_tracker_t t;
  auto id = t.begin();
  EXPECT_TRUE(t.deliver(id, ping(5))) << "a reply for a started request is accepted";
  auto reply = t.wait(id, 200ms);
  ASSERT_TRUE(reply.has_value()) << "a reply delivered before wait() must be collected by wait()";
  EXPECT_TRUE(*reply == ping(5));
}

TEST(HandoffRequestTrackerTest, DeliverDuringWaitWakesTheWaiter) {
  handoff::request_tracker_t t;
  auto id = t.begin();
  std::thread d([&] {
    std::this_thread::sleep_for(50ms);
    t.deliver(id, ping(6));
  });
  auto start = clock_type::now();
  auto reply = t.wait(id, 1500ms);
  auto elapsed = clock_type::now() - start;
  d.join();
  ASSERT_TRUE(reply.has_value());
  EXPECT_TRUE(*reply == ping(6));
  EXPECT_LT(elapsed, 1000ms) << "wait() must return when the reply arrives";
}

TEST(HandoffRequestTrackerTest, WaitTimesOutAndLateDeliverIsDropped) {
  handoff::request_tracker_t t;
  auto id = t.begin();
  auto start = clock_type::now();
  auto reply = t.wait(id, 50ms);
  auto elapsed = clock_type::now() - start;
  EXPECT_FALSE(reply.has_value());
  EXPECT_GE(elapsed, 40ms);
  EXPECT_LT(elapsed, 1000ms);
  EXPECT_FALSE(t.deliver(id, ping(1))) << "a late reply (after the waiter timed out) must be dropped";
}

TEST(HandoffRequestTrackerTest, UnknownIdIsNotDelivered) {
  handoff::request_tracker_t t;
  EXPECT_FALSE(t.deliver(999, ping(1)));
  t.begin();
  EXPECT_FALSE(t.deliver(999, ping(1)));
}

TEST(HandoffRequestTrackerTest, SecondDeliverForTheSameIdFails) {
  handoff::request_tracker_t t;
  auto id = t.begin();
  EXPECT_TRUE(t.deliver(id, ping(1)));
  EXPECT_FALSE(t.deliver(id, ping(2))) << "a request is answered once";
  auto reply = t.wait(id, 100ms);
  ASSERT_TRUE(reply.has_value());
  EXPECT_TRUE(*reply == ping(1)) << "the first reply is the one collected";
}

TEST(HandoffRequestTrackerTest, DeliverAfterWaitCollectedFails) {
  handoff::request_tracker_t t;
  auto id = t.begin();
  ASSERT_TRUE(t.deliver(id, ping(1)));
  ASSERT_TRUE(t.wait(id, 100ms).has_value());
  EXPECT_FALSE(t.deliver(id, ping(2))) << "after wait() returns the id is forgotten";
}

TEST(HandoffRequestTrackerTest, WaitOnUnknownIdReturnsNothingQuickly) {
  handoff::request_tracker_t t;
  auto start = clock_type::now();
  EXPECT_FALSE(t.wait(12345, 1500ms).has_value());
  EXPECT_LT(clock_type::now() - start, 1000ms) << "waiting for an unknown id must not wait for the timeout";
}

TEST(HandoffRequestTrackerTest, RepliesAreMatchedByIdNotOrder) {
  handoff::request_tracker_t t;
  auto id1 = t.begin();
  auto id2 = t.begin();
  ASSERT_TRUE(t.deliver(id2, ping(22)));
  ASSERT_TRUE(t.deliver(id1, ping(11)));
  auto r1 = t.wait(id1, 100ms);
  auto r2 = t.wait(id2, 100ms);
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  EXPECT_TRUE(*r1 == ping(11));
  EXPECT_TRUE(*r2 == ping(22));
}

TEST(HandoffRequestTrackerTest, FailAllWakesWaiterWithNothing) {
  handoff::request_tracker_t t;
  auto id = t.begin();
  std::promise<std::pair<bool, std::chrono::milliseconds>> done;
  auto fut = done.get_future();
  std::thread w([&] {
    auto start = clock_type::now();
    auto reply = t.wait(id, 5000ms);
    done.set_value({reply.has_value(), std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - start)});
  });
  std::this_thread::sleep_for(50ms);
  t.fail_all();
  ASSERT_EQ(std::future_status::ready, fut.wait_for(1500ms)) << "fail_all() must wake a waiter quickly";
  auto [had_reply, elapsed] = fut.get();
  w.join();
  EXPECT_FALSE(had_reply) << "a waiter woken by fail_all() gets no reply";
  EXPECT_LT(elapsed, 1500ms);
}

TEST(HandoffRequestTrackerTest, FailAllWakesEveryWaiter) {
  handoff::request_tracker_t t;
  auto id1 = t.begin();
  auto id2 = t.begin();
  std::atomic<int> woke {0};
  auto waiter = [&](std::uint64_t id) {
    if (!t.wait(id, 5000ms).has_value()) {
      ++woke;
    }
  };
  std::thread w1(waiter, id1);
  std::thread w2(waiter, id2);
  std::this_thread::sleep_for(50ms);
  t.fail_all();
  w1.join();
  w2.join();
  EXPECT_EQ(2, woke.load());
}

TEST(HandoffRequestTrackerTest, BeginStillWorksAfterFailAll) {
  handoff::request_tracker_t t;
  t.begin();
  t.fail_all();
  auto id = t.begin();
  EXPECT_TRUE(t.deliver(id, ping(1))) << "requests begun after fail_all() work normally";
  auto reply = t.wait(id, 100ms);
  ASSERT_TRUE(reply.has_value());
  EXPECT_TRUE(*reply == ping(1));
}

#endif  // __linux__
