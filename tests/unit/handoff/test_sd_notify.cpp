/**
 * @file tests/unit/handoff/test_sd_notify.cpp
 * @brief Blind tests for the raw systemd notifications and unit discovery (src/handoff/sd_notify.*), written from the
 *        spec.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <atomic>
  #include <chrono>
  #include <cstddef>
  #include <cstdlib>
  #include <cstring>
  #include <fcntl.h>
  #include <filesystem>
  #include <optional>
  #include <poll.h>
  #include <string>
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <sys/uio.h>
  #include <sys/un.h>
  #include <thread>
  #include <unistd.h>
  #include <vector>

  // local includes
  #include "src/handoff/sd_notify.h"

using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace sd = handoff::sd;

namespace {

  /**
   * @brief A temporary directory removed on destruction.
   */
  struct temp_dir_t {
    fs::path path;  ///< Directory path.

    temp_dir_t() {
      std::string templ = (fs::temp_directory_path() / "hf_sd_XXXXXX").string();
      std::vector<char> buf(templ.begin(), templ.end());
      buf.push_back('\0');
      char *made = mkdtemp(buf.data());
      if (made != nullptr) {
        path = made;
      }
    }

    ~temp_dir_t() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  };

  /**
   * @brief One datagram received on a test socket.
   */
  struct received_t {
    std::string payload;  ///< Datagram bytes.
    int fd {-1};  ///< Fd attached with SCM_RIGHTS, or -1.
  };

  /**
   * @brief Receive one datagram, waiting at most `timeout_ms`.
   *
   * @return The datagram, or nullopt when none arrived.
   */
  std::optional<received_t> recv_one(int sock, int timeout_ms) {
    pollfd pfd {sock, POLLIN, 0};
    if (poll(&pfd, 1, timeout_ms) <= 0) {
      return std::nullopt;
    }
    char buf[512];
    iovec iov {buf, sizeof(buf)};
    char control[CMSG_SPACE(sizeof(int) * 4)];
    std::memset(control, 0, sizeof(control));
    msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    ssize_t n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    if (n < 0) {
      return std::nullopt;
    }
    received_t out;
    out.payload.assign(buf, static_cast<std::size_t>(n));
    for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
        std::memcpy(&out.fd, CMSG_DATA(c), sizeof(int));
      }
    }
    return out;
  }

  /**
   * @brief Bind a datagram socket to a filesystem path.
   *
   * @return The socket fd, or -1.
   */
  int bind_path(const fs::path &path) {
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::string p = path.string();
    std::strncpy(addr.sun_path, p.c_str(), sizeof(addr.sun_path) - 1);
    if (fd < 0 || bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      if (fd >= 0) {
        close(fd);
      }
      return -1;
    }
    return fd;
  }

  /**
   * @brief Bind a datagram socket to an abstract name (without the leading NUL).
   *
   * @return The socket fd, or -1.
   */
  int bind_abstract(const std::string &name) {
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return -1;
    }
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    std::memcpy(addr.sun_path + 1, name.data(), name.size());
    socklen_t len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), len) != 0) {
      close(fd);
      return -1;
    }
    return fd;
  }

  /**
   * @brief Unique abstract socket name for one test.
   */
  std::string unique_abstract_name(const std::string &tag) {
    static std::atomic<int> counter {0};
    return "hf_sd_test_" + tag + "_" + std::to_string(getpid()) + "_" + std::to_string(++counter);
  }

  /**
   * @brief Closes an fd on destruction when it is valid.
   */
  struct fd_guard_t {
    int fd {-1};  ///< Fd to close.

    ~fd_guard_t() {
      if (fd >= 0) {
        close(fd);
      }
    }
  };

}  // namespace

/**
 * @brief Tests for raw notifications, the barrier and the unit and version parsers.
 */
class HandoffSdNotifyTest: public testing::Test {};

