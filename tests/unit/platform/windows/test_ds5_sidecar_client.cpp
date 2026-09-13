/**
 * @file tests/unit/platform/windows/test_ds5_sidecar_client.cpp
 * @brief Regression coverage for cancellable DS5 Core pipe shutdown.
 */
#ifdef _WIN32

  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>

  #include <chrono>

  #include "src/platform/windows/ds5/ds5_sidecar_client.h"
  #include "src/platform/windows/virtual_device_host/protocol.h"
  #include <gtest/gtest-spi.h>
  #include <gtest/gtest.h>

namespace {
  using get_environment_fn_t = DWORD(WINAPI *)(LPCWSTR, LPWSTR, DWORD);
  using set_environment_fn_t = BOOL(WINAPI *)(LPCWSTR, LPCWSTR);

  struct handle_scope_t {
    explicit handle_scope_t(HANDLE value): handle(value) {}
    ~handle_scope_t() {
      if (handle) CloseHandle(handle);
    }
    HANDLE handle;
  };

  struct event_namespace_scope_t {
    explicit event_namespace_scope_t(std::wstring_view test_name):
        suffix(std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(GetTickCount64()) + L"-" + std::wstring(test_name)) {
      SetEnvironmentVariableW(L"SUNSHINE_DS5_TEST_EVENT_SUFFIX", suffix.c_str());
    }

    ~event_namespace_scope_t() {
      SetEnvironmentVariableW(L"SUNSHINE_DS5_TEST_EVENT_SUFFIX", nullptr);
    }

    std::wstring suffix;
  };

  struct environment_scope_t {
    enum class original_state_e {
      unknown,
      undefined,
      defined,
    };

    environment_scope_t(const wchar_t *variable, const wchar_t *value,
                        get_environment_fn_t get_environment = GetEnvironmentVariableW,
                        set_environment_fn_t set_environment = SetEnvironmentVariableW):
        variable(variable),
        set_environment(set_environment) {
      SetLastError(ERROR_SUCCESS);
      auto required = get_environment(variable, nullptr, 0);
      if (required == 0) {
        const auto error = GetLastError();
        if (error == ERROR_ENVVAR_NOT_FOUND) {
          original_state = original_state_e::undefined;
        }
        else if (error == ERROR_SUCCESS) {
          original_state = original_state_e::defined;
        }
        else {
          ADD_FAILURE() << "GetEnvironmentVariableW size query failed: " << error;
          return;
        }
      }
      else {
        original_state = original_state_e::defined;
      }

      while (required != 0) {
        original_value.resize(required);
        SetLastError(ERROR_SUCCESS);
        const auto copied = get_environment(variable, original_value.data(), required);
        if (copied == 0 && GetLastError() != ERROR_SUCCESS) {
          ADD_FAILURE() << "GetEnvironmentVariableW value read failed: " << GetLastError();
          original_state = original_state_e::unknown;
          original_value.clear();
          return;
        }
        if (copied < required) {
          original_value.resize(copied);
          break;
        }
        required = copied;
      }

      if (!set_environment(variable, value)) {
        ADD_FAILURE() << "SetEnvironmentVariableW scoped write failed: " << GetLastError();
        original_state = original_state_e::unknown;
        return;
      }
      restore_required = true;
    }

    ~environment_scope_t() {
      if (!restore_required)
        return;
      const auto *value = original_state == original_state_e::defined ? original_value.c_str() : nullptr;
      if (!set_environment(variable.c_str(), value)) {
        ADD_FAILURE() << "SetEnvironmentVariableW restore failed: " << GetLastError();
      }
    }

    std::wstring variable;
    std::wstring original_value;
    set_environment_fn_t set_environment;
    original_state_e original_state = original_state_e::unknown;
    bool restore_required = false;
  };

  DWORD WINAPI fail_environment_read(LPCWSTR, LPWSTR, DWORD) {
    SetLastError(ERROR_ACCESS_DENIED);
    return 0;
  }

