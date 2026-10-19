/**
 * @file tests/unit/handoff/test_control.cpp
 * @brief Blind tests for the control socket and the `--drain-handoff` helper (src/handoff/control.*), written from the
 *        spec.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <cstdint>
  #include <cstdlib>
  #include <cstring>
  #include <filesystem>
  #include <fstream>
  #include <iterator>
  #include <optional>
  #include <poll.h>
  #include <string>
  #include <sys/socket.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <sys/un.h>
  #include <thread>
  #include <unistd.h>
  #include <vector>

  // local includes
  #include "src/handoff/control.h"
  #include "src/handoff/instance_file.h"

using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace hf = handoff;

namespace {

  /**
   * @brief A temporary directory removed on destruction.
   */
  struct temp_dir_t {
    fs::path path;  ///< Directory path.

    temp_dir_t() {
      std::string templ = (fs::temp_directory_path() / "hf_ctl_XXXXXX").string();
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
   * @brief Build a sockaddr_un for a filesystem path.
   */
  sockaddr_un make_addr(const fs::path &path) {
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::string p = path.string();
    std::strncpy(addr.sun_path, p.c_str(), sizeof(addr.sun_path) - 1);
    return addr;
  }

  /**
   * @brief Connect to a Unix socket, send `data`, and read one line (bounded by 2 s).
   *
   * @return The bytes read up to and including the first newline, or nullopt when the connection failed.
   */
  std::optional<std::string> talk(const fs::path &path, const std::string &data) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return std::nullopt;
    }
    sockaddr_un addr = make_addr(path);
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      close(fd);
      return std::nullopt;
    }
    send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    std::string out;
    while (out.find('\n') == std::string::npos) {
      pollfd pfd {fd, POLLIN, 0};
      if (poll(&pfd, 1, 2000) <= 0) {
        break;
      }
      char buf[256];
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n <= 0) {
        break;
      }
      out.append(buf, static_cast<std::size_t>(n));
    }
    close(fd);
    return out;
  }

  /**
   * @brief A raw one-shot Unix server for replies the real server cannot produce (garbage, silence).
   *
   * Accepts one connection, reads one line, sends `reply` (when not empty), then waits up to `hold_ms` for the peer
   * to close.
   */
  class raw_server_t {
  public:
    /**
     * @brief Bind and start the serving thread.
     */
    void start(const fs::path &path, std::string reply, int hold_ms) {
      fs::create_directories(path.parent_path());
      _fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      sockaddr_un addr = make_addr(path);
      unlink(path.c_str());
      if (_fd >= 0 && bind(_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 && listen(_fd, 4) == 0) {
        _thread = std::thread([fd = _fd, reply = std::move(reply), hold_ms] {
          pollfd lp {fd, POLLIN, 0};
          if (poll(&lp, 1, 3000) <= 0) {
            return;
          }
          int conn = accept(fd, nullptr, nullptr);
          if (conn < 0) {
            return;
          }
          char ch = 0;
          while (true) {
            pollfd cp {conn, POLLIN, 0};
            if (poll(&cp, 1, 2000) <= 0) {
              break;
            }
            if (read(conn, &ch, 1) <= 0 || ch == '\n') {
              break;
            }
          }
          if (!reply.empty()) {
            send(conn, reply.data(), reply.size(), MSG_NOSIGNAL);
          }
          pollfd hp {conn, POLLIN, 0};
          poll(&hp, 1, hold_ms);
          close(conn);
        });
      }
    }

    /**
     * @brief Join the thread and close the listening socket.
     */
    ~raw_server_t() {
      if (_thread.joinable()) {
        _thread.join();
      }
      if (_fd >= 0) {
        close(_fd);
      }
    }

  private:
    int _fd {-1};  ///< Listening socket.
    std::thread _thread;  ///< Serving thread.
  };

  /**
   * @brief Write an instance file for the given content.
   */
  void write_instance(const fs::path &runtime, const hf::instance_t &inst) {
    fs::path dir = runtime / "instances";
    fs::create_directories(dir);
    std::ofstream out(dir / (std::to_string(inst.pid) + ".json"), std::ios::trunc);
    out << hf::to_json(inst);
  }

  /**
   * @brief Instance with the given fields and the default UDP base.
   */
  hf::instance_t make_instance(pid_t pid, std::uint64_t start, hf::role_e role, hf::state_e state) {
    hf::instance_t inst;
    inst.pid = pid;
    inst.start_time = start;
    inst.role = role;
    inst.udp_base = 47989;
    inst.sessions = 0;
    inst.handoff = state;
    return inst;
  }

  /**
   * @brief Helper environment with short timeouts.
   */
  hf::helper_env_t make_env(const fs::path &runtime) {
    hf::helper_env_t env;
    env.runtime_dir = runtime;
    env.connect_timeout = 1000ms;
    env.reply_timeout = 2000ms;
    env.active_timeout = 300ms;
    return env;
  }

}  // namespace

