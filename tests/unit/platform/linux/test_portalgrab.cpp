/**
 * @file tests/unit/platform/linux/test_portalgrab.cpp
 * @brief Tests that a failed portal enumeration leaves file descriptors it does not own open.
 */
#ifdef SUNSHINE_BUILD_PORTAL

  // standard includes
  #include <chrono>
  #include <cstdlib>
  #include <optional>
  #include <string>
  #include <vector>

  // lib includes
  #include <gtest/gtest.h>

  // system includes
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>

namespace platf {
  /**
   * @brief Enumerate display names through the desktop portal.
   *
   * @return The display names, or an empty list when the portal cannot be reached.
   */
  std::vector<std::string> portal_display_names();
}  // namespace platf

namespace {
  /**
   * @brief Device and inode pair that identifies the object behind a file descriptor.
   */
  struct fd_identity_t {
    dev_t dev;  ///< Device number reported by fstat.
    ino_t ino;  ///< Inode number reported by fstat.
  };

  /**
   * @brief Bus address that never resolves to a real session bus, so the portal is unreachable.
   */
  constexpr auto unreachable_bus_address = "unix:path=/nonexistent/sunshine-test-bus";

  /**
   * @brief Read the device and inode of an open file descriptor.
   *
   * @param fd File descriptor to inspect.
   * @return The identity of the object, or std::nullopt when fstat fails.
   */
  std::optional<fd_identity_t> identify_fd(int fd) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
      return std::nullopt;
    }
    return fd_identity_t {st.st_dev, st.st_ino};
  }

  /**
   * @brief Check whether a file descriptor is open and still refers to the expected object.
   *
   * @param fd File descriptor to check.
   * @param expected Identity recorded before the operation under test.
   * @return True when the descriptor is open and its device and inode match.
   */
  bool same_open_object(int fd, const fd_identity_t &expected) {
    if (::fcntl(fd, F_GETFD) == -1) {
      return false;
    }
    const auto current = identify_fd(fd);
    return current.has_value() && current->dev == expected.dev && current->ino == expected.ino;
  }

  /**
   * @brief RAII guard that points DBUS_SESSION_BUS_ADDRESS at an unreachable bus for its lifetime.
   *
   * The previous value is restored on destruction, or the variable is unset if it was not set before.
   */
  class unreachable_bus_guard_t {
  public:
    /**
     * @brief Save the current bus address and install the unreachable one.
     */
    unreachable_bus_guard_t() {
      const char *previous = std::getenv("DBUS_SESSION_BUS_ADDRESS");
      if (previous != nullptr) {
        previous_value_ = std::string {previous};
      }
      ::setenv("DBUS_SESSION_BUS_ADDRESS", unreachable_bus_address, 1);
    }

    /**
     * @brief Restore the previous bus address, or unset the variable if it was unset before.
     */
    ~unreachable_bus_guard_t() {
      if (previous_value_.has_value()) {
        ::setenv("DBUS_SESSION_BUS_ADDRESS", previous_value_->c_str(), 1);
      } else {
        ::unsetenv("DBUS_SESSION_BUS_ADDRESS");
      }
    }

    unreachable_bus_guard_t(const unreachable_bus_guard_t &) = delete;
    unreachable_bus_guard_t &operator=(const unreachable_bus_guard_t &) = delete;
    unreachable_bus_guard_t(unreachable_bus_guard_t &&) = delete;
    unreachable_bus_guard_t &operator=(unreachable_bus_guard_t &&) = delete;

  private:
    std::optional<std::string> previous_value_;  ///< Bus address before the guard ran, if any.
  };

  /**
   * @brief Open /dev/null read-only.
   *
   * @return The new file descriptor, or -1 on failure.
   */
  int open_dev_null() {
    return ::open("/dev/null", O_RDONLY);
  }
}  // namespace

/**
 * @brief With no reachable portal, the enumeration returns an empty list and fd 0 stays open and unchanged.
 */
TEST(PortalgrabFdTest, FailedEnumerationKeepsFdZeroOpen) {
  unreachable_bus_guard_t bus_guard;

  // Make sure fd 0 is open so the test can observe it. Leave it open afterwards.
  if (::fcntl(0, F_GETFD) == -1) {
    const int fd = open_dev_null();
    ASSERT_EQ(fd, 0) << "Setup: /dev/null should have been opened on fd 0 when fd 0 was closed";
  }

  const auto before = identify_fd(0);
  ASSERT_TRUE(before.has_value()) << "Setup: fstat on fd 0 should succeed before the call";

  const auto names = platf::portal_display_names();

  EXPECT_TRUE(names.empty()) << "Unreachable portal: the enumeration should return an empty list";
  EXPECT_NE(::fcntl(0, F_GETFD), -1) << "Failed enumeration: fd 0 must still be open";
  EXPECT_TRUE(same_open_object(0, *before)) << "Failed enumeration: fd 0 must still refer to the same object (dev/ino)";
}

/**
 * @brief Repeated failed enumerations never close fd 0 or a test-owned descriptor.
 */
TEST(PortalgrabFdTest, RepeatedFailedEnumerationsKeepFdsOpen) {
  unreachable_bus_guard_t bus_guard;

  if (::fcntl(0, F_GETFD) == -1) {
    const int fd = open_dev_null();
    ASSERT_EQ(fd, 0) << "Setup: /dev/null should have been opened on fd 0 when fd 0 was closed";
  }
  const auto fd0_before = identify_fd(0);
  ASSERT_TRUE(fd0_before.has_value()) << "Setup: fstat on fd 0 should succeed before the calls";

  const int owned_fd = open_dev_null();
  ASSERT_GE(owned_fd, 0) << "Setup: opening /dev/null for the test-owned descriptor should succeed";
  const auto owned_before = identify_fd(owned_fd);
  ASSERT_TRUE(owned_before.has_value()) << "Setup: fstat on the test-owned descriptor should succeed";

  for (int i = 0; i < 3; ++i) {
    const auto names = platf::portal_display_names();
    EXPECT_TRUE(names.empty()) << "Call " << i << ": unreachable portal should return an empty list";
  }

  EXPECT_NE(::fcntl(0, F_GETFD), -1) << "After repeated calls: fd 0 must still be open";
  EXPECT_TRUE(same_open_object(0, *fd0_before)) << "After repeated calls: fd 0 must still refer to the same object";
  EXPECT_TRUE(same_open_object(owned_fd, *owned_before)) << "After repeated calls: the test-owned fd must still be open and unchanged";

  // Close the test-owned descriptor only if it is still the same object we opened.
  if (same_open_object(owned_fd, *owned_before)) {
    ::close(owned_fd);
  }
}
#endif
