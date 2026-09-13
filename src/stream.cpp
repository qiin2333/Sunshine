/**
 * @file src/stream.cpp
 * @brief Definitions for the streaming protocols.
 */
#include "process.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <fstream>
#include <openssl/err.h>
#include <rs.h>

#include <boost/atomic.hpp>
#include <boost/endian/arithmetic.hpp>
#include <boost/function.hpp>
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/lock_guard.hpp>
#include <boost/thread/mutex.hpp>

#include "abr.h"

extern "C" {
// clang-format off
#include <moonlight-common-c/src/Limelight-internal.h>
// clang-format on
}

#ifndef DATA_SHARDS_MAX
  #define DATA_SHARDS_MAX 255
#endif

#include "client_fingerprint.h"
#include "config.h"
#include "display_device/display_device.h"
#include "display_device/session.h"
#include "ds5/config.h"
#include "globals.h"
#include "haptics/authored_ir.h"
#include "rtsp.h"
#include "input.h"
#include "logging.h"
#include "mic_mixer.h"
#include "network.h"
#include "perf_recorder.h"
#include "stream.h"
#include "sync.h"
#include "tray/system_tray.h"
#include "tray/tray_state.h"
#include "thread_safe.h"
#include "utility.h"
#include "webhook/webhook.h"

#include "clipboard_bridge.h"
#include "cursor_channel.h"
#include "platform/common.h"
#include "touch_keyboard_session.h"
#include "text_context/bridge.h"

#define IDX_START_A 0
#define IDX_START_B 1
#define IDX_INVALIDATE_REF_FRAMES 2
#define IDX_LOSS_STATS 3
#define IDX_INPUT_DATA 5
#define IDX_RUMBLE_DATA 6
#define IDX_TERMINATION 7
#define IDX_PERIODIC_PING 8
#define IDX_REQUEST_IDR_FRAME 9
#define IDX_ENCRYPTED 10
#define IDX_HDR_MODE 11
#define IDX_RUMBLE_TRIGGER_DATA 12
#define IDX_SET_MOTION_EVENT 13
#define IDX_SET_RGB_LED 14
#define IDX_SET_ADAPTIVE_TRIGGERS 15
#define IDX_MIC_DATA 16
#define IDX_MIC_CONFIG 17
#define IDX_DYNAMIC_PARAM_CHANGE 18  // 统一动态参数调整消息类型（支持码率、分辨率等）
#define IDX_RESOLUTION_CHANGE 19  // 分辨率变化通知
#define IDX_CLIPBOARD 20  // Clipboard sync (Sunshine protocol extension; payload forwarded to user-session GUI agent)
#define IDX_CURSOR 21  // Local cursor mode/update (Sunshine protocol extension)
#define IDX_DS5_HAPTICS_PCM 22  // Authored DualSense actuator PCM (Sunshine protocol extension)
#define IDX_DS5_HAPTICS_IR_V2 23  // Device-independent analyzed haptics (Sunshine protocol extension)
#define IDX_TEXT_CONTEXT 24  // Remote text focus/context update (Sunshine protocol extension)

static const short packetTypes[] = {
  0x0305,  // Start A
  0x0307,  // Start B
  0x0301,  // Invalidate reference frames
  0x0201,  // Loss Stats
  0x0204,  // Frame Stats (unused)
  0x0206,  // Input data
  0x010b,  // Rumble data
  0x0109,  // Termination
  0x0200,  // Periodic Ping
  0x0302,  // IDR frame
  0x0001,  // fully encrypted
  0x010e,  // HDR mode
  0x5500,  // Rumble triggers (Sunshine protocol extension)
  0x5501,  // Set motion event (Sunshine protocol extension)
  0x5502,  // Set RGB LED (Sunshine protocol extension)
  0x5503,  // Set Adaptive triggers (Sunshine protocol extension)
  0x5504,  // Microphone data (Sunshine protocol extension)
  0x5505,  // Microphone config (Sunshine protocol extension)
  0x5506,  // Dynamic parameter change (Sunshine protocol extension) - 统一动态参数调整
  0x5507,  // Resolution change (Sunshine protocol extension) - 分辨率变化通知
  0x5508,  // Clipboard sync (Sunshine protocol extension) - opaque payload forwarded to user-session GUI agent
  0x5509,  // Local cursor mode/update (Sunshine protocol extension)
  0x550A,  // Authored DualSense haptics PCM (Sunshine protocol extension)
  0x550B,  // Device-independent DualSense haptics IR v2 (Sunshine protocol extension)
  0x550C,  // Remote text context update (Sunshine protocol extension)
};

namespace asio = boost::asio;
namespace sys = boost::system;

using asio::ip::tcp;
using asio::ip::udp;

using namespace std::literals;

namespace stream {

  std::uint32_t
  video_rtp_timestamp(
    std::chrono::steady_clock::time_point presentation_time,
    std::chrono::steady_clock::time_point epoch) {
    using signed_rtp_tick = std::chrono::duration<std::int64_t, std::ratio<1, 90000>>;
    return static_cast<std::uint32_t>(
      std::chrono::round<signed_rtp_tick>(presentation_time - epoch).count());
  }

  namespace {
    std::uint32_t
    read_dynamic_param_u32(std::string_view payload, std::size_t offset) {
      return static_cast<std::uint32_t>(static_cast<unsigned char>(payload[offset])) |
             (static_cast<std::uint32_t>(static_cast<unsigned char>(payload[offset + 1])) << 8) |
             (static_cast<std::uint32_t>(static_cast<unsigned char>(payload[offset + 2])) << 16) |
             (static_cast<std::uint32_t>(static_cast<unsigned char>(payload[offset + 3])) << 24);
    }

    std::int32_t
    read_dynamic_param_i32(std::string_view payload, std::size_t offset) {
      const auto value = read_dynamic_param_u32(payload, offset);
      const auto signed_value = (value & 0x80000000u) != 0
                                  ? static_cast<std::int64_t>(value) - (1LL << 32)
                                  : static_cast<std::int64_t>(value);
      return static_cast<std::int32_t>(signed_value);
    }

    float
    read_dynamic_param_f32(std::string_view payload, std::size_t offset) {
      return std::bit_cast<float>(read_dynamic_param_u32(payload, offset));
    }

    std::int64_t
    steady_now_ms() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
    }

    std::int64_t
    idle_ms(std::int64_t now, std::int64_t last_activity) {
      return last_activity == 0 ? -1 : std::max<std::int64_t>(0, now - last_activity);
    }
  }  // namespace

  enum class socket_e : int {
    video,  ///< Video
    audio,  ///< Audio
    microphone,  ///< Microphone
  };

#pragma pack(push, 1)

  struct video_short_frame_header_t {
    uint8_t *
    payload() {
      return (uint8_t *) (this + 1);
    }

    std::uint8_t headerType;  // Always 0x01 for short headers

    // Sunshine extension
    // Frame processing latency, in 1/10 ms units
    //     zero when the frame is repeated or there is no backend implementation
    boost::endian::little_uint16_at frame_processing_latency;

    // Currently known values:
    // 1 = Normal P-frame
    // 2 = IDR-frame
    // 4 = P-frame with intra-refresh blocks
    // 5 = P-frame after reference frame invalidation
    std::uint8_t frameType;

    // Length of the final packet payload for codecs that cannot handle
    // zero padding, such as AV1 (Sunshine extension).
    boost::endian::little_uint16_at lastPayloadLen;

    std::uint8_t unknown[2];
  };

  static_assert(
    sizeof(video_short_frame_header_t) == 8,
    "Short frame header must be 8 bytes");

  struct video_packet_raw_t {
    uint8_t *
    payload() {
      return (uint8_t *) (this + 1);
    }

    RTP_PACKET rtp;
    char reserved[4];

    NV_VIDEO_PACKET packet;
  };

  struct video_packet_enc_prefix_t {
    std::uint8_t iv[12];  // 12-byte IV is ideal for AES-GCM
    std::uint32_t frameNumber;
    std::uint8_t tag[16];
  };

  struct audio_packet_t {
    RTP_PACKET rtp;
  };

  struct control_header_v2 {
    std::uint16_t type;
    std::uint16_t payloadLength;

    uint8_t *
    payload() {
      return (uint8_t *) (this + 1);
    }
  };

  struct control_terminate_t {
    control_header_v2 header;

    std::uint32_t ec;
  };

  struct control_rumble_t {
    control_header_v2 header;

    std::uint32_t useless;

    std::uint16_t id;
    std::uint16_t lowfreq;
    std::uint16_t highfreq;
  };

  struct control_rumble_triggers_t {
    control_header_v2 header;

    std::uint16_t id;
    std::uint16_t left;
    std::uint16_t right;
  };

  struct control_set_motion_event_t {
    control_header_v2 header;

    std::uint16_t id;
    std::uint16_t reportrate;
    std::uint8_t type;
  };

  struct control_set_rgb_led_t {
    control_header_v2 header;

    std::uint16_t id;
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
  };

  struct control_adaptive_triggers_t {
    control_header_v2 header;

    std::uint16_t id;
    /**
     * 0x04 - Right trigger
     * 0x08 - Left trigger
     */
    std::uint8_t event_flags;
    std::uint8_t type_left;
    std::uint8_t type_right;
    std::uint8_t left[DS_EFFECT_PAYLOAD_SIZE];
    std::uint8_t right[DS_EFFECT_PAYLOAD_SIZE];
  };

  struct control_hdr_mode_t {
    control_header_v2 header;

    std::uint8_t enabled;

    // Sunshine protocol extension
    SS_HDR_METADATA metadata;
  };

  struct control_resolution_change_t {
    control_header_v2 header;

    std::uint32_t width;
    std::uint32_t height;
  };

  constexpr std::uint8_t cursor_protocol_version = 1;
  constexpr std::uint8_t cursor_flag_shape = 0x01;
  constexpr std::uint8_t cursor_flag_visible = 0x02;
  constexpr std::size_t cursor_chunk_bytes = 60000;

  struct cursor_update_header_t {
    std::uint8_t version;
    std::uint8_t flags;
    std::uint16_t header_size;
    std::uint32_t shape_id;
    std::uint16_t width;
    std::uint16_t height;
    std::int16_t hotspot_x;
    std::int16_t hotspot_y;
    std::uint32_t total_size;
    std::uint32_t offset;
    std::uint16_t chunk_size;
    std::uint16_t reserved;
  };
  static_assert(sizeof(cursor_update_header_t) == 28);

  typedef struct control_encrypted_t {
    std::uint16_t encryptedHeaderType;  // Always LE 0x0001
    std::uint16_t length;  // sizeof(seq) + 16 byte tag + secondary header and data

    // seq is accepted as an arbitrary value in Moonlight
    std::uint32_t seq;  // Monotonically increasing sequence number (used as IV for AES-GCM)

    uint8_t *
    payload() {
      return (uint8_t *) (this + 1);
    }
    // encrypted control_header_v2 and payload data follow
  } *control_encrypted_p;

  struct audio_fec_packet_t {
    RTP_PACKET rtp;
    AUDIO_FEC_HEADER fecHeader;
  };

  struct mic_packet_t {
    RTP_PACKET rtp;
  };

  // 扩展的RTP包结构，支持16位包类型
  struct rtp_packet_ext_t {
    std::uint8_t header;
    std::uint16_t packetType;  // 16位包类型
    std::uint16_t sequenceNumber;
    std::uint32_t timestamp;
    std::uint32_t ssrc;
  };

