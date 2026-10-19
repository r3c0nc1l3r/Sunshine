/**
 * @file src/upnp.cpp
 * @brief Definitions for UPnP port mapping.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stddef.h>  // workaround for type_t error in miniupnpc 2.3.3, see https://github.com/miniupnp/miniupnp/commit/e263ab6f56c382e10fed31347ec68095d691a0e8
#include <thread>

// lib includes
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>

// local includes
#include "config.h"
#include "confighttp.h"
#include "globals.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "rtsp.h"
#include "stream.h"
#include "upnp.h"
#include "utility.h"

using namespace std::literals;

namespace upnp {

  namespace {
    /**
     * @brief Process-wide UPnP state shared by `start()`, the shutdown guard and `remove_ports_async()`.
     */
    struct state_t {
      std::mutex mutex;  ///< Guards every field below. The unmap scope is atomic and needs no lock.
      bool started = false;  ///< `start()` has been called in this process; a second call is a programming error.
      bool planned = false;  ///< `start()` made an own plan with UPnP on; `remove_ports_async()` acts only then.
      std::vector<planned_mapping_t> own_plan;  ///< Mappings this process requested in `start()`; the second guard.
      std::deque<std::vector<planned_mapping_t>> jobs;  ///< Removal jobs waiting for the worker, in call order.
      bool worker_running = false;  ///< A removal worker thread is draining `jobs`.
      std::condition_variable idle;  ///< Notified when `worker_running` becomes false.
    };

    /**
     * @brief Access the shared UPnP state.
     * @return Reference to the process-wide state.
     */
    state_t &state() {
      // Intentionally never destroyed, so a detached removal worker never touches a destroyed mutex at exit.
      static auto *instance = new state_t;
      return *instance;
    }

    std::atomic<unmap_scope_e> g_unmap_scope {unmap_scope_e::all};  ///< Scope the guard applies when it is destroyed.

#ifdef SUNSHINE_TESTS
    /**
     * @brief The fake gateway of the test seam: an in-memory mapping store.
     */
    struct fake_igd_t {
      std::mutex mutex;  ///< Guards the fields below.
      bool installed = false;  ///< A fake gateway is installed.
      std::set<testing::fake_mapping_t> store;  ///< The mappings on the fake gateway.
    };

    /**
     * @brief Access the fake gateway.
     * @return Reference to the process-wide fake (never destroyed, like `state()`).
     */
    fake_igd_t &fake_igd() {
      static auto *instance = new fake_igd_t;
      return *instance;
    }

    /**
     * @brief Whether a fake gateway is installed.
     * @return `true` when installed.
     */
    bool fake_igd_installed() {
      auto &fake = fake_igd();
      std::lock_guard lock {fake.mutex};
      return fake.installed;
    }

    /**
     * @brief Add or refresh an entry on the fake gateway.
     * @param port Port.
     * @param udp `true` for UDP, `false` for TCP.
     */
    void fake_igd_map(std::uint16_t port, bool udp) {
      auto &fake = fake_igd();
      std::lock_guard lock {fake.mutex};
      if (fake.installed) {
        fake.store.insert({port, !udp});
      }
    }

    /**
     * @brief Delete an entry from the fake gateway; a missing entry counts as removed.
     * @param port Port.
     * @param udp `true` for UDP, `false` for TCP.
     */
    void fake_igd_unmap(std::uint16_t port, bool udp) {
      auto &fake = fake_igd();
      std::lock_guard lock {fake.mutex};
      fake.store.erase({port, !udp});
    }
