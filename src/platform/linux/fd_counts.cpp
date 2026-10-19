/**
 * @file src/platform/linux/fd_counts.cpp
 * @brief Definitions for counting the process's open file descriptors by kind.
 */

// standard includes
#include <string>
#include <string_view>
#include <system_error>

// local includes
#include "fd_counts.h"

namespace platf {
  fd_counts_t count_open_fds(const std::filesystem::path &fd_dir) {
    fd_counts_t counts;

    std::error_code ec;
    std::filesystem::directory_iterator it(fd_dir, ec);
    if (ec) {
      return {};
    }

    for (const auto end = std::filesystem::directory_iterator(); it != end; it.increment(ec)) {
      if (ec) {
        break;
      }

      ++counts.total;

      std::error_code link_ec;
      const auto target = std::filesystem::read_symlink(it->path(), link_ec);
      if (link_ec) {
        continue;
      }

      const std::string name = target.string();
      const std::string_view link {name};
      if (link.starts_with("/dev/dri/renderD")) {
        ++counts.render_nodes;
      } else if (link.starts_with("/dmabuf:") || link.starts_with("anon_inode:dmabuf")) {
        ++counts.dmabufs;
      }
    }

    return counts;
  }
}  // namespace platf
