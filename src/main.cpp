/**
 * @file src/main.cpp
 * @brief Definitions for the main entry point for Sunshine.
 */
// standard includes
#include <cerrno>
#include <codecvt>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>

// platform includes
#ifdef __APPLE__
  #include <mach-o/dyld.h>
#endif
#ifdef __linux__
  #include "platform/linux/graphics.h"

  #include <sys/auxv.h>
  #if defined(SUNSHINE_BUILD_DRM)
    #include "platform/linux/misc.h"
  #endif
#endif

// lib includes
#include <rs.h>
#ifdef _WIN32
  #include <libvirtualhid/license.hpp>
#endif

// local includes
#include "confighttp.h"
#include "display_device.h"
#include "entry_handler.h"
#include "globals.h"
#include "handoff/hooks.h"
#include "httpcommon.h"
#include "logging.h"
#include "main.h"
#include "nvhttp.h"
#include "process.h"
#include "system_tray.h"
#include "upnp.h"
#include "video.h"

#ifdef __linux__
  #include "handoff/close_app.h"
  #include "handoff/control.h"
  #include "handoff/coordinator.h"
  #include "handoff/drain.h"
  #include "handoff/entry.h"
  #include "handoff/instance_file.h"
  #include "handoff/new_ops.h"
  #include "platform/common.h"
  #include "platform/linux/misc.h"
#endif

using namespace std::literals;

std::map<int, std::function<void()>> signal_handlers;  ///< Signal handlers.

/**
 * @brief Forward a POSIX signal to the registered Sunshine handler.
 *
 * @param sig Native signal number being handled.
 */
void on_signal_forwarder(int sig) {
  signal_handlers.at(sig)();
}

/**
 * @brief Register the handler invoked for a POSIX signal.
 *
 * @param sig Native signal number being handled.
 * @param fn Signal handler function to install.
 */
template<class FN>
void on_signal(int sig, FN &&fn) {
  signal_handlers.emplace(sig, std::forward<FN>(fn));

  std::signal(sig, on_signal_forwarder);
}

/**
 * @brief Cmd to func.
 */
std::map<std::string_view, std::function<int(const char *name, int argc, char **argv)>> cmd_to_func {
  {"creds"sv, [](const char *name, int argc, char **argv) {
     return args::creds(name, argc, argv);
   }},
  {"help"sv, [](const char *name, int argc, char **argv) {
     return args::help(name);
   }},
  {"version"sv, [](const char *name, int argc, char **argv) {
     return args::version();
   }},
#ifdef _WIN32
  {"restore-nvprefs-undo"sv, [](const char *name, int argc, char **argv) {
     return args::restore_nvprefs_undo();
   }},
#endif
};

#ifdef _WIN32
/**
 * @brief Handle Windows session-change messages for the monitor window.
 *
 * @param hwnd Window handle receiving the Windows control event.
 * @param uMsg U msg.
 * @param wParam W param.
 * @param lParam L param.
 * @return Process or platform callback exit code.
 */
LRESULT CALLBACK SessionMonitorWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_ENDSESSION:
      {
        // Terminate ourselves with a blocking exit call
        std::cout << "Received WM_ENDSESSION"sv << std::endl;
        lifetime::exit_sunshine(0, false);
        return 0;
      }
    default:
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }
}

/**
 * @brief Handle Windows console control events for shutdown.
 *
 * @param type Protocol, message, or resource type selector.
 * @return Process or platform callback exit code.
 */
WINAPI BOOL ConsoleCtrlHandler(DWORD type) {
  if (type == CTRL_CLOSE_EVENT) {
    BOOST_LOG(info) << "Console closed handler called";
    lifetime::exit_sunshine(0, false);
  }
  return FALSE;
}
#endif

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
constexpr bool tray_is_enabled = true;  ///< Compile-time flag indicating tray support is enabled.
#else
constexpr bool tray_is_enabled = false;
#endif

/**
 * @brief Run the main event loop until Sunshine is asked to exit.
 *
 * @param shutdown_event Shutdown event.
 */
