/**
 * @file src/main.cpp
 * @brief Definitions for the main entry point for Sunshine.
 */
// standard includes
#include <atomic>
#include <chrono>
#include <codecvt>
#include <csignal>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <utility>

// lib includes
#include <rs.h>

// local includes
#include "client_fingerprint.h"
#include "confighttp.h"
#include "display_device/session.h"
#include "entry_handler.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "main.h"
#include "nvhttp.h"
#include "process.h"
#include "tray/system_tray.h"
#include "upnp.h"
#include "version.h"
#include "video.h"
#include "webhook/webhook.h"
#include "webhook/webhook_auth.h"
#include "ds5/config.h"
#include "hdr_enhanced/config.h"

#ifdef _WIN32
  #include "platform/windows/misc.h"
  #include "platform/windows/win_dark_mode.h"
#endif

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>

namespace {
  // Captures the value main() ends up returning so the atexit terminator can use it.
  // Defaults to 0 (success) for the common case where main returns lifetime::desired_exit_code.
  std::atomic<int> g_final_exit_code { 0 };
}
#endif

using namespace std::literals;

std::map<int, std::function<void()>> signal_handlers;
void
on_signal_forwarder(int sig) {
  signal_handlers.at(sig)();
}

template <class FN>
void
on_signal(int sig, FN &&fn) {
  signal_handlers.emplace(sig, std::forward<FN>(fn));

  std::signal(sig, on_signal_forwarder);
}

std::map<std::string_view, std::function<int(const char *name, int argc, char **argv)>> cmd_to_func {
  { "creds"sv, [](const char *name, int argc, char **argv) { return args::creds(name, argc, argv); } },
  { "help"sv, [](const char *name, int argc, char **argv) { return args::help(name); } },
  { "version"sv, [](const char *name, int argc, char **argv) { return args::version(); } },
#ifdef _WIN32
  { "restore-nvprefs-undo"sv, [](const char *name, int argc, char **argv) { return args::restore_nvprefs_undo(); } },
#endif
};

#ifdef _WIN32
LRESULT CALLBACK
SessionMonitorWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_ENDSESSION: {
      // Terminate ourselves with a blocking exit call
      std::cout << "Received WM_ENDSESSION"sv << std::endl;
      lifetime::exit_sunshine(0, false);
      return 0;
    }
    default:
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }
}

WINAPI BOOL
ConsoleCtrlHandler(DWORD type) {
  if (type == CTRL_CLOSE_EVENT) {
    BOOST_LOG(info) << "Console closed handler called";
    lifetime::exit_sunshine(0, false);
  }
  return FALSE;
}
#endif

// Unused on Windows when the GUI agent owns the tray (SUNSHINE_TRAY=0): the
// only remaining reader is the POSIX main-loop condition below.
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
[[maybe_unused]] constexpr bool tray_is_enabled = true;
#else
[[maybe_unused]] constexpr bool tray_is_enabled = false;
#endif

void
mainThreadLoop(const std::shared_ptr<safe::event_t<bool>> &shutdown_event) {
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
  while (system_tray::process_tray_events() == 0);
#else
  // Only the in-process tray sets run_loop, so this build cannot reach here.
  // Block on shutdown anyway: falling through would return to main() and tear
  // the host down immediately if another main-thread feature is ever added.
  shutdown_event->view();
#endif
  BOOST_LOG(info) << "Main loop has exited"sv;
}

/**
 * @brief Run the encoder probe, reporting progress so a stall is diagnosable.
 *
 * The probe talks to the graphics driver and cannot be safely interrupted from
 * the outside — a hung driver call owns its thread until it returns. So the
 * watchdog does not try to cancel anything; it only keeps writing to the log
 * while the probe is outstanding. That turns "Sunshine started but streaming
 * never works and nobody knows why" into a timestamped line naming the phase
 * that is stuck.
 */
