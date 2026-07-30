#pragma once

#include "rivettx/types.hpp"

#include <array>
#include <cstdint>
#include <mutex>

namespace rivettx {

class ITelemetrySource {
 public:
  virtual ~ITelemetrySource() = default;
  virtual bool value(uint16_t sensor_id, int32_t& result) const = 0;
};

class InputProcessor {
 public:
  InputProcessor();

  void set_calibration(
      const std::array<AxisCalibration, kMaxAxes>& calibration);
  const std::array<AxisCalibration, kMaxAxes>& calibration() const;
  ControlInputs process(const RawInputs& raw);

 private:
  std::array<AxisCalibration, kMaxAxes> calibration_{};
  std::array<int32_t, kMaxAxes> filtered_{};
  std::array<int8_t, kMaxSwitches> switch_candidates_{};
  std::array<int8_t, kMaxSwitches> stable_switches_{};
  std::array<TimeUs, kMaxSwitches> switch_changed_at_us_{};
  bool initialized_ = false;
  bool switches_initialized_ = false;
};

struct TrimUpdate {
  uint8_t changed_mask = 0;
  uint8_t centered_mask = 0;
  uint8_t limit_mask = 0;

  bool changed() const
  {
    return changed_mask != 0;
  }
};

class TrimController {
 public:
  TrimUpdate update(Model& model, uint8_t flight_mode,
                    const ControlInputs& inputs, TimeUs now_us);
  void reset();

 private:
  std::array<int8_t, kTrimAxisCount> previous_direction_{};
  std::array<TimeUs, kTrimAxisCount> next_repeat_us_{};
  bool initialized_ = false;
};

class RotaryEncoderDecoder {
 public:
  int8_t update(bool phase_a, bool phase_b);
  void reset();

 private:
  uint8_t previous_state_ = 0;
  int8_t accumulator_ = 0;
  bool initialized_ = false;
};

class MixerEngine {
 public:
  MixerEngine();

  ChannelFrame evaluate(const Model& model, const ControlInputs& inputs,
                        const ITelemetrySource& telemetry, TimeUs now_us);
  const std::array<bool, kMaxLogicalSwitches>& logical_switch_values() const;
  const std::array<TimerState, kMaxTimers>& timer_states() const;
  uint8_t active_flight_mode() const;
  void reset_timer(std::size_t index);
  void reset();

 private:
  struct MixRuntime {
    bool previous_condition = false;
    bool delayed_condition = false;
    TimeUs transition_at_us = 0;
    int32_t value = 0;
  };

  struct LogicalRuntime {
    bool previous_input = false;
    bool value = false;
    TimeUs changed_at_us = 0;
    TimeUs active_until_us = 0;
  };

  int16_t source_value(const SourceRef& source, const Model& model,
                       const ControlInputs& controls,
                       const std::array<int16_t, kMaxInputs>& inputs,
                       const std::array<int32_t, kChannelCount>& channels,
                       const ITelemetrySource& telemetry,
                       uint8_t flight_mode) const;
  bool switch_value(const SwitchRef& ref, const ControlInputs& controls) const;
  bool switch_value_with_logic(const SwitchRef& ref,
                               const ControlInputs& controls) const;
  int16_t apply_curve(int16_t input, const Curve& curve) const;
  int16_t apply_expo(int16_t input, int8_t percent) const;
  void evaluate_logical_switches(
      const Model& model, const ControlInputs& controls,
      const std::array<int16_t, kMaxInputs>& virtual_inputs,
      const std::array<int32_t, kChannelCount>& previous_channels,
      const ITelemetrySource& telemetry, uint8_t flight_mode, TimeUs now_us);
  void update_timers(const Model& model, const ControlInputs& controls,
                     TimeUs now_us);

