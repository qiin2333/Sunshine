/**
 * @file src/platform/windows/ds5/ds5_sidecar_client.cpp
 * @brief Lifecycle-owned client for Sunshine.Ds5Sidecar protocol v1.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include <boost/property_tree/json_parser.hpp>
#include <openssl/evp.h>

#include "ds5_sidecar_client.h"
#include "src/logging.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/virtual_device_host/protocol.h"

namespace platf::ds5 {
  using namespace std::chrono_literals;
  using namespace std::literals;

  namespace {
    using virtual_device_host::protocol::ATTACH_FLAG_GENSHIN_COMPATIBILITY;
    using virtual_device_host::protocol::CAP_AUDIO_POLICY_VIOLATION;
    using virtual_device_host::protocol::CAP_GENSHIN_COMPATIBILITY_IDENTITY;
    using virtual_device_host::protocol::HEADER_SIZE;
    using virtual_device_host::protocol::MAGIC;
    using virtual_device_host::protocol::MAX_PAYLOAD;
    using virtual_device_host::protocol::VERSION;
    using virtual_device_host::protocol::message_e;
    std::atomic_bool trusted_component_available { false };
    // The sidecar protocol identifies devices with a single byte.
    static_assert(platf::MAX_GAMEPADS <= 256, "DS5 device ids must fit the wire format");

    struct message_t {
      message_e type;
      std::uint32_t request_id;
      std::vector<std::uint8_t> payload;
    };

    std::uint16_t read_u16(const std::uint8_t *p) {
      return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
    }

    std::uint32_t read_u32(const std::uint8_t *p) {
      return static_cast<std::uint32_t>(p[0]) |
             (static_cast<std::uint32_t>(p[1]) << 8) |
             (static_cast<std::uint32_t>(p[2]) << 16) |
             (static_cast<std::uint32_t>(p[3]) << 24);
    }

    std::uint64_t read_u64(const std::uint8_t *p) {
      return static_cast<std::uint64_t>(read_u32(p)) |
             (static_cast<std::uint64_t>(read_u32(p + 4)) << 32);
    }

    void write_u16(std::uint8_t *p, std::uint16_t value) {
      p[0] = static_cast<std::uint8_t>(value);
      p[1] = static_cast<std::uint8_t>(value >> 8);
    }

    void write_u32(std::uint8_t *p, std::uint32_t value) {
      p[0] = static_cast<std::uint8_t>(value);
      p[1] = static_cast<std::uint8_t>(value >> 8);
      p[2] = static_cast<std::uint8_t>(value >> 16);
      p[3] = static_cast<std::uint8_t>(value >> 24);
    }

    // A data-plane write that cannot drain within this bound means the sidecar
    // stopped reading (for example a blocked HIDMaestro call on its read loop);
    // failing the write lets the caller bail out instead of freezing.
    constexpr DWORD write_stall_timeout_ms = 5000;

    bool transfer_exact(HANDLE pipe, HANDLE stop_event, void *buffer, std::size_t size, bool write,
                        DWORD wait_timeout = INFINITE) {
      std::size_t offset = 0;
      while (offset < size) {
        if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
          return false;
        }
        OVERLAPPED overlapped {};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) {
          return false;
        }

        DWORD count = 0;
        auto *bytes = static_cast<std::uint8_t *>(buffer) + offset;
        const auto remaining = static_cast<DWORD>(size - offset);
        const auto started = write ?
                               WriteFile(pipe, bytes, remaining, &count, &overlapped) :
                               ReadFile(pipe, bytes, remaining, &count, &overlapped);
        bool completed = started != FALSE;
        if (!completed && GetLastError() == ERROR_IO_PENDING) {
#ifdef SUNSHINE_DS5_SIDECAR_TEST_HOOK
          if (!write) {
            std::array<wchar_t, 256> event_suffix_buffer {};
            const auto event_suffix_size = GetEnvironmentVariableW(
              L"SUNSHINE_DS5_TEST_EVENT_SUFFIX", event_suffix_buffer.data(),
              static_cast<DWORD>(event_suffix_buffer.size()));
            const auto event_suffix = event_suffix_size > 0 && event_suffix_size < event_suffix_buffer.size() ?
                                        std::wstring(event_suffix_buffer.data(), event_suffix_size) :
                                        std::to_wstring(GetCurrentProcessId());
            const auto event_name = L"Local\\sunshine-ds5-test-reader-" + event_suffix;
            if (const auto event = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str())) {
              SetEvent(event);
              CloseHandle(event);
            }
          }
#endif
          const std::array waits { overlapped.hEvent, stop_event };
          const auto wait_result = WaitForMultipleObjects(
            static_cast<DWORD>(waits.size()), waits.data(), FALSE, wait_timeout);
          if (wait_result == WAIT_OBJECT_0) {
            completed = GetOverlappedResult(pipe, &overlapped, &count, FALSE) != FALSE;
          }
          else {
            // The persistent stop event also covers cancel-before-I/O races: if
            // shutdown wins just before ReadFile/WriteFile, this newly issued
            // operation still observes the signaled event and cancels itself.
            CancelIoEx(pipe, &overlapped);
            WaitForSingleObject(overlapped.hEvent, INFINITE);
            completed = false;
          }
        }
        CloseHandle(overlapped.hEvent);
        if (!completed || count == 0) {
          return false;
        }
        offset += count;
      }
      return true;
    }

    bool read_exact(HANDLE pipe, HANDLE stop_event, std::span<std::uint8_t> destination) {
      return transfer_exact(pipe, stop_event, destination.data(), destination.size(), false);
    }

    bool write_exact(HANDLE pipe, HANDLE stop_event, std::span<const std::uint8_t> source) {
      return transfer_exact(
        pipe, stop_event, const_cast<std::uint8_t *>(source.data()), source.size(), true,
        write_stall_timeout_ms);
    }

#ifndef SUNSHINE_DS5_SIDECAR_TEST_HOOK
    std::optional<std::string> sha256_file(const std::filesystem::path &path) {
      std::ifstream stream(path, std::ios::binary);
      if (!stream) {
        return std::nullopt;
      }
      const auto context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
      if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return std::nullopt;
      }
      std::array<char, 64 * 1024> buffer {};
      while (stream) {
        stream.read(buffer.data(), buffer.size());
        const auto count = stream.gcount();
        if (count > 0 &&
            EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(count)) != 1) {
          return std::nullopt;
        }
      }
      if (!stream.eof()) {
        return std::nullopt;
      }
      std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
      unsigned int digest_size = 0;
      if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1) {
        return std::nullopt;
      }
      std::ostringstream result;
      result << std::hex << std::setfill('0');
      for (unsigned int index = 0; index < digest_size; ++index) {
        result << std::setw(2) << static_cast<unsigned int>(digest[index]);
      }
      return result.str();
    }
#endif

    std::filesystem::path sidecar_executable_path() {
#ifdef SUNSHINE_DS5_SIDECAR_TEST_HOOK
      return std::filesystem::path(SUNSHINE_DS5_FAKE_SIDECAR_PATH);
#else
      return platf::appdata().parent_path() / "tools" / "sunshine-ds5-component" /
             "active" / "Sunshine.Ds5Sidecar.exe";
#endif
    }

    std::optional<std::filesystem::path> trusted_sidecar_path() {
      // The test target supplies its purpose-built protocol peer. Production
      // uses only the fixed component directory and never accepts a configured path.
      const auto configured = sidecar_executable_path();
      std::error_code path_error;
      if (!std::filesystem::is_regular_file(configured, path_error) || path_error) {
        return std::nullopt;
      }
#ifdef SUNSHINE_DS5_SIDECAR_TEST_HOOK
      const auto candidate = std::filesystem::weakly_canonical(configured, path_error);
      return path_error ? std::nullopt : std::optional { candidate };
#else
      const auto install_root = std::filesystem::weakly_canonical(
        platf::appdata().parent_path(),
        path_error);
      if (path_error) return std::nullopt;
      const auto expected_active_root = install_root / "tools" /
                                        "sunshine-ds5-component" / "active";
      const auto candidate = std::filesystem::weakly_canonical(configured, path_error);
      if (path_error || candidate.parent_path() != expected_active_root ||
          candidate.filename() != "Sunshine.Ds5Sidecar.exe" ||
          !std::filesystem::is_regular_file(candidate, path_error) || path_error) {
        BOOST_LOG(error) << "Rejected a DualSense sidecar outside the fixed active component directory"sv;
        return std::nullopt;
      }

      boost::property_tree::ptree manifest;
      try {
        std::ifstream manifest_file_stream(expected_active_root / "component.json", std::ios::binary);
        if (!manifest_file_stream.is_open()) {
          throw std::runtime_error("unable to open component manifest");
        }
        boost::property_tree::read_json(manifest_file_stream, manifest);
      }
      catch (const std::exception &exception) {
        BOOST_LOG(error) << "Unable to read the active DualSense component manifest: "sv
                         << exception.what();
        return std::nullopt;
      }
      const auto manifest_file = manifest.get<std::string>("sidecar_file", "");
      auto expected_digest = manifest.get<std::string>("sidecar_sha256", "");
      if (manifest.get<std::uint32_t>("protocol", 0) != VERSION ||
          manifest_file != "Sunshine.Ds5Sidecar.exe" || expected_digest.size() != 64) {
        BOOST_LOG(error) << "The active DualSense component manifest is incomplete or incompatible"sv;
        return std::nullopt;
      }
      std::ranges::transform(expected_digest, expected_digest.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
      const auto actual_digest = sha256_file(candidate);
      if (!actual_digest || *actual_digest != expected_digest) {
        BOOST_LOG(error) << "The active DualSense sidecar failed manifest digest verification"sv;
        return std::nullopt;
      }
      return candidate;
#endif
    }
  }  // namespace

  struct sidecar_client_t::impl_t {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread reader;
    std::mutex write_mutex;
    std::atomic_bool stopping { false };
    std::atomic_bool online { false };
    std::atomic_int global_index { -1 };
    std::uint8_t client_index = 0;
    bool audio_haptics_requested = false;
    bool genshin_compatibility_requested = false;
    bool force_hid_fallback = false;
    feedback_queue_t feedback_queue;
    std::uint32_t next_request_id = 1;

    ~impl_t() {
      close();
      if (stop_event) {
        CloseHandle(stop_event);
      }
    }

    bool send(message_e type, std::uint32_t request_id, std::span<const std::uint8_t> payload) {
      if (stopping) {
        return false;
      }
      std::vector<std::uint8_t> frame(HEADER_SIZE + payload.size());
      const auto header = virtual_device_host::protocol::encode_header(
        type, static_cast<std::uint32_t>(payload.size()), request_id);
      std::copy(header.begin(), header.end(), frame.begin());
      std::copy(payload.begin(), payload.end(), frame.begin() + HEADER_SIZE);
      std::lock_guard lock(write_mutex);
      if (stopping || pipe == INVALID_HANDLE_VALUE) {
        return false;
      }
      if (write_exact(pipe, stop_event, frame)) {
        return true;
      }
      // The write stalled or the pipe broke: cancel the reader's pending read
      // so read_loop's recovery path tears the transport down, instead of
      // letting later senders block on a pipe that will never drain.
      CancelIoEx(pipe, nullptr);
      return false;
    }

    bool receive(message_t &message) {
      std::array<std::uint8_t, HEADER_SIZE> header {};
      if (!read_exact(pipe, stop_event, header) || read_u32(header.data()) != MAGIC ||
          read_u16(header.data() + 4) != VERSION) {
        return false;
      }
      const auto payload_size = read_u32(header.data() + 8);
      if (payload_size > MAX_PAYLOAD) {
        return false;
      }
      message.type = static_cast<message_e>(read_u16(header.data() + 6));
      message.request_id = read_u32(header.data() + 12);
      message.payload.resize(payload_size);
      return read_exact(pipe, stop_event, message.payload);
    }

    void dispatch(message_t &message) {
      const auto &p = message.payload;
      const auto owned_index = global_index.load();
      switch (message.type) {
        case message_e::rumble:
          if (p.size() == 6 && p[0] == owned_index) {
            feedback_queue->raise(gamepad_feedback_msg_t::make_rumble(
              p[1], read_u16(p.data() + 2), read_u16(p.data() + 4)));
          }
          break;
        case message_e::adaptive_triggers:
          if (p.size() == 26 && p[0] == owned_index) {
            std::array<std::uint8_t, 10> left, right;
            std::copy_n(p.data() + 6, 10, left.begin());
            std::copy_n(p.data() + 16, 10, right.begin());
            feedback_queue->raise(gamepad_feedback_msg_t::make_adaptive_triggers(
              p[1], p[2], p[3], p[4], left, right));
          }
          break;
        case message_e::led:
          if (p.size() == 5 && p[0] == owned_index) {
            feedback_queue->raise(gamepad_feedback_msg_t::make_rgb_led(p[1], p[2], p[3], p[4]));
          }
          break;
        case message_e::haptics_pcm:
          if (p.size() >= 24 && p[0] == owned_index) {
            const auto frames = read_u16(p.data() + 4);
            const auto pcm_size = static_cast<std::size_t>(frames) * 4;
            if (p[3] == 2 && p[6] == 16 && read_u32(p.data() + 20) == 48000 &&
                frames <= 240 && p.size() == 24 + pcm_size) {
              feedback_queue->raise(gamepad_feedback_msg_t::make_ds5_haptics_pcm(
                p[1], p[2], frames, read_u32(p.data() + 8), read_u64(p.data() + 12),
                p.data() + 24, pcm_size));
            }
          }
          break;
        case message_e::audio_policy_violation:
          if (p.size() == 4 && p[0] == owned_index) {
            static constexpr std::array<std::string_view, 3> role_names {
              "console", "multimedia", "communications"
            };
            const auto role = p[2] < role_names.size() ? role_names[p[2]] : "unknown"sv;
            force_hid_fallback = true;
            audio_haptics_requested = false;
            BOOST_LOG(warning) << "The virtual DualSense audio endpoint became the Windows "sv
                               << role << " default; falling back to HID-only DualSense"sv;
          }
          break;
        case message_e::error:
          BOOST_LOG(warning) << "DualSense sidecar reported an asynchronous error"sv;
          break;
        default:
          break;
      }
    }

    bool transact(message_e request_type, std::span<const std::uint8_t> payload,
                  message_e reply_type, message_t &reply) {
      const auto request_id = next_request_id++;
      if (!send(request_type, request_id, payload)) {
        return false;
      }
      while (!stopping) {
        // Rumble, LEDs and adaptive triggers share the control channel and are
        // emitted from a different sidecar thread, so they can legitimately
        // arrive ahead of the reply. Dispatch them instead of misreading the
        // first message as the reply.
        message_t message;
        if (!receive(message)) {
          return false;
        }
        if (message.request_id == request_id && message.type == reply_type) {
          reply = std::move(message);
          return true;
        }
        if (message.type == message_e::error && message.request_id == request_id) {
          std::string reason;
          if (message.payload.size() >= 8) {
            const auto length = std::min<std::size_t>(read_u32(message.payload.data() + 4), message.payload.size() - 8);
            reason.assign(reinterpret_cast<const char *>(message.payload.data() + 8), length);
          }
          BOOST_LOG(error) << "DualSense sidecar rejected request: "sv << reason;
          return false;
        }
        dispatch(message);
      }
      return false;
    }

    bool launch_and_connect() {
      const auto trusted_executable = trusted_sidecar_path();
      if (!stop_event || !trusted_executable) {
        BOOST_LOG(error) << "DualSense sidecar is not installed or its active manifest is invalid"sv;
        return false;
      }
      const auto &executable = *trusted_executable;

      std::random_device random;
      const auto pipe_name = "sunshine-ds5-v1-"s + std::to_string(GetCurrentProcessId()) + "-" +
                             std::to_string(random()) + std::to_string(random());
      const auto pipe_path = platf::from_utf8(R"(\\.\pipe\)"s + pipe_name);
      auto executable_w = executable.wstring();
      auto command = L"\"" + executable_w + L"\" --pipe " + platf::from_utf8(pipe_name);
      std::vector<wchar_t> mutable_command(command.begin(), command.end());
      mutable_command.push_back(L'\0');

      STARTUPINFOW startup { sizeof(startup) };
      PROCESS_INFORMATION process_info {};

      job = CreateJobObjectW(nullptr, nullptr);
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits {};
      job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                            &job_limits, sizeof(job_limits))) {
        BOOST_LOG(error) << "Failed to create the DualSense sidecar lifecycle job: "sv << GetLastError();
        if (job) {
          CloseHandle(job);
          job = nullptr;
        }
        return false;
      }

      if (!CreateProcessW(executable_w.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, executable.parent_path().c_str(),
                          &startup, &process_info)) {
        BOOST_LOG(error) << "Failed to launch DualSense sidecar: "sv << GetLastError();
        CloseHandle(job);
        job = nullptr;
        return false;
      }
      if (!AssignProcessToJobObject(job, process_info.hProcess)) {
        BOOST_LOG(error) << "Failed to assign the DualSense sidecar to its lifecycle job: "sv << GetLastError();
        TerminateProcess(process_info.hProcess, ERROR_PROCESS_ABORTED);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        CloseHandle(job);
        job = nullptr;
        return false;
      }
      ResumeThread(process_info.hThread);
      CloseHandle(process_info.hThread);
      process = process_info.hProcess;

      const auto deadline = std::chrono::steady_clock::now() + 10s;
      do {
        if (stopping || WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
          return false;
        }
        const auto connected_pipe = CreateFileW(pipe_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                                0, nullptr, OPEN_EXISTING,
                                                FILE_FLAG_OVERLAPPED, nullptr);
        if (connected_pipe != INVALID_HANDLE_VALUE) {
          std::lock_guard lock(write_mutex);
          pipe = connected_pipe;
          return true;
        }
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
          BOOST_LOG(error) << "DualSense sidecar exited before opening its pipe"sv;
          return false;
        }
        WaitNamedPipeW(pipe_path.c_str(), 100);
      } while (std::chrono::steady_clock::now() < deadline);

      if (stopping || WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
        return false;
      }
      BOOST_LOG(error) << "Timed out connecting to DualSense sidecar pipe"sv;
      return false;
    }

    bool connect_and_attach(const gamepad_id_t &id, bool audio_haptics,
                            bool genshin_compatibility) {
      bool use_genshin_identity = audio_haptics && genshin_compatibility;
      if (!launch_and_connect()) {
        return false;
      }

      message_t reply;
      std::array<std::uint8_t, 4> hello {};
      write_u32(hello.data(), 0);
      if (!transact(message_e::hello, hello, message_e::hello_reply, reply) ||
          reply.payload.size() != 4) {
        return false;
      }
      const auto capabilities = read_u32(reply.payload.data());
      if (audio_haptics && !(capabilities & CAP_AUDIO_POLICY_VIOLATION)) {
        BOOST_LOG(warning) << "DualSense sidecar does not advertise audio endpoint policy protection; "sv
                           << "falling back to HID-only DualSense"sv;
        force_hid_fallback = true;
        audio_haptics = false;
        use_genshin_identity = false;
      }
      if (use_genshin_identity &&
          (capabilities & CAP_GENSHIN_COMPATIBILITY_IDENTITY) == 0) {
        BOOST_LOG(error) << "The installed DualSense sidecar does not support Genshin compatibility mode"sv;
        return false;
      }

      std::array<std::uint8_t, 4> attach_payload {
        static_cast<std::uint8_t>(id.globalIndex),
        id.clientRelativeIndex,
        static_cast<std::uint8_t>(audio_haptics ? 1 : 0),
        static_cast<std::uint8_t>(use_genshin_identity ? ATTACH_FLAG_GENSHIN_COMPATIBILITY : 0),
      };
      // Claim ownership before the transaction so feedback interleaved ahead
      // of the attach reply is routed to the queue instead of dropped.
      global_index = id.globalIndex;
      if (!transact(message_e::attach, attach_payload, message_e::attach_reply, reply) ||
          reply.payload.size() != 8 || reply.payload[0] != attach_payload[0]) {
        return false;
      }
      if (audio_haptics && reply.payload[1] == 0) {
        BOOST_LOG(error) << "DualSense composite attach did not create its four-channel audio endpoint"sv;
        return false;
      }

      client_index = id.clientRelativeIndex;
      audio_haptics_requested = audio_haptics && !force_hid_fallback;
      genshin_compatibility_requested = use_genshin_identity && audio_haptics_requested;
      online = true;
      BOOST_LOG(info) << "DualSense sidecar attached controller "sv << id.globalIndex
                      << (reply.payload[1] ? " with native four-channel haptics" : " (HID only)")
                      << (genshin_compatibility_requested ? " using Genshin compatibility identity" : "");
      return true;
    }

    bool attach(const gamepad_id_t &id, bool audio_haptics, bool genshin_compatibility) {
      // A failed recovery releases global_index from the reader thread before
      // that thread object stops being joinable. Reap it before reusing the
      // same client for a later allocation; assigning over a joinable
      // std::thread would terminate the process.
      if (reader.joinable()) {
        reader.join();
      }
      // A new allocation is an explicit user/session request. Only the
      // automatic recovery of the current allocation inherits the fallback.
      force_hid_fallback = false;
      if (!connect_and_attach(id, audio_haptics, genshin_compatibility)) {
        return false;
      }
      reader = std::thread([this] { read_loop(); });
      return true;
    }

    void close_transport() {
      if (pipe != INVALID_HANDLE_VALUE) {
        std::lock_guard lock(write_mutex);
        CancelIoEx(pipe, nullptr);
        CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
      }
      if (process) {
        if (WaitForSingleObject(process, 1000) == WAIT_TIMEOUT) {
          BOOST_LOG(warning) << "DualSense sidecar did not exit after owner disconnect; terminating owned child"sv;
          TerminateProcess(process, ERROR_PROCESS_ABORTED);
          WaitForSingleObject(process, 1000);
        }
        CloseHandle(process);
        process = nullptr;
      }
      if (job) {
        CloseHandle(job);
        job = nullptr;
      }
    }

    void read_loop() {
      bool recovery_attempted = false;
      while (!stopping) {
        message_t message;
        while (!stopping && receive(message)) {
          dispatch(message);
        }
        online = false;
        if (stopping) {
          break;
        }
        BOOST_LOG(warning) << "DualSense sidecar disconnected unexpectedly"sv;
        close_transport();
        if (recovery_attempted) {
          break;
        }
        recovery_attempted = true;
        const gamepad_id_t id {
          global_index.load(),
          client_index,
        };
        if (!connect_and_attach(id, audio_haptics_requested, genshin_compatibility_requested)) {
          close_transport();
          break;
        }
        BOOST_LOG(info) << "DualSense sidecar recovered after one relaunch"sv;
      }
      online = false;
      if (!stopping) {
        global_index = -1;
        BOOST_LOG(error) << "DualSense sidecar recovery failed; controller ownership was released"sv;
      }
    }

    void close() {
      if (stopping.exchange(true)) {
        return;
      }
      online = false;
      if (stop_event) {
        SetEvent(stop_event);
      }
      if (reader.joinable()) {
        reader.join();
      }
      close_transport();
      global_index = -1;
    }
  };

  bool component_available() noexcept {
    return trusted_component_available.load(std::memory_order_acquire);
  }

  std::optional<std::filesystem::path> trusted_component_path() {
    return trusted_sidecar_path();
  }

  bool refresh_component_availability() noexcept {
    bool available = false;
    try {
      available = trusted_component_path().has_value();
    }
    catch (...) {
      available = false;
    }
    trusted_component_available.store(available, std::memory_order_release);
    return available;
  }

  sidecar_client_t::sidecar_client_t():
      _impl(std::make_unique<impl_t>()) {
    refresh_component_availability();
  }

  sidecar_client_t::~sidecar_client_t() = default;

  bool sidecar_client_t::configured() const {
    return refresh_component_availability();
  }

  bool sidecar_client_t::owns(int global_index) const {
    // Ownership survives a temporary transport outage so the input layer can
    // still release the controller while the reader thread is recovering it.
    return global_index >= 0 && _impl->global_index == global_index;
  }

  int sidecar_client_t::alloc(const gamepad_id_t &id, feedback_queue_t feedback_queue,
                              bool audio_haptics, bool genshin_compatibility) {
    if (!configured() || _impl->global_index >= 0) {
      return -1;
    }
    _impl->feedback_queue = std::move(feedback_queue);
    if (_impl->attach(id, audio_haptics, genshin_compatibility && audio_haptics)) {
      return 0;
    }
    _impl = std::make_unique<impl_t>();
    return -1;
  }

  void sidecar_client_t::free(int global_index) {
    if (_impl->global_index == global_index) {
      _impl->close();
      _impl = std::make_unique<impl_t>();
    }
  }

  void sidecar_client_t::submit_input(int global_index, const gamepad_state_t &state) {
    if (!owns(global_index) || !_impl->online) return;
    std::array<std::uint8_t, 20> payload {};
    payload[0] = static_cast<std::uint8_t>(global_index);
    write_u32(payload.data() + 4, state.buttonFlags);
    payload[8] = state.lt;
    payload[9] = state.rt;
    write_u16(payload.data() + 12, static_cast<std::uint16_t>(state.lsX));
    write_u16(payload.data() + 14, static_cast<std::uint16_t>(state.lsY));
    write_u16(payload.data() + 16, static_cast<std::uint16_t>(state.rsX));
    write_u16(payload.data() + 18, static_cast<std::uint16_t>(state.rsY));
    _impl->send(message_e::input, 0, payload);
  }

  void sidecar_client_t::submit_touch(const gamepad_touch_t &touch) {
    if (!owns(touch.id.globalIndex) || !_impl->online) return;
    std::array<std::uint8_t, 20> payload {};
    payload[0] = static_cast<std::uint8_t>(touch.id.globalIndex);
    payload[1] = touch.eventType;
    write_u32(payload.data() + 4, touch.pointerId);
    write_u32(payload.data() + 8, std::bit_cast<std::uint32_t>(touch.x));
    write_u32(payload.data() + 12, std::bit_cast<std::uint32_t>(touch.y));
    write_u32(payload.data() + 16, std::bit_cast<std::uint32_t>(touch.pressure));
    _impl->send(message_e::touch, 0, payload);
  }

  void sidecar_client_t::submit_motion(const gamepad_motion_t &motion) {
    if (!owns(motion.id.globalIndex) || !_impl->online) return;
    std::array<std::uint8_t, 16> payload {};
    payload[0] = static_cast<std::uint8_t>(motion.id.globalIndex);
    payload[1] = motion.motionType;
    write_u32(payload.data() + 4, std::bit_cast<std::uint32_t>(motion.x));
    write_u32(payload.data() + 8, std::bit_cast<std::uint32_t>(motion.y));
    write_u32(payload.data() + 12, std::bit_cast<std::uint32_t>(motion.z));
    _impl->send(message_e::motion, 0, payload);
  }

  void sidecar_client_t::submit_battery(const gamepad_battery_t &battery) {
    if (!owns(battery.id.globalIndex) || !_impl->online) return;
    std::array<std::uint8_t, 4> payload {
      static_cast<std::uint8_t>(battery.id.globalIndex), battery.state, battery.percentage, 0
    };
    _impl->send(message_e::battery, 0, payload);
  }
}  // namespace platf::ds5