void
probe_encoders_with_watchdog() {
  constexpr auto report_interval = 30s;

  std::promise<void> probe_finished;
  auto probe_finished_future = probe_finished.get_future();

  BOOST_LOG(info) << "Probing for supported encoders..."sv;
  const auto probe_started_at = std::chrono::steady_clock::now();

  std::thread watchdog([&probe_finished_future, report_interval, probe_started_at]() {
    while (probe_finished_future.wait_for(report_interval) == std::future_status::timeout) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - probe_started_at);
      BOOST_LOG(warning) << "Encoder probe has been running for "sv << elapsed.count()
                         << "s and has not returned. This usually means a graphics driver call "
                            "is stuck. Streaming will not work until it completes; the web UI is "
                            "already available so the configuration can be changed."sv;
    }
  });

  const bool probe_failed = video::probe_encoders();

  probe_finished.set_value();
  watchdog.join();

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - probe_started_at);

  if (probe_failed) {
    BOOST_LOG(error) << "Video failed to find working encoder (probe took "sv << elapsed.count() << "ms)"sv;
  }
  else {
    BOOST_LOG(info) << "Encoder probe completed in "sv << elapsed.count() << "ms"sv;
  }
}

int
main(int argc, char *argv[]) {
  lifetime::argv = argv;

  task_pool_util::TaskPool::task_id_t force_shutdown = nullptr;

#ifdef _WIN32
  // Note: this only fires on a normal `return` from main. If the program
  // crashes mid-run (uncaught exception, AV, abort/terminate), atexit is
  // not invoked, so the service supervisor still observes a non-zero exit
  // code and can restart Sunshine as usual.
  std::atexit([]() {
    TerminateProcess(GetCurrentProcess(),
                     static_cast<UINT>(g_final_exit_code.load(std::memory_order_acquire)));
  });

  // Avoid searching the PATH in case a user has configured their system insecurely
  // by placing a user-writable directory in the system-wide PATH variable.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

  // Enable dark mode for the entire process before creating any windows
  // This must be called early, before any windows or system tray icons are created
  win_dark_mode::enable_process_dark_mode();

  // Set locale to UTF-8 instead of C locale
  setlocale(LC_ALL, ".UTF-8");
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  // Use UTF-8 conversion for the default C++ locale (used by boost::log)
  std::locale::global(std::locale(std::locale(), new std::codecvt_utf8<wchar_t>));
#pragma GCC diagnostic pop

  mail::man = std::make_shared<safe::mail_raw_t>();
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--version"sv) {
      std::cout << PROJECT_NAME << " version: " << PROJECT_VER << std::endl;
      return 0;
    }
  }

  // parse config file
  if (config::parse(argc, argv)) {
    return 0;
  }

  auto log_deinit_guard = logging::init(config::sunshine.min_log_level, config::sunshine.log_file, config::sunshine.restore_log);
  if (!log_deinit_guard) {
    BOOST_LOG(error) << "Logging failed to initialize"sv;
  }

  // logging can begin at this point
  // if anything is logged prior to this point, it will appear in stdout, but not in the log viewer in the UI
  // the version should be printed to the log before anything else
  BOOST_LOG(info) << PROJECT_NAME << " version: " << PROJECT_VER;

#ifdef _WIN32
  // Cache the result of is_running_as_system() check once at startup
  is_running_as_system_user = platf::is_running_as_system();
  if (is_running_as_system_user) {
    BOOST_LOG(info) << "Running as SYSTEM user (service mode)";
  }
#endif

  // Log publisher metadata
  log_publisher_data();

  // Log modified_config_settings as JSON
  if (!config::modified_config_settings.empty()) {
    std::ostringstream config_json;
    config_json << "Modified config settings: {";
    bool first = true;
    for (auto &[name, val] : config::modified_config_settings) {
      if (!first) config_json << ", ";
      config_json << "\"" << name << "\": \"" << val << "\"";
      first = false;
    }
    config_json << "}";
    BOOST_LOG(info) << config_json.str();
  }
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

  // Adding this guard here first as it also performs recovery after crash,
  // otherwise people could theoretically end up without display output.
  // It also should be run be destroyed before forced shutdown.
  auto display_device_deinit_guard = display_device::session_t::init();
  if (!display_device_deinit_guard) {
    BOOST_LOG(error) << "Display device session failed to initialize"sv;
  }

#ifdef WIN32
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

  std::thread session_monitor_thread([&]() {
    session_monitor_join_thread_promise.set_value_at_thread_exit();

    WNDCLASSA wnd_class {};
    wnd_class.lpszClassName = "SunshineSessionMonitorClass";
    wnd_class.lpfnWndProc = SessionMonitorWindowProc;
    if (!RegisterClassA(&wnd_class)) {
      session_monitor_hwnd_promise.set_value(NULL);
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
      nullptr);

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
      }
      else {
        BOOST_LOG(warning) << "session_monitor_join_thread_future reached timeout";
      }
    }
    else {
      BOOST_LOG(warning) << "session_monitor_hwnd_future reached timeout";
    }

    session_monitor_thread.detach();
  });

