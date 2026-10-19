/**
 * @file tests/unit/handoff/test_instance_file.cpp
 * @brief Blind tests for the per-process instance files (src/handoff/instance_file.*), written from the spec.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <chrono>
  #include <cstdint>
  #include <cstdlib>
  #include <filesystem>
  #include <fstream>
  #include <iterator>
  #include <map>
  #include <optional>
  #include <string>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <system_error>
  #include <thread>
  #include <unistd.h>
  #include <vector>

  // lib includes
  #include <nlohmann/json.hpp>

  // local includes
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
      std::string templ = (fs::temp_directory_path() / "hf_inst_XXXXXX").string();
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
   * @brief Build an instance value from its main fields.
   */
  hf::instance_t make_instance(pid_t pid, std::uint64_t start, hf::role_e role, hf::state_e handoff, int sessions) {
    hf::instance_t inst;
    inst.pid = pid;
    inst.start_time = start;
    inst.role = role;
    inst.udp_base = 47989;
    inst.sessions = sessions;
    inst.handoff = handoff;
    return inst;
  }

  /**
   * @brief Write an instance file directly (not through the writer under test).
   */
  void write_instance_file(const fs::path &dir, const hf::instance_t &inst) {
    fs::create_directories(dir);
    std::ofstream out(dir / (std::to_string(inst.pid) + ".json"), std::ios::trunc);
    out << hf::to_json(inst);
  }

  /**
   * @brief Write raw content to a file in the directory.
   */
  void write_raw(const fs::path &dir, const std::string &name, const std::string &content) {
    fs::create_directories(dir);
    std::ofstream out(dir / name, std::ios::trunc);
    out << content;
  }

  /**
   * @brief Build a `/proc/<pid>/stat` line whose field 22 is `start`, with the given command name.
   */
  std::string make_stat(int pid, const std::string &comm, std::uint64_t start) {
    std::string line = std::to_string(pid) + " (" + comm + ") S";
    // Fields 4 to 21 (18 fields) are filler; field 22 is the start time.
    for (int i = 4; i <= 21; ++i) {
      line += " 0";
    }
    line += " " + std::to_string(start) + " 0 0\n";
    return line;
  }

  /**
   * @brief A valid instance JSON object as a nlohmann object, to be modified by tests.
   */
  nlohmann::json valid_json() {
    return nlohmann::json::parse(hf::to_json(make_instance(4242, 5551234, hf::role_e::active, hf::state_e::none, 0)));
  }

}  // namespace

/**
 * @brief Tests for role and state names.
 */
class HandoffInstanceFileTest: public testing::Test {};

TEST_F(HandoffInstanceFileTest, RoleWireNames) {
  EXPECT_EQ(hf::to_string(hf::role_e::pending), "pending") << "role pending must serialise as \"pending\"";
  EXPECT_EQ(hf::to_string(hf::role_e::active), "active") << "role active must serialise as \"active\"";
  EXPECT_EQ(hf::to_string(hf::role_e::draining), "draining") << "role draining must serialise as \"draining\"";
}

TEST_F(HandoffInstanceFileTest, StateWireNames) {
  EXPECT_EQ(hf::to_string(hf::state_e::none), "none") << "state none must serialise as \"none\"";
  EXPECT_EQ(hf::to_string(hf::state_e::draining), "draining") << "state draining must serialise as \"draining\"";
  EXPECT_EQ(hf::to_string(hf::state_e::handover_failed), "handover_failed")
    << "state handover_failed must serialise as \"handover_failed\"";
  EXPECT_EQ(hf::to_string(hf::state_e::kill_pending), "kill_pending")
    << "state kill_pending must serialise as \"kill_pending\"";
}

TEST_F(HandoffInstanceFileTest, ParseRoleRoundTripsEveryRole) {
  for (hf::role_e role : {hf::role_e::pending, hf::role_e::active, hf::role_e::draining}) {
    auto parsed = hf::parse_role(hf::to_string(role));
    ASSERT_TRUE(parsed.has_value()) << "parse_role must accept the name produced by to_string for every role";
    EXPECT_EQ(*parsed, role) << "parse_role(to_string(role)) must return the same role";
  }
}

TEST_F(HandoffInstanceFileTest, ParseRoleRejectsUnknownNames) {
  EXPECT_FALSE(hf::parse_role("idle").has_value()) << "an unknown role name must give nullopt";
  EXPECT_FALSE(hf::parse_role("Active").has_value()) << "role names are case-sensitive; \"Active\" must give nullopt";
  EXPECT_FALSE(hf::parse_role("").has_value()) << "an empty role name must give nullopt";
}

TEST_F(HandoffInstanceFileTest, ParseStateRoundTripsEveryState) {
  for (hf::state_e state : {hf::state_e::none, hf::state_e::draining, hf::state_e::handover_failed, hf::state_e::kill_pending}) {
    auto parsed = hf::parse_state(hf::to_string(state));
    ASSERT_TRUE(parsed.has_value()) << "parse_state must accept the name produced by to_string for every state";
    EXPECT_EQ(*parsed, state) << "parse_state(to_string(state)) must return the same state";
  }
}

TEST_F(HandoffInstanceFileTest, ParseStateRejectsUnknownNames) {
  EXPECT_FALSE(hf::parse_state("ready").has_value()) << "an unknown state name must give nullopt";
  EXPECT_FALSE(hf::parse_state("Draining").has_value()) << "state names are case-sensitive; must give nullopt";
  EXPECT_FALSE(hf::parse_state("").has_value()) << "an empty state name must give nullopt";
}

