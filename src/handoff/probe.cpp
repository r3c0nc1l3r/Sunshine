/**
 * @file src/handoff/probe.cpp
 * @brief Implementation of `src/handoff/probe.h` (wave B).
 */
// standard includes
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

// local includes
#include "probe.h"
#include "src/config.h"
#include "src/video.h"

namespace handoff {
  namespace {
    /**
     * @brief Remove trailing whitespace and newlines from a string.
     *
     * @param text Text to trim in place.
     */
    void trim_trailing(std::string &text) {
      while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
      }
    }

    /**
     * @brief Parse a hexadecimal 32-bit id such as `0x10de` or `10de`.
     *
     * @param text Text to parse; it must be a complete hexadecimal number, with an optional `0x` prefix.
     * @return The parsed value, or 0 when the text is empty, not hexadecimal or out of range.
     */
    std::uint32_t parse_hex_id(std::string text) {
      trim_trailing(text);
      if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.erase(0, 2);
      }
      if (text.empty()) {
        return 0;
      }

      std::uint32_t value = 0;
      const char *begin = text.data();
      const char *end = text.data() + text.size();
      const auto [ptr, ec] = std::from_chars(begin, end, value, 16);
      if (ec != std::errc {} || ptr != end) {
        return 0;
      }
      return value;
    }

    /**
     * @brief Read a whole file as text, with trailing whitespace removed.
     *
     * @param path File to read.
     * @return The text, or `std::nullopt` when the file cannot be opened or read.
     */
    std::optional<std::string> read_file_text(const std::string &path) {
      std::ifstream file {path, std::ios::in | std::ios::binary};
      if (!file) {
        return std::nullopt;
      }

      std::ostringstream buffer;
      buffer << file.rdbuf();
      if (file.bad()) {
        return std::nullopt;
      }

      std::string text = buffer.str();
      trim_trailing(text);
      return text;
    }

    /**
     * @brief Final path component of a symbolic link's target.
     *
     * @param path Symbolic link to resolve.
     * @return The file name of the target, or `std::nullopt` when the link cannot be read or has no name.
     */
    std::optional<std::string> read_link_name(const std::string &path) {
      std::error_code ec;
      const auto target = std::filesystem::read_symlink(path, ec);
      if (ec) {
        return std::nullopt;
      }

      auto name = target.filename().string();
      if (name.empty()) {
        return std::nullopt;
      }
      return name;
    }

    /**
     * @brief Render node paths under `/dev/dri`, in ascending name order.
     *
     * @return Full paths of the `renderD*` entries that exist; empty when the directory cannot be read.
     */
    std::vector<std::string> list_render_node_paths() {
      std::vector<std::string> names;

      std::error_code ec;
      for (std::filesystem::directory_iterator it {"/dev/dri", ec}, end; !ec && it != end; it.increment(ec)) {
        auto name = it->path().filename().string();
        if (name.rfind("renderD", 0) == 0) {
          names.push_back(std::move(name));
        }
      }

      std::sort(names.begin(), names.end());
      for (auto &name : names) {
        name = "/dev/dri/" + name;
      }
      return names;
    }

    /**
     * @brief Current encoder probe results, read from the `video::` globals.
     *
     * @return The probe state as wire results.
     */
    protocol::probe_results_t current_probe_results() {
      protocol::probe_results_t results;
      results.hevc_mode = video::active_hevc_mode;
      results.av1_mode = video::active_av1_mode;
      results.yuv444_h264 = video::last_encoder_probe_supported_yuv444_for_codec[0];
      results.yuv444_hevc = video::last_encoder_probe_supported_yuv444_for_codec[1];
      results.yuv444_av1 = video::last_encoder_probe_supported_yuv444_for_codec[2];
      return results;
    }
  }  // namespace

  probe_env_t system_probe_env() {
    probe_env_t env;
    env.build_id = PROJECT_VERSION_COMMIT;
    env.configured_adapter = config::video.adapter_name;
    env.read_text = read_file_text;
    env.read_link_basename = read_link_name;
    env.list_render_nodes = list_render_node_paths;
    env.current_results = current_probe_results;
    return env;
  }

  probe_data_t gather_probe(const probe_env_t &env) {
    probe_data_t data;
    if (env.current_results) {
      data.results = env.current_results();
    }
    data.identity.build_id = env.build_id;

    // The render node: the configured adapter first, otherwise the first render node that exists.
    if (!env.configured_adapter.empty()) {
      data.identity.render_node = env.configured_adapter;
    } else if (env.list_render_nodes) {
      auto nodes = env.list_render_nodes();
      if (!nodes.empty()) {
        data.identity.render_node = nodes.front();
      }
    }

    // Vendor and device ids and the driver version come from sysfs for the node's basename.
    const auto node_path = data.identity.render_node;
    const auto slash = node_path.find_last_of('/');
    const auto node_name = slash == std::string::npos ? node_path : node_path.substr(slash + 1);
    if (!node_name.empty() && env.read_text) {
      const std::string device_dir = "/sys/class/drm/" + node_name + "/device/";
      if (auto vendor = env.read_text(device_dir + "vendor")) {
        data.identity.vendor_id = parse_hex_id(*vendor);
      }
      if (auto device = env.read_text(device_dir + "device")) {
        data.identity.device_id = parse_hex_id(*device);
      }

      // The driver version is only meaningful when both the driver name and its version are readable.
      if (env.read_link_basename) {
        if (auto driver = env.read_link_basename(device_dir + "driver"); driver && !driver->empty()) {
          if (auto version = env.read_text("/sys/module/" + *driver + "/version"); version && !version->empty()) {
            data.identity.driver_version = *version;
          }
        }
      }
    }

    return data;
  }

  std::optional<protocol::probe_results_t> accepted_probe(
    const protocol::hello_t &hello,
    const protocol::probe_identity_t &own
  ) {
    if (!hello.probe || !hello.identity) {
      return std::nullopt;
    }
    if (!(*hello.identity == own)) {
      return std::nullopt;
    }

    // Probe modes are 0 to 5 (the 4:4:4 variants are 4 and 5); anything else is rejected as defensive.
    const auto in_range = [](int mode) {
      return mode >= 0 && mode <= 5;
    };
    if (!in_range(hello.probe->hevc_mode) || !in_range(hello.probe->av1_mode)) {
      return std::nullopt;
    }

    return hello.probe;
  }
}  // namespace handoff