/**
 * @brief Tests for the control socket protocol, the helper and the control socket server.
 */
class HandoffControlTest: public testing::Test {};

TEST_F(HandoffControlTest, ControlSocketPathIsInControlSubdirectory) {
  EXPECT_EQ(hf::control_socket_path("/run/sunshine", 1234), fs::path("/run/sunshine/control/1234.sock"))
    << "the socket must be <runtime dir>/control/<pid>.sock";
}

TEST_F(HandoffControlTest, HandoffVerbIsHandoff) {
  EXPECT_EQ(hf::handoff_verb, "handoff") << "the only verb must be \"handoff\"";
}

TEST_F(HandoffControlTest, EncodeRefusedLine) {
  EXPECT_EQ(hf::encode_reply({hf::reply_kind_e::refused, "busy", 0, 0}), "refused busy\n")
    << "a refused reply must encode as \"refused <reason>\\n\"";
}

TEST_F(HandoffControlTest, EncodeFailedLine) {
  EXPECT_EQ(hf::encode_reply({hf::reply_kind_e::failed, "boom", 0, 0}), "failed boom\n")
    << "a failed reply must encode as \"failed <reason>\\n\"";
}

TEST_F(HandoffControlTest, EncodeKillPendingLine) {
  EXPECT_EQ(hf::encode_reply({hf::reply_kind_e::kill_pending, "stuck", 0, 0}), "kill_pending stuck\n")
    << "a kill_pending reply must encode as \"kill_pending <reason>\\n\"";
}

TEST_F(HandoffControlTest, EncodeCommittedLine) {
  EXPECT_EQ(hf::encode_reply({hf::reply_kind_e::committed, "", 1234, 5551234}), "committed 1234 5551234\n")
    << "a committed reply must encode as \"committed <pid> <start_time>\\n\"";
}

TEST_F(HandoffControlTest, EncodeReplacesNewlinesInReasonWithSpaces) {
  EXPECT_EQ(hf::encode_reply({hf::reply_kind_e::refused, "first\nsecond", 0, 0}), "refused first second\n")
    << "a newline inside the reason must be replaced by a space so the reply stays one line";
}

TEST_F(HandoffControlTest, ParseRoundTripsEveryForm) {
  std::vector<hf::control_reply_t> replies = {
    {hf::reply_kind_e::refused, "no control socket", 0, 0},
    {hf::reply_kind_e::failed, "new process died", 0, 0},
    {hf::reply_kind_e::kill_pending, "uninterruptible", 0, 0},
    {hf::reply_kind_e::committed, "", 4242, 99887766},
  };
  for (const auto &reply : replies) {
    auto parsed = hf::parse_reply(hf::encode_reply(reply));
    ASSERT_TRUE(parsed.has_value()) << "parse_reply must accept what encode_reply produces, kind "
                                    << static_cast<int>(reply.kind);
    EXPECT_EQ(*parsed, reply) << "encode then parse must give the same reply, kind " << static_cast<int>(reply.kind);
  }
}