TEST_F(HandoffInstanceFileTest, ToJsonUsesSpecMemberNames) {
  std::string json = hf::to_json(make_instance(4242, 5551234, hf::role_e::active, hf::state_e::none, 0));
  EXPECT_NE(json.find("\"pid\":4242"), std::string::npos) << "to_json must write \"pid\":4242, got: " << json;
  EXPECT_NE(json.find("\"start_time\":5551234"), std::string::npos)
    << "to_json must write \"start_time\":5551234, got: " << json;
  EXPECT_NE(json.find("\"role\":\"active\""), std::string::npos) << "to_json must write the role name, got: " << json;
  EXPECT_NE(json.find("\"udp_base\":47989"), std::string::npos) << "to_json must write \"udp_base\", got: " << json;
  EXPECT_NE(json.find("\"sessions\":0"), std::string::npos) << "to_json must write \"sessions\", got: " << json;
  EXPECT_NE(json.find("\"handoff\":\"none\""), std::string::npos)
    << "to_json must write the handoff state under \"handoff\", got: " << json;
}

TEST_F(HandoffInstanceFileTest, JsonRoundTripForEveryRoleAndState) {
  const std::vector<hf::role_e> roles = {hf::role_e::pending, hf::role_e::active, hf::role_e::draining};
  const std::vector<hf::state_e> states = {hf::state_e::none, hf::state_e::draining, hf::state_e::handover_failed, hf::state_e::kill_pending};
  for (hf::role_e role : roles) {
    for (hf::state_e state : states) {
      hf::instance_t original = make_instance(31337, 123456789ULL, role, state, 3);
      auto parsed = hf::parse_instance(hf::to_json(original));
      ASSERT_TRUE(parsed.has_value()) << "parse_instance must accept the output of to_json for role "
                                      << hf::to_string(role) << " and state " << hf::to_string(state);
      EXPECT_EQ(*parsed, original) << "to_json then parse_instance must return the same instance for role "
                                   << hf::to_string(role) << " and state " << hf::to_string(state);
    }
  }
}

TEST_F(HandoffInstanceFileTest, ParseAcceptsSpecExample) {
  auto parsed = hf::parse_instance(
    R"({"pid": 1234, "start_time": 5551234, "role": "active", "udp_base": 47989, "sessions": 0, "handoff": "none"})"
  );
  ASSERT_TRUE(parsed.has_value()) << "the example instance file from the spec must parse";
  EXPECT_EQ(parsed->pid, 1234) << "pid must be read from the \"pid\" member";
  EXPECT_EQ(parsed->start_time, 5551234U) << "start_time must be read from the \"start_time\" member";
  EXPECT_EQ(parsed->role, hf::role_e::active) << "role must be read from the \"role\" member";
  EXPECT_EQ(parsed->udp_base, 47989) << "udp_base must be read from the \"udp_base\" member";
  EXPECT_EQ(parsed->sessions, 0) << "sessions must be read from the \"sessions\" member";
  EXPECT_EQ(parsed->handoff, hf::state_e::none) << "handoff must be read from the \"handoff\" member";
}

TEST_F(HandoffInstanceFileTest, ParseRejectsInputThatIsNotJson) {
  EXPECT_FALSE(hf::parse_instance("").has_value()) << "an empty string is not an instance file";
  EXPECT_FALSE(hf::parse_instance("not json").has_value()) << "plain text must give nullopt";
  EXPECT_FALSE(hf::parse_instance("{").has_value()) << "truncated JSON must give nullopt";
  EXPECT_FALSE(hf::parse_instance("[1,2,3]").has_value()) << "a JSON array is not an instance object";
}

TEST_F(HandoffInstanceFileTest, ParseRejectsEachMissingMember) {
  const std::vector<std::string> keys = {"pid", "start_time", "role", "udp_base", "sessions", "handoff"};
  for (const auto &key : keys) {
    nlohmann::json obj = valid_json();
    obj.erase(key);
    auto parsed = hf::parse_instance(obj.dump());
    EXPECT_FALSE(parsed.has_value()) << "an instance file without the \"" << key << "\" member must give nullopt";
  }
}

TEST_F(HandoffInstanceFileTest, ParseRejectsWrongTypeOfEachMember) {
  const std::vector<std::string> keys = {"pid", "start_time", "role", "udp_base", "sessions", "handoff"};
  for (const auto &key : keys) {
    nlohmann::json obj = valid_json();
    obj[key] = "oops";
    auto parsed = hf::parse_instance(obj.dump());
    EXPECT_FALSE(parsed.has_value()) << "a string in the \"" << key << "\" member must give nullopt";
  }
}

TEST_F(HandoffInstanceFileTest, ParseRejectsBooleanForNumericMember) {
  nlohmann::json obj = valid_json();
  obj["sessions"] = true;
  EXPECT_FALSE(hf::parse_instance(obj.dump()).has_value())
    << "a boolean in the \"sessions\" member must give nullopt";
}

TEST_F(HandoffInstanceFileTest, ParseRejectsUnknownRoleName) {
  nlohmann::json obj = valid_json();
  obj["role"] = "idle";
  EXPECT_FALSE(hf::parse_instance(obj.dump()).has_value()) << "an unknown role name must give nullopt";
}