void mainThreadLoop(const std::shared_ptr<safe::event_t<bool>> &shutdown_event) {
  bool run_loop = false;

  // Conditions that would require the main thread event loop
#ifndef _WIN32
  run_loop = tray_is_enabled && config::sunshine.system_tray;  // On Windows, tray runs in separate thread, so no main loop needed for tray
#endif

  if (!run_loop) {
    BOOST_LOG(info) << "No main thread features enabled, skipping event loop"sv;
    // Wait for shutdown
    shutdown_event->view();
    return;
  }

  // Main thread event loop
  BOOST_LOG(info) << "Starting main loop"sv;
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
  #ifdef __linux__
  // The loop also idles without an icon (a handoff-started process) until the tray is started or shutdown is requested.
  system_tray::run_main_loop([&shutdown_event]() {
    return shutdown_event->peek();
  });
  #else
  while (system_tray::process_tray_events() == 0);
  #endif
#endif
  BOOST_LOG(info) << "Main loop has exited"sv;
}

/**
 * @brief Run the main application or worker loop.
 *
 * @param argc The number of arguments.
 * @param argv The arguments.
 * @return Process or platform callback exit code.
 */
int main(int argc, char *argv[]) {
#ifdef __linux__
  // The `--drain-handoff` helper runs before anything else: no log file, no thread, no configuration.
  if (const auto code = handoff::entry::maybe_run_helper(argc, argv)) {
    return *code;
  }

  const bool privileged_execution = getauxval(AT_SECURE) != 0 || platf::has_elevated_privileges(true);
  if (const auto reason = handoff::entry::early_start(privileged_execution); !reason.empty()) {
    std::cerr << reason << '\n';
    return 1;
  }
  if (privileged_execution && !platf::sanitize_process_environment()) {
    std::cerr << "Failed to sanitize the environment for privileged execution: " << std::strerror(errno) << '\n';
    return 1;
  }
#endif

#ifdef __APPLE__
  // Bundle assets are referenced relative to the executable
  // (e.g. ../Resources/assets), so anchor cwd to Contents/MacOS.
  {
    char executable[2048];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) == 0) {
      std::error_code ec;
      auto exec_dir = std::filesystem::weakly_canonical(std::filesystem::path {executable}, ec).parent_path();
      if (!ec) {
        std::filesystem::current_path(exec_dir, ec);
      }
      if (ec) {
        std::cerr << "Failed to set working directory to executable path: " << ec.message() << '\n';
      }
    }
  }
#endif
#if defined(__linux__)
  // On Linux, child threads inherit capabilities from the parent at creation time.
  // We ensure the privileged worker starts while capabilities are still held,
  // and then immediately strip global privileges from the main thread and future tasks.
  // Please ensure that these calls remain ordered as early as possible in startup initialization.
  #if defined(SUNSHINE_BUILD_DRM)
  platf::kms::ensure_privileged_drm_worker_started();
  #endif
  // Check and drop capabilities but use 'false' to retain CAP_SYS_NICE for EGL high priority contexts
  if (platf::has_elevated_privileges(false)) {
    platf::drop_elevated_privileges(false);
  }

  // Next, initialize privileged EGL worker thread
  egl::ensure_privileged_egl_worker_started();
  // Finally, check and drop all capabilities via 'true', which includes CAP_SYS_NICE.
  if (platf::has_elevated_privileges(true)) {
    platf::drop_elevated_privileges(true);
  }
#endif

  lifetime::argv = argv;

  task_pool_util::TaskPool::task_id_t force_shutdown = nullptr;

#ifdef _WIN32
  // Avoid searching the PATH in case a user has configured their system insecurely
  // by placing a user-writable directory in the system-wide PATH variable.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

  setlocale(LC_ALL, "C");
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  // Use UTF-8 conversion for the default C++ locale (used by boost::log)
  std::locale::global(std::locale(std::locale(), new std::codecvt_utf8<wchar_t>));
#pragma GCC diagnostic pop

  mail::man = std::make_shared<safe::mail_raw_t>();

  // parse config file
  if (config::parse(argc, argv)) {
    return 0;
  }

  auto log_deinit_guard = logging::init(config::sunshine.min_log_level, config::sunshine.log_file);
  if (!log_deinit_guard) {
    BOOST_LOG(error) << "Logging failed to initialize"sv;
  }

  // logging can begin at this point
  // if anything is logged prior to this point, it will appear in stdout, but not in the log viewer in the UI
  // the version should be printed to the log before anything else
  BOOST_LOG(info) << PROJECT_NAME << " version: " << PROJECT_VERSION << " commit: " << PROJECT_VERSION_COMMIT;

  // Log publisher metadata
  log_publisher_data();

  // Log modified_config_settings
  config::log_config_settings(config::modified_config_settings, false);
  config::modified_config_settings.clear();

  if (!config::sunshine.cmd.name.empty()) {
    auto fn = cmd_to_func.find(config::sunshine.cmd.name);
    if (fn == std::end(cmd_to_func)) {
      BOOST_LOG(fatal) << "Unknown command: "sv << config::sunshine.cmd.name;

      BOOST_LOG(info) << "Possible commands:"sv;
      for (auto &[key, _] : cmd_to_func) {
        BOOST_LOG(info) << '\t' << key;
      }

      return 7;
    }

    return fn->second(argv[0], config::sunshine.cmd.argc, config::sunshine.cmd.argv);
  }

