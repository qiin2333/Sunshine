/**
 * @file src/ds5/config.h
 * @brief Independently persisted, hot-applied DualSense settings.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <utility>

#include <boost/smart_ptr/shared_ptr.hpp>

namespace ds5_config {
  inline constexpr double MIN_STRENGTH = 0.1;
  inline constexpr double MAX_STRENGTH = 4.0;
  inline constexpr double MIN_CURVE = 0.3;
  inline constexpr double MAX_CURVE = 2.0;
  inline constexpr double MIN_NOISE_GATE = 0.002;
  inline constexpr double MAX_NOISE_GATE = 0.060;

  struct settings_t {
    bool audio_haptics = true;
    double legacy_strength = 1.0;
    double legacy_curve = 0.5;
    double legacy_noise_gate = 0.020;
    bool genshin_compatibility = false;
    std::uint64_t revision = 1;
  };

  enum class load_status_t {
    LOADED,
    MISSING,
    INVALID
  };

  struct load_result_t {
    load_status_t status = load_status_t::MISSING;
    settings_t settings;
  };

  class prepared_settings_t {
  public:
    prepared_settings_t(const prepared_settings_t &) = delete;
    prepared_settings_t &operator=(const prepared_settings_t &) = delete;
    prepared_settings_t(prepared_settings_t &&) noexcept = default;
    prepared_settings_t &operator=(prepared_settings_t &&) noexcept = default;
    ~prepared_settings_t() = default;

    explicit operator bool() const noexcept {
      return settings_ != nullptr;
    }

    const settings_t &value() const noexcept {
      return *settings_;
    }

  private:
    friend prepared_settings_t prepare(settings_t settings) noexcept;
    friend bool commit(prepared_settings_t &&settings) noexcept;

    prepared_settings_t() noexcept = default;
    explicit prepared_settings_t(boost::shared_ptr<const settings_t> settings) noexcept:
        settings_(std::move(settings)) {
    }

    boost::shared_ptr<const settings_t> settings_;
  };

  std::filesystem::path path_for(const std::filesystem::path &sunshine_config_file);
  std::filesystem::path backup_path_for(const std::filesystem::path &settings_file);

  bool validate(const settings_t &settings) noexcept;
  prepared_settings_t prepare(settings_t settings) noexcept;
  bool commit(prepared_settings_t &&settings) noexcept;
  bool configure(settings_t settings) noexcept;
  settings_t current();

  load_result_t load(const std::filesystem::path &path) noexcept;
  bool save(const std::filesystem::path &path, const settings_t &settings) noexcept;
}  // namespace ds5_config
