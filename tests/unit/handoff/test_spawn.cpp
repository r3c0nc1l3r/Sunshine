/**
 * @file tests/unit/handoff/test_spawn.cpp
 * @brief Tests for spawning, listener validation and the kill-first reap (src/handoff/spawn.h).
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <cerrno>
  #include <chrono>
  #include <csignal>
  #include <cstdint>
  #include <cstdlib>
  #include <cstring>
  #include <filesystem>
  #include <fstream>
  #include <optional>
  #include <set>
  #include <sstream>
  #include <string>
  #include <thread>
  #include <vector>

  // platform includes
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <poll.h>
  #include <pthread.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <sys/wait.h>
  #include <unistd.h>

  // local includes
  #include "src/handoff/spawn.h"

  #ifndef P_PIDFD
    #define P_PIDFD 3
  #endif

using namespace std::chrono_literals;

extern char **environ;

namespace {
  using clock_type = std::chrono::steady_clock;

  /**
   * @brief Closes an fd on scope exit.
   */
  struct fd_t {
    int fd;  ///< Owned fd, or -1.

    explicit fd_t(int f = -1):
        fd(f) {}

    ~fd_t() {
      reset();
    }

    void reset(int f = -1) {
      if (fd >= 0) {
        ::close(fd);
      }
      fd = f;
    }

    fd_t(const fd_t &) = delete;
    fd_t &operator=(const fd_t &) = delete;
  };

  /**
   * @brief Temp directory removed on scope exit.
   */
  struct temp_dir_t {
    std::filesystem::path path;  ///< Directory.

    temp_dir_t() {
      std::string tmpl = (std::filesystem::temp_directory_path() / "handoff-spawn-XXXXXX").string();
      std::vector<char> buf(tmpl.begin(), tmpl.end());
      buf.push_back('\0');
      if (::mkdtemp(buf.data())) {
        path = buf.data();
      }
    }

    ~temp_dir_t() {
      std::error_code ec;
      if (!path.empty()) {
        std::filesystem::remove_all(path, ec);
      }
    }
  };

  std::string read_file(const std::filesystem::path &p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  std::vector<std::string> lines_of(const std::string &s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
      out.push_back(line);
    }
    return out;
  }

  /**
   * @brief Wait until a pidfd is readable (the process exited).
   */
  bool wait_pidfd(int pidfd, int timeout_ms) {
    pollfd p {pidfd, POLLIN, 0};
    return ::poll(&p, 1, timeout_ms) == 1 && (p.revents & POLLIN);
  }

  /**
   * @brief Reap a child through its pidfd and close the pidfd.
   */
  int reap(handoff::child_t &child) {
    siginfo_t si {};
    int status = -1;
    if (::waitid(static_cast<idtype_t>(P_PIDFD), child.pidfd, &si, WEXITED) == 0) {
      status = si.si_status;
    }
    ::close(child.pidfd);
    child.pidfd = -1;
    return status;
  }

  /**
   * @brief Kill and reap a child that a test left running (cleanup only).
   */
  void cleanup_child(handoff::child_t &child) {
    if (child.pidfd >= 0) {
      handoff::kill_and_reap(child.pidfd, 1000ms, nullptr);
      ::close(child.pidfd);
      child.pidfd = -1;
    }
  }

  /**
   * @brief Five distinct sockets for a spawn request: four TCP listeners and one socketpair end.
   */
  struct sources_t {
    fd_t https;  ///< Listener.
    fd_t http;  ///< Listener.
    fd_t webui;  ///< Listener.
    fd_t rtsp;  ///< Listener.
    fd_t channel;  ///< Channel end.
    fd_t channel_peer;  ///< Other end of the channel.

    static int listener() {
      int s = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (s < 0) {
        return -1;
      }
      sockaddr_in addr {};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      addr.sin_port = 0;
      if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(s, 4) != 0) {
        ::close(s);
        return -1;
      }
      return s;
    }

    bool init() {
      https.reset(listener());
      http.reset(listener());
      webui.reset(listener());
      rtsp.reset(listener());
      int sp[2];
      if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sp) != 0) {
        return false;
      }
      channel.reset(sp[0]);
      channel_peer.reset(sp[1]);
      return https.fd >= 0 && http.fd >= 0 && webui.fd >= 0 && rtsp.fd >= 0;
    }

    handoff::inherited_fds_t fds() const {
      handoff::inherited_fds_t f;
      f.https = https.fd;
      f.http = http.fd;
      f.webui = webui.fd;
      f.rtsp = rtsp.fd;
      f.channel = channel.fd;
      return f;
    }
  };

  std::string link_of(int fd) {
    char buf[256];
    auto n = ::readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), buf, sizeof(buf) - 1);
    return n > 0 ? std::string(buf, n) : std::string();
  }

  handoff::spawn_request_t shell_request(const sources_t &src, const std::string &script, const std::filesystem::path &out) {
    handoff::spawn_request_t req;
    req.binary = "/bin/sh";
    req.argv = {"sh", "-c", script, "sh", out.string()};
    req.fds = src.fds();
    return req;
  }

  /**
   * @brief Spawn a shell script, wait for it to exit, return the exit status (or -1).
   */
  int run_shell(const handoff::spawn_request_t &req, std::string *error = nullptr) {
    std::string err;
    auto child = handoff::spawn_child(req, &err);
    if (error) {
      *error = err;
    }
    if (!child) {
      return -2;
    }
    if (!wait_pidfd(child->pidfd, 5000)) {
      cleanup_child(*child);
      return -3;
    }
    return reap(*child);
  }
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// child_environment
// ---------------------------------------------------------------------------------------------------------------------

