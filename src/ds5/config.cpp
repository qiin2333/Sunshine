/**
 * @file src/ds5/config.cpp
 * @brief Independent DualSense settings persistence and runtime snapshot.
 */

#include "config.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include <boost/make_shared.hpp>
#include <boost/smart_ptr/atomic_shared_ptr.hpp>
#include <boost/thread/lock_guard.hpp>
#include <boost/thread/mutex.hpp>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

namespace ds5_config {
  namespace {
    namespace fs = std::filesystem;

    constexpr std::uintmax_t MAX_FILE_SIZE = 64 * 1024;
    boost::mutex settings_file_mutex;
    boost::atomic_shared_ptr<const settings_t> active_settings {
      boost::make_shared<const settings_t>()
    };

    void remove_temp_file(const fs::path &path) noexcept {
      if (path.empty()) return;
      std::error_code ignored;
      fs::remove(path, ignored);
    }

    bool replace_file(const fs::path &temporary_path, const fs::path &destination_path) noexcept {
#ifdef _WIN32
      return MoveFileExW(
               temporary_path.c_str(),
               destination_path.c_str(),
               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
             ) != FALSE;
#else
      std::error_code ec;
      fs::rename(temporary_path, destination_path, ec);
      return !ec;
#endif
    }

    bool finish_output(std::ofstream &file) noexcept {
      file.flush();
      if (!file.good()) {
        file.close();
        return false;
      }
      file.close();
      return !file.fail();
    }

    bool backup_existing_file(const fs::path &source_path, const fs::path &backup_path) noexcept {
      fs::path temporary_backup = backup_path;
      temporary_backup += ".tmp";
      remove_temp_file(temporary_backup);

      try {
        std::ifstream source(source_path, std::ios::binary);
        std::ofstream destination(temporary_backup, std::ios::binary | std::ios::trunc);
        if (!source.is_open() || !destination.is_open()) {
          remove_temp_file(temporary_backup);
          return false;
        }

        std::array<char, 8192> buffer {};
        while (source) {
          source.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
          const auto bytes_read = source.gcount();
          if (bytes_read > 0) destination.write(buffer.data(), bytes_read);
        }
        if (source.bad() || !finish_output(destination)) {
          remove_temp_file(temporary_backup);
          return false;
        }
        if (!replace_file(temporary_backup, backup_path)) {
          remove_temp_file(temporary_backup);
          return false;
        }
        return true;
      }
      catch (...) {
        remove_temp_file(temporary_backup);
        return false;
      }
    }
  }  // namespace

  std::filesystem::path path_for(const std::filesystem::path &sunshine_config_file) {
    return sunshine_config_file.empty() ?
             std::filesystem::path {} : sunshine_config_file.parent_path() / "ds5_config.json";
  }

  std::filesystem::path backup_path_for(const std::filesystem::path &settings_file) {
    if (settings_file.empty()) return {};
    auto backup = settings_file;
    backup += ".bak";
    return backup;
  }

  bool validate(const settings_t &settings) noexcept {
    return settings.revision > 0 &&
           (!settings.genshin_compatibility || settings.audio_haptics) &&
           std::isfinite(settings.legacy_strength) &&
           settings.legacy_strength >= MIN_STRENGTH && settings.legacy_strength <= MAX_STRENGTH &&
           std::isfinite(settings.legacy_curve) &&
           settings.legacy_curve >= MIN_CURVE && settings.legacy_curve <= MAX_CURVE &&
           std::isfinite(settings.legacy_noise_gate) &&
           settings.legacy_noise_gate >= MIN_NOISE_GATE && settings.legacy_noise_gate <= MAX_NOISE_GATE;
  }

  prepared_settings_t prepare(settings_t settings) noexcept {
    if (!validate(settings)) return {};
    try {
      return prepared_settings_t {boost::make_shared<const settings_t>(std::move(settings))};
    }
    catch (...) {
      return {};
    }
  }

  bool commit(prepared_settings_t &&settings) noexcept {
    if (!settings) return false;
    active_settings.store(std::move(settings.settings_));
    return true;
  }