TEST_F(HandoffControlTest, ParseAcceptsLineWithoutNewline) {
  auto parsed = hf::parse_reply("failed boom");
  ASSERT_TRUE(parsed.has_value()) << "a reply line without its trailing newline must parse";
  EXPECT_EQ(parsed->kind, hf::reply_kind_e::failed) << "the kind must be failed";
  EXPECT_EQ(parsed->reason, "boom") << "the reason must be the rest of the line";
}

TEST_F(HandoffControlTest, ParseKeepsSpacesInReason) {
  auto parsed = hf::parse_reply("refused a b c\n");
  ASSERT_TRUE(parsed.has_value()) << "a refused line with a multi-word reason must parse";
  EXPECT_EQ(parsed->reason, "a b c") << "the reason must keep its spaces";
}

TEST_F(HandoffControlTest, ParseRejectsUnknownWord) {
  EXPECT_FALSE(hf::parse_reply("maybe yes\n").has_value()) << "an unknown reply word must give nullopt";
  EXPECT_FALSE(hf::parse_reply("\n").has_value()) << "an empty line must give nullopt";
  EXPECT_FALSE(hf::parse_reply("").has_value()) << "an empty string must give nullopt";
}

TEST_F(HandoffControlTest, ParseRejectsCommittedWithBadPid) {
  EXPECT_FALSE(hf::parse_reply("committed abc 5\n").has_value()) << "a non-numeric pid must give nullopt";
  EXPECT_FALSE(hf::parse_reply("committed 0 5\n").has_value()) << "pid 0 is not positive and must give nullopt";
  EXPECT_FALSE(hf::parse_reply("committed -3 5\n").has_value()) << "a negative pid must give nullopt";
}

TEST_F(HandoffControlTest, ParseRejectsCommittedWithBadStartTime) {
  EXPECT_FALSE(hf::parse_reply("committed 12 xyz\n").has_value())
    << "a non-numeric start time must give nullopt";
  EXPECT_FALSE(hf::parse_reply("committed 12\n").has_value())
    << "a committed line without a start time must give nullopt";
}

TEST_F(HandoffControlTest, ExitCodeForEachKind) {
  EXPECT_EQ(hf::exit_code_for(hf::reply_kind_e::refused), hf::helper_exit_e::refused)
    << "refused must map to exit code 1";
  EXPECT_EQ(hf::exit_code_for(hf::reply_kind_e::failed), hf::helper_exit_e::failed)
    << "failed must map to exit code 2";
  EXPECT_EQ(hf::exit_code_for(hf::reply_kind_e::kill_pending), hf::helper_exit_e::kill_pending)
    << "kill_pending must map to exit code 3";
  EXPECT_EQ(hf::exit_code_for(hf::reply_kind_e::committed), hf::helper_exit_e::switched)
    << "committed must map to exit code 0 (the caller then waits for role active)";
}

TEST_F(HandoffControlTest, HelperExitCodeValues) {
  EXPECT_EQ(static_cast<int>(hf::helper_exit_e::switched), 0) << "switched must be exit code 0";
  EXPECT_EQ(static_cast<int>(hf::helper_exit_e::refused), 1) << "refused must be exit code 1";
  EXPECT_EQ(static_cast<int>(hf::helper_exit_e::failed), 2) << "failed must be exit code 2";
  EXPECT_EQ(static_cast<int>(hf::helper_exit_e::kill_pending), 3) << "kill_pending must be exit code 3";
  EXPECT_EQ(static_cast<int>(hf::helper_exit_e::unconfirmed), 4) << "unconfirmed must be exit code 4";
  EXPECT_EQ(static_cast<int>(hf::helper_exit_e::error), 5) << "error must be exit code 5";
}

TEST_F(HandoffControlTest, HelperRejectsWrongArgumentCount) {
  temp_dir_t tmp;
  std::string err;
  EXPECT_EQ(hf::run_helper({}, make_env(tmp.path), err), hf::helper_exit_e::error)
    << "no arguments must be a usage error (exit 5)";
  EXPECT_EQ(hf::run_helper({"4242", "4243"}, make_env(tmp.path), err), hf::helper_exit_e::error)
    << "two pids must be a usage error (exit 5)";
}

