#pragma once

#include <cstdlib>
#include <limits.h>
#include <pwd.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace private_seat_test {
  inline bool within(const std::string &path, const std::string &root) {
    return path == root || (path.size() > root.size() &&
                            path.compare(0, root.size(), root) == 0 &&
                            path[root.size()] == '/');
  }

  // Reject all account home directories, including homes outside /home.
  inline bool is_home_directory(const std::string &path) {
    char own_home[PATH_MAX];
    const auto *own = ::getpwuid(::geteuid());
    if (own && own->pw_dir && ::realpath(own->pw_dir, own_home) && path == own_home) {
      return true;
    }
    bool found = false;
    ::setpwent();
    while (const auto *entry = ::getpwent()) {
      char resolved[PATH_MAX];
      if (entry->pw_dir && ::realpath(entry->pw_dir, resolved) && path == resolved) {
        found = true;
        break;
      }
    }
    ::endpwent();
    return found;
  }

  inline bool scratch_paths(std::string &root, std::string &tmp) {
    const char *root_env = std::getenv("SUNSHINE_PRIVATE_SEAT_TEST_ROOT");
    const char *tmp_env = std::getenv("TMPDIR");
    char resolved[PATH_MAX];
    struct stat st {};
    if (!root_env || root_env[0] != '/' || !::realpath(root_env, resolved)) {
      return false;
    }
    root = resolved;
    if (root != root_env || root == "/" || root == "/home" || root == "/root" || within(root, "/tmp") || within(root, "/dev/shm") || is_home_directory(root) || ::lstat(root_env, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
      return false;
    }
    if (!tmp_env || tmp_env[0] != '/' || !::realpath(tmp_env, resolved)) {
      return false;
    }
    tmp = resolved;
    return within(tmp, root) && ::stat(tmp.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  }
}  // namespace private_seat_test