TEST(HandoffSpawnTest, ChildEnvironmentExactEntriesInOrder) {
  auto env = handoff::child_environment(5555, 2);
  std::vector<std::string> expected {
    "SUNSHINE_HANDOFF_FDS=https:3,http:4,webui:5,rtsp:6",
    "SUNSHINE_HANDOFF_CHANNEL=7",
    "SUNSHINE_HANDOFF_UDP_BASE=5555",
    "SUNSHINE_HANDOFF_GENERATION=2",
  };
  EXPECT_EQ(expected, env);
}

TEST(HandoffSpawnTest, ChildEnvironmentCarriesTheArguments) {
  auto env = handoff::child_environment(48000, 17);
  ASSERT_EQ(4u, env.size());
  EXPECT_EQ("SUNSHINE_HANDOFF_UDP_BASE=48000", env[2]);
  EXPECT_EQ("SUNSHINE_HANDOFF_GENERATION=17", env[3]);
}

TEST(HandoffSpawnTest, ConstantsMatchTheLayout) {
  EXPECT_EQ(3, handoff::first_inherited_fd);
  EXPECT_EQ(7, handoff::channel_child_fd);
}

// ---------------------------------------------------------------------------------------------------------------------
// parse_inherited_fds
// ---------------------------------------------------------------------------------------------------------------------

TEST(HandoffSpawnTest, ParseValidFds) {
  std::string err;
  auto fds = handoff::parse_inherited_fds("https:3,http:4,webui:5,rtsp:6", "7", &err);
  ASSERT_TRUE(fds.has_value()) << "valid input rejected: " << err;
  EXPECT_EQ(3, fds->https);
  EXPECT_EQ(4, fds->http);
  EXPECT_EQ(5, fds->webui);
  EXPECT_EQ(6, fds->rtsp);
  EXPECT_EQ(7, fds->channel);
}

TEST(HandoffSpawnTest, ParseFdsInAnyOrder) {
  std::string err;
  auto fds = handoff::parse_inherited_fds("rtsp:10,webui:9,https:8,http:3", "4", &err);
  ASSERT_TRUE(fds.has_value()) << "order of names must not matter: " << err;
  EXPECT_EQ(8, fds->https);
  EXPECT_EQ(3, fds->http);
  EXPECT_EQ(9, fds->webui);
  EXPECT_EQ(10, fds->rtsp);
  EXPECT_EQ(4, fds->channel);
}

TEST(HandoffSpawnTest, ParseAcceptsNullError) {
  EXPECT_TRUE(handoff::parse_inherited_fds("https:3,http:4,webui:5,rtsp:6", "7", nullptr).has_value());
  EXPECT_FALSE(handoff::parse_inherited_fds("junk", "7", nullptr).has_value()) << "a null error pointer must be fine";
}

namespace {
  void expect_parse_fails(const std::string &fds, const std::string &channel, const std::string &what) {
    std::string err;
    auto r = handoff::parse_inherited_fds(fds, channel, &err);
    EXPECT_FALSE(r.has_value()) << "must be rejected: " << what << " (fds='" << fds << "', channel='" << channel << "')";
    EXPECT_FALSE(err.empty()) << "a rejected input must give a reason: " << what;
  }
}  // namespace

TEST(HandoffSpawnTest, ParseRejectsMissingName) {
  expect_parse_fails("https:3,http:4,webui:5", "7", "rtsp missing");
  expect_parse_fails("http:4,webui:5,rtsp:6", "7", "https missing");
  expect_parse_fails("", "7", "empty list");
}

TEST(HandoffSpawnTest, ParseRejectsDuplicateName) {
  expect_parse_fails("https:3,https:8,http:4,webui:5,rtsp:6", "7", "https twice");
}

TEST(HandoffSpawnTest, ParseRejectsUnknownName) {
  expect_parse_fails("https:3,http:4,webui:5,rtsp:6,extra:8", "7", "unknown name added");
  expect_parse_fails("https:3,http:4,webui:5,quic:6", "7", "unknown name instead of rtsp");
}

TEST(HandoffSpawnTest, ParseRejectsFdBelowThree) {
  expect_parse_fails("https:2,http:4,webui:5,rtsp:6", "7", "https at 2");
  expect_parse_fails("https:0,http:4,webui:5,rtsp:6", "7", "https at 0");
  expect_parse_fails("https:3,http:4,webui:5,rtsp:-1", "7", "negative fd");
}

