#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cstddef>
#include <optional>
#include <string_view>
#include <thread>

namespace platf::dxgi::vdd_frame_channel {

  // Must stay binary-compatible with the producer-side struct in
  // Virtual-Display-Driver/.../ZakoVDD/Driver.cpp (SharedFrameMetadata).
  struct shared_frame_metadata_t {
    UINT32 Magic;            // 'ZVDF' = 0x5A564446
    UINT32 Version;          // 1
    UINT32 Width;
    UINT32 Height;
    UINT32 DxgiFormat;
    UINT32 IsHdr;
    float MaxNits;
    float MinNits;
    float MaxFALL;
    UINT64 FrameCounter;
    UINT64 LastPresentQpc;
    UINT64 LastPublishQpc;
    UINT32 LastPresentationFrameNumber;
    UINT32 LastDirtyRectCount;
    UINT64 ReplacedUnreadFrames;
    UINT64 DroppedConsumerHeldFrames;
    UINT64 DroppedAcquireFailures;
    UINT32 MetadataSize;
    UINT32 SlotCount;
    UINT32 SlotIndex;
    // Packed control word for metadata consistency and object identity:
    //   bits 31..16: ChannelGeneration, incremented by the producer whenever
    //                it recreates the exported texture channel.
    //   bits 15..0 : MetadataSequence, a seqlock counter. Odd means the
    //                producer is updating this block; even means stable.
    //
    // This intentionally reuses the old Reserved0 slot, so MetadataSize stays
    // 128 bytes and older consumers keep reading the same prefix safely.
    UINT32 MetadataSequence;
    UINT32 AdapterLuidLowPart;
    INT32 AdapterLuidHighPart;
    UINT64 ProducerQpcFrequency;
  };

  static_assert(sizeof(shared_frame_metadata_t) == 128,
                "shared_frame_metadata_t must remain ABI-compatible with the VDD producer");
  static_assert(offsetof(shared_frame_metadata_t, MetadataSize) == 96,
                "MetadataSize offset is part of the shared metadata ABI");
  static_assert(offsetof(shared_frame_metadata_t, MetadataSequence) == 108,
                "MetadataSequence offset is part of the shared metadata ABI");
  static_assert(offsetof(shared_frame_metadata_t, ProducerQpcFrequency) == 120,
                "ProducerQpcFrequency offset is part of the shared metadata ABI");

  static constexpr UINT32 meta_magic = 0x5A564446;  // 'ZVDF'
  static constexpr UINT32 meta_version = 1;
  static constexpr UINT32 max_shared_slots = 8;
  static constexpr UINT32 min_metadata_size =
    static_cast<UINT32>(offsetof(shared_frame_metadata_t, ProducerQpcFrequency) + sizeof(UINT64));
  static constexpr DWORD consumer_acquire_wait_budget_ms = 2;
  static constexpr UINT32 metadata_sequence_write_bit = 0x00000001u;

  enum class reject_reason {
    none,
    bad_magic,
    version_mismatch,
    metadata_too_small,
    invalid_dimensions,
    unsupported_format,
    invalid_slot_count,
    slot_index_out_of_range,
    zero_adapter_luid,
    adapter_luid_mismatch,
    texture_desc_mismatch,
    texture_layout_unsupported,
    texture_missing_keyed_mutex,
    texture_missing_shader_resource,
  };

  enum class channel_mode {
    auto_probe,
    legacy_named,
    sealed_required,
  };

  enum class channel_selection {
    unknown,
    sealed_opened,
    forced_legacy,
    caps_unsupported,
    caps_failed,
    open_unsupported,
    open_not_ready,
    open_failed,
    attach_failed,
    sealed_required_failed,
    legacy_opened,
  };

  struct validation_result {
    bool ok = false;
    reject_reason reason = reject_reason::none;

    explicit operator bool() const {
      return ok;
    }
  };

  enum class producer_match {
    unavailable,
    exact,
    sole_mismatch,
  };