  DWORD WINAPI fail_environment_value_read(LPCWSTR, LPWSTR value, DWORD) {
    if (!value) {
      SetLastError(ERROR_SUCCESS);
      return 2;
    }
    SetLastError(ERROR_ACCESS_DENIED);
    return 0;
  }

  BOOL WINAPI fail_environment_write(LPCWSTR, LPCWSTR) {
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }

  int environment_write_calls;

  BOOL WINAPI fail_environment_restore(LPCWSTR, LPCWSTR) {
    if (environment_write_calls++ == 0)
      return TRUE;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }
}  // namespace

TEST(VirtualDeviceHostProtocolTests, FreezesSds5V1Abi) {
  namespace protocol = platf::virtual_device_host::protocol;
  EXPECT_EQ(protocol::MAGIC, 0x35534453u);
  EXPECT_EQ(protocol::VERSION, 1u);
  EXPECT_EQ(protocol::HEADER_SIZE, 16u);
  EXPECT_EQ(protocol::CAP_VIRTUAL_MICROPHONE, 1u << 10);
  EXPECT_EQ(protocol::CAP_PERSISTENT_DEVICE_HOST, 1u << 11);
  EXPECT_EQ(protocol::CAP_MICROPHONE_STATUS, 1u << 12);
  EXPECT_EQ(static_cast<std::uint16_t>(protocol::message_e::mic_create), 12u);
  EXPECT_EQ(static_cast<std::uint16_t>(protocol::message_e::mic_status), 107u);
  EXPECT_EQ(protocol::MIC_CREATE_PAYLOAD_SIZE, 8u);
  EXPECT_EQ(protocol::MIC_CREATE_REPLY_PAYLOAD_SIZE, 16u);
  EXPECT_EQ(protocol::MIC_OPERATION_REPLY_PAYLOAD_SIZE, 8u);
  EXPECT_EQ(protocol::MIC_PCM_HEADER_SIZE, 20u);
  EXPECT_EQ(protocol::MIC_STATUS_PAYLOAD_SIZE, 28u);
  EXPECT_EQ(protocol::MAX_MIC_PCM_FRAMES, 960u);
  EXPECT_EQ(static_cast<std::int32_t>(protocol::mic_result_e::invalid_format), -1001);
  EXPECT_EQ(static_cast<std::int32_t>(protocol::mic_result_e::device_not_created), -1004);
}

TEST(Ds5SidecarClientTests, EnvironmentScopeRestoresPreviousValues) {
  const auto variable = L"SUNSHINE_DS5_TEST_ENVIRONMENT_SCOPE_" + std::to_wstring(GetCurrentProcessId());
  EXPECT_TRUE(SetEnvironmentVariableW(variable.c_str(), nullptr));
  {
    environment_scope_t scoped(variable.c_str(), L"temporary");
  }
  SetLastError(ERROR_SUCCESS);
  EXPECT_EQ(GetEnvironmentVariableW(variable.c_str(), nullptr, 0), 0u);
  EXPECT_EQ(GetLastError(), ERROR_ENVVAR_NOT_FOUND);

  EXPECT_TRUE(SetEnvironmentVariableW(variable.c_str(), L""));
  {
    environment_scope_t scoped(variable.c_str(), L"temporary");
  }
  SetLastError(ERROR_SUCCESS);
  EXPECT_EQ(GetEnvironmentVariableW(variable.c_str(), nullptr, 0), 1u);
  EXPECT_EQ(GetLastError(), ERROR_SUCCESS);

  EXPECT_TRUE(SetEnvironmentVariableW(variable.c_str(), L"original"));
  {
    environment_scope_t scoped(variable.c_str(), L"temporary");
  }
  std::array<wchar_t, 16> restored {};
  EXPECT_EQ(GetEnvironmentVariableW(
              variable.c_str(), restored.data(), static_cast<DWORD>(restored.size())),
            8u);
  EXPECT_STREQ(restored.data(), L"original");
  EXPECT_TRUE(SetEnvironmentVariableW(variable.c_str(), nullptr));
}