#ifdef _WIN32
  config::select_all_gamepad_drivers_if_licensed(lvh::get_license_status().license.licensed());
#endif

  // Adding guard here first as it also performs recovery after crash,
  // otherwise people could theoretically end up without display output.
  // It also should be destroyed before forced shutdown to expedite the cleanup.
  auto display_device_deinit_guard = display_device::init(platf::appdata() / "display_device.state", config::video);
  if (!display_device_deinit_guard) {
    BOOST_LOG(error) << "Display device session failed to initialize"sv;
  }

#ifdef __linux__
  const auto handoff_refusal = handoff::entry::apply_startup_policy(privileged_execution);
  if (handoff::runtime().enabled()) {
    handoff::entry::open_instance_file();
    handoff::session_notifier().start();
  }
#endif

#ifdef _WIN32
  // Modify relevant NVIDIA control panel settings if the system has corresponding gpu
  if (nvprefs_instance.load()) {
    // Restore global settings to the undo file left by improper termination of sunshine.exe
    nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
    // Modify application settings for sunshine.exe
    nvprefs_instance.modify_application_profile();
    // Modify global settings, undo file is produced in the process to restore after improper termination
    nvprefs_instance.modify_global_profile();
    // Unload dynamic library to survive driver re-installation
    nvprefs_instance.unload();
  }

  // Wait as long as possible to terminate Sunshine.exe during logoff/shutdown
  SetProcessShutdownParameters(0x100, SHUTDOWN_NORETRY);

  // We must create a hidden window to receive shutdown notifications since we load gdi32.dll
  std::promise<HWND> session_monitor_hwnd_promise;
  auto session_monitor_hwnd_future = session_monitor_hwnd_promise.get_future();
  std::promise<void> session_monitor_join_thread_promise;
  auto session_monitor_join_thread_future = session_monitor_join_thread_promise.get_future();

  std::jthread session_monitor_thread([&]() {
    platf::set_thread_name("session_monitor");
    session_monitor_join_thread_promise.set_value_at_thread_exit();

    WNDCLASSA wnd_class {};
    wnd_class.lpszClassName = "SunshineSessionMonitorClass";
    wnd_class.lpfnWndProc = SessionMonitorWindowProc;
    if (!RegisterClassA(&wnd_class)) {
      session_monitor_hwnd_promise.set_value(nullptr);
      BOOST_LOG(error) << "Failed to register session monitor window class"sv << std::endl;
      return;
    }

    auto wnd = CreateWindowExA(
      0,
      wnd_class.lpszClassName,
      "Sunshine Session Monitor Window",
      0,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      nullptr,
      nullptr,
      nullptr,
      nullptr
    );

    session_monitor_hwnd_promise.set_value(wnd);

    if (!wnd) {
      BOOST_LOG(error) << "Failed to create session monitor window"sv << std::endl;
      return;
    }

    ShowWindow(wnd, SW_HIDE);

    // Run the message loop for our window
    MSG msg {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  });

  auto session_monitor_join_thread_guard = util::fail_guard([&]() {
    if (session_monitor_hwnd_future.wait_for(1s) == std::future_status::ready) {
      if (HWND session_monitor_hwnd = session_monitor_hwnd_future.get()) {
        PostMessage(session_monitor_hwnd, WM_CLOSE, 0, 0);
      }

      if (session_monitor_join_thread_future.wait_for(1s) == std::future_status::ready) {
        session_monitor_thread.join();
        return;
      } else {
        BOOST_LOG(warning) << "session_monitor_join_thread_future reached timeout";
      }
    } else {
      BOOST_LOG(warning) << "session_monitor_hwnd_future reached timeout";
    }

    session_monitor_thread.detach();
  });

#endif

  task_pool.start(1);

  // Create signal handler after logging has been initialized
  auto shutdown_event = mail::man->event<bool>(mail::shutdown);
  on_signal(SIGINT, [&force_shutdown, &display_device_deinit_guard, shutdown_event]() {
    BOOST_LOG(info) << "Interrupt handler called"sv;

    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet Sunshine's still running: Forcing shutdown"sv;
      logging::log_flush();
      lifetime::debug_trap();
    };
    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    // Break out of the main loop
    shutdown_event->raise(true);

    if (tray_is_enabled && config::sunshine.system_tray) {
      system_tray::end_tray();
    }

    display_device_deinit_guard = nullptr;
  });

  on_signal(SIGTERM, [&force_shutdown, &display_device_deinit_guard, shutdown_event]() {
    handoff::hooks::on_sigterm();

    BOOST_LOG(info) << "Terminate handler called"sv;

    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet Sunshine's still running: Forcing shutdown"sv;
      logging::log_flush();
      lifetime::debug_trap();
    };
    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    // Break out of the main loop
    shutdown_event->raise(true);

    if (tray_is_enabled && config::sunshine.system_tray) {
      system_tray::end_tray();
    }

    display_device_deinit_guard = nullptr;
  });