TEST_F(HandoffControlTest, HelperRejectsNonPositiveOrNonNumericPid) {
  temp_dir_t tmp;
  std::string err;
  EXPECT_EQ(hf::run_helper({"abc"}, make_env(tmp.path), err), hf::helper_exit_e::error)
    << "a non-numeric pid must be a usage error (exit 5)";
  EXPECT_EQ(hf::run_helper({"0"}, make_env(tmp.path), err), hf::helper_exit_e::error)
    << "pid 0 is not a positive pid and must be a usage error (exit 5)";
  EXPECT_EQ(hf::run_helper({"-5"}, make_env(tmp.path), err), hf::helper_exit_e::error)
    << "a negative pid must be a usage error (exit 5)";
}

TEST_F(HandoffControlTest, HelperRefusesWhileDrainingInstanceFile) {
  temp_dir_t tmp;
  write_instance(tmp.path, make_instance(4242, 1, hf::role_e::draining, hf::state_e::draining));
  std::string err;
  EXPECT_EQ(hf::run_helper({"4242"}, make_env(tmp.path), err), hf::helper_exit_e::refused)
    << "with no socket and role draining the helper must refuse with exit 1";
  EXPECT_NE(err.find("a drain is in progress"), std::string::npos)
    << "the reason for a draining instance must be \"a drain is in progress\", got: " << err;
}

TEST_F(HandoffControlTest, HelperRefusesWhileHandoverFailing) {
  temp_dir_t tmp;
  write_instance(tmp.path, make_instance(4242, 1, hf::role_e::active, hf::state_e::handover_failed));
  std::string err;
  EXPECT_EQ(hf::run_helper({"4242"}, make_env(tmp.path), err), hf::helper_exit_e::refused)
    << "with no socket and state handover_failed the helper must refuse with exit 1";
  EXPECT_NE(err.find("handover failing"), std::string::npos)
    << "the reason for handover_failed must be \"handover failing\", got: " << err;
}

TEST_F(HandoffControlTest, HelperRefusesWhileKillPending) {
  temp_dir_t tmp;
  write_instance(tmp.path, make_instance(4242, 1, hf::role_e::active, hf::state_e::kill_pending));
  std::string err;
  EXPECT_EQ(hf::run_helper({"4242"}, make_env(tmp.path), err), hf::helper_exit_e::refused)
    << "with no socket and state kill_pending the helper must refuse with exit 1";
  EXPECT_NE(err.find("kill pending"), std::string::npos)
    << "the reason for kill_pending must be \"kill pending\", got: " << err;
}

TEST_F(HandoffControlTest, HelperRefusesWithoutSocketOrFile) {
  temp_dir_t tmp;
  std::string err;
  EXPECT_EQ(hf::run_helper({"4242"}, make_env(tmp.path), err), hf::helper_exit_e::refused)
    << "with no socket and no instance file the helper must refuse with exit 1";
  EXPECT_NE(err.find("no control socket"), std::string::npos)
    << "the reason must mention the missing control socket, got: " << err;
}

TEST_F(HandoffControlTest, HelperMapsServerRefusedToExitOne) {
  temp_dir_t tmp;
  hf::control_server_t server;
  std::string error;
  ASSERT_TRUE(server.start(hf::control_socket_path(tmp.path, 4242), [](pid_t) {
    return hf::control_reply_t {hf::reply_kind_e::refused, "nope", 0, 0};
  },
                           &error))
    << "the control server must start for the test, error: " << error;
  std::string err;
  EXPECT_EQ(hf::run_helper({"4242"}, make_env(tmp.path), err), hf::helper_exit_e::refused)
    << "a refused reply from the server must give exit 1";
  EXPECT_NE(err.find("nope"), std::string::npos) << "the refusal reason must be written to err, got: " << err;
}

