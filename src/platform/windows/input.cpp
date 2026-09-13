/**
 * @file src/platform/windows/input.cpp
 * @brief Definitions for input handling on Windows.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define WINVER 0x0A00
#include <windows.h>

#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>

#include <ViGEm/Client.h>

#include "dsu_server.h"
#include "ds5/ds5_sidecar_client.h"
#include "src/ds5/config.h"
#include "keylayout.h"
#include "misc.h"
#include "virtual_mouse.h"
#include "src/config.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/common.h"

// Note: HSYNTHETICPOINTERDEVICE and related functions are now included in
// modern MinGW-w64 SDK (UCRT64). The declarations below are only needed for
// very old MinGW versions that predate Windows 10 SDK support.
// 
// If you encounter "undefined" errors on old MinGW, uncomment the following:
// #define SUNSHINE_NEED_SYNTHETIC_POINTER_DECL
#ifdef SUNSHINE_NEED_SYNTHETIC_POINTER_DECL
DECLARE_HANDLE(HSYNTHETICPOINTERDEVICE);
WINUSERAPI HSYNTHETICPOINTERDEVICE WINAPI
CreateSyntheticPointerDevice(POINTER_INPUT_TYPE pointerType, ULONG maxCount, POINTER_FEEDBACK_MODE mode);
WINUSERAPI BOOL WINAPI
InjectSyntheticPointerInput(HSYNTHETICPOINTERDEVICE device, CONST POINTER_TYPE_INFO *pointerInfo, UINT32 count);
WINUSERAPI VOID WINAPI
DestroySyntheticPointerDevice(HSYNTHETICPOINTERDEVICE device);
#endif

namespace platf {
  using namespace std::literals;

  thread_local HDESK _lastKnownInputDesktop = nullptr;

  constexpr touch_port_t target_touch_port {
    0, 0,
    65535, 65535
  };

  enum synthetic_device_creation_options_e: ULONG {
    synthetic_device_creation_none = 0x0,
    synthetic_device_creation_physical_size = 0x1,
    synthetic_device_creation_touchpad_gesture_only = 0x2,
  };

  struct synthetic_device_creation_params_t {
    POINTER_INPUT_TYPE pointerType;
    ULONG maxCount;
    POINTER_FEEDBACK_MODE feedbackMode;
    HMONITOR hMonitor;
    ULONG deviceWidth;
    ULONG deviceHeight;
    ULONG options;
  };

  using create_synthetic_pointer_device2_t = HSYNTHETICPOINTERDEVICE(WINAPI *)(synthetic_device_creation_params_t *params);

  create_synthetic_pointer_device2_t
  get_create_synthetic_pointer_device2_proc() {
    auto user32 = GetModuleHandleA("user32.dll");
    auto fn = (create_synthetic_pointer_device2_t) GetProcAddress(user32, "CreateSyntheticPointerDevice2");
    if (!fn) {
      fn = (create_synthetic_pointer_device2_t) GetProcAddress(user32, MAKEINTRESOURCEA(2690));
    }
    return fn;
  }

  using client_t = util::safe_ptr<_VIGEM_CLIENT_T, vigem_free>;
  using target_t = util::safe_ptr<_VIGEM_TARGET_T, vigem_target_free>;

  void CALLBACK
  x360_notify(
    client_t::pointer client,
    target_t::pointer target,
    std::uint8_t largeMotor, std::uint8_t smallMotor,
    std::uint8_t /* led_number */,
    void *userdata);

  void CALLBACK
  ds4_notify(
    client_t::pointer client,
    target_t::pointer target,
    std::uint8_t largeMotor, std::uint8_t smallMotor,
    DS4_LIGHTBAR_COLOR /* led_color */,
    void *userdata);

  struct gp_touch_context_t {
    uint8_t pointerIndex;
    uint16_t x;
    uint16_t y;
  };

  struct gamepad_context_t {
    target_t gp;
    feedback_queue_t feedback_queue;
    bool back_as_touchpad_click = false;

    union {
      XUSB_REPORT x360;
      DS4_REPORT_EX ds4;
    } report;

    // Map from pointer ID to pointer index
    std::map<uint32_t, uint8_t> pointer_id_map;
    uint8_t available_pointers;

    uint8_t client_relative_index;

    thread_pool_util::ThreadPool::task_id_t repeat_task {};
    std::chrono::steady_clock::time_point last_report_ts;

    gamepad_feedback_msg_t last_rumble;
    gamepad_feedback_msg_t last_rgb_led;
  };

  constexpr float EARTH_G = 9.80665f;

#define MPS2_TO_DS4_ACCEL(x) (int32_t) (((x) / EARTH_G) * 8192)
#define DPS_TO_DS4_GYRO(x) (int32_t) ((x) * (1024 / 64))