#pragma pack(pop)

  constexpr std::size_t
  round_to_pkcs7_padded(std::size_t size) {
    return ((size + 15) / 16) * 16;
  }

  // Maximum payload (bytes) of a single audio RTP packet BEFORE AES-CBC
  // padding. Computed as the worst-case codec frame across all supported
  // encodings, plus a small headroom. Each value below is the per-frame
  // payload of the corresponding codec at its highest negotiated setting:
  //
  //   AC3 @640 kbps:  640e3 / 8 * (1536 / 48000)         = 2560 B  (32 ms)
  //   E-AC3 @384k:    384e3 / 8 * (1536 / 48000)         = 1536 B  (32 ms)
  //   PCM_S16 5ms 8c: 5 * 48 * 8 * sizeof(int16_t)        = 3840 B  (worst PCM)
  //   Opus (any cfg): bounded well under 1500 B in practice
  //
  // The compile-time max() ensures adding a new codec only requires bumping
  // the corresponding constant (or its formula) — no risk of forgetting a
  // magic number elsewhere. session::alloc() consumes
  // round_to_pkcs7_padded(MAX_AUDIO_PACKET_SIZE) for each shard, so the
  // shard buffer width auto-tracks this constant.
  //
  // Note: large frames will exceed Ethernet MTU 1500 and be IP-fragmented at
  // the OS level — expected for raw passthrough on a LAN; receivers
  // reassemble before delivering to the RTP layer.
  namespace audio_payload {
    constexpr std::size_t kAc3MaxFrameBytes    = 2560;  // 640 kbps * 32 ms
    constexpr std::size_t kEac3MaxFrameBytes   = 1536;  // 384 kbps * 32 ms
    constexpr std::size_t kPcmS16MaxFrameBytes = 5 * 48 * 8 * sizeof(int16_t);  // 5 ms * 48 kHz * 8 ch
    constexpr std::size_t kOpusMaxFrameBytes   = 1500;  // generous upper bound
    constexpr std::size_t kHeadroomBytes       = 64;    // future-proofing margin
  }  // namespace audio_payload

  constexpr std::size_t MAX_AUDIO_PACKET_SIZE =
      std::max({
          audio_payload::kAc3MaxFrameBytes,
          audio_payload::kEac3MaxFrameBytes,
          audio_payload::kPcmS16MaxFrameBytes,
          audio_payload::kOpusMaxFrameBytes,
      }) + audio_payload::kHeadroomBytes;

  using av_session_id_t = std::variant<asio::ip::address, std::string>;  // IP address or SS-Ping-Payload from RTSP handshake
  using message_queue_t = std::shared_ptr<safe::queue_t<std::pair<udp::endpoint, std::string>>>;
  using message_queue_queue_t = std::shared_ptr<safe::queue_t<std::tuple<socket_e, av_session_id_t, message_queue_t>>>;

  // return bytes written on success
  // return -1 on error
  static inline int
  encode_audio(bool encrypted, const audio::buffer_t &plaintext, uint8_t *destination, crypto::aes_t &iv, crypto::cipher::cbc_t &cbc) {
    // If encryption isn't enabled
    if (!encrypted) {
      std::copy(std::begin(plaintext), std::end(plaintext), destination);
      return plaintext.size();
    }

    return cbc.encrypt(std::string_view { (char *) std::begin(plaintext), plaintext.size() }, destination, &iv);
  }

  static inline void
  while_starting_do_nothing(const session::lifecycle_t &lifecycle) {
    while (lifecycle.state() == session::state_e::STARTING) {
      std::this_thread::sleep_for(1ms);
    }
  }

  class control_server_t {
  public:
    int
    bind(net::af_e address_family, std::uint16_t port) {
      _host = net::host_create(address_family, _addr, port);

      return !(bool) _host;
    }

    // Get session associated with address.
    // If none are found, try to find a session not yet claimed. (It will be marked by a port of value 0
    // If none of those are found, return nullptr
    session_t *
    get_session(const net::peer_t peer, uint32_t connect_data);

    // Circular dependency:
    //   iterate refers to session
    //   session refers to broadcast_ctx_t
    //   broadcast_ctx_t refers to control_server_t
    // Therefore, iterate is implemented further down the source file
    void
    iterate(std::chrono::milliseconds timeout);

    /**
     * @brief Call the handler for a given control stream message.
     * @param type The message type.
     * @param session The session the message was received on.
     * @param payload The payload of the message.
     * @param reinjected `true` if this message is being reprocessed after decryption.
     */
    void
    call(std::uint16_t type, session_t *session, const std::string_view &payload, bool reinjected);

    void
    map(uint16_t type, std::function<void(session_t *, const std::string_view &)> cb) {
      _map_type_cb.emplace(type, std::move(cb));
    }

    int
    send(const std::string_view &payload, net::peer_t peer) {
      auto packet = enet_packet_create(payload.data(), payload.size(), ENET_PACKET_FLAG_RELIABLE);
      if (enet_peer_send(peer, 0, packet)) {
        enet_packet_destroy(packet);

        return -1;
      }

      return 0;
    }

    int
    send_unreliable(const std::string_view &payload, net::peer_t peer) {
      auto packet = enet_packet_create(payload.data(), payload.size(), 0);
      if (enet_peer_send(peer, 0, packet)) {
        enet_packet_destroy(packet);
        return -1;
      }
      return 0;
    }

    void
    flush() {
      enet_host_flush(_host.get());
    }

    // Callbacks
    std::unordered_map<std::uint16_t, std::function<void(session_t *, const std::string_view &)>> _map_type_cb;

    // All active sessions (including those still waiting for a peer to connect)
    sync_util::sync_t<std::vector<session_t *>> _sessions;

    // ENet peer to session mapping for sessions with a peer connected
    sync_util::sync_t<std::unordered_map<net::peer_t, session_t *>> _peer_to_session;

    ENetAddress _addr;
    net::host_t _host;
  };

  struct broadcast_ctx_t {
    message_queue_queue_t message_queue_queue;

    std::thread recv_thread;
    std::thread video_thread;
    std::thread audio_thread;
    std::thread control_thread;
    std::thread mic_thread;

    asio::io_context io_context;
    asio::io_context mic_io_context;

    udp::socket video_sock { io_context };
    udp::socket audio_sock { io_context };
    udp::socket mic_sock { mic_io_context };

    control_server_t control_server;

    boost::mutex mic_socket_mutex;
    boost::atomic<bool> mic_socket_enabled { false };

    // 单个会话的麦克风加密上下文。
    struct mic_cipher_ctx_t {
      crypto::cipher::cbc_t cipher;

      mic_cipher_ctx_t(const crypto::aes_t &key, bool padding):
          cipher(key, padding) {}

      mic_cipher_ctx_t(mic_cipher_ctx_t &&) noexcept = default;
      mic_cipher_ctx_t &operator=(mic_cipher_ctx_t &&) noexcept = default;
    };

    struct mic_session_ctx_t {
      std::uint32_t session_id;
      std::uint32_t key_id;
      boost::asio::ip::address client_address;
      std::optional<udp::endpoint> source_endpoint;
      std::optional<mic_cipher_ctx_t> cipher;

      mic_session_ctx_t(
        std::uint32_t session_id,
        std::uint32_t key_id,
        boost::asio::ip::address client_address,
        std::optional<mic_cipher_ctx_t> cipher
      ):
          session_id {session_id},
          key_id {key_id},
          client_address {net::normalize_address(std::move(client_address))},
          cipher {std::move(cipher)} {}
    };

    // 每个会话保留独立的加密与 Opus 解码状态，麦克风线程将所有活动源混合后
    // 写入同一个虚拟麦克风设备。会话 ID 是主身份，IP 和 UDP endpoint 只用于路由。
    std::unordered_map<std::uint32_t, boost::shared_ptr<mic_session_ctx_t>> mic_sessions;
    boost::mutex mic_session_mutex;
    boost::atomic<std::uint32_t> mic_session_count { 0 };
    boost::atomic<bool> mic_playout_reset_pending { false };
  };

  struct session_t {
    config_t config;

    safe::mail_t mail;

    std::shared_ptr<input::input_t> input;

    std::thread audioThread;
    std::thread videoThread;

    std::chrono::steady_clock::time_point pingTimeout;

    safe::shared_t<broadcast_ctx_t>::ptr_t broadcast_ref;

    boost::asio::ip::address localAddress;

    // 添加客户端名称字段
    std::string client_name;
    std::string client_cert_uuid;
    bool use_vdd {false};
    int custom_screen_mode {-1};
    bool highly_suspected_unknown_client {false};
    std::string app_name;
    int app_id = 0;

    std::int64_t created_at_ms { 0 };
    std::atomic<std::int64_t> last_control_activity_ms { 0 };
    std::atomic<std::int64_t> last_video_activity_ms { 0 };
    std::atomic<std::int64_t> last_audio_activity_ms { 0 };
    session::lifecycle_t lifecycle;

    struct {
      std::string ping_payload;

      int lowseq;
      udp::endpoint peer;

      std::optional<crypto::cipher::gcm_t> cipher;
      std::uint64_t gcm_iv_counter;

      safe::mail_raw_t::event_t<bool> idr_events;
      safe::mail_raw_t::event_t<std::pair<int64_t, int64_t>> invalidate_ref_frames_events;
      safe::mail_raw_t::event_t<video::dynamic_param_t> dynamic_param_change_events;  // 新增：动态参数调整事件

      std::unique_ptr<platf::deinit_t> qos;
    } video;

    struct {
      crypto::cipher::cbc_t cipher;
      std::string ping_payload;

      std::uint16_t sequenceNumber;
      // avRiKeyId == util::endian::big(First (sizeof(avRiKeyId)) bytes of launch_session->iv)
      std::uint32_t avRiKeyId;
      std::uint32_t timestamp;
      udp::endpoint peer;

      util::buffer_t<char> shards;
      util::buffer_t<uint8_t *> shards_p;

      audio_fec_packet_t fec_packet;
      std::unique_ptr<platf::deinit_t> qos;

      bool enable_mic;
      bool mic_registered { false };
    } audio;

    struct {
      crypto::cipher::gcm_t cipher;
      crypto::aes_t legacy_input_enc_iv;  // Only used when the client doesn't support full control stream encryption
      crypto::aes_t incoming_iv;
      crypto::aes_t outgoing_iv;

      std::uint32_t connect_data;  // Used for new clients with ML_FF_SESSION_ID_V1
      std::string expected_peer_address;  // Only used for legacy clients without ML_FF_SESSION_ID_V1

      net::peer_t peer;
      std::uint32_t seq;

      platf::feedback_queue_t feedback_queue;
      safe::mail_raw_t::event_t<video::hdr_info_t> hdr_queue;
      safe::mail_raw_t::event_t<std::pair<std::uint32_t, std::uint32_t>> resolution_change_queue;  // width, height

      bool local_cursor_mode = false;
      std::uint64_t cursor_revision = 0;
      bool cursor_shape_sent = false;
      std::uint32_t cursor_shape_id = 0;
      bool cursor_visibility_sent = false;
      bool cursor_visible = false;
      std::uint64_t cursor_failed_revision = 0;
      std::uint32_t cursor_send_failures = 0;

      std::unique_ptr<haptics::authored_ir_session_t> authored_haptics;
      bool authored_haptics_init_failed = false;
      // One fallback session per controller: smoothing, throttle and watchdog
      // state must not bleed between pads, and the watchdog stop has to reach
      // the pad that actually stopped sending PCM.
      std::unordered_map<std::uint16_t, std::unique_ptr<haptics::legacy_rumble_session_t>> legacy_haptics;
      bool legacy_haptics_init_failed = false;
      std::unordered_map<std::uint16_t, std::pair<std::uint16_t, std::uint16_t>> compatibility_rumble;
      std::unordered_map<std::uint16_t, std::pair<std::uint16_t, std::uint16_t>> synthesized_haptics_rumble;
    } control;

    std::uint32_t launch_session_id;

    // 保存 launch_session 的关键字段，用于动态参数更新
    bool enable_sops { false };
    bool enable_hdr { false };
    rtsp_stream::synthetic_hdr_config_t synthetic_hdr;
    platf::frame_pipeline_policy_t frame_pipeline_policy;
    bool frame_pipeline_policy_resolved { false };
    hdr::client_display_capabilities_t hdr_capabilities;
    hdr::client_display_capabilities_t reported_hdr_capabilities;
    // Runtime SDR white updates are consumed by the video thread and may also
    // be needed by a control-thread display reconfiguration.
    std::atomic<float> dynamic_sdr_white_nits { 0.0f };
    hdr::target_source_e hdr_target_source { hdr::target_source_e::safe_defaults };

    safe::mail_raw_t::event_t<bool> shutdown_event;
    safe::signal_t controlEnd;

    // Current total bitrate for this session (including FEC overhead) in Kbps
    // This is the user-configured bitrate, not the encoding bitrate
    std::atomic<int> current_total_bitrate { 0 };

    // 标识这是仅控制流会话（只作为输入设备，不传输视频/音频）
    bool control_only { false };
  };

  namespace session {
    // 高位表示显示设备正在重配置，低 31 位记录已登记的视频会话数。
    // 两者共用一个原子状态，确保“确认单会话”和“取得重配置权”由同一次 CAS 完成。
    constexpr std::uint32_t VIDEO_SESSION_RECONFIGURING = std::uint32_t {1} << 31;
    constexpr std::uint32_t VIDEO_SESSION_COUNT_MASK = ~VIDEO_SESSION_RECONFIGURING;

    std::atomic_uint running_sessions;
    boost::atomic<std::uint32_t> video_session_state {0};

    // 在创建音视频线程前登记会话。显示设备重配置期间，新会话在此等待占用位释放，
    // 避免会话启动和 VDD 重建同时进行。返回值表示它是否是首个视频会话。
    std::optional<bool>
    register_video_session() {
      auto state = video_session_state.load(boost::memory_order_acquire);
      for (;;) {
        if ((state & VIDEO_SESSION_RECONFIGURING) != 0) {
          state = video_session_state.wait(state, boost::memory_order_acquire);
          continue;
        }

        const auto count = state & VIDEO_SESSION_COUNT_MASK;
        if (count == VIDEO_SESSION_COUNT_MASK) {
          BOOST_LOG(error) << "Cannot register another video session because the session counter is exhausted"sv;
          return std::nullopt;
        }

        if (video_session_state.compare_exchange_weak(
              state,
              state + 1,
              boost::memory_order_acq_rel,
              boost::memory_order_acquire)) {
          return count == 0;
        }
      }
    }

    // 只递减低位的会话计数，保留可能并发存在的重配置占用位。
    // 返回注销后的会话数，由最后一个会话负责执行平台停止和显示恢复逻辑。
    std::uint32_t
    unregister_video_session() {
      auto state = video_session_state.load(boost::memory_order_acquire);
      for (;;) {
        const auto count = state & VIDEO_SESSION_COUNT_MASK;
        if (count == 0) {
          BOOST_LOG(error) << "Cannot unregister a video session because the session counter is already zero"sv;
          return 0;
        }

        const auto desired = (state & VIDEO_SESSION_RECONFIGURING) | (count - 1);
        if (video_session_state.compare_exchange_weak(
              state,
              desired,
              boost::memory_order_acq_rel,
              boost::memory_order_acquire)) {
          return count - 1;
        }
      }
    }

    std::uint32_t
    video_session_count() {
      return video_session_state.load(boost::memory_order_acquire) & VIDEO_SESSION_COUNT_MASK;
    }

    template <class F>
    bool
    run_display_reconfiguration_if_single_video_session(F &&operation) {
      // 只有恰好一个视频会话时才允许重配置。CAS 成功设置占用位后，
      // 后续视频会话会在 register_video_session() 中等待，不会进入重建窗口。
      auto state = video_session_state.load(boost::memory_order_acquire);
      for (;;) {
        if ((state & VIDEO_SESSION_RECONFIGURING) != 0 ||
            (state & VIDEO_SESSION_COUNT_MASK) != 1) {
          return false;
        }

        if (video_session_state.compare_exchange_weak(
              state,
              state | VIDEO_SESSION_RECONFIGURING,
              boost::memory_order_acq_rel,
              boost::memory_order_acquire)) {
          break;
        }
      }

      // 无论重配置正常返回还是抛出异常，都必须清除占用位并唤醒等待的新会话。
      auto reconfiguration_guard = util::fail_guard([]() {
        video_session_state.fetch_and(VIDEO_SESSION_COUNT_MASK, boost::memory_order_release);
        video_session_state.notify_all();
      });
      std::forward<F>(operation)();
      return true;
    }
  }  // namespace session

  /**
   * First part of cipher must be struct of type control_encrypted_t
   *
   * returns empty string_view on failure
   * returns string_view pointing to payload data
   */
  template <std::size_t max_payload_size>
  static inline std::string_view
  encode_control(session_t *session, const std::string_view &plaintext, std::array<std::uint8_t, max_payload_size> &tagged_cipher) {
    static_assert(
      max_payload_size >= sizeof(control_encrypted_t) + sizeof(crypto::cipher::tag_size),
      "max_payload_size >= sizeof(control_encrypted_t) + sizeof(crypto::cipher::tag_size)");

    if (session->config.controlProtocolType != 13) {
      return plaintext;
    }

    auto seq = session->control.seq++;

    auto &iv = session->control.outgoing_iv;
    if (session->config.encryptionFlagsEnabled & SS_ENC_CONTROL_V2) {
      // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
      // Section 8.2.1. The sequence number is our "invocation" field and the 'CH' in the
      // high bytes is the "fixed" field. Because each client provides their own unique
      // key, our values in the fixed field need only uniquely identify each independent
      // use of the client's key with AES-GCM in our code.
      //
      // The sequence number is 32 bits long which allows for 2^32 control stream messages
      // to be sent to each client before the IV repeats.
      iv.resize(12);
      std::copy_n((uint8_t *) &seq, sizeof(seq), std::begin(iv));
      iv[10] = 'H';  // Host originated
      iv[11] = 'C';  // Control stream
    }
    else {
      // Nvidia's old style encryption uses a 16-byte IV
      iv.resize(16);

      iv[0] = (std::uint8_t) seq;
    }

    auto packet = (control_encrypted_p) tagged_cipher.data();

    auto bytes = session->control.cipher.encrypt(plaintext, packet->payload(), &iv);
    if (bytes <= 0) {
      BOOST_LOG(error) << "Couldn't encrypt control data"sv;
      return {};
    }

    std::uint16_t packet_length = bytes + crypto::cipher::tag_size + sizeof(control_encrypted_t::seq);

    packet->encryptedHeaderType = util::endian::little(0x0001);
    packet->length = util::endian::little(packet_length);
    packet->seq = util::endian::little(seq);

    return std::string_view { (char *) tagged_cipher.data(), packet_length + sizeof(control_encrypted_t) - sizeof(control_encrypted_t::seq) };
  }

  bool
  open_mic_socket_locked(broadcast_ctx_t &ctx) {
    if (ctx.mic_sock.is_open()) {
      ctx.mic_socket_enabled.store(true);
      return true;
    }

    ctx.mic_socket_enabled.store(false);

    auto address_family = net::af_from_enum_string(config::sunshine.address_family);
    auto protocol = address_family == net::IPV4 ? udp::v4() : udp::v6();
    auto mic_port = net::map_port(MIC_STREAM_PORT);
    boost::system::error_code ec;

    const auto bind_address = boost::asio::ip::make_address(net::get_bind_address(address_family), ec);
    if (ec) {
      BOOST_LOG(error) << "Invalid microphone bind address: "sv << ec.message();
      return false;
    }

    ctx.mic_sock.open(protocol, ec);
    if (ec) {
      BOOST_LOG(error) << "Couldn't re-open Microphone socket: "sv << ec.message();
      return false;
    }

    ctx.mic_sock.bind(udp::endpoint(bind_address, mic_port), ec);
    if (ec) {
      BOOST_LOG(error) << "Couldn't re-bind Microphone socket to port ["sv << mic_port << "]: "sv << ec.message();
      boost::system::error_code close_ec;
      ctx.mic_sock.close(close_ec);
      return false;
    }

    ctx.mic_socket_enabled.store(true);
    BOOST_LOG(info) << "Microphone socket opened on " << bind_address << ':' << mic_port;
    return true;
  }

  void
  close_mic_socket_locked(broadcast_ctx_t &ctx) {
    ctx.mic_socket_enabled.store(false);

    if (!ctx.mic_sock.is_open()) {
      return;
    }

    boost::system::error_code ec;
    ctx.mic_sock.close(ec);
    if (ec && ec != boost::asio::error::bad_descriptor) {
      BOOST_LOG(warning) << "Couldn't close Microphone socket: "sv << ec.message();
    }
  }

  void
  cancel_mic_receive_locked(broadcast_ctx_t &ctx) {
    if (!ctx.mic_sock.is_open()) {
      return;
    }

    boost::system::error_code ec;
    ctx.mic_sock.cancel(ec);
    if (ec && ec != boost::asio::error::bad_descriptor) {
      BOOST_LOG(warning) << "Couldn't cancel Microphone receive: "sv << ec.message();
    }
  }

  /**
   * @brief Stop all microphone reception during broadcast shutdown.
   * @param ctx The shared broadcast context.
   * @return true if an open socket was closed.
   */
  bool
  disable_mic_socket(broadcast_ctx_t &ctx) {
    boost::lock_guard<boost::mutex> session_lock(ctx.mic_session_mutex);
    ctx.mic_sessions.clear();
    ctx.mic_session_count.store(0, boost::memory_order_release);

    boost::lock_guard<boost::mutex> socket_lock(ctx.mic_socket_mutex);
    const auto was_open = ctx.mic_sock.is_open();
    close_mic_socket_locked(ctx);
    return was_open;
  }

  /**
   * @brief Release the microphone source registered by the ending session.
   * @param ctx The shared broadcast context.
   * @param session_id The ID of the ending session.
   * @return true if a source registration was removed.
   */
  bool
  release_mic_session(broadcast_ctx_t &ctx, std::uint32_t session_id) {
    boost::lock_guard<boost::mutex> session_lock(ctx.mic_session_mutex);
    if (ctx.mic_sessions.erase(session_id) == 0) {
      return false;
    }

    const auto remaining_sessions = static_cast<std::uint32_t>(ctx.mic_sessions.size());
    ctx.mic_session_count.store(remaining_sessions, boost::memory_order_release);
    if (remaining_sessions == 0) {
      // 记录会话计数经过零点的边沿。即使新会话在 cancel 回调执行前接入，
      // 麦克风线程也必须从新的绝对播放截止时间开始。
      ctx.mic_playout_reset_pending.store(true, boost::memory_order_release);
    }
    if (remaining_sessions != 0) {
      return true;
    }

    boost::lock_guard<boost::mutex> socket_lock(ctx.mic_socket_mutex);
    cancel_mic_receive_locked(ctx);
    return true;
  }

  /**
   * @brief Register an independently decoded microphone source.
   * @param session The session that completed microphone SETUP.
   * @param client_address The authenticated RTSP peer address.
   */
  void
  setup_mic_for_session(session_t &session, const boost::asio::ip::address &client_address) {
    auto &ctx = *session.broadcast_ref.get();

    const auto should_enable_mic_encryption = (session.config.encryptionFlagsEnabled & SS_ENC_MIC) != 0;
    std::optional<broadcast_ctx_t::mic_cipher_ctx_t> cipher;
    if (should_enable_mic_encryption) {
      cipher.emplace(
        session.audio.cipher.key,
        session.audio.cipher.padding
      );
    }

    auto registration = boost::make_shared<broadcast_ctx_t::mic_session_ctx_t>(
      session.launch_session_id,
      session.audio.avRiKeyId,
      client_address,
      std::move(cipher)
    );

    {
      // 统一会话表和套接字生命周期的加锁顺序，避免最后一个旧会话退出时
      // 关闭刚为新会话打开的套接字。
      boost::lock_guard<boost::mutex> session_lock(ctx.mic_session_mutex);
      boost::lock_guard<boost::mutex> socket_lock(ctx.mic_socket_mutex);
      if (!open_mic_socket_locked(ctx)) {
        BOOST_LOG(error) << "Failed to open microphone socket for " << session.client_name;
        return;
      }

      session.audio.mic_registered = true;
      ctx.mic_sessions[session.launch_session_id] = registration;
      ctx.mic_session_count.store(
        static_cast<std::uint32_t>(ctx.mic_sessions.size()),
        boost::memory_order_release);
    }

    BOOST_LOG(info) << "Client " << session.client_name << ": Microphone encryption "
                    << (should_enable_mic_encryption ? "ENABLED" : "DISABLED")
                    << " (session " << session.launch_session_id
                    << " registered for multi-client mixing)";
  }

  int
  start_broadcast(broadcast_ctx_t &ctx);
  void
  end_broadcast(broadcast_ctx_t &ctx);

  static int
  clamp_total_bitrate_to_host_cap(int bitrate_kbps, std::string_view client_name) {
    if (config::video.max_bitrate > 0 && bitrate_kbps > config::video.max_bitrate) {
      BOOST_LOG(info) << "Clamping dynamic bitrate for client '" << client_name
                      << "' from " << bitrate_kbps << " Kbps to host maximum "
                      << config::video.max_bitrate << " Kbps";
      return config::video.max_bitrate;
    }
    return bitrate_kbps;
  }

  static auto broadcast_shared = safe::make_shared<broadcast_ctx_t>(start_broadcast, end_broadcast);

  session_t *
  control_server_t::get_session(const net::peer_t peer, uint32_t connect_data) {
    {
      // Fast path - look up existing session by peer
      auto lg = _peer_to_session.lock();
      auto it = _peer_to_session->find(peer);
      if (it != _peer_to_session->end()) {
        return it->second;
      }
    }

    // Slow path - process new session
    TUPLE_2D(peer_port, peer_addr, platf::from_sockaddr_ex((sockaddr *) &peer->address.address));
    auto lg = _sessions.lock();
    for (auto pos = std::begin(*_sessions); pos != std::end(*_sessions); ++pos) {
      auto session_p = *pos;

      // Skip sessions that are already established
      if (session_p->control.peer) {
        continue;
      }

      // Identify the connection by the unique connect data if the client supports it.
      // Only fall back to IP address matching for clients without session ID support.
      if (session_p->config.mlFeatureFlags & ML_FF_SESSION_ID_V1) {
        if (session_p->control.connect_data != connect_data) {
          continue;
        }
        else {
          BOOST_LOG(debug) << "Initialized new control stream session by connect data match [v2]"sv;
        }
      }
      else {
        if (session_p->control.expected_peer_address != peer_addr) {
          continue;
        }
        else {
          BOOST_LOG(debug) << "Initialized new control stream session by IP address match [v1]"sv;
        }
      }

      // Once the control stream connection is established, RTSP session state can be torn down
      rtsp_stream::launch_session_clear(session_p->launch_session_id);

      session_p->control.peer = peer;

      // Use the local address from the control connection as the source address
      // for other communications to the client. This is necessary to ensure
      // proper routing on multi-homed hosts.
      auto local_address = platf::from_sockaddr((sockaddr *) &peer->localAddress.address);
      session_p->localAddress = boost::asio::ip::make_address(local_address);

      BOOST_LOG(debug) << "Control local address ["sv << local_address << ']';
      BOOST_LOG(debug) << "Control peer address ["sv << peer_addr << ':' << peer_port << ']';

      // Insert this into the map for O(1) lookups in the future
      auto ptslg = _peer_to_session.lock();
      _peer_to_session->emplace(peer, session_p);
      return session_p;
    }

    return nullptr;
  }

  /**
   * @brief Call the handler for a given control stream message.
   * @param type The message type.
   * @param session The session the message was received on.
   * @param payload The payload of the message.
   * @param reinjected `true` if this message is being reprocessed after decryption.
   */
  void
  control_server_t::call(std::uint16_t type, session_t *session, const std::string_view &payload, bool reinjected) {
    // If we are using the encrypted control stream protocol, drop any messages that come off the wire unencrypted
    if (session->config.controlProtocolType == 13 && !reinjected && type != packetTypes[IDX_ENCRYPTED]) {
      BOOST_LOG(error) << "Dropping unencrypted message on encrypted control stream: "sv << util::hex(type).to_string_view();
      return;
    }

    auto cb = _map_type_cb.find(type);
    if (cb == std::end(_map_type_cb)) {
      BOOST_LOG(debug)
        << "type [Unknown] { "sv << util::hex(type).to_string_view() << " }"sv << std::endl
        << "---data---"sv << std::endl
        << util::hex_vec(payload) << std::endl
        << "---end data---"sv;
    }
    else {
      cb->second(session, payload);
    }
  }

  void
  control_server_t::iterate(std::chrono::milliseconds timeout) {
    ENetEvent event;
    auto res = enet_host_service(_host.get(), &event, timeout.count());

    if (res > 0) {
      auto session = get_session(event.peer, event.data);
      if (!session) {
        BOOST_LOG(warning) << "Rejected connection from ["sv << platf::from_sockaddr((sockaddr *) &event.peer->address.address) << "]: it's not properly set up"sv;
        enet_peer_disconnect_now(event.peer, 0);

        return;
      }

      session->pingTimeout = std::chrono::steady_clock::now() + config::stream.ping_timeout;
      session->last_control_activity_ms.store(steady_now_ms(), std::memory_order_relaxed);

      switch (event.type) {
        case ENET_EVENT_TYPE_RECEIVE: {
          net::packet_t packet { event.packet };

          auto type = *(std::uint16_t *) packet->data;
          std::string_view payload { (char *) packet->data + sizeof(type), packet->dataLength - sizeof(type) };

          call(type, session, payload, false);
        } break;
        case ENET_EVENT_TYPE_CONNECT:
          BOOST_LOG(info) << "CLIENT CONNECTED"sv;
          break;
        case ENET_EVENT_TYPE_DISCONNECT:
          BOOST_LOG(info) << "CLIENT DISCONNECTED"sv;
          // No more clients to send video data to ^_^
          session::stop(*session, session::stop_reason_e::control_disconnect);
          break;
        case ENET_EVENT_TYPE_NONE:
          break;
      }
    }
  }

  namespace fec {
    using rs_t = util::safe_ptr<reed_solomon, [](reed_solomon *rs) { reed_solomon_release(rs); }>;

    struct fec_t {
      size_t data_shards;
      size_t nr_shards;
      size_t percentage;

      size_t blocksize;
      size_t prefixsize;
      util::buffer_t<char> shards;
      util::buffer_t<char> headers;
      util::buffer_t<uint8_t *> shards_p;

      std::vector<platf::buffer_descriptor_t> payload_buffers;

      char *
      data(size_t el) {
        return (char *) shards_p[el];
      }

      char *
      prefix(size_t el) {
        return prefixsize ? &headers[el * prefixsize] : nullptr;
      }

      size_t
      size() const {
        return nr_shards;
      }
    };

    static fec_t
    encode(const std::string_view &payload, size_t blocksize, size_t fecpercentage, size_t minparityshards, size_t prefixsize) {
      auto payload_size = payload.size();

      auto pad = payload_size % blocksize != 0;

      auto aligned_data_shards = payload_size / blocksize;
      auto data_shards = aligned_data_shards + (pad ? 1 : 0);
      auto parity_shards = (data_shards * fecpercentage + 99) / 100;

      // increase the FEC percentage for this frame if the parity shard minimum is not met
      if (parity_shards < minparityshards && fecpercentage != 0) {
        parity_shards = minparityshards;
        fecpercentage = (100 * parity_shards) / data_shards;

        BOOST_LOG(verbose) << "Increasing FEC percentage to "sv << fecpercentage << " to meet parity shard minimum"sv << std::endl;
      }

      auto nr_shards = data_shards + parity_shards;

      // If we need to store a zero-padded data shard, allocate that first to
      // to keep the shards in order and reduce buffer fragmentation
      auto parity_shard_offset = pad ? 1 : 0;
      util::buffer_t<char> shards { (parity_shard_offset + parity_shards) * blocksize };
      util::buffer_t<uint8_t *> shards_p { nr_shards };
      std::vector<platf::buffer_descriptor_t> payload_buffers;
      payload_buffers.reserve(2);

      // Point into the payload buffer for all except the final padded data shard
      auto next = std::begin(payload);
      for (auto x = 0; x < aligned_data_shards; ++x) {
        shards_p[x] = (uint8_t *) next;
        next += blocksize;
      }
      payload_buffers.emplace_back(std::begin(payload), aligned_data_shards * blocksize);

      // If the last data shard needs to be zero-padded, we must use the shards buffer
      if (pad) {
        shards_p[aligned_data_shards] = (uint8_t *) &shards[0];

        // GCC doesn't figure out that std::copy_n() can be replaced with memcpy() here
        // and ends up compiling a horribly slow element-by-element copy loop, so we
        // help it by using memcpy()/memset() directly.
        auto copy_len = std::min<size_t>(blocksize, std::end(payload) - next);
        std::memcpy(shards_p[aligned_data_shards], next, copy_len);
        if (copy_len < blocksize) {
          // Zero any additional space after the end of the payload
          std::memset(shards_p[aligned_data_shards] + copy_len, 0, blocksize - copy_len);
        }
      }

      // Add a payload buffer describing the shard buffer
      payload_buffers.emplace_back(std::begin(shards), shards.size());

      if (fecpercentage != 0) {
        // Point into our allocated buffer for the parity shards
        for (auto x = 0; x < parity_shards; ++x) {
          shards_p[data_shards + x] = (uint8_t *) &shards[(parity_shard_offset + x) * blocksize];
        }

        // packets = parity_shards + data_shards
        rs_t rs { reed_solomon_new(data_shards, parity_shards) };

        reed_solomon_encode(rs.get(), shards_p.begin(), nr_shards, blocksize);
      }

      return {
        data_shards,
        nr_shards,
        fecpercentage,
        blocksize,
        prefixsize,
        std::move(shards),
        util::buffer_t<char> { nr_shards * prefixsize },
        std::move(shards_p),
        std::move(payload_buffers),
      };
    }
  }  // namespace fec

  /**
   * @brief Combines two buffers and inserts new buffers at each slice boundary of the result.
   * @param insert_size The number of bytes to insert.
   * @param slice_size The number of bytes between insertions.
   * @param data1 The first data buffer.
   * @param data2 The second data buffer.
   */
  std::vector<uint8_t>
  concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2) {
    auto data_size = data1.size() + data2.size();
    auto pad = data_size % slice_size != 0;
    auto elements = data_size / slice_size + (pad ? 1 : 0);

    std::vector<uint8_t> result;
    result.resize(elements * insert_size + data_size);

    auto next = std::begin(data1);
    auto end = std::end(data1);
    for (auto x = 0; x < elements; ++x) {
      void *p = &result[x * (insert_size + slice_size)];

      // For the last iteration, only copy to the end of the data
      if (x == elements - 1) {
        slice_size = data_size - (x * slice_size);
      }

      // Test if this slice will extend into the next buffer
      if (next + slice_size > end) {
        // Copy the first portion from the first buffer
        auto copy_len = end - next;
        std::copy(next, end, (char *) p + insert_size);

        // Copy the remaining portion from the second buffer
        next = std::begin(data2);
        end = std::end(data2);
        std::copy(next, next + (slice_size - copy_len), (char *) p + copy_len + insert_size);
        next += slice_size - copy_len;
      }
      else {
        std::copy(next, next + slice_size, (char *) p + insert_size);
        next += slice_size;
      }
    }

    return result;
  }

  std::vector<uint8_t>
  replace(const std::string_view &original, const std::string_view &old, const std::string_view &_new) {
    std::vector<uint8_t> replaced;
    replaced.reserve(original.size() + _new.size() - old.size());

    auto begin = std::begin(original);
    auto end = std::end(original);
    auto next = std::search(begin, end, std::begin(old), std::end(old));

    std::copy(begin, next, std::back_inserter(replaced));
    if (next != end) {
      std::copy(std::begin(_new), std::end(_new), std::back_inserter(replaced));
      std::copy(next + old.size(), end, std::back_inserter(replaced));
    }

    return replaced;
  }

  /**
   * @brief Pass gamepad feedback data back to the client.
   * @param session The session object.
   * @param msg The message to pass.
   * @return 0 on success.
   */
  int
  send_feedback_msg(session_t *session, platf::gamepad_feedback_msg_t &msg,
                    bool synthesized_haptics = false) {
    if (!session->control.peer) {
      BOOST_LOG(warning) << "Couldn't send gamepad feedback data, still waiting for PING from Moonlight"sv;
      // Still waiting for PING from Moonlight
      return -1;
    }

    std::string payload;
    bool unreliable = false;
    if (msg.type == platf::gamepad_feedback_e::rumble) {
      control_rumble_t plaintext;
      plaintext.header.type = packetTypes[IDX_RUMBLE_DATA];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      auto &data = msg.data.rumble;
      auto &source = synthesized_haptics ?
                       session->control.synthesized_haptics_rumble :
                       session->control.compatibility_rumble;
      source[msg.id] = {data.lowfreq, data.highfreq};
      const auto compatibility = session->control.compatibility_rumble[msg.id];
      const auto synthesized = session->control.synthesized_haptics_rumble[msg.id];
      const auto lowfreq = std::max(compatibility.first, synthesized.first);
      const auto highfreq = std::max(compatibility.second, synthesized.second);

      plaintext.useless = 0xC0FFEE;
      plaintext.id = util::endian::little(msg.id);
      plaintext.lowfreq = util::endian::little(lowfreq);
      plaintext.highfreq = util::endian::little(highfreq);

      BOOST_LOG(verbose) << "Rumble: "sv << msg.id << " :: "sv << util::hex(lowfreq).to_string_view() << " :: "sv << util::hex(highfreq).to_string_view();
      std::array<std::uint8_t,
        sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    }
    else if (msg.type == platf::gamepad_feedback_e::rumble_triggers) {
      control_rumble_triggers_t plaintext;
      plaintext.header.type = packetTypes[IDX_RUMBLE_TRIGGER_DATA];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      auto &data = msg.data.rumble_triggers;

      plaintext.id = util::endian::little(msg.id);
      plaintext.left = util::endian::little(data.left_trigger);
      plaintext.right = util::endian::little(data.right_trigger);

      BOOST_LOG(verbose) << "Rumble triggers: "sv << msg.id << " :: "sv << util::hex(data.left_trigger).to_string_view() << " :: "sv << util::hex(data.right_trigger).to_string_view();
      std::array<std::uint8_t,
        sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    }
    else if (msg.type == platf::gamepad_feedback_e::set_motion_event_state) {
      control_set_motion_event_t plaintext;
      plaintext.header.type = packetTypes[IDX_SET_MOTION_EVENT];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      auto &data = msg.data.motion_event_state;

      plaintext.id = util::endian::little(msg.id);
      plaintext.reportrate = util::endian::little(data.report_rate);
      plaintext.type = data.motion_type;

      BOOST_LOG(verbose) << "Motion event state: "sv << msg.id << " :: "sv << util::hex(data.report_rate).to_string_view() << " :: "sv << util::hex(data.motion_type).to_string_view();
      std::array<std::uint8_t,
        sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    }
    else if (msg.type == platf::gamepad_feedback_e::set_rgb_led) {
      control_set_rgb_led_t plaintext;
      plaintext.header.type = packetTypes[IDX_SET_RGB_LED];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      auto &data = msg.data.rgb_led;

      plaintext.id = util::endian::little(msg.id);
      plaintext.r = data.r;
      plaintext.g = data.g;
      plaintext.b = data.b;

      BOOST_LOG(verbose) << "RGB: "sv << msg.id << " :: "sv << util::hex(data.r).to_string_view() << util::hex(data.g).to_string_view() << util::hex(data.b).to_string_view();
      std::array<std::uint8_t,
        sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    }
    else if (msg.type == platf::gamepad_feedback_e::set_adaptive_triggers) {
      control_adaptive_triggers_t plaintext;
      plaintext.header.type = packetTypes[IDX_SET_ADAPTIVE_TRIGGERS];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      plaintext.id = util::endian::little(msg.id);
      plaintext.event_flags = msg.data.adaptive_triggers.event_flags;
      plaintext.type_left = msg.data.adaptive_triggers.type_left;
      std::ranges::copy(msg.data.adaptive_triggers.left, plaintext.left);
      plaintext.type_right = msg.data.adaptive_triggers.type_right;
      std::ranges::copy(msg.data.adaptive_triggers.right, plaintext.right);

      std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    }
    else if (msg.type == platf::gamepad_feedback_e::ds5_haptics_pcm) {
      const bool sends_raw_pcm = (session->config.mlFeatureFlags & ML_FF_DS5_HAPTICS_PCM) != 0;
      const bool sends_authored_ir = (session->config.mlFeatureFlags & ML_FF_DS5_HAPTICS_IR_V2) != 0;

      const auto &data = msg.data.ds5_haptics;
      const auto pcm_size = static_cast<std::size_t>(data.frame_count) * 4;
      auto write_u16 = [](std::uint8_t *p, std::uint16_t v) {
        p[0] = static_cast<std::uint8_t>(v);
        p[1] = static_cast<std::uint8_t>(v >> 8);
      };
      auto write_u32 = [](std::uint8_t *p, std::uint32_t v) {
        p[0] = static_cast<std::uint8_t>(v);
        p[1] = static_cast<std::uint8_t>(v >> 8);
        p[2] = static_cast<std::uint8_t>(v >> 16);
        p[3] = static_cast<std::uint8_t>(v >> 24);
      };
      auto write_u64 = [&write_u32](std::uint8_t *p, std::uint64_t v) {
        write_u32(p, static_cast<std::uint32_t>(v));
        write_u32(p + 4, static_cast<std::uint32_t>(v >> 32));
      };
      if (!sends_raw_pcm && !sends_authored_ir) {
        if (!session->control.legacy_haptics_init_failed) {
          auto [controller_session, inserted] = session->control.legacy_haptics.try_emplace(msg.id);
          if (inserted) {
            auto created = std::make_unique<haptics::legacy_rumble_session_t>();
            if (!created->ready()) {
              session->control.legacy_haptics.erase(controller_session);
              session->control.legacy_haptics_init_failed = true;
              BOOST_LOG(error) << "Unable to initialize legacy DualSense haptics fallback"sv;
              return 0;
            }
            controller_session->second = std::move(created);
          }
          const auto rumble = controller_session->second->process(
            msg.id, data.flags, data.frame_count, data.sequence, data.presentation_time_us,
            std::span<const std::uint8_t> {data.pcm.data(), pcm_size});
          if (rumble) {
            auto legacy_msg = platf::gamepad_feedback_msg_t::make_rumble(
              rumble->controller_id, rumble->low_frequency, rumble->high_frequency);
            return send_feedback_msg(session, legacy_msg, true);
          }
        }
        return 0;
      }
      // A malformed client that declares both modes gets the lossless physical
      // route. Official clients reject this combination before connecting.
      else if (sends_authored_ir && !sends_raw_pcm) {
        if (!session->control.authored_haptics && !session->control.authored_haptics_init_failed) {
          auto authored_haptics = std::make_unique<haptics::authored_ir_session_t>();
          if (!authored_haptics->ready()) {
            session->control.authored_haptics_init_failed = true;
            BOOST_LOG(error) << "Unable to initialize authored DualSense haptics analysis"sv;
          }
          else {
            session->control.authored_haptics = std::move(authored_haptics);
          }
        }
        if (!session->control.authored_haptics) {
          return 0;
        }
        const auto wire = session->control.authored_haptics->process(
          msg.id, data.flags, data.frame_count, data.sequence, data.presentation_time_us,
          std::span<const std::uint8_t> {data.pcm.data(), pcm_size});
        if (!wire) {
          return 0;
        }

        std::array<std::uint8_t, sizeof(control_header_v2) + haptics::authored_ir_v2_wire_size> plaintext {};
        auto *control = plaintext.data();
        write_u16(control, packetTypes[IDX_DS5_HAPTICS_IR_V2]);
        write_u16(control + 2, haptics::authored_ir_v2_wire_size);
        std::ranges::copy(*wire, control + sizeof(control_header_v2));

        std::array<std::uint8_t,
          sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(plaintext.size()) + crypto::cipher::tag_size>
          encrypted_payload;
        payload = encode_control(session, util::view(plaintext), encrypted_payload);
      }
      else {
        constexpr std::size_t wire_header_size = 28;
        std::vector<std::uint8_t> plaintext(sizeof(control_header_v2) + wire_header_size + pcm_size);
        auto *control = plaintext.data();
        write_u16(control, packetTypes[IDX_DS5_HAPTICS_PCM]);
        write_u16(control + 2, static_cast<std::uint16_t>(wire_header_size + pcm_size));
        auto *wire = control + sizeof(control_header_v2);
        wire[0] = 1;
        wire[1] = data.flags;
        write_u16(wire + 2, wire_header_size);
        write_u16(wire + 4, msg.id);
        write_u16(wire + 6, data.frame_count);
        write_u32(wire + 8, data.sequence);
        write_u64(wire + 12, data.presentation_time_us);
        write_u32(wire + 20, 48000);
        wire[24] = 2;
        wire[25] = 16;
        wire[26] = wire[27] = 0;
        std::copy_n(data.pcm.begin(), pcm_size, wire + wire_header_size);

        std::array<std::uint8_t,
          sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(
            sizeof(control_header_v2) + wire_header_size + 240 * 4) + crypto::cipher::tag_size>
          encrypted_payload;
        payload = encode_control(session,
                                 std::string_view(reinterpret_cast<const char *>(plaintext.data()), plaintext.size()),
                                 encrypted_payload);
      }
      unreliable = true;
    }
    else {
      BOOST_LOG(error) << "Unknown gamepad feedback message type"sv;
      return -1;
    }

    const auto send_result = unreliable
      ? session->broadcast_ref->control_server.send_unreliable(payload, session->control.peer)
      : session->broadcast_ref->control_server.send(payload, session->control.peer);
    if (send_result) {
      TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
      BOOST_LOG(warning) << "Couldn't send gamepad feedback to ["sv << addr << ':' << port << ']';

      return -1;
    }

    return 0;
  }

  int
  send_hdr_mode(session_t *session, video::hdr_info_t hdr_info) {
    if (!session->control.peer) {
      BOOST_LOG(warning) << "Couldn't send HDR mode, still waiting for PING from Moonlight"sv;
      // Still waiting for PING from Moonlight
      return -1;
    }

    control_hdr_mode_t plaintext {};
    plaintext.header.type = packetTypes[IDX_HDR_MODE];
    plaintext.header.payloadLength = sizeof(control_hdr_mode_t) - sizeof(control_header_v2);

    plaintext.enabled = hdr_info->enabled;
    plaintext.metadata = hdr_info->metadata;

    std::array<std::uint8_t,
      sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
      encrypted_payload;

    auto payload = encode_control(session, util::view(plaintext), encrypted_payload);
    if (session->broadcast_ref->control_server.send(payload, session->control.peer)) {
      TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
      BOOST_LOG(warning) << "Couldn't send HDR mode to ["sv << addr << ':' << port << ']';

      return -1;
    }

    BOOST_LOG(debug) << "Sent HDR mode: " << hdr_info->enabled;
    return 0;
  }

  int
  send_resolution_change(session_t *session, std::uint32_t width, std::uint32_t height) {
    if (!session->control.peer) {
      BOOST_LOG(warning) << "Couldn't send resolution change, still waiting for PING from Moonlight"sv;
      // Still waiting for PING from Moonlight
      return -1;
    }

    control_resolution_change_t plaintext {};
    plaintext.header.type = packetTypes[IDX_RESOLUTION_CHANGE];
    plaintext.header.payloadLength = sizeof(control_resolution_change_t) - sizeof(control_header_v2);

    plaintext.width = util::endian::little(width);
    plaintext.height = util::endian::little(height);

    std::array<std::uint8_t,
      sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
      encrypted_payload;

    auto payload = encode_control(session, util::view(plaintext), encrypted_payload);
    if (session->broadcast_ref->control_server.send(payload, session->control.peer)) {
      TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
      BOOST_LOG(warning) << "Couldn't send resolution change to ["sv << addr << ':' << port << ']';

      return -1;
    }

    BOOST_LOG(debug) << "Sent resolution change: " << width << "x" << height;
    return 0;
  }

  /**
   * Variable-length sibling of encode_control(). Encrypts an arbitrary-sized
   * plaintext into the caller-provided output buffer. The output buffer must be
   * at least `sizeof(control_encrypted_t) + round_to_pkcs7_padded(plaintext.size()) + tag_size`.
   * Returns the encoded view, or empty on failure / insufficient capacity.
   */
  static std::string_view
  encode_control_buf(session_t *session, std::string_view plaintext, std::uint8_t *out, std::size_t out_cap) {
    const std::size_t needed = sizeof(control_encrypted_t)
                               + crypto::cipher::round_to_pkcs7_padded(plaintext.size())
                               + crypto::cipher::tag_size;
    if (out_cap < needed) {
      BOOST_LOG(error) << "encode_control_buf: insufficient buffer ("sv << out_cap << " < "sv << needed << ")"sv;
      return {};
    }

    if (session->config.controlProtocolType != 13) {
      // Pre-v2 control protocol — caller must already have written plaintext;
      // not used by clipboard sync (gated by gui_alive() + capability flags).
      return plaintext;
    }

    auto seq = session->control.seq++;

    auto &iv = session->control.outgoing_iv;
    if (session->config.encryptionFlagsEnabled & SS_ENC_CONTROL_V2) {
      iv.resize(12);
      std::copy_n((uint8_t *) &seq, sizeof(seq), std::begin(iv));
      iv[10] = 'H';
      iv[11] = 'C';
    }
    else {
      iv.resize(16);
      iv[0] = (std::uint8_t) seq;
    }

    auto packet = (control_encrypted_p) out;
    auto bytes = session->control.cipher.encrypt(plaintext, packet->payload(), &iv);
    if (bytes <= 0) {
      BOOST_LOG(error) << "encode_control_buf: encrypt failed"sv;
      return {};
    }

    std::uint16_t packet_length = bytes + crypto::cipher::tag_size + sizeof(control_encrypted_t::seq);
    packet->encryptedHeaderType = util::endian::little(0x0001);
    packet->length = util::endian::little(packet_length);
    packet->seq = util::endian::little(seq);

    return std::string_view {
      (char *) out,
      packet_length + sizeof(control_encrypted_t) - sizeof(control_encrypted_t::seq)
    };
  }

  /**
   * Send a clipboard payload as an IDX_CLIPBOARD control packet. `payload` is
   * an opaque byte string; this function adds the framing header and encrypts.
   * Returns 0 on success, -1 on failure or oversized payload. The raw payload
   * is capped below 65535 bytes so the encrypted control frame length also
   * fits in the protocol's 16-bit length field.
   */
  int
  send_clipboard(session_t *session, const clipboard_bridge::payload_t &payload) {
    if (!session->control.peer) {
      return -1;
    }

    const std::size_t plaintext_size = sizeof(control_header_v2) + payload.size();
    const std::size_t encrypted_packet_length =
      crypto::cipher::round_to_pkcs7_padded(plaintext_size)
      + crypto::cipher::tag_size
      + sizeof(control_encrypted_t::seq);
    if (payload.size() > clipboard_bridge::kMaxPayloadBytes ||
        encrypted_packet_length > std::numeric_limits<std::uint16_t>::max()) {
      BOOST_LOG(warning) << "send_clipboard: payload too large ("sv << payload.size()
                         << " bytes; encrypted control frame would be "sv
                         << encrypted_packet_length << " bytes)"sv;
      return -1;
    }

    std::vector<std::uint8_t> plaintext(plaintext_size);
    auto *hdr = reinterpret_cast<control_header_v2 *>(plaintext.data());
    hdr->type = packetTypes[IDX_CLIPBOARD];
    hdr->payloadLength = (std::uint16_t) payload.size();
    if (!payload.empty()) {
      std::memcpy(plaintext.data() + sizeof(control_header_v2), payload.data(), payload.size());
    }

    std::vector<std::uint8_t> encrypted(
      sizeof(control_encrypted_t)
      + crypto::cipher::round_to_pkcs7_padded(plaintext.size())
      + crypto::cipher::tag_size);

    auto view = encode_control_buf(
      session,
      std::string_view { (char *) plaintext.data(), plaintext.size() },
      encrypted.data(),
      encrypted.size());
    if (view.empty()) {
      return -1;
    }

    if (session->broadcast_ref->control_server.send(view, session->control.peer)) {
      BOOST_LOG(warning) << "Couldn't send clipboard packet"sv;
      return -1;
    }
    return 0;
  }

  int
  send_text_context(session_t *session, const text_context::payload_t &payload) {
    if (!session->control.peer || session->config.controlProtocolType != 13 ||
        payload.size() != text_context::kWireSize) {
      return -1;
    }
    const std::size_t plaintext_size = sizeof(control_header_v2) + payload.size();
    std::vector<std::uint8_t> plaintext(plaintext_size);
    auto *hdr = reinterpret_cast<control_header_v2 *>(plaintext.data());
    hdr->type = packetTypes[IDX_TEXT_CONTEXT];
    hdr->payloadLength = static_cast<std::uint16_t>(payload.size());
    std::memcpy(plaintext.data() + sizeof(control_header_v2), payload.data(), payload.size());

    std::vector<std::uint8_t> encrypted(
      sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(plaintext.size()) +
      crypto::cipher::tag_size);
    const auto view = encode_control_buf(session,
      std::string_view {reinterpret_cast<char *>(plaintext.data()), plaintext.size()},
      encrypted.data(), encrypted.size());
    if (view.empty() || session->broadcast_ref->control_server.send(view, session->control.peer)) {
      BOOST_LOG(warning) << "Couldn't send remote text context packet"sv;
      return -1;
    }
    return 0;
  }

  int
  send_cursor_update(session_t *session,
                     const cursor_channel::snapshot_t &cursor,
                     bool include_shape) {
    if (!session->control.peer || session->config.controlProtocolType != 13) {
      return -1;
    }

    const std::size_t total_size = include_shape ? cursor.bgra.size() : 0;
    if (include_shape &&
        (!cursor.has_shape || cursor.width == 0 || cursor.height == 0 ||
         total_size != static_cast<std::size_t>(cursor.width) * cursor.height * 4u ||
         total_size > std::numeric_limits<std::uint32_t>::max())) {
      BOOST_LOG(warning) << "Refusing invalid local cursor shape"sv;
      return -1;
    }

    std::size_t offset = 0;
    do {
      const std::size_t chunk_size =
        include_shape ? std::min(cursor_chunk_bytes, total_size - offset) : 0;
      const std::size_t cursor_payload_size = sizeof(cursor_update_header_t) + chunk_size;
      const std::size_t plaintext_size = sizeof(control_header_v2) + cursor_payload_size;
      const std::size_t encrypted_packet_length =
        crypto::cipher::round_to_pkcs7_padded(plaintext_size)
        + crypto::cipher::tag_size
        + sizeof(control_encrypted_t::seq);
      if (cursor_payload_size > std::numeric_limits<std::uint16_t>::max() ||
          encrypted_packet_length > std::numeric_limits<std::uint16_t>::max()) {
        BOOST_LOG(warning) << "Local cursor chunk exceeds control packet limit"sv;
        return -1;
      }

      std::vector<std::uint8_t> plaintext(plaintext_size);
      auto *control_header = reinterpret_cast<control_header_v2 *>(plaintext.data());
      control_header->type = util::endian::little<std::uint16_t>(packetTypes[IDX_CURSOR]);
      control_header->payloadLength =
        util::endian::little<std::uint16_t>(static_cast<std::uint16_t>(cursor_payload_size));

      auto *cursor_header = reinterpret_cast<cursor_update_header_t *>(
        plaintext.data() + sizeof(control_header_v2)
      );
      cursor_header->version = cursor_protocol_version;
      cursor_header->flags = (include_shape ? cursor_flag_shape : 0) |
                             (cursor.visible ? cursor_flag_visible : 0);
      cursor_header->header_size =
        util::endian::little<std::uint16_t>(sizeof(cursor_update_header_t));
      cursor_header->shape_id = util::endian::little(cursor.shape_id);
      cursor_header->width =
        util::endian::little<std::uint16_t>(include_shape ? cursor.width : 0);
      cursor_header->height =
        util::endian::little<std::uint16_t>(include_shape ? cursor.height : 0);
      cursor_header->hotspot_x = util::endian::little<std::int16_t>(
        include_shape ? cursor.hotspot_x : 0
      );
      cursor_header->hotspot_y = util::endian::little<std::int16_t>(
        include_shape ? cursor.hotspot_y : 0
      );
      cursor_header->total_size =
        util::endian::little<std::uint32_t>(static_cast<std::uint32_t>(total_size));
      cursor_header->offset =
        util::endian::little<std::uint32_t>(static_cast<std::uint32_t>(offset));
      cursor_header->chunk_size =
        util::endian::little<std::uint16_t>(static_cast<std::uint16_t>(chunk_size));
      cursor_header->reserved = 0;

      if (chunk_size != 0) {
        std::memcpy(
          plaintext.data() + sizeof(control_header_v2) + sizeof(cursor_update_header_t),
          cursor.bgra.data() + offset,
          chunk_size
        );
      }

      std::vector<std::uint8_t> encrypted(
        sizeof(control_encrypted_t)
        + crypto::cipher::round_to_pkcs7_padded(plaintext.size())
        + crypto::cipher::tag_size
      );
      auto view = encode_control_buf(
        session,
        std::string_view {reinterpret_cast<char *>(plaintext.data()), plaintext.size()},
        encrypted.data(),
        encrypted.size()
      );
      if (view.empty() ||
          session->broadcast_ref->control_server.send(view, session->control.peer)) {
        BOOST_LOG(warning) << "Couldn't send local cursor update"sv;
        return -1;
      }

      offset += chunk_size;
    } while (include_shape && offset < total_size);

    return 0;
  }

  void
  controlBroadcastThread(control_server_t *server) {
    server->map(packetTypes[IDX_PERIODIC_PING], [](session_t *session, const std::string_view &payload) {
      BOOST_LOG(verbose) << "type [IDX_PERIODIC_PING]"sv;
    });

    server->map(packetTypes[IDX_START_A], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_START_A]"sv;
    });

    server->map(packetTypes[IDX_START_B], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_START_B]"sv;
    });

    server->map(packetTypes[IDX_LOSS_STATS], [&](session_t *session, const std::string_view &payload) {
      int32_t *stats = (int32_t *) payload.data();
      auto count = stats[0];
      std::chrono::milliseconds t { stats[1] };

      auto lastGoodFrame = stats[3];

      BOOST_LOG(verbose)
        << "type [IDX_LOSS_STATS]"sv << std::endl
        << "---begin stats---" << std::endl
        << "loss count since last report [" << count << ']' << std::endl
        << "time in milli since last report [" << t.count() << ']' << std::endl
        << "last good frame [" << lastGoodFrame << ']' << std::endl
        << "---end stats---";
    });

    server->map(packetTypes[IDX_REQUEST_IDR_FRAME], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_REQUEST_IDR_FRAME]"sv;

      session->video.idr_events->raise(true);
    });

    // 辅助函数：处理分辨率变更
    auto handle_resolution_change = [](session_t *session, int new_width, int new_height) {
      int old_width = session->config.monitor.width;
      int old_height = session->config.monitor.height;
      
      BOOST_LOG(info) << "Dynamic resolution change requested: " << old_width << "x" << old_height 
                      << " -> " << new_width << "x" << new_height;

      // 验证分辨率范围
      constexpr int MAX_RESOLUTION = 16384;
      if (new_width <= 0 || new_width > MAX_RESOLUTION || new_height <= 0 || new_height > MAX_RESOLUTION) {
        BOOST_LOG(warning) << "Invalid resolution value: " << new_width << "x" << new_height;
        return;
      }

      // 检查分辨率是否真的改变了
      if (old_width == new_width && old_height == new_height) {
        BOOST_LOG(debug) << "Resolution unchanged, ignoring request";
        return;
      }

      // 检测是否是旋转导致的宽高互换（例如：1920x1080 -> 1080x1920）
      bool is_rotation = (old_width == new_height && old_height == new_width);
      if (is_rotation) {
        BOOST_LOG(info) << "Detected display rotation: width and height swapped";
      }

      // 更新会话配置
      session->config.monitor.width = new_width;
      session->config.monitor.height = new_height;
      perf::update_session_display(
        session->launch_session_id,
        session->config.monitor.width,
        session->config.monitor.height,
        session->config.monitor.framerate
      );

      // 创建临时的 launch_session_t 来更新显示设备配置
      // 注意：必须按照结构体声明顺序初始化字段
      rtsp_stream::launch_session_t temp_launch_session {};
      temp_launch_session.id = session->launch_session_id;
      temp_launch_session.client_cert_uuid = session->client_cert_uuid;
      temp_launch_session.client_name = session->client_name;
      temp_launch_session.width = new_width;
      temp_launch_session.height = new_height;
      temp_launch_session.fps = session->config.monitor.framerate;
      temp_launch_session.enable_hdr = session->enable_hdr;
      // Keep the synthetic-HDR state and the resolved frame pipeline policy in
      // sync with the active session; dropping them would re-parse the display
      // prep decision and flip the source display to HDR mid-stream.
      temp_launch_session.synthetic_hdr = session->synthetic_hdr;
      temp_launch_session.frame_pipeline_policy = session->frame_pipeline_policy;
      temp_launch_session.frame_pipeline_policy_resolved = session->frame_pipeline_policy_resolved;
      temp_launch_session.enable_sops = session->enable_sops;
      temp_launch_session.use_vdd = session->use_vdd;
      temp_launch_session.custom_screen_mode = session->custom_screen_mode;
      temp_launch_session.hdr_capabilities = session->hdr_capabilities;
      temp_launch_session.reported_hdr_capabilities = session->reported_hdr_capabilities;
      const auto dynamic_sdr_white_nits = session->dynamic_sdr_white_nits.load(std::memory_order_acquire);
      temp_launch_session.hdr_capabilities.sdr_white_nits = dynamic_sdr_white_nits;
      temp_launch_session.reported_hdr_capabilities.sdr_white_nits = dynamic_sdr_white_nits;
      temp_launch_session.hdr_target_source = session->hdr_target_source;

      bool active_display_resolved = true;
      const auto active_display_event = mail::man->event<std::string>(mail::active_display);
      const auto active_display = active_display_event->view(std::chrono::milliseconds {0});
      const bool has_active_display = active_display && !active_display->empty();
      const std::string target_display = has_active_display ?
                                           *active_display :
                                           session->config.monitor.display_name;
      if (!target_display.empty()) {
        const auto devices = display_device::enum_available_devices();
        const auto device_it = std::find_if(devices.begin(), devices.end(), [&](const auto &entry) {
          return entry.first == target_display ||
                 entry.second.display_name == target_display ||
                 entry.second.friendly_name == target_display;
        });
        if (device_it != devices.end()) {
          temp_launch_session.env["SUNSHINE_CLIENT_DISPLAY_NAME"] = device_it->first;
          temp_launch_session.use_vdd = device_it->second.friendly_name == ZAKO_NAME;
        }
        else if (has_active_display) {
          active_display_resolved = false;
          BOOST_LOG(warning) << "Current capture display [" << target_display
                             << "] is no longer available; skipping display device reconfiguration";
        }
        else {
          temp_launch_session.env["SUNSHINE_CLIENT_DISPLAY_NAME"] = target_display;
        }
      }

      // 更新显示设备配置（重新配置模式）
      // 注意：这也会触发捕获端和编码器的重新初始化，以适配新的分辨率
      if (is_rotation) {
        BOOST_LOG(info) << "Reconfiguring display device for rotation: " << old_width << "x" << old_height 
                        << " -> " << new_width << "x" << new_height;
      }
      else {
        BOOST_LOG(info) << "Reconfiguring display device for new resolution: " << old_width << "x" << old_height 
                        << " -> " << new_width << "x" << new_height;
      }
      
      if (active_display_resolved) {
        const bool display_reconfigured = stream::session::run_display_reconfiguration_if_single_video_session([&]() {
          const auto result = display_device::session_t::get().configure_display(config::video, temp_launch_session, true);
          if (!result) {
            BOOST_LOG(warning) << "Dynamic display reconfiguration failed: " << result.message;
          }
        });
        if (!display_reconfigured) {
          BOOST_LOG(info) << "Skipping display device reconfiguration for the dynamic resolution request because other streaming sessions are active";
        }
      }
      else {
        BOOST_LOG(info) << "Dynamic stream resolution updated without changing the unavailable capture display";
      }

      // 请求 IDR 帧以确保客户端能正确显示新分辨率
      // 这对于旋转场景特别重要，因为宽高互换需要新的关键帧
      session->video.idr_events->raise(true);

      // 注意：编码器和触摸端口的更新会在捕获端重新初始化时自动处理
      // - 编码器会在重新初始化时使用新的宽高（通过 config.monitor.width/height）
      // - 触摸端口会在视频捕获循环中通过 make_port() 自动更新
      BOOST_LOG(info) << "Dynamic stream resolution updated: " << new_width << "x" << new_height
                      << (is_rotation ? " (rotation detected)" : "");
    };

    // 统一动态参数更新协议 (IDX_DYNAMIC_PARAM_CHANGE)
    // Payload 格式：
    // - 参数类型 (int, 4字节): 0=分辨率, 1=FPS, 2=码率, 3=QP, 4=FEC, 5=预设, 6=自适应量化, 7=多遍编码, 8=VBV缓冲区, 9=客户端SDR白位
    // - 参数值：
    //   * 分辨率 (类型0): 2个int (8字节, width和height)
    //   * FPS (类型1): 1个float (4字节)
    //   * 其他单值参数（码率、QP等）: 1个int (4字节)
    server->map(packetTypes[IDX_DYNAMIC_PARAM_CHANGE], [&, handle_resolution_change](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_DYNAMIC_PARAM_CHANGE]"sv;

      constexpr size_t WIRE_WORD_SIZE = sizeof(std::uint32_t);
      constexpr size_t MIN_PAYLOAD_SIZE = WIRE_WORD_SIZE;
      if (payload.size() < MIN_PAYLOAD_SIZE) {
        BOOST_LOG(warning) << "Invalid payload size for dynamic param change. Expected at least " 
                           << MIN_PAYLOAD_SIZE << " bytes, got " << payload.size();
        return;
      }

      const auto param_type = read_dynamic_param_i32(payload, 0);
      
      if (param_type < 0 || param_type >= static_cast<int>(video::dynamic_param_type_e::MAX_PARAM_TYPE)) {
        BOOST_LOG(warning) << "Invalid parameter type: " << param_type;
        return;
      }

      const auto param_type_enum = static_cast<video::dynamic_param_type_e>(param_type);
      
      // 处理分辨率变更（需要两个int值）
      if (param_type_enum == video::dynamic_param_type_e::RESOLUTION) {
        constexpr size_t RESOLUTION_PAYLOAD_SIZE = WIRE_WORD_SIZE * 3;  // 类型 + width + height
        if (payload.size() < RESOLUTION_PAYLOAD_SIZE) {
          BOOST_LOG(warning) << "Invalid payload size for resolution change. Expected " 
                             << RESOLUTION_PAYLOAD_SIZE << " bytes, got " << payload.size();
          return;
        }

        const auto width = read_dynamic_param_i32(payload, WIRE_WORD_SIZE);
        const auto height = read_dynamic_param_i32(payload, WIRE_WORD_SIZE * 2);
        handle_resolution_change(session, width, height);
        return;
      }

      // 处理FPS变更（需要float值）
      if (param_type_enum == video::dynamic_param_type_e::FPS) {
        constexpr size_t FPS_PAYLOAD_SIZE = WIRE_WORD_SIZE * 2;
        if (payload.size() < FPS_PAYLOAD_SIZE) {
          BOOST_LOG(warning) << "Invalid payload size for FPS change. Expected " 
                             << FPS_PAYLOAD_SIZE << " bytes, got " << payload.size();
          return;
        }

        const float new_fps = read_dynamic_param_f32(payload, WIRE_WORD_SIZE);
        
        if (new_fps <= 0.0f || new_fps > 1000.0f) {
          BOOST_LOG(warning) << "Invalid FPS value: " << new_fps;
          return;
        }

        session->config.monitor.framerate = static_cast<int>(new_fps);
        perf::update_session_display(
          session->launch_session_id,
          session->config.monitor.width,
          session->config.monitor.height,
          session->config.monitor.framerate
        );
        
        video::dynamic_param_t param;
        param.type = video::dynamic_param_type_e::FPS;
        param.value.float_value = new_fps;
        param.valid = true;
        session->video.dynamic_param_change_events->raise(param);
        
        BOOST_LOG(info) << "Dynamic FPS change: " << new_fps << " fps";
        return;
      }

      // This is an HDR conversion parameter, not part of the display/VDD HDR
      // capability tuple. Apply it on the video thread without rebuilding the
      // display or encoder.
      if (param_type_enum == video::dynamic_param_type_e::CLIENT_SDR_WHITE_NITS) {
        constexpr size_t SDR_WHITE_PAYLOAD_SIZE = WIRE_WORD_SIZE * 2;
        if (payload.size() != SDR_WHITE_PAYLOAD_SIZE) {
          BOOST_LOG(warning) << "Invalid payload size for client SDR white. Expected "
                             << SDR_WHITE_PAYLOAD_SIZE << " bytes, got " << payload.size();
          return;
        }
        if (session->config.controlProtocolType != 13 || session->config.monitor.dynamicRange == 0) {
          BOOST_LOG(warning) << "Ignoring client SDR white update outside an encrypted HDR session";
          return;
        }

        const float sdr_white_nits = read_dynamic_param_f32(payload, WIRE_WORD_SIZE);
        if (!video::is_valid_client_sdr_white_nits(sdr_white_nits)) {
          BOOST_LOG(warning) << "Invalid client SDR white value: " << sdr_white_nits;
          return;
        }

        video::dynamic_param_t param {};
        param.type = video::dynamic_param_type_e::CLIENT_SDR_WHITE_NITS;
        param.value.float_value = sdr_white_nits;
        param.valid = true;
        session->dynamic_sdr_white_nits.store(sdr_white_nits, std::memory_order_release);
        session->video.dynamic_param_change_events->raise(param);

        BOOST_LOG(info) << "Dynamic client SDR white change: " << sdr_white_nits << " nits";
        return;
      }

      // 处理其他单值参数（码率、QP等，使用int值）
      constexpr size_t INT_PARAM_PAYLOAD_SIZE = WIRE_WORD_SIZE * 2;
      if (payload.size() < INT_PARAM_PAYLOAD_SIZE) {
        BOOST_LOG(warning) << "Invalid payload size for dynamic param change. Expected at least " 
                           << INT_PARAM_PAYLOAD_SIZE << " bytes, got " << payload.size();
        return;
      }

      const auto param_value = read_dynamic_param_i32(payload, WIRE_WORD_SIZE);

      video::dynamic_param_t param;
      param.type = param_type_enum;
      param.valid = true;

      // 参数验证和处理的辅助lambda
      auto validate_and_raise = [&](bool valid, auto value, const char *name, const char *unit = "") {
        if (valid) {
          if constexpr (std::is_same_v<decltype(value), bool>) {
            param.value.bool_value = value;
          } else {
            param.value.int_value = value;
          }
          session->video.dynamic_param_change_events->raise(param);
          BOOST_LOG(info) << "Dynamic " << name << " change: " << value << unit;
          return true;
        }
        BOOST_LOG(warning) << "Invalid " << name << " value: " << param_value;
        return false;
      };

      switch (param_type_enum) {
        case video::dynamic_param_type_e::BITRATE: {
          const auto valid_bitrate = param_value > 0 && param_value <= 800000;
          const auto capped_bitrate = valid_bitrate ? clamp_total_bitrate_to_host_cap(param_value, session->client_name) : param_value;
          if (validate_and_raise(valid_bitrate, capped_bitrate, "bitrate", " Kbps")) {
            session->current_total_bitrate = capped_bitrate;
          }
          break;
        }
        case video::dynamic_param_type_e::QP:
          validate_and_raise(param_value >= 0 && param_value <= 51, param_value, "QP");
          break;
        case video::dynamic_param_type_e::FEC_PERCENTAGE:
          validate_and_raise(param_value >= 0 && param_value <= 100, param_value, "FEC percentage", "%");
          break;
        case video::dynamic_param_type_e::ADAPTIVE_QUANTIZATION: {
          bool enabled = (param_value != 0);
          param.value.bool_value = enabled;
          session->video.dynamic_param_change_events->raise(param);
          BOOST_LOG(info) << "Dynamic adaptive quantization change: " << (enabled ? "enabled" : "disabled");
          break;
        }
        case video::dynamic_param_type_e::MULTI_PASS:
          validate_and_raise(param_value >= 0 && param_value <= 2, param_value, "multi-pass");
          break;
        case video::dynamic_param_type_e::VBV_BUFFER_SIZE:
          validate_and_raise(param_value > 0, param_value, "VBV buffer size", " Kbps");
          break;
        case video::dynamic_param_type_e::PRESET:
          param.value.int_value = param_value;
          session->video.dynamic_param_change_events->raise(param);
          BOOST_LOG(info) << "Dynamic preset change: " << param_value;
          break;
        default:
          BOOST_LOG(warning) << "Unsupported parameter type: " << param_type;
          break;
      }
    });

    server->map(packetTypes[IDX_INVALIDATE_REF_FRAMES], [&](session_t *session, const std::string_view &payload) {
      auto frames = (std::int64_t *) payload.data();
      auto firstFrame = frames[0];
      auto lastFrame = frames[1];

      BOOST_LOG(debug)
        << "type [IDX_INVALIDATE_REF_FRAMES]"sv << std::endl
        << "firstFrame [" << firstFrame << ']' << std::endl
        << "lastFrame [" << lastFrame << ']';

      session->video.invalidate_ref_frames_events->raise(std::make_pair(firstFrame, lastFrame));
    });

    server->map(packetTypes[IDX_CLIPBOARD], [](session_t *session, const std::string_view &payload) {
      // Forward opaque payload to the user-session GUI agent. Drop silently if
      // disabled by config or if the GUI is not currently subscribed.
      if (!config::input.clipboard_sync) {
        return;
      }
      auto &bridge = clipboard_bridge::bridge_t::instance();
      if (!bridge.gui_alive()) {
        return;
      }
      clipboard_bridge::payload_t bytes(payload.begin(), payload.end());
      bridge.on_inbound(session->launch_session_id, std::move(bytes));
    });

    server->map(packetTypes[IDX_CURSOR], [](session_t *session, const std::string_view &payload) {
      if (payload.size() != 4 ||
          static_cast<std::uint8_t>(payload[0]) != cursor_protocol_version) {
        BOOST_LOG(warning) << "Ignoring malformed local cursor mode request"sv;
        return;
      }

      const auto requested_mode = static_cast<std::uint8_t>(payload[1]);
      if (requested_mode > 1) {
        BOOST_LOG(warning) << "Ignoring unknown local cursor mode "sv
                           << static_cast<unsigned int>(requested_mode);
        return;
      }

      const bool enable = requested_mode == 1;
      if (enable && session->config.controlProtocolType != 13) {
        BOOST_LOG(warning) << "Ignoring local cursor mode on an unencrypted control stream"sv;
        return;
      }
      if (enable && !cursor_channel::producer_available()) {
        BOOST_LOG(warning) << "Ignoring local cursor mode without an active cursor producer"sv;
        return;
      }
      if (session->control.local_cursor_mode == enable) {
        return;
      }

      session->control.local_cursor_mode = enable;
      session->control.cursor_revision = 0;
      session->control.cursor_shape_sent = false;
      session->control.cursor_visibility_sent = false;
      session->control.cursor_failed_revision = 0;
      session->control.cursor_send_failures = 0;
      cursor_channel::set_session_enabled(session->launch_session_id, enable);
      BOOST_LOG(info) << "Local cursor mode "sv << (enable ? "enabled"sv : "disabled"sv)
                      << " for session "sv << session->launch_session_id;
    });

    server->map(packetTypes[IDX_INPUT_DATA], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_INPUT_DATA]"sv;

      auto tagged_cipher_length = util::endian::big(*(int32_t *) payload.data());
      std::string_view tagged_cipher { payload.data() + sizeof(tagged_cipher_length), (size_t) tagged_cipher_length };

      std::vector<uint8_t> plaintext;

      auto &cipher = session->control.cipher;
      auto &iv = session->control.legacy_input_enc_iv;
      if (cipher.decrypt(tagged_cipher, plaintext, &iv)) {
        // something went wrong :(

        BOOST_LOG(error) << "Failed to verify tag"sv;

        session::stop(*session, session::stop_reason_e::protocol_error);
        return;
      }

      if (tagged_cipher_length >= 16 + iv.size()) {
        std::copy(payload.end() - 16, payload.end(), std::begin(iv));
      }

      input::passthrough(session->input, std::move(plaintext));
    });

    server->map(packetTypes[IDX_ENCRYPTED], [server](session_t *session, const std::string_view &payload) {
      BOOST_LOG(verbose) << "type [IDX_ENCRYPTED]"sv;

      auto header = (control_encrypted_p) (payload.data() - 2);

      auto length = util::endian::little(header->length);
      auto seq = util::endian::little(header->seq);

      if (length < (16 + 4 + 4)) {
        BOOST_LOG(warning) << "Control: Runt packet"sv;
        return;
      }

      auto tagged_cipher_length = length - 4;
      std::string_view tagged_cipher { (char *) header->payload(), (size_t) tagged_cipher_length };

      auto &cipher = session->control.cipher;
      auto &iv = session->control.incoming_iv;
      if (session->config.encryptionFlagsEnabled & SS_ENC_CONTROL_V2) {
        // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
        // Section 8.2.1. The sequence number is our "invocation" field and the 'CC' in the
        // high bytes is the "fixed" field. Because each client provides their own unique
        // key, our values in the fixed field need only uniquely identify each independent
        // use of the client's key with AES-GCM in our code.
        //
        // The sequence number is 32 bits long which allows for 2^32 control stream messages
        // to be received from each client before the IV repeats.
        iv.resize(12);
        std::copy_n((uint8_t *) &seq, sizeof(seq), std::begin(iv));
        iv[10] = 'C';  // Client originated
        iv[11] = 'C';  // Control stream
      }
      else {
        // Nvidia's old style encryption uses a 16-byte IV
        iv.resize(16);

        iv[0] = (std::uint8_t) seq;
      }

      std::vector<uint8_t> plaintext;
      if (cipher.decrypt(tagged_cipher, plaintext, &iv)) {
        // something went wrong :(

        BOOST_LOG(error) << "Failed to verify tag"sv;

        session::stop(*session, session::stop_reason_e::protocol_error);
        return;
      }

      auto type = *(std::uint16_t *) plaintext.data();
      std::string_view next_payload { (char *) plaintext.data() + 4, plaintext.size() - 4 };

      if (type == packetTypes[IDX_ENCRYPTED]) {
        BOOST_LOG(error) << "Bad packet type [IDX_ENCRYPTED] found"sv;
        session::stop(*session, session::stop_reason_e::protocol_error);
        return;
      }

      // IDX_INPUT_DATA callback will attempt to decrypt unencrypted data, therefore we need pass it directly
      if (type == packetTypes[IDX_INPUT_DATA]) {
        plaintext.erase(std::begin(plaintext), std::begin(plaintext) + 4);
        input::passthrough(session->input, std::move(plaintext));
      }
      else {
        server->call(type, session, next_payload, true);
      }
    });

    // This thread handles latency-sensitive control messages
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    // Check for both the full shutdown event and the shutdown event for this
    // broadcast to ensure we can inform connected clients of our graceful
    // termination when we shut down.
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);
    auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    while (!shutdown_event->peek() && !broadcast_shutdown_event->peek()) {
      bool has_session_awaiting_peer = false;
      bool has_ds5_haptics_session = false;

      {
        auto lg = server->_sessions.lock();

        auto now = std::chrono::steady_clock::now();

        KITTY_WHILE_LOOP(auto pos = std::begin(*server->_sessions), pos != std::end(*server->_sessions), {
          // Don't perform additional session processing if we're shutting down
          if (shutdown_event->peek() || broadcast_shutdown_event->peek()) {
            break;
          }

          auto session = *pos;

          if (now > session->pingTimeout) {
            auto address = session->control.peer ? platf::from_sockaddr((sockaddr *) &session->control.peer->address.address) : session->control.expected_peer_address;
            BOOST_LOG(info) << address << ": Ping Timeout"sv;
            session::stop(*session, session::stop_reason_e::control_timeout);
          }

          if (session->lifecycle.state() == session::state_e::STOPPING) {
            clipboard_bridge::bridge_t::instance().session_stopped(session->launch_session_id);
            text_context::bridge_t::instance().session_stopped(session->launch_session_id);
            cursor_channel::remove_session(session->launch_session_id);
            pos = server->_sessions->erase(pos);

            if (session->control.peer) {
              {
                auto ptslg = server->_peer_to_session.lock();
                server->_peer_to_session->erase(session->control.peer);
              }

              enet_peer_disconnect_now(session->control.peer, 0);
            }

            session->controlEnd.raise(true);
            continue;
          }

          // Remember if we have a session that's waiting for a peer to connect to the
          // control stream. This ensures the clients are properly notified even when
          // the app terminates before they finish connecting.
          if (!session->control.peer) {
            has_session_awaiting_peer = true;
          }
          else {
            const auto ds5_settings = ds5_config::current();
            has_ds5_haptics_session |=
              ds5_settings.audio_haptics &&
              (session->config.mlFeatureFlags & (ML_FF_DS5_HAPTICS_PCM | ML_FF_DS5_HAPTICS_IR_V2)) != 0;
            auto &feedback_queue = session->control.feedback_queue;
            while (feedback_queue->peek()) {
              auto feedback_msg = feedback_queue->pop();

              send_feedback_msg(session, *feedback_msg);
            }

            for (auto &[controller_id, controller_haptics] : session->control.legacy_haptics) {
              if (const auto rumble = controller_haptics->poll(now)) {
                auto legacy_msg = platf::gamepad_feedback_msg_t::make_rumble(
                  controller_id, rumble->low_frequency, rumble->high_frequency);
                send_feedback_msg(session, legacy_msg, true);
              }
            }

            auto &hdr_queue = session->control.hdr_queue;
            while (session->control.peer && hdr_queue->peek()) {
              auto hdr_info = hdr_queue->pop();

              send_hdr_mode(session, std::move(hdr_info));
            }

            auto &resolution_change_queue = session->control.resolution_change_queue;
            while (session->control.peer && resolution_change_queue->peek()) {
              auto resolution = resolution_change_queue->pop();
              
              if (resolution) {
                send_resolution_change(session, resolution->first, resolution->second);
              }
            }

            if (session->control.local_cursor_mode) {
              cursor_channel::snapshot_t cursor;
              if (cursor_channel::copy_latest(session->control.cursor_revision, cursor)) {
                const bool include_shape =
                  cursor.has_shape &&
                  (!session->control.cursor_shape_sent ||
                   session->control.cursor_shape_id != cursor.shape_id);
                const bool visibility_changed =
                  !session->control.cursor_visibility_sent ||
                  session->control.cursor_visible != cursor.visible;
                const bool waiting_for_shape =
                  !cursor.has_shape &&
                  session->control.cursor_shape_sent &&
                  session->control.cursor_shape_id != cursor.shape_id;

                if ((include_shape || visibility_changed) &&
                    send_cursor_update(session, cursor, include_shape) == 0) {
                  session->control.cursor_revision = cursor.revision;
                  session->control.cursor_failed_revision = 0;
                  session->control.cursor_send_failures = 0;
                  session->control.cursor_visibility_sent = true;
                  session->control.cursor_visible = cursor.visible;
                  if (include_shape) {
                    session->control.cursor_shape_sent = true;
                    session->control.cursor_shape_id = cursor.shape_id;
                  }
                }
                else if (!include_shape && !visibility_changed) {
                  if (!waiting_for_shape) {
                    session->control.cursor_revision = cursor.revision;
                    session->control.cursor_failed_revision = 0;
                    session->control.cursor_send_failures = 0;
                  }
                }
                else {
                  if (session->control.cursor_failed_revision != cursor.revision) {
                    session->control.cursor_failed_revision = cursor.revision;
                    session->control.cursor_send_failures = 0;
                  }

                  if (++session->control.cursor_send_failures >= 3) {
                    BOOST_LOG(warning) << "Dropping local cursor revision "sv
                                       << cursor.revision
                                       << " after repeated send failures"sv;
                    session->control.cursor_revision = cursor.revision;
                    session->control.cursor_failed_revision = 0;
                    session->control.cursor_send_failures = 0;
                  }
                }
              }
            }
          }

          ++pos;
        })

        // Drain any clipboard messages enqueued by the GUI HTTP layer and
        // dispatch them to matching sessions. We're already holding the
        // _sessions lock here, which serialises with the per-session
        // erase/insert above.
        {
          std::deque<clipboard_bridge::outbound_msg_t> msgs;
          clipboard_bridge::bridge_t::instance().drain_outbound(msgs);
          for (auto &msg : msgs) {
            for (auto *s : *server->_sessions) {
              if (!s->control.peer) {
                continue;
              }
              if (msg.target != clipboard_bridge::kBroadcast
                  && s->launch_session_id != msg.target) {
                continue;
              }
              send_clipboard(s, msg.bytes);
            }
          }
        }

        {
          std::deque<text_context::outbound_msg_t> messages;
          text_context::bridge_t::instance().drain_outbound(messages);
          for (auto &message : messages) {
            bool delivered = false;
            for (auto *session : *server->_sessions) {
              if (!session->control.peer || session->launch_session_id != message.target ||
                  (session->config.mlFeatureFlags & ML_FF_REMOTE_TEXT_CONTEXT) == 0) {
                continue;
              }
              delivered = send_text_context(session, message.bytes) == 0;
              break;
            }
            BOOST_LOG(debug) << "Remote text context outbound: target=" << message.target
                            << ", delivered=" << delivered;
          }
        }
      }

      // Don't break until any pending sessions either expire or connect
      if (proc::proc.running() == 0 && !has_session_awaiting_peer) {
        BOOST_LOG(info) << "Process terminated"sv;
        break;
      }

      // Cursor shapes are low-frequency, but pointer role transitions should
      // still feel immediate. Poll the latest-value bridge once per frame while
      // local cursor mode is active without adding another control thread.
      // Text-context sends ride the same wakeups instead of raising the cadence
      // permanently: every observation is preceded by that client's touch/mouse
      // packets, and enet_host_service returns per incoming event, so the drain
      // runs on traffic; the idle timeout bounds delivery on a quiet socket.
      server->iterate(has_ds5_haptics_session ? 5ms :
                      cursor_channel::local_mode_active() ? 16ms : 150ms);
    }

    // Let all remaining connections know the server is shutting down
    // reason: graceful termination
    std::uint32_t reason = 0x80030023;

    control_terminate_t plaintext;
    plaintext.header.type = packetTypes[IDX_TERMINATION];
    plaintext.header.payloadLength = sizeof(plaintext.ec);
    plaintext.ec = util::endian::big<uint32_t>(reason);

    std::array<std::uint8_t,
      sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
      encrypted_payload;

    auto lg = server->_sessions.lock();
    for (auto pos = std::begin(*server->_sessions); pos != std::end(*server->_sessions); ++pos) {
      auto session = *pos;

      // The normal STOPPING path removes sessions from _sessions and updates
      // the clipboard bridge immediately. If the control thread exits via the
      // final shutdown path instead, make the same paired lifecycle update here
      // so capability/session_count cannot retain stale launch IDs.
      clipboard_bridge::bridge_t::instance().session_stopped(session->launch_session_id);
      text_context::bridge_t::instance().session_stopped(session->launch_session_id);
      cursor_channel::remove_session(session->launch_session_id);

      // We may not have gotten far enough to have an ENet connection yet
      if (session->control.peer) {
        auto payload = encode_control(session, util::view(plaintext), encrypted_payload);

        if (server->send(payload, session->control.peer)) {
          TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
          BOOST_LOG(warning) << "Couldn't send termination code to ["sv << addr << ':' << port << ']';
        }
      }

      session->shutdown_event->raise(true);
      session->controlEnd.raise(true);
    }

    server->flush();
  }

  namespace {
    bool
    is_terminal_udp_receive_error(const boost::system::error_code &ec) noexcept {
      return ec == boost::asio::error::operation_aborted ||
             ec == boost::asio::error::bad_descriptor ||
             ec == boost::system::errc::bad_file_descriptor ||
             ec == boost::system::errc::not_a_socket;
    }

    bool
    is_recoverable_udp_receive_error(const boost::system::error_code &ec) noexcept {
      return ec == boost::system::errc::connection_refused ||
             ec == boost::system::errc::connection_reset ||
             ec == boost::asio::error::message_size;
    }
  }  // namespace

  void
  micRecvThread(broadcast_ctx_t &ctx) {
    auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    auto &mic_io = ctx.mic_io_context;

    udp::endpoint peer;
    std::array<char, 2048> mic_recv_buffer;
    bool mic_device_initialized = false;
    std::unique_ptr<platf::deinit_t> audio_thread_guard;
    audio::audio_ctx_ref_t mic_audio_ref;
    mic_mixer::mixer_t mixer;
    std::unordered_set<mic_mixer::source_id_t> mixer_sources;

    struct MicStats {
      uint64_t total_packets = 0;
      uint64_t accepted_packets = 0;
      uint64_t decode_failed = 0;
      uint64_t invalid_data = 0;
      uint64_t unregistered = 0;
      uint64_t route_ambiguous = 0;
      uint64_t wasapi_backpressure_drops = 0;
    };
    MicStats stats;

    auto log_mic_stats = [&](std::string_view reason) {
      const auto mixer_stats = mixer.take_stats();
      const auto has_diagnostics = stats.total_packets != 0 ||
                                   stats.wasapi_backpressure_drops != 0 ||
                                   mixer_stats.duplicate_packets != 0 ||
                                   mixer_stats.late_packets != 0 ||
                                   mixer_stats.buffer_overflow_packets != 0 ||
                                   mixer_stats.timeline_reanchors != 0 ||
                                   mixer_stats.plc_frames != 0 ||
                                   mixer_stats.decode_failures != 0 ||
                                   mixer_stats.skipped_playout_frames != 0;
      if (!has_diagnostics) {
        stats = {};
        return;
      }

      BOOST_LOG(info) << "Microphone diagnostics ("sv << reason
                      << "): packets="sv << stats.total_packets
                      << ", accepted="sv << stats.accepted_packets
                      << ", route_decode_failed="sv << stats.decode_failed
                      << ", invalid="sv << stats.invalid_data
                      << ", unregistered="sv << stats.unregistered
                      << ", route_ambiguous="sv << stats.route_ambiguous
                      << ", duplicate="sv << mixer_stats.duplicate_packets
                      << ", late="sv << mixer_stats.late_packets
                      << ", queue_overflow="sv << mixer_stats.buffer_overflow_packets
                      << ", reanchors="sv << mixer_stats.timeline_reanchors
                      << ", plc="sv << mixer_stats.plc_frames
                      << ", opus_decode_failed="sv << mixer_stats.decode_failures
                      << ", skipped_slots="sv << mixer_stats.skipped_playout_frames
                      << ", wasapi_drops="sv << stats.wasapi_backpressure_drops;
      stats = {};
    };

    auto release_mic_device = [&]() {
      if (mic_device_initialized) {
        audio::release_mic_redirect_device();
        BOOST_LOG(debug) << "Microphone redirect session released"sv;
      }
      mic_device_initialized = false;
      mic_audio_ref = {};
      mixer.clear();
      mixer_sources.clear();
    };

    auto decode_candidate_payload = [](const boost::shared_ptr<broadcast_ctx_t::mic_session_ctx_t> &mic_session,
                                      const std::uint8_t *audio_data,
                                      std::size_t data_size,
                                      std::uint16_t sequence_number,
                                      std::vector<std::uint8_t> &decoded_payload) -> bool {
      decoded_payload.clear();
      if (!mic_session->cipher) {
        if (!mic_mixer::is_valid_opus_packet(audio_data, data_size)) {
          return false;
        }
        decoded_payload.assign(audio_data, audio_data + data_size);
        return true;
      }

      auto &cipher_ctx = *mic_session->cipher;
      // 麦克风 CBC IV 的前 4 字节为会话 key ID 与 16 位包序号之和，其余字节为 0。
      crypto::aes_t current_iv(16);
      const auto iv_sequence = mic_session->key_id + sequence_number;
      const auto iv_sequence_be = util::endian::big<std::uint32_t>(iv_sequence);
      std::memcpy(current_iv.data(), &iv_sequence_be, sizeof(iv_sequence_be));
      std::memset(current_iv.data() + sizeof(iv_sequence_be), 0, current_iv.size() - sizeof(iv_sequence_be));

      const std::string_view ciphertext {
        reinterpret_cast<const char *>(audio_data),
        data_size,
      };
      return cipher_ctx.cipher.decrypt(ciphertext, decoded_payload, &current_iv) == 0 &&
             mic_mixer::is_valid_opus_packet(decoded_payload.data(), decoded_payload.size());
    };

    auto process_audio_data = [&](const std::uint8_t *audio_data,
                                  std::size_t data_size,
                                  std::uint16_t sequence_number,
                                  std::optional<std::uint32_t> timestamp_ms,
                                  std::uint32_t source_key_id,
                                  const udp::endpoint &source_endpoint) {
      if (!ctx.mic_socket_enabled.load()) {
        return;
      }

      ++stats.total_packets;

      const auto source_address = net::normalize_address(source_endpoint.address());
      std::vector<boost::shared_ptr<broadcast_ctx_t::mic_session_ctx_t>> candidates;
      {
        boost::lock_guard<boost::mutex> lock(ctx.mic_session_mutex);
        for (const auto &[session_id, session] : ctx.mic_sessions) {
          (void) session_id;
          if (session->client_address == source_address) {
            candidates.emplace_back(session);
          }
        }
      }

      if (candidates.empty()) {
        ++stats.unregistered;
        return;
      }

      std::sort(candidates.begin(), candidates.end(), [](const auto &left, const auto &right) {
        return left->session_id < right->session_id;
      });

      // 现有客户端在线路上使用固定 SSRC，因此 endpoint 和密钥试解仍是兼容路径。
      // 若后续客户端把会话 key ID 放入 SSRC，则可在首次数据包上直接定位会话。
      std::vector<boost::shared_ptr<broadcast_ctx_t::mic_session_ctx_t>> ordered_candidates;
      ordered_candidates.reserve(candidates.size());
      auto append_candidates = [&](const auto &predicate) {
        for (const auto &candidate : candidates) {
          if (predicate(*candidate) &&
              std::find(ordered_candidates.begin(), ordered_candidates.end(), candidate) == ordered_candidates.end()) {
            ordered_candidates.emplace_back(candidate);
          }
        }
      };

      append_candidates([&](const auto &candidate) {
        return candidate.source_endpoint == source_endpoint;
      });
      if (source_key_id != MIC_PACKET_MAGIC) {
        append_candidates([&](const auto &candidate) {
          return candidate.key_id == source_key_id;
        });
      }
      if (candidates.size() == 1) {
        append_candidates([](const auto &) {
          return true;
        });
      }
      append_candidates([](const auto &candidate) {
        return !candidate.source_endpoint && candidate.cipher.has_value();
      });
      append_candidates([](const auto &candidate) {
        return !candidate.source_endpoint;
      });
      // 加密载荷可以在源端口变化后重新通过密钥定位。明文载荷没有足够身份信息，
      // 多会话下不能安全接管另一个 endpoint 的绑定。
      append_candidates([](const auto &candidate) {
        return candidate.cipher.has_value();
      });

      boost::shared_ptr<broadcast_ctx_t::mic_session_ctx_t> mic_session;
      std::vector<std::uint8_t> decoded_payload;
      for (const auto &candidate : ordered_candidates) {
        if (decode_candidate_payload(candidate, audio_data, data_size, sequence_number, decoded_payload)) {
          mic_session = candidate;
          break;
        }
      }

      if (!mic_session) {
        if (ordered_candidates.empty()) {
          ++stats.route_ambiguous;
        }
        else {
          ++stats.decode_failed;
        }
        return;
      }

      if (!mic_device_initialized) {
        return;
      }

      if (!mixer.add_source(mic_session->session_id)) {
        ++stats.invalid_data;
        return;
      }
      mixer_sources.insert(mic_session->session_id);
      if (!mixer.push_packet(
            mic_session->session_id,
            decoded_payload.data(),
            decoded_payload.size(),
            sequence_number,
            timestamp_ms)) {
        ++stats.invalid_data;
        return;
      }

      std::vector<std::uint32_t> displaced_sources;
      {
        boost::lock_guard<boost::mutex> lock(ctx.mic_session_mutex);
        const auto registered = ctx.mic_sessions.find(mic_session->session_id);
        if (registered == ctx.mic_sessions.end() || registered->second != mic_session) {
          ++stats.unregistered;
          return;
        }

        // 一个 UDP endpoint 在同一时刻只属于一个已验证音源。重新绑定时清理旧映射，
        // 避免同 IP 多会话在首次试解阶段留下重复 endpoint。
        for (auto &[session_id, registered_session] : ctx.mic_sessions) {
          (void) session_id;
          if (registered_session != mic_session && registered_session->source_endpoint == source_endpoint) {
            registered_session->source_endpoint.reset();
            displaced_sources.emplace_back(registered_session->session_id);
          }
        }
        mic_session->source_endpoint = source_endpoint;
      }

      for (const auto displaced_source : displaced_sources) {
        mixer.remove_source(displaced_source);
        mixer_sources.erase(displaced_source);
      }

      ++stats.accepted_packets;
    };

    auto retry_delay = 300ms;  // 初始重试延迟，连续错误指数退避到最大5秒
    boost::function<void(const boost::system::error_code, size_t)> mic_recv_func;
    boost::function<void()> schedule_mix;
    bool retry_receive_after_error = false;
    bool reset_mic_io_cycle = false;
    bool reopen_mic_socket_after_error = false;
    bool mix_timer_armed = false;
    std::uint64_t mix_timer_generation = 0;
    using mix_clock_t = asio::steady_timer::clock_type;
    std::optional<mix_clock_t::time_point> next_mix_deadline;
    asio::steady_timer mix_timer {mic_io};
    auto cancel_mix_timer = [&]() {
      mix_timer_armed = false;
      ++mix_timer_generation;
      next_mix_deadline.reset();
      try {
        mix_timer.cancel();
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Failed to cancel microphone mix timer: "sv << e.what();
      }
    };
    auto reset_playout_clock_if_pending = [&]() {
      if (ctx.mic_playout_reset_pending.load(boost::memory_order_acquire) &&
          ctx.mic_playout_reset_pending.exchange(false, boost::memory_order_acq_rel)) {
        cancel_mix_timer();
      }
    };
    auto schedule_receive = [&]() -> bool {
      boost::lock_guard<boost::mutex> lock(ctx.mic_socket_mutex);
      if (broadcast_shutdown_event->peek() ||
          !ctx.mic_socket_enabled.load() ||
          !ctx.mic_sock.is_open() ||
          !mic_device_initialized ||
          ctx.mic_session_count.load(boost::memory_order_acquire) == 0) {
        return false;
      }

      try {
        ctx.mic_sock.async_receive_from(asio::buffer(mic_recv_buffer), peer, 0, mic_recv_func);
        return true;
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Failed to restart microphone receive: "sv << e.what();
        return false;
      }
    };
    auto retry_with_reopened_mic_socket = [&]() {
      retry_receive_after_error = true;
      mixer.clear();
      mixer_sources.clear();
      reopen_mic_socket_after_error = true;
      cancel_mix_timer();
    };

    mic_recv_func = [&](const boost::system::error_code &ec, size_t received_bytes) {
      if (!ctx.mic_socket_enabled.load()) {
        return;
      }

      if (is_terminal_udp_receive_error(ec)) {
        BOOST_LOG(debug) << "Microphone receive stopped: "sv << ec.message();
        // 最后一个会话退出会取消挂起的接收。如果新会话在取消回调执行前完成注册，
        // 需要在同一个 io_context 周期内恢复接收和混音，不能把新会话留在空转状态。
        if (!broadcast_shutdown_event->peek() &&
            ctx.mic_socket_enabled.load() &&
            ctx.mic_session_count.load(boost::memory_order_acquire) != 0 &&
            mic_device_initialized) {
          if (schedule_receive()) {
            schedule_mix();
          }
          else {
            if (!broadcast_shutdown_event->peek() &&
                ctx.mic_socket_enabled.load() &&
                ctx.mic_session_count.load(boost::memory_order_acquire) != 0 &&
                mic_device_initialized) {
              retry_with_reopened_mic_socket();
            }
            else {
              reset_mic_io_cycle = true;
              cancel_mix_timer();
            }
          }
        }
        else {
          // 没有活动会话时同时终止旧混音链，让 io_context 回到外层重新判断状态。
          // cancel() 无法撤回已经排队的成功回调，因此还要用周期标记阻止旧回调自重挂。
          reset_mic_io_cycle = true;
          cancel_mix_timer();
        }
        return;
      }

      // 正常处理数据包或遇到可恢复错误后继续挂接接收。
      auto fg = util::fail_guard([&]() {
        if (schedule_receive()) {
          // 新会话可能在旧混音回调因会话数为零退出后接入。
          // 接收恢复时同时保证混音定时器仍在运行。
          schedule_mix();
        }
      });

      if (ec) {
        if (is_recoverable_udp_receive_error(ec)) {
          BOOST_LOG(debug) << "Mic socket recoverable receive error: "sv << ec.message();
        }
        else {
          BOOST_LOG(error) << "Mic socket error: "sv << ec.message();
          fg.disable();
          // 未分类 socket 错误会进入退避重试。旧播放 slot 已失去实时性，
          // 只清理 mixer 状态，不重建与网络错误无关的 WASAPI 设备。
          retry_with_reopened_mic_socket();
        }
        return;
      }

      retry_receive_after_error = false;
      retry_delay = 300ms;

      if (received_bytes < sizeof(RTP_PACKET)) {
        return;
      }

      // 尝试16位扩展包类型
      if (received_bytes >= sizeof(rtp_packet_ext_t)) {
        auto *header_ext = (rtp_packet_ext_t *) mic_recv_buffer.data();
        if (header_ext->packetType == packetTypes[IDX_MIC_DATA]) {
          size_t header_size = sizeof(rtp_packet_ext_t);
          if (received_bytes > header_size) {
            uint16_t sequence_number = util::endian::little(header_ext->sequenceNumber);
            const auto source_key_id = util::endian::little(header_ext->ssrc);
            process_audio_data(
              reinterpret_cast<const uint8_t *>(mic_recv_buffer.data()) + header_size,
              received_bytes - header_size,
              sequence_number,
              std::nullopt,
              source_key_id,
              peer);
          }
          return;
        }
      }

      // 8位包类型
      auto *header = (mic_packet_t *) mic_recv_buffer.data();
      if (header->rtp.packetType == MIC_PACKET_TYPE_OPUS) {
        size_t header_size = sizeof(mic_packet_t);
        if (received_bytes > header_size) {
          // 客户端按小端序发送序列号（MicrophoneStream.java 使用 LITTLE_ENDIAN）
          // 服务端必须按小端序读取，否则会读错（比如 1 会读成 256）
          uint16_t sequence_number = util::endian::little(header->rtp.sequenceNumber);
          const auto timestamp_ms = util::endian::little(header->rtp.timestamp);
          const auto source_key_id = util::endian::little(header->rtp.ssrc);
          size_t data_size = received_bytes - header_size;
          
          // BOOST_LOG(verbose) << "Received MIC packet: total=" << received_bytes 
          //                 << " bytes, header=" << header_size 
          //                 << " bytes, data=" << data_size 
          //                 << " bytes, sequenceNumber=" << sequence_number << " (little-endian)"
          //                 << " from " << client_ip;
          process_audio_data(
            reinterpret_cast<const uint8_t *>(mic_recv_buffer.data()) + header_size,
            data_size,
            sequence_number,
            timestamp_ms,
            source_key_id,
            peer);
        }
      }
    };

    schedule_mix = [&]() {
      reset_playout_clock_if_pending();
      if (ctx.mic_session_count.load(boost::memory_order_acquire) == 0) {
        return;
      }
      if (mix_timer_armed) {
        return;
      }

      mix_timer_armed = true;
      const auto generation = ++mix_timer_generation;
      if (!next_mix_deadline) {
        next_mix_deadline = mix_clock_t::now() + 20ms;
      }
      mix_timer.expires_at(*next_mix_deadline);
      mix_timer.async_wait([&, generation](const boost::system::error_code &ec) {
        if (generation != mix_timer_generation) {
          return;
        }
        reset_playout_clock_if_pending();
        if (generation != mix_timer_generation) {
          return;
        }
        mix_timer_armed = false;

        if (ec == boost::asio::error::operation_aborted ||
            broadcast_shutdown_event->peek() ||
            retry_receive_after_error ||
            reset_mic_io_cycle ||
            ctx.mic_session_count.load(boost::memory_order_acquire) == 0) {
          return;
        }

        const auto now = mix_clock_t::now();
        if (now >= *next_mix_deadline + 20ms) {
          const auto missed_frames = static_cast<std::size_t>((now - *next_mix_deadline) / 20ms);
          mixer.skip_playout_frames(missed_frames);
          *next_mix_deadline += 20ms * missed_frames;
        }

        std::unordered_set<mic_mixer::source_id_t> registered_sources;
        {
          boost::lock_guard<boost::mutex> lock(ctx.mic_session_mutex);
          for (const auto &[session_id, session] : ctx.mic_sessions) {
            (void) session;
            registered_sources.insert(session_id);
          }
        }

        for (auto source_it = mixer_sources.begin(); source_it != mixer_sources.end();) {
          if (!registered_sources.contains(*source_it)) {
            mixer.remove_source(*source_it);
            source_it = mixer_sources.erase(source_it);
          }
          else {
            ++source_it;
          }
        }

        if (auto mixed = mixer.mix_next_frame()) {
          const auto write_result = audio::write_mic_pcm(mixed->data(), mixed->size());
          if (write_result == 0) {
            ++stats.wasapi_backpressure_drops;
          }
          if (write_result < 0) {
            if (write_result == -2) {
              BOOST_LOG(info) << "Microphone output backend became unavailable; reinitializing"sv;
            }
            else {
              BOOST_LOG(warning) << "Microphone output write failed; reinitializing"sv;
            }
            release_mic_device();
            retry_receive_after_error = true;
            reset_mic_io_cycle = true;
            cancel_mix_timer();

            boost::lock_guard<boost::mutex> lock(ctx.mic_socket_mutex);
            cancel_mic_receive_locked(ctx);
            return;
          }
        }

        *next_mix_deadline += 20ms;
        schedule_mix();
      });
    };

    BOOST_LOG(debug) << "Starting microphone receive thread";

    bool had_active_mic_sessions = false;

    while (!broadcast_shutdown_event->peek()) {
      if (ctx.mic_session_count.load(boost::memory_order_acquire) == 0) {
        retry_delay = 300ms;  // 会话结束时重置延迟
        reset_playout_clock_if_pending();
        release_mic_device();
        if (had_active_mic_sessions) {
          log_mic_stats("sessions ended"sv);
          had_active_mic_sessions = false;
        }
        (void) broadcast_shutdown_event->view(100ms);
        continue;
      }
      had_active_mic_sessions = true;

      if (reopen_mic_socket_after_error || !ctx.mic_socket_enabled.load()) {
        bool has_active_sessions = false;
        bool socket_ready = false;
        {
          // 复用注册/注销路径的锁序，在会话锁内重新确认 shutdown 尚未清空会话。
          boost::lock_guard<boost::mutex> session_lock(ctx.mic_session_mutex);
          has_active_sessions = !ctx.mic_sessions.empty();
          if (has_active_sessions) {
            boost::lock_guard<boost::mutex> socket_lock(ctx.mic_socket_mutex);
            if (reopen_mic_socket_after_error) {
              close_mic_socket_locked(ctx);
            }
            socket_ready = open_mic_socket_locked(ctx);
          }
        }
        if (!has_active_sessions) {
          continue;
        }
        if (!socket_ready) {
          (void) broadcast_shutdown_event->view(retry_delay);
          retry_delay = std::min(retry_delay * 2, 5000ms);
          continue;
        }
        reopen_mic_socket_after_error = false;
      }

      // 在麦克风设备的完整生命周期内持有音频上下文，确保音频采集退出后仍能恢复设备。
      if (!mic_device_initialized) {
        if (!audio_thread_guard) {
          audio_thread_guard = platf::init_audio_thread();
          if (!audio_thread_guard) {
            (void) broadcast_shutdown_event->view(retry_delay);
            retry_delay = std::min(retry_delay * 2, 5000ms);
            continue;
          }
        }

        // 客户端麦克风可以在主机音频串流关闭时独立工作，因此活动的麦克风
        // 会话需要能够创建并持有自己的音频控制上下文。
        mic_audio_ref = audio::get_audio_ctx_ref();
        if (!mic_audio_ref) {
          (void) broadcast_shutdown_event->view(retry_delay);
          retry_delay = std::min(retry_delay * 2, 5000ms);
          continue;
        }

        if (audio::init_mic_redirect_device() != 0) {
          mic_audio_ref = {};
          (void) broadcast_shutdown_event->view(retry_delay);
          retry_delay = std::min(retry_delay * 2, 5000ms);  // 指数退避，最大5秒
          continue;
        }

        if (!ctx.mic_socket_enabled.load() ||
            ctx.mic_session_count.load(boost::memory_order_acquire) == 0) {
          audio::release_mic_redirect_device();
          mic_audio_ref = {};
          continue;
        }

        mic_device_initialized = true;
        retry_delay = 300ms;
      }

      if (mic_io.stopped()) {
        mic_io.restart();
      }
      retry_receive_after_error = false;
      reset_mic_io_cycle = false;
      if (!schedule_receive()) {
        if (ctx.mic_socket_enabled.load() && !broadcast_shutdown_event->peek()) {
          (void) broadcast_shutdown_event->view(100ms);
        }
        continue;
      }
      schedule_mix();

      mic_io.run();
      if (retry_receive_after_error &&
          ctx.mic_socket_enabled.load() &&
          !broadcast_shutdown_event->peek()) {
        (void) broadcast_shutdown_event->view(retry_delay);
        retry_delay = std::min(retry_delay * 2, 5000ms);
      }
    }

    release_mic_device();
    log_mic_stats("shutdown"sv);

    BOOST_LOG(debug) << "Microphone receive thread ended";
  }

  void
  recvThread(broadcast_ctx_t &ctx) {
    std::unordered_map<av_session_id_t, message_queue_t> peer_to_video_session;
    std::unordered_map<av_session_id_t, message_queue_t> peer_to_audio_session;

    auto &video_sock = ctx.video_sock;
    auto &audio_sock = ctx.audio_sock;
    auto &message_queue_queue = ctx.message_queue_queue;
    auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    auto &io = ctx.io_context;

    std::array<udp::endpoint, 2> peers;
    std::array<std::array<char, 2048>, 2> buffers;
    std::array<std::function<void(const boost::system::error_code, size_t)>, 2> recv_funcs;

    // 统一处理PING包逻辑
    auto handle_ping = [](auto &session_map, auto &peer, auto &buf, size_t bytes, std::string_view type_str) {
      try {
        if (bytes == 4) {
          if (auto it = session_map.find(peer.address()); it != std::end(session_map)) {
            BOOST_LOG(debug) << "RAISE: "sv << peer.address().to_string() << ':' << peer.port() << " :: " << type_str;
            it->second->raise(peer, std::string { buf.data(), bytes });
          }
        }
        else if (bytes >= sizeof(SS_PING)) {
          auto ping = (PSS_PING) buf.data();
          if (auto it = session_map.find(std::string { ping->payload, sizeof(ping->payload) }); it != std::end(session_map)) {
            BOOST_LOG(debug) << "RAISE: "sv << peer.address().to_string() << ':' << peer.port() << " :: " << type_str;
            it->second->raise(peer, std::string { buf.data(), bytes });
          }
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Error processing packet: " << e.what();
      }
    };

    // 更新会话映射
    auto update_session_map = [](auto &message_queue_queue, auto &video_map, auto &audio_map) {
      while (message_queue_queue->peek()) {
        if (auto message_queue_opt = message_queue_queue->pop()) {
          auto [socket_type, session_id, message_queue] = *message_queue_opt;
          auto &target_map = socket_type == socket_e::video ? video_map :
                                                              (socket_type == socket_e::audio ? audio_map : throw std::runtime_error("Unknown socket type"));

          if (message_queue) {
            target_map.emplace(session_id, message_queue);
          }
          else {
            target_map.erase(session_id);
          }
        }
      }
    };

    // 初始化接收函数
    auto init_recv_func = [&](auto &sock, size_t buf_idx, auto &session_map, std::string_view type_str) {
      recv_funcs[buf_idx] = [&, buf_idx, type_str](const boost::system::error_code &ec, size_t bytes) {
        auto &peer = peers[buf_idx];

        // 广播关闭时，已关闭的套接字不能再次挂接接收；其他终止错误说明共享接收链已经失效。
        if (is_terminal_udp_receive_error(ec)) {
          if (broadcast_shutdown_event->peek()) {
            BOOST_LOG(debug) << type_str << " socket closed: "sv << ec.message();
          }
          else {
            BOOST_LOG(error) << type_str << " socket closed unexpectedly: "sv << ec.message();
            broadcast_shutdown_event->raise(true);
          }
          return;
        }

        // 瞬态错误后继续挂接共享 UDP 接收。在 Windows 上，客户端断开产生的
        // ICMP Port Unreachable 可能让 async_receive_from() 返回 connection_reset。
        auto receive_again = util::fail_guard([&]() {
          if (broadcast_shutdown_event->peek()) {
            return;
          }

          if (!sock.is_open()) {
            if (!broadcast_shutdown_event->peek()) {
              BOOST_LOG(error) << type_str << " socket closed before receive could be restarted"sv;
              broadcast_shutdown_event->raise(true);
            }
            return;
          }

          try {
            sock.async_receive_from(asio::buffer(buffers[buf_idx]), peer, 0, recv_funcs[buf_idx]);
          }
          catch (const std::exception &e) {
            if (!broadcast_shutdown_event->peek()) {
              BOOST_LOG(error) << "Failed to restart async receive for "sv << type_str << ": "sv << e.what();
              broadcast_shutdown_event->raise(true);
            }
          }
        });

        if (ec) {
          if (is_recoverable_udp_receive_error(ec)) {
            BOOST_LOG(debug) << type_str << " socket recoverable receive error: "sv << ec.message();
          }
          else {
            BOOST_LOG(error) << type_str << " receive error: "sv << ec.message();
            receive_again.disable();
            // 未分类错误不能继续复用半失效的共享接收链，关闭当前广播后由下一次会话重建。
            broadcast_shutdown_event->raise(true);
          }
          return;
        }

        BOOST_LOG(verbose) << "Recv: "sv << peer.address().to_string() << ':' << peer.port() << " :: " << type_str;

        update_session_map(message_queue_queue, peer_to_video_session, peer_to_audio_session);
        if (bytes == 0) {
          BOOST_LOG(warning) << "Received empty packet";
          // 即使是空包，也继续接收
        }
        else {
          handle_ping(session_map, peer, buffers[buf_idx], bytes, type_str);
        }
      };
    };

    try {
      init_recv_func(video_sock, 0, peer_to_video_session, "VIDEO");
      init_recv_func(audio_sock, 1, peer_to_audio_session, "AUDIO");

      video_sock.async_receive_from(asio::buffer(buffers[0]), peers[0], 0, recv_funcs[0]);
      audio_sock.async_receive_from(asio::buffer(buffers[1]), peers[1], 0, recv_funcs[1]);

      io.run();
      if (!broadcast_shutdown_event->peek()) {
        BOOST_LOG(error) << "Shared UDP receive loop stopped unexpectedly"sv;
        broadcast_shutdown_event->raise(true);
      }
    }
    catch (const std::exception &e) {
      BOOST_LOG(fatal) << "recvThread exception: " << e.what();
      broadcast_shutdown_event->raise(true);
    }
  }

  void
  videoBroadcastThread(udp::socket &sock) {
    auto shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
    auto video_epoch = std::chrono::steady_clock::now();

    // Video traffic is sent on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    logging::min_max_avg_periodic_logger<double> frame_processing_latency_logger(debug, "Frame processing latency", "ms");

    logging::time_delta_periodic_logger frame_send_batch_latency_logger(debug, "Network: each send_batch() latency");
    logging::time_delta_periodic_logger frame_fec_latency_logger(debug, "Network: each FEC block latency");
    logging::time_delta_periodic_logger frame_network_latency_logger(debug, "Network: frame's overall network latency");

    crypto::aes_t iv(12);

    auto timer = platf::create_high_precision_timer();
    if (!timer || !*timer) {
      BOOST_LOG(error) << "Failed to create timer, aborting video broadcast thread";
      return;
    }

    auto ratecontrol_next_frame_start = std::chrono::steady_clock::now();

    while (auto packet = packets->pop()) {
      if (shutdown_event->peek()) {
        break;
      }

      frame_network_latency_logger.first_point_now();

      auto session = (session_t *) packet->channel_data;
      session->last_video_activity_ms.store(steady_now_ms(), std::memory_order_relaxed);
      auto lowseq = session->video.lowseq;

      std::string_view payload { (char *) packet->data(), packet->data_size() };
      std::vector<uint8_t> payload_with_replacements;

      // Apply replacements on the packet payload before performing any other operations.
      // We need to know the final frame size to calculate the last packet size, and we
      // must avoid matching replacements against the frame header or any other non-video
      // part of the payload.
      if (packet->is_idr() && packet->replacements) {
        for (auto &replacement : *packet->replacements) {
          auto frame_old = replacement.old;
          auto frame_new = replacement._new;

          payload_with_replacements = replace(payload, frame_old, frame_new);
          payload = { (char *) payload_with_replacements.data(), payload_with_replacements.size() };
        }
      }

      video_short_frame_header_t frame_header = {};
      frame_header.headerType = 0x01;  // Short header type
      frame_header.frameType = packet->is_idr()                     ? 2 :
                               packet->after_ref_frame_invalidation ? 5 :
                                                                      1;
      frame_header.lastPayloadLen = (payload.size() + sizeof(frame_header)) % (session->config.packetsize - sizeof(NV_VIDEO_PACKET));
      if (frame_header.lastPayloadLen == 0) {
        frame_header.lastPayloadLen = session->config.packetsize - sizeof(NV_VIDEO_PACKET);
      }

      const auto frame_dequeue_time = std::chrono::steady_clock::now();
      auto processing_start = packet->frame_timestamp;
      if (packet->pipeline_trace && packet->pipeline_trace->capture_ready) {
        // Presentation time belongs to the RTP clock. Host processing starts
        // when the captured frame is ready for conversion and encoding.
        processing_start = packet->pipeline_trace->capture_ready;
      }

      if (processing_start) {
        auto duration_to_latency = [](const std::chrono::steady_clock::duration &duration) {
          const auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
          return (uint16_t) std::clamp<decltype(duration_us)>((duration_us + 50) / 100, 0, std::numeric_limits<uint16_t>::max());
        };

        uint16_t latency = duration_to_latency(frame_dequeue_time - *processing_start);
        frame_header.frame_processing_latency = latency;
        frame_processing_latency_logger.collect_and_log(latency / 10.);
        perf::record_host_latency(session->launch_session_id, latency / 10., frame_dequeue_time);
      }
      else {
        frame_header.frame_processing_latency = 0;
      }

      if (packet->pipeline_trace) {
        const auto &trace = *packet->pipeline_trace;
        const auto elapsed_ms = [](const auto &begin, const auto &end) -> std::optional<double> {
          if (!begin || !end) {
            return std::nullopt;
          }

          return std::chrono::duration<double, std::milli>(*end - *begin).count();
        };

        perf::pipeline_sample_t sample;
        sample.capture_to_convert_ms = elapsed_ms(trace.capture_ready, trace.convert_begin);
        sample.convert_ms = elapsed_ms(trace.convert_begin, trace.convert_end);
        sample.encode_queue_ms = elapsed_ms(trace.convert_end, trace.encode_submit);
        sample.encode_ms = elapsed_ms(trace.encode_submit, trace.packet_ready);
        sample.packet_to_broadcast_ms = elapsed_ms(trace.packet_ready, std::optional { frame_dequeue_time });
        sample.total_ms = elapsed_ms(trace.capture_ready, std::optional { frame_dequeue_time });
        perf::record_pipeline_sample(session->launch_session_id, sample, frame_dequeue_time);
      }

      auto fecPercentage = config::stream.fec_percentage;

      // Insert space for packet headers
      auto blocksize = session->config.packetsize + MAX_RTP_HEADER_SIZE;
      auto payload_blocksize = blocksize - sizeof(video_packet_raw_t);
      auto payload_new = concat_and_insert(sizeof(video_packet_raw_t), payload_blocksize,
        std::string_view { (char *) &frame_header, sizeof(frame_header) }, payload);

      payload = std::string_view { (char *) payload_new.data(), payload_new.size() };

      // There are 2 bits for FEC block count for a maximum of 4 FEC blocks
      constexpr auto MAX_FEC_BLOCKS = 4;

      // The max number of data shards per block is found by solving this system of equations for D:
      // D = 255 - P
      // P = D * F
      // which results in the solution:
      // D = 255 / (1 + F)
      // multiplied by 100 since F is the percentage as an integer:
      // D = (255 * 100) / (100 + F)
      auto max_data_shards_per_fec_block = (DATA_SHARDS_MAX * 100) / (100 + fecPercentage);

      // Compute the number of FEC blocks needed for this frame using the block size and max shards
      auto max_data_per_fec_block = max_data_shards_per_fec_block * blocksize;
      auto fec_blocks_needed = (payload.size() + (max_data_per_fec_block - 1)) / max_data_per_fec_block;

      // If the number of FEC blocks needed exceeds the protocol limit, turn off FEC for this frame.
      // For normal FEC percentages, this should only happen for enormous frames (over 800 packets at 20%).
      if (fec_blocks_needed > MAX_FEC_BLOCKS) {
        BOOST_LOG(warning) << "Skipping FEC for abnormally large encoded frame (needed "sv << fec_blocks_needed << " FEC blocks)"sv;
        fecPercentage = 0;
        fec_blocks_needed = MAX_FEC_BLOCKS;
      }

      std::array<std::string_view, MAX_FEC_BLOCKS> fec_blocks;
      decltype(fec_blocks)::iterator
        fec_blocks_begin = std::begin(fec_blocks),
        fec_blocks_end = std::begin(fec_blocks) + fec_blocks_needed;

      BOOST_LOG(verbose) << "Generating "sv << fec_blocks_needed << " FEC blocks"sv;

      // Align individual FEC blocks to blocksize
      auto unaligned_size = payload.size() / fec_blocks_needed;
      auto aligned_size = ((unaligned_size + (blocksize - 1)) / blocksize) * blocksize;

      // If we exceed the 10-bit FEC packet index (which means our frame exceeded 4096 packets),
      // the frame will be unrecoverable. Log an error for this case.
      if (aligned_size / blocksize >= 1024) {
        BOOST_LOG(error) << "Encoder produced a frame too large to send! Is the encoder broken? (needed "sv << (aligned_size / blocksize) << " packets)"sv;
      }

      // Split the data into aligned FEC blocks
      for (int x = 0; x < fec_blocks_needed; ++x) {
        if (x == fec_blocks_needed - 1) {
          // The last block must extend to the end of the payload
          fec_blocks[x] = payload.substr(x * aligned_size);
        }
        else {
          // Earlier blocks just extend to the next block offset
          fec_blocks[x] = payload.substr(x * aligned_size, aligned_size);
        }
      }

      try {
        // Use around 80% of 1Gbps          1Gbps            percent    ms     packet      byte
        size_t ratecontrol_packets_in_1ms = std::giga::num * 80 / 100 / 1000 / blocksize / 8;

        // Send less than 64K in a single batch.
        // On Windows, batches above 64K seem to bypass SO_SNDBUF regardless of its size,
        // appear in "Other I/O" and begin waiting for interrupts.
        // This gives inconsistent performance so we'd rather avoid it.
        size_t send_batch_size = 64 * 1024 / blocksize;
        // Also don't exceed 64 packets, which can happen when Moonlight requests
        // unusually small packet size.
        // Generic Segmentation Offload on Linux can't do more than 64.
        send_batch_size = std::min<size_t>(64, send_batch_size);

        // Don't ignore the last ratecontrol group of the previous frame
        auto ratecontrol_frame_start = std::max(ratecontrol_next_frame_start, std::chrono::steady_clock::now());

        size_t ratecontrol_frame_packets_sent = 0;
        size_t ratecontrol_group_packets_sent = 0;

        // RTP video timestamps use the 90 kHz media clock and the original
        // capture presentation timestamp. Frames without a capture timestamp
        // (intentional duplicates) retain the next scheduled pacing time.
        const bool frame_is_dupe = !packet->frame_timestamp;
        const auto presentation_time = packet->frame_timestamp.value_or(ratecontrol_next_frame_start);
        const auto timestamp = video_rtp_timestamp(presentation_time, video_epoch);

        auto blockIndex = 0;
        std::for_each(fec_blocks_begin, fec_blocks_end, [&](std::string_view &current_payload) {
          auto packets = (current_payload.size() + (blocksize - 1)) / blocksize;

          for (int x = 0; x < packets; ++x) {
            auto *inspect = (video_packet_raw_t *) &current_payload[x * blocksize];

            inspect->packet.frameIndex = packet->frame_index();
            inspect->packet.streamPacketIndex = ((uint32_t) lowseq + x) << 8;

            // Match multiFecFlags with Moonlight
            inspect->packet.multiFecFlags = 0x10;
            inspect->packet.multiFecBlocks = (blockIndex << 4) | ((fec_blocks_needed - 1) << 6);

            inspect->packet.flags = FLAG_CONTAINS_PIC_DATA;
            if (x == 0) {
              inspect->packet.flags |= FLAG_SOF;
            }
            if (x == packets - 1) {
              inspect->packet.flags |= FLAG_EOF;
            }
          }

          frame_fec_latency_logger.first_point_now();
          // If video encryption is enabled, we allocate space for the encryption header before each shard
          auto shards = fec::encode(current_payload, blocksize, fecPercentage, session->config.minRequiredFecPackets,
            session->video.cipher ? sizeof(video_packet_enc_prefix_t) : 0);
          frame_fec_latency_logger.second_point_now_and_log();

          auto peer_address = session->video.peer.address();
          auto batch_info = platf::batched_send_info_t {
            shards.headers.begin(),
            shards.prefixsize,
            shards.payload_buffers,
            shards.blocksize,
            0,
            0,
            (uintptr_t) sock.native_handle(),
            peer_address,
            session->video.peer.port(),
            session->localAddress,
          };

          size_t next_shard_to_send = 0;

          // set FEC info now that we know for sure what our percentage will be for this frame
          for (auto x = 0; x < shards.size(); ++x) {
            auto *inspect = (video_packet_raw_t *) shards.data(x);

            inspect->packet.fecInfo =
              (x << 12 |
                shards.data_shards << 22 |
                shards.percentage << 4);

            inspect->rtp.header = 0x80 | FLAG_EXTENSION;
            inspect->rtp.sequenceNumber = util::endian::big<uint16_t>(lowseq + x);
            inspect->rtp.timestamp = util::endian::big<uint32_t>(timestamp);

            inspect->packet.multiFecBlocks = (blockIndex << 4) | ((fec_blocks_needed - 1) << 6);
            inspect->packet.frameIndex = packet->frame_index();

            // Encrypt this shard if video encryption is enabled
            if (session->video.cipher) {
              // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
              // Section 8.2.1. The sequence number is our "invocation" field and the 'V' in the
              // high bytes is the "fixed" field. Because each client provides their own unique
              // key, our values in the fixed field need only uniquely identify each independent
              // use of the client's key with AES-GCM in our code.
              //
              // The IV counter is 64 bits long which allows for 2^64 encrypted video packets
              // to be sent to each client before the IV repeats.
              std::copy_n((uint8_t *) &session->video.gcm_iv_counter, sizeof(session->video.gcm_iv_counter), std::begin(iv));
              iv[11] = 'V';  // Video stream
              session->video.gcm_iv_counter++;

              // Encrypt the target buffer in place
              auto *prefix = (video_packet_enc_prefix_t *) shards.prefix(x);
              prefix->frameNumber = packet->frame_index();
              std::copy(std::begin(iv), std::end(iv), prefix->iv);
              session->video.cipher->encrypt(std::string_view { (char *) inspect, (size_t) blocksize },
                prefix->tag, (uint8_t *) inspect, &iv);
            }

            if (x - next_shard_to_send + 1 >= send_batch_size ||
                x + 1 == shards.size()) {
              // Do pacing within the frame.
              // Also trigger pacing before the first send_batch() of the frame
              // to account for the last send_batch() of the previous frame.
              if (ratecontrol_group_packets_sent >= ratecontrol_packets_in_1ms ||
                  ratecontrol_frame_packets_sent == 0) {
                auto due = ratecontrol_frame_start +
                           std::chrono::duration_cast<std::chrono::nanoseconds>(1ms) *
                             ratecontrol_frame_packets_sent / ratecontrol_packets_in_1ms;

                auto now = std::chrono::steady_clock::now();
                if (now < due) {
                  timer->sleep_for(due - now);
                }

                ratecontrol_group_packets_sent = 0;
              }

              size_t current_batch_size = x - next_shard_to_send + 1;
              batch_info.block_offset = next_shard_to_send;
              batch_info.block_count = current_batch_size;

              frame_send_batch_latency_logger.first_point_now();
              // Use a batched send if it's supported on this platform
              if (!platf::send_batch(batch_info)) {
                // Batched send is not available, so send each packet individually
                BOOST_LOG(verbose) << "Falling back to unbatched send"sv;
                for (auto y = 0; y < current_batch_size; y++) {
                  auto send_info = platf::send_info_t {
                    shards.prefix(next_shard_to_send + y),
                    shards.prefixsize,
                    shards.data(next_shard_to_send + y),
                    shards.blocksize,
                    (uintptr_t) sock.native_handle(),
                    peer_address,
                    session->video.peer.port(),
                    session->localAddress,
                  };

                  platf::send(send_info);
                }
              }
              frame_send_batch_latency_logger.second_point_now_and_log();

              ratecontrol_group_packets_sent += current_batch_size;
              ratecontrol_frame_packets_sent += current_batch_size;
              next_shard_to_send = x + 1;
            }
          }

          // remember this in case the next frame comes immediately
          ratecontrol_next_frame_start = ratecontrol_frame_start +
                                         std::chrono::duration_cast<std::chrono::nanoseconds>(1ms) *
                                           ratecontrol_frame_packets_sent / ratecontrol_packets_in_1ms;

          frame_network_latency_logger.second_point_now_and_log();

          BOOST_LOG(verbose) << "Sent Frame seq ["sv << packet->frame_index() << "] pts ["sv << timestamp
                             << "] shards ["sv << shards.size() << "/"sv << shards.percentage << "%]"sv
                             << (frame_is_dupe ? " Dupe" : "")
                             << (packet->is_idr() ? " Key" : "")
                             << (packet->after_ref_frame_invalidation ? " RFI" : "");

          ++blockIndex;
          lowseq += shards.size();
        });

        session->video.lowseq = lowseq;
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Broadcast video failed "sv << e.what();
        std::this_thread::sleep_for(100ms);
      }
    }

    shutdown_event->raise(true);
  }

  void
  audioBroadcastThread(udp::socket &sock) {
    auto shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    auto packets = mail::man->queue<audio::packet_t>(mail::audio_packets);

    audio_packet_t audio_packet;
    fec::rs_t rs { reed_solomon_new(RTPA_DATA_SHARDS, RTPA_FEC_SHARDS) };
    crypto::aes_t iv(16);

    // For unknown reasons, the RS parity matrix computed by our RS implementation
    // doesn't match the one Nvidia uses for audio data. I'm not exactly sure why,
    // but we can simply replace it with the matrix generated by OpenFEC which
    // works correctly. This is possible because the data and FEC shard count is
    // constant and known in advance.
    const unsigned char parity[] = { 0x77, 0x40, 0x38, 0x0e, 0xc7, 0xa7, 0x0d, 0x6c };
    memcpy(rs.get()->p, parity, sizeof(parity));

    audio_packet.rtp.header = 0x80;
    audio_packet.rtp.packetType = 97;
    audio_packet.rtp.ssrc = 0;

    // Audio traffic is sent on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    while (auto packet = packets->pop()) {
      if (shutdown_event->peek()) {
        break;
      }

      TUPLE_2D_REF(channel_data, packet_data, *packet);
      auto session = (session_t *) channel_data;
      session->last_audio_activity_ms.store(steady_now_ms(), std::memory_order_relaxed);

      auto sequenceNumber = session->audio.sequenceNumber;
      auto timestamp = session->audio.timestamp;

      *(std::uint32_t *) iv.data() = util::endian::big<std::uint32_t>(session->audio.avRiKeyId + sequenceNumber);

      auto &shards_p = session->audio.shards_p;

      // 检查客户端是否启用了音频加密
      bool audio_encryption_enabled = (session->config.encryptionFlagsEnabled & SS_ENC_AUDIO) != 0;
      if (sequenceNumber == 0) {
        // 只在第一个包时记录一次，避免日志过多
        BOOST_LOG(info) << "Audio encryption status: encryptionFlagsEnabled=0x" 
                        << std::hex << session->config.encryptionFlagsEnabled << std::dec
                        << ", SS_ENC_AUDIO (0x04) check: " << (audio_encryption_enabled ? "enabled" : "disabled")
                        << ", will " << (audio_encryption_enabled ? "ENCRYPT" : "NOT encrypt") << " audio data";
      }

      size_t plaintext_size = packet_data.size();
      
      // 验证 cipher 是否已初始化
      if (sequenceNumber == 0) {
        bool cipher_initialized = (session->audio.cipher.key.size() > 0);
        BOOST_LOG(info) << "Audio cipher status: initialized=" << (cipher_initialized ? "yes" : "no")
                        << ", key_size=" << session->audio.cipher.key.size();
      }
      
      auto bytes = encode_audio(audio_encryption_enabled, packet_data,
        shards_p[sequenceNumber % RTPA_DATA_SHARDS], iv, session->audio.cipher);
      
      if (sequenceNumber == 0) {
        // 验证加密是否真的执行了
        if (audio_encryption_enabled) {
          // 加密后的大小应该大于等于明文（因为 PKCS5 填充）
          bool encryption_applied = (bytes >= plaintext_size && bytes % 16 == 0);
          BOOST_LOG(info) << "Audio packet encryption: plaintext_size=" << plaintext_size 
                          << ", encrypted_size=" << bytes
                          << ", encryption " << (encryption_applied ? "SUCCESS (data is encrypted)" : "FAILED (data may not be encrypted)");
        } else {
          BOOST_LOG(info) << "Audio packet: plaintext_size=" << plaintext_size 
                          << ", output_size=" << bytes
                          << " (NOT encrypted, sent as plaintext)";
        }
      }
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio packet"sv;
        break;
      }

      BOOST_LOG(verbose) << "Audio [seq "sv << sequenceNumber << ", pts "sv << timestamp << "] ::  send..."sv;

      audio_packet.rtp.sequenceNumber = util::endian::big(sequenceNumber);
      audio_packet.rtp.timestamp = util::endian::big(timestamp);

      session->audio.sequenceNumber++;
      session->audio.timestamp += session->config.audio.packetDuration;

      auto peer_address = session->audio.peer.address();
      try {
        auto send_info = platf::send_info_t {
          (const char *) &audio_packet,
          sizeof(audio_packet),
          (const char *) shards_p[sequenceNumber % RTPA_DATA_SHARDS],
          (size_t) bytes,
          (uintptr_t) sock.native_handle(),
          peer_address,
          session->audio.peer.port(),
          session->localAddress,
        };
        platf::send(send_info);

        auto &fec_packet = session->audio.fec_packet;
        // initialize the FEC header at the beginning of the FEC block
        if (sequenceNumber % RTPA_DATA_SHARDS == 0) {
          fec_packet.fecHeader.baseSequenceNumber = util::endian::big(sequenceNumber);
          fec_packet.fecHeader.baseTimestamp = util::endian::big(timestamp);
        }

        // generate parity shards at the end of the FEC block
        if ((sequenceNumber + 1) % RTPA_DATA_SHARDS == 0) {
          reed_solomon_encode(rs.get(), shards_p.begin(), RTPA_TOTAL_SHARDS, bytes);

          for (auto x = 0; x < RTPA_FEC_SHARDS; ++x) {
            fec_packet.rtp.sequenceNumber = util::endian::big<std::uint16_t>(sequenceNumber + x + 1);
            fec_packet.fecHeader.fecShardIndex = x;

            auto send_info = platf::send_info_t {
              (const char *) &fec_packet,
              sizeof(fec_packet),
              (const char *) shards_p[RTPA_DATA_SHARDS + x],
              (size_t) bytes,
              (uintptr_t) sock.native_handle(),
              peer_address,
              session->audio.peer.port(),
              session->localAddress,
            };
            platf::send(send_info);
            BOOST_LOG(verbose) << "Audio FEC ["sv << (sequenceNumber & ~(RTPA_DATA_SHARDS - 1)) << ' ' << x << "] ::  send..."sv;
          }
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Broadcast audio failed "sv << e.what();
        std::this_thread::sleep_for(100ms);
      }
    }

    shutdown_event->raise(true);
  }

  int
  start_broadcast(broadcast_ctx_t &ctx) {
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);
    auto protocol = address_family == net::IPV4 ? udp::v4() : udp::v6();
    auto control_port = net::map_port(CONTROL_PORT);
    auto video_port = net::map_port(VIDEO_STREAM_PORT);
    auto audio_port = net::map_port(AUDIO_STREAM_PORT);

    if (ctx.control_server.bind(address_family, control_port)) {
      BOOST_LOG(error) << "Couldn't bind Control server to port ["sv << control_port << "], likely another process already bound to the port"sv;

      return -1;
    }

    boost::system::error_code ec;
    ctx.video_sock.open(protocol, ec);
    if (ec) {
      BOOST_LOG(fatal) << "Couldn't open socket for Video server: "sv << ec.message();

      return -1;
    }

    // Set video socket send buffer size (SO_SENDBUF) to 1MB
    try {
      ctx.video_sock.set_option(boost::asio::socket_base::send_buffer_size(1024 * 1024));
    }
    catch (...) {
      BOOST_LOG(error) << "Failed to set video socket send buffer size (SO_SENDBUF)";
    }

    auto bind_addr_str = net::get_bind_address(address_family);
    const auto bind_addr = boost::asio::ip::make_address(bind_addr_str, ec);
    if (ec) {
      BOOST_LOG(fatal) << "Invalid bind address: "sv << bind_addr_str << " - " << ec.message();
      return -1;
    }

    ctx.video_sock.bind(udp::endpoint(bind_addr, video_port), ec);
    if (ec) {
      BOOST_LOG(fatal) << "Couldn't bind Video server to port ["sv << video_port << "]: "sv << ec.message();

      return -1;
    }

    ctx.audio_sock.open(protocol, ec);
    if (ec) {
      BOOST_LOG(fatal) << "Couldn't open socket for Audio server: "sv << ec.message();

      return -1;
    }

    ctx.audio_sock.bind(udp::endpoint(bind_addr, audio_port), ec);
    if (ec) {
      BOOST_LOG(fatal) << "Couldn't bind Audio server to port ["sv << audio_port << "]: "sv << ec.message();

      return -1;
    }

    ctx.message_queue_queue = std::make_shared<message_queue_queue_t::element_type>(30);

    ctx.video_thread = std::thread { videoBroadcastThread, std::ref(ctx.video_sock) };
    ctx.audio_thread = std::thread { audioBroadcastThread, std::ref(ctx.audio_sock) };
    ctx.control_thread = std::thread { controlBroadcastThread, &ctx.control_server };

    ctx.recv_thread = std::thread { recvThread, std::ref(ctx) };
    ctx.mic_thread = std::thread { micRecvThread, std::ref(ctx) };

    return 0;
  }

  void
  end_broadcast(broadcast_ctx_t &ctx) {
    auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);

    broadcast_shutdown_event->raise(true);

    auto video_packets = mail::man->queue<video::packet_t>(mail::video_packets);
    auto audio_packets = mail::man->queue<audio::packet_t>(mail::audio_packets);

    // Minimize delay stopping video/audio threads
    video_packets->stop();
    audio_packets->stop();

    ctx.message_queue_queue->stop();
    ctx.io_context.stop();
    ctx.mic_io_context.stop();

    ctx.video_sock.close();
    ctx.audio_sock.close();

    if (disable_mic_socket(ctx)) {
      BOOST_LOG(debug) << "Microphone socket closed and encryption context securely cleared";
    }

    video_packets.reset();
    audio_packets.reset();

    BOOST_LOG(debug) << "Waiting for main listening thread to end..."sv;
    ctx.recv_thread.join();
    BOOST_LOG(debug) << "Waiting for main video thread to end..."sv;
    ctx.video_thread.join();
    BOOST_LOG(debug) << "Waiting for main audio thread to end..."sv;
    ctx.audio_thread.join();
    BOOST_LOG(debug) << "Waiting for main control thread to end..."sv;
    ctx.control_thread.join();
    BOOST_LOG(debug) << "Waiting for microphone thread to end..."sv;
    ctx.mic_thread.join();
    BOOST_LOG(debug) << "All broadcasting threads ended"sv;

    broadcast_shutdown_event->reset();
  }

  int
  recv_ping(session_t *session, decltype(broadcast_shared)::ptr_t ref, socket_e type, std::string_view expected_payload, udp::endpoint &peer, std::chrono::milliseconds timeout) {
    auto messages = std::make_shared<message_queue_t::element_type>(30);
    av_session_id_t session_id = std::string { expected_payload };

    // Only allow matches on the peer address for legacy clients
    if (!(session->config.mlFeatureFlags & ML_FF_SESSION_ID_V1)) {
      ref->message_queue_queue->raise(type, peer.address(), messages);
    }
    ref->message_queue_queue->raise(type, session_id, messages);

    auto fg = util::fail_guard([&]() {
      messages->stop();

      // remove message queue from session
      if (!(session->config.mlFeatureFlags & ML_FF_SESSION_ID_V1)) {
        ref->message_queue_queue->raise(type, peer.address(), nullptr);
      }
      ref->message_queue_queue->raise(type, session_id, nullptr);
    });

    auto start_time = std::chrono::steady_clock::now();
    auto current_time = start_time;

    while (current_time - start_time < config::stream.ping_timeout) {
      auto delta_time = current_time - start_time;

      auto msg_opt = messages->pop(config::stream.ping_timeout - delta_time);
      if (!msg_opt) {
        break;
      }

      TUPLE_2D_REF(recv_peer, msg, *msg_opt);
      if (msg.find(expected_payload) != std::string::npos) {
        // Match the new PING payload format
        BOOST_LOG(debug) << "Received ping [v2] from "sv << recv_peer.address() << ':' << recv_peer.port() << " ["sv << util::hex_vec(msg) << ']';
      }
      else if (!(session->config.mlFeatureFlags & ML_FF_SESSION_ID_V1) && msg == "PING"sv) {
        // Match the legacy fixed PING payload only if the new type is not supported
        BOOST_LOG(debug) << "Received ping [v1] from "sv << recv_peer.address() << ':' << recv_peer.port() << " ["sv << util::hex_vec(msg) << ']';
      }
      else {
        BOOST_LOG(debug) << "Received non-ping from "sv << recv_peer.address() << ':' << recv_peer.port() << " ["sv << util::hex_vec(msg) << ']';
        current_time = std::chrono::steady_clock::now();
        continue;
      }

      // Update connection details.
      peer = recv_peer;
      return 0;
    }

    BOOST_LOG(error) << "Initial Ping Timeout"sv;
    return -1;
  }

  void
  videoThread(session_t *session) {
    auto global_shutdown_event = mail::man->event<bool>(mail::shutdown);
    auto fg = util::fail_guard([&]() {
      const auto reason = global_shutdown_event->peek() ? session::stop_reason_e::host_terminate : session::stop_reason_e::video_ended;
      session::stop(*session, reason);
    });

    while_starting_do_nothing(session->lifecycle);

    auto ref = broadcast_shared.ref();
    auto error = recv_ping(session, ref, socket_e::video, session->video.ping_payload, session->video.peer, config::stream.ping_timeout);
    if (error < 0) {
      return;
    }

    // Enable local prioritization and QoS tagging on video traffic if requested by the client
    auto address = session->video.peer.address();
    session->video.qos = platf::enable_socket_qos(ref->video_sock.native_handle(), address,
      session->video.peer.port(), platf::qos_data_type_e::video, session->config.videoQosType != 0);

    BOOST_LOG(debug) << "Start capturing Video"sv;
    // Debug: Log the display_name before calling video::capture
    BOOST_LOG(debug) << "stream.cpp: session->config.monitor.display_name = [" << (session->config.monitor.display_name.empty() ? "<empty>" : session->config.monitor.display_name) << "]";
    video::capture(session->mail, session->config.monitor, session, session->video.dynamic_param_change_events);
  }

  void
  audioThread(session_t *session) {
    auto global_shutdown_event = mail::man->event<bool>(mail::shutdown);
    auto fg = util::fail_guard([&]() {
      const auto reason = global_shutdown_event->peek() ? session::stop_reason_e::host_terminate : session::stop_reason_e::audio_ended;
      session::stop(*session, reason);
    });

    while_starting_do_nothing(session->lifecycle);

    auto ref = broadcast_shared.ref();
    auto error = recv_ping(session, ref, socket_e::audio, session->audio.ping_payload, session->audio.peer, config::stream.ping_timeout);
    if (error < 0) {
      return;
    }

    // Enable local prioritization and QoS tagging on audio traffic if requested by the client
    auto address = session->audio.peer.address();
    session->audio.qos = platf::enable_socket_qos(ref->audio_sock.native_handle(), address,
      session->audio.peer.port(), platf::qos_data_type_e::audio, session->config.audioQosType != 0);

    BOOST_LOG(debug) << "Start capturing Audio"sv;
    audio::capture(session->mail, session->config.audio, session);
  }

  namespace session {
    const char *
    stop_reason_name(stop_reason_e reason) {
      switch (reason) {
        case stop_reason_e::none:
          return "none";
        case stop_reason_e::control_disconnect:
          return "control_disconnect";
        case stop_reason_e::control_timeout:
          return "control_timeout";
        case stop_reason_e::protocol_error:
          return "protocol_error";
        case stop_reason_e::video_ended:
          return "video_ended";
        case stop_reason_e::audio_ended:
          return "audio_ended";
        case stop_reason_e::client_cancel:
          return "client_cancel";
        case stop_reason_e::host_terminate:
          return "host_terminate";
      }
      return "unknown";
    }
    state_e
    state(session_t &session) {
      return session.lifecycle.state();
    }

    bool
    has_active_video_sessions() {
      return video_session_count() > 0;
    }

    void
    stop(session_t &session, stop_reason_e reason) {
      while_starting_do_nothing(session.lifecycle);
      if (!session.lifecycle.request_stop(reason)) {
        return;
      }

      BOOST_LOG(info) << "Stopping streaming session "sv << session.launch_session_id
                      << " [client_uuid="sv << session.client_cert_uuid
                      << ", reason="sv << stop_reason_name(reason) << ']';

      perf::end_session(session.launch_session_id);
      session.shutdown_event->raise(true);
    }

    void
    join(session_t &session) {
      // Current Nvidia drivers have a bug where NVENC can deadlock the encoder thread with hardware-accelerated
      // GPU scheduling enabled. If this happens, we will terminate ourselves and the service can restart.
      // The alternative is that Sunshine can never start another session until it's manually restarted.
      auto task = []() {
        BOOST_LOG(fatal) << "Hang detected! Session failed to terminate in 10 seconds."sv;
        logging::log_flush();
        lifetime::debug_trap();
      };
      auto force_kill = task_pool.pushDelayed(task, 10s).task_id;
      auto fg = util::fail_guard([&force_kill]() {
        // Cancel the kill task if we manage to return from this function
        task_pool.cancel(force_kill);
      });

      // 仅控制流会话没有视频/音频线程
      if (!session.control_only) {
        BOOST_LOG(debug) << "Waiting for video to end..."sv;
        session.videoThread.join();
        BOOST_LOG(debug) << "Waiting for audio to end..."sv;
        session.audioThread.join();
      }
      else {
        BOOST_LOG(debug) << "Control-only session: skipping video/audio thread join"sv;
      }
      BOOST_LOG(debug) << "Waiting for control to end..."sv;
      session.controlEnd.view();
      // Reset input on session stop to avoid stuck repeated keys
      BOOST_LOG(debug) << "Resetting Input..."sv;
      input::reset(session.input);
      tray_state::remove_session(session.launch_session_id);

      // 对于仅控制流会话，只减少总会话计数，不调用 streaming_will_stop
      // 只有当所有非控制流会话都结束时才调用 streaming_will_stop
      if (session.control_only) {
        --running_sessions;
        BOOST_LOG(debug) << "Control-only session ended (remaining sessions: "sv << running_sessions.load() << ")"sv;
      }
      else {
        // 非仅控制流会话：减少两个计数器
        --running_sessions;

        if (session.audio.mic_registered) {
          const auto was_registered = release_mic_session(*session.broadcast_ref.get(), session.launch_session_id);
          session.audio.mic_registered = false;
          BOOST_LOG(debug) << "Microphone registration for session " << session.launch_session_id
                           << (was_registered ? " released"sv : " was already absent"sv);
        }

        // If this is the last non-control-only session, invoke the platform callbacks
        if (unregister_video_session() == 0) {
          bool restore_display_state { true };
          if (proc::proc.running()) {
            tray_state::set_paused(proc::proc.get_last_run_app_name());
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
            system_tray::update_tray_pausing(proc::proc.get_last_run_app_name());
#endif

            // TODO: make this configurable per app
            restore_display_state = false;
          }
          else {
            tray_state::set_idle(proc::proc.get_last_run_app_name());
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
            system_tray::update_tray_stopped(proc::proc.get_last_run_app_name());
#endif
          }

          if (restore_display_state) {
            display_device::session_t::get().restore_state();
          }

#ifdef _WIN32
          // Restore the touch-keyboard registry transaction started at
          // stream start (see the touch_kb mount in nvhttp_stream_start).
          touch_kb::end_session();
#endif

          platf::streaming_will_stop();
        }
      }

      perf::end_session(session.launch_session_id);

      // Clean up ABR state for this client
      abr::cleanup(session.client_name);

      std::string client_ip = session.control.expected_peer_address;
      if (session.control.peer) {
        try {
          client_ip = platf::from_sockaddr(
            (sockaddr *) &session.control.peer->address.address
          );
        }
        catch (...) {
          // Preserve the handshake address when the live peer cannot be
          // converted during teardown.
        }
      }

      const auto lifecycle = session.lifecycle.snapshot();
      try {
        webhook::send_event_async(webhook::event_t {
          .type = webhook::event_type_t::NV_SESSION_END,
          .timestamp = webhook::get_current_timestamp(),
          .client_name = session.client_name,
          .client_ip = std::move(client_ip),
          .app_name = session.app_name,
          .app_id = session.app_id,
          .session_id = std::to_string(session.launch_session_id),
          .extra_data = {{"reason", stop_reason_name(lifecycle.stop_reason)}}
        });
      }
      catch (...) {
        BOOST_LOG(error) << "Webhook session end event construction failed"sv;
      }

      BOOST_LOG(debug) << "Session ended [session_id="sv << session.launch_session_id
                       << ", client_uuid="sv << session.client_cert_uuid
                       << ", reason="sv << stop_reason_name(lifecycle.stop_reason) << ']';
    }

    int
    start(session_t &session, const std::string &addr_string) {
      session.input = input::alloc(session.mail, session.launch_session_id);

      session.broadcast_ref = broadcast_shared.ref();
      if (!session.broadcast_ref) {
        return -1;
      }

      bool first_video_session {false};
      bool video_session_registered {false};
      if (!session.control_only) {
        const auto registration = register_video_session();
        if (!registration) {
          return -1;
        }

        first_video_session = *registration;
        video_session_registered = true;
      }
      // 登记后的启动失败由 guard 回退计数；正常进入 RUNNING 后转交 join() 注销。
      auto video_session_registration_guard = util::fail_guard([&]() {
        if (video_session_registered) {
          unregister_video_session();
        }
      });

      session.control.expected_peer_address = addr_string;
      auto addr = boost::asio::ip::make_address(addr_string);
      session.video.peer.address(addr);
      session.video.peer.port(0);
      session.audio.peer.address(addr);
      session.audio.peer.port(0);

      // 会话加入 _sessions 后，控制线程可能立即读取它。因此必须先初始化
      // 控制线程依赖的所有字段，否则默认构造的超时时间会被误判为已经过期。
      session.pingTimeout = std::chrono::steady_clock::now() + config::stream.ping_timeout;
      session.lifecycle.set_state(state_e::STARTING);
      auto starting_guard = util::fail_guard([&]() {
        session.lifecycle.set_state(state_e::RUNNING);
      });
      if (session.control_only) {
        BOOST_LOG(info) << "Starting control-only session from ["sv << addr_string << "] - will only handle input control"sv;
      }
      else {
        BOOST_LOG(debug) << "Expecting incoming session connections from "sv << addr_string;
      }

      // Register with the text-context bridge before publishing the session:
      // once the pointer is in the shared list the control thread may dispatch
      // input immediately, and an unregistered launch ID would silently drop
      // that first click/touch candidate.
      text_context::bridge_t::instance().session_started(session.launch_session_id);

      // 将完成初始化的会话加入共享列表。
      {
        auto lg = session.broadcast_ref->control_server._sessions.lock();
        session.broadcast_ref->control_server._sessions->push_back(&session);
      }
      clipboard_bridge::bridge_t::instance().session_started(session.launch_session_id);

      // 仅控制流会话不启动视频/音频线程
      if (!session.control_only) {
        session.audioThread = std::thread { audioThread, &session };
        session.videoThread = std::thread { videoThread, &session };
      }
      else {
        BOOST_LOG(debug) << "Control-only session: skipping video and audio thread creation"sv;
      }

      // 在生命周期仍为 STARTING 时完成麦克风注册。session::stop() 会等待该边界，
      // 避免 join() 在套接字引用和路由信息配对前释放会话。
      if (!session.control_only) {
        if (session.audio.enable_mic) {
          setup_mic_for_session(session, addr);
        }
      }

      ++running_sessions;
      video_session_registration_guard.disable();

      perf::begin_session({
        session.launch_session_id,
        session.client_name,
        session.config.monitor.width,
        session.config.monitor.height,
        session.config.monitor.framerate,
        session.current_total_bitrate.load(std::memory_order_relaxed),
        ::config::video.encoder.empty() ? "auto" : ::config::video.encoder,
        ::config::video.capture.empty() ? "auto" : ::config::video.capture,
        session.control_only,
      });
      tray_state::add_session(
        session.launch_session_id,
        session.client_name,
        session.highly_suspected_unknown_client
      );

      // 仅控制流会话不触发 streaming_will_start 回调，因为它们不传输视频/音频
      // 但它们仍然需要被计入 running_sessions，以便正确管理会话
      if (session.control_only) {
        BOOST_LOG(debug) << "Control-only session started (total sessions: "sv << running_sessions.load() << ")"sv;
      }
      else {
        // If this is the first non-control-only session, invoke the platform callbacks
        if (first_video_session) {
          platf::streaming_will_start();
          tray_state::set_streaming(proc::proc.get_last_run_app_name());
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
          system_tray::update_tray_playing(proc::proc.get_last_run_app_name());
#endif
        }
      }

      try {
        std::map<std::string, std::string> extra_data {
          {"resolution", std::to_string(session.config.monitor.width) + "x" + std::to_string(session.config.monitor.height)},
          {"fps", std::to_string(session.config.monitor.framerate)}
        };
        if (session.highly_suspected_unknown_client) {
          extra_data.emplace(
            "client_integrity_warning",
            client_fingerprint::suspicious_client_code
          );
        }
        webhook::send_event_async(webhook::event_t {
          .type = webhook::event_type_t::NV_SESSION_START,
          .timestamp = webhook::get_current_timestamp(),
          .client_name = session.client_name,
          .client_ip = addr_string,
          .app_name = session.app_name,
          .app_id = session.app_id,
          .session_id = std::to_string(session.launch_session_id),
          .extra_data = std::move(extra_data)
        });
      }
      catch (...) {
        BOOST_LOG(error) << "Webhook session start event construction failed"sv;
      }

      // 仅在 join() 依赖的状态全部提交后发布 RUNNING，
      // 会话线程和停止请求都会等待该边界。
      session.lifecycle.set_state(state_e::RUNNING);
      starting_guard.disable();

      return 0;
    }

    std::shared_ptr<session_t>
    alloc(config_t &config, rtsp_stream::launch_session_t &launch_session) {
      auto session = std::make_shared<session_t>();

      auto mail = std::make_shared<safe::mail_raw_t>();

      session->shutdown_event = mail->event<bool>(mail::shutdown);
      session->launch_session_id = launch_session.id;
      session->created_at_ms = steady_now_ms();

      // 设置客户端名称
      session->client_name = launch_session.client_name;
      session->client_cert_uuid = launch_session.client_cert_uuid;
      session->use_vdd = launch_session.use_vdd;
      session->custom_screen_mode = launch_session.custom_screen_mode;
      session->highly_suspected_unknown_client = launch_session.highly_suspected_unknown_client;
      session->app_id = launch_session.appid;
      session->app_name = proc::proc.get_app_name(launch_session.appid);

      // 保存 launch_session 的关键字段，用于后续动态参数更新
      session->enable_sops = launch_session.enable_sops;
      session->enable_hdr = launch_session.enable_hdr;
      session->synthetic_hdr = launch_session.synthetic_hdr;
      session->frame_pipeline_policy = launch_session.frame_pipeline_policy;
      session->frame_pipeline_policy_resolved = launch_session.frame_pipeline_policy_resolved;
      session->hdr_capabilities = launch_session.hdr_capabilities;
      session->reported_hdr_capabilities = launch_session.reported_hdr_capabilities;
      session->dynamic_sdr_white_nits.store(
        launch_session.hdr_capabilities.sdr_white_nits,
        std::memory_order_release);
      session->hdr_target_source = launch_session.hdr_target_source;

      session->config = config;

      // Initialize current total bitrate (including FEC) from config
      // config.monitor.bitrate is the encoding bitrate (excluding FEC)
      // We need to convert it to total bitrate (including FEC)
      int encoding_bitrate = config.monitor.bitrate;
      int fec_percentage = config::stream.fec_percentage;
      if (fec_percentage > 0 && fec_percentage <= 80) {
        // Convert encoding bitrate to total bitrate: total = encoding * 100 / (100 - fec_percentage)
        session->current_total_bitrate = (int) (encoding_bitrate * 100.f / (100 - fec_percentage));
      }
      else {
        // If FEC percentage is 0 or > 80%, encoding bitrate equals total bitrate
        session->current_total_bitrate = encoding_bitrate;
      }

      session->control.connect_data = launch_session.control_connect_data;
      session->control.feedback_queue = mail->queue<platf::gamepad_feedback_msg_t>(mail::gamepad_feedback);
      session->control.hdr_queue = mail->event<video::hdr_info_t>(mail::hdr);
      session->control.resolution_change_queue = mail->event<std::pair<std::uint32_t, std::uint32_t>>(mail::resolution_change);
      session->control.legacy_input_enc_iv = launch_session.iv;
      session->control.cipher = crypto::cipher::gcm_t {
        launch_session.gcm_key, false
      };

      session->video.idr_events = mail->event<bool>(mail::idr);
      session->video.invalidate_ref_frames_events = mail->event<std::pair<int64_t, int64_t>>(mail::invalidate_ref_frames);
      session->video.dynamic_param_change_events = mail->event<video::dynamic_param_t>(mail::dynamic_param_change);
      session->video.lowseq = 0;
      session->video.ping_payload = launch_session.av_ping_payload;
      if (config.encryptionFlagsEnabled & SS_ENC_VIDEO) {
        BOOST_LOG(info) << "Video encryption enabled"sv;
        session->video.cipher = crypto::cipher::gcm_t {
          launch_session.gcm_key, false
        };
        session->video.gcm_iv_counter = 0;
      }

      // Per-shard capacity must match MAX_AUDIO_PACKET_SIZE (defined near
      // the top of this file). The previous hardcoded 2048 here would
      // overflow with AC3 frames (~2560 B encrypted) and corrupt neighbour
      // shards / RS parity for E-AC3 (~1552 B), causing client-side decode
      // failures even when the encoder reported success.
      constexpr auto max_block_size = crypto::cipher::round_to_pkcs7_padded(MAX_AUDIO_PACKET_SIZE);

      util::buffer_t<char> shards { RTPA_TOTAL_SHARDS * max_block_size };
      util::buffer_t<uint8_t *> shards_p { RTPA_TOTAL_SHARDS };

      for (auto x = 0; x < RTPA_TOTAL_SHARDS; ++x) {
        shards_p[x] = (uint8_t *) &shards[x * max_block_size];
      }

      // Audio FEC spans multiple audio packets,
      // therefore its session specific
      session->audio.shards = std::move(shards);
      session->audio.shards_p = std::move(shards_p);

      session->audio.fec_packet.rtp.header = 0x80;
      session->audio.fec_packet.rtp.packetType = 127;
      session->audio.fec_packet.rtp.timestamp = 0;
      session->audio.fec_packet.rtp.ssrc = 0;

      session->audio.fec_packet.fecHeader.payloadType = 97;
      session->audio.fec_packet.fecHeader.ssrc = 0;

      session->audio.cipher = crypto::cipher::cbc_t {
        launch_session.gcm_key, true
      };

      session->audio.ping_payload = launch_session.av_ping_payload;
      std::uint32_t av_ri_key_id_be;
      std::memcpy(&av_ri_key_id_be, launch_session.iv.data(), sizeof(av_ri_key_id_be));
      session->audio.avRiKeyId = util::endian::big(av_ri_key_id_be);
      session->audio.sequenceNumber = 0;
      session->audio.timestamp = 0;

      session->audio.enable_mic = launch_session.enable_mic;

      session->control_only = launch_session.control_only;

      session->control.peer = nullptr;

      session->mail = std::move(mail);

      return session;
    }

    bool
    change_dynamic_param_for_client(const std::string &client_name, const video::dynamic_param_t &param) {
      auto effective_param = param;

      // 先检查是否有活动的广播引用，避免在无活跃session时
      // 触发start_broadcast/end_broadcast循环（"僵尸广播"），
      // 这可能阻塞HTTPS服务器线程导致客户端显示主机离线
      if (!broadcast_shared.has_ref()) {
        return false;
      }

      auto broadcast_ref = broadcast_shared.ref();
      if (!broadcast_ref) {
        BOOST_LOG(warning) << "No broadcast context available when changing dynamic parameter for client";
        return false;
      }

      auto lg = broadcast_ref->control_server._sessions.lock();
      for (auto session_p : *broadcast_ref->control_server._sessions) {
        if (session_p->client_name == client_name &&
            session_p->lifecycle.state() == state_e::RUNNING) {
          // Update session's current total bitrate if this is a bitrate change
          if (effective_param.type == video::dynamic_param_type_e::BITRATE && effective_param.valid) {
            effective_param.value.int_value = clamp_total_bitrate_to_host_cap(effective_param.value.int_value, client_name);
            // The param.value.int_value is the total bitrate (user-configured, including FEC)
            session_p->current_total_bitrate = effective_param.value.int_value;
            perf::update_session_bitrate(session_p->launch_session_id, effective_param.value.int_value);
            BOOST_LOG(info) << "Updated session total bitrate for client '" << client_name
                            << "': " << effective_param.value.int_value << " Kbps (including FEC)";
          }

          session_p->video.dynamic_param_change_events->raise(effective_param);
          BOOST_LOG(info) << "Sent dynamic parameter change event to client '" << client_name
                          << "': type=" << (int) effective_param.type;
          return true;
        }
      }

      BOOST_LOG(warning) << "No active session found for client: " << client_name;
      return false;
    }

    std::vector<session_info_t>
    get_all_sessions_info() {
      std::vector<session_info_t> sessions_info;

      // 关键修复：先检查是否有活动的引用，避免触发 start_broadcast
      // 如果没有活动的引用，说明没有活动的 session，直接返回空列表
      if (!broadcast_shared.has_ref()) {
        return sessions_info;
      }

      auto broadcast_ref = broadcast_shared.ref();
      if (!broadcast_ref) {
        BOOST_LOG(warning) << "No broadcast context when getting all sessions info";
        return sessions_info;
      }

      // 在持有锁的情况下，快速复制会话的基本信息
      // 由于存储的是原始指针，我们需要在持有锁时快速访问
      auto lg = broadcast_ref->control_server._sessions.lock();
      for (auto session_p : *broadcast_ref->control_server._sessions) {
        // 双重检查：确保会话指针仍然有效
        if (!session_p) {
          continue;
        }

        try {
          session_info_t info;

          info.client_name = session_p->client_name;
          info.client_uuid = session_p->client_cert_uuid;
          info.session_id = session_p->launch_session_id;

          const auto lifecycle = session_p->lifecycle.snapshot();
          info.stop_reason = stop_reason_name(lifecycle.stop_reason);

          const auto now_ms = steady_now_ms();
          info.uptime_ms = std::max<std::int64_t>(0, now_ms - session_p->created_at_ms);
          info.control_idle_ms = idle_ms(now_ms, session_p->last_control_activity_ms.load(std::memory_order_relaxed));
          info.video_idle_ms = idle_ms(now_ms, session_p->last_video_activity_ms.load(std::memory_order_relaxed));
          info.audio_idle_ms = idle_ms(now_ms, session_p->last_audio_activity_ms.load(std::memory_order_relaxed));
          info.control_connected = lifecycle.state == state_e::RUNNING && session_p->control.peer != nullptr;

          // Get client address
          if (session_p->control.peer) {
            try {
              info.client_address = platf::from_sockaddr((sockaddr *) &session_p->control.peer->address.address);
            }
            catch (...) {
              info.client_address = session_p->control.expected_peer_address;
            }
          }
          else {
            info.client_address = session_p->control.expected_peer_address;
          }

          // Get session state
          switch (lifecycle.state) {
            case state_e::STOPPED:
              info.state = "STOPPED";
              break;
            case state_e::STOPPING:
              info.state = "STOPPING";
              break;
            case state_e::STARTING:
              info.state = "STARTING";
              break;
            case state_e::RUNNING:
              info.state = "RUNNING";
              break;
            default:
              info.state = "UNKNOWN";
              break;
          }

          // Get video configuration
          info.width = session_p->config.monitor.width;
          info.height = session_p->config.monitor.height;
          info.fps = session_p->config.monitor.framerate;

          // Get current total bitrate (including FEC) from session-specific field
          // This is the user-configured bitrate, which may have been changed dynamically
          info.bitrate = session_p->current_total_bitrate.load(std::memory_order_relaxed);

          // Get audio and other settings
          info.host_audio = session_p->config.audio.flags[audio::config_t::HOST_AUDIO];
          info.enable_hdr = session_p->config.monitor.dynamicRange > 0;
          info.enable_mic = session_p->audio.enable_mic;
          info.use_vdd = session_p->use_vdd;
          info.hdr_brightness_reported = session_p->reported_hdr_capabilities.reported;
          info.hdr_brightness_source = hdr::to_string(session_p->hdr_target_source);
          info.hdr_max_nits = session_p->hdr_capabilities.max_nits;
          info.hdr_min_nits = session_p->hdr_capabilities.min_nits;
          info.hdr_max_full_frame_nits = session_p->hdr_capabilities.max_full_frame_nits;

          // Get app information
          try {
            info.app_id = proc::proc.running();
            info.app_name = (info.app_id > 0) ? proc::proc.get_last_run_app_name() : "None";
          }
          catch (...) {
            info.app_id = 0;
            info.app_name = "None";
          }

          sessions_info.push_back(info);
        }
        catch (const std::exception &e) {
          BOOST_LOG(warning) << "Error processing session: " << e.what() << " when getting all sessions info";
          continue;
        }
        catch (...) {
          BOOST_LOG(warning) << "Unknown error processing session when getting all sessions info";
          continue;
        }
      }

      return sessions_info;
    }
  }  // namespace session
}  // namespace stream
