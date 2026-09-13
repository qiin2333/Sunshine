/**
 * @file src/config.cpp
 * @brief Definitions for the configuration of Sunshine.
 */
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "config.h"
#include "entry_handler.h"
#include "file_handler.h"
#include "logging.h"
#include "nvhttp.h"
#include "rtsp.h"
#include "utility.h"
#include "globals.h"

#include "display_device/parsed_config.h"
#include "platform/common.h"

#ifdef _WIN32
  #include <shellapi.h>
  #include "platform/windows/misc.h"
#endif

#ifndef __APPLE__
  // For NVENC legacy constants
  #include <ffnvcodec/nvEncodeAPI.h>
#endif

namespace fs = std::filesystem;
using namespace std::literals;

#define CA_DIR "credentials"
#define PRIVATE_KEY_FILE CA_DIR "/cakey.pem"
#define CERTIFICATE_FILE CA_DIR "/cacert.pem"

#define APPS_JSON_PATH file_handler::path_to_utf8(platf::appdata() / "apps.json")
namespace config {

  namespace {
    std::mutex config_file_mutex;

    std::optional<std::string>
    read_config_file_contents() {
      const auto path = file_handler::path_from_utf8(sunshine.config_file);
      std::error_code error;
      const auto exists = fs::exists(path, error);
      if (error) {
        BOOST_LOG(warning) << "Failed to inspect config file: " << error.message();
        return std::nullopt;
      }
      if (!exists) {
        return std::string {};
      }

      std::ifstream input(path, std::ios::binary);
      if (!input.is_open()) {
        BOOST_LOG(warning) << "Failed to open config file for reading"sv;
        return std::nullopt;
      }
      std::string contents {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
      };
      if (input.bad()) {
        BOOST_LOG(warning) << "Failed while reading config file"sv;
        return std::nullopt;
      }
      return contents;
    }
  }

  namespace nv {

    nvenc::nvenc_two_pass
    twopass_from_view(const std::string_view &preset) {
      if (preset == "disabled") return nvenc::nvenc_two_pass::disabled;
      if (preset == "quarter_res") return nvenc::nvenc_two_pass::quarter_resolution;
      if (preset == "full_res") return nvenc::nvenc_two_pass::full_resolution;
      BOOST_LOG(warning) << "config: unknown nvenc_twopass value: " << preset;
      return nvenc::nvenc_two_pass::quarter_resolution;
    }

    nvenc::nvenc_split_frame_encoding
    split_encode_from_view(const std::string_view &preset) {
      using enum nvenc::nvenc_split_frame_encoding;
      if (preset == "disabled") return disabled;
      if (preset == "driver_decides") return driver_decides;
      if (preset == "enabled") return force_enabled;
      if (preset == "two_strips") return two_strips;
      if (preset == "three_strips") return three_strips;
      if (preset == "four_strips") return four_strips;
      BOOST_LOG(warning) << "config: unknown nvenc_split_encode value: " << preset;
      return driver_decides;
    }

    nvenc::nvenc_lookahead_level
    lookahead_level_from_view(const std::string_view &level) {
      using enum nvenc::nvenc_lookahead_level;
      if (level == "disabled" || level == "0") return disabled;
      if (level == "1") return level_1;
      if (level == "2") return level_2;
      if (level == "3") return level_3;
      if (level == "autoselect" || level == "auto") return autoselect;
      BOOST_LOG(warning) << "config: unknown nvenc_lookahead_level value: " << level;
      return disabled;
    }

    nvenc::nvenc_temporal_filter_level
    temporal_filter_level_from_view(const std::string_view &level) {
      using enum nvenc::nvenc_temporal_filter_level;
      if (level == "disabled" || level == "0") return disabled;
      if (level == "4") return level_4;
      BOOST_LOG(warning) << "config: unknown nvenc_temporal_filter_level value: " << level;
      return disabled;
    }

    nvenc::nvenc_rate_control_mode
    rate_control_mode_from_view(const std::string_view &mode) {
      using enum nvenc::nvenc_rate_control_mode;
      if (mode == "cbr") return cbr;
      if (mode == "vbr") return vbr;
      BOOST_LOG(warning) << "config: unknown nvenc_rate_control_mode value: " << mode;
      return cbr;
    }

  }  // namespace nv

  namespace amd {
#if !defined(_WIN32) || defined(DOXYGEN)
  // values accurate as of 27/12/2022, but aren't strictly necessary for MacOS build
  #define AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED 100
  #define AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_QUALITY 30
  #define AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_BALANCED 70
  #define AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED 10
  #define AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY 0
  #define AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED 5
  #define AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED 1
  #define AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY 2
  #define AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED 0
  #define AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CONSTANT_QP 0
  #define AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR 3
  #define AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR 2
  #define AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR 1
  #define AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CONSTANT_QP 0
  #define AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR 3
  #define AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR 2
  #define AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR 1
  #define AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP 0
  #define AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR 1
  #define AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR 2
  #define AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR 3
  #define AMF_VIDEO_ENCODER_AV1_USAGE_TRANSCODING 0
  #define AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY 1
  #define AMF_VIDEO_ENCODER_AV1_USAGE_ULTRA_LOW_LATENCY 2
  #define AMF_VIDEO_ENCODER_AV1_USAGE_WEBCAM 3
  #define AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY_HIGH_QUALITY 5
  #define AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING 0
  #define AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY 1
  #define AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY 2
  #define AMF_VIDEO_ENCODER_HEVC_USAGE_WEBCAM 3
  #define AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY_HIGH_QUALITY 5
  #define AMF_VIDEO_ENCODER_USAGE_TRANSCODING 0
  #define AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY 1
  #define AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY 2
  #define AMF_VIDEO_ENCODER_USAGE_WEBCAM 3
  #define AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY_HIGH_QUALITY 5
  #define AMF_VIDEO_ENCODER_UNDEFINED 0
  #define AMF_VIDEO_ENCODER_CABAC 1
  #define AMF_VIDEO_ENCODER_CALV 2
#else
  #ifdef _GLIBCXX_USE_C99_INTTYPES
    #undef _GLIBCXX_USE_C99_INTTYPES
  #endif
  #include <AMF/components/VideoEncoderAV1.h>
  #include <AMF/components/VideoEncoderHEVC.h>
  #include <AMF/components/VideoEncoderVCE.h>
#endif

    enum class quality_av1_e : int {
      speed = AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED,  ///< Speed preset
      quality = AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_QUALITY,  ///< Quality preset
      balanced = AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_BALANCED  ///< Balanced preset
    };

    enum class quality_hevc_e : int {
      speed = AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED,  ///< Speed preset
      quality = AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY,  ///< Quality preset
      balanced = AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED  ///< Balanced preset
    };

    enum class quality_h264_e : int {
      speed = AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED,  ///< Speed preset
      quality = AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY,  ///< Quality preset
      balanced = AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED  ///< Balanced preset
    };

    enum class rc_av1_e : int {
      cbr = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR,  ///< CBR
      cqp = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CONSTANT_QP,  ///< CQP
      vbr_latency = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR,  ///< VBR with latency constraints
      vbr_peak = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR,  ///< VBR with peak constraints
      qvbr = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_QUALITY_VBR,  ///< Quality VBR
      hqvbr = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_HIGH_QUALITY_VBR,  ///< High Quality VBR
      hqcbr = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_HIGH_QUALITY_CBR  ///< High Quality CBR
    };

    enum class rc_hevc_e : int {
      cbr = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR,  ///< CBR
      cqp = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CONSTANT_QP,  ///< CQP
      vbr_latency = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR,  ///< VBR with latency constraints
      vbr_peak = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR,  ///< VBR with peak constraints
      qvbr = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_QUALITY_VBR,  ///< Quality VBR
      hqvbr = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_HIGH_QUALITY_VBR,  ///< High Quality VBR
      hqcbr = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_HIGH_QUALITY_CBR  ///< High Quality CBR
    };

    enum class rc_h264_e : int {
      cbr = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR,  ///< CBR
      cqp = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP,  ///< CQP
      vbr_latency = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR,  ///< VBR with latency constraints
      vbr_peak = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR,  ///< VBR with peak constraints
      qvbr = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_QUALITY_VBR,  ///< Quality VBR
      hqvbr = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_HIGH_QUALITY_VBR,  ///< High Quality VBR
      hqcbr = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_HIGH_QUALITY_CBR  ///< High Quality CBR
    };

    enum class usage_av1_e : int {
      transcoding = AMF_VIDEO_ENCODER_AV1_USAGE_TRANSCODING,  ///< Transcoding preset
      webcam = AMF_VIDEO_ENCODER_AV1_USAGE_WEBCAM,  ///< Webcam preset
      lowlatency_high_quality = AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY_HIGH_QUALITY,  ///< Low latency high quality preset
      lowlatency = AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY,  ///< Low latency preset
      ultralowlatency = AMF_VIDEO_ENCODER_AV1_USAGE_ULTRA_LOW_LATENCY  ///< Ultra low latency preset
    };

    enum class usage_hevc_e : int {
      transcoding = AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING,  ///< Transcoding preset
      webcam = AMF_VIDEO_ENCODER_HEVC_USAGE_WEBCAM,  ///< Webcam preset
      lowlatency_high_quality = AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY_HIGH_QUALITY,  ///< Low latency high quality preset
      lowlatency = AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY,  ///< Low latency preset
      ultralowlatency = AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY  ///< Ultra low latency preset
    };

