#include "rivettx/services.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace rivettx {

namespace {

bool version_is_newer(const std::string& candidate,
                      const std::string& current)
{
  const auto parse = [](const std::string& value,
                        std::array<uint32_t, 3>& parts) {
    parts = {};
    std::size_t part = 0;
    bool digit_seen = false;
    for (const char character : value) {
      if (character == '.') {
        if (!digit_seen || part + 1 >= parts.size()) {
          return false;
        }
        ++part;
        digit_seen = false;
      } else if (character >= '0' && character <= '9') {
        digit_seen = true;
        const uint32_t digit =
            static_cast<uint32_t>(character - '0');
        if (parts[part] > (UINT32_MAX - digit) / 10U) {
          return false;
        }
        parts[part] = parts[part] * 10U + digit;
      } else {
        return false;
      }
    }
    return digit_seen;
  };

  std::array<uint32_t, 3> candidate_parts{};
  std::array<uint32_t, 3> current_parts{};
  return parse(candidate, candidate_parts) &&
         parse(current, current_parts) &&
         candidate_parts > current_parts;
}

}  // namespace

void DiagnosticLog::push(LogEvent event)
{
  events_[write_index_] = event;
  write_index_ = (write_index_ + 1) % events_.size();
  size_ = std::min(events_.size(), size_ + 1);
}

std::size_t DiagnosticLog::size() const
{
  return size_;
}

bool DiagnosticLog::event_at(std::size_t chronological_index,
                             LogEvent& event) const
{
  if (chronological_index >= size_) {
    return false;
  }
  const std::size_t oldest =
      (write_index_ + events_.size() - size_) % events_.size();
  event = events_[(oldest + chronological_index) % events_.size()];
  return true;
}

void DiagnosticLog::clear()
{
  events_ = {};
  write_index_ = 0;
  size_ = 0;
}

CrashSnapshot make_crash_snapshot(uint32_t reset_reason,
                                  uint32_t control_sequence,
                                  const SafetyStatus& safety,
                                  const DiagnosticLog& log)
{
  CrashSnapshot snapshot{};
  snapshot.reset_reason = reset_reason;
  snapshot.control_sequence = control_sequence;
  snapshot.safety = safety;
  const std::size_t count =
      std::min<std::size_t>(snapshot.recent_events.size(), log.size());
  const std::size_t start = log.size() - count;
  for (std::size_t i = 0; i < count; ++i) {
    (void)log.event_at(start + i, snapshot.recent_events[i]);
  }
  snapshot.event_count = static_cast<uint8_t>(count);
  return snapshot;
}

TelemetryLogger::TelemetryLogger(ITelemetryLogSink& sink,
                                 uint32_t minimum_period_ms)
    : sink_(sink), minimum_period_ms_(minimum_period_ms)
{
}

void TelemetryLogger::start()
{
  active_ = true;
  failed_ = false;
  last_sample_us_ = 0;
}

bool TelemetryLogger::stop()
{
  active_ = false;
  const bool success = sink_.flush();
  failed_ = failed_ || !success;
  return success;
}

bool TelemetryLogger::active() const
{
  return active_;
}

bool TelemetryLogger::failed() const
{
  return failed_;
}

bool TelemetryLogger::sample(const TelemetryRegistry& telemetry,
                             TimeUs now_us)
{
  if (!active_ ||
      (last_sample_us_ != 0 &&
       now_us - last_sample_us_ <
           static_cast<TimeUs>(minimum_period_ms_) * 1000)) {
    return !failed_;
  }
  for (const auto& entry : telemetry.entries()) {
    if (entry.discovered &&
        !sink_.append(now_us, entry.id, entry.value)) {
      failed_ = true;
      active_ = false;
      return false;
    }
  }
  last_sample_us_ = now_us;
  return true;
}

bool TelemetryLogger::flush()
{
  const bool success = sink_.flush();
  failed_ = failed_ || !success;
  return success;
}