#endif
  }  // namespace

  std::vector<planned_mapping_t> plan_mappings(int port, int udp_base, bool web_ui_on_wan) {
    // This function is pure and does not check the port range. The "Port out of range" warning is logged by
    // net::map_port and net::map_udp_port where the ports are bound, not here.
    // Same arithmetic as net::map_port (TCP, relative to `port`) and net::map_udp_port (UDP, relative to `udp_base`).
    auto tcp = [port](int offset) {
      return static_cast<std::uint16_t>(port + offset);
    };
    auto udp = [udp_base](int offset) {
      return static_cast<std::uint16_t>(udp_base + offset);
    };

    std::vector<planned_mapping_t> plan {
      {tcp(rtsp_stream::RTSP_SETUP_PORT), false, "Sunshine - RTSP"s},
      {udp(stream::VIDEO_STREAM_PORT), true, "Sunshine - Video"s},
      {udp(stream::AUDIO_STREAM_PORT), true, "Sunshine - Audio"s},
      {udp(stream::CONTROL_PORT), true, "Sunshine - Control"s},
      {tcp(nvhttp::PORT_HTTP), false, "Sunshine - Client HTTP"s},
      {tcp(nvhttp::PORT_HTTPS), false, "Sunshine - Client HTTPS"s},
    };

    // Only plan the Web Manager port if it may be reached from the WAN
    if (web_ui_on_wan) {
      plan.push_back({tcp(confighttp::PORT_HTTPS), false, "Sunshine - Web UI"s});
    }

    return plan;
  }

  std::vector<planned_mapping_t> select_for_scope(const std::vector<planned_mapping_t> &plan, unmap_scope_e scope) {
    std::vector<planned_mapping_t> selected;
    for (const auto &entry : plan) {
      // `all` keeps everything, `udp_only` keeps the UDP entries and `none` keeps nothing.
      if (scope == unmap_scope_e::all || (scope == unmap_scope_e::udp_only && entry.udp)) {
        selected.push_back(entry);
      }
    }
    return selected;
  }

  std::vector<planned_mapping_t> removal_plan(const std::set<std::uint16_t> &requested, const std::vector<planned_mapping_t> &own_plan) {
    std::vector<planned_mapping_t> removals;
    // `requested` is a set, so the entries come out in ascending port order.
    for (auto requested_port : requested) {
      // Guard 2: never select a port this process maps itself, whether it is UDP or TCP.
      auto is_own = std::any_of(own_plan.begin(), own_plan.end(), [requested_port](const planned_mapping_t &entry) {
        return entry.port == requested_port;
      });
      if (!is_own) {
        // The description is not sent to the gateway when a mapping is deleted, so it stays empty.
        removals.push_back({requested_port, true, std::string {}});
      }
    }
    return removals;
  }

  void set_unmap_scope(unmap_scope_e scope) {
    g_unmap_scope.store(scope);
  }

  unmap_scope_e unmap_scope() {
    return g_unmap_scope.load();
  }

  /**
   * @brief UPnP port mapping description and lease state.
   */
  struct mapping_t {
    struct {
      std::string wan;  ///< External gateway port exposed by the mapping.
      std::string lan;  ///< Internal Sunshine port targeted by the mapping.
      std::string proto;  ///< Transport protocol used by the mapping.
    } port;  ///< WAN/LAN/protocol tuple for the mapped port.

    std::string description;  ///< Human-readable UPnP lease description advertised to the gateway.
  };

  static std::string_view status_string(int status) {
    switch (status) {
      case 0:
        return "No IGD device found"sv;
      case 1:
        return "Valid IGD device found"sv;
      case 2:
        return "Valid IGD device found,  but it isn't connected"sv;
      case 3:
        return "A UPnP device has been found,  but it wasn't recognized as an IGD"sv;
    }

    return "Unknown status"sv;
  }

  /**
   * This function is a wrapper around UPNP_GetValidIGD() that returns the status code. There is a pre-processor
   * check to determine which version of the function to call based on the version of the MiniUPnPc library.
   */
  int UPNP_GetValidIGDStatus(device_t &device, urls_t *urls, IGDdatas *data, std::array<char, INET6_ADDRESS_STRLEN> &lan_addr) {
#if (MINIUPNPC_API_VERSION >= 18)
    return UPNP_GetValidIGD(device.get(), &urls->el, data, lan_addr.data(), (int) lan_addr.size(), nullptr, 0);
#else
    return UPNP_GetValidIGD(device.get(), &urls->el, data, lan_addr.data(), (int) lan_addr.size());
#endif
  }

  namespace {
    /**
     * @brief Convert a planned mapping into the form the gateway calls take.
     * @param entry The planned mapping.
     * @return The mapping with WAN and LAN ports set to the planned port.
     */
    mapping_t to_mapping(const planned_mapping_t &entry) {
      auto port = std::to_string(entry.port);
      return mapping_t {{port, port, entry.udp ? "UDP"s : "TCP"s}, entry.description};
    }
  }  // namespace

  /**
   * @brief RAII helper that runs shutdown cleanup when destroyed.
   */
  class deinit_t: public platf::deinit_t {
  public:
    /**
     * @brief Start the mapping thread for the given plan.
     * @param entries The mappings this process requests, as `plan_mappings` returns them.
     */
    explicit deinit_t(std::vector<planned_mapping_t> entries):
        plan {std::move(entries)} {
      for (const auto &entry : plan) {
        mappings.push_back(to_mapping(entry));
      }

      // Start the mapping thread
      upnp_thread = std::jthread {&deinit_t::upnp_thread_proc, this};
    }

    /**
     * @brief Destroy the UPnP deinitializer.
     */
    ~deinit_t() {
      upnp_thread.join();
    }

    /**
     * @brief Opens pinholes for IPv6 traffic if the IGD is capable.
     * @details Not many IGDs support this feature, so we perform error logging with debug level.
     * @return `true` if the pinholes were opened successfully.
     */
    bool create_ipv6_pinholes() {
      int err;
      device_t device {upnpDiscover(2000, nullptr, nullptr, 0, IPv6, 2, &err)};
      if (!device || err) {
        BOOST_LOG(debug) << "Couldn't discover any IPv6 UPNP devices"sv;
        return false;
      }

      IGDdatas data;
      urls_t urls;
      std::array<char, INET6_ADDRESS_STRLEN> lan_addr;
      auto status = upnp::UPNP_GetValidIGDStatus(device, &urls, &data, lan_addr);
      if (status != 1 && status != 2) {
        BOOST_LOG(debug) << "No valid IPv6 IGD: "sv << status_string(status);
        return false;
      }

      if (data.IPv6FC.controlurl[0] != 0) {
        int firewallEnabled;
        int pinholeAllowed;

        // Check if this firewall supports IPv6 pinholes
        err = UPNP_GetFirewallStatus(urls->controlURL_6FC, data.IPv6FC.servicetype, &firewallEnabled, &pinholeAllowed);
        if (err == UPNPCOMMAND_SUCCESS) {
          BOOST_LOG(debug) << "UPnP IPv6 firewall control available. Firewall is "sv
                           << (firewallEnabled ? "enabled"sv : "disabled"sv)
                           << ", pinhole is "sv
                           << (pinholeAllowed ? "allowed"sv : "disallowed"sv);

          if (pinholeAllowed) {
            // Create pinholes for each port
            auto mapping_period = std::to_string(PORT_MAPPING_LIFETIME.count());
            auto shutdown_event = mail::man->event<bool>(mail::shutdown);

            for (auto it = std::begin(mappings); it != std::end(mappings) && !shutdown_event->peek(); ++it) {
              auto mapping = *it;
              char uniqueId[8];

              // Open a pinhole for the LAN port, since there will be no WAN->LAN port mapping on IPv6
              err = UPNP_AddPinhole(urls->controlURL_6FC, data.IPv6FC.servicetype, "", "0", lan_addr.data(), mapping.port.lan.c_str(), mapping.port.proto.c_str(), mapping_period.c_str(), uniqueId);
              if (err == UPNPCOMMAND_SUCCESS) {
                BOOST_LOG(debug) << "Successfully created pinhole for "sv << mapping.port.proto << ' ' << mapping.port.lan;
              } else {
                BOOST_LOG(debug) << "Failed to create pinhole for "sv << mapping.port.proto << ' ' << mapping.port.lan << ": "sv << err;
              }
            }

            return err == 0;
          } else {
            BOOST_LOG(debug) << "IPv6 pinholes are not allowed by the IGD"sv;
            return false;
          }
        } else {
          BOOST_LOG(debug) << "Failed to get IPv6 firewall status: "sv << err;
          return false;
        }
      } else {
        BOOST_LOG(debug) << "IPv6 Firewall Control is not supported by the IGD"sv;
        return false;
      }
    }

    /**
     * @brief Maps a port via UPnP.
     * @param data IGDdatas from UPNP_GetValidIGD()
     * @param urls urls_t from UPNP_GetValidIGD()
     * @param lan_addr Local IP address to map to
     * @param mapping Information about port to map
     * @return `true` on success.
     */
    bool map_upnp_port(const IGDdatas &data, const urls_t &urls, const std::string &lan_addr, const mapping_t &mapping) {
      char intClient[16];
      char intPort[6];
      char desc[80];
      char enabled[4];
      char leaseDuration[16];
      bool indefinite = false;

      // First check if this port is already mapped successfully
      BOOST_LOG(debug) << "Checking for existing UPnP port mapping for "sv << mapping.port.wan;
      auto err = UPNP_GetSpecificPortMappingEntry(
        urls->controlURL,
        data.first.servicetype,
        // In params
        mapping.port.wan.c_str(),
        mapping.port.proto.c_str(),
        nullptr,
        // Out params
        intClient,
        intPort,
        desc,
        enabled,
        leaseDuration
      );
      if (err == 714) {  // NoSuchEntryInArray
        BOOST_LOG(debug) << "Mapping entry not found for "sv << mapping.port.wan;
      } else if (err == UPNPCOMMAND_SUCCESS) {
        // Some routers change the description, so we can't check that here
        if (!std::strcmp(intClient, lan_addr.c_str())) {
          if (std::atoi(leaseDuration) == 0) {
            BOOST_LOG(debug) << "Static mapping entry found for "sv << mapping.port.wan;

            // It's a static mapping, so we're done here
            return true;
          } else {
            BOOST_LOG(debug) << "Mapping entry found for "sv << mapping.port.wan << " ("sv << leaseDuration << " seconds remaining)"sv;
          }
        } else {
          BOOST_LOG(warning) << "UPnP conflict detected with: "sv << intClient;

          // Some UPnP IGDs won't let unauthenticated clients delete other conflicting port mappings
          // for security reasons, but we will give it a try anyway.
          err = UPNP_DeletePortMapping(
            urls->controlURL,
            data.first.servicetype,
            mapping.port.wan.c_str(),
            mapping.port.proto.c_str(),
            nullptr
          );
          if (err) {
            BOOST_LOG(error) << "Unable to delete conflicting UPnP port mapping: "sv << err;
            return false;
          }
        }
      } else {
        BOOST_LOG(error) << "UPNP_GetSpecificPortMappingEntry() failed: "sv << err;

        // If we get a strange error from the router, we'll assume it's some old broken IGDv1
        // device and only use indefinite lease durations to hopefully avoid confusing it.
        if (err != 606) {  // Unauthorized
          indefinite = true;
        }
      }

      // Add/update the port mapping
      auto mapping_period = std::to_string(indefinite ? 0 : PORT_MAPPING_LIFETIME.count());
      err = UPNP_AddPortMapping(
        urls->controlURL,
        data.first.servicetype,
        mapping.port.wan.c_str(),
        mapping.port.lan.c_str(),
        lan_addr.data(),
        mapping.description.c_str(),
        mapping.port.proto.c_str(),
        nullptr,
        mapping_period.c_str()
      );

      if (err != UPNPCOMMAND_SUCCESS && !indefinite) {
        // This may be an old/broken IGD that doesn't like non-static mappings.
        BOOST_LOG(debug) << "Trying static mapping after failure: "sv << err;
        err = UPNP_AddPortMapping(
          urls->controlURL,
          data.first.servicetype,
          mapping.port.wan.c_str(),
          mapping.port.lan.c_str(),
          lan_addr.data(),
          mapping.description.c_str(),
          mapping.port.proto.c_str(),
          nullptr,
          "0"
        );
      }

      if (err) {
        BOOST_LOG(error) << "Failed to map "sv << mapping.port.proto << ' ' << mapping.port.lan << ": "sv << err;
        return false;
      }

      BOOST_LOG(debug) << "Successfully mapped "sv << mapping.port.proto << ' ' << mapping.port.lan;
      return true;
    }

    /**
     * @brief Unmaps the ports that the current unmap scope selects.
     * @details The scope is read here, when the guard runs, so a draining process removes only its own UDP
     *          mappings (`udp_only`), or nothing (`none`). The TCP ports are shared with the active process.
     * @param urls urls_t from UPNP_GetValidIGD()
     * @param data IGDdatas from UPNP_GetValidIGD()
     */
    void unmap_all_upnp_ports(const urls_t &urls, const IGDdatas &data) {
      for (const auto &entry : select_for_scope(plan, unmap_scope())) {
        auto wan = std::to_string(entry.port);
        auto proto = entry.udp ? "UDP" : "TCP";
        auto status = UPNP_DeletePortMapping(
          urls->controlURL,
          data.first.servicetype,
          wan.c_str(),
          proto,
          nullptr
        );

        if (status && status != 714) {  // NoSuchEntryInArray
          BOOST_LOG(warning) << "Failed to unmap "sv << proto << ' ' << wan << ": "sv << status;
        } else {
          BOOST_LOG(debug) << "Successfully unmapped "sv << proto << ' ' << wan;
        }
      }
    }

    /**
     * @brief Maintains UPnP port forwarding rules
     */
    void upnp_thread_proc() {
      platf::set_thread_name("upnp");
      auto shutdown_event = mail::man->event<bool>(mail::shutdown);
      bool mapped = false;
      IGDdatas data;
      urls_t mapped_urls;
      auto address_family = net::get_effective_address_family(net::af_from_enum_string(config::sunshine.address_family));

      // Refresh UPnP rules every few minutes. They can be lost if the router reboots,
      // WAN IP address changes, or various other conditions.
      do {
#ifdef SUNSHINE_TESTS
        if (fake_igd_installed()) {
          // Test seam: the fake gateway is found at once and holds the mappings in memory.
          for (auto it = std::begin(plan); it != std::end(plan) && !shutdown_event->peek(); ++it) {
            fake_igd_map(it->port, it->udp);
          }
          mapped = true;
          continue;
        }
#endif
        int err = 0;
        device_t device {upnpDiscover(2000, nullptr, nullptr, 0, IPv4, 2, &err)};
        if (!device || err) {
          BOOST_LOG(warning) << "Couldn't discover any IPv4 UPNP devices"sv;
          mapped = false;
          continue;
        }

        for (auto dev = device.get(); dev != nullptr; dev = dev->pNext) {
          BOOST_LOG(debug) << "Found device: "sv << dev->descURL;
        }

        std::array<char, INET6_ADDRESS_STRLEN> lan_addr;

        urls_t urls;
        auto status = upnp::UPNP_GetValidIGDStatus(device, &urls, &data, lan_addr);
        if (status != 1 && status != 2) {
          BOOST_LOG(error) << status_string(status);
          mapped = false;
          continue;
        }

        std::string lan_addr_str {lan_addr.data()};

        BOOST_LOG(debug) << "Found valid IGD device: "sv << urls->rootdescURL;

        for (auto it = std::begin(mappings); it != std::end(mappings) && !shutdown_event->peek(); ++it) {
          map_upnp_port(data, urls, lan_addr_str, *it);
        }

        if (!mapped) {
          BOOST_LOG(info) << "Completed UPnP port mappings to "sv << lan_addr_str << " via "sv << urls->rootdescURL;
        }

        // If we are listening on IPv6 and the IGD has an IPv6 firewall enabled, try to create IPv6 firewall pinholes
        if (address_family == net::af_e::BOTH) {
          if (create_ipv6_pinholes() && !mapped) {
            // Only log the first time through
            BOOST_LOG(info) << "Successfully opened IPv6 pinholes on the IGD"sv;
          }
        }

        mapped = true;
        mapped_urls = std::move(urls);
      } while (!shutdown_event->view(REFRESH_INTERVAL));

      if (mapped) {
        // Give a queued background removal up to 3 s to finish, so it does not race this unmap. Wait only here.
        {
          auto &shared = state();
          std::unique_lock lock {shared.mutex};
          if (!shared.idle.wait_for(lock, 3s, [&shared] {
                return !shared.worker_running;
              })) {
            BOOST_LOG(warning) << "Background UPnP port removal still running after 3 s; unmapping anyway"sv;
          }
        }

#ifdef SUNSHINE_TESTS
        if (fake_igd_installed()) {
          // Test seam: remove what the current scope selects from the fake gateway.
          for (const auto &entry : select_for_scope(plan, unmap_scope())) {
            fake_igd_unmap(entry.port, entry.udp);
          }
          return;
        }
#endif

        // Unmap ports upon termination
        BOOST_LOG(info) << "Unmapping UPNP ports..."sv;
        unmap_all_upnp_ports(mapped_urls, data);
      }
    }

    std::vector<planned_mapping_t> plan;  ///< Planned mappings, kept in plan form for the unmap scope.
    std::vector<mapping_t> mappings;  ///< Port mappings Sunshine should keep registered with the gateway.
    std::jthread upnp_thread;  ///< Worker thread that refreshes mappings until shutdown.
  };

  namespace {
    /**
     * @brief Delete the given UDP mappings on the gateway. Runs on the removal worker.
     * @details Discovers the gateway as the mapping thread does. "No such entry" counts as removed; any other failure
     *          is logged as a warning, with no retry.
     * @param entries The UDP mappings to delete, from `removal_plan`.
     */
    void remove_entries(const std::vector<planned_mapping_t> &entries) {
#ifdef SUNSHINE_TESTS
      if (fake_igd_installed()) {
        // Test seam: the fake gateway has no other LAN host, so every planned UDP entry is deleted.
        for (const auto &entry : entries) {
          fake_igd_unmap(entry.port, entry.udp);
        }
        return;
      }
#endif
      int err = 0;
      device_t device {upnpDiscover(2000, nullptr, nullptr, 0, IPv4, 2, &err)};
      if (!device || err) {
        BOOST_LOG(warning) << "Couldn't discover any IPv4 UPNP devices to remove ports from"sv;
        return;
      }

      IGDdatas data;
      urls_t urls;
      std::array<char, INET6_ADDRESS_STRLEN> lan_addr;
      auto status = UPNP_GetValidIGDStatus(device, &urls, &data, lan_addr);
      if (status != 1 && status != 2) {
        BOOST_LOG(warning) << "No valid IGD to remove UPnP ports from: "sv << status_string(status);
        return;
      }

      for (const auto &entry : entries) {
        auto wan = std::to_string(entry.port);

        // Guard 3: look the mapping up first and delete it only when this host owns it. A UDP mapping that
        // another LAN host holds is left alone.
        char intClient[64];
        char intPort[16];
        char desc[80];
        char enabled[4];
        char leaseDuration[16];
        auto lookup_err = UPNP_GetSpecificPortMappingEntry(
          urls->controlURL,
          data.first.servicetype,
          wan.c_str(),
          "UDP",
          nullptr,
          intClient,
          intPort,
          desc,
          enabled,
          leaseDuration
        );
        if (lookup_err == 714) {  // NoSuchEntryInArray: nothing to remove
          BOOST_LOG(debug) << "No UDP mapping to remove for "sv << wan;
          continue;
        }
        if (lookup_err != UPNPCOMMAND_SUCCESS) {
          BOOST_LOG(warning) << "Skipping removal of UDP "sv << wan << ": lookup failed: "sv << lookup_err;
          continue;
        }
        if (std::strcmp(intClient, lan_addr.data()) != 0) {
          BOOST_LOG(warning) << "Not removing UDP "sv << wan << ": it is mapped to another LAN host"sv;
          continue;
        }

        auto del_err = UPNP_DeletePortMapping(urls->controlURL, data.first.servicetype, wan.c_str(), "UDP", nullptr);
        if (del_err && del_err != 714) {  // NoSuchEntryInArray
          BOOST_LOG(warning) << "Failed to remove UDP "sv << wan << ": "sv << del_err;
        } else {
          BOOST_LOG(debug) << "Removed UDP "sv << wan;
        }
      }
    }

    /**
     * @brief Run one removal job, and contain any exception so it never reaches std::terminate.
     * @param job The UDP mappings to delete.
     */
    void run_job(const std::vector<planned_mapping_t> &job) {
      try {
        remove_entries(job);
      } catch (const std::exception &e) {
        try {
          BOOST_LOG(warning) << "UPnP port removal failed: "sv << e.what();
        } catch (...) {
        }
      } catch (...) {
        try {
          BOOST_LOG(warning) << "UPnP port removal failed"sv;
        } catch (...) {
        }
      }
    }

    /**
     * @brief Worker loop for `remove_ports_async`: runs queued jobs in call order, then exits when the queue is empty.
     */
    void removal_worker() {
      platf::set_thread_name("upnp_remove");
      auto &shared = state();
      while (true) {
        std::vector<planned_mapping_t> job;
        {
          std::lock_guard lock {shared.mutex};
          if (shared.jobs.empty()) {
            shared.worker_running = false;
            shared.idle.notify_all();
            return;
          }
          job = std::move(shared.jobs.front());
          shared.jobs.pop_front();
        }
        run_job(job);
      }
    }
  }  // namespace

  std::unique_ptr<platf::deinit_t> start(int udp_base) {
    auto &shared = state();
    {
      std::lock_guard lock {shared.mutex};
      if (shared.started) {
        BOOST_LOG(error) << "upnp::start() called twice in one process; the second call maps nothing"sv;
        return nullptr;
      }
      shared.started = true;
    }

    if (!config::sunshine.flags[config::flag::UPNP]) {
      return nullptr;
    }

    // Only plan the Web Manager port if it may be reached from the WAN
    auto web_ui_on_wan = net::from_enum_string(config::nvhttp.origin_web_ui_allowed) > net::LAN;
    auto plan = plan_mappings(config::sunshine.port, udp_base, web_ui_on_wan);

    {
      // This plan is the own_plan that `remove_ports_async` checks against, so it never removes these mappings.
      std::lock_guard lock {shared.mutex};
      shared.own_plan = plan;
      shared.planned = true;
    }

    return std::make_unique<deinit_t>(std::move(plan));
  }

  void remove_ports_async(std::set<std::uint16_t> ports) {
    auto &shared = state();
    std::lock_guard lock {shared.mutex};

    // Without a plan from `start()` (UPnP off, or not started here) there is no guard to check, so do nothing.
    if (!shared.planned) {
      return;
    }

    // Guard 2: `removal_plan` drops every port this process maps and keeps only UDP entries.
    auto entries = removal_plan(ports, shared.own_plan);
    if (entries.empty()) {
      return;
    }

    shared.jobs.push_back(std::move(entries));
    if (!shared.worker_running) {
      shared.worker_running = true;
      try {
        std::thread {removal_worker}.detach();
      } catch (const std::exception &e) {
        // The worker never started, so this job is the only one queued: drop it and clear the flag.
        shared.worker_running = false;
        shared.jobs.pop_back();
        BOOST_LOG(warning) << "Could not start the UPnP port removal worker, dropping the job: "sv << e.what();
      }
    }
  }