    enum class usage_h264_e : int {
      transcoding = AMF_VIDEO_ENCODER_USAGE_TRANSCODING,  ///< Transcoding preset
      webcam = AMF_VIDEO_ENCODER_USAGE_WEBCAM,  ///< Webcam preset
      lowlatency_high_quality = AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY_HIGH_QUALITY,  ///< Low latency high quality preset
      lowlatency = AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY,  ///< Low latency preset
      ultralowlatency = AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY  ///< Ultra low latency preset
    };

    enum coder_e : int {
      _auto = AMF_VIDEO_ENCODER_UNDEFINED,  ///< Auto
      cabac = AMF_VIDEO_ENCODER_CABAC,  ///< CABAC
      cavlc = AMF_VIDEO_ENCODER_CALV  ///< CAVLC
    };

    template <class T>
    std::optional<int>
    quality_from_view(const std::string_view &quality_type, const std::optional<int>(&original)) {
#define _CONVERT_(x) \
  if (quality_type == #x##sv) return (int) T::x
      _CONVERT_(balanced);
      _CONVERT_(quality);
      _CONVERT_(speed);
#undef _CONVERT_
      return original;
    }

    template <class T>
    std::optional<int>
    rc_from_view(const std::string_view &rc, const std::optional<int>(&original)) {
#define _CONVERT_(x) \
  if (rc == #x##sv) return (int) T::x
      _CONVERT_(cbr);
      _CONVERT_(cqp);
      _CONVERT_(vbr_latency);
      _CONVERT_(vbr_peak);
      _CONVERT_(qvbr);
      _CONVERT_(hqvbr);
      _CONVERT_(hqcbr);
#undef _CONVERT_
      return original;
    }

    template <class T>
    std::optional<int>
    usage_from_view(const std::string_view &usage, const std::optional<int>(&original)) {
#define _CONVERT_(x) \
  if (usage == #x##sv) return (int) T::x
      _CONVERT_(lowlatency);
      _CONVERT_(lowlatency_high_quality);
      _CONVERT_(transcoding);
      _CONVERT_(ultralowlatency);
      _CONVERT_(webcam);
#undef _CONVERT_
      return original;
    }

    int
    coder_from_view(const std::string_view &coder) {
      if (coder == "auto"sv) return _auto;
      if (coder == "cabac"sv || coder == "ac"sv) return cabac;
      if (coder == "cavlc"sv || coder == "vlc"sv) return cavlc;

      return _auto;
    }
  }  // namespace amd

  namespace qsv {
    enum preset_e : int {
      veryslow = 1,  ///< veryslow preset
      slower = 2,  ///< slower preset
      slow = 3,  ///< slow preset
      medium = 4,  ///< medium preset
      fast = 5,  ///< fast preset
      faster = 6,  ///< faster preset
      veryfast = 7  ///< veryfast preset
    };

    enum cavlc_e : int {
      _auto = false,  ///< Auto
      enabled = true,  ///< Enabled
      disabled = false  ///< Disabled
    };

    std::optional<int>
    preset_from_view(const std::string_view &preset) {
#define _CONVERT_(x) \
  if (preset == #x##sv) return x
      _CONVERT_(veryslow);
      _CONVERT_(slower);
      _CONVERT_(slow);
      _CONVERT_(medium);
      _CONVERT_(fast);
      _CONVERT_(faster);
      _CONVERT_(veryfast);
#undef _CONVERT_
      return std::nullopt;
    }

    std::optional<int>
    coder_from_view(const std::string_view &coder) {
      if (coder == "auto"sv) return _auto;
      if (coder == "cabac"sv || coder == "ac"sv) return disabled;
      if (coder == "cavlc"sv || coder == "vlc"sv) return enabled;
      return std::nullopt;
    }

  }  // namespace qsv

  namespace vt {

    enum coder_e : int {
      _auto = 0,  ///< Auto
      cabac,  ///< CABAC
      cavlc  ///< CAVLC
    };

    int
    coder_from_view(const std::string_view &coder) {
      if (coder == "auto"sv) return _auto;
      if (coder == "cabac"sv || coder == "ac"sv) return cabac;
      if (coder == "cavlc"sv || coder == "vlc"sv) return cavlc;

      return -1;
    }

    int
    allow_software_from_view(const std::string_view &software) {
      if (software == "allowed"sv || software == "forced") return 1;

      return 0;
    }

    int
    force_software_from_view(const std::string_view &software) {
      if (software == "forced") return 1;

      return 0;
    }

    int
    rt_from_view(const std::string_view &rt) {
      if (rt == "disabled" || rt == "off" || rt == "0") return 0;

      return 1;
    }

  }  // namespace vt

  namespace sw {
    int
    svtav1_preset_from_view(const std::string_view &preset) {
#define _CONVERT_(x, y) \
  if (preset == #x##sv) return y
      _CONVERT_(veryslow, 1);
      _CONVERT_(slower, 2);
      _CONVERT_(slow, 4);
      _CONVERT_(medium, 5);
      _CONVERT_(fast, 7);
      _CONVERT_(faster, 9);
      _CONVERT_(veryfast, 10);
      _CONVERT_(superfast, 11);
      _CONVERT_(ultrafast, 12);
#undef _CONVERT_
      return 11;  // Default to superfast
    }
  }  // namespace sw

  video_t video {
    28,  // qp

    0,  // hevc_mode
    0,  // av1_mode

    0,  // max_bitrate
    2,  // min_threads
    {
      "superfast"s,  // preset
      "zerolatency"s,  // tune
      11,  // superfast
    },  // software

    {},  // nv
    true,  // nv_realtime_hags
    true,  // nv_opengl_vulkan_on_dxgi
    true,  // nv_sunshine_high_power_mode
    false,  // vdd_keep_enabled
    false,  // vdd_headless_create_enabled
    false,  // vdd_reuse (default: recreate VDD for each client)
    true,  // vdd_borrowed_texture
    true,  // vdd_vulkan_hdr_bridge (automatic for HDR VDD sessions)
    {},  // nv_legacy

    {
      qsv::medium,  // preset
      qsv::_auto,  // cavlc
      false,  // slow_hevc
    },  // qsv

    {
      std::nullopt,  // usage (h264): driver default, matching FFmpeg amfenc
      std::nullopt,  // usage (hevc): driver default, matching FFmpeg amfenc
      std::nullopt,  // usage (av1): driver default, matching FFmpeg amfenc
      std::nullopt,  // rate control (h264): driver/AMF default, matching FFmpeg amfenc
      std::nullopt,  // rate control (hevc): driver/AMF default, matching FFmpeg amfenc
      std::nullopt,  // rate control (av1): driver/AMF default, matching FFmpeg amfenc
      std::nullopt,  // enforce_hrd: unset by default, matching FFmpeg amfenc
      std::nullopt,  // quality (h264): driver default, matching FFmpeg amfenc
      std::nullopt,  // quality (hevc): driver default, matching FFmpeg amfenc
      std::nullopt,  // quality (av1): driver default, matching FFmpeg amfenc
      std::nullopt,  // preanalysis: unset by default, matching FFmpeg amfenc
      std::nullopt,  // vbaq: unset by default, matching FFmpeg amfenc
      (int) amd::coder_e::_auto,  // coder
      23,  // qvbr_quality (1-51, default 23)
      0,  // ltr_frames
      0,  // slices_per_frame
      false,  // avcodec_compat: keep clean standalone AMF path by default
    },  // amd

    {
      0,
      0,
      1,
      -1,
    },  // vt

    {
      false,  // strict_rc_buffer
    },  // vaapi

    {},  // capture
    {},  // encoder
    {},  // adapter_name
    {},  // output_name
    {},  // capture_target (default: empty, will be set to "display" in apply_config)
    {},  // window_title
    true,  // capture_cursor (default: composite the host mouse cursor)
    (int) display_device::parsed_config_t::device_prep_e::no_operation,  // display_device_prep
    (int) display_device::parsed_config_t::resolution_change_e::automatic,  // resolution_change
    {},  // manual_resolution
    (int) display_device::parsed_config_t::refresh_rate_change_e::automatic,  // refresh_rate_change
    {},  // manual_refresh_rate
    (int) display_device::parsed_config_t::hdr_prep_e::automatic,  // hdr_prep
    {},  // display_mode_remapping
    false,  // variable_refresh_rate
    0,  // minimum_fps_target (0 = auto, about half the stream FPS)
    true,  // input_activity_boost
    60,  // input_activity_boost_fps
    150,  // input_activity_boost_window_ms
    "balanced"s,  // downscaling_quality (default: bicubic for best quality/performance balance)
    "auto"s,  // hdr_luminance_analysis (automatic for HDR streams, inactive for SDR)
    "auto"s,  // capture_compute_shader (automatic capability and benefit detection)
    false,  // wgc_disable_secure_desktop (disabled by default for security)
    true,  // dynamic_resolution_follow_display (default: on; matches existing behavior. Set false for legacy clients like PSVita Moonlight.)
  };

  audio_t audio {
    {},  // audio_sink
    {},  // virtual_sink
    "vb_cable"s,  // microphone_redirect_backend
    true,  // stream audio
    true,  // stream_mic (enable microphone streaming from client)
    true,  // install_steam_drivers
  };

  stream_t stream {
    10s,  // ping_timeout

    APPS_JSON_PATH,

    20,  // fecPercentage

    ENCRYPTION_MODE_NEVER,  // lan_encryption_mode
    ENCRYPTION_MODE_OPPORTUNISTIC,  // wan_encryption_mode
  };

  nvhttp_t nvhttp {
    "lan",  // origin web manager

    PRIVATE_KEY_FILE,
    CERTIFICATE_FILE,

    platf::get_host_name(),  // sunshine_name,
    "[]",
    "sunshine_state.json"s,  // file_state
    "[]"s,  // file_mappings
    48020,  // file_mapping_port
    false,  // usb_forwarding_enabled: explicit host opt-in
    0,  // usb_forwarding_port: automatic (main port + 7)
    {},  // external_ip
    {
      "1280x720"s,
      "1920x1080"s,
      "2560x1080"s,
      "2560x1440"s,
      "2560x1600"s,
      "3440x1440"s,
      "1920x1200"s,
      "3840x2160"s,
      "3840x1600"s,
    },  // supported resolutions

    { "60", "90", "120", "144" },  // supported fps (支持小数刷新率)

    SLEEP_MODE_SUSPEND,  // sleep_mode: default to S3 suspend

    10,  // pair_max_attempts: default 10 attempts per IP per 60s

    true,  // client_fingerprint_remote_rules
    "https://raw.githubusercontent.com/AlkaidLab/sunshine-client-fingerprint-rules/main/stable.json",
    {},  // client_fingerprint_rules_certificate (empty uses the pinned built-in certificate)
    24,  // client_fingerprint_rules_refresh_hours
  };

  input_t input {
    {
      { 0x10, 0xA0 },
      { 0x11, 0xA2 },
      { 0x12, 0xA4 },
    },
    -1ms,  // back_button_timeout
    500ms,  // key_repeat_delay
    std::chrono::duration<double> { 1 / 24.9 },  // key_repeat_period

    {
      platf::supported_gamepads(nullptr).front().name.data(),
      platf::supported_gamepads(nullptr).front().name.size(),
    },  // Default gamepad
    true,  // back as touchpad click enabled (manual DS4 only)
    true,  // client gamepads with motion events are emulated as DS4
    true,  // client gamepads with touchpads are emulated as DS4
    true,  // ds5_inputtino_randomize_mac
    false, // enable_dsu_server - disabled by default
    26760, // dsu_server_port - default DSU server port

    true,  // keyboard enabled
    true,  // mouse enabled
    true,  // controller enabled
    true,  // always send scancodes
    true,  // high resolution scrolling
    true,  // native pen/touch support
    true,  // native touchpad optimization
    true,  // virtual mouse (use driver if available)
    false, // amf_draw_mouse_cursor
    true,  // clipboard_sync (default on; effective only when the user-session GUI agent is alive and forwards data)
    true,  // client_gamepad_override (client-declared type wins; no official client sends it today)
  };

  sunshine_t sunshine {
    "en",  // locale
    "en",  // tray_locale (托盘菜单语言)
    2,  // min_log_level
    0,  // flags
    {},  // User file
    {},  // Username
    {},  // Password
    {},  // Password Salt
    file_handler::path_to_utf8(platf::appdata() / "sunshine.conf"),  // config file
    {},  // cmd args
    47989,  // Base port number
    "ipv4",  // Address family
    {},  // Bind address
    file_handler::path_to_utf8(platf::appdata() / "sunshine.log"),  // log file
    false,  // restore_log - 默认不恢复日志文件
    50,  // max_log_size_mb - 默认50MB，超过自动轮转
    false,  // notify_pre_releases
    true,  // system_tray
    {},  // prep commands
  };

  bool
  endline(char ch) {
    return ch == '\r' || ch == '\n';
  }

  bool
  space_tab(char ch) {
    return ch == ' ' || ch == '\t';
  }

  bool
  whitespace(char ch) {
    return space_tab(ch) || endline(ch);
  }

  template <class It>
  It
  find_comment(It begin, It end) {
    bool quoted = false;
    bool escaped = false;
    for (auto pos = begin; pos != end; ++pos) {
      if (quoted) {
        if (escaped) {
          escaped = false;
        }
        else if (*pos == '\\') {
          escaped = true;
        }
        else if (*pos == '"') {
          quoted = false;
        }
      }
      else if (*pos == '"') {
        quoted = true;
      }
      else if (*pos == '#') {
        return pos;
      }
    }
    return end;
  }

  std::string
  to_string(const char *begin, const char *end) {
    std::string result;

    KITTY_WHILE_LOOP(auto pos = begin, pos != end, {
      auto comment = find_comment(pos, end);
      auto endl = std::find_if(comment, end, endline);

      result.append(pos, comment);

      pos = endl;
    })

    return result;
  }

  template <class It>
  It
  skip_list(It skipper, It end) {
    int stack = 1;
    bool quoted = false;
    bool escaped = false;
    bool comment = false;
    while (skipper != end && stack) {
      if (comment) {
        comment = !endline(*skipper);
      }
      else if (quoted) {
        if (escaped) {
          escaped = false;
        }
        else if (*skipper == '\\') {
          escaped = true;
        }
        else if (*skipper == '"') {
          quoted = false;
        }
      }
      else if (*skipper == '"') {
        quoted = true;
      }
      else if (*skipper == '#') {
        comment = true;
      }
      else if (*skipper == '[') {
        ++stack;
      }
      else if (*skipper == ']') {
        --stack;
      }

      ++skipper;
    }

    return skipper;
  }

  std::pair<
    std::string_view::const_iterator,
    std::optional<std::pair<std::string, std::string>>>
  parse_option(std::string_view::const_iterator begin, std::string_view::const_iterator end) {
    begin = std::find_if_not(begin, end, whitespace);
    auto endl = std::find_if(begin, end, endline);
    auto endc = find_comment(begin, endl);
    endc = std::find_if(std::make_reverse_iterator(endc), std::make_reverse_iterator(begin), std::not_fn(whitespace)).base();

    auto eq = std::find(begin, endc, '=');
    if (eq == endc || eq == begin) {
      return std::make_pair(endl, std::nullopt);
    }

    auto end_name = std::find_if_not(std::make_reverse_iterator(eq), std::make_reverse_iterator(begin), space_tab).base();
    auto begin_val = std::find_if_not(eq + 1, endc, space_tab);

    if (begin_val == endl) {
      return std::make_pair(endl, std::nullopt);
    }

    // Lists might contain newlines
    if (*begin_val == '[') {
      endl = skip_list(begin_val + 1, end);

      // Check if we reached the end of the file without finding a closing bracket
      // We know we have a valid closing bracket if:
      // 1. We didn't reach the end, or
      // 2. We reached the end but the last character was the matching closing bracket
      if (endl == end && end == begin_val + 1) {
        BOOST_LOG(warning) << "config: Missing ']' in config option: " << to_string(begin, end_name);
        return std::make_pair(endl, std::nullopt);
      }
    }

    return std::make_pair(
      endl,
      std::make_pair(to_string(begin, end_name), to_string(begin_val, endl)));
  }

  std::unordered_map<std::string, std::string>
  parse_config(const std::string_view &file_content) {
    std::unordered_map<std::string, std::string> vars;

    auto pos = std::begin(file_content);
    auto end = std::end(file_content);

    while (pos < end) {
      // auto newline = std::find_if(pos, end, [](auto ch) { return ch == '\n' || ch == '\r'; });
      TUPLE_2D(endl, var, parse_option(pos, end));

      pos = endl;
      if (pos != end) {
        pos += (*pos == '\r') ? 2 : 1;
      }

      if (!var) {
        continue;
      }

      vars.emplace(std::move(*var));
    }

    return vars;
  }

  void
  string_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::string &input) {
    auto it = vars.find(name);
    if (it == std::end(vars)) {
      return;
    }

    input = std::move(it->second);

    vars.erase(it);
  }

  template <typename T, typename F>
  void
  generic_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, T &input, F &&f) {
    std::string tmp;
    string_f(vars, name, tmp);
    if (!tmp.empty()) {
      input = f(tmp);
    }
  }

  void
  string_restricted_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::string &input, const std::vector<std::string_view> &allowed_vals) {
    std::string temp;
    string_f(vars, name, temp);

    for (auto &allowed_val : allowed_vals) {
      if (temp == allowed_val) {
        input = std::move(temp);
        return;
      }
    }
  }

  void
  path_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, fs::path &input) {
    // appdata needs to be retrieved once only
    static auto appdata = platf::appdata();

    std::string temp;
    string_f(vars, name, temp);

    if (!temp.empty()) {
      input = file_handler::path_from_utf8(temp);
    }

    if (input.is_relative()) {
      input = appdata / input;
    }

    auto dir = input;
    dir.remove_filename();

    // Ensure the directories exists
    if (!fs::exists(dir)) {
      fs::create_directories(dir);
    }
  }