BatteryMonitor::BatteryMonitor(BatteryConfig config) : config_(config)
{
}

BatteryState BatteryMonitor::update(uint16_t sample_mv)
{
  if (sample_mv == 0) {
    return state_;
  }
  if (filtered_mv_ == 0) {
    filtered_mv_ = sample_mv;
  } else {
    const uint32_t alpha =
        clamp<uint32_t>(1, config_.filter_percent, 100);
    const int32_t difference =
        static_cast<int32_t>(sample_mv) -
        static_cast<int32_t>(filtered_mv_);
    const int32_t next =
        static_cast<int32_t>(filtered_mv_) +
        difference * static_cast<int32_t>(alpha) / 100;
    filtered_mv_ = static_cast<uint32_t>(std::max<int32_t>(0, next));
  }

  switch (state_) {
    case BatteryState::Critical:
      if (filtered_mv_ >=
          static_cast<uint32_t>(config_.critical_mv) +
              config_.hysteresis_mv) {
        state_ = filtered_mv_ <= config_.low_mv ? BatteryState::Low
                                                : BatteryState::Normal;
      }
      break;
    case BatteryState::Low:
      if (filtered_mv_ <= config_.critical_mv) {
        state_ = BatteryState::Critical;
      } else if (filtered_mv_ >=
                 static_cast<uint32_t>(config_.low_mv) +
                     config_.hysteresis_mv) {
        state_ = BatteryState::Normal;
      }
      break;
    case BatteryState::Unknown:
    case BatteryState::Normal:
      if (filtered_mv_ <= config_.critical_mv) {
        state_ = BatteryState::Critical;
      } else if (filtered_mv_ <= config_.low_mv) {
        state_ = BatteryState::Low;
      } else {
        state_ = BatteryState::Normal;
      }
      break;
  }
  return state_;
}

uint16_t BatteryMonitor::voltage_mv() const
{
  return static_cast<uint16_t>(
      std::min<uint32_t>(filtered_mv_, UINT16_MAX));
}

BatteryState BatteryMonitor::state() const
{
  return state_;
}

void AlarmEngine::set_alarm(std::size_t index, TelemetryAlarm alarm)
{
  if (index < alarms_.size()) {
    alarms_[index] = alarm;
    active_[index] = false;
    last_report_us_[index] = 0;
  }
}

bool AlarmEngine::evaluate(const TelemetryRegistry& telemetry, TimeUs now_us,
                           AlarmEvent& event)
{
  constexpr TimeUs kMaximumTelemetryAgeUs = 2000000;
  for (std::size_t i = 0; i < alarms_.size(); ++i) {
    const auto& alarm = alarms_[i];
    if (!alarm.enabled) {
      continue;
    }
    const TelemetryEntry* entry = telemetry.find(alarm.sensor_id);
    if (entry == nullptr || !entry->discovered ||
        now_us < entry->updated_at_us ||
        now_us - entry->updated_at_us > kMaximumTelemetryAgeUs) {
      if (active_[i]) {
        active_[i] = false;
        event = {alarm.sensor_id, 0, false};
        return true;
      }
      continue;
    }
    const int32_t value = entry->value;

    bool triggered = false;
    if (alarm.comparison == AlarmComparison::Below) {
      triggered = active_[i] ? value < alarm.threshold + alarm.hysteresis
                             : value < alarm.threshold;
    } else {
      triggered = active_[i] ? value > alarm.threshold - alarm.hysteresis
                             : value > alarm.threshold;
    }

    const bool changed = triggered != active_[i];
    active_[i] = triggered;
    const bool repeat_due =
        triggered &&
        (last_report_us_[i] == 0 ||
         now_us - last_report_us_[i] >=
             static_cast<TimeUs>(alarm.repeat_seconds) * 1000000);
    if (changed || repeat_due) {
      last_report_us_[i] = now_us;
      event = {alarm.sensor_id, value, triggered};
      return true;
    }
  }
  return false;
}