TEST(HandoffSpawnTest, ParseRejectsNonNumericFd) {
  expect_parse_fails("https:x,http:4,webui:5,rtsp:6", "7", "letters");
  expect_parse_fails("https:3a,http:4,webui:5,rtsp:6", "7", "trailing letters");
  expect_parse_fails("https:,http:4,webui:5,rtsp:6", "7", "empty number");
  expect_parse_fails("https:3,http:4,webui:5,rtsp:6.5", "7", "decimal point");
}

TEST(HandoffSpawnTest, ParseRejectsDuplicateFd) {
  expect_parse_fails("https:3,http:3,webui:5,rtsp:6", "7", "two names on one fd");
}

TEST(HandoffSpawnTest, ParseRejectsMalformedEntries) {
  expect_parse_fails("https3,http:4,webui:5,rtsp:6", "7", "no colon");
  expect_parse_fails("https:3,,http:4,webui:5,rtsp:6", "7", "empty entry");
}

TEST(HandoffSpawnTest, ParseRejectsBadChannel) {
  expect_parse_fails("https:3,http:4,webui:5,rtsp:6", "abc", "non-numeric channel");
  expect_parse_fails("https:3,http:4,webui:5,rtsp:6", "", "empty channel");
  expect_parse_fails("https:3,http:4,webui:5,rtsp:6", "2", "channel below 3");
  expect_parse_fails("https:3,http:4,webui:5,rtsp:6", "-1", "negative channel");
  expect_parse_fails("https:3,http:4,webui:5,rtsp:6", "7x", "channel with trailing text");
}

TEST(HandoffSpawnTest, ParseRejectsChannelEqualToAListener) {
  expect_parse_fails("https:3,http:4,webui:5,rtsp:6", "5", "channel repeats a listener fd");
}

// ---------------------------------------------------------------------------------------------------------------------
// validate_listener
// ---------------------------------------------------------------------------------------------------------------------

namespace {
  int port_of(int fd) {
    sockaddr_storage ss {};
    socklen_t len = sizeof(ss);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&ss), &len) != 0) {
      return -1;
    }
    if (ss.ss_family == AF_INET6) {
      return ntohs(reinterpret_cast<sockaddr_in6 *>(&ss)->sin6_port);
    }
    return ntohs(reinterpret_cast<sockaddr_in *>(&ss)->sin_port);
  }
}  // namespace

TEST(HandoffSpawnTest, ValidateListenerAcceptsListeningIpv4Socket) {
  fd_t s(sources_t::listener());
  ASSERT_GE(s.fd, 0);
  int port = port_of(s.fd);
  ASSERT_GT(port, 0);
  EXPECT_EQ("", handoff::validate_listener(s.fd, port, false)) << "a listening IPv4 TCP socket on the right port is valid";
}

TEST(HandoffSpawnTest, ValidateListenerRejectsWrongPort) {
  fd_t s(sources_t::listener());
  ASSERT_GE(s.fd, 0);
  int port = port_of(s.fd);
  EXPECT_NE("", handoff::validate_listener(s.fd, port == 65535 ? 1025 : port + 1, false)) << "wrong port";
}

TEST(HandoffSpawnTest, ValidateListenerRejectsFamilyMismatch) {
  fd_t s(sources_t::listener());
  ASSERT_GE(s.fd, 0);
  EXPECT_NE("", handoff::validate_listener(s.fd, port_of(s.fd), true)) << "IPv6 expected, IPv4 socket";
}

