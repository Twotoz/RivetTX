#pragma once

#include "rivettx/crsf.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rivettx {

enum class LogSeverity : uint8_t {
  Debug,
  Info,
  Warning,
  Error,
  Fatal,
};

enum class LogCode : uint16_t {
  Boot,
  BootSelfTestPassed,
  BootSelfTestFailed,
  WatchdogRecovery,
  Brownout,
  InputStale,
  MixerDeadline,
  SafetyLocked,
  SafetyEnabled,
  ModuleLost,
  ModuleRecovered,
  CrsfCrcError,
  LuaKilled,
  LuaOutOfMemory,
  StorageRecovered,
  StorageFailed,
  OtaStarted,
  OtaRejected,
  OtaReady,
  TelemetryAlarm,
  TelemetryLogFailed,
  BatteryLow,
  BatteryCritical,
};

struct LogEvent {
  TimeUs time_us = 0;
  LogSeverity severity = LogSeverity::Info;
  LogCode code = LogCode::Boot;
  int32_t argument0 = 0;
  int32_t argument1 = 0;
};

constexpr std::size_t kDiagnosticCapacity = 128;

class DiagnosticLog {
 public:
  void push(LogEvent event);
  std::size_t size() const;
  bool event_at(std::size_t chronological_index, LogEvent& event) const;
  void clear();

 private:
  std::array<LogEvent, kDiagnosticCapacity> events_{};
  std::size_t write_index_ = 0;
  std::size_t size_ = 0;
};

struct CrashSnapshot {
  uint32_t magic = 0x52564352U;  // RVCR
  uint32_t reset_reason = 0;
  uint32_t control_sequence = 0;
  SafetyStatus safety{};
  std::array<LogEvent, 32> recent_events{};
  uint8_t event_count = 0;
};

class ICrashStore {
 public:
  virtual ~ICrashStore() = default;
  virtual bool write(const CrashSnapshot& snapshot) = 0;
  virtual bool read(CrashSnapshot& snapshot) = 0;
  virtual void clear() = 0;
};

CrashSnapshot make_crash_snapshot(uint32_t reset_reason,
                                  uint32_t control_sequence,
                                  const SafetyStatus& safety,
                                  const DiagnosticLog& log);

class ITelemetryLogSink {
 public:
  virtual ~ITelemetryLogSink() = default;
  virtual bool append(TimeUs time_us, uint16_t sensor_id, int32_t value) = 0;
  virtual bool flush() = 0;
};

class TelemetryLogger {
 public:
  explicit TelemetryLogger(ITelemetryLogSink& sink,
                           uint32_t minimum_period_ms = 100);
  void start();
  bool stop();
  bool active() const;
  bool failed() const;
  bool sample(const TelemetryRegistry& telemetry, TimeUs now_us);
  bool flush();

 private:
  ITelemetryLogSink& sink_;
  uint32_t minimum_period_ms_;
  TimeUs last_sample_us_ = 0;
  bool active_ = false;
  bool failed_ = false;
};

enum class BatteryState : uint8_t {
  Unknown,
  Normal,
  Low,
  Critical,
};

struct BatteryConfig {
  uint16_t low_mv = 3500;
  uint16_t critical_mv = 3200;
  uint16_t hysteresis_mv = 100;
  uint8_t filter_percent = 10;
};

class BatteryMonitor {
 public:
  explicit BatteryMonitor(BatteryConfig config = {});
  BatteryState update(uint16_t sample_mv);
  uint16_t voltage_mv() const;
  BatteryState state() const;

 private:
  BatteryConfig config_{};
  uint32_t filtered_mv_ = 0;
  BatteryState state_ = BatteryState::Unknown;
};

enum class AlarmComparison : uint8_t {
  Below,
  Above,
};

struct TelemetryAlarm {
  bool enabled = false;
  uint16_t sensor_id = 0;
  AlarmComparison comparison = AlarmComparison::Below;
  int32_t threshold = 0;
  int32_t hysteresis = 0;
  uint16_t repeat_seconds = 10;
};

