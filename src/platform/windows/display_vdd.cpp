/**
 * @file src/platform/windows/display_vdd.cpp
 * @brief VDD direct-capture backend.
 *
 * Opens the named D3D11 shared texture exported by the ZakoVDD driver
 * (SharedFrameExporter, see Virtual-Display-Driver/Virtual Display Driver (HDR)/
 * ZakoVDD/Driver.cpp). Bypasses DXGI Desktop Duplication / WGC entirely so it
 * works:
 *   - in SYSTEM service context (SunshineService)
 *   - before any user logs on
 *   - across user-switch / lock / RDP transitions
 *   - with full HDR (R10G10B10A2 / RGBA16F)
 * Limitation: only captures VDD virtual monitors, not physical displays.
 */

#include "display.h"
#include "misc.h"
#include "src/config.h"
#include "src/display_device/vdd_ioctl.h"
#include "src/main.h"
#include "vdd_frame_channel.h"

#include <d3d11_1.h>
#include <sddl.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace platf {
  using namespace std::literals;
}

namespace platf::dxgi {

  using SharedFrameMetadata = vdd_frame_channel::shared_frame_metadata_t;

  static std::wstring
  vdd_texture_name(unsigned int monitor_idx, UINT32 slot_idx) {
    std::wstring base = L"Global\\ZakoVDD_Frame_" + std::to_wstring(monitor_idx);
    if (slot_idx == 0) {
      return base;
    }
    return base + L"_Slot_" + std::to_wstring(slot_idx);
  }

  static std::optional<bool>
  vdd_borrowed_texture_env_override() {
    const char *raw_value = std::getenv("SUNSHINE_VDD_BORROWED_TEXTURE");
    if (!raw_value || !*raw_value) {
      return std::nullopt;
    }

    std::string value { raw_value };
    std::transform(value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "1" || value == "true" || value == "on" || value == "yes" || value == "enabled") {
      return true;
    }
    if (value == "0" || value == "false" || value == "off" || value == "no" || value == "disabled") {
      return false;
    }
    BOOST_LOG(warning) << "[vdd] ignoring invalid SUNSHINE_VDD_BORROWED_TEXTURE value: "sv << value;
    return std::nullopt;
  }

  static std::optional<vdd_frame_channel::channel_mode>
  vdd_frame_channel_mode_env_override() {
    const char *raw_value = std::getenv("SUNSHINE_VDD_FRAME_CHANNEL");
    if (!raw_value || !*raw_value) {
      return std::nullopt;
    }

    std::string value {raw_value};
    std::transform(value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto mode = vdd_frame_channel::parse_channel_mode(value);
    if (!mode) {
      BOOST_LOG(warning) << "[vdd] ignoring invalid SUNSHINE_VDD_FRAME_CHANNEL value: "sv << value
                         << " (expected auto, legacy, or sealed)"sv;
    }
    return mode;
  }

  static std::optional<LUID>
  vdd_device_adapter_luid(ID3D11Device *d3d_device) {
    IDXGIDevice *dxgi_device_p = nullptr;
    HRESULT hr = d3d_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_device_p));
    if (FAILED(hr) || !dxgi_device_p) {
      BOOST_LOG(error) << "[vdd_capture] failed to query IDXGIDevice for adapter LUID: 0x"sv
                       << util::hex(hr).to_string_view();
      return std::nullopt;
    }
    dxgi_t dxgi_device {dxgi_device_p};

    IDXGIAdapter *adapter_p = nullptr;
    hr = dxgi_device->GetAdapter(&adapter_p);
    if (FAILED(hr) || !adapter_p) {
      BOOST_LOG(error) << "[vdd_capture] failed to query IDXGIAdapter for adapter LUID: 0x"sv
                       << util::hex(hr).to_string_view();
      return std::nullopt;
    }
    util::safe_ptr<IDXGIAdapter, Release<IDXGIAdapter>> adapter {adapter_p};