TEST_F(HandoffSdNotifyTest, NotifyDeliversPayloadExactly) {
  temp_dir_t tmp;
  fd_guard_t rx {bind_path(tmp.path / "notify.sock")};
  ASSERT_GE(rx.fd, 0) << "the test receiver socket must bind";

  ASSERT_TRUE(sd::notify((tmp.path / "notify.sock").string(), "READY=1"))
    << "notify must return true when the datagram was sent to a live receiver";
  auto got = recv_one(rx.fd, 1000);
  ASSERT_TRUE(got.has_value()) << "the receiver must get the datagram";
  EXPECT_EQ(got->payload, "READY=1") << "the payload must arrive exactly as given";
  EXPECT_EQ(got->fd, -1) << "no fd must be attached when attach_fd is not given";
}

TEST_F(HandoffSdNotifyTest, NotifyAttachesFdWithScmRights) {
  temp_dir_t tmp;
  fd_guard_t rx {bind_path(tmp.path / "notify.sock")};
  ASSERT_GE(rx.fd, 0) << "the test receiver socket must bind";

  int pipefd[2] = {-1, -1};
  ASSERT_EQ(pipe2(pipefd, O_CLOEXEC), 0) << "the test needs a pipe";
  fd_guard_t pr {pipefd[0]};
  fd_guard_t pw {pipefd[1]};

  ASSERT_TRUE(sd::notify((tmp.path / "notify.sock").string(), "BARRIER=1", pw.fd))
    << "notify with attach_fd must return true";
  auto got = recv_one(rx.fd, 1000);
  ASSERT_TRUE(got.has_value()) << "the receiver must get the datagram";
  EXPECT_EQ(got->payload, "BARRIER=1") << "the payload must arrive exactly as given";
  ASSERT_GE(got->fd, 0) << "the receiver must get a file descriptor through SCM_RIGHTS";
  fd_guard_t received {got->fd};

  const char byte = 'x';
  ASSERT_EQ(write(received.fd, &byte, 1), 1) << "the received fd must be writable";
  char back = 0;
  ASSERT_EQ(read(pr.fd, &back, 1), 1) << "the write end received over SCM_RIGHTS must feed the test pipe";
  EXPECT_EQ(back, 'x') << "the attached fd must be the write end of the pipe";
}

TEST_F(HandoffSdNotifyTest, NotifyAbstractSocketWithAtSign) {
  std::string name = unique_abstract_name("abs");
  fd_guard_t rx {bind_abstract(name)};
  ASSERT_GE(rx.fd, 0) << "the abstract test receiver must bind";

  ASSERT_TRUE(sd::notify("@" + name, "READY=1"))
    << "a NOTIFY_SOCKET starting with '@' must send to an abstract socket";
  auto got = recv_one(rx.fd, 1000);
  ASSERT_TRUE(got.has_value()) << "the abstract receiver must get the datagram";
  EXPECT_EQ(got->payload, "READY=1") << "the payload must arrive exactly as given on an abstract socket";
}

TEST_F(HandoffSdNotifyTest, NotifyEmptySocketIsFalse) {
  EXPECT_FALSE(sd::notify("", "READY=1"))
    << "an empty NOTIFY_SOCKET means not running under systemd, so notify must return false";
}

TEST_F(HandoffSdNotifyTest, NotifyMissingReceiverIsFalse) {
  temp_dir_t tmp;
  EXPECT_FALSE(sd::notify((tmp.path / "absent.sock").string(), "READY=1"))
    << "sending to a socket with no receiver must return false";
}

TEST_F(HandoffSdNotifyTest, MainPidPayloadFormat) {
  EXPECT_EQ(sd::mainpid_payload(1234), "MAINPID=1234") << "the payload must be MAINPID=<pid>";
  EXPECT_EQ(sd::mainpid_payload(1), "MAINPID=1") << "a pid of 1 must still be formatted as MAINPID=1";
}

