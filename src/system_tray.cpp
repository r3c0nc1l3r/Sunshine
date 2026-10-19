/**
 * @file src/system_tray.cpp
 * @brief Definitions for the system tray icon and notification system.
 */
// macros
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1

  /**
   * @def TRAY_ICON
   * @brief Path to the default system tray icon.
   */
  #define TRAY_ICON WEB_DIR "images/logo-sunshine.svg"
  /**
   * @def TRAY_ICON_PLAYING
   * @brief Path to the system tray icon used while streaming.
   */
  #define TRAY_ICON_PLAYING WEB_DIR "images/sunshine-playing.svg"
  /**
   * @def TRAY_ICON_PAUSING
   * @brief Path to the system tray icon used while streaming is paused.
   */
  #define TRAY_ICON_PAUSING WEB_DIR "images/sunshine-pausing.svg"
  /**
   * @def TRAY_ICON_LOCKED
   * @brief Path to the system tray icon used for pairing requests.
   */
  #define TRAY_ICON_LOCKED WEB_DIR "images/sunshine-locked.svg"
  /**
   * @def TRAY_ICON_VIRTUALHID
   * @brief Path to the Virtual HID Driver notification icon.
   */
  #define TRAY_ICON_VIRTUALHID WEB_DIR "images/logo-libvirtualhid.svg"

  #if defined(_WIN32)
    /**
     * @def WIN32_LEAN_AND_MEAN
     * @brief Macro for WIN32 LEAN AND MEAN.
     */
    #define WIN32_LEAN_AND_MEAN
    #include <AccCtrl.h>
    #include <AclAPI.h>
  #elif defined(__APPLE__) || defined(__MACH__)
    #include <CoreFoundation/CoreFoundation.h>
    #include <dispatch/dispatch.h>
  #endif

  // standard includes
  #include <array>
  #include <atomic>
  #include <chrono>
  #include <condition_variable>
  #include <csignal>
  #include <cstddef>
  #include <filesystem>
  #include <format>
  #include <functional>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <string_view>
  #include <system_error>
  #include <thread>
  #include <unordered_map>
  #include <utility>

  // lib includes
  #include <boost/filesystem.hpp>
  #include <boost/process/v1/environment.hpp>
  #include <tray.h>
  #ifdef _WIN32
    #include <libvirtualhid/license.hpp>
  #endif

  // local includes
  #include "config.h"
  #include "confighttp.h"
  #include "display_device.h"
  #include "logging.h"
  #include "platform/common.h"
  #include "process.h"
  #include "src/entry_handler.h"
  #include "src/handoff/hooks.h"
  #include "system_tray.h"
  #ifdef _WIN32
    #include "platform/windows/utf_utils.h"
  #endif

using namespace std::literals;

// system_tray namespace
namespace system_tray {
  namespace {
    /**
     * @brief Access the process-wide tray initialization state.
     *
     * @return Atomic initialization flag shared by tray operations.
     */
    std::atomic_bool &tray_initialized_state() {
      static std::atomic_bool initialized {false};
      return initialized;
    }

    /**
     * @brief Access the mutex that serializes tray data and UI changes.
     *
     * @return Mutex shared by tray operations.
     */
    std::mutex &tray_state_mutex() {
      static std::mutex mutex;
      return mutex;
    }

    /**
     * @brief Access the owned tray worker thread.
     *
     * @return Worker thread used by the threaded tray lifecycle.
     */
    std::jthread &tray_worker_thread() {
      static std::jthread worker;
      return worker;
    }

    /**
     * @brief Pending command of the main-thread tray loop (a single slot; the last command wins).
     */
    enum class tray_command_e {
      none,  ///< Nothing pending.
      start,  ///< Show the icon for the first time.
      resume,  ///< Show the icon again after a suspend.
      suspend,  ///< Remove the icon, keeping the process.
    };

    /**
     * @brief The tray lifecycle: phase, command slot and the backend entry points.
     *
     * Guarded by `mutex`. This mutex is never held across a backend call, and never while taking
     * `tray_state_mutex()`; the loop thread takes it for short sections only.
     */
    struct lifecycle_t {
      std::mutex mutex;  ///< Guards every member below.
      std::condition_variable cv;  ///< Wakes the loop (new command, end) and the waiters (phase change).
      tray_phase_e phase {tray_phase_e::not_started};  ///< Lifecycle phase.
      tray_command_e command {tray_command_e::none};  ///< The pending command.
      bool resume_after_suspend {false};  ///< A start/resume arrived while a suspend was in flight; run it after.
      bool ended {false};  ///< `end_tray` ran.
      bool loop_entered {false};  ///< `run_main_loop` was entered.
      bool loop_exited {false};  ///< `run_main_loop` returned.
      std::thread::id loop_thread;  ///< The thread that runs `run_main_loop`.
      bool enabled_for_testing {false};  ///< Count the tray as enabled (tests).
      tray_backend_t backend;  ///< Backend entry points; null members take the production values.
    };

    /**
     * @brief Access the process-wide tray lifecycle.
     *
     * @return The lifecycle.
     */
    lifecycle_t &lifecycle() {
      static lifecycle_t state;
      return state;
    }

    std::atomic_bool lifecycle_entries_enabled {true};  ///< Whether the Quit and Restart entries can be chosen.

  #ifdef __linux__
    /**
     * @brief Production backend `exit`: ask the library's loop to return and remove the icon.
     */
    void backend_exit_default() {
      tray_exit();
    }

