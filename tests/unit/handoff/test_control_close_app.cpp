/**
 * @file tests/unit/handoff/test_control_close_app.cpp
 * @brief Blind tests for the `close-app` parts of the control socket (src/handoff/control.h): the reply forms, the
 *        socket mode decision, the per-verb server dispatch, the peer uid check and the `--close-app` helper. Written
 *        from docs/drain-handoff.md ("The control socket", "The `close-app` verb and the `control_socket` option") and
 *        the header only.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <atomic>
  #include <chrono>
  #include <cstdint>
  #include <cstdlib>
  #include <cstring>
  #include <filesystem>
  #include <fstream>
  #include <optional>
  #include <poll.h>
  #include <string>
  #include <sys/socket.h>
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
    fs::path path;  ///< Directory path; empty when creation failed.

    /**
     * @brief Create a fresh directory under the system temp directory.
     */
    temp_dir_t() {
      std::string templ = (fs::temp_directory_path() / "hf_cc_XXXXXX").string();
      std::vector<char> buf(templ.begin(), templ.end());
      buf.push_back('\0');
      char *made = mkdtemp(buf.data());
      if (made != nullptr) {
        path = made;
      }
    }

    /**
     * @brief Remove the directory and everything in it.
     */
    ~temp_dir_t() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  };

  /**
   * @brief Restores the test-only peer uid override to "no override" on destruction, also when a test fails.
   */
  struct uid_override_guard_t {
    /**
     * @brief Install the override.
     *
     * @param uid Uid to report as the peer uid, or `std::nullopt`.
     */
    explicit uid_override_guard_t(std::optional<uid_t> uid) {
      hf::set_peer_uid_override_for_tests(uid);
    }

    /**
     * @brief Clear the override.
     */
    ~uid_override_guard_t() {
      hf::set_peer_uid_override_for_tests(std::nullopt);
    }
  };

  /**
   * @brief Build a sockaddr_un for a filesystem path.
   *
   * @param path Socket path.
   * @return Address.
   */
  sockaddr_un make_addr(const fs::path &path) {
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::string p = path.string();
    std::strncpy(addr.sun_path, p.c_str(), sizeof(addr.sun_path) - 1);
    return addr;
  }

  /**
   * @brief Connect to a control socket, send `data` and read one reply line (at most 2 s per read).
   *
   * @param path Socket path.
   * @param data Bytes to send.
   * @return The bytes read up to and including the first newline; a text starting with `<` when connecting failed.
   */
  std::string talk(const fs::path &path, const std::string &data) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return "<socket failed>";
    }
    sockaddr_un addr = make_addr(path);
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      close(fd);
      return "<connect failed>";
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
   * @brief Write an instance file `<runtime>/instances/<pid>.json` for the given instance.
   *
   * @param runtime Runtime directory.
   * @param inst Instance content.
   */
  void write_instance(const fs::path &runtime, const hf::instance_t &inst) {
    fs::path dir = runtime / "instances";
    fs::create_directories(dir);
    std::ofstream out(dir / (std::to_string(inst.pid) + ".json"), std::ios::trunc);
    out << hf::to_json(inst);
  }

  /**
   * @brief Instance with the given fields and the default UDP base.
   *
   * @param pid Process id.
   * @param role Role.
   * @param state Handoff state.
   * @return Instance.
   */
  hf::instance_t make_instance(pid_t pid, hf::role_e role, hf::state_e state) {
    hf::instance_t inst;
    inst.pid = pid;
    inst.start_time = 1;
    inst.role = role;
    inst.udp_base = 47989;
    inst.sessions = 0;
    inst.handoff = state;
    return inst;
  }

  /**
   * @brief Helper environment with short timeouts.
   *
   * @param runtime Runtime directory.
   * @return Environment.
   */
  hf::helper_env_t make_env(const fs::path &runtime) {
    hf::helper_env_t env;
    env.runtime_dir = runtime;
    env.connect_timeout = 1000ms;
    env.reply_timeout = 2000ms;
    env.active_timeout = 300ms;
    return env;
  }

  /**
   * @brief A `closed` reply.
   *
   * @param app `true` for `closed app`, `false` for `closed none`.
   * @return Reply.
   */
  hf::control_reply_t closed_reply(bool app) {
    hf::control_reply_t reply;
    reply.kind = hf::reply_kind_e::closed;
    reply.closed_app = app;
    return reply;
  }

  /**
   * @brief A `refused` reply with a reason.
   *
   * @param reason Reason text.
   * @return Reply.
   */
  hf::control_reply_t refused_reply(const std::string &reason) {
    hf::control_reply_t reply;
    reply.kind = hf::reply_kind_e::refused;
    reply.reason = reason;
    return reply;
  }

}  // namespace