TEST_F(HandoffInstanceFileTest, ParseRejectsUnknownStateName) {
  nlohmann::json obj = valid_json();
  obj["handoff"] = "ready";
  EXPECT_FALSE(hf::parse_instance(obj.dump()).has_value()) << "an unknown handoff state name must give nullopt";
}

TEST_F(HandoffInstanceFileTest, ParseStatStartTimeWithPlainCommand) {
  auto start = hf::parse_stat_start_time(make_stat(1234, "sunshine", 5551234));
  ASSERT_TRUE(start.has_value()) << "a well-formed stat line must parse";
  EXPECT_EQ(*start, 5551234U) << "field 22 (starttime) must be returned";
}

TEST_F(HandoffInstanceFileTest, ParseStatStartTimeWithSpacesInCommand) {
  auto start = hf::parse_stat_start_time(make_stat(1234, "my game app", 777));
  ASSERT_TRUE(start.has_value()) << "a command name containing spaces must still parse";
  EXPECT_EQ(*start, 777U) << "fields must be counted after the last ')', so spaces in the command must not shift them";
}

TEST_F(HandoffInstanceFileTest, ParseStatStartTimeWithParenthesisInCommand) {
  auto start = hf::parse_stat_start_time(make_stat(1234, "weird) name)x", 888));
  ASSERT_TRUE(start.has_value()) << "a command name containing ')' must still parse";
  EXPECT_EQ(*start, 888U) << "the command ends at the last ')', so a ')' inside it must not shift the fields";
}

TEST_F(HandoffInstanceFileTest, ParseStatStartTimeRejectsShortLine) {
  EXPECT_FALSE(hf::parse_stat_start_time("1234 (sunshine) S 1 2").has_value())
    << "a stat line with too few fields must give nullopt";
}

TEST_F(HandoffInstanceFileTest, ParseStatStartTimeRejectsGarbage) {
  EXPECT_FALSE(hf::parse_stat_start_time("garbage").has_value()) << "garbage must give nullopt";
  EXPECT_FALSE(hf::parse_stat_start_time("").has_value()) << "an empty string must give nullopt";
  EXPECT_FALSE(hf::parse_stat_start_time("1234 sunshine S 1 2 3").has_value())
    << "a line without parentheses must give nullopt";
}

TEST_F(HandoffInstanceFileTest, ParseStatStartTimeRejectsMissingCloseParenthesis) {
  EXPECT_FALSE(hf::parse_stat_start_time("1234 (sunshine S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22").has_value())
    << "a line without a closing ')' must give nullopt";
}

TEST_F(HandoffInstanceFileTest, ParseStatStartTimeRejectsNonNumericStartTime) {
  std::string line = "1234 (sunshine) S 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 abc 0 0\n";
  EXPECT_FALSE(hf::parse_stat_start_time(line).has_value())
    << "a non-numeric field 22 must give nullopt";
}

TEST_F(HandoffInstanceFileTest, ReadStartTimeOfSelfIsSetAndStable) {
  auto first = hf::read_start_time(getpid());
  ASSERT_TRUE(first.has_value()) << "the start time of the running test process must be readable from /proc";
  EXPECT_GT(*first, 0U) << "a live process must have a non-zero start time";
  auto second = hf::read_start_time(getpid());
  ASSERT_TRUE(second.has_value()) << "the start time must still be readable on a second call";
  EXPECT_EQ(*first, *second) << "the start time of one process must not change between reads";
}

TEST_F(HandoffInstanceFileTest, ReadStartTimeOfReapedChildIsNullopt) {
  pid_t child = fork();
  ASSERT_GE(child, 0) << "fork must succeed for the test";
  if (child == 0) {
    _exit(0);
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child) << "the test must reap the child it forked";
  EXPECT_FALSE(hf::read_start_time(child).has_value())
    << "a reaped child has no /proc entry, so its start time must be nullopt";
}

TEST_F(HandoffInstanceFileTest, RuntimeDirPrefersRuntimeDirectory) {
  auto dir = hf::runtime_dir("/run/sunshine-test", "/run/user/1000");
  ASSERT_TRUE(dir.has_value()) << "RUNTIME_DIRECTORY set must give a runtime directory";
  EXPECT_EQ(*dir, fs::path("/run/sunshine-test")) << "RUNTIME_DIRECTORY must win over XDG_RUNTIME_DIR";
}

TEST_F(HandoffInstanceFileTest, RuntimeDirUsesFirstEntryOfList) {
  auto dir = hf::runtime_dir("/run/a:/run/b", "");
  ASSERT_TRUE(dir.has_value()) << "a RUNTIME_DIRECTORY list must give a runtime directory";
  EXPECT_EQ(*dir, fs::path("/run/a")) << "only the first ':'-separated entry of RUNTIME_DIRECTORY must be used";
}

TEST_F(HandoffInstanceFileTest, RuntimeDirFallsBackToXdgRuntimeDir) {
  auto dir = hf::runtime_dir("", "/run/user/1000");
  ASSERT_TRUE(dir.has_value()) << "without RUNTIME_DIRECTORY, XDG_RUNTIME_DIR must be used";
  EXPECT_EQ(*dir, fs::path("/run/user/1000/sunshine")) << "the fallback must be XDG_RUNTIME_DIR/sunshine";
}