  struct producer_selection {
    int index = -1;
    producer_match match = producer_match::unavailable;
  };

  enum class pending_cursor_action {
    wait_for_events,
    retry_cursor_frame,
  };

  inline pending_cursor_action
  select_pending_cursor_action(bool cursor_update_pending,
                               bool desktop_frame_ready) {
    return cursor_update_pending && !desktop_frame_ready ?
             pending_cursor_action::retry_cursor_frame :
             pending_cursor_action::wait_for_events;
  }

  inline producer_selection
  select_producer(int exact_index, int only_valid_index, unsigned int valid_count) {
    if (exact_index >= 0) {
      return {exact_index, producer_match::exact};
    }
    if (valid_count == 1 && only_valid_index >= 0) {
      return {only_valid_index, producer_match::sole_mismatch};
    }
    return {};
  }

  inline bool
  luid_equal(const LUID &a, const LUID &b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
  }

  inline bool
  luid_is_zero(const LUID &luid) {
    return luid.LowPart == 0 && luid.HighPart == 0;
  }

  inline bool
  supported_producer_format(DXGI_FORMAT format) {
    switch (format) {
      case DXGI_FORMAT_R16G16B16A16_FLOAT:
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_B8G8R8X8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM:
        return true;
      default:
        return false;
    }
  }

  inline std::optional<channel_mode>
  parse_channel_mode(std::string_view value) {
    if (value == "auto") {
      return channel_mode::auto_probe;
    }
    if (value == "legacy" || value == "legacy_named" || value == "named") {
      return channel_mode::legacy_named;
    }
    if (value == "sealed" || value == "sealed_required") {
      return channel_mode::sealed_required;
    }
    return std::nullopt;
  }

  inline const char *
  channel_mode_name(channel_mode mode) {
    switch (mode) {
      case channel_mode::auto_probe:
        return "auto";
      case channel_mode::legacy_named:
        return "legacy_named";
      case channel_mode::sealed_required:
        return "sealed_required";
    }
    return "unknown";
  }

  inline const char *
  channel_selection_name(channel_selection selection) {
    switch (selection) {
      case channel_selection::unknown:
        return "unknown";
      case channel_selection::sealed_opened:
        return "sealed_opened";
      case channel_selection::forced_legacy:
        return "forced_legacy";
      case channel_selection::caps_unsupported:
        return "caps_unsupported";
      case channel_selection::caps_failed:
        return "caps_failed";
      case channel_selection::open_unsupported:
        return "open_unsupported";
      case channel_selection::open_not_ready:
        return "open_not_ready";
      case channel_selection::open_failed:
        return "open_failed";
      case channel_selection::attach_failed:
        return "attach_failed";
      case channel_selection::sealed_required_failed:
        return "sealed_required_failed";
      case channel_selection::legacy_opened:
        return "legacy_opened";
    }
    return "unknown";
  }

  inline DWORD
  bounded_consumer_acquire_timeout_ms(DWORD frame_wait_timeout_ms) {
    return frame_wait_timeout_ms < consumer_acquire_wait_budget_ms ?
             frame_wait_timeout_ms :
             consumer_acquire_wait_budget_ms;
  }

  inline UINT16
  metadata_sequence_counter(UINT32 metadata_sequence) {
    return static_cast<UINT16>(metadata_sequence & 0xFFFFu);
  }

  inline UINT16
  metadata_channel_generation(UINT32 metadata_sequence) {
    return static_cast<UINT16>(metadata_sequence >> 16);
  }

  inline bool
  metadata_sequence_is_stable(UINT32 metadata_sequence) {
    return (metadata_sequence_counter(metadata_sequence) & metadata_sequence_write_bit) == 0;
  }

  // Producer state the consumer latched when it attached to the channel. Every
  // wake-up path must compare a fresh metadata snapshot against this, otherwise
  // a producer reconfigure stays invisible and the consumer keeps re-reading an
  // orphaned slot forever.
  struct producer_channel_identity {
    UINT16 generation = 0;
    UINT32 width = 0;
    UINT32 height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  };