#ifdef _WIN32
  // Terminate gracefully on Windows when console window is closed
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif

  // These are declared before the handoff objects so that they are destroyed after them.
  std::unique_ptr<platf::deinit_t> mDNS;
  std::unique_ptr<platf::deinit_t> upnp_unmap;

#ifdef __linux__
  // Declaration order is destruction order in reverse (see `handoff/entry.h`, section 7): the coordinator goes first,
  // then the control server, then the handoff process.
  handoff::entry::handoff_process_t handoff_process {
    [shutdown_event]() {
      shutdown_event->view();
    },
    [shutdown_event]() {
      shutdown_event->raise(true);
    }
  };
  std::optional<handoff::control_server_t> handoff_control;
  std::optional<handoff::coordinator_t> coordinator;
  if (handoff::runtime().enabled()) {
    handoff_control.emplace();
    coordinator.emplace(*handoff_control);
    handoff::set_new_ops_coordinator(&*coordinator);

    // A handoff-started process starts its control socket when it becomes the main process.
    if (!handoff::hooks::started_by_handoff()) {
      const char *runtime_directory = std::getenv("RUNTIME_DIRECTORY");
      const char *xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR");
      if (const auto dir = handoff::runtime_dir(runtime_directory ? runtime_directory : "", xdg_runtime_dir ? xdg_runtime_dir : "")) {
        coordinator->start_control(handoff::control_socket_path(*dir, getpid()).string());
      }
    }
  }
  auto coordinator_hook_guard = util::fail_guard([]() {
    handoff::set_new_ops_coordinator(nullptr);
  });

  // Without the drain handoff, `control_socket = enabled` serves the `close-app` verb alone (`control.h`). Nothing is
  // created with mode `none`. A start failure is logged by `control_server_t::start()`; Sunshine keeps running.
  std::optional<handoff::control_server_t> close_app_control;
  if (!handoff::runtime().enabled() && handoff::control_socket_mode(false, config::drain_handoff.control_socket) == handoff::control_socket_mode_e::close_app_only) {
    const char *runtime_directory = std::getenv("RUNTIME_DIRECTORY");
    const char *xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    if (const auto dir = handoff::runtime_dir(runtime_directory ? runtime_directory : "", xdg_runtime_dir ? xdg_runtime_dir : "")) {
      handoff::control_server_t::handlers_t handlers;
      handlers.close_app = [](pid_t peer) {
        return handoff::close_app(peer);
      };
      close_app_control.emplace();
      if (!close_app_control->start(handoff::control_socket_path(*dir, getpid()), std::move(handlers), nullptr)) {
        close_app_control.reset();
      }
    } else {
      BOOST_LOG(error) << "control_socket is enabled, but no runtime directory is set; the close-app verb is not served";
    }
  }
#endif

  proc::refresh(config::stream.file_apps);

  // If any of the following fail, we log an error and continue event though sunshine will not function correctly.
  // This allows access to the UI to fix configuration problems or view the logs.

  auto platf_deinit_guard = platf::init();
  if (!platf_deinit_guard) {
    BOOST_LOG(error) << "Platform failed to initialize"sv;
  }

  auto proc_deinit_guard = proc::init();
  if (!proc_deinit_guard) {
    BOOST_LOG(error) << "Proc failed to initialize"sv;
  }

  reed_solomon_init();