#endif

  task_pool.start(1);

  // Create signal handler after logging has been initialized
  auto shutdown_event = mail::man->event<bool>(mail::shutdown);
  on_signal(SIGINT, [&force_shutdown, shutdown_event]() {
    BOOST_LOG(info) << "Interrupt handler called"sv;

    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet Sunshine's still running: Forcing shutdown"sv;
      logging::log_flush();
      lifetime::debug_trap();
    };
    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    shutdown_event->raise(true);
  });

  on_signal(SIGTERM, [&force_shutdown, shutdown_event]() {
    BOOST_LOG(info) << "Terminate handler called"sv;

    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet Sunshine's still running: Forcing shutdown"sv;
      logging::log_flush();
      lifetime::debug_trap();
    };
    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    shutdown_event->raise(true);
  });

#ifdef _WIN32
  // Terminate gracefully on Windows when console window is closed
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif

  proc::refresh(config::stream.file_apps);

  if (!hdr_enhanced::manager().initialize()) {
    BOOST_LOG(warning) << "HDR enhancement configuration or selected component is unavailable; optional processing is disabled";
  }

#ifdef _WIN32
  ds5_config::load_result_t ds5_settings_result {
    ds5_config::load_status_t::INVALID,
    {}
  };
  try {
    ds5_settings_result = ds5_config::load(
      ds5_config::path_for(file_handler::path_from_utf8(config::sunshine.config_file))
    );
  }
  catch (...) {
  }
  if (ds5_settings_result.status == ds5_config::load_status_t::INVALID ||
      !ds5_config::configure(std::move(ds5_settings_result.settings))) {
    BOOST_LOG(error) << "DualSense configuration is invalid; using default DualSense settings"sv;
    ds5_config::configure({});
  }
#endif

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
  auto input_deinit_guard = input::init();

  if (input::probe_gamepads()) {
    BOOST_LOG(warning) << "No gamepad input is available"sv;
  }

  if (http::init()) {
    BOOST_LOG(fatal) << "HTTP interface failed to initialize"sv;

#ifdef _WIN32
    BOOST_LOG(fatal) << "To relaunch Sunshine successfully, use the shortcut in the Start Menu. Do not run Sunshine.exe manually."sv;
    std::this_thread::sleep_for(10s);
#endif

    return -1;
  }

  auto client_fingerprint_deinit_guard = client_fingerprint::init({
    .remote_rules_enabled = config::nvhttp.client_fingerprint_remote_rules,
    .signing_certificate = file_handler::path_from_utf8(config::nvhttp.client_fingerprint_rules_certificate),
    .cache_file = platf::appdata() / "client-fingerprint-rules.json",
  });
  if (!client_fingerprint_deinit_guard) {
    BOOST_LOG(warning) << "Client fingerprint rules are unavailable; using built-in rules";
  }

  std::unique_ptr<platf::deinit_t> mDNS;
  std::future<void> sync_mDNS;
  if (config::sunshine.flags[config::flag::MDNS_BROADCAST]) {
    BOOST_LOG(info) << "mDNS broadcast enabled"sv;
    sync_mDNS = std::async(std::launch::async, [&mDNS]() {
      mDNS = platf::publish::start();
    });
  }

  std::unique_ptr<platf::deinit_t> upnp_unmap;
  auto sync_upnp = std::async(std::launch::async, [&upnp_unmap]() {
    upnp_unmap = upnp::start();
  });

  // FIXME: Temporary workaround: Simple-Web_server needs to be updated or replaced
  if (shutdown_event->peek()) {
    return lifetime::desired_exit_code;
  }

  webhook::auth::load_result_t webhook_auth_result {
    webhook::auth::load_status_t::INVALID,
    {}
  };
  try {
    webhook_auth_result = webhook::auth::load(
      webhook::auth::path_for(config::sunshine.config_file)
    );
  }
  catch (...) {
    // Treat path construction failures exactly like an invalid Webhook file.
    // Webhook configuration must never abort Sunshine startup.
  }
  const bool webhook_configured = webhook::configure(std::move(webhook_auth_result.settings));
  if (webhook_auth_result.status == webhook::auth::load_status_t::INVALID ||
      !webhook_configured) {
    BOOST_LOG(error) << "Webhook configuration is invalid; Webhook delivery is disabled"sv;
  }

  auto webhook_deinit_guard = webhook::init();
  if (!webhook_deinit_guard || !webhook::runtime_active()) {
    BOOST_LOG(warning) << "Webhook runtime is unavailable; Sunshine will continue without Webhook delivery"sv;
  }

  // Start the configuration web UI before probing encoders.
  //
  // probe_encoders() drives the graphics driver directly, and a bad driver or a
  // bad encoder option can make it block for a very long time or forever. When
  // the probe ran on the main thread ahead of every server, such a stall meant
  // the web UI never bound its port: the service looked healthy, but the user
  // had no way to read the logs or undo the option that caused the stall.
  //
  // confighttp does not consume probe results. It reads the active encoder name
  // through an atomic (empty until the probe lands) and HDR pipeline status
  // under its own mutex, so bringing it up early races with nothing. nvhttp and
  // rtsp_stream do depend on the probe (codec support flags, YUV444 support),
  // so they stay behind it.
  std::thread configThread { confighttp::start };

  probe_encoders_with_watchdog();

  std::thread httpThread { nvhttp::start };
  std::thread rtspThread { rtsp_stream::start };