ModuleSupervisor::ModuleSupervisor(ICrsfTransport& transport,
                                   CrsfParser& parser,
                                   DiagnosticLog& diagnostics)
    : transport_(transport), parser_(parser), diagnostics_(diagnostics)
{
}

bool ModuleSupervisor::send(const crsf::Frame& frame, TimeUs now_us)
{
  if (status_.state == ModuleState::Passthrough) {
    return false;
  }
  const bool result = transport_.write(frame.bytes.data(), frame.size);
  if (result) {
    status_.last_transmit_us = now_us;
    ++status_.frames_sent;
  }
  return result;
}

void ModuleSupervisor::send_model_id(TimeUs now_us)
{
  (void)send(crsf::make_model_id_frame(model_id_), now_us);
}

void ModuleSupervisor::start(uint8_t model_id, TimeUs now_us)
{
  model_id_ = model_id;
  status_.state = ModuleState::Starting;
  status_.last_receive_us = 0;
  started_at_us_ = now_us;
  next_ping_us_ = now_us;
  transport_.set_baud_rate(400000);
  send_model_id(now_us);
}

void ModuleSupervisor::set_model_id(uint8_t model_id, TimeUs now_us)
{
  model_id_ = model_id;
  if (status_.state != ModuleState::Passthrough) {
    send_model_id(now_us);
  }
}

bool ModuleSupervisor::send_channels(const ChannelFrame& frame,
                                     TimeUs now_us)
{
  return send(crsf::make_channels_frame(frame), now_us);
}

void ModuleSupervisor::poll(TimeUs now_us)
{
  if (status_.state == ModuleState::Passthrough) {
    return;
  }
  std::array<uint8_t, 128> bytes{};
  const std::size_t count = transport_.read(bytes.data(), bytes.size());
  for (std::size_t i = 0; i < count; ++i) {
    (void)parser_.feed(bytes[i], now_us);
  }

  const TimeUs parser_frame = parser_.last_valid_frame_us();
  if (parser_frame != 0 && parser_frame != last_parser_frame_us_) {
    const bool recovered = status_.state == ModuleState::Offline;
    status_.last_receive_us = parser_frame;
    status_.state = ModuleState::Online;
    last_parser_frame_us_ = parser_frame;
    if (recovered) {
      diagnostics_.push(
          {now_us, LogSeverity::Info, LogCode::ModuleRecovered, 0, 0});
      send_model_id(now_us);
    }
  }

  const TimeUs receive_reference =
      status_.last_receive_us != 0 ? status_.last_receive_us : started_at_us_;
  if (now_us >= receive_reference &&
      now_us - receive_reference > 1000000) {
    if (status_.state != ModuleState::Offline) {
      status_.state = ModuleState::Offline;
      diagnostics_.push(
          {now_us, LogSeverity::Warning, LogCode::ModuleLost, 0, 0});
    }
  }

  if (now_us >= next_ping_us_) {
    (void)send(crsf::make_device_ping(), now_us);
    next_ping_us_ = now_us + 1000000;
  }
}

void ModuleSupervisor::request_bind(bool unbind, TimeUs now_us)
{
  (void)send(crsf::make_bind_frame(unbind), now_us);
}

void ModuleSupervisor::capture_failsafe(Model& model,
                                        const ChannelFrame& frame)
{
  for (std::size_t i = 0; i < kChannelCount; ++i) {
    model.outputs[i].failsafe = frame.channels[i];
  }
}

bool ModuleSupervisor::enter_passthrough(bool maintenance_allowed)
{
  if (!maintenance_allowed) {
    return false;
  }
  status_.state = ModuleState::Passthrough;
  transport_.set_baud_rate(420000);
  return true;
}