TEST_F(HandoffControlTest, HelperMapsServerFailedToExitTwo) {
  temp_dir_t tmp;
  hf::control_server_t server;
  std::string error;
  ASSERT_TRUE(server.start(hf::control_socket_path(tmp.path, 4242), [](pid_t) {
    return hf::control_reply_t {hf::reply_kind_e::failed, "rolled back", 0, 0};
  },
                           &error))
    << "the control server must start for the test, error: " << error;
  std::string err;
  EXPECT_EQ(hf::run_helper({"4242"}, make_env(tmp.path), err), hf::helper_exit_e::failed)
    << "a failed reply from the server must give exit 2";
}

TEST_F(HandoffControlTest, HelperMapsServerKillPendingToExitThree) {
  temp_dir_t tmp;
  hf::control_server_t server;
  std::string error;
  ASSERT_TRUE(server.start(hf::control_socket_path(tmp.path, 4242), [](pid_t) {
    return hf::control_reply_t {hf::reply_kind_e::kill_pending, "stuck", 0, 0};
  },
                           &error))
    << "the control server must start for the test, error: " << error;
  std::string err;
  EXPECT_EQ(hf::run_helper({"4242"}, make_env(tmp.path), err), hf::helper_exit_e::kill_pending)
    << "a kill_pending reply from the server must give exit 3";
}

TEST_F(HandoffControlTest, HelperCommittedWithActiveInstanceIsSwitched) {
  temp_dir_t tmp;
  pid_t self = getpid();
  std::uint64_t start = hf::read_start_time(self).value_or(0);
  ASSERT_NE(start, 0U) << "the test process must have a readable start time for this test";

  write_instance(tmp.path, make_instance(self, start, hf::role_e::active, hf::state_e::none));
  hf::control_server_t server;
  std::string error;
  ASSERT_TRUE(server.start(hf::control_socket_path(tmp.path, 4242), [=](pid_t) {
    return hf::control_reply_t {
      hf::reply_kind_e::committed,
      "",
      self,
      start,
    };
  },
                           &error))
    << "the control server must start for the test, error: " << error;

  hf::helper_env_t env = make_env(tmp.path);
  env.active_timeout = 1000ms;
  std::string err;
  EXPECT_EQ(hf::run_helper({"4242"}, env, err), hf::helper_exit_e::switched)
    << "committed with an active instance file that matches pid and start time must give exit 0, err: " << err;
}

TEST_F(HandoffControlTest, HelperCommittedWithoutActiveFileIsUnconfirmed) {
  temp_dir_t tmp;
  pid_t self = getpid();
  std::uint64_t start = hf::read_start_time(self).value_or(0);
  hf::control_server_t server;
  std::string error;
  ASSERT_TRUE(server.start(hf::control_socket_path(tmp.path, 4242), [=](pid_t) {
    return hf::control_reply_t {
      hf::reply_kind_e::committed,
      "",
      self,
      start,
    };
  },
                           &error))
    << "the control server must start for the test, error: " << error;

  auto begin = std::chrono::steady_clock::now();
  std::string err;
  EXPECT_EQ(hf::run_helper({"4242"}, make_env(tmp.path), err), hf::helper_exit_e::unconfirmed)
    << "committed with no instance file showing role active must give exit 4";
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 3s)
    << "the helper must stop waiting after its active timeout";
}

TEST_F(HandoffControlTest, HelperCommittedWithPendingFileIsUnconfirmed) {
  temp_dir_t tmp;
  pid_t self = getpid();
  std::uint64_t start = hf::read_start_time(self).value_or(0);
  write_instance(tmp.path, make_instance(self, start, hf::role_e::pending, hf::state_e::none));
  hf::control_server_t server;
  std::string error;
  ASSERT_TRUE(server.start(hf::control_socket_path(tmp.path, 4242), [=](pid_t) {
    return hf::control_reply_t {
      hf::reply_kind_e::committed,
      "",
      self,
      start,
    };
  },
                           &error))
    << "the control server must start for the test, error: " << error;
  std::string err;
  EXPECT_EQ(hf::run_helper({"4242"}, make_env(tmp.path), err), hf::helper_exit_e::unconfirmed)
    << "committed with the new file still in role pending must give exit 4";
}