  void
  path_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::string &input) {
    fs::path temp = file_handler::path_from_utf8(input);

    path_f(vars, name, temp);

    input = file_handler::path_to_utf8(temp);
  }

  void
  int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, int &input) {
    auto it = vars.find(name);

    if (it == std::end(vars)) {
      return;
    }

    std::string_view val = it->second;

    // If value is something like: "756" instead of 756
    if (val.size() >= 2 && val[0] == '"') {
      val = val.substr(1, val.size() - 2);
    }

    // If that integer is in hexadecimal
    if (val.size() >= 2 && val.substr(0, 2) == "0x"sv) {
      input = util::from_hex<int>(val.substr(2));
    }
    else {
      input = util::from_view(val);
    }

    vars.erase(it);
  }

  void
  int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::optional<int> &input) {
    auto it = vars.find(name);

    if (it == std::end(vars)) {
      return;
    }

    std::string_view val = it->second;

    // If value is something like: "756" instead of 756
    if (val.size() >= 2 && val[0] == '"') {
      val = val.substr(1, val.size() - 2);
    }

    // If that integer is in hexadecimal
    if (val.size() >= 2 && val.substr(0, 2) == "0x"sv) {
      input = util::from_hex<int>(val.substr(2));
    }
    else {
      input = util::from_view(val);
    }

    vars.erase(it);
  }

  template <class F>
  void
  int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, int &input, F &&f) {
    std::string tmp;
    string_f(vars, name, tmp);
    if (!tmp.empty()) {
      input = f(tmp);
    }
  }

  template <class F>
  void
  int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::optional<int> &input, F &&f) {
    std::string tmp;
    string_f(vars, name, tmp);
    if (!tmp.empty()) {
      input = f(tmp);
    }
  }

  void
  int_between_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, int &input, const std::pair<int, int> &range) {
    int temp = input;

    int_f(vars, name, temp);

    TUPLE_2D_REF(lower, upper, range);
    if (temp >= lower && temp <= upper) {
      input = temp;
    }
  }

  bool
  to_bool(std::string &boolean) {
    std::for_each(std::begin(boolean), std::end(boolean), [](char ch) { return (char) std::tolower(ch); });

    return boolean == "true"sv ||
           boolean == "yes"sv ||
           boolean == "enable"sv ||
           boolean == "enabled"sv ||
           boolean == "on"sv ||
           (std::find(std::begin(boolean), std::end(boolean), '1') != std::end(boolean));
  }

  void
  bool_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, bool &input) {
    std::string tmp;
    string_f(vars, name, tmp);

    if (tmp.empty()) {
      return;
    }

    input = to_bool(tmp);
  }

  void
  bool_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::optional<bool> &input) {
    std::string tmp;
    string_f(vars, name, tmp);

    if (tmp.empty()) {
      return;
    }

    input = to_bool(tmp);
  }

  void
  int_between_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::optional<int> &input, const std::pair<int, int> &range) {
    std::optional<int> temp;
    int_f(vars, name, temp);

    if (!temp) {
      return;
    }

    TUPLE_2D_REF(lower, upper, range);
    if (*temp >= lower && *temp <= upper) {
      input = *temp;
    }
  }

  void
  double_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, double &input) {
    std::string tmp;
    string_f(vars, name, tmp);

    if (tmp.empty()) {
      return;
    }

    char *c_str_p;
    auto val = std::strtod(tmp.c_str(), &c_str_p);

    if (c_str_p == tmp.c_str()) {
      return;
    }

    input = val;
  }

  void
  double_between_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, double &input, const std::pair<double, double> &range) {
    double temp = input;

    double_f(vars, name, temp);

    TUPLE_2D_REF(lower, upper, range);
    if (temp >= lower && temp <= upper) {
      input = temp;
    }
  }

  void
  list_string_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::vector<std::string> &input) {
    std::string string;
    string_f(vars, name, string);

    if (string.empty()) {
      return;
    }

    input.clear();

    auto begin = std::cbegin(string);
    if (*begin == '[') {
      ++begin;
    }

    begin = std::find_if_not(begin, std::cend(string), whitespace);
    if (begin == std::cend(string)) {
      return;
    }

    auto pos = begin;
    while (pos < std::cend(string)) {
      if (*pos == '[') {
        pos = skip_list(pos + 1, std::cend(string)) + 1;
      }
      else if (*pos == ']') {
        break;
      }
      else if (*pos == ',') {
        input.emplace_back(begin, pos);
        pos = begin = std::find_if_not(pos + 1, std::cend(string), whitespace);
      }
      else {
        ++pos;
      }
    }

    if (pos != begin) {
      input.emplace_back(begin, pos);
    }
  }

  void
  list_display_mode_remapping_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::vector<video_t::display_mode_remapping_t> &input) {
    std::string string;
    string_f(vars, name, string);

    std::stringstream jsonStream;

    // check if string is empty, i.e. when the value doesn't exist in the config file
    if (string.empty()) {
      return;
    }

    // We need to add a wrapping object to make it valid JSON, otherwise ptree cannot parse it.
    jsonStream << "{\"display_mode_remapping\":" << string << "}";

    boost::property_tree::ptree jsonTree;
    boost::property_tree::read_json(jsonStream, jsonTree);

    for (auto &[_, entry] : jsonTree.get_child("display_mode_remapping"s)) {
      auto type = entry.get_optional<std::string>("type"s);
      auto received_resolution = entry.get_optional<std::string>("received_resolution"s);
      auto received_fps = entry.get_optional<std::string>("received_fps"s);
      auto final_resolution = entry.get_optional<std::string>("final_resolution"s);
      auto final_refresh_rate = entry.get_optional<std::string>("final_refresh_rate"s);

      input.push_back(video_t::display_mode_remapping_t {
        type.value_or(""),
        received_resolution.value_or(""),
        received_fps.value_or(""),
        final_resolution.value_or(""),
        final_refresh_rate.value_or("") });
    }
  }

  void
  list_prep_cmd_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::vector<prep_cmd_t> &input) {
    std::string string;
    string_f(vars, name, string);

    std::stringstream jsonStream;

    // check if string is empty, i.e. when the value doesn't exist in the config file
    if (string.empty()) {
      return;
    }

    // We need to add a wrapping object to make it valid JSON, otherwise ptree cannot parse it.
    jsonStream << "{\"prep_cmd\":" << string << "}";

    boost::property_tree::ptree jsonTree;
    boost::property_tree::read_json(jsonStream, jsonTree);

    for (auto &[_, prep_cmd] : jsonTree.get_child("prep_cmd"s)) {
      auto do_cmd = prep_cmd.get_optional<std::string>("do"s);
      auto undo_cmd = prep_cmd.get_optional<std::string>("undo"s);
      auto elevated = prep_cmd.get_optional<bool>("elevated"s);

      input.emplace_back(do_cmd.value_or(""), undo_cmd.value_or(""), elevated.value_or(false));
    }
  }

  void
  list_int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::vector<int> &input) {
    std::vector<std::string> list;
    list_string_f(vars, name, list);

    // check if list is empty, i.e. when the value doesn't exist in the config file
    if (list.empty()) {
      return;
    }

    // The framerate list must be cleared before adding values from the file configuration.
    // If the list is not cleared, then the specified parameters do not affect the behavior of the sunshine server.
    // That is, if you set only 30 fps in the configuration file, it will not work because by default, during initialization the list includes 10, 30, 60, 90 and 120 fps.
    input.clear();
    for (auto &el : list) {
      std::string_view val = el;

      // If value is something like: "756" instead of 756
      if (val.size() >= 2 && val[0] == '"') {
        val = val.substr(1, val.size() - 2);
      }

      int tmp;

      // If the integer is a hexadecimal
      if (val.size() >= 2 && val.substr(0, 2) == "0x"sv) {
        tmp = util::from_hex<int>(val.substr(2));
      }
      else {
        tmp = util::from_view(val);
      }
      input.emplace_back(tmp);
    }
  }

  void
  map_int_int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::unordered_map<int, int> &input) {
    std::vector<int> list;
    list_int_f(vars, name, list);

    // The list needs to be a multiple of 2
    if (list.size() % 2) {
      BOOST_LOG(warning) << "config: expected "sv << name << " to have a multiple of two elements --> not "sv << list.size();
      return;
    }

    int x = 0;
    while (x < list.size()) {
      auto key = list[x++];
      auto val = list[x++];

      input.emplace(key, val);
    }
  }

  int
  apply_flags(const char *line) {
    int ret = 0;
    while (*line != '\0') {
      switch (*line) {
        case '0':
          config::sunshine.flags[config::flag::PIN_STDIN].flip();
          break;
        case '1':
          config::sunshine.flags[config::flag::FRESH_STATE].flip();
          break;
        case '2':
          config::sunshine.flags[config::flag::FORCE_VIDEO_HEADER_REPLACE].flip();
          break;
        case 'p':
          config::sunshine.flags[config::flag::UPNP].flip();
          break;
        default:
          BOOST_LOG(warning) << "config: Unrecognized flag: ["sv << *line << ']' << std::endl;
          ret = -1;
      }

      ++line;
    }

    return ret;
  }

  std::vector<std::string_view> &
  get_supported_gamepad_options() {
    const auto options = platf::supported_gamepads(nullptr);
    static std::vector<std::string_view> opts {};
    opts.reserve(options.size());
    for (auto &opt : options) {
      opts.emplace_back(opt.name);
    }
    return opts;
  }

  void apply_config(std::unordered_map<std::string, std::string> &&vars) {
    for (auto &[name, val] : vars) {
      const auto log_value = name == "file_mappings" && !val.empty() ? "<redacted>"s : val;
      BOOST_LOG(info) << "config: '"sv << name << "' = "sv << log_value;
      modified_config_settings[name] = val;
    }

    int_f(vars, "qp", video.qp);
    int_f(vars, "min_threads", video.min_threads);
    int_between_f(vars, "hevc_mode", video.hevc_mode, { 0, 3 });
    int_between_f(vars, "av1_mode", video.av1_mode, { 0, 3 });
    string_f(vars, "sw_preset", video.sw.sw_preset);
    if (!video.sw.sw_preset.empty()) {
      video.sw.svtav1_preset = sw::svtav1_preset_from_view(video.sw.sw_preset);
    }
    string_f(vars, "sw_tune", video.sw.sw_tune);

    int_between_f(vars, "nvenc_preset", video.nv.quality_preset, { 1, 7 });
    int_between_f(vars, "nvenc_vbv_increase", video.nv.vbv_percentage_increase, { 0, 400 });
    bool_f(vars, "nvenc_spatial_aq", video.nv.adaptive_quantization);
    bool_f(vars, "nvenc_temporal_aq", video.nv.enable_temporal_aq);
    generic_f(vars, "nvenc_twopass", video.nv.two_pass, nv::twopass_from_view);
    bool_f(vars, "nvenc_h264_cavlc", video.nv.h264_cavlc);
    generic_f(vars, "nvenc_split_encode", video.nv.split_frame_encoding, nv::split_encode_from_view);
    int_between_f(vars, "nvenc_lookahead_depth", video.nv.lookahead_depth, { 0, 32 });
    generic_f(vars, "nvenc_lookahead_level", video.nv.lookahead_level, nv::lookahead_level_from_view);
    generic_f(vars, "nvenc_temporal_filter", video.nv.temporal_filter_level, nv::temporal_filter_level_from_view);
    generic_f(vars, "nvenc_rate_control", video.nv.rate_control_mode, nv::rate_control_mode_from_view);
    int_between_f(vars, "nvenc_target_quality", video.nv.target_quality, { 0, 63 });
    bool_f(vars, "nvenc_cuda_array_input", video.nv.cuda_array_input);
    bool_f(vars, "nvenc_realtime_hags", video.nv_realtime_hags);
    bool_f(vars, "nvenc_opengl_vulkan_on_dxgi", video.nv_opengl_vulkan_on_dxgi);
    bool_f(vars, "nvenc_latency_over_power", video.nv_sunshine_high_power_mode);

#if !defined(__ANDROID__) && !defined(__APPLE__)
    video.nv_legacy.preset = video.nv.quality_preset + 11;
    video.nv_legacy.multipass = video.nv.two_pass == nvenc::nvenc_two_pass::quarter_resolution ? NV_ENC_TWO_PASS_QUARTER_RESOLUTION :
                                video.nv.two_pass == nvenc::nvenc_two_pass::full_resolution    ? NV_ENC_TWO_PASS_FULL_RESOLUTION :
                                                                                                 NV_ENC_MULTI_PASS_DISABLED;
    video.nv_legacy.h264_coder = video.nv.h264_cavlc ? NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC : NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
    video.nv_legacy.aq = video.nv.adaptive_quantization;
    video.nv_legacy.vbv_percentage_increase = video.nv.vbv_percentage_increase;
#endif

    int_f(vars, "qsv_preset", video.qsv.qsv_preset, qsv::preset_from_view);
    int_f(vars, "qsv_coder", video.qsv.qsv_cavlc, qsv::coder_from_view);
    bool_f(vars, "qsv_slow_hevc", video.qsv.qsv_slow_hevc);

    std::string quality;
    string_f(vars, "amd_quality", quality);
    if (!quality.empty()) {
      video.amd.amd_quality_h264 = amd::quality_from_view<amd::quality_h264_e>(quality, video.amd.amd_quality_h264);
      video.amd.amd_quality_hevc = amd::quality_from_view<amd::quality_hevc_e>(quality, video.amd.amd_quality_hevc);
      video.amd.amd_quality_av1 = amd::quality_from_view<amd::quality_av1_e>(quality, video.amd.amd_quality_av1);
    }

    std::string rc;
    string_f(vars, "amd_rc", rc);
    int_f(vars, "amd_coder", video.amd.amd_coder, amd::coder_from_view);
    if (!rc.empty()) {
      video.amd.amd_rc_h264 = amd::rc_from_view<amd::rc_h264_e>(rc, video.amd.amd_rc_h264);
      video.amd.amd_rc_hevc = amd::rc_from_view<amd::rc_hevc_e>(rc, video.amd.amd_rc_hevc);
      video.amd.amd_rc_av1 = amd::rc_from_view<amd::rc_av1_e>(rc, video.amd.amd_rc_av1);
    }

    std::string usage;
    string_f(vars, "amd_usage", usage);
    if (!usage.empty()) {
      video.amd.amd_usage_h264 = amd::usage_from_view<amd::usage_h264_e>(usage, video.amd.amd_usage_h264);
      video.amd.amd_usage_hevc = amd::usage_from_view<amd::usage_hevc_e>(usage, video.amd.amd_usage_hevc);
      video.amd.amd_usage_av1 = amd::usage_from_view<amd::usage_av1_e>(usage, video.amd.amd_usage_av1);
    }

    // HQVBR/HQCBR requires two-pass encoding, incompatible with Ultra Low Latency usage.
    // Auto-upgrade usage to Low Latency High Quality when necessary.
    // RC values: HIGH_QUALITY_VBR=5, HIGH_QUALITY_CBR=6 (same across H.264/HEVC/AV1)
    // Usage values: ULTRA_LOW_LATENCY=1(H264/HEVC)/2(AV1), LOW_LATENCY_HIGH_QUALITY=5 (all codecs)
    auto adjust_usage_for_hq_rc = [](const std::optional<int> &rc, std::optional<int> &usage, int ull_val, int llhq_val, const char *codec) {
      if (rc && (*rc == 5 || *rc == 6) && usage && *usage == ull_val) {
        BOOST_LOG(warning) << "AMD " << codec << ": HQVBR/HQCBR is incompatible with Ultra Low Latency usage, "
                           << "auto-switching to Low Latency High Quality";
        usage = llhq_val;
      }
    };
    adjust_usage_for_hq_rc(video.amd.amd_rc_h264, video.amd.amd_usage_h264, 1, 5, "H.264");
    adjust_usage_for_hq_rc(video.amd.amd_rc_hevc, video.amd.amd_usage_hevc, 1, 5, "HEVC");
    adjust_usage_for_hq_rc(video.amd.amd_rc_av1, video.amd.amd_usage_av1, 2, 5, "AV1");

    int_f(vars, "amd_preanalysis", video.amd.amd_preanalysis, [](const std::string_view &value) {
      auto tmp = std::string { value };
      return to_bool(tmp) ? 1 : 0;
    });
    int_f(vars, "amd_vbaq", video.amd.amd_vbaq, [](const std::string_view &value) {
      auto tmp = std::string { value };
      return to_bool(tmp) ? 1 : 0;
    });
    int_f(vars, "amd_enforce_hrd", video.amd.amd_enforce_hrd, [](const std::string_view &value) {
      auto tmp = std::string { value };
      return to_bool(tmp) ? 1 : 0;
    });
    int_between_f(vars, "amd_qvbr_quality", video.amd.amd_qvbr_quality, { 1, 51 });
    int_between_f(vars, "amd_ltr_frames", video.amd.amd_ltr_frames, { 0, 4 });
    int_between_f(vars, "amd_slices_per_frame", video.amd.amd_slices_per_frame, { 0, 4 });
    bool_f(vars, "amd_avcodec_compat", video.amd.amd_avcodec_compat);
    bool_f(vars, "amd_multi_hw_instance", video.amd.amd_multi_hw_instance);
    // FFmpeg-aligned opt-in toggles (default nullopt = let AMD driver decide,
    // matches FFmpeg amfenc.c behavior of never setting the property unless
    // the user explicitly opts in). See AlkaidLab/foundation-sunshine#666 for
    // the RDNA4 freeze that motivated removing aggressive defaults.
    bool_f(vars, "amd_high_motion_qb", video.amd.amd_high_motion_qb);
    bool_f(vars, "amd_lowlatency_mode", video.amd.amd_lowlatency_mode);
    int_between_f(vars, "amd_input_queue_size", video.amd.amd_input_queue_size, { 1, 16 });
    // AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE_* enum: 0=NONE, 1=POWER_SAVING_REAL_TIME, 2=REAL_TIME, 3=LOWEST_LATENCY
    int_between_f(vars, "amd_av1_latency_mode", video.amd.amd_av1_latency_mode, { 0, 3 });

    int_f(vars, "vt_coder", video.vt.vt_coder, vt::coder_from_view);
    int_f(vars, "vt_software", video.vt.vt_allow_sw, vt::allow_software_from_view);
    int_f(vars, "vt_software", video.vt.vt_require_sw, vt::force_software_from_view);
    int_f(vars, "vt_realtime", video.vt.vt_realtime, vt::rt_from_view);

    bool_f(vars, "vaapi_strict_rc_buffer", video.vaapi.strict_rc_buffer);

    string_f(vars, "capture", video.capture);
    
#ifdef _WIN32
    // Check if WGC is selected and we're running in service mode
    // If so, automatically switch to DDX since WGC doesn't work in system mode
    if (!video.capture.empty() && video.capture == "wgc") {
      if (is_running_as_system_user) {
        BOOST_LOG(warning) << "WGC capture requires user session mode. Automatically switching to DDX capture."sv;
        video.capture = "ddx";
      }
    }
#endif
    
    string_f(vars, "encoder", video.encoder);
    string_f(vars, "adapter_name", video.adapter_name);
    {
      auto first = video.adapter_name.find_first_not_of(" \t\r\n");
      auto last = video.adapter_name.find_last_not_of(" \t\r\n");
      std::string trimmed = (first == std::string::npos) ? std::string {} : video.adapter_name.substr(first, last - first + 1);
      std::string lower = trimmed;
      std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return std::tolower(c); }
      );
      if (trimmed.empty() || lower == "default" || lower == "auto") {
        video.adapter_name.clear();
      }
      else {
        video.adapter_name = std::move(trimmed);
      }
    }
    string_f(vars, "output_name", video.output_name);
    