void ModuleSupervisor::leave_passthrough(uint8_t model_id, TimeUs now_us)
{
  model_id_ = model_id;
  transport_.set_baud_rate(400000);
  transport_.reset_module();
  ++status_.resets;
  status_.state = ModuleState::Starting;
  status_.last_receive_us = 0;
  start(model_id, now_us);
}

bool ModuleSupervisor::passthrough_write(const uint8_t* bytes,
                                         std::size_t size)
{
  return status_.state == ModuleState::Passthrough &&
         transport_.write(bytes, size);
}

std::size_t ModuleSupervisor::passthrough_read(uint8_t* bytes,
                                               std::size_t capacity)
{
  return status_.state == ModuleState::Passthrough
             ? transport_.read(bytes, capacity)
             : 0;
}

const ModuleStatus& ModuleSupervisor::status() const
{
  return status_;
}

void CalibrationWizard::begin(uint8_t active_axes)
{
  step_ = CalibrationStep::Center;
  active_axes_ = static_cast<uint8_t>(
      std::min<std::size_t>(active_axes, kMaxAxes));
  center_sum_ = {};
  center_samples_ = 0;
  for (std::size_t i = 0; i < calibration_.size(); ++i) {
    auto& item = calibration_[i];
    item = AxisCalibration{};
    if (i >= active_axes_) {
      continue;
    }
    item.minimum = INT16_MAX;
    item.center = 0;
    item.maximum = INT16_MIN;
  }
}

void CalibrationWizard::sample(const RawInputs& inputs)
{
  if (!inputs.valid) {
    return;
  }
  if (step_ == CalibrationStep::Center) {
    for (std::size_t i = 0; i < active_axes_; ++i) {
      center_sum_[i] += inputs.axes[i];
    }
    ++center_samples_;
  } else if (step_ == CalibrationStep::MoveExtremes) {
    for (std::size_t i = 0; i < active_axes_; ++i) {
      calibration_[i].minimum =
          std::min(calibration_[i].minimum, inputs.axes[i]);
      calibration_[i].maximum =
          std::max(calibration_[i].maximum, inputs.axes[i]);
    }
  }
}

bool CalibrationWizard::next()
{
  switch (step_) {
    case CalibrationStep::Center:
      if (center_samples_ < 10) {
        return false;
      }
      for (std::size_t i = 0; i < active_axes_; ++i) {
        calibration_[i].center =
            static_cast<int16_t>(center_sum_[i] / center_samples_);
      }
      step_ = CalibrationStep::MoveExtremes;
      return true;
    case CalibrationStep::MoveExtremes:
      for (std::size_t i = 0; i < active_axes_; ++i) {
        const auto& item = calibration_[i];
        if (item.maximum - item.minimum < 100 ||
            item.minimum >= item.center ||
            item.center >= item.maximum) {
          return false;
        }
      }
      step_ = CalibrationStep::Review;
      return true;
    case CalibrationStep::Review:
      step_ = CalibrationStep::Complete;
      return true;
    default:
      return false;
  }
}

void CalibrationWizard::cancel()
{
  step_ = CalibrationStep::Cancelled;
}

CalibrationStep CalibrationWizard::step() const
{
  return step_;
}

const std::array<AxisCalibration, kMaxAxes>& CalibrationWizard::result() const
{
  return calibration_;
}

ScriptSupervisor::ScriptSupervisor(IScriptVm& vm,
                                   DiagnosticLog& diagnostics,
                                   ScriptBudget budget)
    : vm_(vm), diagnostics_(diagnostics), budget_(budget)
{
}