TEST(Ds5SidecarClientTests, EnvironmentScopeReportsApiFailures) {
  EXPECT_NONFATAL_FAILURE(
    { environment_scope_t scoped(L"SUNSHINE_DS5_TEST_READ_FAILURE", L"temporary",
                                 fail_environment_read, fail_environment_write); },
    "GetEnvironmentVariableW size query failed");
  EXPECT_NONFATAL_FAILURE(
    { environment_scope_t scoped(L"SUNSHINE_DS5_TEST_VALUE_READ_FAILURE", L"temporary",
                                 fail_environment_value_read, fail_environment_write); },
    "GetEnvironmentVariableW value read failed");
  EXPECT_NONFATAL_FAILURE(
    { environment_scope_t scoped(L"SUNSHINE_DS5_TEST_SET_FAILURE", L"temporary",
                                 GetEnvironmentVariableW, fail_environment_write); },
    "SetEnvironmentVariableW scoped write failed");
  EXPECT_NONFATAL_FAILURE(
    {
      environment_write_calls = 0;
      environment_scope_t scoped(L"SUNSHINE_DS5_TEST_RESTORE_FAILURE", L"temporary",
                                 GetEnvironmentVariableW, fail_environment_restore);
    },
    "SetEnvironmentVariableW restore failed");
}

TEST(Ds5SidecarClientTests, UnassignedIndexIsNotOwned) {
  platf::ds5::sidecar_client_t client;
  EXPECT_FALSE(client.owns(-1));
}