TEST_F(HandoffInstanceFileTest, RuntimeDirNulloptWhenNeitherSet) {
  EXPECT_FALSE(hf::runtime_dir("", "").has_value())
    << "with neither RUNTIME_DIRECTORY nor XDG_RUNTIME_DIR set there is no runtime directory";
}

TEST_F(HandoffInstanceFileTest, WriterCreatesDirectoryAndFile) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  hf::instance_t initial = make_instance(4242, 5551234, hf::role_e::pending, hf::state_e::none, 0);
  hf::instance_writer_t writer(dir, initial);
  EXPECT_FALSE(fs::exists(dir)) << "constructing a writer must not write anything yet";

  ASSERT_TRUE(writer.write()) << "write() must succeed in a writable temp directory";
  ASSERT_TRUE(fs::is_directory(dir)) << "the instances directory must be created on first write";
  EXPECT_EQ(fs::status(dir).permissions() & fs::perms::all, fs::perms::owner_all)
    << "the instances directory must be created with mode 0700";
  fs::path file = dir / "4242.json";
  ASSERT_TRUE(fs::exists(file)) << "the file must be named <pid>.json";

  std::ifstream in(file);
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto parsed = hf::parse_instance(content);
  ASSERT_TRUE(parsed.has_value()) << "the written file must hold a valid instance JSON, got: " << content;
  EXPECT_EQ(*parsed, initial) << "the written file must hold the initial content";
}

TEST_F(HandoffInstanceFileTest, WriterLeavesNoTemporaryFiles) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  hf::instance_writer_t writer(dir, make_instance(7, 8, hf::role_e::active, hf::state_e::none, 0));
  ASSERT_TRUE(writer.write()) << "write() must succeed";
  ASSERT_TRUE(writer.set_role(hf::role_e::draining)) << "set_role() must succeed";
  ASSERT_TRUE(writer.set_sessions(2)) << "set_sessions() must succeed";

  int entries = 0;
  for (const auto &entry : fs::directory_iterator(dir)) {
    (void) entry;
    ++entries;
  }
  EXPECT_EQ(entries, 1) << "after writes only <pid>.json may remain; temporary files must be renamed away";
}

TEST_F(HandoffInstanceFileTest, SetRolePersistsAndUpdatesCurrent) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  hf::instance_writer_t writer(dir, make_instance(4242, 99, hf::role_e::pending, hf::state_e::none, 0));
  ASSERT_TRUE(writer.write()) << "write() must succeed";
  ASSERT_TRUE(writer.set_role(hf::role_e::active)) << "set_role() must return true on success";

  EXPECT_EQ(writer.current().role, hf::role_e::active) << "current() must reflect the new role";
  auto parsed = hf::parse_instance([&] {
    std::ifstream in(dir / "4242.json");
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }());
  ASSERT_TRUE(parsed.has_value()) << "the file must remain a valid instance after set_role()";
  EXPECT_EQ(parsed->role, hf::role_e::active) << "set_role() must persist the new role to the file";
  EXPECT_EQ(parsed->start_time, 99U) << "set_role() must not change the other fields";
}

TEST_F(HandoffInstanceFileTest, SetStatePersistsToFile) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  hf::instance_writer_t writer(dir, make_instance(4242, 99, hf::role_e::active, hf::state_e::none, 0));
  ASSERT_TRUE(writer.set_state(hf::state_e::kill_pending)) << "set_state() must return true on success";

  EXPECT_EQ(writer.current().handoff, hf::state_e::kill_pending) << "current() must reflect the new state";
  std::ifstream in(dir / "4242.json");
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto parsed = hf::parse_instance(content);
  ASSERT_TRUE(parsed.has_value()) << "the file must be a valid instance after set_state(), got: " << content;
  EXPECT_EQ(parsed->handoff, hf::state_e::kill_pending) << "set_state() must persist the handoff state to the file";
}

TEST_F(HandoffInstanceFileTest, SetSessionsPersistsToFile) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  hf::instance_writer_t writer(dir, make_instance(4242, 99, hf::role_e::active, hf::state_e::none, 0));
  ASSERT_TRUE(writer.set_sessions(3)) << "set_sessions() must return true on success";

  EXPECT_EQ(writer.current().sessions, 3) << "current() must reflect the new session count";
  std::ifstream in(dir / "4242.json");
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto parsed = hf::parse_instance(content);
  ASSERT_TRUE(parsed.has_value()) << "the file must be a valid instance after set_sessions(), got: " << content;
  EXPECT_EQ(parsed->sessions, 3) << "set_sessions() must persist the session count to the file";
}

TEST_F(HandoffInstanceFileTest, CurrentReturnsLatestContentAfterSeveralUpdates) {
  temp_dir_t tmp;
  hf::instance_t initial = make_instance(55, 66, hf::role_e::pending, hf::state_e::none, 0);
  hf::instance_writer_t writer(tmp.path / "instances", initial);
  ASSERT_TRUE(writer.set_role(hf::role_e::active));
  ASSERT_TRUE(writer.set_sessions(1));
  ASSERT_TRUE(writer.set_state(hf::state_e::draining));

  hf::instance_t expected = make_instance(55, 66, hf::role_e::active, hf::state_e::draining, 1);
  EXPECT_EQ(writer.current(), expected) << "current() must return the content after all updates, with pid and start "
                                           "time unchanged";
}

