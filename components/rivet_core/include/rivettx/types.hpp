#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rivettx {

constexpr std::size_t kMaxAxes = 8;
constexpr std::size_t kMaxInputs = 16;
constexpr std::size_t kChannelCount = 16;
constexpr std::size_t kMaxSwitches = 16;
constexpr std::size_t kMaxLogicalSwitches = 24;
constexpr std::size_t kMaxMixes = 64;
constexpr std::size_t kMaxCurves = 16;
constexpr std::size_t kCurvePoints = 9;
constexpr std::size_t kMaxFlightModes = 5;
constexpr std::size_t kMaxGVars = 9;
constexpr std::size_t kMaxTimers = 3;
constexpr std::size_t kMaxSpecialFunctions = 24;
constexpr std::size_t kMaxTelemetrySensors = 32;
constexpr int16_t kResolution = 1024;

using TimeUs = uint64_t;

template <typename T>
constexpr T clamp(T low, T value, T high)
{
  return value < low ? low : (value > high ? high : value);
}

struct AxisCalibration {
  int16_t minimum = 0;
  int16_t center = 2048;
  int16_t maximum = 4095;
  uint16_t deadband = 8;
  uint8_t filter_percent = 25;
  bool inverted = false;
};

struct RawInputs {
  std::array<int16_t, kMaxAxes> axes{};
  std::array<bool, kMaxSwitches> switches{};
  TimeUs sampled_at_us = 0;
  bool valid = false;
};

struct ControlInputs {
  std::array<int16_t, kMaxAxes> axes{};
  std::array<bool, kMaxSwitches> switches{};
  TimeUs sampled_at_us = 0;
  bool valid = false;
};

enum class SourceKind : uint8_t {
  Axis,
  Input,
  Channel,
  Constant,
  Telemetry,
  GVar,
};

struct SourceRef {
  SourceKind kind = SourceKind::Constant;
  uint8_t index = 0;
  int16_t constant = 0;
};

struct SwitchRef {
  // -1 means always active, 0..15 physical, 16..39 logical.
  int8_t index = -1;
  bool inverted = false;
};

struct Curve {
  bool enabled = false;
  std::array<int16_t, kCurvePoints> points{
      -1024, -768, -512, -256, 0, 256, 512, 768, 1024};
};

struct InputLine {
  bool enabled = false;
  uint8_t source_axis = 0;
  uint8_t destination = 0;
  int16_t weight_percent = 100;
  int8_t expo_percent = 0;
  int8_t curve_index = -1;
  SwitchRef condition{};
  uint8_t flight_mode_mask = 0xFF;
};

enum class MixMode : uint8_t {
  Add,
  Multiply,
  Replace,
};

struct MixLine {
  bool enabled = false;
  uint8_t destination = 0;
  SourceRef source{};
  int16_t weight_percent = 100;
  int16_t offset = 0;
  int8_t curve_index = -1;
  SwitchRef condition{};
  MixMode mode = MixMode::Add;
  uint16_t delay_up_ms = 0;
  uint16_t delay_down_ms = 0;
  uint16_t speed_up_per_second = 0;
  uint16_t speed_down_per_second = 0;
  uint8_t flight_mode_mask = 0xFF;
  bool carry_trim = true;
};

struct OutputLimit {
  int16_t minimum = -1024;
  int16_t maximum = 1024;
  int16_t subtrim = 0;
  int16_t failsafe = 0;
  bool reversed = false;
};

enum class LogicalSwitchOp : uint8_t {
  None,
  Greater,
  Less,
  Equal,
  AbsGreater,
  And,
  Or,
  Xor,
  Edge,
  Sticky,
  Timer,
};

struct LogicalSwitch {
  LogicalSwitchOp operation = LogicalSwitchOp::None;
  SourceRef lhs{};
  SourceRef rhs{};
  SwitchRef first{};
  SwitchRef second{};
  SwitchRef and_condition{};
  int16_t threshold = 0;
  uint16_t delay_ms = 0;
  uint16_t duration_ms = 0;
};