#ifdef __linux__
  handoff::entry::device_release_t input_deinit_guard {input::init()};
#else
  auto input_deinit_guard = input::init();
#endif

  if (config::input.controller && input::probe_gamepads()) {
    BOOST_LOG(warning) << "No gamepad input is available"sv;
  }

#ifdef __linux__
  // A handoff-started process now starts the new-side thread, which exchanges HELLO with the old process.
  handoff::new_side_timeouts_t handoff_timeouts;
  handoff_timeouts.go = config::stream.ping_timeout + 25s;
  if (handoff::hooks::started_by_handoff()) {
    handoff::new_ops_config_t ops_config;
    ops_config.port = config::sunshine.port;
    ops_config.ipv6 = net::af_from_enum_string(config::sunshine.address_family) != net::IPV4;
    ops_config.feature_enabled = config::drain_handoff.enabled;
    ops_config.startup_refusal = handoff_refusal;
    ops_config.tray_enabled = tray_is_enabled && config::sunshine.system_tray;
    ops_config.servers_listening_timeout = std::chrono::seconds {handoff::runtime().settings().ready_timeout_s};
    handoff_process.start_new_side(ops_config, upnp_unmap, handoff_timeouts);

    // The accepted probe override must be installed before the probe gate below, or the probe runs.
    if (auto *new_ops = handoff_process.new_ops()) {
      new_ops->wait_probe_decision(handoff_timeouts.hello + 2s);
    }
  }
#endif

  if (!handoff::hooks::startup_probe_skipped()) {
    if (video::probe_encoders()) {
      BOOST_LOG(error) << "Video failed to find working encoder"sv;
    }
  }

  if (http::init()) {
    BOOST_LOG(fatal) << "HTTP interface failed to initialize"sv;

#ifdef __linux__
    if (auto *new_ops = handoff_process.new_ops()) {
      new_ops->servers_failed("HTTP interface failed to initialize");
    }
#endif

#ifdef _WIN32
    BOOST_LOG(fatal) << "To relaunch Sunshine successfully, use the shortcut in the Start Menu. Do not run Sunshine.exe manually."sv;
    std::this_thread::sleep_for(10s);
#endif

    return -1;
  }

  auto sync_mDNS = std::async(std::launch::async, [&mDNS]() {
    // A process started by a handoff publishes nothing before COMMIT; the handoff glue publishes there.
    if (!handoff::hooks::started_by_handoff()) {
      mDNS = handoff::hooks::mdns_start(false);
    }
  });

  auto sync_upnp = std::async(std::launch::async, [&upnp_unmap]() {
    // A process started by a handoff maps nothing before COMMIT; the handoff glue starts UPnP there.
    if (!handoff::hooks::started_by_handoff()) {
      upnp_unmap = upnp::start(handoff::hooks::udp_base());
    }
  });

  // FIXME: Temporary workaround: Simple-Web_server needs to be updated or replaced
  if (shutdown_event->peek()) {
    // Released here as at the end of main, before the guards.
    if (sync_upnp.valid()) {
      sync_upnp.wait();
    }
    if (sync_mDNS.valid()) {
      sync_mDNS.wait();
    }
    upnp_unmap.reset();
    mDNS.reset();
    return lifetime::desired_exit_code;
  }

  bool start_servers = true;
#ifdef __linux__
  // The listening callbacks count towards READY=1 (a normal start) or tell the new side (a handoff-started process).
  handoff::entry::listening_latch_t listening_latch {4, handoff::entry::notify_ready_if_systemd};
  std::function<void(unsigned short)> arrive = [&listening_latch](unsigned short) {
    listening_latch.arrive();
  };
  if (handoff::hooks::started_by_handoff()) {
    auto *new_ops = handoff_process.new_ops();
    if (new_ops) {
      arrive = [new_ops](unsigned short) {
        new_ops->server_listening();
      };
      // The servers adopt the inherited fds, so they start only when the listeners were validated.
      start_servers = new_ops->wait_listeners_validated(handoff_timeouts.hello + std::chrono::seconds {handoff::runtime().settings().ready_timeout_s}) == handoff::validation_e::ok;
    } else {
      start_servers = false;
    }
  }
  rtsp_stream::set_listening_callback(arrive);
  nvhttp::set_listening_callback(arrive);
  confighttp::set_listening_callback(arrive);