TEST_F(HandoffControlTest, HelperCommittedWithStartTimeMismatchIsUnconfirmed) {
  temp_dir_t tmp;
  pid_t self = getpid();
  std::uint64_t start = hf::read_start_time(self).value_or(0);
  write_instance(tmp.path, make_instance(self, start + 1, hf::role_e::active, hf::state_e::none));
  hf::control_server_t server;
  std::string error;
  ASSERT_TRUE(server.start(hf::control_socket_path(tmp.path, 4242), [=](pid_t) {
    return hf::control_reply_t {
      hf::reply_kind_e::committed,
      "",
      self,
      start,
    };
  },
                           &error))
    << "the control server must start for the test, error: " << error;
  std::string err;
  EXPECT_EQ(hf::run_helper({"4242"}, make_env(tmp.path), err), hf::helper_exit_e::unconfirmed)
    << "an active file with another start time is not the committed process and must give exit 4";
}

TEST_F(HandoffControlTest, HelperGarbageReplyIsError) {
  temp_dir_t tmp;
  raw_server_t raw;
  raw.start(hf::control_socket_path(tmp.path, 4242), "maybe a reply\n", 0);
  std::string err;
  EXPECT_EQ(hf::run_helper({"4242"}, make_env(tmp.path), err), hf::helper_exit_e::error)
    << "an unparsable reply line must give exit 5";
}

TEST_F(HandoffControlTest, HelperReplyTimeoutIsError) {
  temp_dir_t tmp;
  raw_server_t raw;
  raw.start(hf::control_socket_path(tmp.path, 4242), "", 800);
  hf::helper_env_t env = make_env(tmp.path);
  env.reply_timeout = 200ms;
  auto begin = std::chrono::steady_clock::now();
  std::string err;
  EXPECT_EQ(hf::run_helper({"4242"}, env, err), hf::helper_exit_e::error)
    << "a server that never answers must give exit 5 when the reply timeout passes";
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 2s) << "the helper must honour its reply timeout";
}

TEST_F(HandoffControlTest, ServerSocketHasMode0600) {
  temp_dir_t tmp;
  hf::control_server_t server;
  std::string error;
  fs::path path = hf::control_socket_path(tmp.path, 4242);
  ASSERT_TRUE(server.start(path, [](pid_t) {
    return hf::control_reply_t {hf::reply_kind_e::refused, "x", 0, 0};
  },
                           &error))
    << "start must succeed, error: " << error;
  ASSERT_TRUE(fs::exists(path)) << "start must create the socket file at the given path";
  EXPECT_EQ(fs::status(path).permissions() & fs::perms::all, fs::perms::owner_read | fs::perms::owner_write)
    << "the socket must have mode 0600";
}

TEST_F(HandoffControlTest, ServerCreatesControlDirectoryWithMode0700) {
  temp_dir_t tmp;
  hf::control_server_t server;
  std::string error;
  fs::path path = hf::control_socket_path(tmp.path, 4242);
  ASSERT_TRUE(server.start(path, [](pid_t) {
    return hf::control_reply_t {hf::reply_kind_e::refused, "x", 0, 0};
  },
                           &error))
    << "start must succeed when the control directory does not exist yet, error: " << error;
  EXPECT_TRUE(fs::is_directory(path.parent_path())) << "start must create the control directory";
  EXPECT_EQ(fs::status(path.parent_path()).permissions() & fs::perms::all, fs::perms::owner_all)
    << "the control directory must have mode 0700";
}

