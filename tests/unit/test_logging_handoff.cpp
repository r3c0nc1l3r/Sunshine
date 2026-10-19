/**
 * @file tests/unit/test_logging_handoff.cpp
 * @brief Blind tests for the drain-handoff logging additions: process tag, append mode and rotation control.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <vector>

// local includes
#include <src/logging.h>

namespace {
  /**
   * @brief Read a whole text file.
   *
   * @param path File to read.
   * @return File content, empty when the file does not exist.
   */
  std::string read_text(const std::filesystem::path &path) {
    std::ifstream input {path};
    return {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
  }

  /**
   * @brief Split text into lines without the line terminators.
   *
   * @param text Text to split.
   * @return The lines, in order.
   */
  std::vector<std::string> split_lines(const std::string &text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
      if (c == '\n') {
        lines.push_back(current);
        current.clear();
      } else {
        current.push_back(c);
      }
    }
    if (!current.empty()) {
      lines.push_back(current);
    }
    return lines;
  }
}  // namespace

/**
 * @brief Fixture that gives each logging test its own directory and resets the process tag.
 */
class LoggingHandoffTest: public BaseTest {
protected:
  /**
   * @brief Create an empty per-test directory and clear the process tag.
   */
  void SetUp() override {
    BaseTest::SetUp();
    const auto *info_ptr = testing::UnitTest::GetInstance()->current_test_info();
    test_directory = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / (std::string {"logging_handoff_"} + info_ptr->name());
    std::filesystem::remove_all(test_directory);
    std::filesystem::create_directories(test_directory);
    log_path = test_directory / "handoff.log";
    logging::set_process_tag("");
  }

  /**
   * @brief Clear the process tag and remove the per-test directory.
   */
  void TearDown() override {
    logging::set_process_tag("");
    std::error_code ignored;
    std::filesystem::remove_all(test_directory, ignored);
    BaseTest::TearDown();
  }

  /**
   * @brief Write text into the test log file, replacing any previous content.
   *
   * @param text Text to write.
   */
  void write_text_for_test(const std::string &text) const {
    std::ofstream output {log_path};
    output << text;
  }

  std::filesystem::path test_directory;  ///< Per-test directory, removed in TearDown.
  std::filesystem::path log_path;  ///< Log file used by the test.
};

TEST_F(LoggingHandoffTest, ProcessTagIsEmptyByDefaultAfterClearing) {
  EXPECT_EQ(logging::process_tag(), "") << "process_tag() must be empty when no tag was set";
}

TEST_F(LoggingHandoffTest, SetProcessTagStoresTagWithoutBrackets) {
  logging::set_process_tag("drain");
  EXPECT_EQ(logging::process_tag(), "drain") << "process_tag() must return the tag as given, without brackets";

  logging::set_process_tag("");
  EXPECT_EQ(logging::process_tag(), "") << "set_process_tag(\"\") must clear the tag";
}

TEST_F(LoggingHandoffTest, SetProcessTagReplacesPreviousTag) {
  logging::set_process_tag("first");
  logging::set_process_tag("second");
  EXPECT_EQ(logging::process_tag(), "second") << "a later set_process_tag must replace the earlier tag";
}

TEST_F(LoggingHandoffTest, TagAppearsAfterSeverityInLogLine) {
  auto deinit = logging::init(2, log_path.string(), true);
  ASSERT_NE(deinit, nullptr);

  logging::set_process_tag("drain");
  BOOST_LOG(info) << "TagPositionMarker one";
  logging::set_process_tag("");
  BOOST_LOG(info) << "TagPositionMarker two";
  deinit.reset();

  const auto lines = split_lines(read_text(log_path));
  const std::regex tagged {R"(^\[[^\]]+\]: Info: \[drain\] TagPositionMarker one$)"};
  const std::regex untagged {R"(^\[[^\]]+\]: Info: TagPositionMarker two$)"};

  const auto tagged_count = std::ranges::count_if(lines, [&](const std::string &line) {
    return std::regex_match(line, tagged);
  });
  const auto untagged_count = std::ranges::count_if(lines, [&](const std::string &line) {
    return std::regex_match(line, untagged);
  });

  EXPECT_EQ(tagged_count, 1) << "a line logged with tag \"drain\" must be `[<time>]: Info: [drain] <message>`; log was:\n"
                             << read_text(log_path);
  EXPECT_EQ(untagged_count, 1) << "a line logged after clearing the tag must be `[<time>]: Info: <message>` with no tag brackets; log was:\n"
                               << read_text(log_path);
}