TEST_F(HandoffInstanceFileTest, RemoveDeletesFileAndIsSafeTwice) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  hf::instance_writer_t writer(dir, make_instance(4242, 1, hf::role_e::active, hf::state_e::none, 0));
  ASSERT_TRUE(writer.write()) << "write() must succeed";
  ASSERT_TRUE(fs::exists(dir / "4242.json")) << "precondition: the file exists after write()";

  EXPECT_NO_THROW(writer.remove()) << "remove() must not throw";
  EXPECT_FALSE(fs::exists(dir / "4242.json")) << "remove() must delete the instance file";
  EXPECT_NO_THROW(writer.remove()) << "a second remove() must be safe when the file no longer exists";
  EXPECT_FALSE(fs::exists(dir / "4242.json")) << "the file must stay deleted after the second remove()";
}

TEST_F(HandoffInstanceFileTest, RemoveBeforeAnyWriteIsSafe) {
  temp_dir_t tmp;
  hf::instance_writer_t writer(tmp.path / "instances", make_instance(4242, 1, hf::role_e::active, hf::state_e::none, 0));
  EXPECT_NO_THROW(writer.remove()) << "remove() on a writer that never wrote must be safe";
  EXPECT_FALSE(fs::exists(tmp.path / "instances" / "4242.json")) << "no file must appear from remove()";
}

TEST_F(HandoffInstanceFileTest, RemoveInstanceFileDeletesNamedFileOnly) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  write_instance_file(dir, make_instance(77, 1, hf::role_e::active, hf::state_e::none, 0));
  write_instance_file(dir, make_instance(78, 1, hf::role_e::active, hf::state_e::none, 0));

  hf::remove_instance_file(dir, 77);
  EXPECT_FALSE(fs::exists(dir / "77.json")) << "remove_instance_file must delete <pid>.json for the given pid";
  EXPECT_TRUE(fs::exists(dir / "78.json")) << "remove_instance_file must not touch other pids' files";
}

TEST_F(HandoffInstanceFileTest, RemoveInstanceFileMissingIsNoop) {
  temp_dir_t tmp;
  EXPECT_NO_THROW(hf::remove_instance_file(tmp.path / "instances", 77))
    << "removing a file that does not exist must be a no-op";
}

TEST_F(HandoffInstanceFileTest, ScanMissingDirectoryReportsDirMissing) {
  temp_dir_t tmp;
  auto scan = hf::scan_instances(tmp.path / "absent", [](pid_t) {
    return std::optional<std::uint64_t>(1);
  });
  EXPECT_TRUE(scan.dir_missing) << "a directory that does not exist must set dir_missing";
  EXPECT_TRUE(scan.live.empty()) << "a missing directory must give no live instances";
  EXPECT_TRUE(scan.stale.empty()) << "a missing directory must give no stale pids";
  EXPECT_FALSE(scan.any_unreadable) << "a missing directory is not an unreadable file";
}

TEST_F(HandoffInstanceFileTest, ScanEmptyDirectoryFindsNothing) {
  temp_dir_t tmp;
  fs::create_directories(tmp.path / "instances");
  auto scan = hf::scan_instances(tmp.path / "instances", [](pid_t) {
    return std::optional<std::uint64_t>(1);
  });
  EXPECT_FALSE(scan.dir_missing) << "an existing directory must not set dir_missing";
  EXPECT_TRUE(scan.live.empty()) << "an empty directory must give no live instances";
  EXPECT_TRUE(scan.stale.empty()) << "an empty directory must give no stale pids";
  EXPECT_FALSE(scan.any_unreadable) << "an empty directory must not be unreadable";
}

TEST_F(HandoffInstanceFileTest, ScanLiveFileWithMatchingStartTime) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  hf::instance_t inst = make_instance(100, 555, hf::role_e::active, hf::state_e::none, 0);
  write_instance_file(dir, inst);
  auto scan = hf::scan_instances(dir, [](pid_t pid) {
    return pid == 100 ? std::optional<std::uint64_t>(555) : std::nullopt;
  });
  ASSERT_EQ(scan.live.size(), 1U) << "a file whose pid is alive with the recorded start time must be live";
  EXPECT_EQ(scan.live[0], inst) << "the live entry must hold the file's content";
  EXPECT_TRUE(scan.stale.empty()) << "a live file must not be listed as stale";
}

TEST_F(HandoffInstanceFileTest, ScanStaleWhenPidIsGone) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  write_instance_file(dir, make_instance(100, 555, hf::role_e::active, hf::state_e::none, 0));
  auto scan = hf::scan_instances(dir, [](pid_t) {
    return std::optional<std::uint64_t>();
  });
  EXPECT_TRUE(scan.live.empty()) << "a file whose pid does not exist must not be live";
  ASSERT_EQ(scan.stale.size(), 1U) << "a file whose pid does not exist must be reported as stale";
  EXPECT_EQ(scan.stale[0], 100) << "the stale entry must be the pid of the file";
}

TEST_F(HandoffInstanceFileTest, ScanStaleWhenStartTimeDiffers) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  write_instance_file(dir, make_instance(100, 555, hf::role_e::active, hf::state_e::none, 0));
  auto scan = hf::scan_instances(dir, [](pid_t) {
    return std::optional<std::uint64_t>(999);
  });
  EXPECT_TRUE(scan.live.empty()) << "a pid with another start time is pid reuse and must not be live";
  ASSERT_EQ(scan.stale.size(), 1U) << "a pid with another start time must be reported as stale";
  EXPECT_EQ(scan.stale[0], 100) << "the stale entry must be the pid of the file";
}

