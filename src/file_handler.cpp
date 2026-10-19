/**
 * @file file_handler.cpp
 * @brief Definitions for file handling functions.
 */

// standard includes
#include <cerrno>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <system_error>

#ifdef _WIN32
  #include <windows.h>
#else
  // lib includes
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

// local includes
#include "file_handler.h"
#include "logging.h"

namespace file_handler {
  namespace {
    /**
     * @brief Build a unique name for a temporary file next to the target file.
     *
     * @param target The file that will be replaced.
     * @return A file name of the form `<name>.tmp.<random hex>`.
     */
    std::string make_temp_name(const std::filesystem::path &target) {
      thread_local std::mt19937_64 rng {std::random_device {}()};
      return std::format("{}.tmp.{:016x}", target.filename().string(), rng());
    }

    /**
     * @brief Directory that holds the target file; `.` when the path has no parent component.
     *
     * @param target The file path.
     * @return The directory to create the temporary file in.
     */
    std::filesystem::path directory_of(const std::filesystem::path &target) {
      auto dir = target.parent_path();
      return dir.empty() ? std::filesystem::path {"."} : dir;
    }

#ifndef _WIN32
    /**
     * @brief Write every byte of a buffer to a file descriptor, retrying on EINTR and short writes.
     *
     * @param fd Open file descriptor.
     * @param data Bytes to write.
     * @return `0` on success, otherwise the errno of the failed `write`.
     */
    int write_all(int fd, std::string_view data) {
      while (!data.empty()) {
        const ssize_t written = ::write(fd, data.data(), data.size());
        if (written < 0) {
          if (errno == EINTR) {
            continue;
          }
          return errno;
        }
        data.remove_prefix(static_cast<std::size_t>(written));
      }
      return 0;
    }
#endif
  }  // namespace

  std::string get_parent_directory(const std::string &path) {
    // remove any trailing path separators
    std::string trimmed_path = path;
    while (!trimmed_path.empty() && trimmed_path.back() == '/') {
      trimmed_path.pop_back();
    }

    std::filesystem::path p(trimmed_path);
    return p.parent_path().string();
  }

  bool make_directory(const std::string &path) {
    // first, check if the directory already exists
    if (std::filesystem::exists(path)) {
      return true;
    }

    return std::filesystem::create_directories(path);
  }

  std::string read_file(const char *path) {
    if (!std::filesystem::exists(path)) {
      BOOST_LOG(debug) << "Missing file: " << path;
      return {};
    }

    std::ifstream in(path);
    return std::string {(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
  }

  int write_file(const char *path, const std::string_view &contents) {
    std::ofstream out(path);

    if (!out.is_open()) {
      return -1;
    }

    out << contents;

    return 0;
  }

  std::error_code write_file_atomic(const char *path, std::string_view contents) {
    const std::filesystem::path target {path};
    const auto dir = directory_of(target);

#ifdef _WIN32
    // Temporary file, then replace the target with MoveFileExW. Permission bits are not applied on Windows.
    constexpr DWORD max_chunk = 1U << 30;
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::filesystem::path tmp_path;
    for (int attempt = 0; attempt < 16 && handle == INVALID_HANDLE_VALUE; ++attempt) {
      tmp_path = dir / make_temp_name(target);
      handle = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (handle == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) {
        return {static_cast<int>(GetLastError()), std::system_category()};
      }
    }
    if (handle == INVALID_HANDLE_VALUE) {
      return {static_cast<int>(ERROR_FILE_EXISTS), std::system_category()};
    }

    const auto sys_error = []() {
      return std::error_code {static_cast<int>(GetLastError()), std::system_category()};
    };
    // Close the handle if it is still open, remove the temporary file and report the error.
    auto fail = [&](const std::error_code ec) {
      if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
      }
      DeleteFileW(tmp_path.c_str());
      return ec;
    };

    std::size_t offset = 0;
    while (offset < contents.size()) {
      const std::size_t remaining = contents.size() - offset;
      const DWORD chunk = remaining > max_chunk ? max_chunk : static_cast<DWORD>(remaining);
      DWORD written = 0;
      if (!WriteFile(handle, contents.data() + offset, chunk, &written, nullptr) || written == 0) {
        return fail(sys_error());
      }
      offset += written;
    }
    if (!FlushFileBuffers(handle)) {
      return fail(sys_error());
    }
    const bool closed = CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;
    if (!closed) {
      return fail(sys_error());
    }
    if (!MoveFileExW(tmp_path.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return fail(sys_error());
    }
    return {};
#else
    // The permission bits of an existing file (following a symlink to its target) are kept; otherwise 0600.
    mode_t mode = 0600;
    struct stat existing {};
    if (::stat(path, &existing) == 0) {
      mode = existing.st_mode & 07777;
    } else if (errno != ENOENT) {
      return {errno, std::generic_category()};
    }

    // Create the temporary file exclusively; a name collision retries with another random name.
    int fd = -1;
    std::filesystem::path tmp_path;
    for (int attempt = 0; attempt < 16 && fd < 0; ++attempt) {
      tmp_path = dir / make_temp_name(target);
      fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (fd < 0 && errno != EEXIST) {
        return {errno, std::generic_category()};
      }
    }
    if (fd < 0) {
      return {EEXIST, std::generic_category()};
    }

    // Remove the temporary file and report the error; `target` is never touched on failure.
    auto fail = [&](const int err) {
      ::unlink(tmp_path.c_str());
      return std::error_code {err, std::generic_category()};
    };

    if (::fchmod(fd, mode) != 0) {
      const int err = errno;
      ::close(fd);
      return fail(err);
    }
    if (const int err = write_all(fd, contents); err != 0) {
      ::close(fd);
      return fail(err);
    }
    if (::fsync(fd) != 0) {
      const int err = errno;
      ::close(fd);
      return fail(err);
    }
    if (::close(fd) != 0) {
      return fail(errno);
    }
    if (::rename(tmp_path.c_str(), path) != 0) {
      return fail(errno);
    }

    // Best effort: make the rename itself durable.
    if (const int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); dir_fd >= 0) {
      (void) ::fsync(dir_fd);
      ::close(dir_fd);
    }
    return {};
#endif
  }
}  // namespace file_handler
