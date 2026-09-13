/**
 * @file src/config.h
 * @brief Declarations for the configuration of Sunshine.
 */
#pragma once

#include <bitset>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "nvenc/nvenc_config.h"

namespace config {
  // track modified config options
  inline std::unordered_map<std::string, std::string> modified_config_settings;

  struct video_t {
    // ffmpeg params
    int qp;  // higher == more compression and less quality

    int hevc_mode;
    int av1_mode;

    int max_bitrate;  // Maximum bitrate, sets ceiling in kbps for bitrate requested from client
    int min_threads;  // Minimum number of threads/slices for CPU encoding
    struct {
      std::string sw_preset;
      std::string sw_tune;
      std::optional<int> svtav1_preset;
    } sw;

    nvenc::nvenc_config nv;
    bool nv_realtime_hags;
    bool nv_opengl_vulkan_on_dxgi;
    bool nv_sunshine_high_power_mode;
    bool vdd_keep_enabled;
    /** When true, after stream end if no display is found (headless), create Zako VDD automatically. Default false. */
    bool vdd_headless_create_enabled;
    /** When true, reuse existing VDD on client switch instead of destroying and recreating. Default true. */
    bool vdd_reuse;
    /** When true, Zako Direct Capture borrows VDD shared textures instead of copying into Sunshine-owned capture textures. */
    bool vdd_borrowed_texture;
    /** Automatically validate and expose Vulkan HDR colorspaces for HDR VDD sessions. */
    bool vdd_vulkan_hdr_bridge;

    struct {
      int preset;
      int multipass;
      int h264_coder;
      int aq;
      int vbv_percentage_increase;
    } nv_legacy;

    struct {
      std::optional<int> qsv_preset;
      std::optional<int> qsv_cavlc;
      bool qsv_slow_hevc;
    } qsv;

    struct {
      std::optional<int> amd_usage_h264;
      std::optional<int> amd_usage_hevc;
      std::optional<int> amd_usage_av1;
      std::optional<int> amd_rc_h264;
      std::optional<int> amd_rc_hevc;
      std::optional<int> amd_rc_av1;
      std::optional<int> amd_enforce_hrd;
      std::optional<int> amd_quality_h264;
      std::optional<int> amd_quality_hevc;
      std::optional<int> amd_quality_av1;
      std::optional<int> amd_preanalysis;
      std::optional<int> amd_vbaq;
      int amd_coder;
      int amd_qvbr_quality = 23;  // QVBR quality level 1-51 (higher=better, default=23)
      int amd_ltr_frames = 0;  // LTR frames for RFI (0=disabled by default; matches FFmpeg amfenc behavior to avoid static-region color blocks)
      int amd_slices_per_frame = 0;  // Slices/tiles per frame (0=client decides, 1-4=minimum)
      bool amd_avcodec_compat = false;  // Optional AVCodec-like AMF adapter; false keeps the clean standalone path.
      std::optional<bool> amd_multi_hw_instance;
      // The properties below historically had aggressive hardcoded defaults that
      // forced AMF code paths FFmpeg never touches (HIGH_MOTION_QUALITY_BOOST=on,
      // INPUT_QUEUE_SIZE=1, LOWLATENCY_MODE=on, AV1 LOWEST_LATENCY). Those paths
      // expose latent AMD driver bugs (e.g. RDNA4 Adrenalin 26.5.x freeze after
      // ~minutes, AlkaidLab/foundation-sunshine#666). Default is nullopt =
      // "do not call SetProperty" so the driver picks its own default, matching
      // FFmpeg amfenc behavior. Users can still opt in via the WebUI.
      std::optional<bool> amd_high_motion_qb;
      std::optional<bool> amd_lowlatency_mode;
      // Standalone: AMF INPUT_QUEUE_SIZE. AVCodec compatibility: async_depth cap.
      std::optional<int> amd_input_queue_size;   // 1-16
      std::optional<int> amd_av1_latency_mode;   // AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE_*
    } amd;

    struct {
      int vt_allow_sw;
      int vt_require_sw;
      int vt_realtime;
      int vt_coder;
    } vt;

    struct {
      bool strict_rc_buffer;
    } vaapi;