  enum class producer_channel_change {
    none,
    generation,
    resolution_or_format,
  };

  inline producer_channel_change
  detect_producer_channel_change(const shared_frame_metadata_t &meta,
                                 const producer_channel_identity &known) {
    if (metadata_channel_generation(meta.MetadataSequence) != known.generation) {
      return producer_channel_change::generation;
    }
    if (meta.Width != known.width || meta.Height != known.height ||
        static_cast<DXGI_FORMAT>(meta.DxgiFormat) != known.format) {
      return producer_channel_change::resolution_or_format;
    }
    return producer_channel_change::none;
  }

  inline bool
  read_stable_metadata(const shared_frame_metadata_t *source,
                       shared_frame_metadata_t &snapshot,
                       unsigned attempts = 6) {
    if (!source) {
      return false;
    }

    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
      const UINT32 seq_before = source->MetadataSequence;
      if (!metadata_sequence_is_stable(seq_before)) {
        std::this_thread::yield();
        continue;
      }

      snapshot = *source;
      const UINT32 seq_after = source->MetadataSequence;
      if (seq_before == seq_after && metadata_sequence_is_stable(seq_after)) {
        return true;
      }

      std::this_thread::yield();
    }

    return false;
  }

  inline validation_result
  validate_metadata_for_probe(const shared_frame_metadata_t &meta) {
    if (meta.Magic != meta_magic) {
      return {false, reject_reason::bad_magic};
    }
    if (meta.Version != meta_version) {
      return {false, reject_reason::version_mismatch};
    }
    if (meta.MetadataSize < min_metadata_size) {
      return {false, reject_reason::metadata_too_small};
    }
    if (meta.Width == 0 || meta.Height == 0 ||
        meta.Width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        meta.Height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
      return {false, reject_reason::invalid_dimensions};
    }
    if (!supported_producer_format(static_cast<DXGI_FORMAT>(meta.DxgiFormat))) {
      return {false, reject_reason::unsupported_format};
    }
    if (meta.SlotCount == 0 || meta.SlotCount > max_shared_slots) {
      return {false, reject_reason::invalid_slot_count};
    }
    if (meta.SlotIndex >= meta.SlotCount) {
      return {false, reject_reason::slot_index_out_of_range};
    }
    return {true, reject_reason::none};
  }

  inline validation_result
  validate_metadata_for_attach(const shared_frame_metadata_t &meta, const LUID &device_luid) {
    auto result = validate_metadata_for_probe(meta);
    if (!result) {
      return result;
    }

    LUID producer_luid {};
    producer_luid.LowPart = meta.AdapterLuidLowPart;
    producer_luid.HighPart = meta.AdapterLuidHighPart;
    if (luid_is_zero(producer_luid)) {
      return {false, reject_reason::zero_adapter_luid};
    }
    if (!luid_equal(producer_luid, device_luid)) {
      return {false, reject_reason::adapter_luid_mismatch};
    }
    return {true, reject_reason::none};
  }

  inline validation_result
  validate_texture_desc(const D3D11_TEXTURE2D_DESC &desc,
                        const shared_frame_metadata_t &meta) {
    if (desc.Width != meta.Width || desc.Height != meta.Height ||
        desc.Format != static_cast<DXGI_FORMAT>(meta.DxgiFormat)) {
      return {false, reject_reason::texture_desc_mismatch};
    }
    if (desc.ArraySize != 1 || desc.MipLevels != 1 ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0) {
      return {false, reject_reason::texture_layout_unsupported};
    }
    if ((desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) == 0) {
      return {false, reject_reason::texture_missing_keyed_mutex};
    }
    if ((desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
      return {false, reject_reason::texture_missing_shader_resource};
    }
    return {true, reject_reason::none};
  }

}  // namespace platf::dxgi::vdd_frame_channel
