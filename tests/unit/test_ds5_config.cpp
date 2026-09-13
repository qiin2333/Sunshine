/**
 * @file tests/unit/test_ds5_config.cpp
 * @brief Tests for the standalone ds5_config.json store and runtime snapshot.
 */

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "../tests_common.h"
#include "src/ds5/config.h"
#include "src/ds5/config_api.h"

namespace {
  namespace fs = std::filesystem;

  class Ds5ConfigTest : public testing::Test {
  protected:
    void SetUp() override {
      root_ = fs::temp_directory_path() /
              ("sunshine_ds5_config_test_" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)));
      std::error_code ignored;
      fs::remove_all(root_, ignored);
      fs::create_directories(root_);
      path_ = root_ / "ds5_config.json";
      previous_ = ds5_config::current();
    }

    void TearDown() override {
      ds5_config::configure(previous_);
      std::error_code ignored;
      fs::remove_all(root_, ignored);
    }

    void write_json(const nlohmann::json &value) {
      std::ofstream file(path_, std::ios::binary | std::ios::trunc);
      file << value.dump(2) << '\n';
    }

    std::string read_text(const fs::path &path) {
      std::ifstream file(path, std::ios::binary);
      return {std::istreambuf_iterator<char> {file}, std::istreambuf_iterator<char> {}};
    }

    fs::path root_;
    fs::path path_;
    ds5_config::settings_t previous_;
  };

  nlohmann::json valid_json() {
    return {
      {"ds5_audio_haptics", false},
      {"ds5_legacy_haptics_strength", 1.5},
      {"ds5_legacy_haptics_curve", 0.5},
      {"ds5_legacy_haptics_noise_gate", 0.006},
      {"ds5_genshin_compatibility", false},
    };
  }

  bool same_values(const ds5_config::settings_t &left, const ds5_config::settings_t &right) {
    return left.audio_haptics == right.audio_haptics &&
           left.legacy_strength == right.legacy_strength &&
           left.legacy_curve == right.legacy_curve &&
           left.legacy_noise_gate == right.legacy_noise_gate &&
           left.genshin_compatibility == right.genshin_compatibility;
  }
}  // namespace

TEST_F(Ds5ConfigTest, ResolvesBesideSelectedSunshineConfig) {
  EXPECT_EQ(
    ds5_config::path_for(root_ / "sunshine.conf"),
    root_ / "ds5_config.json"
  );
  EXPECT_TRUE(ds5_config::path_for({}).empty());
}

TEST_F(Ds5ConfigTest, MissingFileReturnsDefaults) {
  const auto result = ds5_config::load(path_);
  EXPECT_EQ(result.status, ds5_config::load_status_t::MISSING);
  EXPECT_TRUE(result.settings.audio_haptics);
  EXPECT_DOUBLE_EQ(result.settings.legacy_strength, 1.0);
  EXPECT_DOUBLE_EQ(result.settings.legacy_curve, 0.5);
  EXPECT_DOUBLE_EQ(result.settings.legacy_noise_gate, 0.020);
  EXPECT_FALSE(result.settings.genshin_compatibility);
  EXPECT_EQ(result.settings.revision, 1);
}

TEST_F(Ds5ConfigTest, RejectsMalformedSchemaAndInvalidNumbers) {
  write_json({{"ds5_audio_haptics", true}});
  EXPECT_EQ(ds5_config::load(path_).status, ds5_config::load_status_t::INVALID);

  auto input = valid_json();
  input["unexpected"] = true;
  write_json(input);
  EXPECT_EQ(ds5_config::load(path_).status, ds5_config::load_status_t::INVALID);

  auto invalid = ds5_config::settings_t {};
  invalid.legacy_strength = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(ds5_config::validate(invalid));
  invalid = {};
  invalid.legacy_noise_gate = 0.061;
  EXPECT_FALSE(ds5_config::validate(invalid));
  invalid = {};
  invalid.audio_haptics = false;
  invalid.genshin_compatibility = true;
  EXPECT_FALSE(ds5_config::validate(invalid));
}

TEST_F(Ds5ConfigTest, IgnoresRetiredEnabledFieldAndDropsItOnSave) {
  auto input = valid_json();
  input["ds5_enabled"] = true;
  write_json(input);

  const auto loaded = ds5_config::load(path_);
  ASSERT_EQ(loaded.status, ds5_config::load_status_t::LOADED);
  ASSERT_TRUE(ds5_config::save(path_, loaded.settings));

  const auto saved = nlohmann::json::parse(read_text(path_));
  EXPECT_FALSE(saved.contains("ds5_enabled"));
  EXPECT_EQ(saved.size(), 5);
}

