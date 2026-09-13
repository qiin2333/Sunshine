/**
 * @file src/ds5/config_api.cpp
 * @brief Authenticated HTTP handlers and conditional transactions for DualSense settings.
 */

#include "config_api.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <boost/thread/lock_guard.hpp>
#include <boost/thread/mutex.hpp>

#include "src/file_handler.h"
#include "src/logging.h"

namespace ds5_config::api {
  namespace {
    using json = nlohmann::json;

    constexpr std::size_t MAX_REQUEST_SIZE = 64 * 1024;
    constexpr std::size_t MAX_ENTITY_TAG_SIZE = 512;

    // Covers disk inspection, precondition evaluation, persistence, and
    // snapshot publication. Runtime readers use only the atomic snapshot.
    boost::mutex settings_transaction_mutex;

    enum class if_match_result_t {
      MATCH,
      MISMATCH,
      INVALID,
    };

    bool same_values(const settings_t &left, const settings_t &right) noexcept {
      return left.audio_haptics == right.audio_haptics &&
             left.legacy_strength == right.legacy_strength &&
             left.legacy_curve == right.legacy_curve &&
             left.legacy_noise_gate == right.legacy_noise_gate &&
             left.genshin_compatibility == right.genshin_compatibility;
    }

    std::string make_entity_tag(const settings_t &settings, bool persisted) {
      std::ostringstream stream;
      stream.imbue(std::locale::classic());
      stream << "\"ds5-v1-" << std::dec << settings.revision << '-'
             << (persisted ? '1' : '0') << '-'
             << (settings.audio_haptics ? '1' : '0')
             << (settings.genshin_compatibility ? '1' : '0')
             << '-' << std::hex << std::setfill('0')
             << std::setw(16) << std::bit_cast<std::uint64_t>(settings.legacy_strength)
             << '-' << std::setw(16) << std::bit_cast<std::uint64_t>(settings.legacy_curve)
             << '-' << std::setw(16) << std::bit_cast<std::uint64_t>(settings.legacy_noise_gate)
             << '\"';
      return stream.str();
    }

    bool valid_entity_tag_character(unsigned char value) noexcept {
      return value == 0x21 || (value >= 0x23 && value <= 0x7e) || value >= 0x80;
    }

    if_match_result_t evaluate_if_match(
      std::string_view value,
      std::string_view current_entity_tag
    ) noexcept {
      const auto first = value.find_first_not_of(" \t");
      if (first == std::string_view::npos) return if_match_result_t::INVALID;
      const auto last = value.find_last_not_of(" \t");
      value = value.substr(first, last - first + 1);

      // This API requires the exact strong validator returned by one GET.
      // Wildcards, weak tags, lists, and duplicate header values are rejected.
      if (value.size() < 2 || value.size() > MAX_ENTITY_TAG_SIZE ||
          value.front() != '\"' || value.back() != '\"') {
        return if_match_result_t::INVALID;
      }
      for (const unsigned char character : value.substr(1, value.size() - 2)) {
        if (!valid_entity_tag_character(character)) return if_match_result_t::INVALID;
      }
      return value == current_entity_tag ?
               if_match_result_t::MATCH : if_match_result_t::MISMATCH;
    }

    config_state_t query_state_locked(const std::filesystem::path &path) {
      const auto disk = load(path);
      const auto active = current();
      const bool persisted = disk.status == load_status_t::LOADED &&
                             same_values(disk.settings, active);
      return {
        disk.status,
        active,
        persisted,
        make_entity_tag(active, persisted),
      };
    }

    std::uint64_t next_revision(std::uint64_t revision) noexcept {
      return revision == (std::numeric_limits<std::uint64_t>::max)() ? 1 : revision + 1;
    }

    SimpleWeb::CaseInsensitiveMultimap json_headers(std::string_view entity_tag = {}) {
      SimpleWeb::CaseInsensitiveMultimap headers {
        {"Content-Type", "application/json"},
        {"Cache-Control", "no-store"},
        {"X-Content-Type-Options", "nosniff"},
        {"X-Frame-Options", "DENY"},
        {"Content-Security-Policy", "frame-ancestors 'none';"},
      };
      if (!entity_tag.empty()) headers.emplace("ETag", std::string(entity_tag));
      return headers;
    }

    void write_json(
      resp_https_t response,
      SimpleWeb::StatusCode status,
      const json &body,
      std::string_view entity_tag = {}
    ) {
      response->write(status, body.dump(), json_headers(entity_tag));
    }

    json error_json(const char *message, const char *error_code = nullptr) {
      json body {{"status", false}, {"error", message}};
      if (error_code) body["error_code"] = error_code;
      return body;
    }

    void write_error(
      resp_https_t response,
      SimpleWeb::StatusCode status,
      const char *message,
      const char *error_code = nullptr
    ) {
      write_json(std::move(response), status, error_json(message, error_code));
    }