/**
 * @brief Tests for the `close-app` parts of the control socket and the `--close-app` helper.
 */
class HandoffControlCloseAppTest: public testing::Test {};

/**
 * @brief `closed app` and `closed none` encode and parse (with and without the newline), `closed` alone and `closed
 * maybe` do not parse, and `exit_code_for(closed)` is 0 while `unsupported` is exit code 6.
 */
TEST_F(HandoffControlCloseAppTest, ClosedFormsEncodeParseAndExitCodes) {
  EXPECT_EQ(hf::encode_reply(closed_reply(true)), "closed app\n") << "a closed reply with an app must encode as \"closed app\\n\"";
  EXPECT_EQ(hf::encode_reply(closed_reply(false)), "closed none\n") << "a closed reply without an app must encode as \"closed none\\n\"";

  const auto app = hf::parse_reply("closed app\n");
  ASSERT_TRUE(app.has_value()) << "\"closed app\" must parse";
  EXPECT_EQ(*app, closed_reply(true)) << "\"closed app\" must parse to kind closed with closed_app true";

  const auto none = hf::parse_reply("closed none");
  ASSERT_TRUE(none.has_value()) << "\"closed none\" (without newline) must parse";
  EXPECT_EQ(*none, closed_reply(false)) << "\"closed none\" must parse to kind closed with closed_app false";

  EXPECT_FALSE(hf::parse_reply("closed").has_value()) << "a bare \"closed\" line is not one of the forms and must not parse";
  EXPECT_FALSE(hf::parse_reply("closed maybe").has_value()) << "\"closed maybe\" is not one of the forms and must not parse";

  EXPECT_EQ(static_cast<int>(hf::exit_code_for(hf::reply_kind_e::closed)), 0) << "exit_code_for(closed) must be 0";
  EXPECT_EQ(static_cast<int>(hf::helper_exit_e::unsupported), 6) << "helper exit code unsupported must be 6";
}

/**
 * @brief `control_socket_mode()`: an enabled handoff gives `handoff`; otherwise the `control_socket` option gives
 * `close_app_only`; with neither there is `none`.
 */
TEST_F(HandoffControlCloseAppTest, ControlSocketModeFollowsHandoffAndOption) {
  EXPECT_EQ(hf::control_socket_mode(false, true), hf::control_socket_mode_e::close_app_only)
    << "handoff disabled (for example privileged execution) with control_socket on must give close_app_only";
  EXPECT_EQ(hf::control_socket_mode(false, false), hf::control_socket_mode_e::none)
    << "handoff disabled and control_socket off must give none";
  EXPECT_EQ(hf::control_socket_mode(true, false), hf::control_socket_mode_e::handoff)
    << "handoff enabled must give handoff, even with control_socket off";
  EXPECT_EQ(hf::control_socket_mode(true, true), hf::control_socket_mode_e::handoff)
    << "handoff enabled and control_socket on must give handoff";
}

/**
 * @brief With a `close-app` handler, `close-app` is answered with that handler's reply and `handoff` is refused with
 * `refused drain handoff is disabled` without running any handler.
 */
TEST_F(HandoffControlCloseAppTest, CloseAppServedAndHandoffRefusedWithoutHandoffHandler) {
  temp_dir_t tmp;
  ASSERT_FALSE(tmp.path.empty()) << "a temp directory must be created for the socket";
  std::atomic<int> close_calls {0};

  hf::control_server_t::handlers_t handlers;
  handlers.close_app = [&close_calls](pid_t) {
    ++close_calls;
    return closed_reply(true);
  };

  hf::control_server_t server;
  std::string err;
  const fs::path path = hf::control_socket_path(tmp.path, 4242);
  ASSERT_TRUE(server.start(path, handlers, &err)) << "the control server must start on a temp-dir path, error: " << err;

  EXPECT_EQ(talk(path, "close-app\n"), "closed app\n") << "close-app must be answered with the close_app handler's reply, encoded as \"closed app\"";
  EXPECT_EQ(talk(path, "handoff\n"), "refused drain handoff is disabled\n") << "handoff must be refused with \"drain handoff is disabled\" when the handoff handler is empty";
  EXPECT_EQ(close_calls.load(), 1) << "the close_app handler must run once (for close-app only), ran " << close_calls.load() << " times";
}

/**
 * @brief Without a `close_app` handler, `close-app` is refused with `refused unknown verb`. A one-handler `start()` is
 * the same as a `handoff`-only server: `handoff` runs its handler and `close-app` is refused.
 */