TEST_F(Ds5ConfigTest, SavesBacksUpAndReloadsCompleteSettings) {
  const ds5_config::settings_t previous {true, 1.2, 0.8, 0.010};
  auto replacement = ds5_config::settings_t {true, 2.0, 0.5, 0.006};
  replacement.genshin_compatibility = true;
  replacement.revision = 9;

  ASSERT_TRUE(ds5_config::save(path_, previous));
  const auto previous_contents = read_text(path_);
  ASSERT_TRUE(ds5_config::save(path_, replacement));
  EXPECT_EQ(read_text(ds5_config::backup_path_for(path_)), previous_contents);

  const auto loaded = ds5_config::load(path_);
  ASSERT_EQ(loaded.status, ds5_config::load_status_t::LOADED);
  EXPECT_EQ(loaded.settings.audio_haptics, replacement.audio_haptics);
  EXPECT_DOUBLE_EQ(loaded.settings.legacy_strength, replacement.legacy_strength);
  EXPECT_DOUBLE_EQ(loaded.settings.legacy_curve, replacement.legacy_curve);
  EXPECT_DOUBLE_EQ(loaded.settings.legacy_noise_gate, replacement.legacy_noise_gate);
  EXPECT_TRUE(loaded.settings.genshin_compatibility);
  // Revision describes only the current process and is not persisted.
  EXPECT_EQ(loaded.settings.revision, 1);
}

TEST_F(Ds5ConfigTest, PreparedSnapshotDoesNotPublishUntilCommit) {
  const ds5_config::settings_t active {true, 1.0, 1.0, 0.020};
  auto replacement = ds5_config::settings_t {false, 2.0, 0.5, 0.006};
  replacement.revision = active.revision + 1;
  ASSERT_TRUE(ds5_config::configure(active));

  auto prepared = ds5_config::prepare(replacement);
  ASSERT_TRUE(prepared);
  EXPECT_TRUE(ds5_config::current().audio_haptics);
  ASSERT_TRUE(ds5_config::commit(std::move(prepared)));
  EXPECT_FALSE(ds5_config::current().audio_haptics);
  EXPECT_DOUBLE_EQ(ds5_config::current().legacy_curve, 0.5);
  EXPECT_EQ(ds5_config::current().revision, replacement.revision);
}

TEST_F(Ds5ConfigTest, ConditionalUpdateRequiresTheExactQueriedStrongValidator) {
  auto initial = ds5_config::settings_t {};
  initial.revision = 7;
  ASSERT_TRUE(ds5_config::configure(initial));
  ASSERT_TRUE(ds5_config::save(path_, initial));
  const auto snapshot = ds5_config::api::query_state(path_);
  ASSERT_EQ(snapshot.disk_status, ds5_config::load_status_t::LOADED);
  ASSERT_TRUE(snapshot.persisted);
  ASSERT_FALSE(snapshot.entity_tag.empty());

  auto replacement = initial;
  replacement.audio_haptics = false;
  using status_t = ds5_config::api::update_status_t;
  EXPECT_EQ(
    ds5_config::api::update_state(path_, replacement, std::nullopt).status,
    status_t::PRECONDITION_REQUIRED
  );
  for (const auto &invalid : {
         std::string {"*"},
         "W/" + snapshot.entity_tag,
         snapshot.entity_tag + ",",
         snapshot.entity_tag + "," + snapshot.entity_tag,
       }) {
    EXPECT_EQ(
      ds5_config::api::update_state(path_, replacement, invalid).status,
      status_t::INVALID_PRECONDITION
    );
  }
  EXPECT_EQ(ds5_config::current().revision, initial.revision);
  EXPECT_TRUE(ds5_config::current().audio_haptics);
}

TEST_F(Ds5ConfigTest, ConditionalUpdateRejectsAStaleSnapshot) {
  auto initial = ds5_config::settings_t {};
  initial.revision = 7;
  ASSERT_TRUE(ds5_config::configure(initial));
  ASSERT_TRUE(ds5_config::save(path_, initial));
  const auto snapshot = ds5_config::api::query_state(path_);

  auto first = initial;
  first.legacy_strength = 1.25;
  const auto first_result = ds5_config::api::update_state(path_, first, snapshot.entity_tag);
  ASSERT_EQ(first_result.status, ds5_config::api::update_status_t::APPLIED);
  EXPECT_EQ(first_result.state.settings.revision, 8);
  EXPECT_NE(first_result.state.entity_tag, snapshot.entity_tag);

  auto stale = initial;
  stale.audio_haptics = false;
  const auto stale_result = ds5_config::api::update_state(path_, stale, snapshot.entity_tag);
  EXPECT_EQ(stale_result.status, ds5_config::api::update_status_t::PRECONDITION_FAILED);
  EXPECT_TRUE(ds5_config::current().audio_haptics);
  EXPECT_EQ(ds5_config::current().revision, 8);

  const auto disk = ds5_config::load(path_);
  ASSERT_EQ(disk.status, ds5_config::load_status_t::LOADED);
  EXPECT_TRUE(disk.settings.audio_haptics);
}