#define APPLY_CALIBRATION(val, bias, scale) (int32_t) (((float) (val) + (bias)) / (scale))

  constexpr DS4_TOUCH ds4_touch_unused = {
    .bPacketCounter = 0,
    .bIsUpTrackingNum1 = 0x80,
    .bTouchData1 = { 0x00, 0x00, 0x00 },
    .bIsUpTrackingNum2 = 0x80,
    .bTouchData2 = { 0x00, 0x00, 0x00 },
  };

  // See https://github.com/ViGEm/ViGEmBus/blob/22835473d17fbf0c4d4bb2f2d42fd692b6e44df4/sys/Ds4Pdo.cpp#L153-L164
  constexpr DS4_REPORT_EX ds4_report_init_ex = {
    { { .bThumbLX = 0x80,
      .bThumbLY = 0x80,
      .bThumbRX = 0x80,
      .bThumbRY = 0x80,
      .wButtons = DS4_BUTTON_DPAD_NONE,
      .bSpecial = 0,
      .bTriggerL = 0,
      .bTriggerR = 0,
      .wTimestamp = 0,
      .bBatteryLvl = 0xFF,
      .wGyroX = 0,
      .wGyroY = 0,
      .wGyroZ = 0,
      .wAccelX = 0,
      .wAccelY = 0,
      .wAccelZ = 0,
      ._bUnknown1 = { 0x00, 0x00, 0x00, 0x00, 0x00 },
      .bBatteryLvlSpecial = 0x1A,  // Wired - Full battery
      ._bUnknown2 = { 0x00, 0x00 },
      .bTouchPacketsN = 1,
      .sCurrentTouch = ds4_touch_unused,
      .sPreviousTouch = { ds4_touch_unused, ds4_touch_unused } } }
  };

  /**
   * @brief Updates the DS4 input report with the provided motion data.
   * @details Acceleration is in m/s^2 and gyro is in deg/s.
   * @param gamepad The gamepad to update.
   * @param motion_type The type of motion data.
   * @param x X component of motion.
   * @param y Y component of motion.
   * @param z Z component of motion.
   */
  static void
  ds4_update_motion(gamepad_context_t &gamepad, uint8_t motion_type, float x, float y, float z) {
    auto &report = gamepad.report.ds4.Report;

    // Use int32 to process this data, so we can clamp if needed.
    int32_t intX, intY, intZ;

    switch (motion_type) {
      case LI_MOTION_TYPE_ACCEL:
        // Convert to the DS4's accelerometer scale
        intX = MPS2_TO_DS4_ACCEL(x);
        intY = MPS2_TO_DS4_ACCEL(y);
        intZ = MPS2_TO_DS4_ACCEL(z);

        // Apply the inverse of ViGEmBus's calibration data
        intX = APPLY_CALIBRATION(intX, -297, 1.010796f);
        intY = APPLY_CALIBRATION(intY, -42, 1.014614f);
        intZ = APPLY_CALIBRATION(intZ, -512, 1.024768f);
        break;
      case LI_MOTION_TYPE_GYRO:
        // Convert to the DS4's gyro scale
        intX = DPS_TO_DS4_GYRO(x);
        intY = DPS_TO_DS4_GYRO(y);
        intZ = DPS_TO_DS4_GYRO(z);

        // Apply the inverse of ViGEmBus's calibration data
        intX = APPLY_CALIBRATION(intX, 1, 0.977596f);
        intY = APPLY_CALIBRATION(intY, 0, 0.972370f);
        intZ = APPLY_CALIBRATION(intZ, 0, 0.971550f);
        break;
      default:
        return;
    }

    // Clamp the values to the range of the data type
    intX = std::clamp(intX, INT16_MIN, INT16_MAX);
    intY = std::clamp(intY, INT16_MIN, INT16_MAX);
    intZ = std::clamp(intZ, INT16_MIN, INT16_MAX);

    // Populate the report
    switch (motion_type) {
      case LI_MOTION_TYPE_ACCEL:
        report.wAccelX = (int16_t) intX;
        report.wAccelY = (int16_t) intY;
        report.wAccelZ = (int16_t) intZ;
        break;
      case LI_MOTION_TYPE_GYRO:
        report.wGyroX = (int16_t) intX;
        report.wGyroY = (int16_t) intY;
        report.wGyroZ = (int16_t) intZ;
        break;
      default:
        return;
    }
  }

  class vigem_t {
  public:
    int
    init() {
      // Probe ViGEm during startup to see if we can successfully attach gamepads. This will allow us to
      // immediately display the error message in the web UI even before the user tries to stream.
      client_t client { vigem_alloc() };
      VIGEM_ERROR status = vigem_connect(client.get());
      if (!VIGEM_SUCCESS(status)) {
        // Log a special fatal message for this case to show the error in the web UI
        BOOST_LOG(fatal) << "ViGEmBus is not installed or running. You must install ViGEmBus for gamepad support! (If you don't need gamepad support, you can ignore this message.)"sv;
        BOOST_LOG(fatal) << "ViGEmBus 没有安装或运行。您必须安装 ViGEmBus 才能支持游戏手柄！如果不需要使用游戏手柄，可以忽略此提示。"sv;
      }
      else {
        vigem_disconnect(client.get());
      }

      gamepads.resize(MAX_GAMEPADS);

      return 0;
    }

    /**
     * @brief Attaches a new gamepad.
     * @param id The gamepad ID.
     * @param feedback_queue The queue for posting messages back to the client.
     * @param gp_type The type of gamepad.
     * @param back_as_touchpad_click Whether Back should remain mapped to the DS4 touchpad click for this device.
     * @return 0 on success.
     */
    int
    alloc_gamepad_internal(const gamepad_id_t &id, feedback_queue_t &feedback_queue, VIGEM_TARGET_TYPE gp_type,
                           bool back_as_touchpad_click) {
      auto &gamepad = gamepads[id.globalIndex];
      assert(!gamepad.gp);

      gamepad.client_relative_index = id.clientRelativeIndex;
      gamepad.last_report_ts = std::chrono::steady_clock::now();
      gamepad.back_as_touchpad_click = back_as_touchpad_click;

      // Establish a connect to the ViGEm driver if we don't have one yet
      if (!client) {
        BOOST_LOG(debug) << "Connecting to ViGEmBus driver"sv;
        client.reset(vigem_alloc());

        auto status = vigem_connect(client.get());
        if (!VIGEM_SUCCESS(status)) {
          BOOST_LOG(warning) << "Couldn't setup connection to ViGEm for gamepad support ["sv << util::hex(status).to_string_view() << ']';
          client.reset();
          return -1;
        }
      }

      if (gp_type == Xbox360Wired) {
        gamepad.gp.reset(vigem_target_x360_alloc());
        XUSB_REPORT_INIT(&gamepad.report.x360);
      }
      else {
        gamepad.gp.reset(vigem_target_ds4_alloc());

        // There is no equivalent DS4_REPORT_EX_INIT()
        gamepad.report.ds4 = ds4_report_init_ex;

        // Set initial accelerometer and gyro state
        ds4_update_motion(gamepad, LI_MOTION_TYPE_ACCEL, 0.0f, EARTH_G, 0.0f);
        ds4_update_motion(gamepad, LI_MOTION_TYPE_GYRO, 0.0f, 0.0f, 0.0f);

        // Request motion events from the client at 100 Hz
        feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(gamepad.client_relative_index, LI_MOTION_TYPE_ACCEL, 100));
        feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(gamepad.client_relative_index, LI_MOTION_TYPE_GYRO, 100));

        // We support pointer index 0 and 1
        gamepad.available_pointers = 0x3;
      }

      auto status = vigem_target_add(client.get(), gamepad.gp.get());
      if (!VIGEM_SUCCESS(status)) {
        BOOST_LOG(error) << "Couldn't add Gamepad to ViGEm connection ["sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      gamepad.feedback_queue = std::move(feedback_queue);

      if (gp_type == Xbox360Wired) {
        status = vigem_target_x360_register_notification(client.get(), gamepad.gp.get(), x360_notify, this);
      }
      else {
        status = vigem_target_ds4_register_notification(client.get(), gamepad.gp.get(), ds4_notify, this);
      }

      if (!VIGEM_SUCCESS(status)) {
        BOOST_LOG(warning) << "Couldn't register notifications for rumble support ["sv << util::hex(status).to_string_view() << ']';
      }

      return 0;
    }

    /**
     * @brief Detaches the specified gamepad
     * @param nr The gamepad.
     */
    void
    free_target(int nr) {
      auto &gamepad = gamepads[nr];

      if (gamepad.repeat_task) {
        task_pool.cancel(gamepad.repeat_task);
        gamepad.repeat_task = 0;
      }

      if (gamepad.gp && vigem_target_is_attached(gamepad.gp.get())) {
        auto status = vigem_target_remove(client.get(), gamepad.gp.get());
        if (!VIGEM_SUCCESS(status)) {
          BOOST_LOG(warning) << "Couldn't detach gamepad from ViGEm ["sv << util::hex(status).to_string_view() << ']';
        }
      }

      gamepad.gp.reset();

      // Disconnect from ViGEm if we just removed the last gamepad
      bool disconnect = true;
      for (auto &gamepad : gamepads) {
        if (gamepad.gp && vigem_target_is_attached(gamepad.gp.get())) {
          disconnect = false;
          break;
        }
      }
      if (disconnect) {
        BOOST_LOG(debug) << "Disconnecting from ViGEmBus driver"sv;
        vigem_disconnect(client.get());
        client.reset();
      }
    }

    /**
     * @brief Pass rumble data back to the client.
     * @param target The gamepad.
     * @param largeMotor The large motor.
     * @param smallMotor The small motor.
     */
    void
    rumble(target_t::pointer target, std::uint8_t largeMotor, std::uint8_t smallMotor) {
      for (int x = 0; x < gamepads.size(); ++x) {
        auto &gamepad = gamepads[x];

        if (gamepad.gp.get() == target) {
          // Convert from 8-bit to 16-bit values
          uint16_t normalizedLargeMotor = largeMotor << 8;
          uint16_t normalizedSmallMotor = smallMotor << 8;

          // Don't resend duplicate rumble data
          if (normalizedSmallMotor != gamepad.last_rumble.data.rumble.highfreq ||
              normalizedLargeMotor != gamepad.last_rumble.data.rumble.lowfreq) {
            // We have to use the client-relative index when communicating back to the client
            gamepad_feedback_msg_t msg = gamepad_feedback_msg_t::make_rumble(
              gamepad.client_relative_index, normalizedLargeMotor, normalizedSmallMotor);
            gamepad.feedback_queue->raise(msg);
            gamepad.last_rumble = msg;
          }
          return;
        }
      }
    }

    /**
     * @brief Pass RGB LED data back to the client.
     * @param target The gamepad.
     * @param r The red channel.
     * @param g The red channel.
     * @param b The red channel.
     */
    void
    set_rgb_led(target_t::pointer target, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
      for (int x = 0; x < gamepads.size(); ++x) {
        auto &gamepad = gamepads[x];

        if (gamepad.gp.get() == target) {
          // Don't resend duplicate RGB data
          if (r != gamepad.last_rgb_led.data.rgb_led.r ||
              g != gamepad.last_rgb_led.data.rgb_led.g ||
              b != gamepad.last_rgb_led.data.rgb_led.b) {
            // We have to use the client-relative index when communicating back to the client
            gamepad_feedback_msg_t msg = gamepad_feedback_msg_t::make_rgb_led(gamepad.client_relative_index, r, g, b);
            gamepad.feedback_queue->raise(msg);
            gamepad.last_rgb_led = msg;
          }
          return;
        }
      }
    }

    /**
     * @brief vigem_t destructor.
     */
    ~vigem_t() {
      if (client) {
        for (auto &gamepad : gamepads) {
          if (gamepad.gp && vigem_target_is_attached(gamepad.gp.get())) {
            auto status = vigem_target_remove(client.get(), gamepad.gp.get());
            if (!VIGEM_SUCCESS(status)) {
              BOOST_LOG(warning) << "Couldn't detach gamepad from ViGEm ["sv << util::hex(status).to_string_view() << ']';
            }
          }
        }

        vigem_disconnect(client.get());
      }
    }

    std::vector<gamepad_context_t> gamepads;

    client_t client;
  };

  void CALLBACK
  x360_notify(
    client_t::pointer client,
    target_t::pointer target,
    std::uint8_t largeMotor, std::uint8_t smallMotor,
    std::uint8_t /* led_number */,
    void *userdata) {
    BOOST_LOG(debug)
      << "largeMotor: "sv << (int) largeMotor << std::endl
      << "smallMotor: "sv << (int) smallMotor;

    task_pool.push(&vigem_t::rumble, (vigem_t *) userdata, target, largeMotor, smallMotor);
  }

  void CALLBACK
  ds4_notify(
    client_t::pointer client,
    target_t::pointer target,
    std::uint8_t largeMotor, std::uint8_t smallMotor,
    DS4_LIGHTBAR_COLOR led_color,
    void *userdata) {
    BOOST_LOG(debug)
      << "largeMotor: "sv << (int) largeMotor << std::endl
      << "smallMotor: "sv << (int) smallMotor << std::endl
      << "LED: "sv << util::hex(led_color.Red).to_string_view() << ' '
      << util::hex(led_color.Green).to_string_view() << ' '
      << util::hex(led_color.Blue).to_string_view() << std::endl;

    task_pool.push(&vigem_t::rumble, (vigem_t *) userdata, target, largeMotor, smallMotor);
    task_pool.push(&vigem_t::set_rgb_led, (vigem_t *) userdata, target, led_color.Red, led_color.Green, led_color.Blue);
  }

  // Per-app mouse mode: 0=auto (use global config), 1=force virtual mouse, 2=force SendInput
  static std::atomic<int> current_mouse_mode { 0 };
  // Per-app gamepad mode: 0=inherit global, 1=auto, 2=Xbox 360, 3=DualShock 4, 4=DualSense
  static std::atomic<int> current_gamepad_mode { 0 };
  // Runtime copy of the persisted global policy. 0 means the startup config is authoritative.
  static std::atomic<int> current_global_gamepad_mode { 0 };

  static int
  gamepad_mode_from_preference(std::string_view preference) {
    if (preference == "x360"sv) return 2;
    if (preference == "ds4"sv) return 3;
    if (preference == "ds5"sv) return 4;
    return 1;
  }

  void
  set_mouse_mode(int mode) {
    current_mouse_mode.store(mode, std::memory_order_relaxed);
    BOOST_LOG(info) << "Mouse mode set to: "sv << (mode == 0 ? "auto" : mode == 1 ? "virtual mouse" : "SendInput");
  }

  void
  set_gamepad_mode(int mode) {
    if (mode < 0 || mode > 4) {
      mode = 0;
    }
    current_gamepad_mode.store(mode, std::memory_order_relaxed);
    constexpr std::array<std::string_view, 5> names { "global", "auto", "Xbox 360", "DualShock 4", "DualSense" };
    BOOST_LOG(info) << "Gamepad mode set to: "sv << names[mode];
  }

  void
  set_global_gamepad_mode(std::string_view preference) {
    const auto mode = gamepad_mode_from_preference(preference);
    const auto runtime_mode = current_global_gamepad_mode.load(std::memory_order_relaxed);
    const auto previous_mode = runtime_mode == 0 ? gamepad_mode_from_preference(config::input.gamepad) : runtime_mode;
    if (previous_mode == mode) {
      return;
    }
    current_global_gamepad_mode.store(mode, std::memory_order_relaxed);
    constexpr std::array<std::string_view, 5> names { "startup config", "auto", "Xbox 360", "DualShock 4", "DualSense" };
    BOOST_LOG(info) << "Global gamepad mode hot-applied to: "sv << names[mode]
                    << "; active virtual controllers are unchanged"sv;
  }

  // Client-declared gamepad type for the in-flight session, published at
  // /launch time (see nvhttp.cpp). Empty = undeclared.
  static std::mutex client_gamepad_pref_mutex;
  static std::string client_gamepad_pref;

  void
  set_client_gamepad_pref(std::string pref) {
    std::lock_guard lock(client_gamepad_pref_mutex);
    client_gamepad_pref = std::move(pref);
  }

  static std::string
  get_client_gamepad_pref() {
    std::lock_guard lock(client_gamepad_pref_mutex);
    return client_gamepad_pref;
  }

  static int
  effective_gamepad_mode() {
    const auto app_mode = current_gamepad_mode.load(std::memory_order_relaxed);
    if (app_mode != 0) {
      return app_mode;
    }
    const auto global_mode = current_global_gamepad_mode.load(std::memory_order_relaxed);
    if (global_mode != 0) {
      return global_mode;
    }
    if (config::input.gamepad == "x360"sv) return 2;
    if (config::input.gamepad == "ds4"sv) return 3;
    if (config::input.gamepad == "ds5"sv) return 4;
    return 1;
  }

  struct input_raw_t {
    ~input_raw_t() {
      delete vigem;
      delete vmouse_dev;
      if (dsu_server) {
        dsu_server->stop();
        delete dsu_server;
        dsu_server = nullptr;
      }
    }

    // 初始化DSU服务器（延迟初始化）
    void
    init_dsu_server() {
      if (dsu_server != nullptr) return;

      // 获取DSU服务器端口
      uint16_t server_port = config::input.dsu_server_port;

      dsu_server = new dsu_server_t { server_port };
      if (dsu_server->start()) {
        dsu_server->stop();
        delete dsu_server;
        dsu_server = nullptr;
      }
    }

    vigem_t *vigem;
    std::unique_ptr<ds5::sidecar_client_t> ds5_sidecar;
    dsu_server_t *dsu_server;
    vmouse::device_t *vmouse_dev;
    int vmouse_vscroll_accum = 0;
    int vmouse_hscroll_accum = 0;

    decltype(CreateSyntheticPointerDevice) *fnCreateSyntheticPointerDevice;
    create_synthetic_pointer_device2_t fnCreateSyntheticPointerDevice2;
    decltype(InjectSyntheticPointerInput) *fnInjectSyntheticPointerInput;
    decltype(DestroySyntheticPointerDevice) *fnDestroySyntheticPointerDevice;
  };

  input_t
  input() {
    input_t result { new input_raw_t {} };
    auto &raw = *(input_raw_t *) result.get();

    raw.vigem = new vigem_t {};
    if (raw.vigem->init()) {
      delete raw.vigem;
      raw.vigem = nullptr;
    }

    raw.ds5_sidecar = std::make_unique<ds5::sidecar_client_t>();

    // 初始化DSU服务器（延迟初始化，只在需要时创建）
    raw.dsu_server = nullptr;

    // 初始化虚拟鼠标设备
    if (config::input.virtual_mouse) {
      raw.vmouse_dev = new vmouse::device_t(vmouse::create());
      if (raw.vmouse_dev->is_available()) {
        BOOST_LOG(info) << "Virtual mouse driver connected"sv;
      }
      else {
        BOOST_LOG(info) << "Virtual mouse driver not available, using SendInput"sv;
      }
    }
    else {
      raw.vmouse_dev = nullptr;
      BOOST_LOG(info) << "Virtual mouse driver disabled by config"sv;
    }

    // Get pointers to virtual touch/pen input functions (Win10 1809+)
    raw.fnCreateSyntheticPointerDevice = (decltype(CreateSyntheticPointerDevice) *) GetProcAddress(GetModuleHandleA("user32.dll"), "CreateSyntheticPointerDevice");
    raw.fnCreateSyntheticPointerDevice2 = get_create_synthetic_pointer_device2_proc();
    raw.fnInjectSyntheticPointerInput = (decltype(InjectSyntheticPointerInput) *) GetProcAddress(GetModuleHandleA("user32.dll"), "InjectSyntheticPointerInput");
    raw.fnDestroySyntheticPointerDevice = (decltype(DestroySyntheticPointerDevice) *) GetProcAddress(GetModuleHandleA("user32.dll"), "DestroySyntheticPointerDevice");

    return result;
  }

  /**
   * @brief Calls SendInput() and switches input desktops if required.
   * @param i The `INPUT` struct to send.
   */
  void
  send_input(INPUT &i) {
  retry:
    auto send = SendInput(1, &i, sizeof(INPUT));
    if (send != 1) {
      auto hDesk = syncThreadDesktop();
      if (_lastKnownInputDesktop != hDesk) {
        _lastKnownInputDesktop = hDesk;
        goto retry;
      }
      BOOST_LOG(error) << "Couldn't send input"sv;
    }
  }

  /**
   * @brief Calls InjectSyntheticPointerInput() and switches input desktops if required.
   * @details Must only be called if InjectSyntheticPointerInput() is available.
   * @param input The global input context.
   * @param device The synthetic pointer device handle.
   * @param pointerInfo An array of `POINTER_TYPE_INFO` structs.
   * @param count The number of elements in `pointerInfo`.
   * @return true if input was successfully injected.
   */
  bool
  inject_synthetic_pointer_input(input_raw_t *input, HSYNTHETICPOINTERDEVICE device, const POINTER_TYPE_INFO *pointerInfo, UINT32 count) {
  retry:
    if (!input->fnInjectSyntheticPointerInput(device, pointerInfo, count)) {
      auto hDesk = syncThreadDesktop();
      if (_lastKnownInputDesktop != hDesk) {
        _lastKnownInputDesktop = hDesk;
        goto retry;
      }
      return false;
    }
    return true;
  }

  void
  abs_mouse(input_t &input, const touch_port_t &touch_port, float x, float y) {
    INPUT i {};

    i.type = INPUT_MOUSE;
    auto &mi = i.mi;

    mi.dwFlags =
      MOUSEEVENTF_MOVE |
      MOUSEEVENTF_ABSOLUTE |

      // MOUSEEVENTF_VIRTUALDESK maps to the entirety of the desktop rather than the primary desktop
      MOUSEEVENTF_VIRTUALDESK;

    auto scaled_x = std::lround((x + touch_port.offset_x) * ((float) target_touch_port.width / (float) touch_port.width));
    auto scaled_y = std::lround((y + touch_port.offset_y) * ((float) target_touch_port.height / (float) touch_port.height));

    mi.dx = scaled_x;
    mi.dy = scaled_y;

    send_input(i);
  }

  void
  move_mouse(input_t &input, int deltaX, int deltaY) {
    auto &raw = *(input_raw_t *) input.get();
    if (current_mouse_mode.load(std::memory_order_relaxed) != 2 && raw.vmouse_dev && raw.vmouse_dev->is_available()) {
      raw.vmouse_dev->move((int16_t) deltaX, (int16_t) deltaY);
      return;
    }

    INPUT i {};

    i.type = INPUT_MOUSE;
    auto &mi = i.mi;

    mi.dwFlags = MOUSEEVENTF_MOVE;
    mi.dx = deltaX;
    mi.dy = deltaY;

    send_input(i);
  }

  util::point_t
  get_mouse_loc(input_t &input) {
    (void) input;
    POINT p;
    if (!GetCursorPos(&p)) {
      return util::point_t { 0.0, 0.0 };
    }

    return util::point_t {
      (double) p.x,
      (double) p.y
    };
  }

  void
  button_mouse(input_t &input, int button, bool release) {
    auto &raw = *(input_raw_t *) input.get();
    if (current_mouse_mode.load(std::memory_order_relaxed) != 2 && raw.vmouse_dev && raw.vmouse_dev->is_available()) {
      uint8_t mask = 0;
      switch (button) {
        case 1: mask = vmouse::BTN_LEFT; break;
        case 2: mask = vmouse::BTN_MIDDLE; break;
        case 3: mask = vmouse::BTN_RIGHT; break;
        case 4: mask = vmouse::BTN_SIDE; break;
        default: mask = vmouse::BTN_EXTRA; break;
      }
      raw.vmouse_dev->button(mask, release);
      return;
    }

    INPUT i {};

    i.type = INPUT_MOUSE;
    auto &mi = i.mi;

    if (button == 1) {
      mi.dwFlags = release ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN;
    }
    else if (button == 2) {
      mi.dwFlags = release ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN;
    }
    else if (button == 3) {
      mi.dwFlags = release ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN;
    }
    else if (button == 4) {
      mi.dwFlags = release ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN;
      mi.mouseData = XBUTTON1;
    }
    else {
      mi.dwFlags = release ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN;
      mi.mouseData = XBUTTON2;
    }

    send_input(i);
  }

  void
  scroll(input_t &input, int distance) {
    auto &raw = *(input_raw_t *) input.get();
    if (current_mouse_mode.load(std::memory_order_relaxed) != 2 && raw.vmouse_dev && raw.vmouse_dev->is_available()) {
      // HID wheel uses notch units; distance is in WHEEL_DELTA (120) units.
      // Accumulate sub-notch deltas for high-resolution scrolling support.
      raw.vmouse_vscroll_accum += distance;
      while (raw.vmouse_vscroll_accum <= -WHEEL_DELTA ||
             raw.vmouse_vscroll_accum >= WHEEL_DELTA) {
        const auto notches = std::clamp(
          raw.vmouse_vscroll_accum / WHEEL_DELTA,
          -127,
          127
        );
        if (!raw.vmouse_dev->scroll(static_cast<int8_t>(notches))) {
          raw.vmouse_vscroll_accum = 0;
          break;
        }
        raw.vmouse_vscroll_accum -= notches * WHEEL_DELTA;
      }
      return;
    }

    // Do not replay stale virtual-device deltas after switching back from SendInput.
    raw.vmouse_vscroll_accum = 0;

    INPUT i {};

    i.type = INPUT_MOUSE;
    auto &mi = i.mi;

    mi.dwFlags = MOUSEEVENTF_WHEEL;
    mi.mouseData = distance;

    send_input(i);
  }

  void
  hscroll(input_t &input, int distance) {
    auto &raw = *(input_raw_t *) input.get();
    if (current_mouse_mode.load(std::memory_order_relaxed) != 2 && raw.vmouse_dev && raw.vmouse_dev->is_available()) {
      raw.vmouse_hscroll_accum += distance;
      while (raw.vmouse_hscroll_accum <= -WHEEL_DELTA ||
             raw.vmouse_hscroll_accum >= WHEEL_DELTA) {
        const auto notches = std::clamp(
          raw.vmouse_hscroll_accum / WHEEL_DELTA,
          -127,
          127
        );
        if (!raw.vmouse_dev->hscroll(static_cast<int8_t>(notches))) {
          raw.vmouse_hscroll_accum = 0;
          break;
        }
        raw.vmouse_hscroll_accum -= notches * WHEEL_DELTA;
      }
      return;
    }

    // Do not replay stale virtual-device deltas after switching back from SendInput.
    raw.vmouse_hscroll_accum = 0;

    INPUT i {};

    i.type = INPUT_MOUSE;
    auto &mi = i.mi;

    mi.dwFlags = MOUSEEVENTF_HWHEEL;
    mi.mouseData = distance;

    send_input(i);
  }

  void
  keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags) {
    INPUT i {};
    i.type = INPUT_KEYBOARD;
    auto &ki = i.ki;

    // If the client did not normalize this VK code to a US English layout, we can't accurately convert it to a scancode.
    // If we're set to always send scancodes, we will use the current keyboard layout to convert to a scancode. This will
    // assume the client and host have the same keyboard layout, but it's probably better than always using US English.
    if (!(flags & SS_KBE_FLAG_NON_NORMALIZED)) {
      // Mask off the extended key byte
      ki.wScan = VK_TO_SCANCODE_MAP[modcode & 0xFF];
    }
    else if (config::input.always_send_scancodes && modcode != VK_LWIN && modcode != VK_RWIN && modcode != VK_PAUSE) {
      // For some reason, MapVirtualKey(VK_LWIN, MAPVK_VK_TO_VSC) doesn't seem to work :/
      ki.wScan = MapVirtualKey(modcode, MAPVK_VK_TO_VSC);
    }

    // If we can map this to a scancode, send it as a scancode for maximum game compatibility.
    if (ki.wScan) {
      ki.dwFlags = KEYEVENTF_SCANCODE;
    }
    else {
      // If there is no scancode mapping or it's non-normalized, send it as a regular VK event.
      ki.wVk = modcode;
    }

    // https://docs.microsoft.com/en-us/windows/win32/inputdev/about-keyboard-input#keystroke-message-flags
    switch (modcode) {
      case VK_LWIN:
      case VK_RWIN:
      case VK_RMENU:
      case VK_RCONTROL:
      case VK_INSERT:
      case VK_DELETE:
      case VK_HOME:
      case VK_END:
      case VK_PRIOR:
      case VK_NEXT:
      case VK_UP:
      case VK_DOWN:
      case VK_LEFT:
      case VK_RIGHT:
      case VK_DIVIDE:
      case VK_APPS:
        ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        break;
      default:
        break;
    }

    if (release) {
      ki.dwFlags |= KEYEVENTF_KEYUP;
    }

    send_input(i);
  }

  struct client_input_raw_t: public client_input_t {
    client_input_raw_t(input_t &input) {
      global = (input_raw_t *) input.get();
    }

    ~client_input_raw_t() override {
      if (penRepeatTask) {
        task_pool.cancel(penRepeatTask);
      }
      if (touchRepeatTask) {
        task_pool.cancel(touchRepeatTask);
      }
      if (touchpadRepeatTask) {
        task_pool.cancel(touchpadRepeatTask);
      }

      if (pen) {
        global->fnDestroySyntheticPointerDevice(pen);
      }
      if (touch) {
        global->fnDestroySyntheticPointerDevice(touch);
      }
      if (touchpad) {
        global->fnDestroySyntheticPointerDevice(touchpad);
      }
    }

    input_raw_t *global;

    // Device state and handles for pen and touch input must be stored in the per-client
    // input context, because each connected client may be sending their own independent
    // pen/touch events. To maintain separation, we expose separate pen and touch devices
    // for each client.

    HSYNTHETICPOINTERDEVICE pen {};
    POINTER_TYPE_INFO penInfo {};
    thread_pool_util::ThreadPool::task_id_t penRepeatTask {};

    HSYNTHETICPOINTERDEVICE touch {};
    POINTER_TYPE_INFO touchInfo[10] {};
    UINT32 activeTouchSlots {};
    thread_pool_util::ThreadPool::task_id_t touchRepeatTask {};

    HSYNTHETICPOINTERDEVICE touchpad {};
    POINTER_TYPE_INFO touchpadInfo[5] {};
    std::uint32_t touchpadClientPointerIds[5] {};
    UINT32 activeTouchpadSlots {};
    ULONG touchpadWidthHimetric {};
    ULONG touchpadHeightHimetric {};
    DWORD touchpadTimestampMs {};
    std::uint64_t touchpadRepeatGeneration {};
    thread_pool_util::ThreadPool::task_id_t touchpadRepeatTask {};
    std::mutex touchpadMutex;
  };

  /**
   * @brief Allocates a context to store per-client input data.
   * @param input The global input context.
   * @return A unique pointer to a per-client input data context.
   */
  std::unique_ptr<client_input_t>
  allocate_client_input_context(input_t &input) {
    return std::make_unique<client_input_raw_t>(input);
  }

  /**
   * @brief Compacts the touch slots into a contiguous block and updates the active count.
   * @details Since this swaps entries around, all slot pointers/references are invalid after compaction.
   * @param raw The client-specific input context.
   */
  void
  perform_touch_compaction(client_input_raw_t *raw) {
    // Windows requires all active touches be contiguous when fed into InjectSyntheticPointerInput().
    UINT32 i;
    for (i = 0; i < ARRAYSIZE(raw->touchInfo); i++) {
      if (raw->touchInfo[i].touchInfo.pointerInfo.pointerFlags == POINTER_FLAG_NONE) {
        // This is an empty slot. Look for a later entry to move into this slot.
        for (UINT32 j = i + 1; j < ARRAYSIZE(raw->touchInfo); j++) {
          if (raw->touchInfo[j].touchInfo.pointerInfo.pointerFlags != POINTER_FLAG_NONE) {
            std::swap(raw->touchInfo[i], raw->touchInfo[j]);
            break;
          }
        }

        // If we didn't find anything, we've reached the end of active slots.
        if (raw->touchInfo[i].touchInfo.pointerInfo.pointerFlags == POINTER_FLAG_NONE) {
          break;
        }
      }
    }

    // Update the number of active touch slots
    raw->activeTouchSlots = i;
  }

  /**
   * @brief Gets a pointer slot by client-relative pointer ID, claiming a new one if necessary.
   * @param raw The raw client-specific input context.
   * @param pointerId The client's pointer ID.
   * @param eventType The LI_TOUCH_EVENT value from the client.
   * @return A pointer to the slot entry.
   */
  POINTER_TYPE_INFO *
  pointer_by_id(client_input_raw_t *raw, uint32_t pointerId, uint8_t eventType) {
    // Compact active touches into a single contiguous block
    perform_touch_compaction(raw);

    // Try to find a matching pointer ID
    for (UINT32 i = 0; i < ARRAYSIZE(raw->touchInfo); i++) {
      if (raw->touchInfo[i].touchInfo.pointerInfo.pointerId == pointerId &&
          raw->touchInfo[i].touchInfo.pointerInfo.pointerFlags != POINTER_FLAG_NONE) {
        if (eventType == LI_TOUCH_EVENT_DOWN && (raw->touchInfo[i].touchInfo.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT)) {
          BOOST_LOG(warning) << "Pointer "sv << pointerId << " already down. Did the client drop an up/cancel event?"sv;
        }

        return &raw->touchInfo[i];
      }
    }

    if (eventType != LI_TOUCH_EVENT_HOVER && eventType != LI_TOUCH_EVENT_DOWN) {
      BOOST_LOG(warning) << "Unexpected new pointer "sv << pointerId << " for event "sv << (uint32_t) eventType << ". Did the client drop a down/hover event?"sv;
    }

    // If there was none, grab an unused entry and increment the active slot count
    for (UINT32 i = 0; i < ARRAYSIZE(raw->touchInfo); i++) {
      if (raw->touchInfo[i].touchInfo.pointerInfo.pointerFlags == POINTER_FLAG_NONE) {
        raw->touchInfo[i].touchInfo.pointerInfo.pointerId = pointerId;
        raw->activeTouchSlots = i + 1;
        return &raw->touchInfo[i];
      }
    }

    return nullptr;
  }

  void
  perform_touchpad_compaction(client_input_raw_t *raw) {
    UINT32 i;
    for (i = 0; i < ARRAYSIZE(raw->touchpadInfo); i++) {
      if (raw->touchpadInfo[i].touchInfo.pointerInfo.pointerFlags == POINTER_FLAG_NONE) {
        for (UINT32 j = i + 1; j < ARRAYSIZE(raw->touchpadInfo); j++) {
          if (raw->touchpadInfo[j].touchInfo.pointerInfo.pointerFlags != POINTER_FLAG_NONE) {
            std::swap(raw->touchpadInfo[i], raw->touchpadInfo[j]);
            std::swap(raw->touchpadClientPointerIds[i], raw->touchpadClientPointerIds[j]);
            break;
          }
        }

        if (raw->touchpadInfo[i].touchInfo.pointerInfo.pointerFlags == POINTER_FLAG_NONE) {
          break;
        }
      }
    }

    raw->activeTouchpadSlots = i;
  }

  UINT32
  find_unused_touchpad_injection_id(client_input_raw_t *raw) {
    for (UINT32 pointerId = 0; pointerId < ARRAYSIZE(raw->touchpadInfo); pointerId++) {
      bool used = false;
      for (UINT32 i = 0; i < ARRAYSIZE(raw->touchpadInfo); i++) {
        if (raw->touchpadInfo[i].touchInfo.pointerInfo.pointerFlags != POINTER_FLAG_NONE &&
            raw->touchpadInfo[i].touchInfo.pointerInfo.pointerId == pointerId) {
          used = true;
          break;
        }
      }

      if (!used) {
        return pointerId;
      }
    }

    return ARRAYSIZE(raw->touchpadInfo);
  }

  POINTER_TYPE_INFO *
  touchpad_pointer_by_id(client_input_raw_t *raw, uint32_t pointerId, uint8_t eventType) {
    perform_touchpad_compaction(raw);

    for (UINT32 i = 0; i < ARRAYSIZE(raw->touchpadInfo); i++) {
      if (raw->touchpadClientPointerIds[i] == pointerId &&
          raw->touchpadInfo[i].touchInfo.pointerInfo.pointerFlags != POINTER_FLAG_NONE) {
        if (eventType == LI_TOUCH_EVENT_DOWN && (raw->touchpadInfo[i].touchInfo.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT)) {
          BOOST_LOG(warning) << "Touchpad pointer "sv << pointerId << " already down. Did the client drop an up/cancel event?"sv;
        }

        return &raw->touchpadInfo[i];
      }
    }

    if (eventType != LI_TOUCH_EVENT_HOVER && eventType != LI_TOUCH_EVENT_DOWN) {
      BOOST_LOG(warning) << "Unexpected new touchpad pointer "sv << pointerId << " for event "sv << (uint32_t) eventType << ". Did the client drop a down/hover event?"sv;

      // A button-only event carries no contact state, so it can only update an
      // already-active pointer. Allocating a new slot here would leave a phantom
      // contact that never gets an end event and is replayed forever by the
      // repeat task, sticking the button down.
      if (eventType == LI_TOUCH_EVENT_BUTTON_ONLY) {
        return nullptr;
      }
    }

    for (UINT32 i = 0; i < ARRAYSIZE(raw->touchpadInfo); i++) {
      if (raw->touchpadInfo[i].touchInfo.pointerInfo.pointerFlags == POINTER_FLAG_NONE) {
        auto injectionId = find_unused_touchpad_injection_id(raw);
        if (injectionId >= ARRAYSIZE(raw->touchpadInfo)) {
          break;
        }

        raw->touchpadClientPointerIds[i] = pointerId;
        raw->touchpadInfo[i].touchInfo.pointerInfo.pointerId = injectionId;
        raw->activeTouchpadSlots = i + 1;
        return &raw->touchpadInfo[i];
      }
    }

    return nullptr;
  }

  /**
   * @brief Populate common `POINTER_INFO` members shared between pen and touch events.
   * @param pointerInfo The pointer info to populate.
   * @param touchPort The current viewport for translating to screen coordinates.
   * @param eventType The type of touch/pen event.
   * @param x The normalized 0.0-1.0 X coordinate.
   * @param y The normalized 0.0-1.0 Y coordinate.
   */
  void
  populate_common_pointer_info(POINTER_INFO &pointerInfo, const touch_port_t &touchPort, uint8_t eventType, float x, float y) {
    switch (eventType) {
      case LI_TOUCH_EVENT_HOVER:
        pointerInfo.pointerFlags &= ~POINTER_FLAG_INCONTACT;
        pointerInfo.pointerFlags |= POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE;
        pointerInfo.ptPixelLocation.x = x * touchPort.width + touchPort.offset_x;
        pointerInfo.ptPixelLocation.y = y * touchPort.height + touchPort.offset_y;
        break;
      case LI_TOUCH_EVENT_DOWN:
        pointerInfo.pointerFlags |= POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN;
        pointerInfo.ptPixelLocation.x = x * touchPort.width + touchPort.offset_x;
        pointerInfo.ptPixelLocation.y = y * touchPort.height + touchPort.offset_y;
        break;
      case LI_TOUCH_EVENT_UP:
        // We expect to get another LI_TOUCH_EVENT_HOVER if the pointer remains in range
        pointerInfo.pointerFlags &= ~(POINTER_FLAG_INCONTACT | POINTER_FLAG_INRANGE);
        pointerInfo.pointerFlags |= POINTER_FLAG_UP;
        break;
      case LI_TOUCH_EVENT_MOVE:
        pointerInfo.pointerFlags |= POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE;
        pointerInfo.ptPixelLocation.x = x * touchPort.width + touchPort.offset_x;
        pointerInfo.ptPixelLocation.y = y * touchPort.height + touchPort.offset_y;
        break;
      case LI_TOUCH_EVENT_CANCEL:
      case LI_TOUCH_EVENT_CANCEL_ALL:
        // If we were in contact with the touch surface at the time of the cancellation,
        // we'll set POINTER_FLAG_UP, otherwise set POINTER_FLAG_UPDATE.
        if (pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) {
          pointerInfo.pointerFlags |= POINTER_FLAG_UP;
        }
        else {
          pointerInfo.pointerFlags |= POINTER_FLAG_UPDATE;
        }
        pointerInfo.pointerFlags &= ~(POINTER_FLAG_INCONTACT | POINTER_FLAG_INRANGE);
        pointerInfo.pointerFlags |= POINTER_FLAG_CANCELED;
        break;
      case LI_TOUCH_EVENT_HOVER_LEAVE:
        pointerInfo.pointerFlags &= ~(POINTER_FLAG_INCONTACT | POINTER_FLAG_INRANGE);
        pointerInfo.pointerFlags |= POINTER_FLAG_UPDATE;
        break;
      case LI_TOUCH_EVENT_BUTTON_ONLY:
        // On Windows, we can only pass buttons if we have an active pointer
        if (pointerInfo.pointerFlags != POINTER_FLAG_NONE) {
          pointerInfo.pointerFlags |= POINTER_FLAG_UPDATE;
        }
        break;
      default:
        BOOST_LOG(warning) << "Unknown touch event: "sv << (uint32_t) eventType;
        break;
    }
  }

  // Active pointer interactions sent via InjectSyntheticPointerInput() seem to be automatically
  // cancelled by Windows if not repeated/updated within about a second. To avoid this, refresh
  // the injected input periodically.
  constexpr auto ISPI_REPEAT_INTERVAL = 50ms;

  /**
   * @brief Repeats the current touch state to avoid the interactions timing out.
   * @param raw The raw client-specific input context.
   */
  void
  repeat_touch(client_input_raw_t *raw) {
    if (!inject_synthetic_pointer_input(raw->global, raw->touch, raw->touchInfo, raw->activeTouchSlots)) {
      auto err = GetLastError();
      BOOST_LOG(warning) << "Failed to refresh virtual touch input: "sv << err;
    }

    raw->touchRepeatTask = task_pool.pushDelayed(repeat_touch, ISPI_REPEAT_INTERVAL, raw).task_id;
  }

  /**
   * @brief Repeats the current pen state to avoid the interactions timing out.
   * @param raw The raw client-specific input context.
   */
  void
  repeat_pen(client_input_raw_t *raw) {
    if (!inject_synthetic_pointer_input(raw->global, raw->pen, &raw->penInfo, 1)) {
      auto err = GetLastError();
      BOOST_LOG(warning) << "Failed to refresh virtual pen input: "sv << err;
    }

    raw->penRepeatTask = task_pool.pushDelayed(repeat_pen, ISPI_REPEAT_INTERVAL, raw).task_id;
  }

  /**
   * @brief Cancels all active touches.
   * @param raw The raw client-specific input context.
   */
  void
  cancel_all_active_touches(client_input_raw_t *raw) {
    // Cancel touch repeat callbacks
    if (raw->touchRepeatTask) {
      task_pool.cancel(raw->touchRepeatTask);
      raw->touchRepeatTask = nullptr;
    }

    // Compact touches to update activeTouchSlots
    perform_touch_compaction(raw);

    // If we have active slots, cancel them all
    if (raw->activeTouchSlots > 0) {
      for (UINT32 i = 0; i < raw->activeTouchSlots; i++) {
        populate_common_pointer_info(raw->touchInfo[i].touchInfo.pointerInfo, {}, LI_TOUCH_EVENT_CANCEL_ALL, 0.0f, 0.0f);
        raw->touchInfo[i].touchInfo.touchMask = TOUCH_MASK_NONE;
      }
      if (!inject_synthetic_pointer_input(raw->global, raw->touch, raw->touchInfo, raw->activeTouchSlots)) {
        auto err = GetLastError();
        BOOST_LOG(warning) << "Failed to cancel all virtual touch input: "sv << err;
      }
    }

    // Zero all touch state
    std::memset(raw->touchInfo, 0, sizeof(raw->touchInfo));
    raw->activeTouchSlots = 0;
  }

  // These are edge-triggered pointer state flags that should always be cleared next frame
  constexpr auto EDGE_TRIGGERED_POINTER_FLAGS = POINTER_FLAG_DOWN | POINTER_FLAG_UP | POINTER_FLAG_CANCELED | POINTER_FLAG_UPDATE;

  constexpr ULONG HIMETRIC_PER_MM = 100;
  constexpr ULONG DEFAULT_TOUCHPAD_WIDTH_HIMETRIC = 92 * HIMETRIC_PER_MM;
  constexpr ULONG DEFAULT_TOUCHPAD_HEIGHT_HIMETRIC = 54 * HIMETRIC_PER_MM;
  constexpr ULONG MIN_TOUCHPAD_SIZE_HIMETRIC = 40 * HIMETRIC_PER_MM;
  constexpr ULONG MAX_TOUCHPAD_SIZE_HIMETRIC = 200 * HIMETRIC_PER_MM;
  constexpr std::uint8_t TOUCHPAD_BUTTON_PRIMARY = 0x01;
  constexpr auto TOUCHPAD_BUTTON_POINTER_FLAGS = POINTER_FLAG_FIRSTBUTTON |
                                                 POINTER_FLAG_SECONDBUTTON |
                                                 POINTER_FLAG_THIRDBUTTON |
                                                 POINTER_FLAG_FOURTHBUTTON |
                                                 POINTER_FLAG_FIFTHBUTTON;

  ULONG
  touchpad_size_to_himetric(std::uint16_t sizeMm, ULONG fallback) {
    if (!sizeMm) {
      return fallback;
    }

    return std::clamp<ULONG>((ULONG) sizeMm * HIMETRIC_PER_MM, MIN_TOUCHPAD_SIZE_HIMETRIC, MAX_TOUCHPAD_SIZE_HIMETRIC);
  }

  void
  set_touchpad_location(POINTER_INFO &pointerInfo, ULONG widthHimetric, ULONG heightHimetric, float x, float y) {
    pointerInfo.ptHimetricLocation.x = std::lround(x * widthHimetric);
    pointerInfo.ptHimetricLocation.y = std::lround(y * heightHimetric);
    pointerInfo.ptHimetricLocationRaw = pointerInfo.ptHimetricLocation;
  }

  void
  populate_touchpad_pointer_info(POINTER_INFO &pointerInfo, uint8_t eventType, float x, float y, ULONG widthHimetric, ULONG heightHimetric) {
    auto buttonFlags = pointerInfo.pointerFlags & TOUCHPAD_BUTTON_POINTER_FLAGS;

    switch (eventType) {
      case LI_TOUCH_EVENT_HOVER:
        pointerInfo.pointerFlags = buttonFlags | POINTER_FLAG_INRANGE | POINTER_FLAG_CONFIDENCE;
        set_touchpad_location(pointerInfo, widthHimetric, heightHimetric, x, y);
        break;
      case LI_TOUCH_EVENT_DOWN:
        pointerInfo.pointerFlags = buttonFlags | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_CONFIDENCE;
        set_touchpad_location(pointerInfo, widthHimetric, heightHimetric, x, y);
        break;
      case LI_TOUCH_EVENT_UP:
        pointerInfo.pointerFlags = buttonFlags | POINTER_FLAG_CONFIDENCE;
        break;
      case LI_TOUCH_EVENT_MOVE:
        pointerInfo.pointerFlags = buttonFlags | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_CONFIDENCE;
        set_touchpad_location(pointerInfo, widthHimetric, heightHimetric, x, y);
        break;
      case LI_TOUCH_EVENT_CANCEL:
      case LI_TOUCH_EVENT_CANCEL_ALL:
        pointerInfo.pointerFlags = buttonFlags | POINTER_FLAG_CANCELED | POINTER_FLAG_CONFIDENCE;
        break;
      case LI_TOUCH_EVENT_HOVER_LEAVE:
        pointerInfo.pointerFlags = buttonFlags | POINTER_FLAG_CONFIDENCE;
        break;
      case LI_TOUCH_EVENT_BUTTON_ONLY:
        if (pointerInfo.pointerFlags != POINTER_FLAG_NONE) {
          pointerInfo.pointerFlags |= POINTER_FLAG_CONFIDENCE;
        }
        break;
      default:
        BOOST_LOG(warning) << "Unknown touchpad event: "sv << (uint32_t) eventType;
        break;
    }
  }

  void
  update_touchpad_button_state(POINTER_INFO &pointerInfo, std::uint8_t buttonState) {
    // Never set a button flag on an empty slot, otherwise the contact becomes
    // permanently active without an end event.
    if (pointerInfo.pointerFlags == POINTER_FLAG_NONE) {
      pointerInfo.ButtonChangeType = POINTER_CHANGE_NONE;
      return;
    }

    bool wasDown = !!(pointerInfo.pointerFlags & POINTER_FLAG_FIRSTBUTTON);
    bool isDown = !!(buttonState & TOUCHPAD_BUTTON_PRIMARY);

    if (isDown) {
      pointerInfo.pointerFlags |= POINTER_FLAG_FIRSTBUTTON;
    }
    else {
      pointerInfo.pointerFlags &= ~POINTER_FLAG_FIRSTBUTTON;
    }

    if (wasDown != isDown) {
      pointerInfo.ButtonChangeType = isDown ? POINTER_CHANGE_FIRSTBUTTON_DOWN : POINTER_CHANGE_FIRSTBUTTON_UP;
    }
    else {
      pointerInfo.ButtonChangeType = POINTER_CHANGE_NONE;
    }
  }

  void
  update_touchpad_frame_timestamps(client_input_raw_t *raw) {
    auto timestampMs = GetTickCount();
    if (!timestampMs) {
      timestampMs = 1;
    }

    if (raw->touchpadTimestampMs != 0 && (LONG) (timestampMs - raw->touchpadTimestampMs) <= 0) {
      timestampMs = raw->touchpadTimestampMs == std::numeric_limits<DWORD>::max() ? 1 : raw->touchpadTimestampMs + 1;
    }

    for (UINT32 i = 0; i < raw->activeTouchpadSlots; i++) {
      if (raw->touchpadInfo[i].touchInfo.pointerInfo.pointerFlags != POINTER_FLAG_NONE) {
        raw->touchpadInfo[i].touchInfo.pointerInfo.dwTime = timestampMs;
      }
    }

    raw->touchpadTimestampMs = timestampMs;
  }

  bool
  touchpad_event_ends_pointer(std::uint8_t eventType) {
    return eventType == LI_TOUCH_EVENT_UP ||
           eventType == LI_TOUCH_EVENT_CANCEL ||
           eventType == LI_TOUCH_EVENT_HOVER_LEAVE;
  }

  void
  cancel_pending_touchpad_repeat(client_input_raw_t *raw) {
    raw->touchpadRepeatGeneration++;
    if (raw->touchpadRepeatTask) {
      task_pool.cancel(raw->touchpadRepeatTask);
      raw->touchpadRepeatTask = nullptr;
    }
  }

  void
  repeat_touchpad(client_input_raw_t *raw, std::uint64_t generation) {
    std::lock_guard lock(raw->touchpadMutex);

    if (generation != raw->touchpadRepeatGeneration || !raw->touchpad || raw->activeTouchpadSlots == 0) {
      return;
    }

    update_touchpad_frame_timestamps(raw);
    if (!inject_synthetic_pointer_input(raw->global, raw->touchpad, raw->touchpadInfo, raw->activeTouchpadSlots)) {
      auto err = GetLastError();
      BOOST_LOG(warning) << "Failed to refresh virtual touchpad input: "sv << err;
    }

    raw->touchpadRepeatTask = task_pool.pushDelayed(repeat_touchpad, ISPI_REPEAT_INTERVAL, raw, generation).task_id;
  }

  void
  cancel_all_active_touchpad(client_input_raw_t *raw) {
    cancel_pending_touchpad_repeat(raw);

    perform_touchpad_compaction(raw);

    if (raw->activeTouchpadSlots > 0) {
      for (UINT32 i = 0; i < raw->activeTouchpadSlots; i++) {
        populate_touchpad_pointer_info(raw->touchpadInfo[i].touchInfo.pointerInfo, LI_TOUCH_EVENT_CANCEL_ALL, 0.0f, 0.0f, raw->touchpadWidthHimetric, raw->touchpadHeightHimetric);
        raw->touchpadInfo[i].touchInfo.touchMask = TOUCH_MASK_NONE;
      }
      update_touchpad_frame_timestamps(raw);
      if (!inject_synthetic_pointer_input(raw->global, raw->touchpad, raw->touchpadInfo, raw->activeTouchpadSlots)) {
        auto err = GetLastError();
        BOOST_LOG(warning) << "Failed to cancel all virtual touchpad input: "sv << err;
      }
    }

    std::memset(raw->touchpadInfo, 0, sizeof(raw->touchpadInfo));
    std::memset(raw->touchpadClientPointerIds, 0, sizeof(raw->touchpadClientPointerIds));
    raw->activeTouchpadSlots = 0;
    raw->touchpadTimestampMs = 0;
  }

  POINTER_TYPE_INFO *
  touchpad_contact_update(
    client_input_raw_t *raw,
    std::uint8_t eventType,
    std::uint8_t buttonState,
    std::uint16_t rotation,
    std::uint32_t pointerId,
    float x,
    float y,
    float pressure,
    float contactAreaMajor,
    float contactAreaMinor) {
    auto pointer = touchpad_pointer_by_id(raw, pointerId, eventType);
    if (!pointer) {
      BOOST_LOG(error) << "No unused touchpad pointer entries! Cancelling all active touchpad contacts!"sv;
      cancel_all_active_touchpad(raw);
      pointer = touchpad_pointer_by_id(raw, pointerId, eventType);
    }

    if (!pointer) {
      return pointer;
    }

    pointer->type = PT_TOUCHPAD;

    auto &touchInfo = pointer->touchInfo;
    touchInfo.pointerInfo.pointerType = PT_TOUCHPAD;

    populate_touchpad_pointer_info(
      touchInfo.pointerInfo,
      eventType,
      x,
      y,
      raw->touchpadWidthHimetric,
      raw->touchpadHeightHimetric);
    update_touchpad_button_state(touchInfo.pointerInfo, buttonState);

    touchInfo.touchMask = TOUCH_MASK_NONE;

    if (touchInfo.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) {
      if (pressure != 0.0f) {
        touchInfo.touchMask |= TOUCH_MASK_PRESSURE;
        touchInfo.pressure = (UINT32) (pressure * 1024);
      }
      else {
        touchInfo.pressure = 512;
      }

      if (contactAreaMajor != 0.0f && contactAreaMinor != 0.0f) {
        LONG contactWidth = std::max<LONG>(1, std::lround(contactAreaMajor * raw->touchpadWidthHimetric));
        LONG contactHeight = std::max<LONG>(1, std::lround(contactAreaMinor * raw->touchpadHeightHimetric));

        touchInfo.rcContact.left = std::max<LONG>(0, touchInfo.pointerInfo.ptHimetricLocation.x - contactWidth / 2);
        touchInfo.rcContact.right = std::min<LONG>((LONG) raw->touchpadWidthHimetric, touchInfo.pointerInfo.ptHimetricLocation.x + (contactWidth + 1) / 2);
        touchInfo.rcContact.top = std::max<LONG>(0, touchInfo.pointerInfo.ptHimetricLocation.y - contactHeight / 2);
        touchInfo.rcContact.bottom = std::min<LONG>((LONG) raw->touchpadHeightHimetric, touchInfo.pointerInfo.ptHimetricLocation.y + (contactHeight + 1) / 2);
        touchInfo.touchMask |= TOUCH_MASK_CONTACTAREA;
      }
    }
    else {
      touchInfo.pressure = 0;
      touchInfo.rcContact = {};
    }

    if (rotation != LI_ROT_UNKNOWN) {
      touchInfo.touchMask |= TOUCH_MASK_ORIENTATION;
      touchInfo.orientation = rotation;
    }
    else {
      touchInfo.orientation = 0;
    }

    return pointer;
  }

  bool
  ensure_touchpad_device(client_input_raw_t *raw, std::uint16_t deviceWidthMm, std::uint16_t deviceHeightMm, std::uint8_t eventType) {
    if (!raw->global->fnCreateSyntheticPointerDevice2 ||
        !raw->global->fnInjectSyntheticPointerInput ||
        !raw->global->fnDestroySyntheticPointerDevice) {
      BOOST_LOG(warning) << "Touchpad input requires Windows 11 CreateSyntheticPointerDevice2 support"sv;
      return false;
    }

    auto widthHimetric = touchpad_size_to_himetric(deviceWidthMm, DEFAULT_TOUCHPAD_WIDTH_HIMETRIC);
    auto heightHimetric = touchpad_size_to_himetric(deviceHeightMm, DEFAULT_TOUCHPAD_HEIGHT_HIMETRIC);

    if (!raw->touchpad) {
      if (eventType != LI_TOUCH_EVENT_CANCEL_ALL) {
        BOOST_LOG(info) << "Creating virtual touchpad input device"sv;
        synthetic_device_creation_params_t params {};
        params.pointerType = PT_TOUCHPAD;
        params.maxCount = ARRAYSIZE(raw->touchpadInfo);
        params.feedbackMode = POINTER_FEEDBACK_NONE;
        params.hMonitor = nullptr;
        params.deviceWidth = widthHimetric;
        params.deviceHeight = heightHimetric;
        params.options = synthetic_device_creation_physical_size;

        raw->touchpad = raw->global->fnCreateSyntheticPointerDevice2(&params);
        if (!raw->touchpad) {
          auto err = GetLastError();
          BOOST_LOG(warning) << "Failed to create virtual touchpad device: "sv << err;
          return false;
        }

        raw->touchpadWidthHimetric = widthHimetric;
        raw->touchpadHeightHimetric = heightHimetric;
      }
      else {
        return false;
      }
    }

    return true;
  }

  void
  finish_touchpad_contact(client_input_raw_t *raw, POINTER_TYPE_INFO *pointer, std::uint8_t eventType) {
    if (!pointer) {
      return;
    }

    pointer->touchInfo.pointerInfo.pointerFlags &= ~EDGE_TRIGGERED_POINTER_FLAGS;
    pointer->touchInfo.pointerInfo.ButtonChangeType = POINTER_CHANGE_NONE;

    if (touchpad_event_ends_pointer(eventType)) {
      auto pointerIndex = (UINT32) (pointer - raw->touchpadInfo);
      raw->touchpadClientPointerIds[pointerIndex] = 0;
      *pointer = {};
    }
  }

  bool
  inject_touchpad_frame(client_input_raw_t *raw) {
    update_touchpad_frame_timestamps(raw);
    if (!inject_synthetic_pointer_input(raw->global, raw->touchpad, raw->touchpadInfo, raw->activeTouchpadSlots)) {
      auto err = GetLastError();
      BOOST_LOG(warning) << "Failed to inject virtual touchpad input: "sv << err;
      return false;
    }

    return true;
  }

  void
  schedule_touchpad_repeat(client_input_raw_t *raw) {
    perform_touchpad_compaction(raw);

    if (raw->activeTouchpadSlots > 0) {
      raw->touchpadRepeatTask = task_pool.pushDelayed(repeat_touchpad, ISPI_REPEAT_INTERVAL, raw, raw->touchpadRepeatGeneration).task_id;
    }
  }

  void
  touchpad_update(client_input_t *input, const touchpad_input_t &touchpad) {
    auto raw = (client_input_raw_t *) input;

    std::lock_guard lock(raw->touchpadMutex);

    if (!ensure_touchpad_device(raw, touchpad.deviceWidthMm, touchpad.deviceHeightMm, touchpad.eventType)) {
      return;
    }

    if (touchpad.eventType == LI_TOUCH_EVENT_CANCEL_ALL) {
      cancel_all_active_touchpad(raw);
      return;
    }

    cancel_pending_touchpad_repeat(raw);

    auto pointer = touchpad_contact_update(
      raw,
      touchpad.eventType,
      touchpad.buttonState,
      touchpad.rotation,
      touchpad.pointerId,
      touchpad.x,
      touchpad.y,
      touchpad.pressure,
      touchpad.contactAreaMajor,
      touchpad.contactAreaMinor);

    if (!pointer || !inject_touchpad_frame(raw)) {
      return;
    }

    finish_touchpad_contact(raw, pointer, touchpad.eventType);
    schedule_touchpad_repeat(raw);
  }

  void
  touchpad_frame_update(client_input_t *input, const touchpad_frame_t &touchpad) {
    auto raw = (client_input_raw_t *) input;

    if (touchpad.contactCount > MAX_TOUCHPAD_FRAME_CONTACTS) {
      BOOST_LOG(warning) << "Touchpad frame contact count out of range ["sv << (uint32_t) touchpad.contactCount << ']';
      return;
    }

    if (touchpad.contactCount == 0) {
      return;
    }

    std::lock_guard lock(raw->touchpadMutex);

    if (!ensure_touchpad_device(raw, touchpad.deviceWidthMm, touchpad.deviceHeightMm, touchpad.contacts[0].eventType)) {
      return;
    }

    cancel_pending_touchpad_repeat(raw);

    std::array<POINTER_TYPE_INFO *, MAX_TOUCHPAD_FRAME_CONTACTS> updatedPointers {};

    for (std::uint8_t i = 0; i < touchpad.contactCount; i++) {
      auto &contact = touchpad.contacts[i];
      updatedPointers[i] = touchpad_contact_update(
        raw,
        contact.eventType,
        touchpad.buttonState,
        touchpad.rotation,
        contact.pointerId,
        contact.x,
        contact.y,
        contact.pressure,
        0.0f,
        0.0f);
    }

    if (!inject_touchpad_frame(raw)) {
      return;
    }

    for (std::uint8_t i = 0; i < touchpad.contactCount; i++) {
      finish_touchpad_contact(raw, updatedPointers[i], touchpad.contacts[i].eventType);
    }

    schedule_touchpad_repeat(raw);
  }

  /**
   * @brief Sends a touch event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param touch The touch event.
   */
  void
  touch_update(client_input_t *input, const touch_port_t &touch_port, const touch_input_t &touch) {
    auto raw = (client_input_raw_t *) input;

    // Bail if we're not running on an OS that supports virtual touch input
    if (!raw->global->fnCreateSyntheticPointerDevice ||
        !raw->global->fnInjectSyntheticPointerInput ||
        !raw->global->fnDestroySyntheticPointerDevice) {
      BOOST_LOG(warning) << "Touch input requires Windows 10 1809 or later"sv;
      return;
    }

    // If there's not already a virtual touch device, create one now
    if (!raw->touch) {
      if (touch.eventType != LI_TOUCH_EVENT_CANCEL_ALL) {
        BOOST_LOG(info) << "Creating virtual touch input device"sv;
        raw->touch = raw->global->fnCreateSyntheticPointerDevice(PT_TOUCH, ARRAYSIZE(raw->touchInfo), POINTER_FEEDBACK_DEFAULT);
        if (!raw->touch) {
          auto err = GetLastError();
          BOOST_LOG(warning) << "Failed to create virtual touch device: "sv << err;
          return;
        }
      }
      else {
        // No need to cancel anything if we had no touch input device
        return;
      }
    }

    // Cancel touch repeat callbacks
    if (raw->touchRepeatTask) {
      task_pool.cancel(raw->touchRepeatTask);
      raw->touchRepeatTask = nullptr;
    }

    // If this is a special request to cancel all touches, do that and return
    if (touch.eventType == LI_TOUCH_EVENT_CANCEL_ALL) {
      cancel_all_active_touches(raw);
      return;
    }

    // Find or allocate an entry for this touch pointer ID
    auto pointer = pointer_by_id(raw, touch.pointerId, touch.eventType);
    if (!pointer) {
      BOOST_LOG(error) << "No unused pointer entries! Cancelling all active touches!"sv;
      cancel_all_active_touches(raw);
      pointer = pointer_by_id(raw, touch.pointerId, touch.eventType);
    }

    pointer->type = PT_TOUCH;

    auto &touchInfo = pointer->touchInfo;
    touchInfo.pointerInfo.pointerType = PT_TOUCH;

    // Populate shared pointer info fields
    populate_common_pointer_info(touchInfo.pointerInfo, touch_port, touch.eventType, touch.x, touch.y);

    touchInfo.touchMask = TOUCH_MASK_NONE;

    // Pressure and contact area only apply to in-contact pointers.
    //
    // The clients also pass distance and tool size for hovers, but Windows doesn't
    // provide APIs to receive that data.
    if (touchInfo.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) {
      if (touch.pressureOrDistance != 0.0f) {
        touchInfo.touchMask |= TOUCH_MASK_PRESSURE;

        // Convert the 0.0f..1.0f float to the 0..1024 range that Windows uses
        touchInfo.pressure = (UINT32) (touch.pressureOrDistance * 1024);
      }
      else {
        // The default touch pressure is 512
        touchInfo.pressure = 512;
      }

      if (touch.contactAreaMajor != 0.0f && touch.contactAreaMinor != 0.0f) {
        // For the purposes of contact area calculation, we will assume the touches
        // are at a 45 degree angle if rotation is unknown. This will scale the major
        // axis value by width and height equally.
        float rotationAngleDegs = touch.rotation == LI_ROT_UNKNOWN ? 45 : touch.rotation;

        float majorAxisAngle = rotationAngleDegs * (M_PI / 180);
        float minorAxisAngle = majorAxisAngle + (M_PI / 2);

        // Estimate the contact rectangle
        float contactWidth = (std::cos(majorAxisAngle) * touch.contactAreaMajor) + (std::cos(minorAxisAngle) * touch.contactAreaMinor);
        float contactHeight = (std::sin(majorAxisAngle) * touch.contactAreaMajor) + (std::sin(minorAxisAngle) * touch.contactAreaMinor);

        // Convert into screen coordinates centered at the touch location and constrained by screen dimensions
        touchInfo.rcContact.left = std::max<LONG>(touch_port.offset_x, touchInfo.pointerInfo.ptPixelLocation.x - std::floor(contactWidth / 2));
        touchInfo.rcContact.right = std::min<LONG>(touch_port.offset_x + touch_port.width, touchInfo.pointerInfo.ptPixelLocation.x + std::ceil(contactWidth / 2));
        touchInfo.rcContact.top = std::max<LONG>(touch_port.offset_y, touchInfo.pointerInfo.ptPixelLocation.y - std::floor(contactHeight / 2));
        touchInfo.rcContact.bottom = std::min<LONG>(touch_port.offset_y + touch_port.height, touchInfo.pointerInfo.ptPixelLocation.y + std::ceil(contactHeight / 2));

        touchInfo.touchMask |= TOUCH_MASK_CONTACTAREA;
      }
    }
    else {
      touchInfo.pressure = 0;
      touchInfo.rcContact = {};
    }

    if (touch.rotation != LI_ROT_UNKNOWN) {
      touchInfo.touchMask |= TOUCH_MASK_ORIENTATION;
      touchInfo.orientation = touch.rotation;
    }
    else {
      touchInfo.orientation = 0;
    }

    if (!inject_synthetic_pointer_input(raw->global, raw->touch, raw->touchInfo, raw->activeTouchSlots)) {
      auto err = GetLastError();
      BOOST_LOG(warning) << "Failed to inject virtual touch input: "sv << err;
      return;
    }

    // Clear pointer flags that should only remain set for one frame
    touchInfo.pointerInfo.pointerFlags &= ~EDGE_TRIGGERED_POINTER_FLAGS;

    // If we still have an active touch, refresh the touch state periodically
    if (raw->activeTouchSlots > 1 || touchInfo.pointerInfo.pointerFlags != POINTER_FLAG_NONE) {
      raw->touchRepeatTask = task_pool.pushDelayed(repeat_touch, ISPI_REPEAT_INTERVAL, raw).task_id;
    }
  }

  /**
   * @brief Sends a pen event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param pen The pen event.
   */
  void
  pen_update(client_input_t *input, const touch_port_t &touch_port, const pen_input_t &pen) {
    auto raw = (client_input_raw_t *) input;

    // Bail if we're not running on an OS that supports virtual pen input
    if (!raw->global->fnCreateSyntheticPointerDevice ||
        !raw->global->fnInjectSyntheticPointerInput ||
        !raw->global->fnDestroySyntheticPointerDevice) {
      BOOST_LOG(warning) << "Pen input requires Windows 10 1809 or later"sv;
      return;
    }

    // If there's not already a virtual pen device, create one now
    if (!raw->pen) {
      if (pen.eventType != LI_TOUCH_EVENT_CANCEL_ALL) {
        BOOST_LOG(info) << "Creating virtual pen input device"sv;
        raw->pen = raw->global->fnCreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
        if (!raw->pen) {
          auto err = GetLastError();
          BOOST_LOG(warning) << "Failed to create virtual pen device: "sv << err;
          return;
        }
      }
      else {
        // No need to cancel anything if we had no pen input device
        return;
      }
    }

    // Cancel pen repeat callbacks
    if (raw->penRepeatTask) {
      task_pool.cancel(raw->penRepeatTask);
      raw->penRepeatTask = nullptr;
    }

    raw->penInfo.type = PT_PEN;

    auto &penInfo = raw->penInfo.penInfo;
    penInfo.pointerInfo.pointerType = PT_PEN;
    penInfo.pointerInfo.pointerId = 0;

    // Populate shared pointer info fields
    populate_common_pointer_info(penInfo.pointerInfo, touch_port, pen.eventType, pen.x, pen.y);

    // Windows only supports a single pen button, so send all buttons as the barrel button
    if (pen.penButtons) {
      penInfo.penFlags |= PEN_FLAG_BARREL;
    }
    else {
      penInfo.penFlags &= ~PEN_FLAG_BARREL;
    }

    switch (pen.toolType) {
      default:
      case LI_TOOL_TYPE_PEN:
        penInfo.penFlags &= ~PEN_FLAG_ERASER;
        break;
      case LI_TOOL_TYPE_ERASER:
        penInfo.penFlags |= PEN_FLAG_ERASER;
        break;
      case LI_TOOL_TYPE_UNKNOWN:
        // Leave tool flags alone
        break;
    }

    penInfo.penMask = PEN_MASK_NONE;

    // Windows doesn't support hover distance, so only pass pressure/distance when the pointer is in contact
    if ((penInfo.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) && pen.pressureOrDistance != 0.0f) {
      penInfo.penMask |= PEN_MASK_PRESSURE;

      // Convert the 0.0f..1.0f float to the 0..1024 range that Windows uses
      penInfo.pressure = (UINT32) (pen.pressureOrDistance * 1024);
    }
    else {
      // The default pen pressure is 0
      penInfo.pressure = 0;
    }

    if (pen.rotation != LI_ROT_UNKNOWN) {
      penInfo.penMask |= PEN_MASK_ROTATION;
      penInfo.rotation = pen.rotation;
    }
    else {
      penInfo.rotation = 0;
    }

    // We require rotation and tilt to perform the conversion to X and Y tilt angles
    if (pen.tilt != LI_TILT_UNKNOWN && pen.rotation != LI_ROT_UNKNOWN) {
      auto rotationRads = pen.rotation * (M_PI / 180.f);
      auto tiltRads = pen.tilt * (M_PI / 180.f);
      auto r = std::sin(tiltRads);
      auto z = std::cos(tiltRads);

      // Convert polar coordinates into X and Y tilt angles
      penInfo.penMask |= PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
      penInfo.tiltX = (INT32) (std::atan2(std::sin(-rotationRads) * r, z) * 180.f / M_PI);
      penInfo.tiltY = (INT32) (std::atan2(std::cos(-rotationRads) * r, z) * 180.f / M_PI);
    }
    else {
      penInfo.tiltX = 0;
      penInfo.tiltY = 0;
    }

    if (!inject_synthetic_pointer_input(raw->global, raw->pen, &raw->penInfo, 1)) {
      auto err = GetLastError();
      BOOST_LOG(warning) << "Failed to inject virtual pen input: "sv << err;
      return;
    }

    // Clear pointer flags that should only remain set for one frame
    penInfo.pointerInfo.pointerFlags &= ~EDGE_TRIGGERED_POINTER_FLAGS;

    // If we still have an active pen interaction, refresh the pen state periodically
    if (penInfo.pointerInfo.pointerFlags != POINTER_FLAG_NONE) {
      raw->penRepeatTask = task_pool.pushDelayed(repeat_pen, ISPI_REPEAT_INTERVAL, raw).task_id;
    }
  }

  void
  unicode(input_t &input, char *utf8, int size) {
    // We can do no worse than one UTF-16 character per byte of UTF-8
    WCHAR wide[size];

    int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, size, wide, size);
    if (chars <= 0) {
      return;
    }

    // Send all key down events
    for (int i = 0; i < chars; i++) {
      INPUT input {};
      input.type = INPUT_KEYBOARD;
      input.ki.wScan = wide[i];
      input.ki.dwFlags = KEYEVENTF_UNICODE;
      send_input(input);
    }

    // Send all key up events
    for (int i = 0; i < chars; i++) {
      INPUT input {};
      input.type = INPUT_KEYBOARD;
      input.ki.wScan = wide[i];
      input.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
      send_input(input);
    }
  }

  int
  alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue) {
    auto raw = (input_raw_t *) input.get();

    // Component files may have been installed while Sunshine stayed running.
    // Refresh once per allocation; the input packet hot path reads only the cache.
    ds5::refresh_component_availability();
    const bool ds5_available = raw->ds5_sidecar && ds5::component_available();
    // Client-declared preference (Sunshine /launch extension) outranks the
    // per-app and global host-side selection while client_gamepad_override is on.
    const auto client_pref = get_client_gamepad_pref();
    const bool client_declared = !client_pref.empty() && config::input.client_gamepad_override;
    auto gamepad_mode = client_declared
      ? gamepad_mode_from_preference(client_pref)
      : effective_gamepad_mode();
    const auto per_app_override = !client_declared && current_gamepad_mode.load(std::memory_order_relaxed) != 0;
    const char *selection_source = client_declared ? "client selection" : (per_app_override ? "per-app selection" : "global selection");

    if (gamepad_mode == 4 && !ds5_available) {
      BOOST_LOG(warning) << "DualSense emulation was requested by "sv << selection_source
                         << ", but its optional component is unavailable; falling back to automatic gamepad selection"sv;
      gamepad_mode = 1;
    }

    if (gamepad_mode == 4) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be DualSense controller ("
                      << selection_source << ')';
      const auto ds5_settings = ds5_config::current();
      const auto result = raw->ds5_sidecar->alloc(
        id,
        feedback_queue,
        ds5_settings.audio_haptics,
        ds5_settings.genshin_compatibility
      );
      if (result == 0) {
        feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_ACCEL, 100));
        feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_GYRO, 100));
      }
      return result;
    }

    if (!raw->vigem) {
      return 0;
    }

    VIGEM_TARGET_TYPE selectedGamepadType;

    if (gamepad_mode == 2) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be Xbox 360 controller ("sv << selection_source << ')';
      selectedGamepadType = Xbox360Wired;
    }
    else if (gamepad_mode == 3) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be DualShock 4 controller ("sv << selection_source << ')';
      selectedGamepadType = DualShock4Wired;
    }
    else if (metadata.type == LI_CTYPE_PS) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be DualShock 4 controller (auto-selected by client-reported type)"sv;
      selectedGamepadType = DualShock4Wired;
    }
    else if (metadata.type == LI_CTYPE_XBOX) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be Xbox 360 controller (auto-selected by client-reported type)"sv;
      selectedGamepadType = Xbox360Wired;
    }
    else if (config::input.motion_as_ds4 && (metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO))) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be DualShock 4 controller (auto-selected by motion sensor presence)"sv;
      selectedGamepadType = DualShock4Wired;
    }
    else if (config::input.touchpad_as_ds4 && (metadata.capabilities & LI_CCAP_TOUCHPAD)) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be DualShock 4 controller (auto-selected by touchpad presence)"sv;
      selectedGamepadType = DualShock4Wired;
    }
    else if (config::input.enable_dsu_server && (metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO))) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be DualShock 4 controller (auto-selected by DSU server enabled and motion sensor presence)"sv;
      selectedGamepadType = DualShock4Wired;
    }
    else {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be Xbox 360 controller (default)"sv;
      selectedGamepadType = Xbox360Wired;
    }

    if (selectedGamepadType == Xbox360Wired) {
      if (metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO)) {
        BOOST_LOG(warning) << "Gamepad " << id.globalIndex << " has motion sensors, but they are not usable when emulating an Xbox 360 controller"sv;
      }
      if (metadata.capabilities & LI_CCAP_TOUCHPAD) {
        BOOST_LOG(warning) << "Gamepad " << id.globalIndex << " has a touchpad, but it is not usable when emulating an Xbox 360 controller"sv;
      }
      if (metadata.capabilities & LI_CCAP_RGB_LED) {
        BOOST_LOG(warning) << "Gamepad " << id.globalIndex << " has an RGB LED, but it is not usable when emulating an Xbox 360 controller"sv;
      }
    }
    else if (selectedGamepadType == DualShock4Wired) {
      if (!(metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO))) {
        BOOST_LOG(warning) << "Gamepad " << id.globalIndex << " is emulating a DualShock 4 controller, but the client gamepad doesn't have motion sensors active"sv;
      }
      if (!(metadata.capabilities & LI_CCAP_TOUCHPAD)) {
        BOOST_LOG(warning) << "Gamepad " << id.globalIndex << " is emulating a DualShock 4 controller, but the client gamepad doesn't have a touchpad"sv;
      }
    }

    const bool back_as_touchpad_click = selectedGamepadType == DualShock4Wired &&
                                         gamepad_mode == 3 &&
                                         config::input.ds4_back_as_touchpad_click;
    return raw->vigem->alloc_gamepad_internal(id, feedback_queue, selectedGamepadType, back_as_touchpad_click);
  }

  void
  free_gamepad(input_t &input, int nr) {
    auto raw = (input_raw_t *) input.get();

    if (raw->ds5_sidecar && raw->ds5_sidecar->owns(nr)) {
      raw->ds5_sidecar->free(nr);
      return;
    }

    if (!raw->vigem) {
      return;
    }

    raw->vigem->free_target(nr);
  }

  /**
   * @brief Converts the standard button flags into X360 format.
   * @param gamepad_state The gamepad button/axis state sent from the client.
   * @return XUSB_BUTTON flags.
   */
  static XUSB_BUTTON
  x360_buttons(const gamepad_state_t &gamepad_state) {
    int buttons {};

    auto flags = gamepad_state.buttonFlags;
    if (flags & DPAD_UP) buttons |= XUSB_GAMEPAD_DPAD_UP;
    if (flags & DPAD_DOWN) buttons |= XUSB_GAMEPAD_DPAD_DOWN;
    if (flags & DPAD_LEFT) buttons |= XUSB_GAMEPAD_DPAD_LEFT;
    if (flags & DPAD_RIGHT) buttons |= XUSB_GAMEPAD_DPAD_RIGHT;
    if (flags & START) buttons |= XUSB_GAMEPAD_START;
    if (flags & BACK) buttons |= XUSB_GAMEPAD_BACK;
    if (flags & LEFT_STICK) buttons |= XUSB_GAMEPAD_LEFT_THUMB;
    if (flags & RIGHT_STICK) buttons |= XUSB_GAMEPAD_RIGHT_THUMB;
    if (flags & LEFT_BUTTON) buttons |= XUSB_GAMEPAD_LEFT_SHOULDER;
    if (flags & RIGHT_BUTTON) buttons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
    if (flags & (HOME | MISC_BUTTON)) buttons |= XUSB_GAMEPAD_GUIDE;
    if (flags & A) buttons |= XUSB_GAMEPAD_A;
    if (flags & B) buttons |= XUSB_GAMEPAD_B;
    if (flags & X) buttons |= XUSB_GAMEPAD_X;
    if (flags & Y) buttons |= XUSB_GAMEPAD_Y;

    return (XUSB_BUTTON) buttons;
  }

  /**
   * @brief Updates the X360 input report with the provided gamepad state.
   * @param gamepad The gamepad to update.
   * @param gamepad_state The gamepad button/axis state sent from the client.
   */
  static void
  x360_update_state(gamepad_context_t &gamepad, const gamepad_state_t &gamepad_state) {
    auto &report = gamepad.report.x360;

    report.wButtons = x360_buttons(gamepad_state);
    report.bLeftTrigger = gamepad_state.lt;
    report.bRightTrigger = gamepad_state.rt;
    report.sThumbLX = gamepad_state.lsX;
    report.sThumbLY = gamepad_state.lsY;
    report.sThumbRX = gamepad_state.rsX;
    report.sThumbRY = gamepad_state.rsY;
  }

  static DS4_DPAD_DIRECTIONS
  ds4_dpad(const gamepad_state_t &gamepad_state) {
    auto flags = gamepad_state.buttonFlags;
    if (flags & DPAD_UP) {
      if (flags & DPAD_RIGHT) {
        return DS4_BUTTON_DPAD_NORTHEAST;
      }
      else if (flags & DPAD_LEFT) {
        return DS4_BUTTON_DPAD_NORTHWEST;
      }
      else {
        return DS4_BUTTON_DPAD_NORTH;
      }
    }

    else if (flags & DPAD_DOWN) {
      if (flags & DPAD_RIGHT) {
        return DS4_BUTTON_DPAD_SOUTHEAST;
      }
      else if (flags & DPAD_LEFT) {
        return DS4_BUTTON_DPAD_SOUTHWEST;
      }
      else {
        return DS4_BUTTON_DPAD_SOUTH;
      }
    }

    else if (flags & DPAD_RIGHT) {
      return DS4_BUTTON_DPAD_EAST;
    }

    else if (flags & DPAD_LEFT) {
      return DS4_BUTTON_DPAD_WEST;
    }

    return DS4_BUTTON_DPAD_NONE;
  }

  /**
   * @brief Converts the standard button flags into DS4 format.
   * @param gamepad_state The gamepad button/axis state sent from the client.
   * @return DS4_BUTTONS flags.
   */
  static DS4_BUTTONS
  ds4_buttons(const gamepad_state_t &gamepad_state) {
    int buttons {};

    auto flags = gamepad_state.buttonFlags;
    if (flags & LEFT_STICK) buttons |= DS4_BUTTON_THUMB_LEFT;
    if (flags & RIGHT_STICK) buttons |= DS4_BUTTON_THUMB_RIGHT;
    if (flags & LEFT_BUTTON) buttons |= DS4_BUTTON_SHOULDER_LEFT;
    if (flags & RIGHT_BUTTON) buttons |= DS4_BUTTON_SHOULDER_RIGHT;
    if (flags & START) buttons |= DS4_BUTTON_OPTIONS;
    if (flags & BACK) buttons |= DS4_BUTTON_SHARE;
    if (flags & A) buttons |= DS4_BUTTON_CROSS;
    if (flags & B) buttons |= DS4_BUTTON_CIRCLE;
    if (flags & X) buttons |= DS4_BUTTON_SQUARE;
    if (flags & Y) buttons |= DS4_BUTTON_TRIANGLE;

    if (gamepad_state.lt > 0) buttons |= DS4_BUTTON_TRIGGER_LEFT;
    if (gamepad_state.rt > 0) buttons |= DS4_BUTTON_TRIGGER_RIGHT;

    return (DS4_BUTTONS) buttons;
  }

  static DS4_SPECIAL_BUTTONS
  ds4_special_buttons(const gamepad_context_t &gamepad, const gamepad_state_t &gamepad_state) {
    int buttons {};

    if (gamepad_state.buttonFlags & HOME) buttons |= DS4_SPECIAL_BUTTON_PS;

    // Allow either PS4/PS5 clickpad button or Xbox Series X share button to activate DS4 clickpad
    if (gamepad_state.buttonFlags & (TOUCHPAD_BUTTON | MISC_BUTTON)) buttons |= DS4_SPECIAL_BUTTON_TOUCHPAD;

    if (gamepad.back_as_touchpad_click && (gamepad_state.buttonFlags & BACK)) buttons |= DS4_SPECIAL_BUTTON_TOUCHPAD;

    return (DS4_SPECIAL_BUTTONS) buttons;
  }

  static std::uint8_t
  to_ds4_triggerX(std::int16_t v) {
    return (v + std::numeric_limits<std::uint16_t>::max() / 2 + 1) / 257;
  }

  static std::uint8_t
  to_ds4_triggerY(std::int16_t v) {
    auto new_v = -((std::numeric_limits<std::uint16_t>::max() / 2 + v - 1)) / 257;

    return new_v == 0 ? 0xFF : (std::uint8_t) new_v;
  }

  /**
   * @brief Updates the DS4 input report with the provided gamepad state.
   * @param gamepad The gamepad to update.
   * @param gamepad_state The gamepad button/axis state sent from the client.
   */
  static void
  ds4_update_state(gamepad_context_t &gamepad, const gamepad_state_t &gamepad_state) {
    auto &report = gamepad.report.ds4.Report;

    report.wButtons = static_cast<uint16_t>(ds4_buttons(gamepad_state)) | static_cast<uint16_t>(ds4_dpad(gamepad_state));
    report.bSpecial = ds4_special_buttons(gamepad, gamepad_state);

    report.bTriggerL = gamepad_state.lt;
    report.bTriggerR = gamepad_state.rt;

    report.bThumbLX = to_ds4_triggerX(gamepad_state.lsX);
    report.bThumbLY = to_ds4_triggerY(gamepad_state.lsY);

    report.bThumbRX = to_ds4_triggerX(gamepad_state.rsX);
    report.bThumbRY = to_ds4_triggerY(gamepad_state.rsY);
  }

  /**
   * @brief Sends DS4 input with updated timestamps and repeats to keep timestamp updated.
   * @details Some applications require updated timestamps values to register DS4 input.
   * @param vigem The global ViGEm context object.
   * @param nr The global gamepad index.
   */
  void
  ds4_update_ts_and_send(vigem_t *vigem, int nr) {
    auto &gamepad = vigem->gamepads[nr];

    // Cancel any pending updates. We will requeue one here when we're finished.
    if (gamepad.repeat_task) {
      task_pool.cancel(gamepad.repeat_task);
      gamepad.repeat_task = 0;
    }

    if (gamepad.gp && vigem_target_is_attached(gamepad.gp.get())) {
      auto now = std::chrono::steady_clock::now();
      auto delta_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - gamepad.last_report_ts);

      // Timestamp is reported in 5.333us units
      gamepad.report.ds4.Report.wTimestamp += (uint16_t) (delta_ns.count() / 5333);

      // Send the report to the virtual device
      auto status = vigem_target_ds4_update_ex(vigem->client.get(), gamepad.gp.get(), gamepad.report.ds4);
      if (!VIGEM_SUCCESS(status)) {
        BOOST_LOG(warning) << "Couldn't send gamepad input to ViGEm ["sv << util::hex(status).to_string_view() << ']';
        return;
      }

      // Repeat at least every 100ms to keep the 16-bit timestamp field from overflowing
      gamepad.last_report_ts = now;
      gamepad.repeat_task = task_pool.pushDelayed(ds4_update_ts_and_send, 100ms, vigem, nr).task_id;
    }
  }

  /**
   * @brief Updates virtual gamepad with the provided gamepad state.
   * @param input The input context.
   * @param nr The gamepad index to update.
   * @param gamepad_state The gamepad button/axis state sent from the client.
   */
  void
  gamepad_update(input_t &input, int nr, const gamepad_state_t &gamepad_state) {
    auto raw = (input_raw_t *) input.get();

    if (raw->ds5_sidecar && raw->ds5_sidecar->owns(nr)) {
      raw->ds5_sidecar->submit_input(nr, gamepad_state);
      return;
    }
    auto vigem = raw->vigem;

    // If there is no gamepad support
    if (!vigem) {
      return;
    }

    auto &gamepad = vigem->gamepads[nr];
    if (!gamepad.gp) {
      return;
    }

    VIGEM_ERROR status;

    if (vigem_target_get_type(gamepad.gp.get()) == Xbox360Wired) {
      x360_update_state(gamepad, gamepad_state);
      status = vigem_target_x360_update(vigem->client.get(), gamepad.gp.get(), gamepad.report.x360);
      if (!VIGEM_SUCCESS(status)) {
        BOOST_LOG(warning) << "Couldn't send gamepad input to ViGEm ["sv << util::hex(status).to_string_view() << ']';
      }
    }
    else {
      ds4_update_state(gamepad, gamepad_state);
      ds4_update_ts_and_send(vigem, nr);
    }
  }

  /**
   * @brief Sends a gamepad touch event to the OS.
   * @param input The global input context.
   * @param touch The touch event.
   */
  void
  gamepad_touch(input_t &input, const gamepad_touch_t &touch) {
    auto raw = (input_raw_t *) input.get();
    if (raw->ds5_sidecar && raw->ds5_sidecar->owns(touch.id.globalIndex)) {
      raw->ds5_sidecar->submit_touch(touch);
      return;
    }
    auto vigem = raw->vigem;

    // If there is no gamepad support
    if (!vigem) {
      return;
    }

    auto &gamepad = vigem->gamepads[touch.id.globalIndex];
    if (!gamepad.gp) {
      return;
    }

    // Touch is only supported on DualShock 4 controllers
    if (vigem_target_get_type(gamepad.gp.get()) != DualShock4Wired) {
      return;
    }

    auto &report = gamepad.report.ds4.Report;

    uint8_t pointerIndex;
    if (touch.eventType == LI_TOUCH_EVENT_DOWN) {
      if (gamepad.available_pointers & 0x1) {
        // Reserve pointer index 0 for this touch
        gamepad.pointer_id_map[touch.pointerId] = pointerIndex = 0;
        gamepad.available_pointers &= ~(1 << pointerIndex);

        // Set pointer 0 down
        report.sCurrentTouch.bIsUpTrackingNum1 &= ~0x80;
        report.sCurrentTouch.bIsUpTrackingNum1++;
      }
      else if (gamepad.available_pointers & 0x2) {
        // Reserve pointer index 1 for this touch
        gamepad.pointer_id_map[touch.pointerId] = pointerIndex = 1;
        gamepad.available_pointers &= ~(1 << pointerIndex);

        // Set pointer 1 down
        report.sCurrentTouch.bIsUpTrackingNum2 &= ~0x80;
        report.sCurrentTouch.bIsUpTrackingNum2++;
      }
      else {
        BOOST_LOG(warning) << "No more free pointer indices! Did the client miss an touch up event?"sv;
        return;
      }
    }
    else if (touch.eventType == LI_TOUCH_EVENT_CANCEL_ALL) {
      // Raise both pointers
      report.sCurrentTouch.bIsUpTrackingNum1 |= 0x80;
      report.sCurrentTouch.bIsUpTrackingNum2 |= 0x80;

      // Remove all pointer index mappings
      gamepad.pointer_id_map.clear();

      // All pointers are now available
      gamepad.available_pointers = 0x3;
    }
    else {
      auto i = gamepad.pointer_id_map.find(touch.pointerId);
      if (i == gamepad.pointer_id_map.end()) {
        BOOST_LOG(warning) << "Pointer ID not found! Did the client miss a touch down event?"sv;
        return;
      }

      pointerIndex = (*i).second;

      if (touch.eventType == LI_TOUCH_EVENT_UP || touch.eventType == LI_TOUCH_EVENT_CANCEL) {
        // Remove the pointer index mapping
        gamepad.pointer_id_map.erase(i);

        // Set pointer up
        if (pointerIndex == 0) {
          report.sCurrentTouch.bIsUpTrackingNum1 |= 0x80;
        }
        else {
          report.sCurrentTouch.bIsUpTrackingNum2 |= 0x80;
        }

        // Free the pointer index
        gamepad.available_pointers |= (1 << pointerIndex);
      }
      else if (touch.eventType != LI_TOUCH_EVENT_MOVE) {
        BOOST_LOG(warning) << "Unsupported touch event for gamepad: "sv << (uint32_t) touch.eventType;
        return;
      }
    }

    // Touchpad is 1920x943 according to ViGEm
    uint16_t x = touch.x * 1920;
    uint16_t y = touch.y * 943;
    uint8_t touchData[] = {
      (uint8_t) (x & 0xFF),  // Low 8 bits of X
      (uint8_t) (((x >> 8) & 0x0F) | ((y & 0x0F) << 4)),  // High 4 bits of X and low 4 bits of Y
      (uint8_t) (((y >> 4) & 0xFF))  // High 8 bits of Y
    };

    report.sCurrentTouch.bPacketCounter++;
    if (touch.eventType != LI_TOUCH_EVENT_CANCEL_ALL) {
      if (pointerIndex == 0) {
        memcpy(report.sCurrentTouch.bTouchData1, touchData, sizeof(touchData));
      }
      else {
        memcpy(report.sCurrentTouch.bTouchData2, touchData, sizeof(touchData));
      }
    }

    ds4_update_ts_and_send(vigem, touch.id.globalIndex);
  }

  /**
   * @brief Sends a gamepad motion event to the OS.
   * @param input The global input context.
   * @param motion The motion event.
   */
  void
  gamepad_motion(input_t &input, const gamepad_motion_t &motion) {
    auto raw = (input_raw_t *) input.get();
    if (raw->ds5_sidecar && raw->ds5_sidecar->owns(motion.id.globalIndex)) {
      raw->ds5_sidecar->submit_motion(motion);
      return;
    }
    auto vigem = raw->vigem;

    // If there is no gamepad support
    if (!vigem) {
      return;
    }

    auto &gamepad = vigem->gamepads[motion.id.globalIndex];
    if (!gamepad.gp) {
      return;
    }

    // Motion is only supported on DualShock 4 controllers
    if (vigem_target_get_type(gamepad.gp.get()) != DualShock4Wired) {
      return;
    }

    ds4_update_motion(gamepad, motion.motionType, motion.x, motion.y, motion.z);
    ds4_update_ts_and_send(vigem, motion.id.globalIndex);

    if (!raw->dsu_server && config::input.enable_dsu_server) {
      raw->init_dsu_server();
    }

    if (raw->dsu_server) {
      if (motion.motionType == LI_MOTION_TYPE_ACCEL) {
        // 发送加速度数据
        BOOST_LOG(debug) << "发送加速度数据到DSU服务器";
        raw->dsu_server->send_motion_data(motion.id.globalIndex,
          motion.x, motion.y, motion.z,
          0.0f, 0.0f, 0.0f);
      }
      else if (motion.motionType == LI_MOTION_TYPE_GYRO) {
        // 发送陀螺仪数据
        BOOST_LOG(debug) << "发送陀螺仪数据到DSU服务器";
        raw->dsu_server->send_motion_data(motion.id.globalIndex,
          0.0f, 0.0f, 0.0f,
          motion.x, motion.y, motion.z);
      }
      else {
        BOOST_LOG(debug) << "未知的运动数据类型: " << (int) motion.motionType;
      }
    }
    else {
      BOOST_LOG(warning) << "DSU服务器未初始化，无法发送运动数据";
    }
  }

  /**
   * @brief Sends a gamepad battery event to the OS.
   * @param input The global input context.
   * @param battery The battery event.
   */
  void
  gamepad_battery(input_t &input, const gamepad_battery_t &battery) {
    auto raw = (input_raw_t *) input.get();
    if (raw->ds5_sidecar && raw->ds5_sidecar->owns(battery.id.globalIndex)) {
      raw->ds5_sidecar->submit_battery(battery);
      return;
    }
    auto vigem = raw->vigem;

    // If there is no gamepad support
    if (!vigem) {
      return;
    }

    auto &gamepad = vigem->gamepads[battery.id.globalIndex];
    if (!gamepad.gp) {
      return;
    }

    // Battery is only supported on DualShock 4 controllers
    if (vigem_target_get_type(gamepad.gp.get()) != DualShock4Wired) {
      return;
    }

    // For details on the report format of these battery level fields, see:
    // https://github.com/torvalds/linux/blob/946c6b59c56dc6e7d8364a8959cb36bf6d10bc37/drivers/hid/hid-playstation.c#L2305-L2314

    auto &report = gamepad.report.ds4.Report;

    // Update the battery state if it is known
    switch (battery.state) {
      case LI_BATTERY_STATE_CHARGING:
      case LI_BATTERY_STATE_DISCHARGING:
        if (battery.state == LI_BATTERY_STATE_CHARGING) {
          report.bBatteryLvlSpecial |= 0x10;  // Connected via USB
        }
        else {
          report.bBatteryLvlSpecial &= ~0x10;  // Not connected via USB
        }

        // If there was a special battery status set before, clear that and
        // initialize the battery level to 50%. It will be overwritten below
        // if the actual percentage is known.
        if ((report.bBatteryLvlSpecial & 0xF) > 0xA) {
          report.bBatteryLvlSpecial = (report.bBatteryLvlSpecial & ~0xF) | 0x5;
        }
        break;

      case LI_BATTERY_STATE_FULL:
        report.bBatteryLvlSpecial = 0x1B;  // USB + Battery Full
        report.bBatteryLvl = 0xFF;
        break;

      case LI_BATTERY_STATE_NOT_PRESENT:
      case LI_BATTERY_STATE_NOT_CHARGING:
        report.bBatteryLvlSpecial = 0x1F;  // USB + Charging Error
        break;

      default:
        break;
    }

    // Update the battery level if it is known
    if (battery.percentage != LI_BATTERY_PERCENTAGE_UNKNOWN) {
      report.bBatteryLvl = battery.percentage * 255 / 100;

      // Don't overwrite low nibble if there's a special status there (see above)
      if ((report.bBatteryLvlSpecial & 0x10) && (report.bBatteryLvlSpecial & 0xF) <= 0xA) {
        report.bBatteryLvlSpecial = (report.bBatteryLvlSpecial & ~0xF) | ((battery.percentage + 5) / 10);
      }
    }

    ds4_update_ts_and_send(vigem, battery.id.globalIndex);
  }

  void
  freeInput(void *p) {
    auto input = (input_raw_t *) p;

    delete input;
  }

  std::vector<supported_gamepad_t> &
  supported_gamepads(input_t *input) {
    if (!input) {
      static std::vector gps {
        supported_gamepad_t { "auto", true, "" },
        supported_gamepad_t { "x360", false, "" },
        supported_gamepad_t { "ds4", false, "" },
        supported_gamepad_t { "ds5", false, "" },
        supported_gamepad_t { "switch", true, "" },
      };

      return gps;
    }

    auto vigem = ((input_raw_t *) input)->vigem;
    auto dsu_server = ((input_raw_t *) input)->dsu_server;
    auto enabled = vigem != nullptr;
    auto switch_enabled = dsu_server != nullptr;
    auto ds5_available = ((input_raw_t *) input)->ds5_sidecar->configured();
    auto reason = enabled ? "" : "gamepads.vigem-not-available";
    auto switch_reason = switch_enabled ? "" : "gamepads.motion-server-not-available";

    // ds4 == ps4
    // This function is queried after runtime configuration changes too. Keep
    // per-thread stable backing storage (the API returns a reference), while
    // avoiding cross-thread writes when HTTP/UI callers query concurrently.
    thread_local std::vector<supported_gamepad_t> gps(5);
    gps[0] = { "auto", true, reason };
    gps[1] = { "x360", enabled, reason };
    gps[2] = { "ds4", enabled, reason };
    gps[3] = { "ds5", ds5_available, ds5_available ? "" : "gamepads.ds5-sidecar-not-available" };
    gps[4] = { "switch", switch_enabled, switch_reason };

    for (auto &[name, is_enabled, reason_disabled] : gps) {
      if (!is_enabled) {
        BOOST_LOG(warning) << "Gamepad " << name << " is disabled due to " << reason_disabled;
      }
    }

    return gps;
  }

  /**
   * @brief Returns the supported platform capabilities to advertise to the client.
   * @return Capability flags.
   */
  platform_caps::caps_t
  get_capabilities() {
    platform_caps::caps_t caps = 0;

    // We support controller touchpad input as long as we're not emulating X360
    if (effective_gamepad_mode() != 2) {
      caps |= platform_caps::controller_touch;
    }

    const auto ds5_settings = ds5_config::current();
    const auto client_pref = get_client_gamepad_pref();
    const bool client_declared = !client_pref.empty() && config::input.client_gamepad_override;
    const auto gamepad_mode = client_declared
      ? gamepad_mode_from_preference(client_pref)
      : effective_gamepad_mode();
    if (gamepad_mode == 4 && ds5_settings.audio_haptics &&
        ds5::refresh_component_availability()) {
      caps |= platform_caps::ds5_haptics_pcm;
    }

    // We support pen and touch input on Win10 1809+
    if (GetProcAddress(GetModuleHandleA("user32.dll"), "CreateSyntheticPointerDevice") != nullptr) {
      if (config::input.native_pen_touch) {
        caps |= platform_caps::pen_touch;
      }
    }
    else {
      BOOST_LOG(warning) << "Touch input requires Windows 10 1809 or later"sv;
    }

    if (get_create_synthetic_pointer_device2_proc() != nullptr) {
      caps |= platform_caps::touchpad;
      if (config::input.native_touchpad_optimization) {
        caps |= platform_caps::touchpad_frame;
      }
    }
    else {
      BOOST_LOG(warning) << "Touchpad input requires Windows 11 CreateSyntheticPointerDevice2 support"sv;
    }

    return caps;
  }
}  // namespace platf
