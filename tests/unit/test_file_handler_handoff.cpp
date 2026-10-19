/**
 * @file tests/unit/test_file_handler_handoff.cpp
 * @brief Blind tests for `file_handler::write_file_atomic` (drain-handoff state and app file writes).
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
  #include <unistd.h>
#endif

// local includes
#include <src/file_handler.h>

namespace {
  /**
   * @brief Read a whole file as bytes.
   *
   * @param path File to read.
   * @return File content.
   */
  std::string read_bytes(const std::filesystem::path &path) {
    std::ifstream input {path, std::ios::binary};
    return {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
  }

  /**
   * @brief Write bytes to a file, replacing it.
   *
   * @param path File to write.
   * @param content Bytes to write.
   */
  void write_bytes(const std::filesystem::path &path, const std::string &content) {
    std::ofstream output {path, std::ios::binary | std::ios::trunc};
    output << content;
  }

  /**
   * @brief List the entry names in a directory.
   *
   * @param directory Directory to list.
   * @return Entry file names, sorted.
   */
  std::vector<std::string> list_names(const std::filesystem::path &directory) {
    std::vector<std::string> names;
    for (const auto &entry : std::filesystem::directory_iterator {directory}) {
      names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
  }
}  // namespace

/**
 * @brief Fixture that gives each atomic-write test a fresh directory.
 */
class FileHandlerAtomicTest: public BaseTest {
protected:
  /**
   * @brief Create an empty per-test directory.
   */
  void SetUp() override {
    BaseTest::SetUp();
    const auto *info_ptr = testing::UnitTest::GetInstance()->current_test_info();
    directory = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / (std::string {"file_handler_atomic_"} + info_ptr->name());
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    target = directory / "sunshine_state.json";
  }

  /**
   * @brief Remove the per-test directory.
   */
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all, ignored);
    std::filesystem::remove_all(directory, ignored);
    BaseTest::TearDown();
  }

  std::filesystem::path directory;  ///< Per-test directory.
  std::filesystem::path target;  ///< File path passed to the writer.
};

TEST_F(FileHandlerAtomicTest, WritesNewFileWithContents) {
  const auto ec = file_handler::write_file_atomic(target.string().c_str(), "{\"root\":{}}");
  EXPECT_FALSE(ec) << "write_file_atomic must return a cleared error code on success, got: " << ec.message();
  EXPECT_EQ(read_bytes(target), "{\"root\":{}}") << "the new file must hold exactly the given contents";
}

TEST_F(FileHandlerAtomicTest, ReplacesExistingContentCompletely) {
  write_bytes(target, "a much longer previous content that must not survive any part of the replacement");

  const auto ec = file_handler::write_file_atomic(target.string().c_str(), "short");
  EXPECT_FALSE(ec) << ec.message();
  EXPECT_EQ(read_bytes(target), "short") << "the old content must be fully replaced, not partially overwritten";
}

TEST_F(FileHandlerAtomicTest, WritesEmptyContents) {
  write_bytes(target, "old");
  EXPECT_FALSE(file_handler::write_file_atomic(target.string().c_str(), "")) << "writing empty contents must succeed";
  EXPECT_EQ(read_bytes(target), "") << "an empty write must leave an empty file";
}

TEST_F(FileHandlerAtomicTest, PreservesEmbeddedNulBytes) {
  const std::string payload {"ab\0cd", 5};
  EXPECT_FALSE(file_handler::write_file_atomic(target.string().c_str(), payload)) << "binary contents must be written unchanged";
  EXPECT_EQ(read_bytes(target), payload) << "embedded NUL bytes must survive the write";
}

TEST_F(FileHandlerAtomicTest, NoTemporaryFileLeftOnSuccess) {
  write_bytes(target, "old");
  ASSERT_FALSE(file_handler::write_file_atomic(target.string().c_str(), "new"));

  const std::vector<std::string> expected {"sunshine_state.json"};
  EXPECT_EQ(list_names(directory), expected) << "a successful atomic write must leave only the target file, no `.tmp.` file";
}

#ifndef _WIN32
TEST_F(FileHandlerAtomicTest, NewFileGetsOwnerOnlyPermissions) {
  ASSERT_FALSE(file_handler::write_file_atomic(target.string().c_str(), "x"));
  const auto perms = std::filesystem::status(target).permissions() & std::filesystem::perms::all;
  EXPECT_EQ(perms, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write) << "a new file must be created with mode 0600";
}