struct FlightMode {
  bool enabled = false;
  SwitchRef condition{};
  std::array<int16_t, kMaxAxes> trims{};
  std::array<int16_t, kMaxGVars> gvars{};
  uint16_t gvar_override_mask = 0;
  uint16_t fade_in_ms = 0;
  uint16_t fade_out_ms = 0;
};

enum class TimerMode : uint8_t {
  Off,
  Absolute,
  Throttle,
  Switch,
};

struct TimerConfig {
  TimerMode mode = TimerMode::Off;
  SwitchRef condition{};
  int32_t start_seconds = 0;
  bool countdown = false;
  bool persistent = false;
};

enum class SpecialAction : uint8_t {
  None,
  Bind,
  SetFailsafe,
  ResetTimer,
  StartTelemetryLog,
  StopTelemetryLog,
  InstantTrim,
  Screenshot,
  EnterModulePassthrough,
};

struct SpecialFunction {
  bool enabled = false;
  SwitchRef condition{};
  SpecialAction action = SpecialAction::None;
  int16_t parameter = 0;
};

struct Model {
  static constexpr uint16_t kSchemaVersion = 3;

  std::array<char, 24> name{};
  uint8_t model_id = 0;
  uint8_t throttle_axis = 2;
  uint8_t throttle_channel = 2;
  uint8_t required_switch_mask = 0;
  uint8_t required_switch_values = 0;
  uint8_t input_count = 0;
  uint8_t mix_count = 0;
  uint8_t logical_switch_count = 0;
  uint8_t flight_mode_count = 1;
  uint8_t curve_count = 0;
  uint8_t special_function_count = 0;
  std::array<InputLine, kMaxInputs> inputs{};
  std::array<MixLine, kMaxMixes> mixes{};
  std::array<LogicalSwitch, kMaxLogicalSwitches> logical_switches{};
  std::array<FlightMode, kMaxFlightModes> flight_modes{};
  std::array<Curve, kMaxCurves> curves{};
  std::array<int16_t, kMaxGVars> gvars{};
  std::array<OutputLimit, kChannelCount> outputs{};
  std::array<TimerConfig, kMaxTimers> timers{};
  std::array<SpecialFunction, kMaxSpecialFunctions> special_functions{};
};

struct ChannelFrame {
  std::array<int16_t, kChannelCount> channels{};
  TimeUs generated_at_us = 0;
  uint32_t sequence = 0;
  bool safe = true;
};

struct TimerState {
  int64_t elapsed_ms = 0;
  bool running = false;
};

enum class DisplayDensity : uint8_t {
  Compact,
  Medium,
  Large,
};

struct DisplayCapabilities {
  uint16_t width = 128;
  uint16_t height = 64;
  uint8_t color_depth = 1;
  bool touch = false;
  bool partial_refresh = false;
  uint8_t preferred_font_height = 8;

  DisplayDensity density() const
  {
    if (width >= 320 || height >= 240) {
      return DisplayDensity::Large;
    }
    if (width >= 160 || height >= 120) {
      return DisplayDensity::Medium;
    }
    return DisplayDensity::Compact;
  }
};

struct InputCapabilities {
  uint8_t axes = 4;
  uint8_t switches = 4;
  uint8_t buttons = 6;
  bool encoder = false;
};

struct ModuleCapabilities {
  uint32_t default_baud = 400000;
  uint32_t maximum_baud = 5250000;
  bool power_control = false;
  bool passthrough = true;
};

struct StorageCapabilities {
  uint16_t model_capacity = 32;
  uint32_t log_bytes = 64 * 1024;
};

struct PowerCapabilities {
  bool battery_sense = true;
  bool controlled_shutdown = false;
  bool charger_status = false;
};

struct HardwareProfile {
  std::array<char, 24> board_name{};
  DisplayCapabilities display{};
  InputCapabilities inputs{};
  ModuleCapabilities module{};
  StorageCapabilities storage{};
  PowerCapabilities power{};
};

Model make_default_model();

}  // namespace rivettx