#endif

  std::jthread httpThread;
  std::jthread configThread;
  std::jthread rtspThread;
#ifdef __linux__
  if (start_servers && handoff::hooks::started_by_handoff()) {
    const auto inherited = handoff::runtime().inherited();
    if (!inherited || !rtsp_stream::adopt_listener(inherited->rtsp, true)) {
      if (auto *new_ops = handoff_process.new_ops()) {
        new_ops->servers_failed("rtsp adopt");
      }
      start_servers = false;
    }
  }
#endif
  if (start_servers) {
    httpThread = std::jthread {nvhttp::start};
    configThread = std::jthread {confighttp::start};
    rtspThread = std::jthread {rtsp_stream::start};

#ifdef __linux__
    if (handoff::runtime().enabled()) {
      // Register once the RTSP listener exists (it is bound or adopted on the RTSP thread).
      for (int i = 0; i < 500 && rtsp_stream::listener_fd() < 0 && !shutdown_event->peek(); ++i) {
        std::this_thread::sleep_for(10ms);
      }
      handoff::listeners().set(handoff::listener_e::rtsp, handoff::rtsp_listener_ops());
    }
#endif
  }

#ifdef _WIN32
  // If we're using the default port and GameStream is enabled, warn the user
  if (config::sunshine.port == 47989 && is_gamestream_enabled()) {
    BOOST_LOG(fatal) << "GameStream is still enabled in GeForce Experience! This *will* cause streaming problems with Sunshine!"sv;
    BOOST_LOG(fatal) << "Disable GameStream on the SHIELD tab in GeForce Experience or change the Port setting on the Advanced tab in the Sunshine Web UI."sv;
  }
#endif

  // A handoff-started process shows its tray at COMMIT (the main loop starts it on request).
  if (tray_is_enabled && config::sunshine.system_tray && !handoff::hooks::started_by_handoff()) {
    BOOST_LOG(info) << "Starting system tray"sv;
#ifdef _WIN32
    system_tray::prepare_tray_virtualhid_license();
    system_tray::prepare_tray_virtualhid_driver();
    // TODO: Windows has a weird bug where when running as a service and on the first Windows boot,
    // the tray icon would not appear even though Sunshine is running correctly otherwise.
    // Restarting the service would allow the icon to appear normally.
    // For now we will keep the Windows tray icon on a separate thread.
    // Ideally, we would run the system tray on the main thread for all platforms.
    system_tray::init_tray_threaded();
#else
    system_tray::init_tray();
#endif
  }

  mainThreadLoop(shutdown_event);

#ifdef __linux__
  // The new side stops first; then the coordinator's early teardown, before the proc guard runs.
  handoff_process.finish_new_side();
  if (coordinator) {
    coordinator->request_shutdown();
    handoff::set_new_ops_coordinator(nullptr);
    coordinator->stop_control();
    coordinator->join_attempt();
  }
  if (close_app_control) {
    close_app_control->stop();
  }
  handoff::listeners().clear(handoff::listener_e::rtsp);
#endif

#ifdef __linux__
  handoff::note_shutdown_stage("client server thread (nvhttp)");
#endif
  if (httpThread.joinable()) {
    httpThread.join();
  }
#ifdef __linux__
  handoff::note_shutdown_stage("web UI server thread (confighttp)");
#endif
  if (configThread.joinable()) {
    configThread.join();
  }
#ifdef __linux__
  handoff::note_shutdown_stage("RTSP server thread");
#endif
  if (rtspThread.joinable()) {
    rtspThread.join();
  }

#ifdef __linux__
  handoff::note_shutdown_stage("task pool");
#endif
  task_pool.stop();
  task_pool.join();

#ifdef _WIN32
  // Restore global NVIDIA control panel settings
  if (nvprefs_instance.owning_undo_file() && nvprefs_instance.load()) {
    nvprefs_instance.restore_global_profile();
    nvprefs_instance.unload();
  }
#endif

  // Released here, as upstream's scope end did, before the app terminate and the device and platform guards; declared
  // early only for the new side's UPnP slot.
  if (sync_upnp.valid()) {
    sync_upnp.wait();
  }
  if (sync_mDNS.valid()) {
    sync_mDNS.wait();
  }
  upnp_unmap.reset();
  mDNS.reset();

  return lifetime::desired_exit_code;
}