#ifdef SUNSHINE_TESTS
  namespace testing {
    void install_fake_igd(const std::set<fake_mapping_t> &initial) {
      auto &fake = fake_igd();
      std::lock_guard lock {fake.mutex};
      fake.installed = true;
      fake.store = initial;
    }

    void remove_fake_igd() {
      auto &fake = fake_igd();
      std::lock_guard lock {fake.mutex};
      fake.installed = false;
      fake.store.clear();
    }

    void fake_igd_add(const std::set<fake_mapping_t> &entries) {
      auto &fake = fake_igd();
      std::lock_guard lock {fake.mutex};
      if (fake.installed) {
        fake.store.insert(entries.begin(), entries.end());
      }
    }

    std::set<fake_mapping_t> fake_igd_mappings() {
      auto &fake = fake_igd();
      std::lock_guard lock {fake.mutex};
      return fake.installed ? fake.store : std::set<fake_mapping_t> {};
    }

    bool wait_idle_for_test(std::chrono::milliseconds timeout) {
      auto &shared = state();
      std::unique_lock lock {shared.mutex};
      return shared.idle.wait_for(lock, timeout, [&shared] {
        return !shared.worker_running;
      });
    }

    void reset_upnp_for_test() {
      wait_idle_for_test(5s);
      auto &shared = state();
      {
        std::lock_guard lock {shared.mutex};
        shared.started = false;
        shared.planned = false;
        shared.own_plan.clear();
        shared.jobs.clear();
      }
      set_unmap_scope(unmap_scope_e::all);
    }
  }  // namespace testing
#endif
}  // namespace upnp