#ifdef _WIN32
    // Capture target: "display" (default) or "window"
    string_f(vars, "capture_target", video.capture_target);
    if (video.capture_target.empty()) {
      video.capture_target = "display";  // Default to display capture
    }
    
    // Window title for window capture
    string_f(vars, "window_title", video.window_title);
    
    // Validate capture_target
    if (video.capture_target != "display" && video.capture_target != "window") {
      BOOST_LOG(warning) << "Invalid capture_target: ["sv << video.capture_target << "], defaulting to 'display'"sv;
      video.capture_target = "display";
    }
    
    // If window capture is selected, ensure window_title is provided
    if (video.capture_target == "window" && video.window_title.empty()) {
      BOOST_LOG(warning) << "capture_target=window but window_title is empty. Window capture may fail."sv;
    }
#endif
    int_f(vars, "display_device_prep", video.display_device_prep, display_device::parsed_config_t::device_prep_from_view);
    int_f(vars, "resolution_change", video.resolution_change, display_device::parsed_config_t::resolution_change_from_view);
    string_f(vars, "manual_resolution", video.manual_resolution);
    list_display_mode_remapping_f(vars, "display_mode_remapping", video.display_mode_remapping);
    int_f(vars, "refresh_rate_change", video.refresh_rate_change, display_device::parsed_config_t::refresh_rate_change_from_view);
    string_f(vars, "manual_refresh_rate", video.manual_refresh_rate);
    int_f(vars, "hdr_prep", video.hdr_prep, display_device::parsed_config_t::hdr_prep_from_view);
    int_f(vars, "max_bitrate", video.max_bitrate);
    bool_f(vars, "variable_refresh_rate", video.variable_refresh_rate);
    int_between_f(vars, "minimum_fps_target", video.minimum_fps_target, { 0, 1000 });
    bool_f(vars, "input_activity_boost", video.input_activity_boost);
    int_between_f(vars, "input_activity_boost_fps", video.input_activity_boost_fps, { 0, 1000 });
    int_between_f(vars, "input_activity_boost_window_ms", video.input_activity_boost_window_ms, { 0, 5000 });
    string_f(vars, "hdr_luminance_analysis", video.hdr_luminance_analysis);
    if (video.hdr_luminance_analysis == "true" ||
        video.hdr_luminance_analysis == "enabled" ||
        video.hdr_luminance_analysis == "yes" ||
        video.hdr_luminance_analysis == "enable" ||
        video.hdr_luminance_analysis == "1") {
      video.hdr_luminance_analysis = "on";
    }
    else if (video.hdr_luminance_analysis == "false" ||
             video.hdr_luminance_analysis == "disabled" ||
             video.hdr_luminance_analysis == "no" ||
             video.hdr_luminance_analysis == "disable" ||
             video.hdr_luminance_analysis == "0") {
      video.hdr_luminance_analysis = "off";
    }
    if (video.hdr_luminance_analysis.empty()) {
      video.hdr_luminance_analysis = "auto";
    }
    if (video.hdr_luminance_analysis != "auto" &&
        video.hdr_luminance_analysis != "on" &&
        video.hdr_luminance_analysis != "off") {
      BOOST_LOG(warning) << "Invalid hdr_luminance_analysis: ["sv << video.hdr_luminance_analysis
                         << "], valid options are: auto, on, off. Defaulting to 'auto'"sv;
      video.hdr_luminance_analysis = "auto";
    }
    bool_f(vars, "wgc_disable_secure_desktop", video.wgc_disable_secure_desktop);
    bool_f(vars, "dynamic_resolution_follow_display", video.dynamic_resolution_follow_display);
    bool_f(vars, "vdd_keep_enabled", video.vdd_keep_enabled);
    bool_f(vars, "vdd_headless_create", video.vdd_headless_create_enabled);
    bool_f(vars, "vdd_reuse", video.vdd_reuse);
    bool_f(vars, "vdd_borrowed_texture", video.vdd_borrowed_texture);
    bool_f(vars, "vdd_vulkan_hdr_bridge", video.vdd_vulkan_hdr_bridge);

    // Whether to composite the host mouse cursor into the captured frames.
    // The runtime toggle Ctrl+Alt+Shift+N (handled in input.cpp) overrides this at runtime.
    bool_f(vars, "capture_cursor", video.capture_cursor);
    display_cursor = video.capture_cursor;

    // Downscaling quality: "fast" (bilinear+8pt average), "balanced" (bicubic), "high_quality" (future: lanczos)
    string_f(vars, "downscaling_quality", video.downscaling_quality);
    if (video.downscaling_quality.empty()) {
      video.downscaling_quality = "balanced";  // Default to bicubic
    }
    // Validate downscaling_quality
    if (video.downscaling_quality != "fast" && 
        video.downscaling_quality != "balanced" && 
        video.downscaling_quality != "high_quality") {
      BOOST_LOG(warning) << "Invalid downscaling_quality: ["sv << video.downscaling_quality 
                         << "], valid options are: fast, balanced, high_quality. Defaulting to 'balanced'"sv;
      video.downscaling_quality = "balanced";
    }

    // Compute-shader RGB->YUV conversion (HDR fast path).
    string_f(vars, "capture_compute_shader", video.capture_compute_shader);
    if (video.capture_compute_shader.empty()) {
      video.capture_compute_shader = "auto";
    }
    if (video.capture_compute_shader != "auto" &&
        video.capture_compute_shader != "on" &&
        video.capture_compute_shader != "off") {
      BOOST_LOG(warning) << "Invalid capture_compute_shader: ["sv << video.capture_compute_shader
                         << "], valid options are: auto, on, off. Defaulting to 'auto'"sv;
      video.capture_compute_shader = "auto";
    }

    path_f(vars, "pkey", nvhttp.pkey);
    path_f(vars, "cert", nvhttp.cert);
    string_f(vars, "sunshine_name", nvhttp.sunshine_name);
    string_f(vars, "clients", nvhttp.clients);
    path_f(vars, "log_path", config::sunshine.log_file);
    path_f(vars, "file_state", nvhttp.file_state);
    string_f(vars, "file_mappings", nvhttp.file_mappings);
    int file_mapping_port = nvhttp.file_mapping_port;
    int_between_f(vars, "file_mapping_port", file_mapping_port, { 1024, 65535 });
    nvhttp.file_mapping_port = static_cast<std::uint16_t>(file_mapping_port);
    bool_f(vars, "usb_forwarding_enabled", nvhttp.usb_forwarding_enabled);
    int usb_forwarding_port = nvhttp.usb_forwarding_port;
    int_f(vars, "usb_forwarding_port", usb_forwarding_port);
    if (usb_forwarding_port == 0 || (usb_forwarding_port >= 1024 && usb_forwarding_port <= 65535)) {
      nvhttp.usb_forwarding_port = static_cast<std::uint16_t>(usb_forwarding_port);
    } else {
      BOOST_LOG(warning) << "Ignoring invalid usb_forwarding_port: expected 0 or 1024-65535";
    }

    // Must be run after "file_state"
    config::sunshine.credentials_file = config::nvhttp.file_state;
    path_f(vars, "credentials_file", config::sunshine.credentials_file);

    string_f(vars, "external_ip", nvhttp.external_ip);
    list_string_f(vars, "resolutions"s, nvhttp.resolutions);
    list_string_f(vars, "fps"s, nvhttp.fps);
    int_between_f(vars, "sleep_mode", nvhttp.sleep_mode, { SLEEP_MODE_SUSPEND, SLEEP_MODE_AWAY });
    int_between_f(vars, "pair_max_attempts", nvhttp.pair_max_attempts, { 0, 50 });
    bool_f(vars, "client_fingerprint_remote_rules", nvhttp.client_fingerprint_remote_rules);
    string_f(vars, "client_fingerprint_rules_url", nvhttp.client_fingerprint_rules_url);
    string_f(vars, "client_fingerprint_rules_certificate", nvhttp.client_fingerprint_rules_certificate);
    if (!nvhttp.client_fingerprint_rules_certificate.empty()) {
      auto certificate_path = file_handler::path_from_utf8(nvhttp.client_fingerprint_rules_certificate);
      if (certificate_path.is_relative()) {
        certificate_path = platf::appdata() / certificate_path;
      }
      nvhttp.client_fingerprint_rules_certificate = file_handler::path_to_utf8(certificate_path);
    }
    int_between_f(
      vars,
      "client_fingerprint_rules_refresh_hours",
      nvhttp.client_fingerprint_rules_refresh_hours,
      {1, 168}
    );
    list_prep_cmd_f(vars, "global_prep_cmd", config::sunshine.prep_cmds);

    string_f(vars, "audio_sink", audio.sink);
    string_f(vars, "virtual_sink", audio.virtual_sink);
    string_restricted_f(
      vars,
      "microphone_redirect_backend",
      audio.microphone_redirect_backend,
      { "vb_cable"sv, "usbip_experimental"sv, "auto"sv, "disabled"sv }
    );
    bool_f(vars, "stream_audio", audio.stream);
    bool_f(vars, "stream_mic", audio.stream_mic);
    bool_f(vars, "install_steam_audio_drivers", audio.install_steam_drivers);

    string_restricted_f(vars, "origin_web_ui_allowed", nvhttp.origin_web_ui_allowed, { "pc"sv, "lan"sv, "wan"sv });

    int to = -1;
    int_between_f(vars, "ping_timeout", to, { -1, std::numeric_limits<int>::max() });
    if (to != -1) {
      stream.ping_timeout = std::chrono::milliseconds(to);
    }

    int_between_f(vars, "lan_encryption_mode", stream.lan_encryption_mode, { 0, 2 });
    int_between_f(vars, "wan_encryption_mode", stream.wan_encryption_mode, { 0, 2 });

    path_f(vars, "file_apps", stream.file_apps);