  bool configure(settings_t settings) noexcept {
    return commit(prepare(std::move(settings)));
  }

  settings_t current() {
    return *active_settings.load();
  }

  load_result_t load(const std::filesystem::path &path) noexcept {
    if (path.empty()) return {load_status_t::INVALID, {}};
    try {
      boost::lock_guard<boost::mutex> lock(settings_file_mutex);
      std::error_code ec;
      if (!std::filesystem::exists(path, ec)) {
        return ec ? load_result_t {load_status_t::INVALID, {}} :
                    load_result_t {load_status_t::MISSING, {}};
      }
      const auto size = std::filesystem::file_size(path, ec);
      if (ec || size == 0 || size > MAX_FILE_SIZE) return {load_status_t::INVALID, {}};

      std::ifstream file(path, std::ios::binary);
      if (!file.is_open()) return {load_status_t::INVALID, {}};
      std::string contents(static_cast<std::size_t>(size), '\0');
      file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
      if (file.gcount() != static_cast<std::streamsize>(contents.size()) ||
          file.peek() != std::char_traits<char>::eof()) {
        return {load_status_t::INVALID, {}};
      }

      const auto input = nlohmann::json::parse(contents);
      if (!input.is_object() || (input.size() != 5 && input.size() != 6) ||
          !input.contains("ds5_audio_haptics") || !input["ds5_audio_haptics"].is_boolean() ||
          !input.contains("ds5_legacy_haptics_strength") || !input["ds5_legacy_haptics_strength"].is_number() ||
          !input.contains("ds5_legacy_haptics_curve") || !input["ds5_legacy_haptics_curve"].is_number() ||
          !input.contains("ds5_legacy_haptics_noise_gate") || !input["ds5_legacy_haptics_noise_gate"].is_number() ||
          !input.contains("ds5_genshin_compatibility") || !input["ds5_genshin_compatibility"].is_boolean() ||
          (input.size() == 6 &&
           (!input.contains("ds5_enabled") || !input["ds5_enabled"].is_boolean()))) {
        return {load_status_t::INVALID, {}};
      }

      settings_t settings {
        input["ds5_audio_haptics"].get<bool>(),
        input["ds5_legacy_haptics_strength"].get<double>(),
        input["ds5_legacy_haptics_curve"].get<double>(),
        input["ds5_legacy_haptics_noise_gate"].get<double>(),
        input["ds5_genshin_compatibility"].get<bool>(),
      };
      return validate(settings) ? load_result_t {load_status_t::LOADED, settings} :
                                  load_result_t {load_status_t::INVALID, {}};
    }
    catch (...) {
      return {load_status_t::INVALID, {}};
    }
  }

  bool save(const std::filesystem::path &path, const settings_t &settings) noexcept {
    if (path.empty() || !validate(settings)) return false;
    std::filesystem::path temporary_path;
    try {
      boost::lock_guard<boost::mutex> lock(settings_file_mutex);
      temporary_path = path;
      temporary_path += ".tmp";
      remove_temp_file(temporary_path);

      const nlohmann::json output {
        {"ds5_audio_haptics", settings.audio_haptics},
        {"ds5_legacy_haptics_strength", settings.legacy_strength},
        {"ds5_legacy_haptics_curve", settings.legacy_curve},
        {"ds5_legacy_haptics_noise_gate", settings.legacy_noise_gate},
        {"ds5_genshin_compatibility", settings.genshin_compatibility},
      };
      std::ofstream file(temporary_path, std::ios::binary | std::ios::trunc);
      if (!file.is_open()) return false;
      file << output.dump(2) << '\n';
      if (!finish_output(file)) {
        remove_temp_file(temporary_path);
        return false;
      }

      std::error_code exists_error;
      const bool destination_exists = std::filesystem::exists(path, exists_error);
      if (exists_error ||
          (destination_exists && !backup_existing_file(path, backup_path_for(path))) ||
          !replace_file(temporary_path, path)) {
        remove_temp_file(temporary_path);
        return false;
      }
      return true;
    }
    catch (...) {
      remove_temp_file(temporary_path);
      return false;
    }
  }
}  // namespace ds5_config