    json settings_json(
      const settings_t &settings,
      bool persisted,
      std::optional<bool> changed = std::nullopt
    ) {
      json result {
        {"status", true},
        {"applied", true},
        {"persisted", persisted},
        {"revision", settings.revision},
        {"ds5_audio_haptics", settings.audio_haptics},
        {"ds5_legacy_haptics_strength", settings.legacy_strength},
        {"ds5_legacy_haptics_curve", settings.legacy_curve},
        {"ds5_legacy_haptics_noise_gate", settings.legacy_noise_gate},
        {"ds5_genshin_compatibility", settings.genshin_compatibility},
      };
      if (changed) result["changed"] = *changed;
      return result;
    }

    bool parse_settings(const json &input, settings_t &settings) {
      if (!input.is_object() || input.size() != 5 ||
          !input.contains("ds5_audio_haptics") || !input["ds5_audio_haptics"].is_boolean() ||
          !input.contains("ds5_legacy_haptics_strength") || !input["ds5_legacy_haptics_strength"].is_number() ||
          !input.contains("ds5_legacy_haptics_curve") || !input["ds5_legacy_haptics_curve"].is_number() ||
          !input.contains("ds5_legacy_haptics_noise_gate") || !input["ds5_legacy_haptics_noise_gate"].is_number() ||
          !input.contains("ds5_genshin_compatibility") || !input["ds5_genshin_compatibility"].is_boolean()) {
        return false;
      }
      settings = {
        input["ds5_audio_haptics"].get<bool>(),
        input["ds5_legacy_haptics_strength"].get<double>(),
        input["ds5_legacy_haptics_curve"].get<double>(),
        input["ds5_legacy_haptics_noise_gate"].get<double>(),
        input["ds5_genshin_compatibility"].get<bool>(),
      };
      return validate(settings);
    }

    std::optional<std::string> request_if_match(const req_https_t &request) {
      const auto [first, last] = request->header.equal_range("If-Match");
      if (first == last) return std::nullopt;

      std::string value;
      for (auto entry = first; entry != last; ++entry) {
        if (!value.empty()) value.push_back(',');
        value.append(entry->second);
        if (value.size() > MAX_ENTITY_TAG_SIZE) break;
      }
      return value;
    }

    json precondition_error_json(
      const char *message,
      const char *error_code,
      std::uint64_t revision
    ) {
      auto body = error_json(message, error_code);
      body["revision"] = revision;
      return body;
    }

    void get_config_impl(resp_https_t response, const std::filesystem::path &path) {
      const auto state = query_state(path);
      if (state.disk_status == load_status_t::INVALID) {
        write_error(
          std::move(response),
          SimpleWeb::StatusCode::server_error_internal_server_error,
          "Failed to read DualSense configuration",
          "ds5_config_invalid"
        );
        return;
      }
      write_json(
        std::move(response),
        SimpleWeb::StatusCode::success_ok,
        settings_json(state.settings, state.persisted),
        state.entity_tag
      );
    }