TEST_F(HandoffSdNotifyTest, BarrierCompletesWhenReceiverClosesFd) {
  temp_dir_t tmp;
  fs::path sock = tmp.path / "notify.sock";
  fd_guard_t rx {bind_path(sock)};
  ASSERT_GE(rx.fd, 0) << "the test receiver socket must bind";

  std::string payload;
  bool got_fd = false;
  std::thread receiver([&] {
    auto got = recv_one(rx.fd, 3000);
    if (got.has_value()) {
      payload = got->payload;
      if (got->fd >= 0) {
        got_fd = true;
        close(got->fd);  // Closing the received write end lets the barrier's read end see end-of-file.
      }
    }
  });

  bool done = sd::barrier(sock.string(), 2000ms);
  receiver.join();
  EXPECT_EQ(payload, "BARRIER=1") << "the barrier must send exactly BARRIER=1 to NOTIFY_SOCKET";
  EXPECT_TRUE(got_fd) << "the barrier datagram must carry a pipe write end through SCM_RIGHTS";
  EXPECT_TRUE(done) << "the barrier must complete once the receiver closes the received fd";
}

TEST_F(HandoffSdNotifyTest, BarrierTimesOutWhenReceiverKeepsFd) {
  temp_dir_t tmp;
  fs::path sock = tmp.path / "notify.sock";
  fd_guard_t rx {bind_path(sock)};
  ASSERT_GE(rx.fd, 0) << "the test receiver socket must bind";

  std::atomic<bool> release {false};
  std::atomic<bool> holding {false};
  int held_fd = -1;
  std::thread receiver([&] {
    auto got = recv_one(rx.fd, 3000);
    if (got.has_value() && got->fd >= 0) {
      held_fd = got->fd;
      holding = true;
    }
    // Keep the received fd open until the test releases it, so the read end never reports end-of-file.
    for (int i = 0; i < 300 && !release.load(); ++i) {
      std::this_thread::sleep_for(10ms);
    }
  });

  auto begin = std::chrono::steady_clock::now();
  bool done = sd::barrier(sock.string(), 300ms);
  auto elapsed = std::chrono::steady_clock::now() - begin;
  EXPECT_FALSE(done) << "the barrier must return false while the receiver still holds the write end";
  EXPECT_LT(elapsed, 2s) << "the barrier must give up at its timeout";
  EXPECT_TRUE(holding.load()) << "the test receiver must have taken the barrier's fd to make the case meaningful";

  release = true;
  receiver.join();
  if (held_fd >= 0) {
    close(held_fd);
  }
}

TEST_F(HandoffSdNotifyTest, BarrierEmptySocketIsFalse) {
  EXPECT_FALSE(sd::barrier("", 100ms))
    << "an empty NOTIFY_SOCKET means no systemd, so the barrier must return false";
}

TEST_F(HandoffSdNotifyTest, UnitFromCgroupSystemUnit) {
  auto unit = sd::unit_from_cgroup("0::/system.slice/sunshine.service\n");
  ASSERT_TRUE(unit.has_value()) << "a system unit cgroup line must yield a unit name";
  EXPECT_EQ(*unit, "sunshine.service") << "the unit must be the .service path component";
}

TEST_F(HandoffSdNotifyTest, UnitFromCgroupUserUnit) {
  auto unit = sd::unit_from_cgroup(
    "0::/user.slice/user-1000.slice/user@1000.service/app.slice/sunshine.service\n"
  );
  ASSERT_TRUE(unit.has_value()) << "a user unit cgroup line must yield a unit name";
  EXPECT_EQ(*unit, "sunshine.service") << "for a user unit the unit must be sunshine.service, not user@1000.service";
}

TEST_F(HandoffSdNotifyTest, UnitFromCgroupUsesV2LineInMixedFile) {
  std::string mixed =
    "1:name=systemd:/system.slice/old.service\n"
    "0::/system.slice/sunshine.service\n";
  auto unit = sd::unit_from_cgroup(mixed);
  ASSERT_TRUE(unit.has_value()) << "a mixed v1 and v2 file must still yield the v2 unit";
  EXPECT_EQ(*unit, "sunshine.service") << "the cgroup v2 line (0::) must be used, not the v1 line";
}