    DXGI_ADAPTER_DESC desc {};
    hr = adapter->GetDesc(&desc);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "[vdd_capture] IDXGIAdapter::GetDesc failed: 0x"sv
                       << util::hex(hr).to_string_view();
      return std::nullopt;
    }
    return desc.AdapterLuid;
  }

  static vdd_frame_channel::channel_selection
  probe_sealed_frame_channel(display_device::vdd_ioctl::frame_channel_caps &caps) {
    caps = {};
    switch (display_device::vdd_ioctl::query_frame_channel_caps(caps)) {
      case display_device::vdd_ioctl::frame_channel_status::supported: {
        BOOST_LOG(info) << "[vdd_capture] sealed frame-channel caps: version="sv << caps.version
                        << " flags=0x"sv << util::hex(caps.flags).to_string_view()
                        << " max_slots="sv << caps.max_shared_slots
                        << " metadata_size="sv << caps.metadata_size;
        if (caps.max_shared_slots == 0) {
          BOOST_LOG(warning) << "[vdd_capture] sealed frame-channel reported zero shared slots; using hardened legacy named channel"sv;
          return vdd_frame_channel::channel_selection::caps_failed;
        }
        const auto required_flags = display_device::vdd_ioctl::required_sealed_frame_channel_flags();
        if (caps.version != vdd_frame_channel::meta_version ||
            caps.metadata_size < vdd_frame_channel::min_metadata_size ||
            (caps.flags & required_flags) != required_flags) {
          BOOST_LOG(warning) << "[vdd_capture] sealed frame-channel caps do not satisfy the required ABI/security contract; using hardened legacy named channel"sv;
          return vdd_frame_channel::channel_selection::caps_failed;
        }
        return vdd_frame_channel::channel_selection::unknown;
      }
      case display_device::vdd_ioctl::frame_channel_status::unsupported:
        BOOST_LOG(debug) << "[vdd_capture] sealed frame-channel IOCTL unsupported; using hardened legacy named channel"sv;
        return vdd_frame_channel::channel_selection::caps_unsupported;
      case display_device::vdd_ioctl::frame_channel_status::interface_missing:
        BOOST_LOG(debug) << "[vdd_capture] VDD IOCTL control interface missing; using hardened legacy named channel"sv;
        return vdd_frame_channel::channel_selection::caps_unsupported;
      case display_device::vdd_ioctl::frame_channel_status::failed:
        BOOST_LOG(warning) << "[vdd_capture] sealed frame-channel capability probe failed; using hardened legacy named channel"sv;
        return vdd_frame_channel::channel_selection::caps_failed;
    }
    return vdd_frame_channel::channel_selection::caps_failed;
  }

  static void
  close_sealed_frame_channel_handles(display_device::vdd_ioctl::frame_channel_open_response &channel) {
    auto close_if_valid = [](std::uint64_t handle_value) {
      if (handle_value != 0) {
        CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle_value)));
      }
    };

    close_if_valid(channel.metadata_handle);
    close_if_valid(channel.frame_ready_event_handle);
    for (const auto &slot : channel.slots) {
      close_if_valid(slot.texture_handle);
    }
    channel = {};
  }

  static display_device::vdd_ioctl::frame_channel_open_status
  open_sealed_frame_channel(unsigned int monitor_idx,
                            const LUID &device_luid,
                            UINT32 desired_slots,
                            display_device::vdd_ioctl::frame_channel_open_response &channel,
                            bool log_failures = true) {
    display_device::vdd_ioctl::frame_channel_open_request request {};
    request.monitor_index = monitor_idx;
    request.required_flags = display_device::vdd_ioctl::required_sealed_frame_channel_flags();
    request.desired_slots = desired_slots;
    request.adapter_luid_low_part = device_luid.LowPart;
    request.adapter_luid_high_part = device_luid.HighPart;
    return display_device::vdd_ioctl::open_frame_channel(request, channel, log_failures);
  }

  static void
  log_vdd_metadata_reject(const SharedFrameMetadata &meta,
                          const LUID &device_luid,
                          vdd_frame_channel::reject_reason reason) {
    LUID producer_luid {};
    producer_luid.LowPart = meta.AdapterLuidLowPart;
    producer_luid.HighPart = meta.AdapterLuidHighPart;

    switch (reason) {
      case vdd_frame_channel::reject_reason::bad_magic:
        BOOST_LOG(error) << "[vdd_capture] bad metadata magic: 0x"sv
                         << util::hex(meta.Magic).to_string_view();
        break;
      case vdd_frame_channel::reject_reason::version_mismatch:
        BOOST_LOG(error) << "[vdd_capture] metadata version mismatch: producer="sv
                         << meta.Version << " consumer="sv << vdd_frame_channel::meta_version;
        break;
      case vdd_frame_channel::reject_reason::metadata_too_small:
        BOOST_LOG(error) << "[vdd_capture] metadata block too small: producer="sv
                         << meta.MetadataSize << " required="sv << vdd_frame_channel::min_metadata_size;
        break;
      case vdd_frame_channel::reject_reason::invalid_dimensions:
        BOOST_LOG(warning) << "[vdd_capture] producer has invalid dimensions: "sv
                           << meta.Width << "x"sv << meta.Height;
        break;
      case vdd_frame_channel::reject_reason::unsupported_format:
        BOOST_LOG(error) << "[vdd_capture] unsupported producer DXGI format: "sv << meta.DxgiFormat;
        break;
      case vdd_frame_channel::reject_reason::invalid_slot_count:
        BOOST_LOG(error) << "[vdd_capture] invalid producer slot count: "sv << meta.SlotCount
                         << " max="sv << vdd_frame_channel::max_shared_slots;
        break;
      case vdd_frame_channel::reject_reason::slot_index_out_of_range:
        BOOST_LOG(error) << "[vdd_capture] producer slot index "sv << meta.SlotIndex
                         << " outside slot count "sv << meta.SlotCount;
        break;
      case vdd_frame_channel::reject_reason::zero_adapter_luid:
        BOOST_LOG(error) << "[vdd_capture] producer adapter LUID is zero; refusing ambiguous shared texture channel"sv;
        break;
      case vdd_frame_channel::reject_reason::adapter_luid_mismatch:
        BOOST_LOG(error) << "[vdd_capture] producer adapter LUID "sv
                         << producer_luid.LowPart << ":"sv << producer_luid.HighPart
                         << " does not match Sunshine D3D device LUID "sv
                         << device_luid.LowPart << ":"sv << device_luid.HighPart;
        break;
      default:
        BOOST_LOG(error) << "[vdd_capture] metadata rejected for reason "sv << static_cast<int>(reason);
        break;
    }
  }

  static bool
  vdd_metadata_is_attachable(const SharedFrameMetadata &meta, const LUID &device_luid) {
    const auto result = vdd_frame_channel::validate_metadata_for_attach(meta, device_luid);
    if (!result) {
      log_vdd_metadata_reject(meta, device_luid, result.reason);
      return false;
    }
    return true;
  }

  static bool
  vdd_metadata_is_probe_valid(const SharedFrameMetadata &meta) {
    return static_cast<bool>(vdd_frame_channel::validate_metadata_for_probe(meta));
  }

  /**
   * @brief Report whether the producer recreated or resized the channel.
   * @details Shared by every consumer wake-up path. Keeping the comparison in one
   *          place is deliberate: when only the frame-ready branch performed it, a
   *          cursor-only wake could spin forever on an orphaned slot because the
   *          old FrameReady event is never signalled again after a reconfigure.
   */
  static bool
  vdd_producer_channel_changed(const SharedFrameMetadata &meta,
                               const vdd_frame_channel::producer_channel_identity &known,
                               std::string_view context) {
    switch (vdd_frame_channel::detect_producer_channel_change(meta, known)) {
      case vdd_frame_channel::producer_channel_change::generation:
        BOOST_LOG(info) << "[vdd_capture] producer channel generation changed ("sv << context
                        << "): old="sv << known.generation
                        << " new="sv << vdd_frame_channel::metadata_channel_generation(meta.MetadataSequence)
                        << "; requesting reinit"sv;
        return true;
      case vdd_frame_channel::producer_channel_change::resolution_or_format:
        BOOST_LOG(info) << "[vdd_capture] producer resolution/format changed ("sv << context
                        << "): "sv << known.width << "x"sv << known.height
                        << " fmt="sv << static_cast<int>(known.format)
                        << " -> "sv << meta.Width << "x"sv << meta.Height
                        << " fmt="sv << meta.DxgiFormat
                        << "; requesting reinit"sv;
        return true;
      case vdd_frame_channel::producer_channel_change::none:
        break;
    }
    return false;
  }

  static bool
  vdd_texture_desc_matches_metadata(const D3D11_TEXTURE2D_DESC &desc,
                                    const SharedFrameMetadata &meta,
                                    UINT32 slot) {
    const auto result = vdd_frame_channel::validate_texture_desc(desc, meta);
    if (result) {
      return true;
    }

    switch (result.reason) {
      case vdd_frame_channel::reject_reason::texture_desc_mismatch:
        BOOST_LOG(error) << "[vdd_capture] shared texture slot "sv << slot
                         << " desc mismatch: texture="sv << desc.Width << "x"sv << desc.Height
                         << " fmt="sv << static_cast<int>(desc.Format)
                         << " metadata="sv << meta.Width << "x"sv << meta.Height
                         << " fmt="sv << meta.DxgiFormat;
        break;
      case vdd_frame_channel::reject_reason::texture_layout_unsupported:
        BOOST_LOG(error) << "[vdd_capture] shared texture slot "sv << slot
                         << " has unsupported layout: array="sv << desc.ArraySize
                         << " mips="sv << desc.MipLevels
                         << " samples="sv << desc.SampleDesc.Count << "/"sv << desc.SampleDesc.Quality;
        break;
      case vdd_frame_channel::reject_reason::texture_missing_keyed_mutex:
        BOOST_LOG(error) << "[vdd_capture] shared texture slot "sv << slot
                         << " is missing D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX"sv;
        break;
      case vdd_frame_channel::reject_reason::texture_missing_shader_resource:
        BOOST_LOG(error) << "[vdd_capture] shared texture slot "sv << slot
                         << " is missing D3D11_BIND_SHADER_RESOURCE"sv;
        break;
      default:
        BOOST_LOG(error) << "[vdd_capture] shared texture slot "sv << slot
                         << " rejected for reason "sv << static_cast<int>(result.reason);
        break;
    }
    return false;
  }

  // Mirror of CursorSharedMetadata in ZakoVDD/Driver.cpp. Layout is
  // 4-byte aligned (#pragma pack(push, 4) on the producer side); the
  // standard ABI on x64 already aligns the fields below identically.
  struct CursorSharedMetadata {
    UINT32 Magic;                // 'ZVCU' = 0x5A564355
    UINT32 Version;              // 1
    UINT32 IsVisible;            // 0/1
    INT32  PositionX;            // top-left of cursor image (already hot-spot adjusted, DXGI semantics)
    INT32  PositionY;
    UINT32 PositionId;           // monotonic on position change
    UINT32 ShapeId;              // monotonic on shape change
    UINT32 ShapeType;            // IDDCX_CURSOR_SHAPE_TYPE value (0=mono, 1=color, 2=masked color)
    UINT32 Width;
    UINT32 Height;
    UINT32 Pitch;
    INT32  XHot;
    INT32  YHot;
    UINT32 SdrWhiteLevelX1000;
    UINT32 ShapeBufferSize;
    UINT32 PublicationSequence;  // odd while producer writes, even when stable
    UINT64 LastUpdateQpc;
    // Followed by up to 256 KiB of shape pixels.
  };

  static_assert(sizeof(CursorSharedMetadata) == 72, "Unexpected cursor metadata layout");
  static_assert(offsetof(CursorSharedMetadata, PublicationSequence) == 60, "Unexpected cursor sequence offset");
  static_assert(offsetof(CursorSharedMetadata, LastUpdateQpc) == 64, "Unexpected cursor QPC offset");

  static constexpr UINT32 VDD_CURSOR_MAGIC = 0x5A564355;  // 'ZVCU'
  static constexpr UINT32 VDD_CURSOR_VERSION = 1;
  static constexpr UINT32 VDD_CURSOR_MAX_BYTES = 256u * 256u * 4u;  // matches driver
  static constexpr auto VDD_CURSOR_ATTACH_RETRY_DELAY = 250ms;

  vdd_capture_t::vdd_capture_t() = default;

  vdd_capture_t::~vdd_capture_t() {
    close();
  }

  void
  vdd_capture_t::close() {
    if (m_holdsKey && m_heldSlot < m_keyedMutex.size() && m_keyedMutex[m_heldSlot]) {
      m_keyedMutex[m_heldSlot]->ReleaseSync(0);
      m_holdsKey = false;
    }
    m_keyedMutex.clear();
    m_sharedTex.clear();
    m_heldSlot = 0;
    if (m_pMeta) {
      UnmapViewOfFile(m_pMeta);
      m_pMeta = nullptr;
    }
    if (m_hMeta) {
      CloseHandle(m_hMeta);
      m_hMeta = nullptr;
    }
    if (m_hEvent) {
      CloseHandle(m_hEvent);
      m_hEvent = nullptr;
    }
    detach_cursor_channel();
  }

  void
  vdd_capture_t::detach_cursor_channel() {
    if (m_pCursorMeta) {
      UnmapViewOfFile(m_pCursorMeta);
      m_pCursorMeta = nullptr;
    }
    if (m_hCursorMeta) {
      CloseHandle(m_hCursorMeta);
      m_hCursorMeta = nullptr;
    }
    if (m_hCursorEvent) {
      CloseHandle(m_hCursorEvent);
      m_hCursorEvent = nullptr;
    }
    m_lastSeenCursorShapeId = 0xFFFFFFFFu;
    m_lastSeenCursorPositionId = 0xFFFFFFFFu;
    m_cursorUpdatePending = false;
  }

  bool
  vdd_capture_t::attach_cursor_channel() {
    // Optional: old driver builds do not export cursor mappings. Capture must
    // remain usable in that case, with the cursor overlay simply disabled.
    std::wstring cursor_meta_name = L"Global\\ZakoVDD_CursorMeta_" + std::to_wstring(m_monitorIdx);
    std::wstring cursor_event_name = L"Global\\ZakoVDD_CursorReady_" + std::to_wstring(m_monitorIdx);

    HANDLE cursor_meta = OpenFileMappingW(FILE_MAP_READ, FALSE, cursor_meta_name.c_str());
    if (!cursor_meta) {
      return false;
    }

    const SIZE_T map_size = sizeof(CursorSharedMetadata) + VDD_CURSOR_MAX_BYTES;
    void *cursor_mapping = MapViewOfFile(cursor_meta, FILE_MAP_READ, 0, 0, map_size);
    if (!cursor_mapping) {
      CloseHandle(cursor_meta);
      return false;
    }

    HANDLE cursor_event = OpenEventW(SYNCHRONIZE, FALSE, cursor_event_name.c_str());
    if (!cursor_event) {
      UnmapViewOfFile(cursor_mapping);
      CloseHandle(cursor_meta);
      return false;
    }

    m_hCursorMeta = cursor_meta;
    m_pCursorMeta = cursor_mapping;
    m_hCursorEvent = cursor_event;
    return true;
  }

  bool
  vdd_capture_t::ensure_cursor_channel_attached() {
    if (m_pCursorMeta && m_hCursorEvent) {
      return true;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextCursorAttachAttempt) {
      return false;
    }
    m_nextCursorAttachAttempt = now + VDD_CURSOR_ATTACH_RETRY_DELAY;
    detach_cursor_channel();

    if (attach_cursor_channel()) {
      BOOST_LOG(info) << "[vdd_capture] cursor channel attached for monitor "sv << m_monitorIdx
                      << (m_cursorAttachFailures ? " after retry" : "") << "."sv;
      m_cursorAttachFailures = 0;
      return true;
    }

    ++m_cursorAttachFailures;
    if (m_cursorAttachFailures == 1 || (m_cursorAttachFailures % 40) == 0) {
      BOOST_LOG(info) << "[vdd_capture] cursor channel not ready for monitor "sv << m_monitorIdx
                      << "; capture will retry without restarting the backend."sv;
    }
    return false;
  }

  void
  vdd_capture_t::apply_metadata_snapshot(const SharedFrameMetadata &meta) {
    m_width = meta.Width;
    m_height = meta.Height;
    m_format = static_cast<DXGI_FORMAT>(meta.DxgiFormat);
    m_is_hdr = (meta.IsHdr != 0);
    m_max_nits = meta.MaxNits;
    m_min_nits = meta.MinNits;
    m_max_fall = meta.MaxFALL;
    m_lastFrameCounter = meta.FrameCounter;
    m_lastPresentQpc = meta.LastPresentQpc;
    m_lastPublishQpc = meta.LastPublishQpc;
    m_lastPresentationFrameNumber = meta.LastPresentationFrameNumber;
    m_lastDirtyRectCount = meta.LastDirtyRectCount;
    m_replacedUnreadFrames = meta.ReplacedUnreadFrames;
    m_droppedConsumerHeldFrames = meta.DroppedConsumerHeldFrames;
    m_droppedAcquireFailures = meta.DroppedAcquireFailures;
    m_slotCount = meta.SlotCount;
    m_slotIndex = meta.SlotIndex;
    m_channelGeneration = vdd_frame_channel::metadata_channel_generation(meta.MetadataSequence);
    m_producerQpcFrequency = meta.ProducerQpcFrequency;
    m_adapterLuid.LowPart = meta.AdapterLuidLowPart;
    m_adapterLuid.HighPart = meta.AdapterLuidHighPart;
  }

  bool
  vdd_capture_t::attach_texture_slot(ID3D11Device1 *device,
                                     HANDLE shared_handle,
                                     const SharedFrameMetadata &meta,
                                     UINT32 slot,
                                     bool close_handle,
                                     const wchar_t *debug_name) {
    if (!device || (!shared_handle && !debug_name)) {
      return false;
    }

    ID3D11Texture2D *raw = nullptr;
    HRESULT hr = E_FAIL;
    if (debug_name) {
      hr = device->OpenSharedResourceByName(debug_name,
                                            DXGI_SHARED_RESOURCE_READ,
                                            __uuidof(ID3D11Texture2D),
                                            reinterpret_cast<void **>(&raw));
    }
    else {
      hr = device->OpenSharedResource1(shared_handle,
                                       __uuidof(ID3D11Texture2D),
                                       reinterpret_cast<void **>(&raw));
    }
    if (close_handle) {
      CloseHandle(shared_handle);
    }
    if (FAILED(hr) || !raw) {
      BOOST_LOG(error) << "[vdd_capture] failed to open shared texture slot "sv
                       << slot << ": 0x"sv << util::hex(hr).to_string_view()
                       << (debug_name ? " by name" : " by duplicated handle");
      return false;
    }
    m_sharedTex[slot].reset(raw);

    D3D11_TEXTURE2D_DESC desc {};
    m_sharedTex[slot]->GetDesc(&desc);
    if (!vdd_texture_desc_matches_metadata(desc, meta, slot)) {
      return false;
    }

    IDXGIKeyedMutex *km = nullptr;
    hr = m_sharedTex[slot]->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(&km));
    if (FAILED(hr) || !km) {
      BOOST_LOG(error) << "[vdd_capture] no IDXGIKeyedMutex on shared texture slot "sv
                       << slot << ": 0x"sv << util::hex(hr).to_string_view();
      return false;
    }
    m_keyedMutex[slot].reset(km);
    return true;
  }

  bool
  vdd_capture_t::attach_sealed_channel(ID3D11Device1 *device,
                                       unsigned int monitor_idx,
                                       const LUID &device_luid,
                                       display_device::vdd_ioctl::frame_channel_open_response &sealed_channel) {
    auto fail = [&]() -> bool {
      close();
      close_sealed_frame_channel_handles(sealed_channel);
      return false;
    };

    m_hMeta = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(sealed_channel.metadata_handle));
    sealed_channel.metadata_handle = 0;
    m_hEvent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(sealed_channel.frame_ready_event_handle));
    sealed_channel.frame_ready_event_handle = 0;

    m_pMeta = MapViewOfFile(m_hMeta, FILE_MAP_READ, 0, 0, sizeof(SharedFrameMetadata));
    if (!m_pMeta) {
      BOOST_LOG(error) << "[vdd_capture] MapViewOfFile(sealed metadata) failed: "sv << GetLastError();
      return fail();
    }

    auto *meta = static_cast<const SharedFrameMetadata *>(m_pMeta);
    SharedFrameMetadata meta_snapshot {};
    if (!vdd_frame_channel::read_stable_metadata(meta, meta_snapshot)) {
      BOOST_LOG(error) << "[vdd_capture] unable to read a stable sealed metadata snapshot"sv;
      return fail();
    }
    if (!vdd_metadata_is_attachable(meta_snapshot, device_luid)) {
      return fail();
    }
    if (meta_snapshot.SlotCount != sealed_channel.slot_count ||
        sealed_channel.slots.size() < meta_snapshot.SlotCount) {
      BOOST_LOG(error) << "[vdd_capture] sealed channel slot mismatch: metadata="sv
                       << meta_snapshot.SlotCount << " response="sv << sealed_channel.slot_count
                       << " handles="sv << sealed_channel.slots.size();
      return fail();
    }

    apply_metadata_snapshot(meta_snapshot);

    m_sharedTex.resize(m_slotCount);
    m_keyedMutex.resize(m_slotCount);
    for (UINT32 slot = 0; slot < m_slotCount; ++slot) {
      auto &slot_handle_value = sealed_channel.slots[slot].texture_handle;
      HANDLE slot_handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(slot_handle_value));
      slot_handle_value = 0;
      if (!attach_texture_slot(device, slot_handle, meta_snapshot, slot, true)) {
        return fail();
      }
    }

    close_sealed_frame_channel_handles(sealed_channel);
    m_legacyNamedChannel = false;
    m_frameChannelSelection = vdd_frame_channel::channel_selection::sealed_opened;
    BOOST_LOG(info) << "[vdd_capture] opened sealed monitor "sv << monitor_idx
                    << " "sv << m_width << "x"sv << m_height
                    << " fmt="sv << static_cast<int>(m_format)
                    << " hdr="sv << m_is_hdr
                    << " luid="sv << m_adapterLuid.LowPart << ":"sv << m_adapterLuid.HighPart
                    << " slot="sv << m_slotIndex << "/"sv << m_slotCount
                    << " meta_size="sv << meta_snapshot.MetadataSize
                    << " meta_seq="sv << meta_snapshot.MetadataSequence
                    << " generation="sv << m_channelGeneration
                    << " mode="sv << vdd_frame_channel::channel_mode_name(m_frameChannelMode)
                    << " channel=sealed"
                    << " selection="sv << vdd_frame_channel::channel_selection_name(m_frameChannelSelection)
                    << " access=duplicated_handles";
    return true;
  }

  vdd_capture_t::sealed_channel_attempt
  vdd_capture_t::try_open_sealed_channel(ID3D11Device1 *device,
                                         unsigned int monitor_idx,
                                         const LUID &device_luid,
                                         const display_device::vdd_ioctl::frame_channel_caps &sealed_caps) {
    if (m_frameChannelMode == vdd_frame_channel::channel_mode::legacy_named) {
      return sealed_channel_attempt::skipped;
    }

    if (!m_sealedFrameChannelSupported) {
      if (m_frameChannelMode == vdd_frame_channel::channel_mode::sealed_required) {
        m_frameChannelSelection = vdd_frame_channel::channel_selection::sealed_required_failed;
        BOOST_LOG(error) << "[vdd_capture] SUNSHINE_VDD_FRAME_CHANNEL=sealed requested, "
                            "but the installed VDD driver does not expose a sealed frame channel"sv;
        return sealed_channel_attempt::required_failed;
      }
      return sealed_channel_attempt::fallback_allowed;
    }

    display_device::vdd_ioctl::frame_channel_open_response sealed_channel;
    const UINT32 desired_slots = std::min<UINT32>(vdd_frame_channel::max_shared_slots, sealed_caps.max_shared_slots);
    const auto open_status = open_sealed_frame_channel(monitor_idx, device_luid, desired_slots, sealed_channel);
    if (open_status == display_device::vdd_ioctl::frame_channel_open_status::opened) {
      if (attach_sealed_channel(device, monitor_idx, device_luid, sealed_channel)) {
        return sealed_channel_attempt::opened;
      }
      if (m_frameChannelMode == vdd_frame_channel::channel_mode::sealed_required) {
        m_frameChannelSelection = vdd_frame_channel::channel_selection::sealed_required_failed;
        return sealed_channel_attempt::required_failed;
      }
      m_frameChannelSelection = vdd_frame_channel::channel_selection::attach_failed;
      BOOST_LOG(warning) << "[vdd_capture] sealed frame channel attach failed; falling back to hardened legacy named channel"sv;
      return sealed_channel_attempt::fallback_allowed;
    }

    if (open_status == display_device::vdd_ioctl::frame_channel_open_status::not_ready) {
      m_frameChannelSelection = vdd_frame_channel::channel_selection::open_not_ready;
      BOOST_LOG(debug) << "[vdd_capture] sealed frame channel is not ready; retrying producer transition"sv;
      return sealed_channel_attempt::not_ready;
    }

    if (m_frameChannelMode == vdd_frame_channel::channel_mode::sealed_required) {
      m_frameChannelSelection = vdd_frame_channel::channel_selection::sealed_required_failed;
      BOOST_LOG(error) << "[vdd_capture] SUNSHINE_VDD_FRAME_CHANNEL=sealed requested, "
                       << "but sealed frame channel open failed with status "sv
                       << display_device::vdd_ioctl::frame_channel_open_status_name(open_status);
      return sealed_channel_attempt::required_failed;
    }

    m_frameChannelSelection =
      open_status == display_device::vdd_ioctl::frame_channel_open_status::unsupported ?
        vdd_frame_channel::channel_selection::open_unsupported :
        vdd_frame_channel::channel_selection::open_failed;
    BOOST_LOG(warning) << "[vdd_capture] sealed frame channel open failed with status "sv
                       << display_device::vdd_ioctl::frame_channel_open_status_name(open_status)
                       << "; falling back to hardened legacy named channel"sv;
    return sealed_channel_attempt::fallback_allowed;
  }

  bool
  vdd_capture_t::attach_legacy_named_channel(ID3D11Device1 *device,
                                             unsigned int monitor_idx,
                                             const LUID &device_luid) {
    std::wstring meta_name = L"Global\\ZakoVDD_Meta_" + std::to_wstring(monitor_idx);
    std::wstring ev_name = L"Global\\ZakoVDD_FrameReady_" + std::to_wstring(monitor_idx);

    // Open metadata first so we can fail fast if the driver isn't running
    // (or hasn't published a swap chain yet for this monitor).
    m_hMeta = OpenFileMappingW(FILE_MAP_READ, FALSE, meta_name.c_str());
    if (!m_hMeta) {
      auto err = GetLastError();
      BOOST_LOG(warning) << "[vdd_capture] OpenFileMappingW failed for monitor "sv
                         << monitor_idx << " (gle="sv << err << "). "sv
                         << "VDD driver running? Monitor active?"sv;
      return false;
    }

    m_pMeta = MapViewOfFile(m_hMeta, FILE_MAP_READ, 0, 0, sizeof(SharedFrameMetadata));
    if (!m_pMeta) {
      BOOST_LOG(error) << "[vdd_capture] MapViewOfFile failed: "sv << GetLastError();
      close();
      return false;
    }

    auto *meta = static_cast<const SharedFrameMetadata *>(m_pMeta);
    SharedFrameMetadata meta_snapshot {};
    if (!vdd_frame_channel::read_stable_metadata(meta, meta_snapshot)) {
      BOOST_LOG(error) << "[vdd_capture] unable to read a stable metadata snapshot"sv;
      close();
      return false;
    }
    if (!vdd_metadata_is_attachable(meta_snapshot, device_luid)) {
      close();
      return false;
    }

    apply_metadata_snapshot(meta_snapshot);

    m_hEvent = OpenEventW(SYNCHRONIZE, FALSE, ev_name.c_str());
    if (!m_hEvent) {
      BOOST_LOG(error) << "[vdd_capture] OpenEventW failed: "sv << GetLastError();
      close();
      return false;
    }

    // Open the named shared texture using ID3D11Device1::OpenSharedResourceByName.
    m_sharedTex.resize(m_slotCount);
    m_keyedMutex.resize(m_slotCount);
    for (UINT32 slot = 0; slot < m_slotCount; ++slot) {
      auto tex_name = vdd_texture_name(monitor_idx, slot);
      if (!attach_texture_slot(device, nullptr, meta_snapshot, slot, false, tex_name.c_str())) {
        close();
        return false;
      }
    }

    if (m_frameChannelSelection != vdd_frame_channel::channel_selection::forced_legacy) {
      m_frameChannelSelection = vdd_frame_channel::channel_selection::legacy_opened;
    }
    BOOST_LOG(info) << "[vdd_capture] opened monitor "sv << monitor_idx
                    << " "sv << m_width << "x"sv << m_height
                    << " fmt="sv << static_cast<int>(m_format)
                    << " hdr="sv << m_is_hdr
                    << " luid="sv << m_adapterLuid.LowPart << ":"sv << m_adapterLuid.HighPart
                    << " slot="sv << m_slotIndex << "/"sv << m_slotCount
                    << " meta_size="sv << meta_snapshot.MetadataSize
                    << " meta_seq="sv << meta_snapshot.MetadataSequence
                    << " generation="sv << m_channelGeneration
                    << " mode="sv << vdd_frame_channel::channel_mode_name(m_frameChannelMode)
                    << " channel="sv << (m_sealedFrameChannelSupported ? "sealed_probe_supported_legacy_capture" : "legacy_named")
                    << " selection="sv << vdd_frame_channel::channel_selection_name(m_frameChannelSelection)
                    << " access=read_only";
    return true;
  }

  int
  vdd_capture_t::init(ID3D11Device *d3d_device, unsigned int monitor_idx) {
    if (!d3d_device) {
      BOOST_LOG(error) << "[vdd_capture] init: null D3D11 device"sv;
      return -1;
    }
    const auto device_luid = vdd_device_adapter_luid(d3d_device);
    if (!device_luid) {
      return -1;
    }
    ID3D11Device1 *dev1_p = nullptr;
    HRESULT hr = d3d_device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&dev1_p));
    if (FAILED(hr) || !dev1_p) {
      BOOST_LOG(error) << "[vdd_capture] ID3D11Device1 not available: 0x"sv
                       << util::hex(hr).to_string_view();
      return -1;
    }
    device1_t dev1 {dev1_p};

    m_monitorIdx = monitor_idx;
    m_nextCursorAttachAttempt = {};
    m_cursorAttachFailures = 0;

    m_frameChannelMode = vdd_frame_channel_mode_env_override().value_or(vdd_frame_channel::channel_mode::auto_probe);
    m_frameChannelSelection = vdd_frame_channel::channel_selection::unknown;
    display_device::vdd_ioctl::frame_channel_caps sealed_caps {};
    if (m_frameChannelMode == vdd_frame_channel::channel_mode::legacy_named) {
      m_sealedFrameChannelSupported = false;
      m_frameChannelSelection = vdd_frame_channel::channel_selection::forced_legacy;
      BOOST_LOG(info) << "[vdd_capture] frame channel mode forced to legacy_named; skipping sealed capability probe"sv;
    }
    else {
      m_frameChannelSelection = probe_sealed_frame_channel(sealed_caps);
      m_sealedFrameChannelSupported = (m_frameChannelSelection == vdd_frame_channel::channel_selection::unknown);
    }
    m_legacyNamedChannel = true;

    switch (try_open_sealed_channel(dev1_p, monitor_idx, *device_luid, sealed_caps)) {
      case sealed_channel_attempt::opened:
        ensure_cursor_channel_attached();
        return 0;
      case sealed_channel_attempt::required_failed:
        return -1;
      case sealed_channel_attempt::not_ready:
        return -1;
      case sealed_channel_attempt::skipped:
      case sealed_channel_attempt::fallback_allowed:
        break;
    }

    if (!attach_legacy_named_channel(dev1_p, monitor_idx, *device_luid)) {
      return -1;
    }
    ensure_cursor_channel_attached();
    return 0;
  }

  capture_e
  vdd_capture_t::next_frame(std::chrono::milliseconds timeout,
                            ID3D11Texture2D **out,
                            uint64_t &out_frame_qpc,
                            bool wait_for_cursor,
                            bool &out_cursor_only) {
    if (out) *out = nullptr;
    out_frame_qpc = 0;
    out_cursor_only = false;

    if (!m_hEvent || m_keyedMutex.empty() || m_sharedTex.empty() || !m_pMeta) {
      return capture_e::error;
    }

    auto *meta = static_cast<const SharedFrameMetadata *>(m_pMeta);

    const bool cursor_channel_ready = wait_for_cursor && ensure_cursor_channel_attached();

    // CursorExporter uses an auto-reset event. Waiting on it alongside the
    // frame event lets pointer-only motion wake a static desktop capture.
    DWORD ms = static_cast<DWORD>(timeout.count() < 0 ? 0 : timeout.count());
    if (!wait_for_cursor) {
      m_cursorUpdatePending = false;
    }

    auto acquire_cursor_frame = [&]() -> capture_e {
      // A cursor-only wake never consumes the frame-ready event, so it must
      // validate the producer itself. After the producer recreates the channel
      // the old FrameReady event is never signalled again: without these checks
      // every subsequent wake would come from the cursor event, re-copy an
      // orphaned slot and pin the encoder to a stale frame until the session is
      // torn down and rebuilt.
      SharedFrameMetadata cursor_meta {};
      if (!vdd_frame_channel::read_stable_metadata(meta, cursor_meta)) {
        BOOST_LOG(warning) << "[vdd_capture] unable to read stable metadata on a cursor-only wake; requesting reinit"sv;
        return capture_e::reinit;
      }
      if (!vdd_metadata_is_attachable(cursor_meta, m_adapterLuid)) {
        return capture_e::reinit;
      }
      if (vdd_producer_channel_changed(cursor_meta, current_channel_identity(), "cursor-only wake"sv)) {
        return capture_e::reinit;
      }

      // The last consumed producer slot is available as key 0 while the
      // desktop is static. Reacquire it briefly so cursor-only updates retain
      // the normal single-copy/zero-copy cost on actual desktop frames.
      const UINT32 slot = m_slotIndex;
      if (slot >= m_keyedMutex.size()) {
        return capture_e::reinit;
      }
      const DWORD acquire_ms = vdd_frame_channel::bounded_consumer_acquire_timeout_ms(ms);
      HRESULT hr = m_keyedMutex[slot]->AcquireSync(0, acquire_ms);
      if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) {
        return capture_e::timeout;
      }
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[vdd_capture] AcquireSync(0) failed for cursor-only slot "sv
                         << slot << ": 0x"sv << util::hex(hr).to_string_view();
        return capture_e::error;
      }
      m_holdsKey = true;
      m_heldSlot = slot;
      if (out) {
        m_sharedTex[slot]->AddRef();
        *out = m_sharedTex[slot].get();
      }
      m_cursorUpdatePending = false;
      out_cursor_only = true;
      return capture_e::ok;
    };

    // An auto-reset cursor event was already consumed, but the texture slot
    // was temporarily busy. Probe key 1 without consuming FrameReady first:
    // a newly-published desktop frame must win over retrying key 0, otherwise
    // a pending cursor update could starve the frame that advances key 1.
    if (m_cursorUpdatePending && wait_for_cursor) {
      bool desktop_frame_ready = false;
      SharedFrameMetadata pending_meta {};
      if (vdd_frame_channel::read_stable_metadata(meta, pending_meta) &&
          pending_meta.SlotIndex < m_keyedMutex.size()) {
        const UINT32 pending_slot = pending_meta.SlotIndex;
        HRESULT probe_hr = m_keyedMutex[pending_slot]->AcquireSync(1, 0);
        if (probe_hr == S_OK) {
          probe_hr = m_keyedMutex[pending_slot]->ReleaseSync(1);
          if (FAILED(probe_hr)) {
            BOOST_LOG(error) << "[vdd_capture] ReleaseSync(1) failed after pending cursor probe for slot "sv
                             << pending_slot << ": 0x"sv << util::hex(probe_hr).to_string_view();
            return capture_e::error;
          }
          desktop_frame_ready = true;
        }
        else if (probe_hr != static_cast<HRESULT>(WAIT_TIMEOUT)) {
          BOOST_LOG(error) << "[vdd_capture] AcquireSync(1) probe failed for pending cursor slot "sv
                           << pending_slot << ": 0x"sv << util::hex(probe_hr).to_string_view();
          return capture_e::error;
        }
      }

      if (vdd_frame_channel::select_pending_cursor_action(
            true,
            desktop_frame_ready
          ) == vdd_frame_channel::pending_cursor_action::retry_cursor_frame) {
        return acquire_cursor_frame();
      }
    }

    HANDLE events[] = {m_hEvent, m_hCursorEvent};
    const DWORD event_count = cursor_channel_ready ? 2 : 1;
    DWORD wr = WaitForMultipleObjects(event_count, events, FALSE, ms);
    if (wr == WAIT_TIMEOUT) {
      return capture_e::timeout;
    }
    if (wr == WAIT_OBJECT_0 + 1) {
      m_cursorUpdatePending = true;
      return acquire_cursor_frame();
    }
    if (wr != WAIT_OBJECT_0) {
      BOOST_LOG(error) << "[vdd_capture] WaitForMultipleObjects: result="sv << wr
                       << " gle="sv << GetLastError();
      return capture_e::error;
    }

    // Coalesce a cursor signal that arrived with the new desktop frame. The
    // caller will read the latest cursor state while processing this frame.
    if (cursor_channel_ready) {
      WaitForSingleObject(m_hCursorEvent, 0);
    }

    // Producer released the latest slot as key 1; consumer acquires that slot.
    SharedFrameMetadata meta_snapshot {};
    if (!vdd_frame_channel::read_stable_metadata(meta, meta_snapshot)) {
      BOOST_LOG(warning) << "[vdd_capture] unable to read stable metadata after frame-ready; requesting reinit"sv;
      return capture_e::reinit;
    }
    if (!vdd_metadata_is_attachable(meta_snapshot, m_adapterLuid)) {
      return capture_e::reinit;
    }
    // Detect producer-side resize / channel recreation before taking the slot:
    // the metadata block can change any time the swap chain is re-created.
    if (vdd_producer_channel_changed(meta_snapshot, current_channel_identity(), "frame-ready"sv)) {
      return capture_e::reinit;
    }

    UINT32 slot = meta_snapshot.SlotIndex;
    if (slot >= m_keyedMutex.size()) {
      BOOST_LOG(warning) << "[vdd_capture] producer slot "sv << slot
                         << " outside opened slot count "sv << m_keyedMutex.size()
                         << "; requesting reinit"sv;
      return capture_e::reinit;
    }

    const DWORD acquire_ms = vdd_frame_channel::bounded_consumer_acquire_timeout_ms(ms);
    HRESULT hr = m_keyedMutex[slot]->AcquireSync(1, acquire_ms);
    if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) {
      ++m_consumerAcquireTimeouts;
      BOOST_LOG(debug) << "[vdd_capture] AcquireSync(1) timeout for slot "sv << slot
                       << " after frame-ready event (timeout_ms="sv << acquire_ms << ')';
      return capture_e::timeout;
    }
    if (FAILED(hr)) {
      BOOST_LOG(error) << "[vdd_capture] AcquireSync(1) failed for slot "sv << slot
                       << ": 0x"sv
                       << util::hex(hr).to_string_view();
      return capture_e::error;
    }
    m_holdsKey = true;
    m_heldSlot = slot;
    m_cursorUpdatePending = false;

    const auto previous_replaced = m_replacedUnreadFrames;
    const auto previous_held = m_droppedConsumerHeldFrames;
    const auto previous_acquire = m_droppedAcquireFailures;

    // Refresh producer metadata (cheap copy from shared mapping).
    apply_metadata_snapshot(meta_snapshot);
    m_slotIndex = slot;
    out_frame_qpc = m_lastPresentQpc ? m_lastPresentQpc : m_lastPublishQpc;

    if (m_replacedUnreadFrames != previous_replaced ||
        m_droppedConsumerHeldFrames != previous_held ||
        m_droppedAcquireFailures != previous_acquire) {
      BOOST_LOG(debug) << "[vdd_capture] producer mailbox stats: frame="sv << m_lastFrameCounter
                       << " replaced_unread="sv << m_replacedUnreadFrames
                       << " dropped_consumer_held="sv << m_droppedConsumerHeldFrames
                       << " dropped_acquire_failures="sv << m_droppedAcquireFailures;
    }

    if (out) {
      m_sharedTex[slot]->AddRef();
      *out = m_sharedTex[slot].get();
    }
    return capture_e::ok;
  }

  capture_e
  vdd_capture_t::handoff_frame(UINT64 release_key, IDXGIKeyedMutex **out_mutex, UINT32 &out_slot) {
    if (out_mutex) *out_mutex = nullptr;
    out_slot = 0;

    if (!m_holdsKey || m_heldSlot >= m_keyedMutex.size() || !m_keyedMutex[m_heldSlot]) {
      BOOST_LOG(error) << "[vdd_capture] handoff_frame called without a held slot"sv;
      return capture_e::error;
    }

    auto *mutex = m_keyedMutex[m_heldSlot].get();
    HRESULT hr = mutex->ReleaseSync(release_key);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "[vdd_capture] ReleaseSync("sv << release_key
                       << ") failed during handoff for slot "sv << m_heldSlot
                       << ": 0x"sv << util::hex(hr).to_string_view();
      return capture_e::error;
    }

    out_slot = m_heldSlot;
    if (out_mutex) {
      mutex->AddRef();
      *out_mutex = mutex;
    }
    m_holdsKey = false;
    m_heldSlot = 0;
    return capture_e::ok;
  }

  capture_e
  vdd_capture_t::release_frame() {
    if (m_holdsKey && m_heldSlot < m_keyedMutex.size() && m_keyedMutex[m_heldSlot]) {
      m_keyedMutex[m_heldSlot]->ReleaseSync(0);
      m_holdsKey = false;
      m_heldSlot = 0;
    }
    return capture_e::ok;
  }

  bool
  vdd_capture_t::poll_cursor(cursor_snapshot &out) {
    out = {};
    if (!m_pCursorMeta && !ensure_cursor_channel_attached()) {
      return false;
    }

    auto *meta = static_cast<const CursorSharedMetadata *>(m_pCursorMeta);
    auto *publication_sequence = reinterpret_cast<volatile const UINT32 *>(&meta->PublicationSequence);
    auto *published_position_id = reinterpret_cast<volatile const UINT32 *>(&meta->PositionId);
    auto *published_shape_id = reinterpret_cast<volatile const UINT32 *>(&meta->ShapeId);

    // The producer uses a seqlock: odd means a publication is in progress and
    // matching even values around the complete header+payload copy identify a
    // coherent snapshot. Sequence zero also supports the preview producer that
    // used this field as Reserved0; its shape and position IDs are commit markers.
    for (int attempt = 0; attempt < 4; ++attempt) {
      const UINT32 sequence_before = *publication_sequence;
      if ((sequence_before & 1u) != 0u) {
        continue;
      }
      MemoryBarrier();

      CursorSharedMetadata header {};
      std::memcpy(&header, meta, sizeof(header));
      if (header.ShapeBufferSize > VDD_CURSOR_MAX_BYTES) {
        continue;
      }

      const bool shape_updated = header.ShapeId != m_lastSeenCursorShapeId;
      std::vector<uint8_t> shape_buffer;
      if (shape_updated && header.ShapeBufferSize > 0) {
        const auto *payload = reinterpret_cast<const uint8_t *>(meta + 1);
        shape_buffer.assign(payload, payload + header.ShapeBufferSize);
      }

      MemoryBarrier();
      const UINT32 sequence_after = *publication_sequence;
      if (sequence_before != sequence_after || (sequence_after & 1u) != 0u) {
        continue;
      }
      if (sequence_after == 0 &&
          (*published_shape_id != header.ShapeId || *published_position_id != header.PositionId)) {
        continue;
      }

      if (header.Magic != VDD_CURSOR_MAGIC || header.Version != VDD_CURSOR_VERSION) {
        return false;
      }
      if (header.ShapeType > 2) {
        return false;
      }

      constexpr UINT32 kMaxCursorWidth = 256;
      constexpr UINT32 kMaxCursorHeight = 512;  // monochrome stores AND+XOR vertically
      const UINT32 max_height = header.ShapeType == 0 ? kMaxCursorHeight : 256;
      const bool empty_hidden_shape = header.IsVisible == 0 &&
                                      header.Width == 0 &&
                                      header.Height == 0 &&
                                      header.Pitch == 0 &&
                                      header.ShapeBufferSize == 0;
      if (!empty_hidden_shape &&
          (header.Width == 0 || header.Width > kMaxCursorWidth ||
           header.Height == 0 || header.Height > max_height)) {
        return false;
      }

      UINT64 required_bytes = 0;
      if (empty_hidden_shape) {
        // A newly-attached producer can publish visibility before its first shape.
      }
      else if (header.ShapeType == 0 /* MONOCHROME */) {
        if ((header.Height & 1u) != 0u || header.Pitch < (header.Width + 7u) / 8u) {
          return false;
        }
        required_bytes = static_cast<UINT64>(header.Pitch) * header.Height;
      }
      else {
        if (header.Pitch < static_cast<UINT64>(header.Width) * 4u) {
          return false;
        }
        required_bytes = static_cast<UINT64>(header.Pitch) * header.Height;
      }
      if (required_bytes > VDD_CURSOR_MAX_BYTES || header.ShapeBufferSize < required_bytes) {
        return false;  // header/payload mismatch; likely a torn read
      }
      if (shape_updated) {
        shape_buffer.resize(static_cast<size_t>(required_bytes));
      }

      out.valid = true;
      out.visible = header.IsVisible != 0;
      out.x = header.PositionX;
      out.y = header.PositionY;
      out.position_id = header.PositionId;
      out.shape_id = header.ShapeId;
      out.shape_type = header.ShapeType;
      out.width = header.Width;
      out.height = header.Height;
      out.pitch = header.Pitch;
      out.xhot = header.XHot;
      out.yhot = header.YHot;
      out.sdr_white_level_x1000 = header.SdrWhiteLevelX1000;
      out.update_qpc = header.LastUpdateQpc;
      out.position_updated = header.PositionId != m_lastSeenCursorPositionId;
      out.shape_updated = shape_updated;
      out.shape_buffer = std::move(shape_buffer);

      if (out.position_updated) {
        m_lastSeenCursorPositionId = header.PositionId;
      }
      return true;
    }

    return false;
  }

  void
  vdd_capture_t::acknowledge_cursor_shape(UINT32 shape_id) {
    m_lastSeenCursorShapeId = shape_id;
  }

  void
  vdd_capture_t::invalidate_cursor_shape() {
    m_lastSeenCursorShapeId = 0xFFFFFFFFu;
  }

  // ===========================================================================
  // display_vdd_vram_t
  // ===========================================================================
  //
  // Resolves the VDD monitor index for a given DXGI display by probing
  // Global\ZakoVDD_Meta_<i> mappings and matching producer-side dimensions
  // against the DXGI output's reported size. Then opens the shared texture
  // on the same D3D11 device as display_base_t to avoid cross-device copies.

  struct vdd_probe_result_t {
    int exact = -1;
    int only_valid = -1;
    int valid_count = 0;
    unsigned int only_valid_width = 0;
    unsigned int only_valid_height = 0;
  };

  // A display-mode commit and the first frame from the replacement VDD swap
  // chain are separate asynchronous events. Wait when no producer exists yet.
  // A sole mismatched producer is unambiguous but cannot represent the DXGI
  // desktop mode; report it immediately so the display factory can use DDX
  // instead of waiting here and then ending the video session.
  static constexpr auto vdd_producer_discovery_window = 8s;
  static constexpr auto vdd_producer_discovery_retry_delay = 100ms;

  static vdd_probe_result_t
  probe_vdd_monitor_index(unsigned int target_w, unsigned int target_h, unsigned int max_probe) {
    vdd_probe_result_t result;

    for (unsigned int i = 0; i < max_probe; ++i) {
      std::wstring meta_name = L"Global\\ZakoVDD_Meta_" + std::to_wstring(i);
      HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, meta_name.c_str());
      if (!h) continue;
      void *p = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(SharedFrameMetadata));
      if (!p) {
        CloseHandle(h);
        continue;
      }
      auto *meta = static_cast<const SharedFrameMetadata *>(p);
      SharedFrameMetadata meta_snapshot {};
      bool stable = vdd_frame_channel::read_stable_metadata(meta, meta_snapshot);
      bool valid = stable && vdd_metadata_is_probe_valid(meta_snapshot);
      unsigned mw = valid ? meta_snapshot.Width : 0;
      unsigned mh = valid ? meta_snapshot.Height : 0;
      unsigned mfmt = valid ? meta_snapshot.DxgiFormat : 0;
      bool mhdr = valid && (meta_snapshot.IsHdr != 0);
      UnmapViewOfFile(p);
      CloseHandle(h);
      if (!valid) continue;
      BOOST_LOG(debug) << "[vdd] probe meta_"sv << i
                       << ": "sv << mw << "x"sv << mh
                       << " fmt="sv << mfmt << " hdr="sv << mhdr;
      ++result.valid_count;
      result.only_valid = static_cast<int>(i);
      result.only_valid_width = mw;
      result.only_valid_height = mh;
      if (result.exact < 0 && mw == target_w && mh == target_h) {
        result.exact = static_cast<int>(i);
      }
    }

    return result;
  }

  static vdd_probe_result_t
  probe_sealed_vdd_monitor_index(const LUID &device_luid,
                                 const display_device::vdd_ioctl::frame_channel_caps &sealed_caps,
                                 unsigned int target_w,
                                 unsigned int target_h,
                                 unsigned int max_probe) {
    vdd_probe_result_t result;

    const UINT32 desired_slots = std::min<UINT32>(vdd_frame_channel::max_shared_slots, sealed_caps.max_shared_slots);
    for (unsigned int i = 0; i < max_probe; ++i) {
      display_device::vdd_ioctl::frame_channel_open_response sealed_channel;
      const auto open_status = open_sealed_frame_channel(i, device_luid, desired_slots, sealed_channel, false);
      if (open_status == display_device::vdd_ioctl::frame_channel_open_status::unsupported ||
          open_status == display_device::vdd_ioctl::frame_channel_open_status::interface_missing) {
        close_sealed_frame_channel_handles(sealed_channel);
        break;
      }
      if (open_status != display_device::vdd_ioctl::frame_channel_open_status::opened) {
        close_sealed_frame_channel_handles(sealed_channel);
        continue;
      }

      HANDLE h_meta = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(sealed_channel.metadata_handle));
      void *p_meta = h_meta ? MapViewOfFile(h_meta, FILE_MAP_READ, 0, 0, sizeof(SharedFrameMetadata)) : nullptr;
      if (!p_meta) {
        BOOST_LOG(debug) << "[vdd] sealed probe failed to map metadata for monitor "sv
                         << i << " (gle="sv << GetLastError() << ")"sv;
        close_sealed_frame_channel_handles(sealed_channel);
        continue;
      }

      auto *meta = static_cast<const SharedFrameMetadata *>(p_meta);
      SharedFrameMetadata meta_snapshot {};
      bool stable = vdd_frame_channel::read_stable_metadata(meta, meta_snapshot);
      auto validation = stable ?
        vdd_frame_channel::validate_metadata_for_attach(meta_snapshot, device_luid) :
        vdd_frame_channel::validation_result {};
      bool valid = stable && static_cast<bool>(validation);
      unsigned mw = valid ? meta_snapshot.Width : 0;
      unsigned mh = valid ? meta_snapshot.Height : 0;
      unsigned mfmt = valid ? meta_snapshot.DxgiFormat : 0;
      bool mhdr = valid && (meta_snapshot.IsHdr != 0);

      UnmapViewOfFile(p_meta);
      close_sealed_frame_channel_handles(sealed_channel);
      if (!valid) {
        BOOST_LOG(debug) << "[vdd] sealed probe rejected monitor "sv << i
                         << " stable="sv << stable
                         << " reason="sv << static_cast<int>(validation.reason);
        continue;
      }

      BOOST_LOG(debug) << "[vdd] sealed probe monitor "sv << i
                       << ": "sv << mw << "x"sv << mh
                       << " fmt="sv << mfmt << " hdr="sv << mhdr;
      ++result.valid_count;
      result.only_valid = static_cast<int>(i);
      result.only_valid_width = mw;
      result.only_valid_height = mh;
      if (result.exact < 0 && mw == target_w && mh == target_h) {
        result.exact = static_cast<int>(i);
      }
    }

    return result;
  }

  // Prefer an exact producer match. A sole mismatched producer is returned as
  // an explicit mismatch so the caller can fail over to DDX without waiting.
  static vdd_frame_channel::producer_selection
  resolve_vdd_monitor_index(unsigned int target_w,
                            unsigned int target_h,
                            unsigned int max_probe = 16,
                            std::optional<std::chrono::steady_clock::time_point> deadline_override = std::nullopt) {
    vdd_probe_result_t last_result;
    const auto deadline = deadline_override.value_or(std::chrono::steady_clock::now() + vdd_producer_discovery_window);

    for (;;) {
      last_result = probe_vdd_monitor_index(target_w, target_h, max_probe);
      const auto selection = vdd_frame_channel::select_producer(
        last_result.exact,
        last_result.only_valid,
        static_cast<unsigned int>(last_result.valid_count)
      );
      if (selection.match == vdd_frame_channel::producer_match::exact) {
        BOOST_LOG(info) << "[vdd] resolved monitor index "sv << selection.index
                        << " for "sv << target_w << "x"sv << target_h << " (exact match)"sv;
        return selection;
      }
      if (selection.match == vdd_frame_channel::producer_match::sole_mismatch) {
        BOOST_LOG(warning) << "[vdd] sole producer monitor "sv << selection.index
                           << " is "sv << last_result.only_valid_width
                           << "x"sv << last_result.only_valid_height
                           << ", while DXGI reports "sv << target_w << "x"sv << target_h
                           << "; direct capture cannot represent this desktop mode"sv;
        return selection;
      }

      if (std::chrono::steady_clock::now() >= deadline) {
        break;
      }

      std::this_thread::sleep_for(vdd_producer_discovery_retry_delay);
    }

    if (last_result.valid_count == 0) {
      BOOST_LOG(warning) << "[vdd] no valid VDD producer found (no Meta_* mappings). "sv
                         << "Is the ZakoVDD driver installed and running?"sv;
    }
    else {
      BOOST_LOG(warning) << "[vdd] "sv << last_result.valid_count
                         << " VDD producers present but none match "sv
                         << target_w << "x"sv << target_h
                         << "."sv;
    }
    return {};
  }

  static vdd_frame_channel::producer_selection
  resolve_sealed_vdd_monitor_index(ID3D11Device *d3d_device,
                                   unsigned int target_w,
                                   unsigned int target_h,
                                   unsigned int max_probe = 16,
                                   std::optional<std::chrono::steady_clock::time_point> deadline_override = std::nullopt) {
    const auto device_luid = vdd_device_adapter_luid(d3d_device);
    if (!device_luid) {
      return {};
    }

    display_device::vdd_ioctl::frame_channel_caps sealed_caps {};
    if (probe_sealed_frame_channel(sealed_caps) != vdd_frame_channel::channel_selection::unknown) {
      return {};
    }

    vdd_probe_result_t last_result;
    const auto deadline = deadline_override.value_or(std::chrono::steady_clock::now() + vdd_producer_discovery_window);

    for (;;) {
      last_result = probe_sealed_vdd_monitor_index(*device_luid, sealed_caps, target_w, target_h, max_probe);
      const auto selection = vdd_frame_channel::select_producer(
        last_result.exact,
        last_result.only_valid,
        static_cast<unsigned int>(last_result.valid_count)
      );
      if (selection.match == vdd_frame_channel::producer_match::exact) {
        BOOST_LOG(info) << "[vdd] resolved sealed monitor index "sv << selection.index
                        << " for "sv << target_w << "x"sv << target_h << " (exact match)"sv;
        return selection;
      }
      if (selection.match == vdd_frame_channel::producer_match::sole_mismatch) {
        BOOST_LOG(warning) << "[vdd] sole sealed producer monitor "sv << selection.index
                           << " is "sv << last_result.only_valid_width
                           << "x"sv << last_result.only_valid_height
                           << ", while DXGI reports "sv << target_w << "x"sv << target_h
                           << "; direct capture cannot represent this desktop mode"sv;
        return selection;
      }

      if (std::chrono::steady_clock::now() >= deadline) {
        break;
      }

      std::this_thread::sleep_for(vdd_producer_discovery_retry_delay);
    }

    if (last_result.valid_count == 0) {
      BOOST_LOG(warning) << "[vdd] no valid sealed VDD producer found for "sv
                         << target_w << "x"sv << target_h << "."sv;
    }
    else {
      BOOST_LOG(warning) << "[vdd] "sv << last_result.valid_count
                         << " sealed VDD producers present but none match "sv
                         << target_w << "x"sv << target_h
                         << "."sv;
    }
    return {};
  }

  int
  display_vdd_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name)) {
      BOOST_LOG(error) << "[vdd] display_base_t::init failed for "sv << display_name;
      return -1;
    }

    // Try to identify which VDD monitor backs this DXGI output by matching
    // dimensions. width/height come from DXGI DesktopCoordinates / orientation.
    // NOTE: We intentionally do NOT trigger CREATEMONITOR here. Sunshine's
    // display-device layer (prepare_vdd) already manages monitor lifecycle.
    // Producer discovery is limited to a bounded wait because the mode commit,
    // swap-chain replacement, and first shared frame are asynchronous.
    const auto target_w = static_cast<unsigned>(width_before_rotation);
    const auto target_h = static_cast<unsigned>(height_before_rotation);
    const auto frame_channel_mode =
      vdd_frame_channel_mode_env_override().value_or(vdd_frame_channel::channel_mode::auto_probe);
    const auto producer_discovery_deadline = std::chrono::steady_clock::now() + vdd_producer_discovery_window;

    vdd_frame_channel::producer_selection producer_selection;
    if (frame_channel_mode != vdd_frame_channel::channel_mode::legacy_named) {
      producer_selection = resolve_sealed_vdd_monitor_index(device.get(), target_w, target_h, 16, producer_discovery_deadline);
      if (producer_selection.match == vdd_frame_channel::producer_match::sole_mismatch) {
        BOOST_LOG(warning) << "[vdd] sealed producer resolution differs from the selected desktop; requesting DDX fallback"sv;
        return -1;
      }
      if (producer_selection.match == vdd_frame_channel::producer_match::unavailable &&
          frame_channel_mode == vdd_frame_channel::channel_mode::sealed_required) {
        BOOST_LOG(error) << "[vdd] SUNSHINE_VDD_FRAME_CHANNEL=sealed requested, "
                         << "but sealed producer discovery failed or found no matching producer for "sv
                         << target_w << "x"sv << target_h;
        return -1;
      }
      if (producer_selection.match == vdd_frame_channel::producer_match::unavailable) {
        BOOST_LOG(warning) << "[vdd] sealed producer discovery was unavailable or found no matching monitor; "
                           << "falling back to legacy Meta_* discovery"sv;
      }
    }

    if (producer_selection.match == vdd_frame_channel::producer_match::unavailable) {
      producer_selection = resolve_vdd_monitor_index(target_w, target_h, 16, producer_discovery_deadline);
    }
    if (producer_selection.match == vdd_frame_channel::producer_match::sole_mismatch) {
      BOOST_LOG(warning) << "[vdd] legacy producer resolution differs from the selected desktop; requesting DDX fallback"sv;
      return -1;
    }
    if (producer_selection.match == vdd_frame_channel::producer_match::unavailable) {
      return -1;
    }
    monitor_idx = static_cast<unsigned int>(producer_selection.index);

    for (;;) {
      if (dup.init(device.get(), monitor_idx) == 0) {
        break;
      }

      if (std::chrono::steady_clock::now() >= producer_discovery_deadline) {
        BOOST_LOG(error) << "[vdd] vdd_capture_t::init failed for monitor "sv << monitor_idx
                         << " before the producer discovery deadline; last_channel_selection="sv
                         << vdd_frame_channel::channel_selection_name(dup.frame_channel_selection());
        return -1;
      }

      BOOST_LOG(debug) << "[vdd] VDD capture attach is not stable yet; retrying monitor "sv
                       << monitor_idx;
      std::this_thread::sleep_for(vdd_producer_discovery_retry_delay);
    }

    // Use producer-reported format as our capture format. complete_img() / image
    // pool will be created against this format.
    capture_format = dup.format();
    capture_linear_gamma = capture_format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    vdd_borrow_enabled = config::video.vdd_borrowed_texture;
    if (auto env_override = vdd_borrowed_texture_env_override()) {
      vdd_borrow_enabled = *env_override;
    }
    vdd_borrow_cooldown_until = {};
    vdd_borrow_last_telemetry = {};
    vdd_borrow_attempts = 0;
    vdd_borrow_successes = 0;
    vdd_borrow_fallbacks = 0;
    vdd_borrow_disabled_frames = 0;
    vdd_borrow_cooldown_frames = 0;
    vdd_borrow_cooldown_events = 0;
    vdd_borrow_deferred_frames = 0;
    vdd_borrow_returned_deferred_frames = 0;
    vdd_borrow_inflight_frames = std::make_shared<std::atomic<UINT64>>(0);
    vdd_borrow_inflight_limit_frames = 0;
    vdd_borrow_deferred_images.clear();
    vdd_last_replaced_unread = dup.replaced_unread_frames();
    vdd_last_dropped_consumer_held = dup.dropped_consumer_held_frames();
    vdd_last_dropped_acquire_failures = dup.dropped_acquire_failures();

    // Validate the producer-reported format against the formats display_vram_t
    // can actually consume (RTV creation, shaders, color conversion). Anything
    // outside this whitelist would crash later in capture/encoding paths.
    {
      auto supported = get_supported_capture_formats();
      bool ok = false;
      for (auto f : supported) {
        if (f == capture_format) { ok = true; break; }
      }
      if (!ok) {
        BOOST_LOG(error) << "[vdd] producer format "sv
                         << dxgi_format_to_string(capture_format)
                         << " is not in display_vram_t::get_supported_capture_formats(); "sv
                         << "rejecting. Extending RTV/shader paths is required to add it."sv;
        return -1;
      }
    }

    BOOST_LOG(info) << "[vdd] backend ready: monitor="sv << monitor_idx
                    << " "sv << dup.width() << "x"sv << dup.height()
                    << " fmt="sv << dxgi_format_to_string(capture_format)
                    << " hdr="sv << dup.is_hdr()
                    << " linear_gamma="sv << capture_linear_gamma
                    << " mode="sv << vdd_frame_channel::channel_mode_name(dup.frame_channel_mode())
                    << " channel="sv << (dup.legacy_named_channel() ? "legacy_named_hardened" : "sealed")
                    << " selection="sv << vdd_frame_channel::channel_selection_name(dup.frame_channel_selection())
                    << " sealed_supported="sv << dup.sealed_frame_channel_supported()
                    << " producer_slots="sv << dup.producer_slot_count()
                    << " generation="sv << dup.producer_channel_generation()
                    << " borrowed_texture="sv << vdd_borrow_enabled;

    // Cursor export is optional and older drivers do not expose it. Likewise,
    // a cursor-only shader failure must not take down otherwise valid capture.
    if (init_cursor_pipeline(config) != 0) {
      cursor_pipeline_ready = false;
      BOOST_LOG(warning) << "[vdd] cursor pipeline init failed; continuing without cursor overlay"sv;
    }
    return 0;
  }

  bool
  display_vdd_vram_t::is_hdr() {
    return dup.is_hdr();
  }

  bool
  display_vdd_vram_t::get_hdr_metadata(SS_HDR_METADATA &metadata) {
    std::memset(&metadata, 0, sizeof(metadata));
    if (!dup.is_hdr()) {
      return false;
    }

    // Report Rec. 2020 primaries with D65 white point. Mirrors
    // display_base_t::get_hdr_metadata(): the actual primaries depend on
    // shader-side conversion (scRGB FP16 -> PQ in Rec. 2020), so reporting
    // 2020 is the safe / consistent choice. Most clients only consume the
    // luminance fields anyway.
    metadata.displayPrimaries[0].x = 0.708f * 50000;
    metadata.displayPrimaries[0].y = 0.292f * 50000;
    metadata.displayPrimaries[1].x = 0.170f * 50000;
    metadata.displayPrimaries[1].y = 0.797f * 50000;
    metadata.displayPrimaries[2].x = 0.131f * 50000;
    metadata.displayPrimaries[2].y = 0.046f * 50000;
    metadata.whitePoint.x = 0.3127f * 50000;
    metadata.whitePoint.y = 0.3290f * 50000;

    // Producer-reported luminance, in nits. SS_HDR_METADATA expects:
    //   maxDisplayLuminance      : nits
    //   minDisplayLuminance      : units of 0.0001 nits
    //   maxFullFrameLuminance    : nits
    auto finite_clamped = [](float value, float min_value, float max_value) {
      if (!std::isfinite(value)) {
        return min_value;
      }
      return std::clamp(value, min_value, max_value);
    };

    metadata.maxDisplayLuminance = static_cast<uint16_t>(finite_clamped(dup.max_nits(), 0.0f, 65535.0f));
    metadata.minDisplayLuminance = static_cast<uint32_t>(finite_clamped(dup.min_nits(), 0.0f, 429496.7295f) * 10000.0f);
    metadata.maxFullFrameLuminance = static_cast<uint16_t>(finite_clamped(dup.max_fall(), 0.0f, 65535.0f));

    // Producer doesn't currently track per-frame content light levels.
    metadata.maxContentLightLevel = 0;
    metadata.maxFrameAverageLightLevel = 0;
    return true;
  }

  // snapshot(), release_snapshot(), and borrowed-texture telemetry live in
  // display_vdd_vram.cpp. Shared D3D11 image details are declared in the
  // private display_vram_internal.h boundary.

}  // namespace platf::dxgi