ScriptSliceResult ScriptSupervisor::tick(TimeUs now_us)
{
  if (!alive_) {
    return {ScriptRunStatus::Error, 0, 0, 0};
  }
  const auto result = vm_.run_slice(budget_.maximum_instructions);
  const bool violation =
      result.elapsed_us > budget_.maximum_slice_us ||
      result.instructions > budget_.maximum_instructions ||
      result.memory_bytes > budget_.maximum_memory_bytes ||
      result.status == ScriptRunStatus::OutOfMemory;
  if (violation) {
    ++strikes_;
  } else {
    strikes_ = 0;
  }

  if (result.status == ScriptRunStatus::OutOfMemory) {
    diagnostics_.push(
        {now_us, LogSeverity::Error, LogCode::LuaOutOfMemory,
         static_cast<int32_t>(result.memory_bytes), 0});
  }
  if (result.status == ScriptRunStatus::Error ||
      strikes_ >= budget_.strikes_before_kill) {
    vm_.terminate();
    alive_ = false;
    diagnostics_.push(
        {now_us, LogSeverity::Error, LogCode::LuaKilled, strikes_, 0});
  }
  return result;
}

bool ScriptSupervisor::alive() const
{
  return alive_;
}

uint8_t ScriptSupervisor::strikes() const
{
  return strikes_;
}

BootManager::BootManager(IOtaBackend& ota, DiagnosticLog& diagnostics)
    : ota_(ota), diagnostics_(diagnostics)
{
}

bool BootManager::finish_startup(const SelfTestResult& result, TimeUs now_us)
{
  if (!result.passed()) {
    diagnostics_.push(
        {now_us, LogSeverity::Fatal, LogCode::BootSelfTestFailed, 0, 0});
    if (ota_.running_image_pending_verification()) {
      (void)ota_.request_rollback();
    }
    return false;
  }
  diagnostics_.push(
      {now_us, LogSeverity::Info, LogCode::BootSelfTestPassed, 0, 0});
  return !ota_.running_image_pending_verification() ||
         ota_.mark_running_image_valid();
}

bool BootManager::enter_recovery(bool recovery_button,
                                 uint32_t failed_boot_count) const
{
  return recovery_button || failed_boot_count >= 3;
}

UpdateManager::UpdateManager(IOtaBackend& ota, DiagnosticLog& diagnostics,
                             std::string target,
                             std::string current_version)
    : ota_(ota),
      diagnostics_(diagnostics),
      target_(std::move(target)),
      current_version_(std::move(current_version))
{
}

bool UpdateManager::install(const FirmwareManifest& manifest,
                            bool maintenance_allowed, TimeUs now_us)
{
  rejection_reason_.clear();
  if (!maintenance_allowed) {
    rejection_reason_ = "transmitter enabled";
  } else if (manifest.project != "rivettx") {
    rejection_reason_ = "wrong project";
  } else if (manifest.target != target_) {
    rejection_reason_ = "wrong hardware target";
  } else if (manifest.minimum_model_schema > Model::kSchemaVersion) {
    rejection_reason_ = "model schema too new";
  } else if (!version_is_newer(manifest.version, current_version_)) {
    rejection_reason_ = "firmware version is not newer";
  } else if (!manifest.signature_present) {
    rejection_reason_ = "missing firmware signature";
  } else if (manifest.url.rfind("https://", 0) != 0) {
    rejection_reason_ = "update URL is not HTTPS";
  }

  if (!rejection_reason_.empty()) {
    diagnostics_.push(
        {now_us, LogSeverity::Warning, LogCode::OtaRejected, 0, 0});
    return false;
  }
  diagnostics_.push(
      {now_us, LogSeverity::Info, LogCode::OtaStarted, 0, 0});
  const bool result = ota_.begin_https_update(manifest.url);
  if (result) {
    diagnostics_.push(
        {now_us, LogSeverity::Info, LogCode::OtaReady, 0, 0});
  }
  return result;
}

const std::string& UpdateManager::rejection_reason() const
{
  return rejection_reason_;
}

BackupService::BackupService(IBackupEndpoint& endpoint)
    : endpoint_(endpoint)
{
}