    /**
     * @brief The backend entry points with the production defaults filled in.
     *
     * Call it with the lifecycle mutex held; use the returned copy after releasing the mutex.
     *
     * @return The entry points.
     */
    tray_backend_t effective_backend_locked() {
      tray_backend_t backend = lifecycle().backend;
      if (!backend.init) {
        backend.init = &init_tray;
      }
      if (!backend.loop) {
        backend.loop = &process_tray_events;
      }
      if (!backend.exit) {
        backend.exit = &backend_exit_default;
      }
      return backend;
    }
  #endif

    /**
     * @brief Hash string-like resource paths without allocating temporary strings.
     */
    struct transparent_string_hash_t {
      using is_transparent = void;  ///< Enable heterogeneous unordered-map lookup.

      /**
       * @brief Hash a string view.
       *
       * @param value String view to hash.
       * @return Hash value for the supplied text.
       */
      std::size_t operator()(const std::string_view value) const noexcept {
        return std::hash<std::string_view> {}(value);
      }
    };

  #ifdef _WIN32
    /**
     * @brief Access storage for dynamic Virtual HID Driver license menu labels.
     *
     * @return Persistent string storage backing the tray menu label pointers.
     */
    std::array<std::string, 9> &virtualhid_license_menu_text_storage() {
      static std::array<std::string, 9> menu_text;
      return menu_text;
    }

    /**
     * @brief Access storage for the Virtual HID Driver compatibility notification.
     *
     * @return Persistent string storage backing the tray notification pointer.
     */
    std::string &virtualhid_driver_notification_text_storage() {
      static std::string notification_text;
      return notification_text;
    }

    /**
     * @brief Access persistent storage for the Virtual HID Driver benefits menu.
     *
     * The tray C API requires a mutable submenu pointer even though Sunshine
     * treats these entries as immutable.
     *
     * @return Persistent Virtual HID Driver benefits menu storage.
     */
    std::array<struct tray_menu, 5> &virtualhid_benefits_menu_storage() {
      static std::array<struct tray_menu, 5> benefits_menu {{
        {.text = "Xbox One, Xbox Series, DualSense (DS5), Switch Pro, and Generic", .disabled = 1},
        {.text = "Raw Input keyboard and mouse for physical-style input", .disabled = 1},
        {.text = "Motion, touchpads, LEDs, and adaptive triggers where supported", .disabled = 1},
        {.text = "Actively developed and supported by LizardByte", .disabled = 1},
        {},
      }};
      return benefits_menu;
    }
  #endif
  }  // namespace

  #ifdef _WIN32
  constexpr auto LIBVIRTUALHID_RELEASES_URL = "https://github.com/LizardByte/libvirtualhid/releases/latest"sv;  ///< Latest Virtual HID Driver release.
  #endif

