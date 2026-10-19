/**
 * @file file_handler.h
 * @brief Declarations for file handling functions.
 */
#pragma once

// standard includes
#include <string>
#include <string_view>
#include <system_error>

/**
 * @brief Responsible for file handling functions.
 */
namespace file_handler {
  /**
   * @brief Get the parent directory of a file or directory.
   * @param path The path of the file or directory.
   * @return The parent directory.
   * @examples
   * std::string parent_dir = get_parent_directory("path/to/file");
   * @examples_end
   */
  std::string get_parent_directory(const std::string &path);

  /**
   * @brief Make a directory.
   * @param path The path of the directory.
   * @return `true` on success, `false` on failure.
   * @examples
   * bool dir_created = make_directory("path/to/directory");
   * @examples_end
   */
  bool make_directory(const std::string &path);

  /**
   * @brief Read a file to string.
   * @param path The path of the file.
   * @return The contents of the file.
   * @examples
   * std::string contents = read_file("path/to/file");
   * @examples_end
   */
  std::string read_file(const char *path);

  /**
   * @brief Writes a file.
   * @param path The path of the file.
   * @param contents The contents to write.
   * @return ``0`` on success, ``-1`` on failure.
   * @examples
   * int write_status = write_file("path/to/file", "file contents");
   * @examples_end
   */
  int write_file(const char *path, const std::string_view &contents);

  /**
   * @brief Replace a file atomically: a reader sees the old content or the new content, never a partial file.
   *
   * Used only for the state file (`sunshine_state.json`) and `apps.json` (drain handoff, "Writes use a temporary
   * file and rename"); `write_file` stays the in-place writer for every other file.
   *
   * Steps: create a temporary file in the same directory as `path` with a unique name (`<name>.tmp.<random>`, opened
   * with `O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC`, mode 0600 or, when `path` exists, the same permission bits as the
   * existing file), write all of `contents`, `fsync` it, close it, `rename` it over `path`, then `fsync` the directory
   * (best effort). On any failure after the temporary file was created, it is unlinked and `path` is untouched. The
   * parent directory must already exist; it is not created. A symlink at `path` is replaced by the new file, not
   * followed. Thread-safe as long as callers do not write the same path concurrently (the callers hold their own
   * locks).
   *
   * @param path The path of the file.
   * @param contents The contents to write.
   * @return A cleared error code on success; otherwise the `errno` of the step that failed.
   * @examples
   * if (auto ec = write_file_atomic("path/to/file", "file contents")) {
   *   // handle ec
   * }
   * @examples_end
   */
  std::error_code write_file_atomic(const char *path, std::string_view contents);
}  // namespace file_handler