    std::string capture;
    std::string encoder;
    std::string adapter_name;

    struct display_mode_remapping_t {
      std::string type;
      std::string received_resolution;
      std::string received_fps;
      std::string final_resolution;
      std::string final_refresh_rate;
    };

    std::string output_name;
    std::string capture_target;  // "display" or "window" - determines whether to capture display or window
    std::string window_title;     // Window title to capture when capture_target="window"
    bool capture_cursor;          // Whether to composite the host mouse cursor into the stream (toggle: Ctrl+Alt+Shift+N)
    int display_device_prep;
    int resolution_change;
    std::string manual_resolution;
    int refresh_rate_change;
    std::string manual_refresh_rate;
    int hdr_prep;
    std::vector<display_mode_remapping_t> display_mode_remapping;
    bool variable_refresh_rate;  // Allow video stream framerate to match render framerate for VRR support
    int minimum_fps_target;  // Minimum FPS target (0 = auto, 1-1000 = minimum FPS to maintain)
    bool input_activity_boost;  // Temporarily raise encoding cadence after local input while VRR is active
    int input_activity_boost_fps;  // Minimum FPS floor to maintain during the input activity boost window
    int input_activity_boost_window_ms;  // Duration of the input activity boost window in milliseconds
    std::string downscaling_quality;  // Downscaling quality: "fast" (bilinear+8pt), "balanced" (bicubic), "high_quality" (future: lanczos)
    std::string hdr_luminance_analysis;  // Per-frame HDR analysis: "auto", "on", "off"
    std::string capture_compute_shader;  // GPU frame conversion: "auto", "on", "off"
    bool wgc_disable_secure_desktop;  // Auto-disable UAC secure desktop when using WGC capture
    bool dynamic_resolution_follow_display;  // If true, follow mid-stream host display resolution changes and notify client via extension; if false, keep initial stream resolution and let scaler handle changes (compatible with legacy clients like PSVita Moonlight that don't implement the extension)
  };

  struct audio_t {
    std::string sink;
    std::string virtual_sink;
    std::string microphone_redirect_backend;
    bool stream;
    bool stream_mic;
    bool install_steam_drivers;
  };

  constexpr int ENCRYPTION_MODE_NEVER = 0;  // Never use video encryption, even if the client supports it
  constexpr int ENCRYPTION_MODE_OPPORTUNISTIC = 1;  // Use video encryption if available, but stream without it if not supported
  constexpr int ENCRYPTION_MODE_MANDATORY = 2;  // Always use video encryption and refuse clients that can't encrypt

  struct stream_t {
    std::chrono::milliseconds ping_timeout;

    std::string file_apps;

    int fec_percentage;

    // Video encryption settings for LAN and WAN streams
    int lan_encryption_mode;
    int wan_encryption_mode;
  };

  // Sleep mode options for PC sleep command
  constexpr int SLEEP_MODE_SUSPEND = 0;   // S3 Sleep (traditional suspend via SetSuspendState)
  constexpr int SLEEP_MODE_HIBERNATE = 1;  // S4 Hibernate (deeper sleep, saves to disk)
  constexpr int SLEEP_MODE_AWAY = 2;       // Away Mode (display off, system stays on, instant wake)

  struct nvhttp_t {
    // Could be any of the following values:
    // pc|lan|wan
    std::string origin_web_ui_allowed;

    std::string pkey;
    std::string cert;

    std::string sunshine_name;
    std::string clients;

    std::string file_state;
    std::string file_mappings;
    std::uint16_t file_mapping_port;
    bool usb_forwarding_enabled;
    std::uint16_t usb_forwarding_port;  // 0: main port + 7; otherwise explicit override

    std::string external_ip;
    std::vector<std::string> resolutions;
    std::vector<std::string> fps;  // 支持小数刷新率，如 "119.88"

    int sleep_mode;  // Sleep mode: 0=suspend(S3), 1=hibernate(S4), 2=away_mode

    int pair_max_attempts;  // Max PIN pairing attempts per IP within 60s window. 0 disables limiting.

    // Signed, warning-only client fingerprint rule feed.
    bool client_fingerprint_remote_rules;
    std::string client_fingerprint_rules_url;
    std::string client_fingerprint_rules_certificate;
    int client_fingerprint_rules_refresh_hours;
  };

