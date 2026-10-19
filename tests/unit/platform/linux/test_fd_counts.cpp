/**
 * @file tests/unit/platform/linux/test_fd_counts.cpp
 * @brief Tests for counting open file descriptors by kind from an fd directory.
 */

#ifdef __linux__

  // standard includes
  #include <chrono>
  #include <filesystem>
  #include <fstream>
  #include <string>
  #include <system_error>

  // lib includes
  #include <gtest/gtest.h>

  // system includes
  #include <fcntl.h>
  #include <unistd.h>

  // local includes
  #include "src/platform/linux/fd_counts.h"

namespace {
  /**
   * @brief Fixture that owns a temporary directory mimicking `/proc/<pid>/fd`.
   */
  class FdCountsTest: public testing::Test {
  protected:
    std::filesystem::path fd_dir;  ///< Temporary fd directory, created in SetUp and removed in TearDown.

    /**
     * @brief Create a fresh, empty, uniquely named directory for each test.
     */
    void SetUp() override {
      const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
      fd_dir = std::filesystem::temp_directory_path() / ("fd_counts_test_" + std::to_string(::getpid()) + "_" + std::to_string(stamp));
      std::filesystem::create_directories(fd_dir);
    }

    /**
     * @brief Remove the temporary directory and everything in it.
     */
    void TearDown() override {
      std::error_code ec;
      std::filesystem::remove_all(fd_dir, ec);
    }

    /**
     * @brief Create one symlink named `name` in the fd directory that points to `target`.
     *
     * @param name Entry name, normally a descriptor number.
     * @param target Link target string; the target does not need to exist.
     */
    void link(const std::string &name, const std::string &target) const {
      std::filesystem::create_symlink(target, fd_dir / name);
    }
  };
}  // namespace

/**
 * @brief A mixed set of symlinks is counted by total, render nodes and dmabufs.
 */
TEST_F(FdCountsTest, CountsMixedEntriesByKind) {
  link("3", "/dev/dri/renderD128");
  link("4", "/dev/dri/renderD129");
  link("5", "/dmabuf:");
  link("6", "/dmabuf:foo");
  link("7", "anon_inode:dmabuf");
  link("8", "anon_inode:[eventfd]");
  link("9", "/dev/dri/card0");
  link("10", "/tmp/x");
  link("11", "socket:[123]");

  const auto counts = platf::count_open_fds(fd_dir);

  EXPECT_EQ(counts.total, 9U);
  EXPECT_EQ(counts.render_nodes, 2U);
  EXPECT_EQ(counts.dmabufs, 3U);
}

/**
 * @brief An empty fd directory yields all-zero counts.
 */
TEST_F(FdCountsTest, EmptyDirectoryYieldsZeros) {
  const auto counts = platf::count_open_fds(fd_dir);

  EXPECT_EQ(counts.total, 0U);
  EXPECT_EQ(counts.render_nodes, 0U);
  EXPECT_EQ(counts.dmabufs, 0U);
}

/**
 * @brief A missing fd directory yields all-zero counts instead of throwing.
 */
TEST_F(FdCountsTest, MissingDirectoryYieldsZeros) {
  const auto counts = platf::count_open_fds(fd_dir / "does-not-exist");

  EXPECT_EQ(counts.total, 0U);
  EXPECT_EQ(counts.render_nodes, 0U);
  EXPECT_EQ(counts.dmabufs, 0U);
}

/**
 * @brief A regular file in the fd directory is unreadable as a link and counts only towards total.
 */
TEST_F(FdCountsTest, RegularFileCountsOnlyTowardsTotal) {
  std::ofstream(fd_dir / "12") << "not a symlink";

  const auto counts = platf::count_open_fds(fd_dir);

  EXPECT_EQ(counts.total, 1U);
  EXPECT_EQ(counts.render_nodes, 0U);
  EXPECT_EQ(counts.dmabufs, 0U);
}

/**
 * @brief Opening and closing a real file changes the total of the live `/proc/self/fd` by one and back.
 */
TEST(FdCountsProcTest, OpenAndCloseChangesProcSelfTotal) {
  const auto before = platf::count_open_fds().total;

  const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  EXPECT_EQ(platf::count_open_fds().total, before + 1);

  ASSERT_EQ(::close(fd), 0);
  EXPECT_EQ(platf::count_open_fds().total, before);
}
#endif