TEST_F(HandoffControlCloseAppTest, CloseAppRefusedWithoutHandlerAndOneHandlerStartIsHandoffOnly) {
  temp_dir_t tmp;
  ASSERT_FALSE(tmp.path.empty()) << "a temp directory must be created for the socket";
  std::atomic<int> handoff_calls {0};

  hf::control_server_t::handlers_t handlers;
  handlers.handoff = [&handoff_calls](pid_t) {
    ++handoff_calls;
    return refused_reply("handoff ran");
  };

  hf::control_server_t server;
  std::string err;
  const fs::path path = hf::control_socket_path(tmp.path, 4242);
  ASSERT_TRUE(server.start(path, handlers, &err)) << "the control server must start, error: " << err;

  EXPECT_EQ(talk(path, "close-app\n"), "refused unknown verb\n") << "close-app without a close_app handler must be refused with \"unknown verb\"";
  EXPECT_EQ(talk(path, "handoff\n"), "refused handoff ran\n") << "handoff must run its handler and write that handler's reply";
  EXPECT_EQ(handoff_calls.load(), 1) << "the handoff handler must run once, ran " << handoff_calls.load() << " times";

  hf::control_server_t::handler_t one_handler = [](pid_t) {
    return refused_reply("one handler");
  };
  hf::control_server_t single;
  const fs::path single_path = hf::control_socket_path(tmp.path, 4243);
  ASSERT_TRUE(single.start(single_path, one_handler, &err)) << "the one-handler start must bind, error: " << err;
  EXPECT_EQ(talk(single_path, "close-app\n"), "refused unknown verb\n") << "the one-handler start() must behave like {handler, {}}: close-app is refused with \"unknown verb\"";
}

/**
 * @brief With a peer uid override that differs from the real uid, `close-app` is refused with `refused peer uid` and
 * the handler does not run; with the override cleared the same request is served.
 */
TEST_F(HandoffControlCloseAppTest, PeerUidMismatchRefusedAndHandlerNotCalled) {
  temp_dir_t tmp;
  ASSERT_FALSE(tmp.path.empty()) << "a temp directory must be created for the socket";
  std::atomic<int> close_calls {0};

  hf::control_server_t::handlers_t handlers;
  handlers.close_app = [&close_calls](pid_t) {
    ++close_calls;
    return closed_reply(false);
  };

  hf::control_server_t server;
  std::string err;
  const fs::path path = hf::control_socket_path(tmp.path, 4242);
  ASSERT_TRUE(server.start(path, handlers, &err)) << "the control server must start, error: " << err;

  {
    uid_override_guard_t guard(static_cast<uid_t>(geteuid()) + 1);
    EXPECT_EQ(talk(path, "close-app\n"), "refused peer uid\n") << "a peer whose uid differs from the socket owner must be refused with \"peer uid\"";
  }
  EXPECT_EQ(close_calls.load(), 0) << "the close_app handler must not run for a refused peer uid, ran " << close_calls.load() << " times";

  EXPECT_EQ(talk(path, "close-app\n"), "closed none\n") << "with the override cleared, the same connection from the same uid must be served";
  EXPECT_EQ(close_calls.load(), 1) << "the close_app handler must run once after the override is cleared, ran " << close_calls.load() << " times";
}

/**
 * @brief `--close-app` helper: the argument must be exactly one positive decimal pid, otherwise exit 5.
 */
TEST_F(HandoffControlCloseAppTest, CloseAppHelperBadArgumentsAreUsageErrors) {
  temp_dir_t tmp;
  std::string err;
  EXPECT_EQ(static_cast<int>(hf::run_close_app_helper({}, make_env(tmp.path), err)), 5) << "no arguments must be a usage error (exit 5)";
  EXPECT_EQ(static_cast<int>(hf::run_close_app_helper({"4242", "4243"}, make_env(tmp.path), err)), 5) << "two pids must be a usage error (exit 5)";
  EXPECT_EQ(static_cast<int>(hf::run_close_app_helper({"0"}, make_env(tmp.path), err)), 5) << "pid 0 must be a usage error (exit 5)";
  EXPECT_EQ(static_cast<int>(hf::run_close_app_helper({"-1"}, make_env(tmp.path), err)), 5) << "pid -1 must be a usage error (exit 5)";
  EXPECT_EQ(static_cast<int>(hf::run_close_app_helper({"abc"}, make_env(tmp.path), err)), 5) << "a non-numeric pid must be a usage error (exit 5)";
}

/**
 * @brief A server answering `closed none` gives the helper exit 0 with the outcome line "closed none".
 */