TEST_F(HandoffInstanceFileTest, ScanGarbageJsonIsUnreadable) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  write_raw(dir, "100.json", "{not json");
  auto scan = hf::scan_instances(dir, [](pid_t) {
    return std::optional<std::uint64_t>(1);
  });
  EXPECT_TRUE(scan.any_unreadable) << "a *.json file that is not JSON must set any_unreadable";
  EXPECT_TRUE(scan.live.empty()) << "an unreadable file must not appear as live";
}

TEST_F(HandoffInstanceFileTest, ScanInvalidInstanceIsUnreadable) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  nlohmann::json obj = valid_json();
  obj.erase("role");
  write_raw(dir, "4242.json", obj.dump());
  auto scan = hf::scan_instances(dir, [](pid_t) {
    return std::optional<std::uint64_t>(5551234);
  });
  EXPECT_TRUE(scan.any_unreadable) << "a *.json file that parses but is not a valid instance must set any_unreadable";
}

TEST_F(HandoffInstanceFileTest, ScanIgnoresNonJsonNames) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  write_raw(dir, "100.txt", "garbage");
  write_raw(dir, "notes", "garbage");
  write_raw(dir, "100.json.tmp", "garbage");
  auto scan = hf::scan_instances(dir, [](pid_t) {
    return std::optional<std::uint64_t>(1);
  });
  EXPECT_FALSE(scan.any_unreadable) << "files whose names do not end in .json must be ignored, not read as unreadable";
  EXPECT_TRUE(scan.live.empty()) << "ignored files must not be live";
  EXPECT_TRUE(scan.stale.empty()) << "ignored files must not be stale";
}

TEST_F(HandoffInstanceFileTest, ScanMixedFilesAreCountedSeparately) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  write_instance_file(dir, make_instance(100, 555, hf::role_e::active, hf::state_e::none, 0));
  write_instance_file(dir, make_instance(200, 666, hf::role_e::active, hf::state_e::none, 0));
  write_raw(dir, "300.json", "{");
  std::map<pid_t, std::uint64_t> alive = {{100, 555}, {200, 1}};
  auto scan = hf::scan_instances(dir, [&](pid_t pid) -> std::optional<std::uint64_t> {
    auto it = alive.find(pid);
    if (it == alive.end()) {
      return std::nullopt;
    }
    return it->second;
  });
  EXPECT_EQ(scan.live.size(), 1U) << "only pid 100 has the recorded start time, so exactly one live entry is expected";
  ASSERT_EQ(scan.stale.size(), 1U) << "pid 200 has another start time and must be the only stale entry";
  EXPECT_EQ(scan.stale[0], 200) << "the stale entry must be pid 200";
  EXPECT_TRUE(scan.any_unreadable) << "the garbage 300.json must set any_unreadable";
}

TEST_F(HandoffInstanceFileTest, ConsumerBusyWhenDirMissingWhileSunshineRuns) {
  hf::scan_result_t scan;
  scan.dir_missing = true;
  EXPECT_TRUE(hf::consumer_busy(scan, {100})) << "a missing instances directory while Sunshine runs must be busy";
}

TEST_F(HandoffInstanceFileTest, ConsumerIdleWhenDirMissingAndNothingRuns) {
  hf::scan_result_t scan;
  scan.dir_missing = true;
  EXPECT_FALSE(hf::consumer_busy(scan, {})) << "a missing directory with no running Sunshine must not be busy";
}

TEST_F(HandoffInstanceFileTest, ConsumerBusyWhenRunningPidHasNoFile) {
  hf::scan_result_t scan;
  EXPECT_TRUE(hf::consumer_busy(scan, {100})) << "a running Sunshine pid without a live file must be busy";
}

TEST_F(HandoffInstanceFileTest, ConsumerBusyWhenRunningPidHasOnlyStaleFile) {
  hf::scan_result_t scan;
  scan.stale.push_back(100);
  EXPECT_TRUE(hf::consumer_busy(scan, {100}))
    << "a running Sunshine pid whose only file is stale has no live file and must be busy";
}

TEST_F(HandoffInstanceFileTest, ConsumerBusyWhenAnotherRunningPidHasNoFile) {
  hf::scan_result_t scan;
  scan.live.push_back(make_instance(100, 1, hf::role_e::active, hf::state_e::none, 0));
  EXPECT_TRUE(hf::consumer_busy(scan, {100, 200}))
    << "one running pid without a file is enough to make Sunshine busy, even when another pid is idle";
}

TEST_F(HandoffInstanceFileTest, ConsumerBusyWhenAnyFileUnreadable) {
  hf::scan_result_t scan;
  scan.any_unreadable = true;
  scan.live.push_back(make_instance(100, 1, hf::role_e::active, hf::state_e::none, 0));
  EXPECT_TRUE(hf::consumer_busy(scan, {100})) << "an unreadable file anywhere must make Sunshine busy";
}

TEST_F(HandoffInstanceFileTest, ConsumerBusyWhenLiveFileIsPending) {
  hf::scan_result_t scan;
  scan.live.push_back(make_instance(100, 1, hf::role_e::pending, hf::state_e::none, 0));
  EXPECT_TRUE(hf::consumer_busy(scan, {100})) << "a live file with role pending must make Sunshine busy";
}