TEST_F(HandoffControlTest, ServerReplacesStaleFileAtPath) {
  temp_dir_t tmp;
  fs::path path = hf::control_socket_path(tmp.path, 4242);
  fs::create_directories(path.parent_path());
  // A stale socket: bound, then closed without unlinking, as a crashed process would leave it.
  {
    int stale = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(stale, 0) << "the test must create a socket for the stale file";
    sockaddr_un addr = make_addr(path);
    ASSERT_EQ(bind(stale, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0)
      << "the test must bind the stale socket at the path";
    close(stale);
  }
  ASSERT_TRUE(fs::exists(path)) << "precondition: the stale socket file is still on disk";
  hf::control_server_t server;
  std::string error;
  ASSERT_TRUE(server.start(path, [](pid_t) {
    return hf::control_reply_t {hf::reply_kind_e::refused, "fresh", 0, 0};
  },
                           &error))
    << "start must replace a stale socket file at the path, error: " << error;
  auto line = talk(path, "handoff\n");
  ASSERT_TRUE(line.has_value()) << "the replaced socket must accept connections";
  EXPECT_EQ(*line, "refused fresh\n") << "the server behind the replaced path must answer the handoff verb";
}

TEST_F(HandoffControlTest, ServerRefusesToReplaceRegularFile) {
  temp_dir_t tmp;
  fs::path path = hf::control_socket_path(tmp.path, 4242);
  fs::create_directories(path.parent_path());
  {
    std::ofstream other(path);
    other << "keep me";
  }
  hf::control_server_t server;
  std::string error;
  bool ok = server.start(path, [](pid_t) {
    return hf::control_reply_t {hf::reply_kind_e::refused, "x", 0, 0};
  },
                         &error);
  EXPECT_FALSE(ok) << "start must fail when a regular file (not a stale socket) sits at the path";
  EXPECT_FALSE(error.empty()) << "a failed start must write a reason to the error string";
  ASSERT_TRUE(fs::is_regular_file(path)) << "the regular file at the path must not be removed";
  std::ifstream in(path);
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(content, "keep me") << "the regular file's content must be left untouched";
}

TEST_F(HandoffControlTest, ServerWritesHandlerReply) {
  temp_dir_t tmp;
  hf::control_server_t server;
  std::string error;
  fs::path path = hf::control_socket_path(tmp.path, 4242);
  ASSERT_TRUE(server.start(path, [](pid_t) {
    return hf::control_reply_t {
      hf::reply_kind_e::committed,
      "",
      1234,
      5,
    };
  },
                           &error))
    << "start must succeed, error: " << error;
  auto line = talk(path, "handoff\n");
  ASSERT_TRUE(line.has_value()) << "the server must answer a handoff line";
  EXPECT_EQ(*line, "committed 1234 5\n") << "the reply written must be the encoded handler reply";
}

TEST_F(HandoffControlTest, ServerRefusesUnknownVerbWithoutCallingHandler) {
  temp_dir_t tmp;
  std::atomic<int> calls {0};
  hf::control_server_t server;
  std::string error;
  fs::path path = hf::control_socket_path(tmp.path, 4242);
  ASSERT_TRUE(server.start(path, [&](pid_t) {
    ++calls;
    return hf::control_reply_t {
      hf::reply_kind_e::committed,
      "",
      1,
      1,
    };
  },
                           &error))
    << "start must succeed, error: " << error;
  auto line = talk(path, "bogus\n");
  ASSERT_TRUE(line.has_value()) << "the server must answer an unknown verb";
  EXPECT_EQ(*line, "refused unknown verb\n") << "any verb other than handoff must get \"refused unknown verb\"";
  EXPECT_EQ(calls.load(), 0) << "the handler must not run for an unknown verb";
}

TEST_F(HandoffControlTest, ServerSurvivesOversizedLine) {
  temp_dir_t tmp;
  std::atomic<int> calls {0};
  hf::control_server_t server;
  std::string error;
  fs::path path = hf::control_socket_path(tmp.path, 4242);
  ASSERT_TRUE(server.start(path, [&](pid_t) {
    ++calls;
    return hf::control_reply_t {hf::reply_kind_e::refused, "after", 0, 0};
  },
                           &error))
    << "start must succeed, error: " << error;
  // The server reads at most 256 bytes of a line; the reply to an oversized line is not specified, so only the
  // handler call count and the server staying up are checked.
  talk(path, std::string(400, 'a') + "\n");
  EXPECT_EQ(calls.load(), 0) << "an oversized line that is not the handoff verb must not run the handler";
  auto line = talk(path, "handoff\n");
  ASSERT_TRUE(line.has_value()) << "the server must still accept a valid handoff after an oversized line";
  EXPECT_EQ(*line, "refused after\n") << "the server must still serve the handoff verb afterwards";
}

TEST_F(HandoffControlTest, ServerPassesPeerPidToHandler) {
  temp_dir_t tmp;
  std::atomic<pid_t> seen {0};
  hf::control_server_t server;
  std::string error;
  fs::path path = hf::control_socket_path(tmp.path, 4242);
  ASSERT_TRUE(server.start(path, [&](pid_t peer) {
    seen = peer;
    return hf::control_reply_t {hf::reply_kind_e::refused, "seen", 0, 0};
  },
                           &error))
    << "start must succeed, error: " << error;
  auto line = talk(path, "handoff\n");
  ASSERT_TRUE(line.has_value()) << "the server must answer a handoff line";
  EXPECT_EQ(seen.load(), getpid()) << "the handler must receive the connecting peer's pid (this test process)";
}

TEST_F(HandoffControlTest, ServerStartFailsWithReasonWhenPathUnusable) {
  temp_dir_t tmp;
  // A regular file where the control directory would be makes the socket path unusable.
  {
    std::ofstream blocker(tmp.path / "blocker");
    blocker << "x";
  }
  hf::control_server_t server;
  std::string error;
  bool ok = server.start(tmp.path / "blocker" / "4242.sock", [](pid_t) {
    return hf::control_reply_t {hf::reply_kind_e::refused, "x", 0, 0};
  },
                         &error);
  EXPECT_FALSE(ok) << "start must fail when the socket path cannot be created";
  EXPECT_FALSE(error.empty()) << "a failed start must write a reason to the error string";
  EXPECT_EQ(error.find('\n'), std::string::npos) << "the failure reason must be one line";
}

TEST_F(HandoffControlTest, ServerStopTwiceAndWithoutStartIsSafe) {
  temp_dir_t tmp;
  hf::control_server_t never_started;
  EXPECT_NO_THROW(never_started.stop()) << "stop() without start() must be safe";

  hf::control_server_t server;
  std::string error;
  ASSERT_TRUE(server.start(hf::control_socket_path(tmp.path, 4242), [](pid_t) {
    return hf::control_reply_t {hf::reply_kind_e::refused, "x", 0, 0};
  },
                           &error))
    << "start must succeed, error: " << error;
  EXPECT_NO_THROW(server.stop()) << "the first stop() must succeed";
  EXPECT_NO_THROW(server.stop()) << "a second stop() must be safe";
}

TEST_F(HandoffControlTest, ServerStopUnlinksSocket) {
  temp_dir_t tmp;
  hf::control_server_t server;
  std::string error;
  fs::path path = hf::control_socket_path(tmp.path, 4242);
  ASSERT_TRUE(server.start(path, [](pid_t) {
    return hf::control_reply_t {hf::reply_kind_e::refused, "x", 0, 0};
  },
                           &error))
    << "start must succeed, error: " << error;
  ASSERT_TRUE(fs::exists(path)) << "precondition: the socket exists while serving";
  server.stop();
  EXPECT_FALSE(fs::exists(path)) << "stop() must unlink the socket file";
}

TEST_F(HandoffControlTest, ServerDestructorStopsAndUnlinks) {
  temp_dir_t tmp;
  fs::path path = hf::control_socket_path(tmp.path, 4242);
  {
    hf::control_server_t server;
    std::string error;
    ASSERT_TRUE(server.start(path, [](pid_t) {
      return hf::control_reply_t {hf::reply_kind_e::refused, "x", 0, 0};
    },
                             &error))
      << "start must succeed, error: " << error;
  }
  EXPECT_FALSE(fs::exists(path)) << "the destructor must stop the server and unlink the socket";
}

#endif  // __linux__
