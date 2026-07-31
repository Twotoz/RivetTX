#pragma once

#include "rivettx/elrs.hpp"
#include "rivettx/services.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rivettx {

enum class AudioAlert : uint8_t {
  CustomTone,
  Startup,
  InitializationSuccess,
  ShutdownRequest,
  MenuConfirmation,
  MenuInvalid,
  ScanCompleted,
  UpdateSuccess,
  FactoryPass,
  OutputsEnabled,
  OutputsLocked,
  TelemetryRecovered,
  BatteryRecovered,
  LinkRecovered,
  ModuleRecovered,
  BatteryLow,
  TelemetryWarning,
  ThrottleWarning,
  ArmWarning,
  SwitchPositionWarning,
  UpdateFailure,
  FactoryFail,
  SafetyFault,
  LinkWeak,
  ModuleOffline,
  TelemetryLost,
  BatteryCritical,
  LinkCritical,
  Failsafe,
  Count,
};

struct AudioSettings {
  static constexpr uint16_t kVersion = 1;
  uint16_t version = kVersion;
  bool master_enabled = true;
  bool ui_enabled = true;
  bool warnings_enabled = true;
  bool startup_enabled = true;
  bool silent_mode = false;
  bool full_mute = false;
  uint8_t intensity_percent = 60;
};

bool validate_audio_settings(AudioSettings& settings);

class AudioAlertScheduler final : public IToneOutput {
 public:
  explicit AudioAlertScheduler(IToneOutput& output);

  void notify(AudioAlert alert);
  bool configure(AudioSettings settings);
  void tick(TimeUs now_us);
  bool play_tone(uint16_t frequency_hz,
                 uint16_t duration_ms) override;
  void stop_tone() override;
  bool available() const override;
  AudioAlert current_alert() const;

 private:
  struct Note {
    uint16_t frequency_hz = 0;
    uint16_t duration_ms = 0;
    uint16_t gap_ms = 0;
  };

  struct Pattern {
    std::array<Note, 6> notes{};
    uint8_t count = 0;
  };

  static Pattern pattern_for(AudioAlert alert, uint16_t custom_frequency,
                             uint16_t custom_duration);
  bool permitted(AudioAlert alert) const;
  AudioAlert take_highest_pending();
  void begin(AudioAlert alert, TimeUs now_us);

  IToneOutput& output_;
  std::atomic<uint32_t> pending_{0};
  std::atomic<uint16_t> custom_frequency_hz_{1000};
  std::atomic<uint16_t> custom_duration_ms_{50};
  AudioAlert current_ = AudioAlert::Count;
  Pattern pattern_{};
  uint8_t note_index_ = 0;
  bool tone_active_ = false;
  TimeUs next_transition_us_ = 0;
  AudioSettings settings_{};
};

struct AudioWarningConfig {
  uint8_t link_weak_percent = 70;
  uint8_t link_critical_percent = 30;
  uint8_t link_hysteresis_percent = 8;
  uint16_t telemetry_freshness_ms = 1500;
  uint16_t weak_repeat_seconds = 10;
  uint16_t critical_repeat_seconds = 3;
  uint16_t lost_repeat_seconds = 5;
  uint16_t battery_low_repeat_seconds = 30;
  uint16_t battery_critical_repeat_seconds = 10;
  uint16_t module_offline_repeat_seconds = 10;
};

class AudioWarningMonitor {
 public:
  explicit AudioWarningMonitor(AudioWarningConfig config = {});

  void tick(const TelemetryRegistry& telemetry, BatteryState battery,
            ModuleState module, SafetyState safety, TimeUs now_us,
            AudioAlertScheduler& audio);
  void telemetry_alarm(bool active, AudioAlertScheduler& audio);

 private:
  enum class LinkState : uint8_t {
    Unknown,
    Healthy,
    Weak,
    Critical,
    Lost,
  };

  static bool repeat_due(TimeUs now_us, TimeUs& last_us,
                         uint16_t seconds);
  void evaluate_battery(BatteryState battery, TimeUs now_us,
                        AudioAlertScheduler& audio);
  void evaluate_module(ModuleState module, TimeUs now_us,
                       AudioAlertScheduler& audio);
  void evaluate_safety(SafetyState safety, AudioAlertScheduler& audio);
  void evaluate_link(const TelemetryRegistry& telemetry,
                     bool outputs_enabled, TimeUs now_us,
                     AudioAlertScheduler& audio);

  AudioWarningConfig config_{};
  BatteryState previous_battery_ = BatteryState::Unknown;
  ModuleState previous_module_ = ModuleState::Starting;
  SafetyState previous_safety_ = SafetyState::Booting;
  LinkState link_state_ = LinkState::Unknown;
  bool module_seen_online_ = false;
  TimeUs outputs_enabled_since_us_ = 0;
  TimeUs last_battery_low_us_ = 0;
  TimeUs last_battery_critical_us_ = 0;
  TimeUs last_module_offline_us_ = 0;
  TimeUs last_link_warning_us_ = 0;
};

}  // namespace rivettx