#if defined(_WIN32) && defined(SUNSHINE_GUI_TRAY) && SUNSHINE_GUI_TRAY >= 1
  // The service wrapper owns user-session agent startup in service mode.
  // Standalone and portable runs need to reconcile the same bundled agent here.
  if (!platf::is_running_as_system()) {
    const auto gui_agent_error = platf::launch_gui_agent();
    if (gui_agent_error) {
      BOOST_LOG(warning) << "Failed to launch bundled GUI agent: "sv << gui_agent_error.message();
    }
  }
#endif

#ifdef _WIN32
  // If we're using the default port and GameStream is enabled, warn the user
  if (config::sunshine.port == 47989 && is_gamestream_enabled()) {
    BOOST_LOG(fatal) << "GameStream is still enabled in GeForce Experience! This *will* cause streaming problems with Sunshine!"sv;
    BOOST_LOG(fatal) << "GeForce Experience 中仍然启用了 GameStream！这将导致流媒体问题与 Sunshine！"sv;
    BOOST_LOG(fatal) << "Disable GameStream on the SHIELD tab in GeForce Experience or change the Port setting on the Advanced tab in the Sunshine Web UI."sv;
    BOOST_LOG(fatal) << "在 GeForce Experience 的 SHIELD 标签中禁用 GameStream，或在 Sunshine Web UI 的 Advanced 标签中更改端口设置。"sv;
  }
#endif

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
  if (tray_is_enabled && config::sunshine.system_tray) {
    BOOST_LOG(info) << "Starting system tray"sv;
  #ifdef _WIN32
    // TODO: Windows has a weird bug where when running as a service and on the first Windows boot,
    // he tray icon would not appear even though Sunshine is running correctly otherwise.
    // Restarting the service would allow the icon to appear normally.
    // For now we will keep the Windows tray icon on a separate thread.
    // Ideally, we would run the system tray on the main thread for all platforms.
    system_tray::init_tray_threaded();
  #else
    system_tray::init_tray();
  #endif
  }
#endif

  mainThreadLoop(shutdown_event);

  client_fingerprint_deinit_guard.reset();

  // Stop outbound callbacks before joining inbound servers. This cancels
  // queued tests while their response objects are still valid.
  webhook_deinit_guard.reset();

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
  system_tray::end_tray();
#endif
  try {
    display_device::session_t::get().restore_state();
  }
  catch (...) {
  }
  display_device_deinit_guard = nullptr;

  httpThread.join();
  configThread.join();
  rtspThread.join();

  task_pool.stop();
  task_pool.join();

  // stop system tray
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
  system_tray::end_tray();
#endif

#ifdef WIN32
  // Restore global NVIDIA control panel settings
  if (nvprefs_instance.owning_undo_file() && nvprefs_instance.load()) {
    nvprefs_instance.restore_global_profile();
    nvprefs_instance.unload();
  }
#endif

#ifdef _WIN32
  // Hand the chosen exit code over to the atexit terminator so it can pass it
  // straight to TerminateProcess. Without this the terminator would always
  // exit with 0 even when lifetime::desired_exit_code was set non-zero by a
  // failure path that still chose to return cleanly from main.
  g_final_exit_code.store(lifetime::desired_exit_code, std::memory_order_release);
#endif
  return lifetime::desired_exit_code;
}