TEST(HandoffSpawnTest, ValidateListenerAcceptsIpv6AndRejectsItForIpv4) {
  fd_t s(::socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (s.fd < 0) {
    GTEST_SKIP() << "IPv6 not available";
  }
  int dual = 0;
  ::setsockopt(s.fd, IPPROTO_IPV6, IPV6_V6ONLY, &dual, sizeof(dual));
  sockaddr_in6 addr {};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_any;
  if (::bind(s.fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(s.fd, 4) != 0) {
    GTEST_SKIP() << "cannot listen on the IPv6 wildcard address";
  }
  int port = port_of(s.fd);
  EXPECT_EQ("", handoff::validate_listener(s.fd, port, true)) << "a listening IPv6 socket is valid for ipv6=true";
  EXPECT_NE("", handoff::validate_listener(s.fd, port, false)) << "IPv4 expected, IPv6 socket";
}

TEST(HandoffSpawnTest, ValidateListenerIpv6RejectsV6OnlyAndAcceptsDualStack) {
  for (int v6only : {1, 0}) {
    fd_t s(::socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (s.fd < 0) {
      GTEST_SKIP() << "IPv6 not available";
    }
    ASSERT_EQ(0, ::setsockopt(s.fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)));
    sockaddr_in6 addr {};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    if (::bind(s.fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(s.fd, 4) != 0) {
      GTEST_SKIP() << "cannot listen on the IPv6 wildcard address";
    }
    auto reason = handoff::validate_listener(s.fd, port_of(s.fd), true);
    if (v6only) {
      EXPECT_NE("", reason) << "ipv6=true (address_family = both) needs a dual-stack socket; IPV6_V6ONLY=1 is invalid";
    } else {
      EXPECT_EQ("", reason) << "a dual-stack IPv6 listener is valid for ipv6=true";
    }
  }
}

TEST(HandoffSpawnTest, ValidateListenerRejectsNonListeningTcpSocket) {
  fd_t s(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  ASSERT_GE(s.fd, 0);
  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(0, ::bind(s.fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)));
  EXPECT_NE("", handoff::validate_listener(s.fd, port_of(s.fd), false)) << "bound but not listening (SO_ACCEPTCONN)";
}

TEST(HandoffSpawnTest, ValidateListenerRejectsUdpSocket) {
  fd_t s(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
  ASSERT_GE(s.fd, 0);
  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(0, ::bind(s.fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)));
  EXPECT_NE("", handoff::validate_listener(s.fd, port_of(s.fd), false)) << "UDP is not TCP";
}

TEST(HandoffSpawnTest, ValidateListenerRejectsUnixListener) {
  fd_t s(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
  ASSERT_GE(s.fd, 0);
  temp_dir_t dir;
  ASSERT_FALSE(dir.path.empty());
  sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  auto path = (dir.path / "s").string();
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  ASSERT_EQ(0, ::bind(s.fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)));
  ASSERT_EQ(0, ::listen(s.fd, 1));
  EXPECT_NE("", handoff::validate_listener(s.fd, 80, false)) << "a Unix socket is not a TCP listener";
}

TEST(HandoffSpawnTest, ValidateListenerRejectsPipe) {
  int p[2];
  ASSERT_EQ(0, ::pipe(p));
  fd_t r(p[0]);
  fd_t w(p[1]);
  EXPECT_NE("", handoff::validate_listener(r.fd, 80, false)) << "a pipe is not a socket";
}

TEST(HandoffSpawnTest, ValidateListenerRejectsInvalidFd) {
  EXPECT_NE("", handoff::validate_listener(-1, 80, false));
  fd_t s(::open("/dev/null", O_RDONLY | O_CLOEXEC));
  int closed = s.fd;
  s.reset();
  EXPECT_NE("", handoff::validate_listener(closed, 80, false)) << "closed fd";
}

// ---------------------------------------------------------------------------------------------------------------------
// spawn_child
// ---------------------------------------------------------------------------------------------------------------------

TEST(HandoffSpawnTest, SpawnedChildHasExactlyTheStandardAndInheritedFds) {
  sources_t src;
  ASSERT_TRUE(src.init());
  temp_dir_t dir;
  ASSERT_FALSE(dir.path.empty());
  // A non-close-on-exec fd that must not leak into the child.
  fd_t leaked(::open("/dev/null", O_RDONLY));
  ASSERT_GE(leaked.fd, 0);
  auto out = dir.path / "out";

  auto req = shell_request(src, R"(ls /proc/$$/fd > "$1.fds")", out);
  std::string err;
  EXPECT_EQ(0, run_shell(req, &err)) << "spawn or run failed: " << err;

  std::set<int> fds;
  for (const auto &l : lines_of(read_file(dir.path / "out.fds"))) {
    fds.insert(std::atoi(l.c_str()));
  }
  for (int i = 3; i <= 7; ++i) {
    EXPECT_TRUE(fds.count(i)) << "fd " << i << " is missing in the child";
  }
  for (int i = 0; i <= 2; ++i) {
    if (::fcntl(i, F_GETFD) >= 0) {
      EXPECT_TRUE(fds.count(i)) << "standard fd " << i << " must be kept";
    }
  }
  for (int fd : fds) {
    EXPECT_TRUE(fd <= 7) << "fd " << fd << " leaked into the child (every other fd above 2 must be closed)";
  }
}

TEST(HandoffSpawnTest, InheritedFdsAreTheRightSockets) {
  sources_t src;
  ASSERT_TRUE(src.init());
  temp_dir_t dir;
  ASSERT_FALSE(dir.path.empty());
  auto out = dir.path / "out";

  auto req = shell_request(src, R"(for i in 3 4 5 6 7; do readlink /proc/$$/fd/$i; done > "$1.links")", out);
  std::string err;
  EXPECT_EQ(0, run_shell(req, &err)) << err;

  auto links = lines_of(read_file(dir.path / "out.links"));
  ASSERT_EQ(5u, links.size());
  EXPECT_EQ(link_of(src.https.fd), links[0]) << "fd 3 must be the https listener";
  EXPECT_EQ(link_of(src.http.fd), links[1]) << "fd 4 must be the http listener";
  EXPECT_EQ(link_of(src.webui.fd), links[2]) << "fd 5 must be the web UI listener";
  EXPECT_EQ(link_of(src.rtsp.fd), links[3]) << "fd 6 must be the rtsp listener";
  EXPECT_EQ(link_of(src.channel.fd), links[4]) << "fd 7 must be the channel";
}

TEST(HandoffSpawnTest, ParentFdsSurviveTheSpawn) {
  sources_t src;
  ASSERT_TRUE(src.init());
  temp_dir_t dir;
  ASSERT_FALSE(dir.path.empty());
  auto out = dir.path / "out";
  auto req = shell_request(src, "true", out);
  EXPECT_EQ(0, run_shell(req));
  EXPECT_GE(::fcntl(src.https.fd, F_GETFD), 0) << "the parent's own fds stay open";
  EXPECT_GE(::fcntl(src.channel.fd, F_GETFD), 0) << "the parent's own fds stay open";
}

TEST(HandoffSpawnTest, SourcesAlreadyAtTargetFdsLandCorrectly) {
  // Runs in a forked child that first clears fds 3-7 and puts the sources there in a permuted order, so a naive
  // placement would clobber one with another. The child writes the sockets it set up to "before" and the spawned shell
  // writes what it found on fds 3-7 to "out.links"; the parent compares.
  temp_dir_t dir;
  ASSERT_FALSE(dir.path.empty());
  auto before_path = (dir.path / "before").string();
  auto out = dir.path / "out";
  fflush(nullptr);
  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    sources_t src;
    if (!src.init()) {
      _exit(1);
    }
    std::string links_before[5] = {
      link_of(src.https.fd),
      link_of(src.http.fd),
      link_of(src.webui.fd),
      link_of(src.rtsp.fd),
      link_of(src.channel.fd),
    };
    int hi[5];
    int *orig[5] = {&src.https.fd, &src.http.fd, &src.webui.fd, &src.rtsp.fd, &src.channel.fd};
    for (int i = 0; i < 5; ++i) {
      hi[i] = ::fcntl(*orig[i], F_DUPFD_CLOEXEC, 100);
      if (hi[i] < 100) {
        _exit(2);
      }
    }
    src.https.reset();
    src.http.reset();
    src.webui.reset();
    src.rtsp.reset();
    src.channel.reset();
    src.channel_peer.reset();
    // https->4, http->3, webui->6, rtsp->5, channel->7 (dup2 replaces whatever sits on those fds).
    const int target[5] = {4, 3, 6, 5, 7};
    for (int i = 0; i < 5; ++i) {
      if (::dup2(hi[i], target[i]) != target[i]) {
        _exit(3);
      }
      ::close(hi[i]);
    }
    src.https.fd = 4;
    src.http.fd = 3;
    src.webui.fd = 6;
    src.rtsp.fd = 5;
    src.channel.fd = 7;
    {
      std::ofstream o(before_path);
      for (const auto &l : links_before) {
        o << l << "\n";
      }
    }
    auto req = shell_request(src, R"(for i in 3 4 5 6 7; do readlink /proc/$$/fd/$i; done > "$1.links")", out);
    std::string err;
    auto child = handoff::spawn_child(req, &err);
    if (!child) {
      _exit(4);
    }
    if (!wait_pidfd(child->pidfd, 5000)) {
      cleanup_child(*child);
      _exit(5);
    }
    int st = reap(*child);
    _exit(st == 0 ? 0 : 6);
  }
  int ws = 0;
  ASSERT_EQ(pid, ::waitpid(pid, &ws, 0));
  ASSERT_TRUE(WIFEXITED(ws)) << "the helper process crashed";
  ASSERT_EQ(0, WEXITSTATUS(ws)) << "helper failed with code " << WEXITSTATUS(ws)
                                << " (1 sockets, 2 dup, 3 dup2, 4 spawn_child failed, 5 child timeout, 6 child failed)";

  auto before = lines_of(read_file(before_path));
  auto links = lines_of(read_file(dir.path / "out.links"));
  ASSERT_EQ(5u, before.size());
  ASSERT_EQ(5u, links.size());
  EXPECT_EQ(before[0], links[0]) << "fd 3 must be the https listener even if its source sat at fd 4";
  EXPECT_EQ(before[1], links[1]) << "fd 4 must be the http listener even if its source sat at fd 3";
  EXPECT_EQ(before[2], links[2]) << "fd 5 must be the web UI listener even if its source sat at fd 6";
  EXPECT_EQ(before[3], links[3]) << "fd 6 must be the rtsp listener even if its source sat at fd 5";
  EXPECT_EQ(before[4], links[4]) << "fd 7 must be the channel";
}

TEST(HandoffSpawnTest, ExtraEnvIsAddedAndReplacesParentValues) {
  sources_t src;
  ASSERT_TRUE(src.init());
  temp_dir_t dir;
  ASSERT_FALSE(dir.path.empty());
  ::setenv("HANDOFF_TEST_PARENT_ONLY", "kept", 1);
  ::setenv("HANDOFF_TEST_REPLACED", "old", 1);
  auto out = dir.path / "out";
  auto req = shell_request(src, R"(env > "$1.env")", out);
  req.extra_env = handoff::child_environment(4242, 3);
  req.extra_env.emplace_back("HANDOFF_TEST_REPLACED=new");
  req.extra_env.emplace_back("HANDOFF_TEST_EXTRA=value");
  int status = run_shell(req);
  ::unsetenv("HANDOFF_TEST_PARENT_ONLY");
  ::unsetenv("HANDOFF_TEST_REPLACED");
  EXPECT_EQ(0, status);

  auto env = lines_of(read_file(dir.path / "out.env"));
  auto count = [&](const std::string &line) {
    return std::count(env.begin(), env.end(), line);
  };
  EXPECT_EQ(1, count("SUNSHINE_HANDOFF_FDS=https:3,http:4,webui:5,rtsp:6"));
  EXPECT_EQ(1, count("SUNSHINE_HANDOFF_CHANNEL=7"));
  EXPECT_EQ(1, count("SUNSHINE_HANDOFF_UDP_BASE=4242"));
  EXPECT_EQ(1, count("SUNSHINE_HANDOFF_GENERATION=3"));
  EXPECT_EQ(1, count("HANDOFF_TEST_EXTRA=value")) << "extra_env entries must reach the child";
  EXPECT_EQ(1, count("HANDOFF_TEST_REPLACED=new")) << "extra_env must replace a parent variable of the same name";
  EXPECT_EQ(0, count("HANDOFF_TEST_REPLACED=old")) << "the old value must be gone";
  EXPECT_EQ(1, count("HANDOFF_TEST_PARENT_ONLY=kept")) << "the parent's environment is inherited";
}

namespace {
  /**
   * @brief Parse a hex field like `SigBlk:\t0000000000000000` from the grep output.
   */
  std::optional<std::uint64_t> sig_field(const std::string &text, const std::string &name) {
    for (const auto &l : lines_of(text)) {
      if (l.rfind(name + ":", 0) == 0) {
        return std::strtoull(l.c_str() + name.size() + 1, nullptr, 16);
      }
    }
    return std::nullopt;
  }
}  // namespace

TEST(HandoffSpawnTest, ChildHasEmptySignalMaskEvenWhenSpawnerBlocks) {
  sources_t src;
  ASSERT_TRUE(src.init());
  temp_dir_t dir;
  ASSERT_FALSE(dir.path.empty());
  auto out = dir.path / "out";

  sigset_t block, old;
  sigemptyset(&block);
  sigaddset(&block, SIGUSR1);
  ASSERT_EQ(0, pthread_sigmask(SIG_BLOCK, &block, &old));
  auto req = shell_request(src, R"(grep -E '^(SigBlk|SigIgn)' /proc/self/status > "$1.sig")", out);
  int status = run_shell(req);
  pthread_sigmask(SIG_SETMASK, &old, nullptr);
  EXPECT_EQ(0, status);

  auto text = read_file(dir.path / "out.sig");
  auto blk = sig_field(text, "SigBlk");
  ASSERT_TRUE(blk.has_value()) << "could not read SigBlk from the child: " << text;
  EXPECT_EQ(0u, *blk) << "the child must start with an empty signal mask";
}

TEST(HandoffSpawnTest, ChildGetsDefaultSignalDispositions) {
  sources_t src;
  ASSERT_TRUE(src.init());
  temp_dir_t dir;
  ASSERT_FALSE(dir.path.empty());
  auto out = dir.path / "out";

  auto prev = std::signal(SIGUSR2, SIG_IGN);
  auto req = shell_request(src, R"(grep -E '^(SigBlk|SigIgn)' /proc/self/status > "$1.sig")", out);
  int status = run_shell(req);
  std::signal(SIGUSR2, prev);
  EXPECT_EQ(0, status);

  auto text = read_file(dir.path / "out.sig");
  auto ign = sig_field(text, "SigIgn");
  ASSERT_TRUE(ign.has_value()) << text;
  EXPECT_EQ(0u, *ign & (1ull << (SIGUSR2 - 1))) << "an ignored signal in the parent must be default in the child";
}

TEST(HandoffSpawnTest, SpawnReturnsAPidAndAPidfd) {
  sources_t src;
  ASSERT_TRUE(src.init());
  handoff::spawn_request_t req;
  req.binary = "/bin/sleep";
  req.argv = {"sleep", "30"};
  req.fds = src.fds();
  std::string err;
  auto child = handoff::spawn_child(req, &err);
  ASSERT_TRUE(child.has_value()) << err;
  EXPECT_GT(child->pid, 0);
  ASSERT_GE(child->pidfd, 0);
  int flags = ::fcntl(child->pidfd, F_GETFD);
  EXPECT_TRUE(flags >= 0 && (flags & FD_CLOEXEC)) << "the parent's pidfd should be close-on-exec";
  EXPECT_FALSE(handoff::pidfd_exited(child->pidfd));
  cleanup_child(*child);
}

TEST(HandoffSpawnTest, SpawnOfMissingBinaryFailsWithReason) {
  sources_t src;
  ASSERT_TRUE(src.init());
  handoff::spawn_request_t req;
  req.binary = "/nonexistent/handoff-test-binary";
  req.argv = {"handoff-test-binary"};
  req.fds = src.fds();
  std::string err;
  auto child = handoff::spawn_child(req, &err);
  if (child) {
    cleanup_child(*child);
  }
  EXPECT_FALSE(child.has_value()) << "a missing binary must fail the spawn";
  EXPECT_FALSE(err.empty()) << "a failed spawn must give a reason";
}

TEST(HandoffSpawnTest, SpawnWithExtraEnvWithoutEqualsFailsWithReason) {
  sources_t src;
  ASSERT_TRUE(src.init());
  handoff::spawn_request_t req;
  req.binary = "/bin/true";
  req.argv = {"true"};
  req.fds = src.fds();
  req.extra_env = {"NOEQUALSSIGN"};
  std::string err;
  auto child = handoff::spawn_child(req, &err);
  if (child) {
    cleanup_child(*child);
  }
  EXPECT_FALSE(child.has_value()) << "an extra_env entry without '=' must fail the spawn";
  EXPECT_FALSE(err.empty()) << "a failed spawn must give a reason";
}

TEST(HandoffSpawnTest, SpawnAcceptsNullError) {
  sources_t src;
  ASSERT_TRUE(src.init());
  handoff::spawn_request_t req;
  req.binary = "/nonexistent/handoff-test-binary";
  req.argv = {"x"};
  req.fds = src.fds();
  auto child = handoff::spawn_child(req, nullptr);
  if (child) {
    cleanup_child(*child);
  }
  EXPECT_FALSE(child.has_value());
}

// ---------------------------------------------------------------------------------------------------------------------
// kill_and_reap, pidfd_exited, open_self_pidfd
// ---------------------------------------------------------------------------------------------------------------------

TEST(HandoffSpawnTest, KillAndReapKillsARunningChild) {
  sources_t src;
  ASSERT_TRUE(src.init());
  handoff::spawn_request_t req;
  req.binary = "/bin/sleep";
  req.argv = {"sleep", "30"};
  req.fds = src.fds();
  std::string err;
  auto child = handoff::spawn_child(req, &err);
  ASSERT_TRUE(child.has_value()) << err;

  int status = -1;
  auto start = clock_type::now();
  auto result = handoff::kill_and_reap(child->pidfd, 2000ms, &status);
  auto elapsed = clock_type::now() - start;
  EXPECT_EQ(handoff::reap_result_e::reaped, result);
  EXPECT_LT(elapsed, 1500ms) << "SIGKILL must end the child at once";
  EXPECT_EQ(SIGKILL, status) << "the exit status of a killed child is SIGKILL";
  EXPECT_TRUE(handoff::pidfd_exited(child->pidfd));
  ::close(child->pidfd);
}

TEST(HandoffSpawnTest, KillAndReapAcceptsNullStatus) {
  sources_t src;
  ASSERT_TRUE(src.init());
  handoff::spawn_request_t req;
  req.binary = "/bin/sleep";
  req.argv = {"sleep", "30"};
  req.fds = src.fds();
  auto child = handoff::spawn_child(req, nullptr);
  ASSERT_TRUE(child.has_value());
  EXPECT_EQ(handoff::reap_result_e::reaped, handoff::kill_and_reap(child->pidfd, 2000ms, nullptr));
  ::close(child->pidfd);
}

TEST(HandoffSpawnTest, KillAndReapOfAlreadyExitedChild) {
  sources_t src;
  ASSERT_TRUE(src.init());
  handoff::spawn_request_t req;
  req.binary = "/bin/true";
  req.argv = {"true"};
  req.fds = src.fds();
  auto child = handoff::spawn_child(req, nullptr);
  ASSERT_TRUE(child.has_value());
  ASSERT_TRUE(wait_pidfd(child->pidfd, 3000)) << "/bin/true did not exit";

  int status = -1;
  EXPECT_EQ(handoff::reap_result_e::reaped, handoff::kill_and_reap(child->pidfd, 1000ms, &status))
    << "a zombie that nobody reaped yet must be reaped";
  EXPECT_EQ(0, status) << "/bin/true exited with 0";
  ::close(child->pidfd);
}

TEST(HandoffSpawnTest, KillAndReapWhenAnotherWaitReapedTheChild) {
  sources_t src;
  ASSERT_TRUE(src.init());
  handoff::spawn_request_t req;
  req.binary = "/bin/true";
  req.argv = {"true"};
  req.fds = src.fds();
  auto child = handoff::spawn_child(req, nullptr);
  ASSERT_TRUE(child.has_value());
  ASSERT_TRUE(wait_pidfd(child->pidfd, 3000));
  int ws = 0;
  ASSERT_EQ(child->pid, ::waitpid(child->pid, &ws, 0)) << "the test's own reap failed";

  int status = 12345;
  EXPECT_EQ(handoff::reap_result_e::reaped_elsewhere, handoff::kill_and_reap(child->pidfd, 1000ms, &status))
    << "ECHILD (reaped by another wait) must be reported as reaped_elsewhere";
  ::close(child->pidfd);
}

TEST(HandoffSpawnTest, KillAndReapOfInvalidPidfdIsAnError) {
  int status = 0;
  EXPECT_EQ(handoff::reap_result_e::error, handoff::kill_and_reap(-1, 100ms, &status));
  fd_t s(::open("/dev/null", O_RDONLY | O_CLOEXEC));
  int closed = s.fd;
  s.reset();
  EXPECT_EQ(handoff::reap_result_e::error, handoff::kill_and_reap(closed, 100ms, &status)) << "a closed fd";
}

TEST(HandoffSpawnTest, KillAndReapOfNonPidfdIsAnError) {
  int p[2];
  ASSERT_EQ(0, ::pipe(p));
  fd_t r(p[0]);
  fd_t w(p[1]);
  EXPECT_EQ(handoff::reap_result_e::error, handoff::kill_and_reap(r.fd, 100ms, nullptr)) << "a pipe is not a pidfd";
}

TEST(HandoffSpawnTest, PidfdExitedBeforeAndAfter) {
  sources_t src;
  ASSERT_TRUE(src.init());
  handoff::spawn_request_t req;
  req.binary = "/bin/sleep";
  req.argv = {"sleep", "30"};
  req.fds = src.fds();
  auto child = handoff::spawn_child(req, nullptr);
  ASSERT_TRUE(child.has_value());
  EXPECT_FALSE(handoff::pidfd_exited(child->pidfd)) << "a running process has not exited";
  ASSERT_EQ(0, ::kill(child->pid, SIGKILL));
  bool exited = false;
  auto deadline = clock_type::now() + 2s;
  while (clock_type::now() < deadline && !(exited = handoff::pidfd_exited(child->pidfd))) {
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_TRUE(exited) << "a killed process (a zombie) counts as exited";
  // Still a zombie until reaped: it keeps counting as exited.
  EXPECT_TRUE(handoff::pidfd_exited(child->pidfd));
  reap(*child);
}

TEST(HandoffSpawnTest, PidfdExitedForInvalidPidfdIsTrue) {
  EXPECT_TRUE(handoff::pidfd_exited(-1)) << "an invalid pidfd counts as exited";
}

TEST(HandoffSpawnTest, OpenSelfPidfdIsValidAndCloseOnExec) {
  int fd = handoff::open_self_pidfd();
  ASSERT_GE(fd, 0);
  fd_t guard(fd);
  int flags = ::fcntl(fd, F_GETFD);
  ASSERT_GE(flags, 0);
  EXPECT_TRUE(flags & FD_CLOEXEC) << "the self pidfd must be close-on-exec";
  EXPECT_FALSE(handoff::pidfd_exited(fd)) << "this process is running";
}

// ---------------------------------------------------------------------------------------------------------------------
// scrub_inherited
// ---------------------------------------------------------------------------------------------------------------------

TEST(HandoffSpawnTest, ScrubInheritedSetsCloexecAndRemovesHandoffVariables) {
  fflush(nullptr);
  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // Child: exit codes name the first failing check; 0 is success.
    int null = ::open("/dev/null", O_RDONLY);
    if (null < 0) {
      _exit(90);
    }
    for (int i = 3; i <= 7; ++i) {
      if (::dup2(null, i) != i) {
        _exit(91);
      }
    }
    for (int i = 3; i <= 7; ++i) {
      if (::fcntl(i, F_GETFD) & FD_CLOEXEC) {
        _exit(92);
      }
    }
    ::setenv("SUNSHINE_HANDOFF_X", "1", 1);
    ::setenv("SUNSHINE_HANDOFF_Y", "2", 1);
    ::setenv("SUNSHINE_HANDOFF_FDS", "https:3", 1);
    ::setenv("SUNSHINE_OTHER", "keep", 1);
    ::setenv("HANDOFF_TEST_KEEP", "keep", 1);
    handoff::scrub_inherited();
    for (int i = 3; i <= 7; ++i) {
      int f = ::fcntl(i, F_GETFD);
      if (f < 0) {
        _exit(10 + i);
      }
      if (!(f & FD_CLOEXEC)) {
        _exit(20 + i);
      }
    }
    if (::getenv("SUNSHINE_HANDOFF_X") || ::getenv("SUNSHINE_HANDOFF_Y") || ::getenv("SUNSHINE_HANDOFF_FDS")) {
      _exit(30);
    }
    for (char **e = environ; e && *e; ++e) {
      if (std::strncmp(*e, "SUNSHINE_HANDOFF_", 17) == 0) {
        _exit(31);
      }
    }
    if (!::getenv("SUNSHINE_OTHER") || !::getenv("HANDOFF_TEST_KEEP")) {
      _exit(32);
    }
    _exit(0);
  }
  int ws = 0;
  ASSERT_EQ(pid, ::waitpid(pid, &ws, 0));
  ASSERT_TRUE(WIFEXITED(ws)) << "the checking child crashed";
  int code = WEXITSTATUS(ws);
  EXPECT_NE(90, code) << "test setup: /dev/null";
  EXPECT_NE(91, code) << "test setup: dup2";
  EXPECT_NE(92, code) << "test setup: dup2'd fds should not be close-on-exec";
  EXPECT_FALSE(code >= 10 && code < 20) << "an fd 3-7 was closed by scrub_inherited (code " << code << ")";
  EXPECT_FALSE(code >= 20 && code < 30) << "FD_CLOEXEC not set on fd " << (code - 20);
  EXPECT_NE(30, code) << "a SUNSHINE_HANDOFF_* variable is still set";
  EXPECT_NE(31, code) << "a SUNSHINE_HANDOFF_* variable is still in environ";
  EXPECT_NE(32, code) << "variables that do not start with SUNSHINE_HANDOFF_ must stay";
  EXPECT_EQ(0, code);
}

#endif  // __linux__