TEST(Ds5SidecarClientTests, AllocThenFreeCancelsBlockedReader) {

  event_namespace_scope_t events(L"blocked-reader");
  const auto reader_name = L"Local\\sunshine-ds5-test-reader-" + events.suffix;
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + events.suffix;
  const auto marker_name = L"Local\\sunshine-ds5-test-marker-" + events.suffix;
  handle_scope_t reader_event(CreateEventW(nullptr, FALSE, FALSE, reader_name.c_str()));
  handle_scope_t continue_event(CreateEventW(nullptr, FALSE, FALSE, continue_name.c_str()));
  handle_scope_t marker_event(CreateEventW(nullptr, FALSE, FALSE, marker_name.c_str()));
  ASSERT_NE(reader_event.handle, nullptr);
  ASSERT_NE(continue_event.handle, nullptr);
  ASSERT_NE(marker_event.handle, nullptr);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-lifecycle-test");
  auto feedback_for_test = feedback;
  platf::ds5::sidecar_client_t client;
  ASSERT_EQ(client.alloc({ 0, 0 }, std::move(feedback), false), 0);

  ASSERT_EQ(WaitForSingleObject(reader_event.handle, 2000), WAIT_OBJECT_0);
  ASSERT_TRUE(SetEvent(continue_event.handle));
  ASSERT_EQ(WaitForSingleObject(marker_event.handle, 2000), WAIT_OBJECT_0);
  const auto marker = feedback_for_test->pop(std::chrono::seconds(2));
  ASSERT_TRUE(marker);
  ASSERT_EQ(marker->type, platf::gamepad_feedback_e::rumble);
  // The second signal is raised only after the marker was processed and the
  // following overlapped read is actually pending.
  ASSERT_EQ(WaitForSingleObject(reader_event.handle, 2000), WAIT_OBJECT_0);

  const auto started = std::chrono::steady_clock::now();
  client.free(0);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST(Ds5SidecarClientTests, AttachSurvivesInterleavedAsyncFeedback) {

  event_namespace_scope_t events(L"interleaved-feedback");
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + events.suffix;
  const auto marker_name = L"Local\\sunshine-ds5-test-marker-" + events.suffix;
  handle_scope_t continue_event(CreateEventW(nullptr, FALSE, FALSE, continue_name.c_str()));
  handle_scope_t marker_event(CreateEventW(nullptr, FALSE, FALSE, marker_name.c_str()));
  ASSERT_NE(continue_event.handle, nullptr);
  ASSERT_NE(marker_event.handle, nullptr);
  ASSERT_NE(SetEnvironmentVariableW(L"SUNSHINE_DS5_TEST_INTERLEAVE", L"1"), 0);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-interleave-test");
  auto feedback_for_test = feedback;
  platf::ds5::sidecar_client_t client;
  // The fake peer emits an async rumble ahead of the attach reply. The
  // transaction must dispatch it and still match the reply; before the
  // multiplexing fix the rumble was misread as the reply and alloc failed.
  EXPECT_EQ(client.alloc({ 0, 0 }, std::move(feedback), false), 0);
  SetEnvironmentVariableW(L"SUNSHINE_DS5_TEST_INTERLEAVE", nullptr);
  const auto early = feedback_for_test->pop(std::chrono::seconds(2));
  ASSERT_TRUE(early);
  EXPECT_EQ(early->type, platf::gamepad_feedback_e::rumble);

  ASSERT_TRUE(SetEvent(continue_event.handle));
  ASSERT_EQ(WaitForSingleObject(marker_event.handle, 2000), WAIT_OBJECT_0);
  const auto marker = feedback_for_test->pop(std::chrono::seconds(2));
  ASSERT_TRUE(marker);
  EXPECT_EQ(marker->type, platf::gamepad_feedback_e::rumble);
  client.free(0);
}

TEST(Ds5SidecarClientTests, RejectsCompositeAttachWithoutAudioEndpoint) {

  event_namespace_scope_t events(L"audio-endpoint");
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + events.suffix;
  handle_scope_t continue_event(CreateEventW(nullptr, FALSE, TRUE, continue_name.c_str()));
  ASSERT_NE(continue_event.handle, nullptr);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-audio-attach-test");
  platf::ds5::sidecar_client_t client;
  EXPECT_EQ(client.alloc({ 0, 0 }, std::move(feedback), true), -1);
  EXPECT_FALSE(client.owns(0));
}

TEST(Ds5SidecarClientTests, FallsBackToHidWhenPeerLacksAudioPolicyCapability) {

  event_namespace_scope_t events(L"legacy-capabilities");
  environment_scope_t legacy_capabilities(L"SUNSHINE_DS5_TEST_LEGACY_CAPABILITIES", L"1");
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + events.suffix;
  handle_scope_t continue_event(CreateEventW(nullptr, FALSE, TRUE, continue_name.c_str()));
  ASSERT_NE(continue_event.handle, nullptr);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-legacy-capabilities-test");
  platf::ds5::sidecar_client_t client;
  EXPECT_EQ(client.alloc({ 0, 0 }, std::move(feedback), true), 0);
  EXPECT_TRUE(client.owns(0));
  client.free(0);
}

TEST(Ds5SidecarClientTests, SendsNegotiatedGenshinCompatibilityAttachFlag) {

  event_namespace_scope_t events(L"genshin-compatibility");
  environment_scope_t enable_compatibility(
    L"SUNSHINE_DS5_TEST_GENSHIN_COMPATIBILITY", L"1");
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + events.suffix;
  const auto compatibility_name =
    L"Local\\sunshine-ds5-test-genshin-compatibility-" + events.suffix;
  handle_scope_t continue_event(CreateEventW(nullptr, FALSE, TRUE, continue_name.c_str()));
  handle_scope_t compatibility_event(CreateEventW(nullptr, FALSE, FALSE, compatibility_name.c_str()));
  ASSERT_NE(continue_event.handle, nullptr);
  ASSERT_NE(compatibility_event.handle, nullptr);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-genshin-compatibility-test");
  platf::ds5::sidecar_client_t client;
  ASSERT_EQ(client.alloc({ 0, 0 }, std::move(feedback), true, true), 0);
  EXPECT_EQ(WaitForSingleObject(compatibility_event.handle, 2000), WAIT_OBJECT_0);
  client.free(0);
}

TEST(Ds5SidecarClientTests, RejectsGenshinCompatibilityWithoutSidecarCapability) {

  event_namespace_scope_t events(L"genshin-capability-required");
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + events.suffix;
  handle_scope_t continue_event(CreateEventW(nullptr, FALSE, TRUE, continue_name.c_str()));
  ASSERT_NE(continue_event.handle, nullptr);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-genshin-capability-test");
  platf::ds5::sidecar_client_t client;
  EXPECT_EQ(client.alloc({ 0, 0 }, std::move(feedback), true, true), -1);
  EXPECT_FALSE(client.owns(0));
}

TEST(Ds5SidecarClientTests, RelaunchesOnceAfterUnexpectedExit) {

  event_namespace_scope_t events(L"recover-once");
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + events.suffix;
  const auto crash_name = L"Local\\sunshine-ds5-test-crash-once-" + events.suffix;
  const auto recovered_name = L"Local\\sunshine-ds5-test-recovered-" + events.suffix;
  handle_scope_t continue_event(CreateEventW(nullptr, FALSE, FALSE, continue_name.c_str()));
  handle_scope_t crash_event(CreateEventW(nullptr, TRUE, FALSE, crash_name.c_str()));
  handle_scope_t recovered_event(CreateEventW(nullptr, FALSE, FALSE, recovered_name.c_str()));
  ASSERT_NE(continue_event.handle, nullptr);
  ASSERT_NE(crash_event.handle, nullptr);
  ASSERT_NE(recovered_event.handle, nullptr);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-recovery-test");
  auto feedback_for_test = feedback;
  platf::ds5::sidecar_client_t client;
  ASSERT_EQ(client.alloc({ 0, 0 }, std::move(feedback), false), 0);
  ASSERT_EQ(WaitForSingleObject(recovered_event.handle, 5000), WAIT_OBJECT_0);
  ASSERT_TRUE(SetEvent(continue_event.handle));
  const auto marker = feedback_for_test->pop(std::chrono::seconds(2));
  ASSERT_TRUE(marker);
  ASSERT_EQ(marker->type, platf::gamepad_feedback_e::rumble);
  EXPECT_TRUE(client.owns(0));
  client.free(0);
}

TEST(Ds5SidecarClientTests, FallsBackToHidOnlyWhenVirtualAudioBecomesDefault) {

  event_namespace_scope_t events(L"audio-policy-fallback");
  environment_scope_t enable_policy_fallback(L"SUNSHINE_DS5_TEST_AUDIO_POLICY_FALLBACK", L"1");
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + events.suffix;
  const auto policy_once_name = L"Local\\sunshine-ds5-test-policy-once-" + events.suffix;
  const auto hid_fallback_name = L"Local\\sunshine-ds5-test-hid-fallback-" + events.suffix;
  handle_scope_t continue_event(CreateEventW(nullptr, FALSE, FALSE, continue_name.c_str()));
  handle_scope_t policy_once_event(CreateEventW(nullptr, TRUE, FALSE, policy_once_name.c_str()));
  handle_scope_t hid_fallback_event(CreateEventW(nullptr, FALSE, FALSE, hid_fallback_name.c_str()));
  ASSERT_NE(continue_event.handle, nullptr);
  ASSERT_NE(policy_once_event.handle, nullptr);
  ASSERT_NE(hid_fallback_event.handle, nullptr);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-audio-policy-fallback-test");
  auto feedback_for_test = feedback;
  platf::ds5::sidecar_client_t client;
  ASSERT_EQ(client.alloc({ 0, 0 }, std::move(feedback), true), 0);
  ASSERT_EQ(WaitForSingleObject(hid_fallback_event.handle, 5000), WAIT_OBJECT_0);
  ASSERT_TRUE(SetEvent(continue_event.handle));
  const auto marker = feedback_for_test->pop(std::chrono::seconds(2));
  ASSERT_TRUE(marker);
  EXPECT_EQ(marker->type, platf::gamepad_feedback_e::rumble);
  EXPECT_TRUE(client.owns(0));
  client.free(0);
}

TEST(Ds5SidecarClientTests, FreeCancelsPendingRecovery) {

  event_namespace_scope_t events(L"cancel-recovery");
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + events.suffix;
  const auto crash_name = L"Local\\sunshine-ds5-test-crash-once-" + events.suffix;
  const auto recovery_started_name = L"Local\\sunshine-ds5-test-recovery-started-" + events.suffix;
  const auto recovery_wait_name = L"Local\\sunshine-ds5-test-recovery-wait-" + events.suffix;
  handle_scope_t continue_event(CreateEventW(nullptr, FALSE, FALSE, continue_name.c_str()));
  handle_scope_t crash_event(CreateEventW(nullptr, TRUE, FALSE, crash_name.c_str()));
  handle_scope_t recovery_started_event(CreateEventW(nullptr, FALSE, FALSE, recovery_started_name.c_str()));
  handle_scope_t recovery_wait_event(CreateEventW(nullptr, TRUE, FALSE, recovery_wait_name.c_str()));
  ASSERT_NE(continue_event.handle, nullptr);
  ASSERT_NE(crash_event.handle, nullptr);
  ASSERT_NE(recovery_started_event.handle, nullptr);
  ASSERT_NE(recovery_wait_event.handle, nullptr);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-cancel-recovery-test");
  platf::ds5::sidecar_client_t client;
  ASSERT_EQ(client.alloc({ 0, 0 }, std::move(feedback), false), 0);
  ASSERT_EQ(WaitForSingleObject(recovery_started_event.handle, 5000), WAIT_OBJECT_0);

  // The transport is offline, but the sidecar still owns the controller id.
  // The input layer relies on owns() to route release to sidecar_client_t::free().
  EXPECT_TRUE(client.owns(0));
  const auto started = std::chrono::steady_clock::now();
  client.free(0);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_LT(elapsed, std::chrono::seconds(3));
  EXPECT_FALSE(client.owns(0));
}

TEST(Ds5SidecarClientTests, ReallocatesAfterRecoveryFailure) {

  event_namespace_scope_t events(L"recovery-failure");
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + events.suffix;
  const auto crash_name = L"Local\\sunshine-ds5-test-crash-always-" + events.suffix;
  handle_scope_t continue_event(CreateEventW(nullptr, FALSE, FALSE, continue_name.c_str()));
  handle_scope_t crash_event(CreateEventW(nullptr, FALSE, FALSE, crash_name.c_str()));
  ASSERT_NE(continue_event.handle, nullptr);
  ASSERT_NE(crash_event.handle, nullptr);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-recovery-failure-test");
  platf::ds5::sidecar_client_t client;
  ASSERT_EQ(client.alloc({ 0, 0 }, std::move(feedback), false), 0);

  // Both the initial sidecar and its one recovery attempt exit immediately.
  ASSERT_EQ(WaitForSingleObject(crash_event.handle, 5000), WAIT_OBJECT_0);
  ASSERT_EQ(WaitForSingleObject(crash_event.handle, 5000), WAIT_OBJECT_0);

  int result = -1;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (unsigned attempt = 0; result < 0 && std::chrono::steady_clock::now() < deadline; ++attempt) {
    auto retry_feedback = mail->queue<platf::gamepad_feedback_msg_t>(
      "ds5-recovery-failure-retry-" + std::to_string(attempt));
    result = client.alloc({ 1, 0 }, std::move(retry_feedback), false);
    if (result < 0) {
      Sleep(10);
    }
  }
  EXPECT_EQ(result, 0);
}

#endif
