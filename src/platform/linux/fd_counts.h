/**
 * @file src/platform/linux/fd_counts.h
 * @brief Declarations for counting the process's open file descriptors by kind.
 */
#pragma once

// standard includes
#include <cstddef>
#include <filesystem>

namespace platf {
  /**
   * @brief Open file descriptors of a process, by kind.
   */
  struct fd_counts_t {
    std::size_t total {0};  ///< All entries in the fd directory.
    std::size_t render_nodes {0};  ///< Entries linking to a DRM render node (`/dev/dri/renderD*`).
    std::size_t dmabufs {0};  ///< Entries linking to a dmabuf (`/dmabuf:*` or `anon_inode:dmabuf`).
  };

  /**
   * @brief Count open file descriptors by reading the symlinks in an fd directory.
   *
   * Entries whose link cannot be read count only towards `total`.
   *
   * @param fd_dir Directory with one symlink per descriptor, normally `/proc/self/fd`.
   * @return Counts; all zero when the directory cannot be read.
   */
  fd_counts_t count_open_fds(const std::filesystem::path &fd_dir = "/proc/self/fd");
}  // namespace platf