#ifndef __ANDROID__
    // TODO: Android can possibly support this
    const auto file_apps_path = file_handler::path_from_utf8(stream.file_apps);
    if (!fs::exists(file_apps_path)) {
      const auto bundled_apps_path = file_handler::path_from_utf8(SUNSHINE_ASSETS_DIR "/apps.json");
      fs::copy_file(bundled_apps_path, file_apps_path);
      fs::permissions(
        file_apps_path,
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::add
      );
    }
#endif

    int_between_f(vars, "fec_percentage", stream.fec_percentage, {1, 255});

    map_int_int_f(vars, "keybindings"s, input.keybindings);

    // This config option will only be used by the UI
    // When editing in the config file itself, use "keybindings"
    bool map_rightalt_to_win = false;
    bool_f(vars, "key_rightalt_to_key_win", map_rightalt_to_win);

    if (map_rightalt_to_win) {
      input.keybindings.emplace(0xA5, 0x5B);
    }

    to = std::numeric_limits<int>::min();
    int_f(vars, "back_button_timeout", to);

    if (to > std::numeric_limits<int>::min()) {
      input.back_button_timeout = std::chrono::milliseconds { to };
    }

    double repeat_frequency { 0 };
    double_between_f(vars, "key_repeat_frequency", repeat_frequency, { 0, std::numeric_limits<double>::max() });

    if (repeat_frequency > 0) {
      config::input.key_repeat_period = std::chrono::duration<double> { 1 / repeat_frequency };
    }

    to = -1;
    int_f(vars, "key_repeat_delay", to);
    if (to >= 0) {
      input.key_repeat_delay = std::chrono::milliseconds { to };
    }

    string_restricted_f(vars, "gamepad"s, input.gamepad, get_supported_gamepad_options());
    bool_f(vars, "ds4_back_as_touchpad_click", input.ds4_back_as_touchpad_click);
    bool_f(vars, "motion_as_ds4", input.motion_as_ds4);
    bool_f(vars, "touchpad_as_ds4", input.touchpad_as_ds4);
    bool_f(vars, "enable_dsu_server", input.enable_dsu_server);
    bool_f(vars, "client_gamepad_override", input.client_gamepad_override);
    
    int temp_port = static_cast<int>(input.dsu_server_port);
    int_between_f(vars, "dsu_server_port", temp_port, { 1024, 65535 });
    input.dsu_server_port = static_cast<uint16_t>(temp_port);

    bool_f(vars, "mouse", input.mouse);
    bool_f(vars, "keyboard", input.keyboard);
    bool_f(vars, "controller", input.controller);

    bool_f(vars, "always_send_scancodes", input.always_send_scancodes);

    bool_f(vars, "high_resolution_scrolling", input.high_resolution_scrolling);
    bool_f(vars, "native_pen_touch", input.native_pen_touch);
    bool_f(vars, "native_touchpad_optimization", input.native_touchpad_optimization);
    bool_f(vars, "virtual_mouse", input.virtual_mouse);
    bool_f(vars, "amf_draw_mouse_cursor", input.amf_draw_mouse_cursor);
    bool_f(vars, "clipboard_sync", input.clipboard_sync);

    bool_f(vars, "notify_pre_releases", sunshine.notify_pre_releases);

    bool_f(vars, "system_tray", sunshine.system_tray);

    int port = sunshine.port;
    int_between_f(vars, "port"s, port, { 1024 + nvhttp::PORT_HTTPS, 65535 - rtsp_stream::RTSP_SETUP_PORT });
    sunshine.port = (std::uint16_t) port;

    string_restricted_f(vars, "address_family", sunshine.address_family, {"ipv4"sv, "both"sv});
    string_f(vars, "bind_address", sunshine.bind_address);

    bool upnp = false;
    bool_f(vars, "upnp"s, upnp);

    if (upnp) {
      config::sunshine.flags[config::flag::UPNP].flip();
    }

    bool close_verify_safe = false;
    bool_f(vars, "close_verify_safe"s, close_verify_safe);

    if (close_verify_safe) {
      config::sunshine.flags[config::flag::CLOSE_VERIFY_SAFE].flip();
    }

    bool mdns_broadcast = true;
    bool_f(vars, "mdns_broadcast"s, mdns_broadcast);

    if (mdns_broadcast) {
      config::sunshine.flags[config::flag::MDNS_BROADCAST].flip();
    }

    bool_f(vars, "restore_log"s, sunshine.restore_log);
    int_f(vars, "max_log_size_mb"s, sunshine.max_log_size_mb);
    if (sunshine.max_log_size_mb < 0) {
      sunshine.max_log_size_mb = 0;
    }

    string_restricted_f(vars, "locale", config::sunshine.locale, {
                                                                   "bg"sv,  // Bulgarian
                                                                   "cs"sv,  // Czech
                                                                   "de"sv,  // German
                                                                   "en"sv,  // English
                                                                   "en_GB"sv,  // English (UK)
                                                                   "en_US"sv,  // English (US)
                                                                   "es"sv,  // Spanish
                                                                   "fr"sv,  // French
                                                                   "it"sv,  // Italian
                                                                   "ja"sv,  // Japanese
                                                                   "pt"sv,  // Portuguese
                                                                   "ru"sv,  // Russian
                                                                   "sv"sv,  // Swedish
                                                                   "tr"sv,  // Turkish
                                                                   "zh"sv,  // Chinese
                                                                   "zh_TW"sv,  // Chinese (Traditional)
                                                                 });

    // 托盘菜单语言设置
    string_restricted_f(vars, "tray_locale", config::sunshine.tray_locale, {
                                                                   "en"sv,  // English
                                                                   "zh"sv,  // Chinese (Simplified)
                                                                   "ja"sv,  // Japanese
                                                                 });

    std::string log_level_string;
    string_f(vars, "min_log_level", log_level_string);

    if (!log_level_string.empty()) {
      if (log_level_string == "verbose"sv) {
        sunshine.min_log_level = 0;
      }
      else if (log_level_string == "debug"sv) {
        sunshine.min_log_level = 1;
      }
      else if (log_level_string == "info"sv) {
        sunshine.min_log_level = 2;
      }
      else if (log_level_string == "warning"sv) {
        sunshine.min_log_level = 3;
      }
      else if (log_level_string == "error"sv) {
        sunshine.min_log_level = 4;
      }
      else if (log_level_string == "fatal"sv) {
        sunshine.min_log_level = 5;
      }
      else if (log_level_string == "none"sv) {
        sunshine.min_log_level = 6;
      }
      else {
        // accept digit directly
        auto val = log_level_string[0];
        if (val >= '0' && val < '7') {
          sunshine.min_log_level = val - '0';
        }
      }
    }

    auto it = vars.find("flags"s);
    if (it != std::end(vars)) {
      apply_flags(it->second.c_str());

      vars.erase(it);
    }

    if (sunshine.min_log_level <= 3) {
      for (auto &[var, _] : vars) {
        std::cout << "Warning: Unrecognized configurable option ["sv << var << ']' << std::endl;
      }
    }
  }

  int
  parse(int argc, char *argv[]) {
    std::unordered_map<std::string, std::string> cmd_vars;
#ifdef _WIN32
    bool shortcut_launch = false;
    bool service_admin_launch = false;
#endif

    for (auto x = 1; x < argc; ++x) {
      auto line = argv[x];

      if (line == "--help"sv) {
        logging::print_help(*argv);
        return 1;
      }
#ifdef _WIN32
      else if (line == "--shortcut"sv) {
        shortcut_launch = true;
      }
      else if (line == "--shortcut-admin"sv) {
        service_admin_launch = true;
      }
#endif
      else if (*line == '-') {
        if (*(line + 1) == '-') {
          sunshine.cmd.name = line + 2;
          sunshine.cmd.argc = argc - x - 1;
          sunshine.cmd.argv = argv + x + 1;

          break;
        }
        if (apply_flags(line + 1)) {
          logging::print_help(*argv);
          return -1;
        }
      }
      else {
        auto line_end = line + strlen(line);

        auto pos = std::find(line, line_end, '=');
        if (pos == line_end) {
          sunshine.config_file = line;
        }
        else {
          TUPLE_EL(var, 1, parse_option(line, line_end));
          if (!var) {
            logging::print_help(*argv);
            return -1;
          }

          TUPLE_EL_REF(name, 0, *var);

          auto it = cmd_vars.find(name);
          if (it != std::end(cmd_vars)) {
            cmd_vars.erase(it);
          }

          cmd_vars.emplace(std::move(*var));
        }
      }
    }

    bool config_loaded = false;
    try {
      // Create appdata folder if it does not exist
      file_handler::make_directory(file_handler::path_to_utf8(platf::appdata()));

      // Create empty config file if it does not exist
      const auto config_path = file_handler::path_from_utf8(sunshine.config_file);
      if (!fs::exists(config_path)) {
        std::ofstream { config_path };
      }

      // Read config file
      auto vars = parse_config(file_handler::read_file(sunshine.config_file.c_str()));

      for (auto &[name, value] : cmd_vars) {
        vars.insert_or_assign(std::move(name), std::move(value));
      }

      // Apply the config. Note: This will try to create any paths
      // referenced in the config, so we may receive exceptions if
      // the path is incorrect or inaccessible.
      apply_config(std::move(vars));
      config_loaded = true;
    }
    catch (const std::filesystem::filesystem_error &err) {
      BOOST_LOG(fatal) << "Failed to apply config: "sv << err.what();
    }
    catch (const boost::filesystem::filesystem_error &err) {
      BOOST_LOG(fatal) << "Failed to apply config: "sv << err.what();
    }

#ifdef _WIN32
    // UCRT64 raises an access denied exception if launching from the shortcut
    // as non-admin and the config folder is not yet present; we can defer
    // so that service instance will do the work instead.

    if (!config_loaded && !shortcut_launch) {
      BOOST_LOG(fatal) << "To relaunch Sunshine successfully, use the shortcut in the Start Menu. Do not run Sunshine.exe manually."sv;
      BOOST_LOG(fatal) << "要成功重新启动 Sunshine, 请使用开始菜单中的快捷方式。不要手动运行 Sunshine.exe"sv;
      std::this_thread::sleep_for(10s);
#else
    if (!config_loaded) {
#endif
      return -1;
    }

#ifdef _WIN32
    // We have to wait until the config is loaded to handle these launches,
    // because we need to have the correct base port loaded in our config.
    // Exception: UCRT64 shortcut_launch instances may have no config loaded due to
    // insufficient permissions to create folder; port defaults will be acceptable.
    if (service_admin_launch) {
      // This is a relaunch as admin to start the service only.
      // GUI is launched by the non-elevated parent after we return.
      service_ctrl::start_service();

      // Always return 1 to ensure Sunshine doesn't start normally
      return 1;
    }
    else if (shortcut_launch) {
      bool service_was_running = service_ctrl::is_service_running();
      if (!service_was_running) {
        // If the service isn't running, relaunch ourselves as admin to start it
        WCHAR executable[MAX_PATH];
        GetModuleFileNameW(NULL, executable, ARRAYSIZE(executable));

        SHELLEXECUTEINFOW shell_exec_info {};
        shell_exec_info.cbSize = sizeof(shell_exec_info);
        shell_exec_info.fMask = SEE_MASK_NOASYNC | SEE_MASK_NO_CONSOLE | SEE_MASK_NOCLOSEPROCESS;
        shell_exec_info.lpVerb = L"runas";
        shell_exec_info.lpFile = executable;
        shell_exec_info.lpParameters = L"--shortcut-admin";
        shell_exec_info.nShow = SW_NORMAL;
        if (!ShellExecuteExW(&shell_exec_info)) {
          auto winerr = GetLastError();
          BOOST_LOG(error) << "Failed executing shell command: " << winerr << std::endl;
          return 1;
        }

        // Wait for the elevated child to finish starting the service.
        // Then launch the GUI from this non-elevated process (no UAC for GUI).
        WaitForSingleObject(shell_exec_info.hProcess, INFINITE);
        CloseHandle(shell_exec_info.hProcess);
      }

      // Service is now running; launch GUI without elevation.
      service_ctrl::wait_for_ui_ready();
      launch_ui();

      // Always return 1 to ensure Sunshine doesn't start normally
      return 1;
    }
#endif

    return 0;
  }

  bool
  update_config(const std::map<std::string, std::string> &updates) {
    std::lock_guard lock { config_file_mutex };
    try {
      const auto file_content = read_config_file_contents();
      if (!file_content) {
        return false;
      }
      const auto existing_config = parse_config(*file_content);
      std::map<std::string, std::string> configMap {existing_config.begin(), existing_config.end()};

      // 更新配置项，同时检查是否有变化
      bool hasChanged = false;
      for (const auto &[key, value] : updates) {
        auto it = configMap.find(key);
        if (value.empty() || value == "null") {
          // 空值则删除该配置项
          if (it != configMap.end()) {
            configMap.erase(it);
            hasChanged = true;
          }
        }
        else {
          // 只有值不同时才更新
          if (it == configMap.end() || it->second != value) {
            configMap[key] = value;
            hasChanged = true;
          }
        }
      }

      if (!hasChanged) {
        BOOST_LOG(info) << "Config unchanged, skip writing";
        return true;
      }

      // 按字母顺序写入配置文件
      std::stringstream configStream;
      for (const auto &[key, value] : configMap) {
        if (!value.empty() && value != "null") {
          configStream << key << " = " << value << std::endl;
        }
      }

      if (file_handler::write_file(sunshine.config_file.c_str(), configStream.str()) != 0) {
        BOOST_LOG(warning) << "Failed to write config file: " << sunshine.config_file;
        return false;
      }
      if (const auto gamepad_update = updates.find("gamepad"); gamepad_update != updates.end()) {
        const auto gamepad = configMap.find("gamepad");
        platf::set_global_gamepad_mode(gamepad == configMap.end() ? "auto"sv : std::string_view {gamepad->second});
      }
      BOOST_LOG(info) << "Config updated successfully";
      return true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "Failed to update config: " << e.what();
      return false;
    }
  }

  std::optional<std::map<std::string, std::string>>
  get_config_snapshot() {
    std::lock_guard lock { config_file_mutex };
    try {
      const auto file_content = read_config_file_contents();
      if (!file_content) {
        return std::nullopt;
      }
      const auto config = parse_config(*file_content);
      return std::map<std::string, std::string> {config.begin(), config.end()};
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "Failed to read config snapshot: " << e.what();
      return std::nullopt;
    }
  }

  bool
  update_full_config(const std::map<std::string, std::string> &fullConfig) {
    std::lock_guard lock { config_file_mutex };
    try {
      // 不需要保存到配置文件的只读字段（API响应字段，不是配置项）
      const std::set<std::string> readonlyFields = {
        "status",           // API响应状态，不是配置项
        "platform",         // 平台信息，编译时确定，只读
        "version",          // 版本号，只读
        "active_encoder",   // 运行时探测后实际使用的编码器，只读
        "display_devices",  // 显示设备列表，运行时枚举，只读
        "adapters",         // 适配器列表，运行时枚举，只读
        "pair_name",        // 配对名称，由系统生成，只读
      };

      // 受保护的字段：由系统托盘控制，需要保留本地配置文件中的原有值
      const std::set<std::string> protectedFields = {
        "vdd_keep_enabled",       // 由系统托盘控制，不通过Web UI修改
        "vdd_headless_create",    // 由系统托盘控制，不通过Web UI修改
        "tray_locale",            // 由系统托盘控制，不通过Web UI修改
      };

      const auto original_file_content = read_config_file_contents();
      if (!original_file_content) {
        return false;
      }
      const auto existing_config = parse_config(*original_file_content);
      std::map<std::string, std::string> originalMap {existing_config.begin(), existing_config.end()};

      // 使用 std::map 保证按字母顺序保存
      std::map<std::string, std::string> resultMap;

      // 收集传入的配置（跳过只读字段和受保护字段）
      for (const auto &[key, value] : fullConfig) {
        // 跳过只读字段
        if (readonlyFields.count(key)) {
          continue;
        }
        // 跳过受保护字段（稍后用本地值覆盖）
        if (protectedFields.count(key)) {
          continue;
        }
        // 跳过空值和 null
        if (value.empty() || value == "null") {
          continue;
        }
        resultMap[key] = value;
      }

      // 添加受保护字段（使用本地配置文件中的原有值）
      for (const auto &field : protectedFields) {
        auto it = originalMap.find(field);
        if (it != originalMap.end() && !it->second.empty() && it->second != "null") {
          resultMap[field] = it->second;
        }
      }

      // 对比新配置与原始配置，相同则跳过写入
      if (resultMap != originalMap) {
        // 按字母顺序写入配置文件
        std::stringstream configStream;
        for (const auto &[key, value] : resultMap) {
          configStream << key << " = " << value << std::endl;
        }
        if (file_handler::write_file(sunshine.config_file.c_str(), configStream.str()) != 0) {
          BOOST_LOG(warning) << "Failed to write config file: " << sunshine.config_file;
          return false;
        }
        BOOST_LOG(info) << "Config saved successfully";
      }
      else {
        BOOST_LOG(info) << "Config unchanged, skip writing";
      }

      const auto gamepad = resultMap.find("gamepad");
      platf::set_global_gamepad_mode(gamepad == resultMap.end() ? "auto"sv : std::string_view {gamepad->second});

      return true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "Failed to save config: " << e.what();
      return false;
    }
  }

  std::string
  get_clients_config() {
    std::lock_guard lock { config_file_mutex };
    return nvhttp.clients;
  }

  bool
  get_client_touch_keyboard_enabled(const std::string &uuid) {
    if (uuid.empty()) {
      return false;
    }
    try {
      const auto clients = nlohmann::json::parse(get_clients_config());
      if (!clients.is_array()) {
        return false;
      }
      for (const auto &entry : clients) {
        if (!entry.is_object() || !entry.contains("uuid")) {
          continue;
        }
        if (entry["uuid"] != uuid) {
          continue;
        }
        return entry.contains("touch") &&
               entry["touch"].is_object() &&
               entry["touch"].value("enabled", false) == true;
      }
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "Failed to read touch profile for client: " << e.what();
    }
    return false;
  }

  bool
  save_clients_config(const std::string &clients) {
    std::lock_guard lock { config_file_mutex };
    try {
      const auto normalized_clients = nlohmann::json::parse(clients).dump();
      std::map<std::string, std::string> config_map;
      const auto file_content = file_handler::read_file(sunshine.config_file.c_str());
      const auto existing_config = parse_config(file_content);
      config_map.insert(existing_config.begin(), existing_config.end());

      const auto existing = config_map.find("clients");
      if (existing == config_map.end() || existing->second != normalized_clients) {
        config_map["clients"] = normalized_clients;

        std::stringstream config_stream;
        for (const auto &[key, value] : config_map) {
          if (!value.empty() && value != "null") {
            config_stream << key << " = " << value << std::endl;
          }
        }

        if (file_handler::write_file(sunshine.config_file.c_str(), config_stream.str()) != 0) {
          BOOST_LOG(warning) << "Failed to write client settings to config file: " << sunshine.config_file;
          return false;
        }
      }

      nvhttp.clients = normalized_clients;
      return true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "Failed to save client settings: " << e.what();
      return false;
    }
  }
}  // namespace config