  struct input_t {
    std::unordered_map<int, int> keybindings;

    std::chrono::milliseconds back_button_timeout;
    std::chrono::milliseconds key_repeat_delay;
    std::chrono::duration<double> key_repeat_period;

    std::string gamepad;
    bool ds4_back_as_touchpad_click;
    bool motion_as_ds4;
    bool touchpad_as_ds4;
    bool ds5_inputtino_randomize_mac;
    bool enable_dsu_server;
    uint16_t dsu_server_port;

    bool keyboard;
    bool mouse;
    bool controller;

    bool always_send_scancodes;

    bool high_resolution_scrolling;
    bool native_pen_touch;
    bool native_touchpad_optimization;
    bool virtual_mouse;
    bool amf_draw_mouse_cursor;
    bool clipboard_sync;  ///< Bidirectional clipboard sync (text + single image). On by default; effective only when the user-session GUI agent is alive. Set to false to force-disable.
    bool client_gamepad_override;  ///< Honor the client-declared controller type carried on the /launch query (Sunshine extension). On by default; set false to keep host-side selection authoritative.
  };

  namespace flag {
    enum flag_e : std::size_t {
      PIN_STDIN = 0,  ///< Read PIN from stdin instead of http
      FRESH_STATE,  ///< Do not load or save state
      FORCE_VIDEO_HEADER_REPLACE,  ///< force replacing headers inside video data
      UPNP,  ///< Try Universal Plug 'n Play
      CONST_PIN,  ///< Use "universal" pin
      CLOSE_VERIFY_SAFE,  ///< Close verify certificate chain safely
      MDNS_BROADCAST,  ///< Enable mDNS broadcast
      FLAG_SIZE  ///< Number of flags
    };
  }

  struct prep_cmd_t {
    prep_cmd_t(std::string &&do_cmd, std::string &&undo_cmd, bool &&elevated):
        do_cmd(std::move(do_cmd)), undo_cmd(std::move(undo_cmd)), elevated(std::move(elevated)) {}
    explicit prep_cmd_t(std::string &&do_cmd, bool &&elevated):
        do_cmd(std::move(do_cmd)), elevated(std::move(elevated)) {}
    std::string do_cmd;
    std::string undo_cmd;
    bool elevated;
  };
  struct sunshine_t {
    std::string locale;
    std::string tray_locale;
    int min_log_level;
    std::bitset<flag::FLAG_SIZE> flags;
    std::string credentials_file;

    std::string username;
    std::string password;
    std::string salt;

    std::string config_file;

    struct cmd_t {
      std::string name;
      int argc;
      char **argv;
    } cmd;

    std::uint16_t port;
    std::string address_family;
    std::string bind_address;

    std::string log_file;
    bool restore_log;  // 是否恢复日志文件（true=恢复，false=覆盖）
    int max_log_size_mb;  // 日志文件最大大小（MB），超过后自动轮转，0=不限制
    bool notify_pre_releases;
    bool system_tray;
    std::vector<prep_cmd_t> prep_cmds;
  };

  extern video_t video;
  extern audio_t audio;
  extern stream_t stream;
  extern nvhttp_t nvhttp;
  extern input_t input;
  extern sunshine_t sunshine;

  int
  parse(int argc, char *argv[]);
  std::unordered_map<std::string, std::string>
  parse_config(const std::string_view &file_content);

  bool
  update_config(const std::map<std::string, std::string> &updates);

  std::optional<std::map<std::string, std::string>>
  get_config_snapshot();

  bool
  update_full_config(const std::map<std::string, std::string> &fullConfig);

  /**
   * Return a synchronized snapshot of the serialized per-client settings.
   */
  std::string
  get_clients_config();

  /**
   * Return the per-client touch-keyboard preference: true when the client
   * entry exists and its touch.enabled is true.  Unknown clients and
   * entries without a touch object default to false.
   */
  bool
  get_client_touch_keyboard_enabled(const std::string &uuid);

  /**
   * Persist per-client settings and publish them to the running process.
   * An unchanged value is treated as a successful save.
   */
  bool
  save_clients_config(const std::string &clients);
}  // namespace config