struct AlarmEvent {
  uint16_t sensor_id = 0;
  int32_t value = 0;
  bool active = false;
};

class AlarmEngine {
 public:
  static constexpr std::size_t kMaximumAlarms = 16;

  void set_alarm(std::size_t index, TelemetryAlarm alarm);
  bool evaluate(const TelemetryRegistry& telemetry, TimeUs now_us,
                AlarmEvent& event);

 private:
  std::array<TelemetryAlarm, kMaximumAlarms> alarms_{};
  std::array<bool, kMaximumAlarms> active_{};
  std::array<TimeUs, kMaximumAlarms> last_report_us_{};
};

enum class ModuleState : uint8_t {
  Offline,
  Starting,
  Online,
  Passthrough,
};

struct ModuleStatus {
  ModuleState state = ModuleState::Offline;
  TimeUs last_receive_us = 0;
  TimeUs last_transmit_us = 0;
  uint32_t resets = 0;
  uint32_t frames_sent = 0;
};

class ModuleSupervisor {
 public:
  ModuleSupervisor(ICrsfTransport& transport, CrsfParser& parser,
                   DiagnosticLog& diagnostics);

  void start(uint8_t model_id, TimeUs now_us);
  void set_model_id(uint8_t model_id, TimeUs now_us);
  bool send_channels(const ChannelFrame& frame, TimeUs now_us);
  void poll(TimeUs now_us);
  void request_bind(bool unbind, TimeUs now_us);
  void capture_failsafe(Model& model, const ChannelFrame& frame);
  bool enter_passthrough(bool maintenance_allowed);
  void leave_passthrough(uint8_t model_id, TimeUs now_us);
  bool passthrough_write(const uint8_t* bytes, std::size_t size);
  std::size_t passthrough_read(uint8_t* bytes, std::size_t capacity);
  const ModuleStatus& status() const;

 private:
  bool send(const crsf::Frame& frame, TimeUs now_us);
  void send_model_id(TimeUs now_us);

  ICrsfTransport& transport_;
  CrsfParser& parser_;
  DiagnosticLog& diagnostics_;
  ModuleStatus status_{};
  uint8_t model_id_ = 0;
  TimeUs next_ping_us_ = 0;
  TimeUs started_at_us_ = 0;
  TimeUs last_parser_frame_us_ = 0;
};

enum class CalibrationStep : uint8_t {
  Idle,
  Center,
  MoveExtremes,
  Review,
  Complete,
  Cancelled,
};

class CalibrationWizard {
 public:
  void begin(uint8_t active_axes = kMaxAxes);
  void sample(const RawInputs& inputs);
  bool next();
  void cancel();
  CalibrationStep step() const;
  const std::array<AxisCalibration, kMaxAxes>& result() const;

 private:
  CalibrationStep step_ = CalibrationStep::Idle;
  std::array<int64_t, kMaxAxes> center_sum_{};
  uint32_t center_samples_ = 0;
  uint8_t active_axes_ = kMaxAxes;
  std::array<AxisCalibration, kMaxAxes> calibration_{};
};

enum class ScriptRunStatus : uint8_t {
  Yielded,
  Completed,
  Error,
  OutOfMemory,
};

struct ScriptSliceResult {
  ScriptRunStatus status = ScriptRunStatus::Yielded;
  uint32_t instructions = 0;
  uint32_t elapsed_us = 0;
  uint32_t memory_bytes = 0;
};

class IScriptVm {
 public:
  virtual ~IScriptVm() = default;
  virtual ScriptSliceResult run_slice(uint32_t instruction_budget) = 0;
  virtual void terminate() = 0;
};

struct ScriptBudget {
  uint32_t maximum_instructions = 20000;
  uint32_t maximum_slice_us = 2000;
  uint32_t maximum_memory_bytes = 96 * 1024;
  uint8_t strikes_before_kill = 3;
};

class ScriptSupervisor {
 public:
  ScriptSupervisor(IScriptVm& vm, DiagnosticLog& diagnostics,
                   ScriptBudget budget = {});
  ScriptSliceResult tick(TimeUs now_us);
  bool alive() const;
  uint8_t strikes() const;