  void tray_open_ui_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Opening UI from system tray"sv;
    launch_ui();
  }

  void tray_donate_github_cb([[maybe_unused]] struct tray_menu *item) {
    platf::open_url("https://github.com/sponsors/LizardByte");
  }

  void tray_donate_patreon_cb([[maybe_unused]] struct tray_menu *item) {
    platf::open_url("https://www.patreon.com/LizardByte");
  }

  void tray_donate_paypal_cb([[maybe_unused]] struct tray_menu *item) {
    platf::open_url("https://www.paypal.com/paypalme/ReenigneArcher");
  }

  #ifdef _WIN32
  void tray_virtualhid_license_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Opening Virtual HID Driver license settings from system tray"sv;
    launch_ui(config::input.gamepad_driver == config::GAMEPAD_DRIVER_VIGEMBUS ? "/config#gamepad_driver" : "/troubleshooting#virtualhid-license");
  }

  void tray_virtualhid_download_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Opening Virtual HID Driver download from system tray"sv;
    platf::open_url(std::string {LIBVIRTUALHID_RELEASES_URL});
  }
  #endif

  /**
   * @brief Forwards Qt log messages to Sunshine's BOOST_LOG logger.
   * @param level Log level: 0=debug, 1=info, 2=warning, 3=error.
   * @param msg The message string from Qt.
   */
  static void qt_log_to_boost(int level, const char *msg) {
    if (msg == nullptr) {
      return;
    }
    switch (level) {
      case 0:
        BOOST_LOG(debug) << "Qt: " << msg;
        break;
      case 1:
        BOOST_LOG(info) << "Qt: " << msg;
        break;
      case 2:
        BOOST_LOG(warning) << "Qt: " << msg;
        break;
      default:
        BOOST_LOG(error) << "Qt: " << msg;
        break;
    }
  }

  void tray_reset_display_device_config_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Resetting display device config from system tray"sv;

    std::ignore = display_device::reset_persistence();
  }

  void tray_restart_cb([[maybe_unused]] struct tray_menu *item) {
    if (const auto blocked = handoff::hooks::restart_blocked(); !blocked.empty()) {
      BOOST_LOG(warning) << "Restart from the system tray refused: "sv << blocked;
      return;
    }

    BOOST_LOG(info) << "Restarting from system tray"sv;

    platf::restart();
  }

  void tray_quit_cb([[maybe_unused]] struct tray_menu *item) {
    if (const auto blocked = handoff::hooks::quit_blocked(); !blocked.empty()) {
      BOOST_LOG(warning) << "Quit from the system tray refused: "sv << blocked;
      return;
    }

    BOOST_LOG(info) << "Quitting from system tray"sv;

  #ifdef _WIN32
    // If we're running in a service, return a special status to
    // tell it to terminate too, otherwise it will just respawn us.
    if (GetConsoleWindow() == nullptr) {
      lifetime::exit_sunshine(ERROR_SHUTDOWN_IN_PROGRESS, true);
      return;
    }
  #endif

    lifetime::exit_sunshine(0, true);
  }

  #ifdef _WIN32
  /**
   * @brief Create the initial Virtual HID Driver license submenu.
   *
   * @return Menu storage with a checking state, benefits, license settings, and driver download.
   */
  std::array<struct tray_menu, 9> initial_virtualhid_license_menu() {
    std::array<struct tray_menu, 9> menu {};
    menu[0] = {.text = "Status: Checking", .disabled = 1};
    menu[1] = {.text = "-"};
    menu[2] = {.text = "Get/Manage License", .cb = tray_virtualhid_license_cb};
    menu[3] = {.text = "Virtual HID Driver Benefits", .submenu = virtualhid_benefits_menu_storage().data()};
    menu[4] = {.text = "Download Virtual HID Driver", .cb = tray_virtualhid_download_cb};
    return menu;
  }

  static auto virtualhid_license_menu = initial_virtualhid_license_menu();  ///< Virtual HID Driver license submenu.
  #endif

  // Tray menu
  static struct tray tray = {
    .icon = TRAY_ICON,
    .tooltip = PROJECT_NAME,
    .menu =
      (struct tray_menu[]) {
        // Tray menu labels currently use the project's English source strings.
        {.text = "Open Sunshine", .cb = tray_open_ui_cb},
        {.text = "-"},
  #ifdef _WIN32
        {.text = "Virtual HID Driver", .submenu = virtualhid_license_menu.data()},
        {.text = "-"},
  #endif
        {.text = "Donate",
         .submenu =
           (struct tray_menu[]) {
             {.text = "GitHub Sponsors", .cb = tray_donate_github_cb},
             {.text = "Patreon", .cb = tray_donate_patreon_cb},
             {.text = "PayPal", .cb = tray_donate_paypal_cb},
             {.text = nullptr}
           }},
        {.text = "-"},
  // Currently display device settings are only supported on Windows
  #ifdef _WIN32
        {.text = "Reset Display Device Config", .cb = tray_reset_display_device_config_cb},
  #endif
        {.text = "Restart", .cb = tray_restart_cb},
        {.text = "Quit", .cb = tray_quit_cb},
        {.text = nullptr}
      },
  #ifdef _WIN32
    .iconPathCount = 5,
    .allIconPaths = {TRAY_ICON, TRAY_ICON_LOCKED, TRAY_ICON_PLAYING, TRAY_ICON_PAUSING, TRAY_ICON_VIRTUALHID},
  #else
    .iconPathCount = 4,
    .allIconPaths = {TRAY_ICON, TRAY_ICON_LOCKED, TRAY_ICON_PLAYING, TRAY_ICON_PAUSING},
  #endif
  };

  #ifdef SUNSHINE_TESTS
  const struct tray &tray_data_for_testing() {
    return tray;
  }

  bool tray_initialized_for_testing() {
    return tray_initialized_state().load();
  }

  void reset_tray_data_for_testing() {
    const std::scoped_lock lock(tray_state_mutex());
    tray.icon = tray.allIconPaths[0];
    tray.tooltip = PROJECT_NAME;
    tray.notification_icon = nullptr;
    tray.notification_text = nullptr;
    tray.notification_title = nullptr;
    tray.notification_cb = nullptr;
    #ifdef _WIN32
    virtualhid_license_menu_text_storage() = {};
    virtualhid_driver_notification_text_storage().clear();
    virtualhid_license_menu = initial_virtualhid_license_menu();
    #endif
  }
  #endif

  #ifdef _WIN32
  /**
   * @brief Return the user-visible label for a Virtual HID Driver license state.
   *
   * @param state License state reported by libvirtualhid.
   * @return Short state label suitable for a tray menu.
   */
  std::string_view virtualhid_license_state_label(lvh::LicenseState state) {
    using enum lvh::LicenseState;

    switch (state) {
      case unlicensed:
        return "Not Activated";
      case licensed:
        return "Licensed";
      case expired:
        return "Expired";
      case disabled:
        return "Disabled";
      case invalid:
        return "Invalid";
      case unavailable:
      default:
        return "Unavailable";
    }
  }

  /**
   * @brief Return explanatory tray text for a non-active license state.
   *
   * @param state License state reported by libvirtualhid.
   * @return Short explanation of the state.
   */
  std::string_view virtualhid_license_state_detail(lvh::LicenseState state) {
    using enum lvh::LicenseState;

    switch (state) {
      case unlicensed:
        return "No license is active on this machine";
      case expired:
        return "The license on this machine has expired";
      case disabled:
        return "The license on this machine is disabled";
      case invalid:
        return "The license on this machine is invalid";
      case licensed:
        return "This machine is activated";
      case unavailable:
      default:
        return "The local license service is unavailable";
    }
  }

  /**
   * @brief Assign text and behavior to one Virtual HID Driver submenu item.
   *
   * @tparam Callback Callback type accepted by the tray library.
   * @param index Submenu index to populate.
   * @param text User-visible menu text.
   * @param disabled Whether the item is informational rather than actionable.
   * @param callback Callback invoked for actionable items.
   */
  template<typename Callback = std::nullptr_t>
  void set_virtualhid_license_menu_item(
    const std::size_t index,
    std::string text,
    const bool disabled,
    Callback callback = nullptr
  ) {
    auto &menu_text = virtualhid_license_menu_text_storage();
    menu_text[index] = std::move(text);
    virtualhid_license_menu[index] = {
      .text = menu_text[index].c_str(),
      .disabled = disabled,
      .cb = callback,
    };
  }

  /**
   * @brief Rebuild the Virtual HID Driver submenu for the latest license state.
   *
   * @param license Latest machine license details.
   */
  void rebuild_virtualhid_license_menu(const lvh::LicenseStatus &license) {
    virtualhid_license_menu = {};
    virtualhid_license_menu_text_storage() = {};

    set_virtualhid_license_menu_item(0, std::format("Status: {}", virtualhid_license_state_label(license.state)), true);
    if (license.licensed()) {
      set_virtualhid_license_menu_item(
        1,
        license.plan_name.empty() ? std::string {virtualhid_license_state_detail(license.state)} : std::format("Plan: {}", license.plan_name),
        true
      );
      set_virtualhid_license_menu_item(
        2,
        license.customer_email.empty() ? "Customer: Not reported" : std::format("Customer: {}", license.customer_email),
        true
      );
      set_virtualhid_license_menu_item(
        3,
        license.activation_limit == 0 ?
          "Machine activations: Not reported" :
          std::format("Machine activations: {} / {}", license.activation_usage, license.activation_limit),
        true
      );
    } else {
      set_virtualhid_license_menu_item(1, std::string {virtualhid_license_state_detail(license.state)}, true);
      set_virtualhid_license_menu_item(2, "Driver-backed keyboard, mouse, and gamepads are locked", true);
      set_virtualhid_license_menu_item(
        3,
        license.service_available ? "License service: Available" : "License service: Unavailable",
        true
      );
    }
    virtualhid_license_menu[4] = {.text = "-"};
    set_virtualhid_license_menu_item(5, "Get/Manage License", false, tray_virtualhid_license_cb);
    set_virtualhid_license_menu_item(6, "Virtual HID Driver Benefits", false);
    virtualhid_license_menu[6].submenu = virtualhid_benefits_menu_storage().data();
    set_virtualhid_license_menu_item(7, "Download Virtual HID Driver", false, tray_virtualhid_download_cb);
  }

  /**
   * @brief Clear notification fields before replacing the tray state.
   */
  void clear_tray_notification() {
    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
  }

  void update_tray_virtualhid_license(const lvh::LicenseStatus &license, const bool notify_if_unlicensed) {
    const std::scoped_lock lock(tray_state_mutex());
    clear_tray_notification();
    rebuild_virtualhid_license_menu(license);

    if (config::input.gamepad_driver.empty()) {
      tray.notification_title = "Choose a Gamepad Driver";
      tray.notification_text =
        "Choose a driver in Input settings. Virtual HID Driver is a paid upgrade; ViGEmBus is limited and has reached end of life.";
      tray.notification_icon = tray.allIconPaths[4];
      tray.notification_cb = []() {
        launch_ui("/config#gamepad_driver");
      };
    } else if (config::input.gamepad_driver != config::GAMEPAD_DRIVER_VIGEMBUS && notify_if_unlicensed && !license.licensed()) {
      tray.notification_title = "Virtual HID Driver License";
      tray.notification_text =
        "Get or manage a license, or use the limited, end-of-life ViGEmBus driver.";
      tray.notification_icon = tray.allIconPaths[4];
      tray.notification_cb = []() {
        launch_ui("/troubleshooting#virtualhid-license");
      };
    }

    if (tray_initialized_state().load()) {
      tray_update(&tray);
    }
  }

  void prepare_tray_virtualhid_license() {
    const auto result = lvh::get_license_status();
    update_tray_virtualhid_license(result.license, !result.license.licensed());
  }

  void update_tray_virtualhid_driver(
    const bool installed,
    const std::string_view version,
    const bool version_compatible,
    const std::string_view supported_versions
  ) {
    if (config::input.gamepad_driver.empty() || config::input.gamepad_driver == config::GAMEPAD_DRIVER_VIGEMBUS || !installed || version_compatible) {
      return;
    }

    const std::scoped_lock lock(tray_state_mutex());
    clear_tray_notification();

    const auto displayed_version = version.empty() ? "unknown" : std::format("v{}", version);
    auto &notification_text = virtualhid_driver_notification_text_storage();
    notification_text = std::format(
      "Installed Virtual HID Driver {} is not supported by this version of Sunshine. Supported versions: {}. Restart Sunshine after updating. Click for instructions.",
      displayed_version,
      supported_versions
    );
    tray.notification_title = "Update Virtual HID Driver";
    tray.notification_text = notification_text.c_str();
    tray.notification_icon = tray.allIconPaths[4];
    tray.notification_cb = []() {
      launch_ui("/troubleshooting#virtualhid");
    };

    BOOST_LOG(warning) << notification_text;
    if (tray_initialized_state().load()) {
      tray_update(&tray);
    }
  }

  void prepare_tray_virtualhid_driver() {
    const auto status = confighttp::get_virtualhid_driver_status();
    update_tray_virtualhid_driver(
      status.value("installed", false),
      status.value("version", std::string {}),
      status.value("version_compatible", false),
      status.value("supported_versions", std::string {})
    );
  }
  #endif

  /**
   * @brief Get resource path.
   *
   * @param relativePath Relative path.
   * @return Absolute path to the resource file for the current platform bundle layout.
   */
  const char *GetResourcePath(const char *relativePath) {
    if (!relativePath || !*relativePath) {
      return nullptr;
    }

    if (std::filesystem::path {relativePath}.is_absolute()) {
      return relativePath;
    }

  #if defined(_WIN32) || defined(__APPLE__)
    // Simple cache ensures our string pointers live forever.
    static std::unordered_map<std::string, std::string, transparent_string_hash_t, std::equal_to<>> g_cache;
    auto search = g_cache.find(std::string_view {relativePath});
    if (search != g_cache.end()) {
      return search->second.c_str();
    }
  #endif

  #ifdef _WIN32
    std::array<wchar_t, 32768> executable {};
    const auto executable_length = GetModuleFileNameW(nullptr, executable.data(), executable.size());
    if (executable_length == 0 || executable_length >= executable.size()) {
      return relativePath;
    }

    const auto full_path =
      (std::filesystem::path {executable.data()}.parent_path() / utf_utils::from_utf8(relativePath)).lexically_normal();
    auto full = utf_utils::to_utf8(full_path.wstring());

    BOOST_LOG(debug) << "System Tray: using " << full << " for icon path";

    const auto it = g_cache.try_emplace(relativePath, std::move(full)).first;
    return it->second.c_str();
  #elif defined(__APPLE__)
    // If we're running from an .app bundle, get the internal Resources dir
    CFBundleRef bundle = CFBundleGetMainBundle();
    if (!bundle) {
      return relativePath;
    }

    CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(bundle);
    if (!resourcesURL) {
      return relativePath;
    }

    char resourcesPath[PATH_MAX];
    bool ok = CFURLGetFileSystemRepresentation(
      resourcesURL,
      true,
      reinterpret_cast<UInt8 *>(resourcesPath),
      sizeof(resourcesPath)
    );
    CFRelease(resourcesURL);
    if (!ok) {
      return relativePath;
    }

    std::string full;
    if (relativePath && relativePath[0] == '/') {
      full = relativePath;
    } else {
      full = std::string(resourcesPath) + "/" + relativePath;
    }

    BOOST_LOG(debug) << "System Tray: using " << full << " for icon path";

    const auto it = g_cache.try_emplace(relativePath, std::move(full)).first;
    return it->second.c_str();
  #else
    return relativePath;
  #endif
  }

  /**
   * @brief Resolve tray icons against the current executable or application bundle.
   */
  void resolve_tray_icon_paths() {
  #if defined(_WIN32) || defined(__APPLE__)
    std::optional<int> notification_icon_index;
    if (tray.notification_icon != nullptr) {
      for (int index = 0; index < tray.iconPathCount; ++index) {
        if (std::string_view {tray.notification_icon} == tray.allIconPaths[index]) {
          notification_icon_index = index;
          break;
        }
      }
    }

    for (int index = 0; index < tray.iconPathCount; ++index) {
      tray.allIconPaths[index] = GetResourcePath(tray.allIconPaths[index]);
    }
    tray.icon = tray.allIconPaths[0];
    if (notification_icon_index.has_value()) {
      tray.notification_icon = tray.allIconPaths[*notification_icon_index];
    }
  #endif
  }

  #ifdef SUNSHINE_TESTS
  const char *resource_path_for_testing(const char *relative_path) {
    return GetResourcePath(relative_path);
  }

  void resolve_tray_icon_paths_for_testing() {
    resolve_tray_icon_paths();
  }
  #endif

  /**
   * @brief Set the disabled state of the Restart and Quit menu entries from the lifecycle setting.
   *
   * Call with `tray_state_mutex()` held. Only changes the menu data; the caller decides about `tray_update`.
   */
  static void apply_lifecycle_entries_locked() {
  #ifdef __linux__
    const int disabled = lifecycle_entries_enabled.load() ? 0 : 1;
    for (struct tray_menu *item = tray.menu; item != nullptr && item->text != nullptr; ++item) {
      if (item->cb == tray_restart_cb || item->cb == tray_quit_cb) {
        item->disabled = disabled;
      }
    }
  #endif
  }

  /**
   * @brief Initialize the tray, optionally observing an owning thread's stop request.
   *
   * @param stop_token Stop token supplied by the managed tray worker, or an empty token for direct initialization.
   * @return Zero when initialization succeeds; otherwise, a non-zero error code.
   */
  int init_tray_with_stop_token(const std::stop_token stop_token) {
  #ifdef _WIN32
    // If we're running as SYSTEM, Explorer.exe will not have permission to open our thread handle
    // to monitor for thread termination. If Explorer fails to open our thread, our tray icon
    // will persist forever if we terminate unexpectedly. To avoid this, we will modify our thread
    // DACL to add an ACE that allows SYNCHRONIZE access to Everyone.
    {
      PACL old_dacl;
      PSECURITY_DESCRIPTOR sd;
      auto security_error = GetSecurityInfo(GetCurrentThread(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl, nullptr, &sd);
      if (security_error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "GetSecurityInfo() failed: "sv << security_error;
        return 1;
      }

      auto free_sd = util::fail_guard([sd]() {
        LocalFree(sd);
      });

      SID_IDENTIFIER_AUTHORITY sid_authority = SECURITY_WORLD_SID_AUTHORITY;
      PSID world_sid;
      if (!AllocateAndInitializeSid(&sid_authority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &world_sid)) {
        security_error = GetLastError();
        BOOST_LOG(warning) << "AllocateAndInitializeSid() failed: "sv << security_error;
        return 1;
      }

      auto free_sid = util::fail_guard([world_sid]() {
        FreeSid(world_sid);
      });

      EXPLICIT_ACCESS ea {};
      ea.grfAccessPermissions = SYNCHRONIZE;
      ea.grfAccessMode = GRANT_ACCESS;
      ea.grfInheritance = NO_INHERITANCE;
      ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      ea.Trustee.ptstrName = (LPSTR) world_sid;

      PACL new_dacl;
      security_error = SetEntriesInAcl(1, &ea, old_dacl, &new_dacl);
      if (security_error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetEntriesInAcl() failed: "sv << security_error;
        return 1;
      }

      auto free_new_dacl = util::fail_guard([new_dacl]() {
        LocalFree(new_dacl);
      });

      security_error = SetSecurityInfo(GetCurrentThread(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
      if (security_error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetSecurityInfo() failed: "sv << security_error;
        return 1;
      }
    }

    // Wait for the shell to be initialized before registering the tray icon.
    // This ensures the tray icon works reliably after a logoff/logon cycle.
    while (GetShellWindow() == nullptr && !stop_token.stop_requested()) {
      Sleep(1000);
    }
    if (stop_token.stop_requested()) {
      return 1;
    }
  #endif

    resolve_tray_icon_paths();

    tray_set_log_callback(qt_log_to_boost);

    tray_set_app_info(PROJECT_NAME, PROJECT_NAME, PROJECT_FQDN);

    {
      const std::scoped_lock lock(tray_state_mutex());
      if (stop_token.stop_requested()) {
        return 1;
      }
      apply_lifecycle_entries_locked();
      if (tray_init(&tray) < 0) {
        BOOST_LOG(warning) << "Failed to create system tray"sv;
        return 1;
      }

      tray_initialized_state().store(true);
    }

    BOOST_LOG(info) << "System tray created"sv;
    return 0;
  }

  int init_tray() {
    const int result = init_tray_with_stop_token({});
  #ifdef __linux__
    if (result == 0) {
      auto &state = lifecycle();
      {
        const std::scoped_lock lock(state.mutex);
        if (!state.ended) {
          state.phase = tray_phase_e::running;
        }
      }
      state.cv.notify_all();
    }
  #endif
    return result;
  }

  int process_tray_events() {
    if (!tray_initialized_state().load()) {
      BOOST_LOG(error) << "System tray is not initialized"sv;
      return 1;
    }

    // Block until an event is processed or tray_quit() is called
    return tray_loop(1);
  }

  int end_tray() {
    auto &worker = tray_worker_thread();
    worker.request_stop();

  #ifdef __linux__
    // Terminal: mark it first, so the loop returns once the backend's loop ends or its idle wait wakes.
    bool was_running = false;
    tray_backend_t backend;
    {
      auto &state = lifecycle();
      const std::scoped_lock lock(state.mutex);
      state.ended = true;
      was_running = state.phase == tray_phase_e::running;
      state.phase = tray_phase_e::ended;
      state.command = tray_command_e::none;
      state.resume_after_suspend = false;
      backend = effective_backend_locked();
    }
    lifecycle().cv.notify_all();

    {
      const std::scoped_lock lock(tray_state_mutex());
      // A running loop may be blocked in the backend although the flag is already false (a suspend in flight).
      if (tray_initialized_state().exchange(false) || was_running) {
        backend.exit();
      }
    }
  #else
    {
      const std::scoped_lock lock(tray_state_mutex());
      if (tray_initialized_state().exchange(false)) {
        tray_exit();
      }
    }
  #endif

    if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
      worker.join();
    }
    return 0;
  }

  void update_tray_playing(std::string app_name) {
    const std::scoped_lock lock(tray_state_mutex());
    if (!tray_initialized_state().load()) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = tray.allIconPaths[2];
    tray_update(&tray);
    tray.icon = tray.allIconPaths[2];
    tray.notification_title = "Stream Started";

    static std::string msg = std::format("Streaming started for {}", app_name);
    tray.notification_text = msg.c_str();
    tray.tooltip = msg.c_str();
    tray.notification_icon = tray.allIconPaths[2];
    tray_update(&tray);
  }

  void update_tray_pausing(std::string app_name) {
    const std::scoped_lock lock(tray_state_mutex());
    if (!tray_initialized_state().load()) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = tray.allIconPaths[3];
    tray_update(&tray);

    static std::string msg = std::format("Streaming paused for {}", app_name);
    tray.icon = tray.allIconPaths[3];
    tray.notification_title = "Stream Paused";
    tray.notification_text = msg.c_str();
    tray.tooltip = msg.c_str();
    tray.notification_icon = tray.allIconPaths[3];
    tray_update(&tray);
  }

  void update_tray_stopped(std::string app_name) {
    const std::scoped_lock lock(tray_state_mutex());
    if (!tray_initialized_state().load()) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = tray.allIconPaths[0];
    tray_update(&tray);

    static std::string msg = std::format("Application {} successfully stopped", app_name);
    tray.icon = tray.allIconPaths[0];
    tray.notification_icon = tray.allIconPaths[0];
    tray.notification_title = "Application Stopped";
    tray.notification_text = msg.c_str();
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);
  }

  void update_tray_require_pin() {
    const std::scoped_lock lock(tray_state_mutex());
    if (!tray_initialized_state().load()) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = tray.allIconPaths[0];
    tray_update(&tray);
    tray.icon = tray.allIconPaths[0];
    tray.notification_title = "Incoming Pairing Request";
    tray.notification_text = "Click here to complete the pairing process";
    tray.notification_icon = tray.allIconPaths[1];
    tray.tooltip = PROJECT_NAME;
    tray.notification_cb = []() {
      launch_ui("/pin");
    };
    tray_update(&tray);
  }

  namespace {
  #ifdef __linux__
    /**
     * @brief Whether the tray counts as enabled for `request_start` and `resume`.
     *
     * @return `true` when `config::sunshine.system_tray` is set or a test enabled it.
     */
    bool tray_enabled() {
      auto &state = lifecycle();
      {
        const std::scoped_lock lock(state.mutex);
        if (state.enabled_for_testing) {
          return true;
        }
      }
      return config::sunshine.system_tray;
    }

    /**
     * @brief Complete a pending suspend on the loop thread.
     *
     * Precondition: `state.mutex` is held by `lock`, the phase is `running` and the command is `suspend`. Calls the
     * backend's `exit` once more (the call `suspend()` made was only queued, and a backend's `exit` has no memory),
     * clears the initialized flag without `tray_state_mutex()` (this thread must never wait for it: an `update_tray_*`
     * may hold it while the backend blocks on this thread), sets the phase `suspended` and notifies the waiters. A
     * start or resume that arrived meanwhile becomes the pending command.
     *
     * @param lock The held lifecycle lock; released and taken again.
     */
    void complete_suspend(std::unique_lock<std::mutex> &lock) {
      auto &state = lifecycle();
      const auto backend = effective_backend_locked();
      state.command = tray_command_e::none;
      lock.unlock();

      tray_initialized_state().store(false);
      backend.exit();

      lock.lock();
      if (!state.ended) {
        state.phase = tray_phase_e::suspended;
        if (state.resume_after_suspend) {
          state.command = tray_command_e::resume;
        }
      }
      state.resume_after_suspend = false;
      state.cv.notify_all();
    }
  #endif
  }  // namespace

  void run_main_loop(const std::function<bool()> &shutdown_requested) {
  #ifdef __linux__
    auto &state = lifecycle();
    {
      const std::scoped_lock lock(state.mutex);
      state.loop_entered = true;
      state.loop_exited = false;
      state.loop_thread = std::this_thread::get_id();
    }

    std::unique_lock lock(state.mutex);
    while (true) {
      // 1. end_tray ran.
      if (state.ended) {
        break;
      }

      // 2. Start or resume: create the icon on this thread.
      if (state.command == tray_command_e::start || state.command == tray_command_e::resume) {
        if (state.phase == tray_phase_e::not_started || state.phase == tray_phase_e::suspended) {
          const auto backend = effective_backend_locked();
          state.command = tray_command_e::none;
          lock.unlock();

          const int result = backend.init();

          lock.lock();
          if (result == 0) {
            if (!state.ended) {
              state.phase = tray_phase_e::running;
              state.cv.notify_all();
            } else {
              // end_tray ran during the init and found nothing to remove: remove the icon now.
              tray_initialized_state().store(false);
              backend.exit();
            }
          } else {
            BOOST_LOG(warning) << "Failed to create system tray"sv;
          }
          continue;
        }

        // Already running: nothing to do.
        state.command = tray_command_e::none;
      }

      if (state.phase == tray_phase_e::running) {
        // 3. A pending suspend completes without calling the loop.
        if (state.command == tray_command_e::suspend) {
          complete_suspend(lock);
          continue;
        }

        const auto backend = effective_backend_locked();
        lock.unlock();
        const int result = backend.loop();
        lock.lock();

        if (result == 0) {
          continue;
        }
        if (state.command == tray_command_e::suspend && !state.ended) {
          complete_suspend(lock);
          continue;
        }
        // end_tray ran, or Quit was chosen, or the backend failed: the loop ends as the old one did.
        break;
      }

      // 4. Idle phases: poll for a shutdown that did not come through end_tray.
      state.command = tray_command_e::none;  // a suspend has nothing to remove here
      lock.unlock();
      const bool shutdown = shutdown_requested && shutdown_requested();
      lock.lock();
      if (shutdown) {
        break;
      }
      if (state.command == tray_command_e::none && !state.ended) {
        state.cv.wait_for(lock, 500ms, [&state]() {
          return state.command != tray_command_e::none || state.ended;
        });
      }
    }

    state.loop_exited = true;
    state.cv.notify_all();
  #else
    // The tray runs on its own thread on other platforms; plain loop for completeness.
    while (process_tray_events() == 0 && !(shutdown_requested && shutdown_requested())) {
    }
  #endif
  }

  bool suspend(std::chrono::milliseconds timeout) {
  #ifdef __linux__
    auto &state = lifecycle();
    std::unique_lock lock(state.mutex);

    if (state.ended) {
      return true;
    }
    if (state.phase != tray_phase_e::running) {
      // Nothing to remove; a start or resume not yet executed is cancelled (the icon stays gone).
      state.command = tray_command_e::none;
      state.resume_after_suspend = false;
      return true;
    }

    state.command = tray_command_e::suspend;
    state.resume_after_suspend = false;
    const auto backend = effective_backend_locked();
    const bool on_loop_thread = state.loop_entered && !state.loop_exited && state.loop_thread == std::this_thread::get_id();
    const bool loop_gone = state.loop_exited;
    lock.unlock();

    if (on_loop_thread) {
      // A tray menu callback: nobody else can pump the loop. Do not take the state mutex (it could be held by an
      // update that waits for this very thread); the loop completes the command once the callback returns.
      tray_initialized_state().store(false);
      backend.exit();
      state.cv.notify_all();
      return false;
    }

    {
      // As end_tray: remove the icon from here, never from the main thread.
      const std::scoped_lock tray_lock(tray_state_mutex());
      tray_initialized_state().exchange(false);
      backend.exit();
    }
    state.cv.notify_all();

    lock.lock();
    if (loop_gone && state.phase == tray_phase_e::running) {
      // No loop is left to acknowledge.
      state.phase = tray_phase_e::suspended;
      state.command = tray_command_e::none;
      return true;
    }
    return state.cv.wait_for(lock, timeout, [&state]() {
      return state.phase != tray_phase_e::running || state.ended;
    });
  #else
    (void) timeout;
    return false;
  #endif
  }

  bool resume() {
  #ifdef __linux__
    auto &state = lifecycle();
    bool entered = false;
    {
      const std::scoped_lock lock(state.mutex);
      entered = state.loop_entered;
    }
    if (!entered && !tray_enabled()) {
      return false;
    }

    {
      const std::scoped_lock lock(state.mutex);
      if (state.ended) {
        return false;
      }
      switch (state.phase) {
        case tray_phase_e::not_started:
        case tray_phase_e::ended:
          return false;
        case tray_phase_e::running:
          if (state.command == tray_command_e::suspend) {
            state.resume_after_suspend = true;  // the suspend in flight finishes, then the icon comes back
          }
          return true;
        case tray_phase_e::suspended:
          state.command = tray_command_e::resume;
          break;
      }
    }
    state.cv.notify_all();
    return true;
  #else
    return false;
  #endif
  }

  bool request_start() {
  #ifdef __linux__
    if (!tray_enabled()) {
      return false;
    }

    auto &state = lifecycle();
    {
      const std::scoped_lock lock(state.mutex);
      if (state.ended) {
        return false;
      }
      switch (state.phase) {
        case tray_phase_e::ended:
          return false;
        case tray_phase_e::running:
          if (state.command == tray_command_e::suspend) {
            state.resume_after_suspend = true;
          }
          return true;
        case tray_phase_e::suspended:
          state.command = tray_command_e::resume;
          break;
        case tray_phase_e::not_started:
          state.command = tray_command_e::start;
          break;
      }
    }
    state.cv.notify_all();
    return true;
  #else
    return false;
  #endif
  }

  void set_lifecycle_entries_enabled(bool enabled) {
    lifecycle_entries_enabled.store(enabled);
  #ifdef __linux__
    bool on_loop_thread = false;
    {
      auto &state = lifecycle();
      const std::scoped_lock lock(state.mutex);
      on_loop_thread = state.loop_entered && !state.loop_exited && state.loop_thread == std::this_thread::get_id();
    }

    const std::scoped_lock lock(tray_state_mutex());
    apply_lifecycle_entries_locked();
    // The main thread cannot queue a blocking update to itself; the next update shows the change.
    if (tray_initialized_state().load() && !on_loop_thread) {
      tray_update(&tray);
    }
  #endif
  }

  tray_phase_e current_phase() {
    auto &state = lifecycle();
    const std::scoped_lock lock(state.mutex);
    return state.phase;
  }

  #ifdef SUNSHINE_TESTS
  void set_backend_for_testing(const tray_backend_t &backend) {
    auto &state = lifecycle();
    const std::scoped_lock lock(state.mutex);
    state.backend = backend;
  }

  void reset_lifecycle_for_testing() {
    auto &state = lifecycle();
    {
      const std::scoped_lock lock(state.mutex);
      state.phase = tray_phase_e::not_started;
      state.command = tray_command_e::none;
      state.resume_after_suspend = false;
      state.ended = false;
      state.loop_entered = false;
      state.loop_exited = false;
      state.loop_thread = {};
      state.enabled_for_testing = false;
      state.backend = {};
    }
    lifecycle_entries_enabled.store(true);
    {
      const std::scoped_lock lock(tray_state_mutex());
      apply_lifecycle_entries_locked();
    }
  }

  void set_enabled_for_testing(bool enabled) {
    auto &state = lifecycle();
    const std::scoped_lock lock(state.mutex);
    state.enabled_for_testing = enabled;
  }
  #endif

  // Threading functions available on all platforms
  /**
   * @brief Run the managed system tray event loop.
   *
   * @param stop_token Stop token owned by the tray worker thread.
   */
  static void tray_thread_worker(const std::stop_token stop_token) {
    platf::set_thread_name("system_tray");
    BOOST_LOG(info) << "System tray thread started"sv;

    // Initialize the tray in this thread
    if (init_tray_with_stop_token(stop_token) != 0) {
      if (!stop_token.stop_requested()) {
        BOOST_LOG(error) << "Failed to initialize tray in thread"sv;
      }
      return;
    }

    // Main tray event loop
    while (!stop_token.stop_requested() && process_tray_events() == 0) {
      // Continue until the tray requests shutdown or event processing stops.
    }

    {
      const std::scoped_lock lock(tray_state_mutex());
      tray_initialized_state().store(false);
    }

    BOOST_LOG(info) << "System tray thread ended"sv;
  }

  int init_tray_threaded() {
    try {
      auto &worker = tray_worker_thread();
      if (worker.joinable()) {
        BOOST_LOG(error) << "System tray thread is already running"sv;
        return 1;
      }
      worker = std::jthread(tray_thread_worker);

      BOOST_LOG(info) << "System tray thread initialized successfully"sv;
      return 0;
    } catch (const std::system_error &e) {
      BOOST_LOG(error) << "Failed to create tray thread: " << e.what();
      return 1;
    }
  }

}  // namespace system_tray
#endif
