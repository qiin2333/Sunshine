/**
 * @file src/platform/windows/display.h
 * @brief Declarations for the Windows display backend.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcommon.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <dxgi1_6.h>

#include <Unknwn.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <AMF/core/Factory.h>
#include <AMF/core/CurrentTime.h>

#include "src/platform/common.h"
#include "src/platform/windows/frame_contract.h"
#include "src/utility.h"
#include "src/video.h"
#include "vdd_frame_channel.h"

namespace display_device::vdd_ioctl {
  struct frame_channel_caps;
  struct frame_channel_open_response;
}

namespace platf::dxgi {
  extern const char *format_str[];

  // Add D3D11_CREATE_DEVICE_DEBUG here to enable the D3D11 debug runtime.
  // You should have a debugger like WinDbg attached to receive debug messages.
  auto constexpr D3D11_CREATE_DEVICE_FLAGS = 0;

  template <class T>
  void
  Release(T *dxgi) {
    dxgi->Release();
  }

  inline
  void
  FreeLibraryHelper(void *item) {
    FreeLibrary((HMODULE) item);
  }

  using hmodule_t = util::safe_ptr<void, FreeLibraryHelper>;
  using factory1_t = util::safe_ptr<IDXGIFactory1, Release<IDXGIFactory1>>;
  using dxgi_t = util::safe_ptr<IDXGIDevice, Release<IDXGIDevice>>;
  using dxgi1_t = util::safe_ptr<IDXGIDevice1, Release<IDXGIDevice1>>;
  using device_t = util::safe_ptr<ID3D11Device, Release<ID3D11Device>>;
  using device1_t = util::safe_ptr<ID3D11Device1, Release<ID3D11Device1>>;
  using device_ctx_t = util::safe_ptr<ID3D11DeviceContext, Release<ID3D11DeviceContext>>;
  using adapter_t = util::safe_ptr<IDXGIAdapter1, Release<IDXGIAdapter1>>;
  using output_t = util::safe_ptr<IDXGIOutput, Release<IDXGIOutput>>;
  using output1_t = util::safe_ptr<IDXGIOutput1, Release<IDXGIOutput1>>;
  using output5_t = util::safe_ptr<IDXGIOutput5, Release<IDXGIOutput5>>;
  using output6_t = util::safe_ptr<IDXGIOutput6, Release<IDXGIOutput6>>;
  using dup_t = util::safe_ptr<IDXGIOutputDuplication, Release<IDXGIOutputDuplication>>;
  using texture2d_t = util::safe_ptr<ID3D11Texture2D, Release<ID3D11Texture2D>>;
  using texture1d_t = util::safe_ptr<ID3D11Texture1D, Release<ID3D11Texture1D>>;
  using resource_t = util::safe_ptr<IDXGIResource, Release<IDXGIResource>>;
  using resource1_t = util::safe_ptr<IDXGIResource1, Release<IDXGIResource1>>;
  using multithread_t = util::safe_ptr<ID3D11Multithread, Release<ID3D11Multithread>>;
  using vs_t = util::safe_ptr<ID3D11VertexShader, Release<ID3D11VertexShader>>;
  using ps_t = util::safe_ptr<ID3D11PixelShader, Release<ID3D11PixelShader>>;
  using cs_t = util::safe_ptr<ID3D11ComputeShader, Release<ID3D11ComputeShader>>;
  using blend_t = util::safe_ptr<ID3D11BlendState, Release<ID3D11BlendState>>;
  using input_layout_t = util::safe_ptr<ID3D11InputLayout, Release<ID3D11InputLayout>>;
  using render_target_t = util::safe_ptr<ID3D11RenderTargetView, Release<ID3D11RenderTargetView>>;
  using shader_res_t = util::safe_ptr<ID3D11ShaderResourceView, Release<ID3D11ShaderResourceView>>;
  using uav_t = util::safe_ptr<ID3D11UnorderedAccessView, Release<ID3D11UnorderedAccessView>>;
  using buf_t = util::safe_ptr<ID3D11Buffer, Release<ID3D11Buffer>>;
  using raster_state_t = util::safe_ptr<ID3D11RasterizerState, Release<ID3D11RasterizerState>>;
  using sampler_state_t = util::safe_ptr<ID3D11SamplerState, Release<ID3D11SamplerState>>;
  using blob_t = util::safe_ptr<ID3DBlob, Release<ID3DBlob>>;
  using depth_stencil_state_t = util::safe_ptr<ID3D11DepthStencilState, Release<ID3D11DepthStencilState>>;
  using depth_stencil_view_t = util::safe_ptr<ID3D11DepthStencilView, Release<ID3D11DepthStencilView>>;
  using keyed_mutex_t = util::safe_ptr<IDXGIKeyedMutex, Release<IDXGIKeyedMutex>>;

  namespace video {
    using device_t = util::safe_ptr<ID3D11VideoDevice, Release<ID3D11VideoDevice>>;
    using ctx_t = util::safe_ptr<ID3D11VideoContext, Release<ID3D11VideoContext>>;
    using processor_t = util::safe_ptr<ID3D11VideoProcessor, Release<ID3D11VideoProcessor>>;
    using processor_out_t = util::safe_ptr<ID3D11VideoProcessorOutputView, Release<ID3D11VideoProcessorOutputView>>;
    using processor_in_t = util::safe_ptr<ID3D11VideoProcessorInputView, Release<ID3D11VideoProcessorInputView>>;
    using processor_enum_t = util::safe_ptr<ID3D11VideoProcessorEnumerator, Release<ID3D11VideoProcessorEnumerator>>;
  }  // namespace video

  class hwdevice_t;
  struct cursor_t {
    std::vector<std::uint8_t> img_data;

    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info {};
    int x = 0;
    int y = 0;
    bool visible = false;
    std::uint32_t shape_id = 0;
  };

  class gpu_cursor_t {
  public:
    gpu_cursor_t():
        cursor_view { 0, 0, 0, 0, 0.0f, 1.0f } {};

    void
    set_pos(LONG topleft_x, LONG topleft_y, LONG display_width, LONG display_height, DXGI_MODE_ROTATION display_rotation, bool visible) {
      this->topleft_x = topleft_x;
      this->topleft_y = topleft_y;
      this->display_width = display_width;
      this->display_height = display_height;
      this->display_rotation = display_rotation;
      this->visible = visible;
      update_viewport();
    }

    void
    set_texture(LONG texture_width, LONG texture_height, texture2d_t &&texture) {
      this->texture = std::move(texture);
      this->texture_width = texture_width;
      this->texture_height = texture_height;
      update_viewport();
    }

    void
    update_viewport() {
      switch (display_rotation) {
        case DXGI_MODE_ROTATION_UNSPECIFIED:
        case DXGI_MODE_ROTATION_IDENTITY:
          cursor_view.TopLeftX = topleft_x;
          cursor_view.TopLeftY = topleft_y;
          cursor_view.Width = texture_width;
          cursor_view.Height = texture_height;
          break;

        case DXGI_MODE_ROTATION_ROTATE90:
          cursor_view.TopLeftX = topleft_y;
          cursor_view.TopLeftY = display_width - texture_width - topleft_x;
          cursor_view.Width = texture_height;
          cursor_view.Height = texture_width;
          break;

        case DXGI_MODE_ROTATION_ROTATE180:
          cursor_view.TopLeftX = display_width - texture_width - topleft_x;
          cursor_view.TopLeftY = display_height - texture_height - topleft_y;
          cursor_view.Width = texture_width;
          cursor_view.Height = texture_height;
          break;

        case DXGI_MODE_ROTATION_ROTATE270:
          cursor_view.TopLeftX = display_height - texture_height - topleft_y;
          cursor_view.TopLeftY = topleft_x;
          cursor_view.Width = texture_height;
          cursor_view.Height = texture_width;
          break;
      }
    }

    texture2d_t texture;
    LONG texture_width;
    LONG texture_height;

    LONG topleft_x;
    LONG topleft_y;

    LONG display_width;
    LONG display_height;
    DXGI_MODE_ROTATION display_rotation;

    shader_res_t input_res;

    D3D11_VIEWPORT cursor_view;

    bool visible;
  };

  class display_base_t: public display_t {
  public:
    int
    init(const ::video::config_t &config, const std::string &display_name);

    capture_e
    capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override;

    factory1_t factory;
    adapter_t adapter;
    output_t output;
    device_t device;
    device_ctx_t device_ctx;
    DXGI_RATIONAL display_refresh_rate;
    int display_refresh_rate_rounded;

    DXGI_MODE_ROTATION display_rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
    int width_before_rotation;
    int height_before_rotation;

    int client_frame_rate;  // Integer framerate for backward compatibility
    DXGI_RATIONAL client_frame_rate_rational;  // Fractional framerate for NTSC support (e.g., 60000/1001 = 59.94fps)
    int adapter_index;
    int output_index;

    DXGI_FORMAT capture_format;
    capture_contract_t capture_contract;

    captured_frame_desc_t
    describe_captured_frame(DXGI_FORMAT format, bool borrowed) const;

    /**
     * @brief Indicates whether the display's output colorspace uses linear gamma.
     *
     * This is determined from DXGI_OUTPUT_DESC1.ColorSpace:
     *   - G10 (gamma 1.0, linear):  capture_linear_gamma = true   (ACM / scRGB)
     *   - G22 (gamma ~2.2, sRGB):   capture_linear_gamma = false  (normal SDR)
     *   - G2084 (PQ / HDR):         capture_linear_gamma = true   (linear light)
     *
     * Shader selection requires BOTH linear_gamma AND FP16 pixel format to use the
     * linear-input shader (ApplySRGBCurve). This is because:
     *   - FP16 + G10/G2084: data is truly in linear light → must apply transfer function
     *   - B8G8R8A8 + G10:   data was converted to sRGB by the capture API (e.g. WGC
     *     requesting 8-bit while display is in ACM mode) → already has sRGB gamma
     *   - FP16 + G22:       driver returned FP16 data with sRGB gamma → identity shader
     */
    bool capture_linear_gamma = false;

    D3D_FEATURE_LEVEL feature_level;

    std::unique_ptr<high_precision_timer> timer = create_high_precision_timer();

    typedef enum _D3DKMT_SCHEDULINGPRIORITYCLASS {
      D3DKMT_SCHEDULINGPRIORITYCLASS_IDLE,  ///< Idle priority class
      D3DKMT_SCHEDULINGPRIORITYCLASS_BELOW_NORMAL,  ///< Below normal priority class
      D3DKMT_SCHEDULINGPRIORITYCLASS_NORMAL,  ///< Normal priority class
      D3DKMT_SCHEDULINGPRIORITYCLASS_ABOVE_NORMAL,  ///< Above normal priority class
      D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH,  ///< High priority class
      D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME  ///< Realtime priority class
    } D3DKMT_SCHEDULINGPRIORITYCLASS;

    typedef UINT D3DKMT_HANDLE;

    typedef struct _D3DKMT_OPENADAPTERFROMLUID {
      LUID AdapterLuid;
      D3DKMT_HANDLE hAdapter;
    } D3DKMT_OPENADAPTERFROMLUID;

    typedef struct _D3DKMT_WDDM_2_7_CAPS {
      union {
        struct
        {
          UINT HwSchSupported : 1;
          UINT HwSchEnabled : 1;
          UINT HwSchEnabledByDefault : 1;
          UINT IndependentVidPnVSyncControl : 1;
          UINT Reserved : 28;
        };
        UINT Value;
      };
    } D3DKMT_WDDM_2_7_CAPS;

    typedef struct _D3DKMT_QUERYADAPTERINFO {
      D3DKMT_HANDLE hAdapter;
      UINT Type;
      VOID *pPrivateDriverData;
      UINT PrivateDriverDataSize;
    } D3DKMT_QUERYADAPTERINFO;

    const UINT KMTQAITYPE_WDDM_2_7_CAPS = 70;

    typedef struct _D3DKMT_CLOSEADAPTER {
      D3DKMT_HANDLE hAdapter;
    } D3DKMT_CLOSEADAPTER;

    typedef NTSTATUS(WINAPI *PD3DKMTSetProcessSchedulingPriorityClass)(HANDLE, D3DKMT_SCHEDULINGPRIORITYCLASS);
    typedef NTSTATUS(WINAPI *PD3DKMTOpenAdapterFromLuid)(D3DKMT_OPENADAPTERFROMLUID *);
    typedef NTSTATUS(WINAPI *PD3DKMTQueryAdapterInfo)(D3DKMT_QUERYADAPTERINFO *);
    typedef NTSTATUS(WINAPI *PD3DKMTCloseAdapter)(D3DKMT_CLOSEADAPTER *);

    virtual bool
    is_hdr() override;
    virtual bool
    get_hdr_metadata(SS_HDR_METADATA &metadata) override;

    /**
     * @brief Current Windows SDR reference white for this DXGI output.
     *        The DisplayConfig query is cached for one second.
     */
    std::optional<float>
    sdr_white_nits() const;

    const char *
    dxgi_format_to_string(DXGI_FORMAT format);
    const char *
    colorspace_to_string(DXGI_COLOR_SPACE_TYPE type);
    virtual std::vector<DXGI_FORMAT>
    get_supported_capture_formats() = 0;

  private:
    std::wstring display_device_name;

    // Cached HDR metadata for change detection
    std::optional<SS_HDR_METADATA> cached_hdr_metadata;
    std::chrono::steady_clock::time_point last_hdr_check_time;
    static constexpr std::chrono::milliseconds hdr_check_interval { 1000 };  // Check every 1 second

    mutable std::optional<float> cached_sdr_white_nits;
    mutable std::chrono::steady_clock::time_point last_sdr_white_check_time;
    static constexpr std::chrono::milliseconds sdr_white_check_interval { 1000 };

  protected:
    std::uint64_t capture_adapter_luid = 0;
    std::uint64_t capture_source_generation = 0;

    int
    get_pixel_pitch() {
      return (capture_format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 : 4;
    }

    virtual capture_e
    snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) = 0;
    virtual capture_e
    release_snapshot() = 0;
    virtual int
    complete_img(img_t *img, bool dummy) = 0;
  };

  /**
   * Display component for devices that use software encoders.
   */
  class display_ram_t: public display_base_t {
  public:
    std::shared_ptr<img_t>
    alloc_img() override;
    int
    dummy_img(img_t *img) override;
    int
    complete_img(img_t *img, bool dummy) override;
    std::vector<DXGI_FORMAT>
    get_supported_capture_formats() override;

    std::unique_ptr<avcodec_encode_device_t>
    make_avcodec_encode_device(pix_fmt_e pix_fmt) override;

    D3D11_MAPPED_SUBRESOURCE img_info;
    texture2d_t texture;
  };

  /**
   * Display component for devices that use hardware encoders.
   */
  class display_vram_t: public display_base_t, public std::enable_shared_from_this<display_vram_t> {
  public:
    std::shared_ptr<img_t>
    alloc_img() override;
    int
    dummy_img(img_t *img_base) override;
    int
    complete_img(img_t *img_base, bool dummy) override;
    std::vector<DXGI_FORMAT>
    get_supported_capture_formats() override;

    bool
    is_codec_supported(std::string_view name, const ::video::config_t &config) override;

    // Only the D3D11 VRAM capture path constructs the pre-encode filter.
    bool
    supports_pre_encode_filter() override {
      return true;
    }

    std::unique_ptr<avcodec_encode_device_t>
    make_avcodec_encode_device(pix_fmt_e pix_fmt) override;

    std::unique_ptr<nvenc_encode_device_t>
    make_nvenc_encode_device(pix_fmt_e pix_fmt) override;

    std::unique_ptr<amf_encode_device_t>
    make_amf_encode_device(pix_fmt_e pix_fmt) override;

    std::unique_ptr<avcodec_encode_device_t>
    make_avcodec_encode_device(pix_fmt_e pix_fmt, const ::video::config_t &config) override;

    std::unique_ptr<nvenc_encode_device_t>
    make_nvenc_encode_device(pix_fmt_e pix_fmt, const ::video::config_t &config) override;

    std::unique_ptr<amf_encode_device_t>
    make_amf_encode_device(pix_fmt_e pix_fmt, const ::video::config_t &config) override;

    /**
     * @brief Current captured SDR white level in nits. Prefer producer metadata
     *        when VDD supplies it, otherwise query the selected Windows output.
     *        The legacy 300-nit value is used only if neither source is available.
     */
    std::optional<float>
    capture_sdr_white_nits() const;

    std::atomic<uint32_t> next_image_id;

  protected:
    // Shared cursor blending pipeline used by display backends that need to
    // composite a software cursor on top of a captured frame (DXGI Desktop
    // Duplication, VDD direct-capture). AMD/WGC paths use different schemes
    // and shadow these in their own subclasses where applicable.
    sampler_state_t sampler_linear;
    sampler_state_t sampler_point;

    blend_t blend_alpha;
    blend_t blend_invert;
    blend_t blend_disable;

    ps_t cursor_ps;
    vs_t cursor_vs;
    buf_t cursor_white_multiplier;

    gpu_cursor_t cursor_alpha;
    gpu_cursor_t cursor_xor;
    // Written from the capture/cursor thread and used by cursor rendering.
    std::atomic<float> cursor_white_multiplier_value { 300.0f / 80.0f };
    // Producer-reported capture white is independent of cursor rendering. Keep
    // it separate so a cursor constant-buffer failure cannot corrupt encoding.
    std::atomic<float> producer_sdr_white_nits { 0.0f };
    bool cursor_white_normalization_enabled = false;
    bool cursor_pipeline_ready = false;

    /**
     * @brief Create the cursor blend shaders, blend states, samplers, and
     *        rotation constant buffer. Caller is responsible for `device` /
     *        `device_ctx` being valid (typically called from a subclass
     *        `init()` after `display_base_t::init()` succeeds).
     * @return 0 on success, -1 on any failure (errors already logged).
     */
    int
    init_cursor_pipeline(const ::video::config_t &config);

    /**
     * @brief Update the HDR cursor luminance from the VDD driver's SDR white
     *        level. A zero value keeps the legacy 300-nit fallback.
     */
    void
    set_cursor_sdr_white_level(UINT32 sdr_white_level_x1000);

    /**
     * @brief Draw the currently-configured cursor_alpha / cursor_xor onto the
     *        given render target. Caller must hold the capture mutex for the
     *        underlying image. After the draw the blend state is reset to
     *        `blend_disable` and the RTV/SRV slots are cleared.
     */
    void
    blend_cursor(ID3D11RenderTargetView *capture_rt);
  };

  /**
   * Display duplicator that uses the DirectX Desktop Duplication API.
   */
  class duplication_t {
  public:
    dup_t dup;
    bool has_frame {};
    bool local_cursor_mode_was_active {};
    cursor_t cursor;
    std::chrono::steady_clock::time_point last_protected_content_warning_time {};

    int
    init(display_base_t *display, const ::video::config_t &config);
    capture_e
    next_frame(DXGI_OUTDUPL_FRAME_INFO &frame_info, std::chrono::milliseconds timeout, resource_t::pointer *res_p);
    capture_e
    update_cursor(const DXGI_OUTDUPL_FRAME_INFO &frame_info, bool &shape_updated);
    capture_e
    reset(dup_t::pointer dup_p = dup_t::pointer());
    capture_e
    release_frame();

    ~duplication_t();
  };

  /**
   * Display backend that uses DDAPI with a software encoder.
   */
  class display_ddup_ram_t: public display_ram_t {
  public:
    int
    init(const ::video::config_t &config, const std::string &display_name);
    capture_e
    snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) override;
    capture_e
    release_snapshot() override;

    duplication_t dup;
  };

  /**
   * Display backend that uses DDAPI with a hardware encoder.
   */
  class display_ddup_vram_t: public display_vram_t {
  public:
    int
    init(const ::video::config_t &config, const std::string &display_name);
    capture_e
    snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) override;
    capture_e
    release_snapshot() override;

    duplication_t dup;

    texture2d_t old_surface_delayed_destruction;
    std::chrono::steady_clock::time_point old_surface_timestamp;
    std::variant<std::monostate, texture2d_t, std::shared_ptr<platf::img_t>> last_frame_variant;
  };

  /**
   * @brief Recover ConsentPromptBehaviorAdmin registry value if a previous WGC session crashed.
   * Safe to call from any capture mode — checks for backup file and restores if found.
   */
  void
  recover_secure_desktop();

  /**
   * Display duplicator that uses the Windows.Graphics.Capture API.
   */
  class wgc_capture_t {
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice uwp_device { nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item { nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool { nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession capture_session { nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame produced_frame { nullptr }, consumed_frame { nullptr };
    SRWLOCK frame_lock = SRWLOCK_INIT;
    CONDITION_VARIABLE frame_present_cv;
    void
    on_frame_arrived(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const &sender, winrt::Windows::Foundation::IInspectable const &);

  public:
    HWND captured_window_hwnd { nullptr };  // Store window handle if capturing a window
    std::string desired_window_title;  // Store desired window title (for logging/debugging)
    int window_capture_width { 0 };  // Actual window capture width (may differ from display width)
    int window_capture_height { 0 };  // Actual window capture height (may differ from display height)
    wgc_capture_t();
    ~wgc_capture_t();

    int
    init(display_base_t *display, const ::video::config_t &config);
    capture_e
    next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out, uint64_t &out_time);
    capture_e
    release_frame();
    int
    set_cursor_visible(bool);
    
    /**
     * @brief Check if the captured window is still valid.
     * @return true if window is valid or not capturing a window, false if window is invalid.
     */
    bool
    is_window_valid() const;
  };

  /**
   * Display backend that uses Windows.Graphics.Capture with a software encoder.
   */
  class display_wgc_ram_t: public display_ram_t {
    wgc_capture_t dup;

  public:
    int
    init(const ::video::config_t &config, const std::string &display_name);
    std::shared_ptr<img_t>
    alloc_img() override;
    capture_e
    snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) override;
    capture_e
    release_snapshot() override;
  };

  /**
   * Display backend that uses Windows.Graphics.Capture with a hardware encoder.
   */
  class display_wgc_vram_t: public display_vram_t {
    wgc_capture_t dup;

  public:
    int
    init(const ::video::config_t &config, const std::string &display_name);
    std::shared_ptr<img_t>
    alloc_img() override;
    capture_e
    snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) override;
    capture_e
    release_snapshot() override;
  };

  class amd_capture_t {

  public:
    amd_capture_t();
    ~amd_capture_t();

    int
    init(display_base_t *display, const ::video::config_t &config, int output_index);
    capture_e
    next_frame(std::chrono::milliseconds timeout, amf::AMFData** out);
    capture_e
    release_frame();

    hmodule_t amfrt_lib;
    amf_uint64 amf_version;
    amf::AMFFactory *amf_factory;

    amf::AMFContextPtr context;
    amf::AMFComponentPtr captureComp;
    amf::AMFSurfacePtr capturedSurface;
    amf_int64 capture_format;
    AMFSize resolution;
  };


  /**
   * Display backend that uses AMD Display Capture with a hardware encoder.
   * Main purpose is to capture AMD Fluid Motion Frames (AFMF)
   */
  class display_amd_vram_t: public display_vram_t {
    amd_capture_t dup;

    blend_t blend_invert;
    blend_t blend_disable;

    ps_t cursor_ps;
    vs_t cursor_vs;

    buf_t cursor_info;

  public:
    int
    init(const ::video::config_t &config, const std::string &display_name);
    capture_e
    snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) override;
    capture_e
    release_snapshot() override;
  };

  /**
   * VDD direct-capture: opens the named D3D11 shared texture exported by
   * the ZakoVDD driver (SharedFrameExporter). This bypasses DXGI Desktop
   * Duplication / WGC and works in SYSTEM service context, before user logon,
   * across session switch, and with full HDR — at the cost of only being able
   * to capture VDD virtual monitors (not physical displays).
   */
  class vdd_capture_t {
  public:
    vdd_capture_t();
    ~vdd_capture_t();

    /**
     * @brief Open the named shared texture / event / metadata for the given
     *        VDD monitor index. Must be called from the same D3D11 device
     *        that will be used for downstream encoding.
     * @param d3d_device  D3D11 device on the same adapter LUID as VDD's RenderAdapter.
     * @param monitor_idx VDD-internal monitor index (0..N-1).
     */
    int
    init(ID3D11Device *d3d_device, unsigned int monitor_idx);

    /**
     * @brief Wait for the next frame (event-driven, no polling).
     * @param timeout         Maximum time to wait.
     * @param out             Receives a Texture2D reference holding the new frame.
     *                        The caller MUST call release_frame() before next_frame()
     *                        to release the keyed mutex.
     * @param out_frame_qpc   QPC value at producer-side push (for latency tracing).
     * @param wait_for_cursor Include the optional cursor-ready event in the wait.
     * @param out_cursor_only True when the wakeup contains cursor state but no
     *                        new desktop frame. `out` then references the most
     *                        recent producer texture reacquired as key 0.
     */
    capture_e
    next_frame(std::chrono::milliseconds timeout,
               ID3D11Texture2D **out,
               uint64_t &out_frame_qpc,
               bool wait_for_cursor,
               bool &out_cursor_only);

    /**
     * @brief Hand the current keyed-mutex slot to a downstream D3D11 consumer.
     *        The producer released the slot as key 1 and next_frame() acquired it.
     *        This releases the slot as release_key instead of key 0 so the encoder
     *        can acquire it on its own device, then release key 0 back to producer.
     */
    capture_e
    handoff_frame(UINT64 release_key, IDXGIKeyedMutex **out_mutex, UINT32 &out_slot);

    /**
     * @brief Release the current keyed-mutex hold so the producer can write again.
     */
    capture_e
    release_frame();

    /**
     * @brief One snapshot of the producer-published hardware cursor state.
     * Mirrors the layout of `CursorSharedMetadata` in ZakoVDD's Driver.cpp.
     * `shape_buffer` is owned by the snapshot (copied out of SHM).
     */
    struct cursor_snapshot {
      bool     valid = false;          ///< True if at least one publish observed.
      bool     visible = false;
      bool     shape_updated = false;  ///< True iff shape_id changed since last poll.
      bool     position_updated = false;
      INT32    x = 0;                  ///< Top-left of cursor image, desktop-relative.
      INT32    y = 0;
      UINT32   position_id = 0;
      UINT32   shape_id = 0;
      UINT32   shape_type = 0;         ///< IDDCX_CURSOR_SHAPE_TYPE value (0=mono, 1=color, 2=masked color).
      UINT32   width = 0;
      UINT32   height = 0;
      UINT32   pitch = 0;
      INT32    xhot = 0;
      INT32    yhot = 0;
      UINT32   sdr_white_level_x1000 = 0;
      UINT64   update_qpc = 0;
      std::vector<uint8_t> shape_buffer;  ///< Empty if !shape_updated or shape is uninitialized.
    };

    /**
     * @brief Non-blocking poll of the latest cursor state published by the
     *        driver-side CursorExporter. Returns false if cursor SHM is not
     *        attached or nothing has been published yet.
     */
    bool
    poll_cursor(cursor_snapshot &out);

    /**
     * @brief Mark a cursor shape as consumed only after both GPU texture
     *        uploads have succeeded. Until then poll_cursor() retries it.
     */
    void
    acknowledge_cursor_shape(UINT32 shape_id);

    /**
     * @brief Force the next poll to include the current cursor shape.
     */
    void
    invalidate_cursor_shape();

    /**
     * @brief Reported producer-side dimensions / format / HDR metadata.
     */
    UINT  width()      const { return m_width; }
    UINT  height()     const { return m_height; }
    DXGI_FORMAT format() const { return m_format; }
    bool  is_hdr()     const { return m_is_hdr; }
    float max_nits()   const { return m_max_nits; }
    float min_nits()   const { return m_min_nits; }
    float max_fall()   const { return m_max_fall; }
    UINT64 frame_counter() const { return m_lastFrameCounter; }
    UINT64 last_present_qpc() const { return m_lastPresentQpc; }
    UINT64 last_publish_qpc() const { return m_lastPublishQpc; }
    UINT32 last_presentation_frame_number() const { return m_lastPresentationFrameNumber; }
    UINT32 last_dirty_rect_count() const { return m_lastDirtyRectCount; }
    UINT64 replaced_unread_frames() const { return m_replacedUnreadFrames; }
    UINT64 dropped_consumer_held_frames() const { return m_droppedConsumerHeldFrames; }
    UINT64 dropped_acquire_failures() const { return m_droppedAcquireFailures; }
    UINT32 producer_slot_count() const { return m_slotCount; }
    UINT32 producer_slot_index() const { return m_slotIndex; }
    UINT16 producer_channel_generation() const { return m_channelGeneration; }
    UINT64 producer_qpc_frequency() const { return m_producerQpcFrequency; }
    LUID producer_adapter_luid() const { return m_adapterLuid; }
    UINT64 consumer_acquire_timeouts() const { return m_consumerAcquireTimeouts; }
    bool sealed_frame_channel_supported() const { return m_sealedFrameChannelSupported; }
    vdd_frame_channel::channel_mode frame_channel_mode() const { return m_frameChannelMode; }
    vdd_frame_channel::channel_selection frame_channel_selection() const { return m_frameChannelSelection; }
    bool legacy_named_channel() const { return m_legacyNamedChannel; }

    /**
     * @brief The producer state this consumer is currently attached to.
     * @details Compared against a fresh metadata snapshot on every wake-up path
     *          so a producer resize or channel recreation is detected no matter
     *          which event woke the consumer.
     */
    vdd_frame_channel::producer_channel_identity current_channel_identity() const {
      return {m_channelGeneration, m_width, m_height, m_format};
    }

  private:
    enum class sealed_channel_attempt {
      skipped,
      opened,
      not_ready,
      fallback_allowed,
      required_failed,
    };

    void close();
    void detach_cursor_channel();
    bool attach_cursor_channel();
    bool ensure_cursor_channel_attached();
    void apply_metadata_snapshot(const vdd_frame_channel::shared_frame_metadata_t &meta);
    bool attach_texture_slot(ID3D11Device1 *device,
                             HANDLE shared_handle,
                             const vdd_frame_channel::shared_frame_metadata_t &meta,
                             UINT32 slot,
                             bool close_handle,
                             const wchar_t *debug_name = nullptr);
    bool attach_sealed_channel(ID3D11Device1 *device,
                               unsigned int monitor_idx,
                               const LUID &device_luid,
                               display_device::vdd_ioctl::frame_channel_open_response &sealed_channel);
    bool attach_legacy_named_channel(ID3D11Device1 *device,
                                     unsigned int monitor_idx,
                                     const LUID &device_luid);
    sealed_channel_attempt try_open_sealed_channel(ID3D11Device1 *device,
                                                  unsigned int monitor_idx,
                                                  const LUID &device_luid,
                                                  const display_device::vdd_ioctl::frame_channel_caps &sealed_caps);

    HANDLE m_hMeta = nullptr;
    void  *m_pMeta = nullptr;
    HANDLE m_hEvent = nullptr;
    std::vector<texture2d_t> m_sharedTex;
    std::vector<keyed_mutex_t> m_keyedMutex;
    bool m_holdsKey = false;
    UINT32 m_heldSlot = 0;

    // Cursor SHM is optional for compatibility with older driver builds. Newer
    // drivers signal cursor-only updates through m_hCursorEvent.
    HANDLE m_hCursorMeta = nullptr;
    void  *m_pCursorMeta = nullptr;
    HANDLE m_hCursorEvent = nullptr;
    unsigned int m_monitorIdx = 0;
    std::chrono::steady_clock::time_point m_nextCursorAttachAttempt {};
    UINT32 m_cursorAttachFailures = 0;
    bool m_cursorUpdatePending = false;
    UINT32 m_lastSeenCursorShapeId = 0xFFFFFFFFu;
    UINT32 m_lastSeenCursorPositionId = 0xFFFFFFFFu;

    UINT m_width = 0;
    UINT m_height = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    bool  m_is_hdr = false;
    float m_max_nits = 0.0f;
    float m_min_nits = 0.0f;
    float m_max_fall = 0.0f;
    UINT64 m_lastFrameCounter = 0;
    UINT64 m_lastPresentQpc = 0;
    UINT64 m_lastPublishQpc = 0;
    UINT32 m_lastPresentationFrameNumber = 0;
    UINT32 m_lastDirtyRectCount = 0;
    UINT64 m_replacedUnreadFrames = 0;
    UINT64 m_droppedConsumerHeldFrames = 0;
    UINT64 m_droppedAcquireFailures = 0;
    UINT32 m_slotCount = 1;
    UINT32 m_slotIndex = 0;
    UINT16 m_channelGeneration = 0;
    UINT64 m_producerQpcFrequency = 0;
    LUID m_adapterLuid = {};
    UINT64 m_consumerAcquireTimeouts = 0;
    bool m_sealedFrameChannelSupported = false;
    vdd_frame_channel::channel_mode m_frameChannelMode = vdd_frame_channel::channel_mode::auto_probe;
    vdd_frame_channel::channel_selection m_frameChannelSelection = vdd_frame_channel::channel_selection::unknown;
    bool m_legacyNamedChannel = true;
  };

  /**
   * Display backend that consumes frames from the ZakoVDD virtual display
   * driver via vdd_capture_t. Mirrors display_amd_vram_t / display_wgc_vram_t.
   */
  class display_vdd_vram_t: public display_vram_t {
    vdd_capture_t dup;
    unsigned int monitor_idx = 0;
    ID3D11Texture2D *current_frame = nullptr;  // Owned ref from dup.next_frame (via AddRef), released in release_snapshot
    bool vdd_borrow_enabled = false;
    std::chrono::steady_clock::time_point vdd_borrow_cooldown_until {};
    std::chrono::steady_clock::time_point vdd_borrow_last_telemetry {};
    UINT64 vdd_borrow_attempts = 0;
    UINT64 vdd_borrow_successes = 0;
    UINT64 vdd_borrow_fallbacks = 0;
    UINT64 vdd_borrow_disabled_frames = 0;
    UINT64 vdd_borrow_cooldown_frames = 0;
    UINT64 vdd_borrow_cooldown_events = 0;
    UINT64 vdd_borrow_deferred_frames = 0;
    UINT64 vdd_borrow_returned_deferred_frames = 0;
    std::shared_ptr<std::atomic<UINT64>> vdd_borrow_inflight_frames {std::make_shared<std::atomic<UINT64>>(0)};
    UINT64 vdd_borrow_inflight_limit_frames = 0;
    UINT64 vdd_last_replaced_unread = 0;
    UINT64 vdd_last_dropped_consumer_held = 0;
    UINT64 vdd_last_dropped_acquire_failures = 0;
    std::vector<std::shared_ptr<platf::img_t>> vdd_borrow_deferred_images;
    bool vdd_local_cursor_mode_active = false;

    void
    log_vdd_borrow_debug_telemetry();

  public:
    int
    init(const ::video::config_t &config, const std::string &display_name);
    capture_e
    snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) override;
    capture_e
    release_snapshot() override;

    // Override HDR queries to use producer-reported metadata from
    // SharedFrameMetadata, instead of querying DXGI on the virtual output
    // (which may not propagate the producer's static HDR meta correctly).
    bool
    is_hdr() override;
    bool
    get_hdr_metadata(SS_HDR_METADATA &metadata) override;
  };

}  // namespace platf::dxgi