 private:
  IScriptVm& vm_;
  DiagnosticLog& diagnostics_;
  ScriptBudget budget_{};
  bool alive_ = true;
  uint8_t strikes_ = 0;
};

struct SelfTestResult {
  bool storage = false;
  bool inputs = false;
  bool display = false;
  bool crsf_uart = false;
  bool control_task = false;
  bool control_runtime = false;
  bool module_link = false;

  bool passed() const
  {
    return storage && inputs && display && crsf_uart && control_task &&
           control_runtime;
  }
};

class IOtaBackend {
 public:
  virtual ~IOtaBackend() = default;
  virtual bool running_image_pending_verification() const = 0;
  virtual bool mark_running_image_valid() = 0;
  virtual bool request_rollback() = 0;
  virtual bool begin_https_update(const std::string& url) = 0;
};

class BootManager {
 public:
  BootManager(IOtaBackend& ota, DiagnosticLog& diagnostics);
  bool finish_startup(const SelfTestResult& result, TimeUs now_us);
  bool enter_recovery(bool recovery_button, uint32_t failed_boot_count) const;

 private:
  IOtaBackend& ota_;
  DiagnosticLog& diagnostics_;
};

struct FirmwareManifest {
  std::string project;
  std::string target;
  std::string version;
  std::string url;
  uint16_t minimum_model_schema = 0;
  bool signature_present = false;
};

class UpdateManager {
 public:
  UpdateManager(IOtaBackend& ota, DiagnosticLog& diagnostics,
                std::string target,
                std::string current_version = "0.1.0");
  bool install(const FirmwareManifest& manifest, bool maintenance_allowed,
               TimeUs now_us);
  const std::string& rejection_reason() const;

 private:
  IOtaBackend& ota_;
  DiagnosticLog& diagnostics_;
  std::string target_;
  std::string current_version_;
  std::string rejection_reason_;
};

class IBackupEndpoint {
 public:
  virtual ~IBackupEndpoint() = default;
  virtual bool publish(const std::string& name,
                       const std::vector<uint8_t>& data) = 0;
  virtual bool receive(const std::string& name,
                       std::vector<uint8_t>& data) = 0;
};

class BackupService {
 public:
  explicit BackupService(IBackupEndpoint& endpoint);
  bool export_file(const std::string& name,
                   const std::vector<uint8_t>& data,
                   bool maintenance_allowed);
  bool import_file(const std::string& name, std::vector<uint8_t>& data,
                   bool maintenance_allowed);

 private:
  IBackupEndpoint& endpoint_;
};

class ISpecialActionHandler {
 public:
  virtual ~ISpecialActionHandler() = default;
  virtual void execute(SpecialAction action, int16_t parameter,
                       TimeUs now_us) = 0;
};

class SpecialFunctionEngine {
 public:
  void evaluate(
      const Model& model, const ControlInputs& inputs,
      const std::array<bool, kMaxLogicalSwitches>& logical_switches,
      ISpecialActionHandler& handler, TimeUs now_us);
  void reset();

 private:
  bool switch_value(
      const SwitchRef& reference, const ControlInputs& inputs,
      const std::array<bool, kMaxLogicalSwitches>& logical_switches) const;

  std::array<bool, kMaxSpecialFunctions> previous_{};
};

enum class PowerDecision : uint8_t {
  StayOn,
  WarnInactivity,
  LockAndShutdown,
};

struct PowerPolicy {
  uint16_t inactivity_minutes = 10;
  uint16_t critical_battery_grace_seconds = 5;
};

class PowerManager {
 public:
  explicit PowerManager(PowerPolicy policy = {});
  void note_activity(TimeUs now_us);
  void request_shutdown();
  PowerDecision evaluate(BatteryState battery, bool output_enabled,
                         TimeUs now_us);
  bool shutdown_requested() const;

 private:
  PowerPolicy policy_{};
  TimeUs last_activity_us_ = 0;
  TimeUs critical_since_us_ = 0;
  bool shutdown_requested_ = false;
};

}  // namespace rivettx