TEST_F(HandoffInstanceFileTest, ConsumerBusyWhenLiveFileHasSessions) {
  hf::scan_result_t scan;
  scan.live.push_back(make_instance(100, 1, hf::role_e::active, hf::state_e::none, 2));
  EXPECT_TRUE(hf::consumer_busy(scan, {100})) << "a live file with sessions above 0 must make Sunshine busy";
}

TEST_F(HandoffInstanceFileTest, ConsumerBusyWhenLiveFileIsKillPending) {
  hf::scan_result_t scan;
  scan.live.push_back(make_instance(100, 1, hf::role_e::active, hf::state_e::kill_pending, 0));
  EXPECT_TRUE(hf::consumer_busy(scan, {100})) << "a live file with state kill_pending must make Sunshine busy";
}

TEST_F(HandoffInstanceFileTest, ConsumerIdleForActiveProcessWithoutSessions) {
  hf::scan_result_t scan;
  scan.live.push_back(make_instance(100, 1, hf::role_e::active, hf::state_e::none, 0));
  EXPECT_FALSE(hf::consumer_busy(scan, {100}))
    << "a running pid with a live active file, no sessions and no handoff must be idle";
}

TEST_F(HandoffInstanceFileTest, ConsumerIdleForDrainingProcessWithoutSessions) {
  // SPEC-ANSWERED (lead): a draining or handover_failed instance with 0 sessions is not busy.
  hf::scan_result_t scan;
  scan.live.push_back(make_instance(100, 1, hf::role_e::draining, hf::state_e::draining, 0));
  EXPECT_FALSE(hf::consumer_busy(scan, {100}))
    << "by the listed rule a draining instance with no sessions is not busy (traffic is checked separately)";
}

TEST_F(HandoffInstanceFileTest, ConsumerIdleForHandoverFailedWithoutSessions) {
  // SPEC-ANSWERED (lead): handover_failed with 0 sessions is not busy (an alert for tooling only).
  hf::scan_result_t scan;
  scan.live.push_back(make_instance(100, 1, hf::role_e::active, hf::state_e::handover_failed, 0));
  EXPECT_FALSE(hf::consumer_busy(scan, {100}))
    << "by the listed rule handover_failed with no sessions is not busy; the spec question is open";
}

TEST_F(HandoffInstanceFileTest, WaitForActiveTrueWhenAlreadyActive) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  write_instance_file(dir, make_instance(4242, 777, hf::role_e::active, hf::state_e::none, 0));
  auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(hf::wait_for_active(dir, 4242, 777, 1000ms))
    << "a file already showing role active with the matching start time must give true";
  EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms) << "an already-active file must be seen quickly";
}

TEST_F(HandoffInstanceFileTest, WaitForActiveTrueWhenFileAppearsLate) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  std::thread writer([&] {
    std::this_thread::sleep_for(150ms);
    write_instance_file(dir, make_instance(4242, 777, hf::role_e::active, hf::state_e::none, 0));
  });
  bool seen = hf::wait_for_active(dir, 4242, 777, 1500ms);
  writer.join();
  EXPECT_TRUE(seen) << "a file that becomes role active within the timeout must give true";
}

TEST_F(HandoffInstanceFileTest, WaitForActiveFalseOnWrongStartTime) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  write_instance_file(dir, make_instance(4242, 1, hf::role_e::active, hf::state_e::none, 0));
  EXPECT_FALSE(hf::wait_for_active(dir, 4242, 2, 300ms))
    << "an active file with another start time is a different process and must give false at the timeout";
}

TEST_F(HandoffInstanceFileTest, WaitForActiveFalseWhenRolePending) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  write_instance_file(dir, make_instance(4242, 777, hf::role_e::pending, hf::state_e::none, 0));
  EXPECT_FALSE(hf::wait_for_active(dir, 4242, 777, 300ms))
    << "a file that still shows role pending must not count as active; expected false at the timeout";
}

TEST_F(HandoffInstanceFileTest, WaitForActiveFalseWhenRoleDraining) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  write_instance_file(dir, make_instance(4242, 777, hf::role_e::draining, hf::state_e::none, 0));
  EXPECT_FALSE(hf::wait_for_active(dir, 4242, 777, 300ms))
    << "a file that shows role draining must not count as active; expected false at the timeout";
}

TEST_F(HandoffInstanceFileTest, WaitForActiveHonoursTimeoutWhenNoFile) {
  temp_dir_t tmp;
  auto begin = std::chrono::steady_clock::now();
  EXPECT_FALSE(hf::wait_for_active(tmp.path / "absent", 4242, 777, 300ms))
    << "with no file at all wait_for_active must give false";
  auto elapsed = std::chrono::steady_clock::now() - begin;
  EXPECT_GE(elapsed, 250ms) << "wait_for_active must wait for about the timeout before giving up, not return early";
  EXPECT_LT(elapsed, 1500ms) << "wait_for_active must not wait much longer than its timeout";
}

TEST_F(HandoffInstanceFileTest, RuntimeDirTreatsRelativeRuntimeDirectoryAsUnset) {
  auto dir = hf::runtime_dir("relative/dir", "/run/user/1000");
  ASSERT_TRUE(dir.has_value()) << "a relative RUNTIME_DIRECTORY must fall back to XDG_RUNTIME_DIR";
  EXPECT_EQ(*dir, fs::path("/run/user/1000/sunshine")) << "a relative RUNTIME_DIRECTORY must count as unset";
}