TEST_F(HandoffSdNotifyTest, UnitFromCgroupLastServiceComponentWins) {
  auto unit = sd::unit_from_cgroup("0::/system.slice/outer.service/inner.service\n");
  ASSERT_TRUE(unit.has_value()) << "a path with two .service components must yield a unit";
  EXPECT_EQ(*unit, "inner.service") << "the unit is the last path component that ends in .service";
}

TEST_F(HandoffSdNotifyTest, UnitFromCgroupWithoutServiceIsNullopt) {
  EXPECT_FALSE(sd::unit_from_cgroup("0::/user.slice/user-1000.slice/session-2.scope\n").has_value())
    << "a cgroup path with no .service component must give nullopt";
  EXPECT_FALSE(sd::unit_from_cgroup("").has_value()) << "empty input must give nullopt";
}

TEST_F(HandoffSdNotifyTest, IsUserUnitForUserManagerPath) {
  EXPECT_TRUE(sd::is_user_unit("0::/user.slice/user-1000.slice/user@1000.service/app.slice/sunshine.service\n"))
    << "a path inside user@<uid>.service must be a user unit";
}

TEST_F(HandoffSdNotifyTest, IsUserUnitFalseForSystemPath) {
  EXPECT_FALSE(sd::is_user_unit("0::/system.slice/sunshine.service\n"))
    << "a system unit path must not be a user unit";
  EXPECT_FALSE(sd::is_user_unit("")) << "empty input must not be a user unit";
}

TEST_F(HandoffSdNotifyTest, ParseSystemdVersionFromFirstLine) {
  auto version = sd::parse_systemd_version("systemd 256 (256.7-1-arch)\n+PAM +AUDIT\n");
  ASSERT_TRUE(version.has_value()) << "the first line of systemctl --version must parse";
  EXPECT_EQ(*version, 256) << "the major version must be 256";
}

TEST_F(HandoffSdNotifyTest, ParseSystemdVersionOlderRelease) {
  auto version = sd::parse_systemd_version("systemd 254 (254.1)\n");
  ASSERT_TRUE(version.has_value()) << "an older systemd line must parse";
  EXPECT_EQ(*version, 254) << "the major version must be 254";
}

TEST_F(HandoffSdNotifyTest, ParseSystemdVersionGarbageIsNullopt) {
  EXPECT_FALSE(sd::parse_systemd_version("garbage").has_value()) << "non-version output must give nullopt";
  EXPECT_FALSE(sd::parse_systemd_version("").has_value()) << "empty output must give nullopt";
}

TEST_F(HandoffSdNotifyTest, ParseMainPidDecimalWithNewline) {
  auto pid = sd::parse_mainpid("1234\n");
  ASSERT_TRUE(pid.has_value()) << "a decimal pid followed by a newline must parse";
  EXPECT_EQ(*pid, 1234) << "the pid must be 1234";
}

TEST_F(HandoffSdNotifyTest, ParseMainPidZeroMeansNone) {
  auto pid = sd::parse_mainpid("0\n");
  ASSERT_TRUE(pid.has_value()) << "0 is a valid number and must parse";
  EXPECT_EQ(*pid, 0) << "a pid of 0 means no main process and must be returned as 0";
}

TEST_F(HandoffSdNotifyTest, ParseMainPidGarbageIsNullopt) {
  EXPECT_FALSE(sd::parse_mainpid("garbage\n").has_value()) << "non-numeric output must give nullopt";
  EXPECT_FALSE(sd::parse_mainpid("").has_value()) << "empty output must give nullopt";
  EXPECT_FALSE(sd::parse_mainpid("12x\n").has_value()) << "a pid with trailing junk must give nullopt";
}

#endif  // __linux__