    void save_config_impl(
      resp_https_t response,
      req_https_t request,
      const std::filesystem::path &path
    ) {
      if (request->content.size() > MAX_REQUEST_SIZE) {
        write_error(
          std::move(response),
          SimpleWeb::StatusCode::client_error_payload_too_large,
          "Request body is too large"
        );
        return;
      }

      const auto input = json::parse(request->content.string(), nullptr, false);
      settings_t requested;
      if (input.is_discarded() || !parse_settings(input, requested)) {
        write_error(
          std::move(response),
          SimpleWeb::StatusCode::client_error_bad_request,
          "Invalid DualSense configuration"
        );
        return;
      }

      const auto if_match_value = request_if_match(request);
      std::optional<std::string_view> if_match;
      if (if_match_value) if_match = *if_match_value;
      const auto result = update_state(path, requested, if_match);

      switch (result.status) {
        case update_status_t::APPLIED:
          BOOST_LOG(info) << "DualSense configuration saved and hot-applied at revision "
                          << result.state.settings.revision
                          << " (audio_haptics=" << result.state.settings.audio_haptics
                          << ", genshin_compatibility=" << result.state.settings.genshin_compatibility << ')';
          write_json(
            std::move(response),
            SimpleWeb::StatusCode::success_ok,
            settings_json(result.state.settings, true, true),
            result.state.entity_tag
          );
          return;
        case update_status_t::UNCHANGED:
          BOOST_LOG(debug) << "DualSense configuration was unchanged at revision "
                           << result.state.settings.revision
                           << "; no file write was needed";
          write_json(
            std::move(response),
            SimpleWeb::StatusCode::success_ok,
            settings_json(result.state.settings, result.state.persisted, false),
            result.state.entity_tag
          );
          return;
        case update_status_t::PRECONDITION_REQUIRED:
          write_json(
            std::move(response),
            SimpleWeb::StatusCode::client_error_precondition_required,
            precondition_error_json(
              "Query DualSense configuration before saving",
              "ds5_precondition_required",
              result.state.settings.revision
            )
          );
          return;
        case update_status_t::PRECONDITION_FAILED:
          write_json(
            std::move(response),
            SimpleWeb::StatusCode::client_error_precondition_failed,
            precondition_error_json(
              "DualSense configuration changed; query it again before saving",
              "ds5_precondition_failed",
              result.state.settings.revision
            )
          );
          return;
        case update_status_t::INVALID_PRECONDITION:
          write_json(
            std::move(response),
            SimpleWeb::StatusCode::client_error_bad_request,
            precondition_error_json(
              "Invalid If-Match header",
              "ds5_if_match_invalid",
              result.state.settings.revision
            )
          );
          return;
        case update_status_t::INVALID_SETTINGS:
          write_error(
            std::move(response),
            SimpleWeb::StatusCode::client_error_bad_request,
            "Invalid DualSense configuration"
          );
          return;
        case update_status_t::INVALID_STORE:
          BOOST_LOG(error) << "DualSense configuration file could not be read; save was rejected";
          write_error(
            std::move(response),
            SimpleWeb::StatusCode::server_error_internal_server_error,
            "Failed to read DualSense configuration",
            "ds5_config_invalid"
          );
          return;
        case update_status_t::SAVE_FAILED:
          BOOST_LOG(error) << "DualSense configuration could not be written to disk";
          write_error(
            std::move(response),
            SimpleWeb::StatusCode::server_error_internal_server_error,
            "Failed to write DualSense configuration"
          );
          return;
        case update_status_t::APPLY_FAILED:
          BOOST_LOG(error) << "DualSense configuration could not be published to the runtime";
          write_error(
            std::move(response),
            SimpleWeb::StatusCode::server_error_internal_server_error,
            "Failed to apply DualSense configuration"
          );
          return;
      }
    }

    void write_unhandled_error(resp_https_t response) noexcept {
      if (!response) return;
      try {
        write_error(
          std::move(response),
          SimpleWeb::StatusCode::server_error_internal_server_error,
          "DualSense request failed"
        );
      }
      catch (...) {
      }
    }
  }  // namespace

  config_state_t query_state(const std::filesystem::path &path) {
    boost::lock_guard<boost::mutex> lock(settings_transaction_mutex);
    return query_state_locked(path);
  }

  update_result_t update_state(
    const std::filesystem::path &path,
    settings_t requested,
    std::optional<std::string_view> if_match
  ) {
    boost::lock_guard<boost::mutex> lock(settings_transaction_mutex);
    auto state = query_state_locked(path);

    if (!validate(requested)) return {update_status_t::INVALID_SETTINGS, std::move(state)};
    if (state.disk_status == load_status_t::INVALID) {
      return {update_status_t::INVALID_STORE, std::move(state)};
    }
    if (!if_match) return {update_status_t::PRECONDITION_REQUIRED, std::move(state)};

    switch (evaluate_if_match(*if_match, state.entity_tag)) {
      case if_match_result_t::INVALID:
        return {update_status_t::INVALID_PRECONDITION, std::move(state)};
      case if_match_result_t::MISMATCH:
        return {update_status_t::PRECONDITION_FAILED, std::move(state)};
      case if_match_result_t::MATCH:
        break;
    }

    if (same_values(state.settings, requested)) {
      return {update_status_t::UNCHANGED, std::move(state)};
    }

    requested.revision = next_revision(state.settings.revision);
    auto prepared = prepare(requested);
    if (!prepared) return {update_status_t::APPLY_FAILED, std::move(state)};
    if (!save(path, prepared.value())) {
      return {update_status_t::SAVE_FAILED, std::move(state)};
    }
    if (!commit(std::move(prepared))) {
      return {update_status_t::APPLY_FAILED, query_state_locked(path)};
    }

    config_state_t updated {
      load_status_t::LOADED,
      requested,
      true,
      make_entity_tag(requested, true),
    };
    return {update_status_t::APPLIED, std::move(updated)};
  }

  void get_config(resp_https_t response, const std::string &sunshine_config_file) noexcept {
    try {
      get_config_impl(response, path_for(file_handler::path_from_utf8(sunshine_config_file)));
    }
    catch (...) {
      write_unhandled_error(std::move(response));
    }
  }

  void save_config(
    resp_https_t response,
    req_https_t request,
    const std::string &sunshine_config_file
  ) noexcept {
    try {
      save_config_impl(response, std::move(request), path_for(file_handler::path_from_utf8(sunshine_config_file)));
    }
    catch (...) {
      write_unhandled_error(std::move(response));
    }
  }
}  // namespace ds5_config::api