bool BackupService::export_file(const std::string& name,
                                const std::vector<uint8_t>& data,
                                bool maintenance_allowed)
{
  return maintenance_allowed && !name.empty() &&
         endpoint_.publish(name, data);
}

bool BackupService::import_file(const std::string& name,
                                std::vector<uint8_t>& data,
                                bool maintenance_allowed)
{
  return maintenance_allowed && !name.empty() &&
         endpoint_.receive(name, data);
}

bool SpecialFunctionEngine::switch_value(
    const SwitchRef& reference, const ControlInputs& inputs,
    const std::array<bool, kMaxLogicalSwitches>& logical_switches) const
{
  bool value = true;
  if (reference.index >= 0 &&
      reference.index < static_cast<int8_t>(kMaxSwitches)) {
    const auto index = static_cast<std::size_t>(reference.index);
    switch (reference.position) {
      case SwitchPosition::Active:
        value = inputs.switches[index];
        break;
      case SwitchPosition::Low:
        value = inputs.switch_positions[index] < 0;
        break;
      case SwitchPosition::Middle:
        value = inputs.switch_positions[index] == 0;
        break;
      case SwitchPosition::High:
        value = inputs.switch_positions[index] > 0;
        break;
    }
  } else if (reference.index >= static_cast<int8_t>(kMaxSwitches)) {
    const auto index =
        static_cast<std::size_t>(reference.index - kMaxSwitches);
    value = index < logical_switches.size() && logical_switches[index];
  }
  return reference.inverted ? !value : value;
}

void SpecialFunctionEngine::evaluate(
    const Model& model, const ControlInputs& inputs,
    const std::array<bool, kMaxLogicalSwitches>& logical_switches,
    ISpecialActionHandler& handler, TimeUs now_us)
{
  const auto count = std::min<std::size_t>(
      model.special_function_count, kMaxSpecialFunctions);
  for (std::size_t i = 0; i < count; ++i) {
    const auto& function = model.special_functions[i];
    const bool active =
        function.enabled &&
        switch_value(function.condition, inputs, logical_switches);
    if (active && !previous_[i] && function.action != SpecialAction::None) {
      handler.execute(function.action, function.parameter, now_us);
    }
    previous_[i] = active;
  }
  for (std::size_t i = count; i < previous_.size(); ++i) {
    previous_[i] = false;
  }
}

void SpecialFunctionEngine::reset()
{
  previous_ = {};
}

PowerManager::PowerManager(PowerPolicy policy) : policy_(policy)
{
}

void PowerManager::note_activity(TimeUs now_us)
{
  last_activity_us_ = now_us;
}

void PowerManager::request_shutdown()
{
  shutdown_requested_ = true;
}

PowerDecision PowerManager::evaluate(BatteryState battery,
                                     bool output_enabled, TimeUs now_us)
{
  if (last_activity_us_ == 0) {
    last_activity_us_ = now_us;
  }
  if (battery == BatteryState::Critical) {
    if (critical_since_us_ == 0) {
      critical_since_us_ = now_us;
    }
    if (now_us - critical_since_us_ >=
        static_cast<TimeUs>(policy_.critical_battery_grace_seconds) *
            1000000) {
      shutdown_requested_ = true;
    }
  } else {
    critical_since_us_ = 0;
  }

  if (shutdown_requested_) {
    return PowerDecision::LockAndShutdown;
  }
  const TimeUs inactivity_limit =
      static_cast<TimeUs>(policy_.inactivity_minutes) * 60 * 1000000;
  if (!output_enabled && now_us - last_activity_us_ >= inactivity_limit) {
    return PowerDecision::LockAndShutdown;
  }
  if (output_enabled &&
      now_us - last_activity_us_ >= inactivity_limit) {
    return PowerDecision::WarnInactivity;
  }
  return PowerDecision::StayOn;
}

bool PowerManager::shutdown_requested() const
{
  return shutdown_requested_;
}

}  // namespace rivettx