  std::array<MixRuntime, kMaxMixes> mix_runtime_{};
  std::array<LogicalRuntime, kMaxLogicalSwitches> logical_runtime_{};
  std::array<bool, kMaxLogicalSwitches> logical_values_{};
  std::array<TimerState, kMaxTimers> timer_states_{};
  std::array<bool, kMaxTimers> timer_initialized_{};
  std::array<bool, kMaxTimers> timer_persistent_{};
  std::array<int64_t, kMaxTimers> timer_start_ms_{};
  std::array<int32_t, kChannelCount> previous_channels_{};
  std::array<int16_t, kChannelCount> fade_from_channels_{};
  TimeUs previous_evaluation_us_ = 0;
  TimeUs previous_timer_us_ = 0;
  uint32_t sequence_ = 0;
  uint8_t active_flight_mode_ = 0;
  uint8_t previous_flight_mode_ = 0;
  TimeUs flight_mode_transition_us_ = 0;
  TimeUs flight_mode_fade_duration_us_ = 0;
  bool flight_mode_initialized_ = false;
};

enum class SafetyState : uint8_t {
  Booting,
  Locked,
  Ready,
  Enabled,
  Fault,
};

enum class SafetyReason : uint8_t {
  None,
  Startup,
  InputsInvalid,
  InputsStale,
  ThrottleHigh,
  SwitchMismatch,
  MixerDeadline,
  BatteryCritical,
  BatterySensor,
  StorageInvalid,
  CalibrationRequired,
  WatchdogRecovery,
  ManualLock,
};

struct SafetyConfig {
  uint32_t maximum_input_age_us = 50000;
  uint32_t maximum_mixer_duration_us = 1500;
  int16_t throttle_safe_value = -900;
  uint16_t minimum_battery_mv = 3200;
  uint8_t healthy_cycles_before_ready = 20;
};

struct SafetyStatus {
  SafetyState state = SafetyState::Booting;
  SafetyReason reason = SafetyReason::Startup;
  uint32_t missed_deadlines = 0;
  uint32_t stale_frames = 0;
  uint16_t battery_mv = 0;
};

class SafetyManager {
 public:
  explicit SafetyManager(SafetyConfig config = {});

  void boot_complete(bool storage_valid, bool watchdog_recovery,
                     bool calibration_valid = true);
  void request_enable();
  void request_lock();
  void report_battery(uint16_t millivolts);
  void report_battery_fault();
  void report_mixer_duration(uint32_t duration_us);
  ChannelFrame gate(const Model& model, const ControlInputs& inputs,
                    const ChannelFrame& proposed, TimeUs now_us);
  SafetyStatus status() const;
  bool maintenance_allowed() const;
  bool begin_maintenance();
  void end_maintenance();

 private:
  bool startup_switches_match(const Model& model,
                              const ControlInputs& inputs) const;
  ChannelFrame safe_frame(const Model& model, TimeUs now_us,
                          uint32_t sequence) const;

  SafetyConfig config_{};
  SafetyStatus status_{};
  uint8_t healthy_cycles_ = 0;
  bool mixer_deadline_pending_ = false;
  bool enable_requested_ = false;
  bool storage_valid_ = false;
  bool calibration_valid_ = false;
  bool maintenance_active_ = false;
  mutable std::mutex mutex_;
};

class IWatchdog {
 public:
  virtual ~IWatchdog() = default;
  virtual void kick() = 0;
};

struct ControlCycleResult {
  ChannelFrame frame{};
  uint32_t mixer_duration_us = 0;
  SafetyStatus safety{};
};

class ControlLoop {
 public:
  ControlLoop(InputProcessor& inputs, MixerEngine& mixer,
              SafetyManager& safety, ITelemetrySource& telemetry,
              IWatchdog& watchdog);

  ControlCycleResult run(const Model& model, const RawInputs& raw,
                         uint16_t battery_mv, TimeUs cycle_started_us,
                         TimeUs cycle_finished_us);

 private:
  InputProcessor& inputs_;
  MixerEngine& mixer_;
  SafetyManager& safety_;
  ITelemetrySource& telemetry_;
  IWatchdog& watchdog_;
};

}  // namespace rivettx