TEST_F(HandoffControlCloseAppTest, CloseAppHelperClosedNoneGivesZeroAndErrLine) {
  temp_dir_t tmp;
  ASSERT_FALSE(tmp.path.empty()) << "a temp directory must be created for the socket";
  const pid_t self = getpid();

  hf::control_server_t::handlers_t handlers;
  handlers.close_app = [](pid_t) {
    return closed_reply(false);
  };
  hf::control_server_t server;
  std::string serr;
  ASSERT_TRUE(server.start(hf::control_socket_path(tmp.path, self), handlers, &serr)) << "the control server must start, error: " << serr;

  std::string err;
  const auto code = hf::run_close_app_helper({std::to_string(self)}, make_env(tmp.path), err);
  EXPECT_EQ(static_cast<int>(code), 0) << "a closed none reply must give exit 0";
  EXPECT_EQ(err, "closed none") << "the outcome line for closed none must be exactly \"closed none\", got \"" << err << "\"";
}

/**
 * @brief A server refusing `close-app` with `refused unknown verb` (an older binary) gives the helper exit 6.
 */
TEST_F(HandoffControlCloseAppTest, CloseAppHelperUnknownVerbGivesSix) {
  temp_dir_t tmp;
  ASSERT_FALSE(tmp.path.empty()) << "a temp directory must be created for the socket";
  const pid_t self = getpid();

  hf::control_server_t::handlers_t handlers;
  handlers.handoff = [](pid_t) {
    return refused_reply("handoff only");
  };
  hf::control_server_t server;
  std::string serr;
  ASSERT_TRUE(server.start(hf::control_socket_path(tmp.path, self), handlers, &serr)) << "the control server must start, error: " << serr;

  std::string err;
  EXPECT_EQ(static_cast<int>(hf::run_close_app_helper({std::to_string(self)}, make_env(tmp.path), err)), 6)
    << "refused unknown verb must give exit 6 (unsupported)";
}

/**
 * @brief Refusals give exit 1 with the reason on the outcome line: a `refused` reply from the server, and, with no
 * socket, a draining instance file ("a drain is in progress").
 */
TEST_F(HandoffControlCloseAppTest, CloseAppHelperRefusedGivesOneWithReason) {
  temp_dir_t tmp;
  ASSERT_FALSE(tmp.path.empty()) << "a temp directory must be created for the socket";
  const pid_t self = getpid();

  hf::control_server_t::handlers_t handlers;
  handlers.close_app = [](pid_t) {
    return refused_reply("previous process still holds the app");
  };
  hf::control_server_t server;
  std::string serr;
  ASSERT_TRUE(server.start(hf::control_socket_path(tmp.path, self), handlers, &serr)) << "the control server must start, error: " << serr;

  std::string err;
  EXPECT_EQ(static_cast<int>(hf::run_close_app_helper({std::to_string(self)}, make_env(tmp.path), err)), 1)
    << "a refused close-app reply must give exit 1";
  EXPECT_NE(err.find("previous process still holds the app"), std::string::npos) << "the refusal reason must be on the outcome line, got \"" << err << "\"";

  write_instance(tmp.path, make_instance(4242, hf::role_e::draining, hf::state_e::draining));
  std::string drain_err;
  EXPECT_EQ(static_cast<int>(hf::run_close_app_helper({"4242"}, make_env(tmp.path), drain_err)), 1)
    << "with no socket and an instance file in role draining the helper must give exit 1";
  EXPECT_NE(drain_err.find("a drain is in progress"), std::string::npos) << "the reason must be \"a drain is in progress\", got \"" << drain_err << "\"";
}

/**
 * @brief With no socket and no instance file the helper gives exit 6 (unsupported).
 */
TEST_F(HandoffControlCloseAppTest, CloseAppHelperNoSocketNoInstanceFileGivesSix) {
  temp_dir_t tmp;
  std::string err;
  EXPECT_EQ(static_cast<int>(hf::run_close_app_helper({"4242"}, make_env(tmp.path), err)), 6)
    << "with no control socket and no instance file the helper must give exit 6 (unsupported)";
}

/**
 * @brief A server that does not answer within the reply timeout gives the helper exit 5.
 */
TEST_F(HandoffControlCloseAppTest, CloseAppHelperSilentServerGivesFive) {
  temp_dir_t tmp;
  ASSERT_FALSE(tmp.path.empty()) << "a temp directory must be created for the socket";
  const pid_t self = getpid();

  hf::control_server_t::handlers_t handlers;
  handlers.close_app = [](pid_t) {
    std::this_thread::sleep_for(600ms);
    return closed_reply(false);
  };
  hf::control_server_t server;
  std::string serr;
  ASSERT_TRUE(server.start(hf::control_socket_path(tmp.path, self), handlers, &serr)) << "the control server must start, error: " << serr;

  hf::helper_env_t env = make_env(tmp.path);
  env.reply_timeout = 100ms;
  std::string err;
  EXPECT_EQ(static_cast<int>(hf::run_close_app_helper({std::to_string(self)}, env, err)), 5)
    << "a reply that does not arrive within reply_timeout must give exit 5";
}

#endif  // __linux__