TEST_F(Ds5ConfigTest, ConditionalUpdateSkipsUnchangedPersistenceAndPublication) {
  auto initial = ds5_config::settings_t {false, 1.5, 0.5, 0.006};
  initial.revision = 11;
  ASSERT_TRUE(ds5_config::configure(initial));
  ASSERT_TRUE(ds5_config::save(path_, initial));
  const auto original_contents = read_text(path_);
  const auto snapshot = ds5_config::api::query_state(path_);
  ASSERT_FALSE(fs::exists(ds5_config::backup_path_for(path_)));

  const auto result = ds5_config::api::update_state(path_, initial, snapshot.entity_tag);
  ASSERT_EQ(result.status, ds5_config::api::update_status_t::UNCHANGED);
  EXPECT_TRUE(result.state.persisted);
  EXPECT_EQ(result.state.settings.revision, initial.revision);
  EXPECT_EQ(result.state.entity_tag, snapshot.entity_tag);
  EXPECT_EQ(ds5_config::current().revision, initial.revision);
  EXPECT_EQ(read_text(path_), original_contents);
  EXPECT_FALSE(fs::exists(ds5_config::backup_path_for(path_)));
}

TEST_F(Ds5ConfigTest, ConditionalUpdateAllowsOnlyOneConcurrentWriterPerSnapshot) {
  auto initial = ds5_config::settings_t {};
  initial.revision = 3;
  ASSERT_TRUE(ds5_config::configure(initial));
  ASSERT_TRUE(ds5_config::save(path_, initial));
  const auto snapshot = ds5_config::api::query_state(path_);

  auto first = initial;
  first.legacy_strength = 1.25;
  auto second = initial;
  second.audio_haptics = false;
  std::optional<ds5_config::api::update_result_t> first_result;
  std::optional<ds5_config::api::update_result_t> second_result;
  std::atomic_int ready {0};
  std::atomic_bool start {false};
  std::thread first_writer([&]() {
    ready.fetch_add(1, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    first_result = ds5_config::api::update_state(path_, first, snapshot.entity_tag);
  });
  std::thread second_writer([&]() {
    ready.fetch_add(1, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    second_result = ds5_config::api::update_state(path_, second, snapshot.entity_tag);
  });
  while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
  start.store(true, std::memory_order_release);
  first_writer.join();
  second_writer.join();

  ASSERT_TRUE(first_result);
  ASSERT_TRUE(second_result);
  const bool first_applied = first_result->status == ds5_config::api::update_status_t::APPLIED;
  const bool second_applied = second_result->status == ds5_config::api::update_status_t::APPLIED;
  EXPECT_NE(first_applied, second_applied);
  EXPECT_EQ(
    first_applied ? second_result->status : first_result->status,
    ds5_config::api::update_status_t::PRECONDITION_FAILED
  );

  const auto active = ds5_config::current();
  const auto expected = first_applied ? first : second;
  EXPECT_TRUE(same_values(active, expected));
  EXPECT_EQ(active.revision, initial.revision + 1);
  const auto disk = ds5_config::load(path_);
  ASSERT_EQ(disk.status, ds5_config::load_status_t::LOADED);
  EXPECT_TRUE(same_values(disk.settings, expected));
}

TEST_F(Ds5ConfigTest, ConditionalNoOpLeavesMissingDefaultStoreUntouched) {
  auto active = ds5_config::settings_t {};
  active.revision = 4;
  ASSERT_TRUE(ds5_config::configure(active));
  ASSERT_FALSE(fs::exists(path_));
  const auto snapshot = ds5_config::api::query_state(path_);
  ASSERT_EQ(snapshot.disk_status, ds5_config::load_status_t::MISSING);
  ASSERT_FALSE(snapshot.persisted);

  const auto result = ds5_config::api::update_state(path_, active, snapshot.entity_tag);
  ASSERT_EQ(result.status, ds5_config::api::update_status_t::UNCHANGED);
  EXPECT_FALSE(result.state.persisted);
  EXPECT_EQ(result.state.settings.revision, active.revision);
  EXPECT_EQ(result.state.entity_tag, snapshot.entity_tag);
  EXPECT_EQ(ds5_config::current().revision, active.revision);
  EXPECT_FALSE(fs::exists(path_));
  EXPECT_FALSE(fs::exists(ds5_config::backup_path_for(path_)));
}

TEST_F(Ds5ConfigTest, InvalidStoreBlocksConditionalUpdate) {
  auto active = ds5_config::settings_t {};
  active.revision = 5;
  ASSERT_TRUE(ds5_config::configure(active));
  write_json({{"ds5_audio_haptics", true}});
  const auto snapshot = ds5_config::api::query_state(path_);
  ASSERT_EQ(snapshot.disk_status, ds5_config::load_status_t::INVALID);

  auto replacement = active;
  replacement.legacy_strength = 1.25;
  const auto result = ds5_config::api::update_state(path_, replacement, snapshot.entity_tag);
  EXPECT_EQ(result.status, ds5_config::api::update_status_t::INVALID_STORE);
  EXPECT_DOUBLE_EQ(ds5_config::current().legacy_strength, active.legacy_strength);
  EXPECT_EQ(read_text(path_), nlohmann::json({{"ds5_audio_haptics", true}}).dump(2) + '\n');
}

TEST_F(Ds5ConfigTest, InvalidSettingsTakePrecedenceOverAnInvalidStore) {
  auto active = ds5_config::settings_t {};
  active.revision = 9;
  ASSERT_TRUE(ds5_config::configure(active));
  const auto before = ds5_config::current();
  write_json({{"ds5_audio_haptics", true}});
  const auto snapshot = ds5_config::api::query_state(path_);
  ASSERT_EQ(snapshot.disk_status, ds5_config::load_status_t::INVALID);

  auto invalid = active;
  invalid.legacy_strength = ds5_config::MAX_STRENGTH + 1.0;
  const auto result = ds5_config::api::update_state(path_, invalid, snapshot.entity_tag);
  EXPECT_EQ(result.status, ds5_config::api::update_status_t::INVALID_SETTINGS);
  const auto after = ds5_config::current();
  EXPECT_TRUE(same_values(after, before));
  EXPECT_EQ(after.revision, before.revision);
  EXPECT_EQ(after.legacy_strength, active.legacy_strength);
}

TEST_F(Ds5ConfigTest, ConcurrentReadersObserveOnlyCompleteSnapshots) {
  ds5_config::settings_t first {true, 1.0, 1.0, 0.020};
  ds5_config::settings_t second {true, 4.0, 0.3, 0.002};
  second.genshin_compatibility = true;
  first.revision = 1;
  second.revision = 2;
  ASSERT_TRUE(ds5_config::configure(first));

  std::atomic_bool running {true};
  std::atomic_bool reader_ready {false};
  std::atomic_bool first_write_complete {false};
  std::atomic_bool observed_during_writes {false};
  std::atomic_bool invalid_snapshot {false};
  std::thread reader([&]() {
    reader_ready.store(true, std::memory_order_release);
    while (!first_write_complete.load(std::memory_order_acquire) &&
           running.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    while (running.load(std::memory_order_acquire)) {
      const auto observed = ds5_config::current();
      const bool is_first = observed.audio_haptics &&
                            observed.legacy_strength == 1.0 && observed.legacy_curve == 1.0 &&
                            observed.legacy_noise_gate == 0.020 && !observed.genshin_compatibility &&
                            observed.revision == 1;
      const bool is_second = observed.audio_haptics &&
                             observed.legacy_strength == 4.0 && observed.legacy_curve == 0.3 &&
                             observed.legacy_noise_gate == 0.002 && observed.genshin_compatibility &&
                             observed.revision == 2;
      observed_during_writes.store(true, std::memory_order_release);
      if (!is_first && !is_second) {
        invalid_snapshot.store(true, std::memory_order_release);
        break;
      }
    }
  });

  while (!reader_ready.load(std::memory_order_acquire)) std::this_thread::yield();
  bool writes_succeeded = ds5_config::configure(second);
  first_write_complete.store(true, std::memory_order_release);
  while (!observed_during_writes.load(std::memory_order_acquire) &&
         !invalid_snapshot.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  for (int iteration = 1; writes_succeeded && iteration < 2000; ++iteration) {
    writes_succeeded = ds5_config::configure(iteration % 2 == 0 ? second : first);
  }
  running.store(false, std::memory_order_release);
  reader.join();
  EXPECT_TRUE(writes_succeeded);
  EXPECT_TRUE(observed_during_writes.load(std::memory_order_acquire));
  EXPECT_FALSE(invalid_snapshot.load(std::memory_order_acquire));
}