TEST_F(LoggingHandoffTest, ConcurrentSetProcessTagWhileLoggingIsBoundedAndNeverTorn) {
  constexpr int record_count = 300;
  auto deinit = logging::init(2, log_path.string(), true);
  ASSERT_NE(deinit, nullptr);

  std::atomic<bool> writer_done {false};
  std::thread tag_thread {[&]() {
    const std::vector<std::string> tags {"alpha", "beta", ""};
    std::size_t index = 0;
    while (!writer_done.load()) {
      logging::set_process_tag(tags[index % tags.size()]);
      ++index;
      std::this_thread::yield();
    }
  }};

  std::thread log_thread {[&]() {
    for (int i = 0; i < record_count; ++i) {
      BOOST_LOG(info) << "ConcurrentTagRecord " << i;
    }
    writer_done.store(true);
  }};

  log_thread.join();
  tag_thread.join();
  logging::set_process_tag("");
  deinit.reset();

  const std::regex record {R"(^\[[^\]]+\]: Info: (\[(alpha|beta)\] )?ConcurrentTagRecord [0-9]+$)"};
  const auto lines = split_lines(read_text(log_path));
  int matched = 0;
  for (const auto &line : lines) {
    if (line.find("ConcurrentTagRecord") == std::string::npos) {
      continue;
    }
    if (std::regex_match(line, record)) {
      ++matched;
    } else {
      ADD_FAILURE() << "a record was torn or mis-tagged under concurrent set_process_tag; bad line: " << line;
    }
  }
  EXPECT_EQ(matched, record_count) << "every record logged while the tag changed must appear exactly once with either no tag, [alpha] or [beta]";
}

TEST_F(LoggingHandoffTest, InitAppendsToExistingFileWhenNotRotating) {
  write_text_for_test("existing content line\n");

  auto deinit = logging::init(2, log_path.string(), true);
  ASSERT_NE(deinit, nullptr);
  BOOST_LOG(info) << "AppendedRecordMarker";
  deinit.reset();

  const auto content = read_text(log_path);
  EXPECT_NE(content.find("existing content line"), std::string::npos) << "append mode must keep the existing file content; file was:\n"
                                                                      << content;
  EXPECT_NE(content.find("AppendedRecordMarker"), std::string::npos) << "the new record must be appended to the file; file was:\n"
                                                                     << content;
  EXPECT_LT(content.find("existing content line"), content.find("AppendedRecordMarker")) << "the old content must come before the appended record";
}

TEST_F(LoggingHandoffTest, NormalStartRotatesOldLogAndStartsFresh) {
  write_text_for_test("previous run content\n");

  auto deinit = logging::init(2, log_path.string());
  ASSERT_NE(deinit, nullptr);
  BOOST_LOG(info) << "FreshStartMarker";
  deinit.reset();

  const auto rotated = std::filesystem::path {log_path.string() + ".1"};
  EXPECT_NE(read_text(rotated).find("previous run content"), std::string::npos) << "a normal start must rename the old log to .1";
  const auto current = read_text(log_path);
  EXPECT_EQ(current.find("previous run content"), std::string::npos) << "a normal start must not keep the old content in the current log";
  EXPECT_NE(current.find("FreshStartMarker"), std::string::npos) << "the new log must hold the records of the new run";
}

TEST_F(LoggingHandoffTest, AppendNoRotateDoesNotRenameExistingLog) {
  write_text_for_test("keep me in place\n");

  auto deinit = logging::init(2, log_path.string(), true);
  ASSERT_NE(deinit, nullptr);
  deinit.reset();

  EXPECT_FALSE(std::filesystem::exists(log_path.string() + ".1")) << "append_no_rotate=true must not rotate the log to .1";
  EXPECT_NE(read_text(log_path).find("keep me in place"), std::string::npos) << "append_no_rotate=true must leave the running log in place";
}

TEST_F(LoggingHandoffTest, RotationFailureKeepsOldContentAndStillInitializes) {
  // A non-empty directory at the oldest generation makes rotate_log_file fail before it renames anything.
  std::filesystem::create_directories(log_path.string() + ".5" + "/child");
  write_text_for_test("current content survives\n");

  auto deinit = logging::init(2, log_path.string());
  ASSERT_NE(deinit, nullptr) << "a failed rotation must not stop logging from initializing";
  BOOST_LOG(info) << "AfterRotationFailureMarker";
  deinit.reset();

  const auto content = read_text(log_path);
  EXPECT_NE(content.find("current content survives"), std::string::npos) << "a failed rotation must keep the old content in the log file; file was:\n"
                                                                         << content;
  EXPECT_NE(content.find("AfterRotationFailureMarker"), std::string::npos) << "records must still be written after a failed rotation";
  EXPECT_TRUE(std::filesystem::is_directory(log_path.string() + ".5")) << "a failed rotation must not touch the generation that blocked it";
}

TEST_F(LoggingHandoffTest, RotateLogFileWithoutCurrentLogSucceeds) {
  EXPECT_FALSE(logging::rotate_log_file(log_path)) << "rotating a missing current log is not a failure";
}

TEST_F(LoggingHandoffTest, LongTagIsKeptAsGiven) {
  const std::string long_tag(64, 'x');
  logging::set_process_tag(long_tag);
  EXPECT_EQ(logging::process_tag(), long_tag) << "a long tag must be stored unchanged";
}