TEST_F(FileHandlerAtomicTest, ExistingFilePermissionBitsAreKept) {
  write_bytes(target, "old");
  std::filesystem::permissions(target, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read, std::filesystem::perm_options::replace);
  const auto before = std::filesystem::status(target).permissions() & std::filesystem::perms::all;

  ASSERT_FALSE(file_handler::write_file_atomic(target.string().c_str(), "new contents"));

  const auto after = std::filesystem::status(target).permissions() & std::filesystem::perms::all;
  EXPECT_EQ(after, before) << "an existing file's permission bits must be kept by the replacement";
  EXPECT_EQ(read_bytes(target), "new contents");
}

TEST_F(FileHandlerAtomicTest, SymlinkAtPathIsReplacedNotFollowed) {
  const auto link_target = directory / "linked_target.json";
  write_bytes(link_target, "untouched target");
  std::filesystem::create_symlink(link_target, target);

  const auto ec = file_handler::write_file_atomic(target.string().c_str(), "replacement");
  EXPECT_FALSE(ec) << ec.message();

  EXPECT_FALSE(std::filesystem::is_symlink(target)) << "the symlink at the path must be replaced by a regular file";
  EXPECT_TRUE(std::filesystem::is_regular_file(target));
  EXPECT_EQ(read_bytes(target), "replacement") << "the new content must be at the path";
  EXPECT_EQ(read_bytes(link_target), "untouched target") << "the file the symlink pointed to must not be written through the link";
}

TEST_F(FileHandlerAtomicTest, MissingDirectoryReturnsEnoent) {
  const auto missing = directory / "no_such_dir" / "sunshine_state.json";
  const auto ec = file_handler::write_file_atomic(missing.string().c_str(), "x");

  ASSERT_TRUE(ec) << "a missing parent directory must be an error";
  EXPECT_EQ(ec.category(), std::generic_category()) << "POSIX errors must use the generic category";
  EXPECT_EQ(ec.value(), ENOENT) << "a missing parent directory must report ENOENT, got " << ec.message();
  EXPECT_FALSE(std::filesystem::exists(missing)) << "the parent directory must not be created";
}

TEST_F(FileHandlerAtomicTest, NonWritableDirectoryReturnsEacces) {
  if (::geteuid() == 0) {
    GTEST_SKIP() << "root bypasses directory permissions; permission case skipped";
  }
  write_bytes(target, "old");
  std::filesystem::permissions(directory, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec, std::filesystem::perm_options::replace);

  const auto ec = file_handler::write_file_atomic(target.string().c_str(), "new");

  EXPECT_TRUE(ec) << "a write into a non-writable directory must fail";
  EXPECT_EQ(ec.value(), EACCES) << "a non-writable directory must report EACCES, got " << ec.message();
  EXPECT_EQ(read_bytes(target), "old") << "the target must be untouched after a failed write";
}
#endif

TEST_F(FileHandlerAtomicTest, FailedRenameLeavesTargetAndNoTemporaryFile) {
  // A directory at the path makes the final rename fail after the temporary file was written.
  std::filesystem::create_directories(target / "occupied");

  const auto ec = file_handler::write_file_atomic(target.string().c_str(), "never lands");

  EXPECT_TRUE(ec) << "replacing a directory with a file must fail";
  EXPECT_TRUE(std::filesystem::is_directory(target)) << "the path must be left as it was when the write fails";
  const std::vector<std::string> expected {"sunshine_state.json"};
  EXPECT_EQ(list_names(directory), expected) << "a failed write must unlink its temporary file, leaving no `.tmp.` entry behind";
}

TEST_F(FileHandlerAtomicTest, ManySequentialWritesEndWithLastContents) {
  for (int i = 0; i < 25; ++i) {
    const auto text = std::to_string(i);
    ASSERT_FALSE(file_handler::write_file_atomic(target.string().c_str(), text)) << "write " << i << " failed";
  }
  EXPECT_EQ(read_bytes(target), "24") << "the last write must be the content that remains";
  const std::vector<std::string> expected {"sunshine_state.json"};
  EXPECT_EQ(list_names(directory), expected) << "repeated writes must not accumulate temporary files";
}