TEST_F(HandoffInstanceFileTest, RuntimeDirTreatsRelativeXdgRuntimeDirAsUnset) {
  EXPECT_FALSE(hf::runtime_dir("", "relative").has_value())
    << "a relative XDG_RUNTIME_DIR must count as unset, giving nullopt when RUNTIME_DIRECTORY is also unset";
  EXPECT_FALSE(hf::runtime_dir("relative", "also/relative").has_value())
    << "when both values are relative there is no runtime directory";
}

TEST_F(HandoffInstanceFileTest, ScanFileNamedDifferentlyFromItsPidIsUnreadable) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  write_raw(dir, "123.json", hf::to_json(make_instance(456, 1, hf::role_e::active, hf::state_e::none, 0)));
  auto scan = hf::scan_instances(dir, [](pid_t) {
    return std::optional<std::uint64_t>(1);
  });
  EXPECT_TRUE(scan.any_unreadable) << "a file named 123.json whose content pid is 456 must count as unreadable";
}

TEST_F(HandoffInstanceFileTest, WriterWritesNothingAfterRemove) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  hf::instance_writer_t writer(dir, make_instance(4242, 99, hf::role_e::active, hf::state_e::none, 0));
  ASSERT_TRUE(writer.write()) << "precondition: the first write must succeed";
  writer.remove();
  EXPECT_FALSE(writer.set_sessions(5)) << "set_sessions() after remove() must return false";
  EXPECT_FALSE(writer.set_role(hf::role_e::draining)) << "set_role() after remove() must return false";
  EXPECT_FALSE(writer.set_state(hf::state_e::kill_pending)) << "set_state() after remove() must return false";
  EXPECT_FALSE(writer.write()) << "write() after remove() must return false";
  EXPECT_FALSE(fs::exists(dir / "4242.json")) << "no instance file may reappear after remove()";
}

TEST_F(HandoffInstanceFileTest, WriterDoesNotFollowSymlinkAtTempName) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  fs::create_directories(dir);
  fs::permissions(dir, fs::perms::owner_all);
  fs::path victim = tmp.path / "victim.txt";
  {
    std::ofstream out(victim);
    out << "original";
  }
  fs::create_symlink(victim, dir / "4242.json.tmp");

  hf::instance_writer_t writer(dir, make_instance(4242, 99, hf::role_e::active, hf::state_e::none, 0));
  bool ok = writer.write();  // Either outcome is acceptable; writing through the symlink is not.
  std::ifstream in(victim);
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(content, "original") << "the symlink target must never be written through the temporary name";
  if (ok) {
    std::ifstream file(dir / "4242.json");
    std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    EXPECT_TRUE(hf::parse_instance(json).has_value()) << "a successful write must leave a valid instance file";
  }
}

TEST_F(HandoffInstanceFileTest, WrittenFileHasMode0600) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "instances";
  hf::instance_writer_t writer(dir, make_instance(4242, 99, hf::role_e::active, hf::state_e::none, 0));
  ASSERT_TRUE(writer.write()) << "precondition: write() must succeed";
  EXPECT_EQ(fs::status(dir / "4242.json").permissions() & fs::perms::all, fs::perms::owner_read | fs::perms::owner_write)
    << "the instance file must be created with mode 0600";
}

TEST_F(HandoffInstanceFileTest, WriteFailsWhenInstancesDirIsSymlink) {
  temp_dir_t tmp;
  fs::path real = tmp.path / "real";
  fs::create_directories(real);
  fs::create_symlink(real, tmp.path / "instances");
  hf::instance_writer_t writer(tmp.path / "instances", make_instance(4242, 99, hf::role_e::active, hf::state_e::none, 0));
  EXPECT_FALSE(writer.write()) << "a symlink as the instances directory must make write() fail";
  EXPECT_TRUE(fs::is_empty(real)) << "nothing may be written through the symlinked directory";
}

TEST_F(HandoffInstanceFileTest, WriteFailsWhenInstancesDirIsGroupOrOtherWritable) {
  temp_dir_t tmp;
  fs::path group_dir = tmp.path / "group";
  fs::path other_dir = tmp.path / "other";
  fs::create_directories(group_dir);
  fs::create_directories(other_dir);
  fs::permissions(group_dir, fs::perms::owner_all | fs::perms::group_write, fs::perm_options::replace);
  fs::permissions(other_dir, fs::perms::owner_all | fs::perms::others_write, fs::perm_options::replace);

  hf::instance_writer_t group_writer(group_dir, make_instance(4242, 99, hf::role_e::active, hf::state_e::none, 0));
  EXPECT_FALSE(group_writer.write()) << "a group-writable instances directory must make write() fail";
  hf::instance_writer_t other_writer(other_dir, make_instance(4242, 99, hf::role_e::active, hf::state_e::none, 0));
  EXPECT_FALSE(other_writer.write()) << "an other-writable instances directory must make write() fail";
}

TEST_F(HandoffInstanceFileTest, WriterCreatesMissingNestedParents) {
  temp_dir_t tmp;
  fs::path dir = tmp.path / "a" / "b" / "instances";
  hf::instance_writer_t writer(dir, make_instance(4242, 99, hf::role_e::active, hf::state_e::none, 0));
  EXPECT_TRUE(writer.write()) << "write() must create missing parent directories";
  EXPECT_TRUE(fs::is_directory(dir)) << "the nested instances directory must exist after write()";
}

#endif  // __linux__
