#include "esp_platform.hpp"

#include "rivettx/audio.hpp"
#include "rivettx/board_power.hpp"
#include "rivettx/core.hpp"
#include "rivettx/crsf.hpp"
#include "rivettx/elrs.hpp"
#include "rivettx/product.hpp"
#include "rivettx/removable_storage.hpp"
#include "rivettx/services.hpp"
#include "rivettx/storage.hpp"
#include "rivettx/ui.hpp"
#include "rivettx/lua_vm.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {

using namespace rivettx;
using namespace rivettx::esp32;

constexpr char kTag[] = "rivettx";
constexpr uint32_t kControlPeriodUs = 4000;

enum class AppScreen : uint8_t {
  Home,
  Menu,
  Warnings,
  Models,
  Outputs,
  ModelSetup,
  Inputs,
  Mixes,
  Limits,
  FlightModes,
  Curves,
  Logical,
  Special,
  Timers,
  Telemetry,
  Finder,
  Elrs,
  Usb,
  Web,
  Power,
  System,
};

#if CONFIG_FREERTOS_UNICORE
constexpr BaseType_t kControlCore = 0;
#else
constexpr BaseType_t kControlCore = 1;
#endif
constexpr BaseType_t kServiceCore = 0;

BatteryConfig battery_config()
{
  BatteryConfig config{};
  config.low_mv = CONFIG_RIVETTX_BATTERY_LOW_MV;
  config.critical_mv = std::min<uint16_t>(
      CONFIG_RIVETTX_BATTERY_CRITICAL_MV,
      CONFIG_RIVETTX_BATTERY_LOW_MV - 100);
  return config;
}

SafetyConfig safety_config()
{
  SafetyConfig config{};
  config.minimum_battery_mv = battery_config().critical_mv;
  return config;
}

AudioWarningConfig audio_warning_config()
{
  AudioWarningConfig config{};
  config.link_weak_percent = CONFIG_RIVETTX_LINK_WARNING_LQ;
  config.link_critical_percent = std::min<uint8_t>(
      CONFIG_RIVETTX_LINK_CRITICAL_LQ,
      CONFIG_RIVETTX_LINK_WARNING_LQ - 1);
  return config;
}

Rtc6715Config rtc6715_config()
{
  Rtc6715Config config{};
#if CONFIG_RIVETTX_RX5808_ENABLED
  config.transition_interval_us =
      CONFIG_RIVETTX_RX5808_TRANSITION_INTERVAL_US;
  config.tune_timeout_ms = CONFIG_RIVETTX_RX5808_TUNE_TIMEOUT_MS;
#endif
  return config;
}

VrxControllerConfig vrx_controller_config()
{
  VrxControllerConfig config{};
#if CONFIG_RIVETTX_RX5808_ENABLED
  config.scan_dwell_ms = CONFIG_RIVETTX_RX5808_SCAN_DWELL_MS;
  config.tune_timeout_ms = CONFIG_RIVETTX_RX5808_TUNE_TIMEOUT_MS;
  config.rssi_sample_interval_ms =
      CONFIG_RIVETTX_RX5808_RSSI_SAMPLE_INTERVAL_MS;
  config.rssi_stale_ms = CONFIG_RIVETTX_RX5808_RSSI_STALE_MS;
  config.rssi_min_adc = CONFIG_RIVETTX_RX5808_RSSI_MIN_ADC;
  config.rssi_max_adc = CONFIG_RIVETTX_RX5808_RSSI_MAX_ADC;
  config.rssi_filter_shift = CONFIG_RIVETTX_RX5808_RSSI_FILTER_SHIFT;
  config.rssi_hysteresis_percent =
      CONFIG_RIVETTX_RX5808_RSSI_HYSTERESIS_PERCENT;
#endif
  return config;
}

BoardPowerConfig board_power_config()
{
  BoardPowerConfig config{};
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  config.sample_interval_ms = CONFIG_RIVETTX_BOARD_POWER_SAMPLE_INTERVAL_MS;
  config.default_backlight_percent =
      CONFIG_RIVETTX_BACKLIGHT_DEFAULT_PERCENT;
#endif
  return config;
}

Amt630aConfig amt630a_config()
{
  Amt630aConfig config{};
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  config.operation_timeout_ms = CONFIG_RIVETTX_AMT630A_FLASH_TIMEOUT_MS;
#endif
  return config;
}

RemovableStorageConfig removable_storage_config()
{
  RemovableStorageConfig config{};
#if CONFIG_RIVETTX_SDMMC_ENABLED
  config.detect_debounce_ms = CONFIG_RIVETTX_SD_DETECT_DEBOUNCE_MS;
  config.initial_clock_khz = CONFIG_RIVETTX_SD_INITIAL_CLOCK_KHZ;
  config.maximum_clock_khz = CONFIG_RIVETTX_SD_MAX_CLOCK_KHZ;
#endif
  return config;
}

#if CONFIG_RIVETTX_OPENPOCKET_REV_A
extern const uint8_t
    amt_image_start[] asm("_binary_amt630a_openpocket_er_tft050a3_2_bin_start");
extern const uint8_t
    amt_image_end[] asm("_binary_amt630a_openpocket_er_tft050a3_2_bin_end");
constexpr std::array<uint8_t, 32> kAmtImageSha256{
    0x9b, 0xd5, 0x53, 0x56, 0x86, 0x1b, 0xfe, 0x61,
    0xb3, 0x63, 0x13, 0xe5, 0x31, 0xaa, 0xe6, 0xbd,
    0xe8, 0xb2, 0x07, 0x13, 0x84, 0xf0, 0x08, 0xd3,
    0x53, 0x43, 0x67, 0xb3, 0x69, 0x5a, 0xff, 0x9f};
#endif

class SpecialActionMailbox final : public ISpecialActionHandler {
 public:
  void execute(SpecialAction action, int16_t parameter,
               TimeUs) override
  {
    if (count_ < actions_.size()) {
      actions_[count_++] = {action, parameter};
    }
  }

  bool pop(SpecialAction& action, int16_t& parameter)
  {
    if (read_ >= count_) {
      read_ = 0;
      count_ = 0;
      return false;
    }
    action = actions_[read_].first;
    parameter = actions_[read_].second;
    ++read_;
    return true;
  }

 private:
  std::array<std::pair<SpecialAction, int16_t>, kMaxSpecialFunctions>
      actions_{};
  std::size_t read_ = 0;
  std::size_t count_ = 0;
};

class LuaCrsfMailbox final : public ILuaCrsfSink {
 public:
  bool submit(const crsf::Frame& frame) override
  {
    bool queued = false;
    taskENTER_CRITICAL(&lock_);
    if (count_ < frames_.size()) {
      frames_[write_] = frame;
      write_ = (write_ + 1) % frames_.size();
      ++count_;
      queued = true;
    }
    taskEXIT_CRITICAL(&lock_);
    return queued;
  }

  bool pop(crsf::Frame& frame)
  {
    bool available = false;
    taskENTER_CRITICAL(&lock_);
    if (count_ != 0) {
      frame = frames_[read_];
      read_ = (read_ + 1) % frames_.size();
      --count_;
      available = true;
    }
    taskEXIT_CRITICAL(&lock_);
    return available;
  }

 private:
  std::array<crsf::Frame, 4> frames_{};
  std::size_t read_ = 0;
  std::size_t write_ = 0;
  std::size_t count_ = 0;
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
};

struct Application {
  EspBoard board;
  EspBoardPowerIo board_power_io;
  EspRemovableStorageBackend removable_storage_io{board_power_io};
  RemovableStorageService removable_storage{
      removable_storage_io, removable_storage_config()};
  BoardPowerController board_power{board_power_io, board_power_config()};
  Amt630aController display_controller{board_power_io, amt630a_config()};
  EspRx5808Io vrx_io{board};
  Rtc6715Backend vrx_hardware{vrx_io, rtc6715_config()};
  VrxController vrx{vrx_hardware, vrx_controller_config()};
  EspCrsfTransport transport;
  CrsfTransmitGate rf_transport{transport};
  Ssd1306Display display;
  EspAt7456eSpi osd_spi;
  At7456eDriver osd{osd_spi};
  EspToneOutput tones;
  EspPcmOutput pcm_output;
  EspUsbGamepad usb_gamepad;
  AudioAlertScheduler audio{tones};
  SpeakerService speaker{pcm_output};
  AudioWarningMonitor audio_warnings{audio_warning_config()};
  EspWatchdog watchdog;
  EspOtaBackend ota;
  NvsCrashStore crash_store;
  NvsBootState boot_state;
  DiagnosticLog diagnostics;
  DiagnosticLog service_diagnostics;
  DiagnosticLog boot_diagnostics;
  TelemetryRegistry telemetry;
  TelemetryRegistry service_telemetry;
  CrsfParser parser{telemetry};
  ModuleSupervisor module{rf_transport, parser, diagnostics};
  ElrsDeviceManager elrs{rf_transport, parser};
  ElrsFinder finder{audio};
  InputProcessor input_processor;
  MixerEngine mixer;
  TrimController trim_controls;
  SafetyManager safety{safety_config()};
  BatteryMonitor battery{battery_config()};
  BatterySnapshotStore battery_snapshot;
  BatteryEstimator battery_estimator{
      battery_config().critical_mv,
      static_cast<uint16_t>(battery_config().low_mv + 700)};
  PosixFileStore files{"/models"};
  TransactionalModelStore model_store{files, "active.rvm"};
  ModelLibrary model_library{files};
  CalibrationStore calibration_store{files, "calibration.bin"};
  WifiBackupPortal backup_portal{model_store, model_library, safety};
  CsvTelemetrySink telemetry_sink{"/models/telemetry.csv"};
  TelemetryLogger telemetry_logger{telemetry_sink, 100};
  AlarmEngine alarms;
  SpecialFunctionEngine special_functions;
  SpecialActionMailbox special_actions;
  PowerManager power;
  MonoCanvas canvas{128, 64};
  MonoCanvas lua_canvas{128, 64};
  LuaCrsfMailbox lua_crsf;
  LuaVm lua{service_telemetry, parser, lua_crsf, lua_canvas, &audio};
  ScriptSupervisor scripts{lua, service_diagnostics};
  UiController ui{display, canvas};
  BootManager boot{
      ota, boot_diagnostics,
#if CONFIG_RIVETTX_OPENPOCKET_OSD
      BootProductProfile::OpenPocketOsd};
#else
      BootProductProfile::StandaloneOled};
#endif
  Model model = make_default_model();
  Model edit_model = make_default_model();
  std::array<StoredModelSummary, kMaximumStoredModels> model_summaries{};
  ControlInputs latest_controls{};
  ChannelFrame latest_frame{};
  std::array<TimerState, kMaxTimers> latest_timers{};
  std::array<TelemetryEntry, kMaxTelemetrySensors> latest_telemetry{};
  ElrsManagerStatus latest_elrs{};
  ElrsFinderStatus latest_finder{};
  SafetyStatus latest_safety{};
  ModuleState latest_module_state = ModuleState::Starting;
  SafetyState latest_safety_state = SafetyState::Booting;
  portMUX_TYPE frame_lock = portMUX_INITIALIZER_UNLOCKED;
  VrxStatus latest_vrx{};
  BoardPowerStatus latest_board_power{};
  Amt630aStatus latest_display_controller{};
  portMUX_TYPE vrx_command_lock = portMUX_INITIALIZER_UNLOCKED;
  VrxCommandQueue vrx_commands{};
  std::atomic<int16_t> vrx_scan_result{-1};
  bool vrx_initialized = false;
  CharacterOsdFrame pending_osd_frame{};
  portMUX_TYPE osd_frame_lock = portMUX_INITIALIZER_UNLOCKED;
  std::atomic<uint32_t> osd_frame_generation{0};
  std::atomic<bool> osd_healthy{false};
  std::atomic<bool> osd_video_present{false};
  std::atomic<uint8_t> osd_video_standard{
      static_cast<uint8_t>(At7456eVideoStandard::Unknown)};
  uint8_t latest_buttons = 0;
  int8_t latest_encoder_delta = 0;
  bool latest_encoder_pressed = false;
  std::atomic<bool> finder_enabled{false};
  std::atomic<bool> usb_simulator_enabled{false};
  std::atomic<bool> usb_rf_lock{true};
  std::atomic<bool> display_power_requested{true};
  std::atomic<uint8_t> backlight_requested{
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
      CONFIG_RIVETTX_BACKLIGHT_DEFAULT_PERCENT};
#else
      0};
#endif
  std::atomic<bool> sd_card_present{false};
  TimeUs safety_chord_started_us = 0;
  bool safety_chord_fired = false;
  SafetyState previous_safety_state = SafetyState::Booting;
  BatteryState previous_battery_state = BatteryState::Unknown;
  bool fault_snapshot_saved = false;
  CrashSnapshot pending_snapshot{};
  std::atomic<bool> snapshot_pending{false};
  std::atomic<uint32_t> healthy_control_cycles{0};
  std::atomic<int8_t> logging_request{-1};
  std::atomic<bool> logging_active{false};
  std::atomic<bool> logging_failed{false};
  std::atomic<bool> screenshot_requested{false};
  std::atomic<bool> persist_runtime_model{false};
  Model runtime_model_to_persist = make_default_model();
  uint8_t runtime_model_slot_to_persist = 0;
  uint32_t runtime_model_generation_to_persist = 0;
  Model pending_model_activation = make_default_model();
  uint8_t pending_model_slot = 0;
  uint32_t pending_model_generation = 0;
  uint8_t active_runtime_model_slot = 0;
  uint32_t active_runtime_model_generation = 0;
  // 0 idle, 1 pending, 2 applied, -1 rejected.
  std::atomic<int8_t> model_activation_state{0};
  std::mutex runtime_model_mutex;
  std::atomic<uint32_t> last_user_activity_ms{0};
  bool calibration_valid = false;
  uint32_t generation = 0;
};

Application app;

#if CONFIG_RIVETTX_RX5808_ENABLED
bool queue_vrx_command(VrxCommandType type, uint8_t band = 0,
                       uint8_t channel = 0)
{
  bool queued = false;
  taskENTER_CRITICAL(&app.vrx_command_lock);
  queued = app.vrx_commands.push({type, band, channel});
  taskEXIT_CRITICAL(&app.vrx_command_lock);
  if (!queued) {
    ESP_LOGE(kTag, "RX5808 command queue full; command=%u rejected",
             static_cast<unsigned>(type));
  }
  return queued;
}

bool take_vrx_command(VrxCommand& command)
{
  taskENTER_CRITICAL(&app.vrx_command_lock);
  const bool pending = app.vrx_commands.pop(command);
  taskEXIT_CRITICAL(&app.vrx_command_lock);
  return pending;
}
#endif

void queue_crash_snapshot(uint32_t reason)
{
  const auto snapshot =
      make_crash_snapshot(reason, app.latest_frame.sequence,
                          app.safety.status(), app.diagnostics);
  taskENTER_CRITICAL(&app.frame_lock);
  app.pending_snapshot = snapshot;
  taskEXIT_CRITICAL(&app.frame_lock);
  app.snapshot_pending.store(true, std::memory_order_release);
}

void control_task(void*)
{
  const esp_err_t watchdog_registration = esp_task_wdt_add(nullptr);
  const bool watchdog_registered = watchdog_registration == ESP_OK;
  if (!watchdog_registered) {
    ESP_LOGE(kTag, "control watchdog registration failed: %s",
             esp_err_to_name(watchdog_registration));
    app.safety.report_watchdog_fault();
  }
  TickType_t last_wake = xTaskGetTickCount();
  TimeUs scheduled_release_us = now_us();
  const TickType_t period =
      std::max<TickType_t>(1, pdMS_TO_TICKS(kControlPeriodUs / 1000));

  while (true) {
    const TimeUs started = now_us();
    const bool simulator_active =
        app.usb_simulator_enabled.load(std::memory_order_acquire);
    // Simulator mode is a hard RF safety boundary on Revision A: ELRS power
    // is off and no CRSF frame, including an armed CH5, is transmitted.
    app.rf_transport.set_transmit_enabled(!simulator_active);
    if (simulator_active) {
      app.safety.request_lock();
    }
    if (app.model_activation_state.load(std::memory_order_acquire) == 1) {
      Model candidate{};
      bool model_changed = false;
      {
        const std::lock_guard<std::mutex> lock(app.runtime_model_mutex);
        candidate = app.pending_model_activation;
        model_changed =
            app.pending_model_slot != app.active_runtime_model_slot;
        app.active_runtime_model_slot = app.pending_model_slot;
        app.active_runtime_model_generation =
            app.pending_model_generation;
      }
      app.safety.request_lock();
      app.model = candidate;
      if (model_changed) {
        app.mixer.reset_for_model_change();
      } else {
        app.mixer.reset();
      }
      app.trim_controls.reset();
      app.special_functions.reset();
      app.module.set_model_id(app.model.model_id, started);
#if CONFIG_RIVETTX_RX5808_ENABLED
      queue_vrx_command(VrxCommandType::Tune, app.model.vrx_band,
                        app.model.vrx_channel);
#endif
      app.model_activation_state.store(2, std::memory_order_release);
    }
    const RawInputs raw = app.board.sample_inputs(started);
    const ControlInputs controls = app.input_processor.process(raw);

    const bool safety_chord = raw.switches[2] && raw.switches[3];
    if (safety_chord) {
      if (app.safety_chord_started_us == 0) {
        app.safety_chord_started_us = started;
      } else if (!app.safety_chord_fired &&
                 started - app.safety_chord_started_us >= 1000000) {
        if (app.safety.status().state == SafetyState::Enabled) {
          app.safety.request_lock();
        } else {
          app.safety.request_enable();
        }
        app.safety_chord_fired = true;
      }
    } else {
      app.safety_chord_started_us = 0;
      app.safety_chord_fired = false;
    }

    const TrimUpdate trim_update = app.trim_controls.update(
        app.model, app.mixer.active_flight_mode(), controls, started);
    if (trim_update.changed()) {
      {
        const std::lock_guard<std::mutex> lock(app.runtime_model_mutex);
        app.runtime_model_to_persist = app.model;
        app.runtime_model_slot_to_persist =
            app.active_runtime_model_slot;
        app.runtime_model_generation_to_persist =
            app.active_runtime_model_generation;
      }
      app.persist_runtime_model.store(true, std::memory_order_release);
      app.last_user_activity_ms.store(
          static_cast<uint32_t>(started / 1000), std::memory_order_release);
    }

    const ChannelFrame proposed =
        app.mixer.evaluate(app.model, controls, app.telemetry, started);
    app.special_functions.evaluate(
        app.model, controls, app.mixer.logical_switch_values(),
        app.special_actions, started);
    SpecialAction action = SpecialAction::None;
    int16_t parameter = 0;
    while (app.special_actions.pop(action, parameter)) {
      switch (action) {
        case SpecialAction::Bind:
          app.module.request_bind(false, started);
          break;
        case SpecialAction::SetFailsafe:
          app.module.capture_failsafe(app.model, proposed);
          {
            const std::lock_guard<std::mutex> lock(
                app.runtime_model_mutex);
            app.runtime_model_to_persist = app.model;
            app.runtime_model_slot_to_persist =
                app.active_runtime_model_slot;
            app.runtime_model_generation_to_persist =
                app.active_runtime_model_generation;
          }
          app.persist_runtime_model.store(true, std::memory_order_release);
          break;
        case SpecialAction::ResetTimer:
          if (parameter >= 0) {
            app.mixer.reset_timer(static_cast<std::size_t>(parameter));
          }
          break;
        case SpecialAction::StartTelemetryLog:
          app.logging_request.store(1, std::memory_order_release);
          break;
        case SpecialAction::StopTelemetryLog:
          app.logging_request.store(0, std::memory_order_release);
          break;
        case SpecialAction::InstantTrim: {
          auto& mode =
              app.model.flight_modes[app.mixer.active_flight_mode()];
          for (std::size_t axis = 0; axis < kMaxAxes; ++axis) {
            mode.trims[axis] = static_cast<int16_t>(clamp<int32_t>(
                -kResolution,
                static_cast<int32_t>(mode.trims[axis]) -
                    controls.axes[axis],
                kResolution));
          }
          {
            const std::lock_guard<std::mutex> lock(
                app.runtime_model_mutex);
            app.runtime_model_to_persist = app.model;
            app.runtime_model_slot_to_persist =
                app.active_runtime_model_slot;
            app.runtime_model_generation_to_persist =
                app.active_runtime_model_generation;
          }
          app.persist_runtime_model.store(true, std::memory_order_release);
          break;
        }
        case SpecialAction::Screenshot:
          app.screenshot_requested.store(true, std::memory_order_release);
          break;
        case SpecialAction::EnterModulePassthrough:
          (void)app.module.enter_passthrough(
              app.safety.maintenance_allowed());
          break;
        case SpecialAction::None:
          break;
      }
    }
    const TimeUs mixed_at = now_us();
    const uint32_t mixer_duration = static_cast<uint32_t>(
        mixed_at >= scheduled_release_us ? mixed_at - scheduled_release_us : 0);

    const BatterySensorSample battery_sample =
        app.board.sample_battery();
    BatteryState battery_state = BatteryState::Unknown;
    if (battery_sample.configured && battery_sample.valid) {
      battery_state = app.battery.update(battery_sample.millivolts);
    }
    if (battery_sample.sensor_fault()) {
      app.safety.report_battery_fault();
    } else {
      app.safety.report_battery(
          battery_sample.configured ? battery_sample.millivolts : 0,
          battery_sample.configured);
    }
    app.safety.report_module_ready(
        app.module.status().state == ModuleState::Online);
    app.safety.report_mixer_duration(mixer_duration);
    ChannelFrame frame =
        app.safety.gate(app.model, controls, proposed, mixed_at);

    (void)app.module.send_channels(frame, now_us());
    crsf::Frame lua_frame{};
    if (app.lua_crsf.pop(lua_frame)) {
      (void)app.rf_transport.write(
          lua_frame.bytes.data(), lua_frame.size);
    }
    app.module.poll(now_us());
    app.elrs.tick(now_us());
    if (watchdog_registered && raw.valid &&
        mixer_duration <= safety_config().maximum_mixer_duration_us) {
      app.healthy_control_cycles.fetch_add(1, std::memory_order_relaxed);
    }

    if (battery_state != app.previous_battery_state) {
      if (battery_state == BatteryState::Low) {
        app.diagnostics.push(
            {now_us(), LogSeverity::Warning, LogCode::BatteryLow,
             app.battery.voltage_mv(), 0});
      } else if (battery_state == BatteryState::Critical) {
        app.diagnostics.push(
            {now_us(), LogSeverity::Error, LogCode::BatteryCritical,
             app.battery.voltage_mv(), 0});
      }
      app.previous_battery_state = battery_state;
    }

    const SafetyStatus safety_status = app.safety.status();
    const SafetyState safety_state = safety_status.state;
    if (safety_state != app.previous_safety_state) {
      app.diagnostics.push(
          {now_us(),
           safety_state == SafetyState::Enabled ? LogSeverity::Info
                                                : LogSeverity::Warning,
           safety_state == SafetyState::Enabled ? LogCode::SafetyEnabled
                                                : LogCode::SafetyLocked,
           static_cast<int32_t>(app.safety.status().reason), 0});
      app.previous_safety_state = safety_state;
      if (safety_state == SafetyState::Fault &&
          !app.fault_snapshot_saved) {
        queue_crash_snapshot(
            static_cast<uint32_t>(app.safety.status().reason));
        app.fault_snapshot_saved = true;
      } else if (safety_state != SafetyState::Fault) {
        app.fault_snapshot_saved = false;
      }
    }

    taskENTER_CRITICAL(&app.frame_lock);
    app.latest_controls = controls;
    app.latest_frame = frame;
    app.latest_timers = app.mixer.timer_states();
    app.latest_telemetry = app.telemetry.entries();
    app.latest_elrs = app.elrs.status();
    app.latest_safety = safety_status;
    app.latest_module_state = app.module.status().state;
    app.latest_safety_state = safety_state;
    app.latest_buttons =
        static_cast<uint8_t>((raw.switches[0] ? 1U : 0U) |
                             (raw.switches[1] ? 2U : 0U) |
                             (raw.switches[2] ? 4U : 0U) |
                             (raw.switches[3] ? 8U : 0U));
    app.latest_encoder_delta = static_cast<int8_t>(clamp<int16_t>(
        -16,
        static_cast<int16_t>(app.latest_encoder_delta) + raw.encoder_delta,
        16));
    app.latest_encoder_pressed = raw.encoder_pressed;
    taskEXIT_CRITICAL(&app.frame_lock);
    const uint16_t published_battery_mv =
        battery_sample.configured && battery_sample.valid
            ? app.battery.voltage_mv()
            : static_cast<uint16_t>(0);
    app.battery_snapshot.publish(
        {published_battery_mv, battery_state,
         !battery_sample.sensor_fault(), frame.sequence});

    if (watchdog_registered) {
      app.watchdog.kick();
    }
    scheduled_release_us += kControlPeriodUs;
    vTaskDelayUntil(&last_wake, period);
  }
}

UiHomeStatus current_home_status(
    const ControlInputs& controls, const ChannelFrame& frame,
    const std::array<TelemetryEntry, kMaxTelemetrySensors>& telemetry,
    const SafetyStatus& safety, BatteryState battery_state,
    uint16_t battery_mv, bool battery_sensor_valid,
    ModuleState module_state, bool model_dirty)
{
  UiHomeStatus home{};
  for (std::size_t axis = 0; axis < home.axes.size(); ++axis) {
    home.axes[axis] = controls.axes[axis];
  }
  home.channels = frame.channels;
  home.battery_mv = battery_mv;
  const auto product_power = app.battery_estimator.estimate(
      battery_mv, battery_sensor_valid, ChargeState::Unknown, false);
  home.battery_percent = product_power.percentage;
  home.battery_percent_valid = product_power.percentage_valid;
  home.outputs_enabled = safety.state == SafetyState::Enabled;
  home.module_online = module_state == ModuleState::Online;
  home.logging = app.logging_active.load(std::memory_order_acquire);
#if CONFIG_RIVETTX_OPENPOCKET_OSD
  home.video_signal =
      app.osd_video_present.load(std::memory_order_acquire);
#else
  home.video_signal = true;
#endif
  home.vrx_band = app.edit_model.vrx_band;
  home.vrx_channel = app.edit_model.vrx_channel;

  bool link_discovered = false;
  for (const auto& sensor : telemetry) {
    if (sensor.discovered &&
        sensor.id == crsf::SensorUplinkLinkQuality) {
      home.link_quality = static_cast<uint8_t>(
          clamp<int32_t>(0, sensor.value, 100));
      link_discovered = true;
      break;
    }
  }
  const auto add_warning = [&home](UiWarningCode warning) {
    for (std::size_t index = 0; index < home.warning_count; ++index) {
      if (home.warnings[index] == warning) {
        return;
      }
    }
    if (home.warning_count < home.warnings.size()) {
      home.warnings[home.warning_count++] = warning;
    }
  };
  switch (safety.reason) {
    case SafetyReason::StorageInvalid:
      add_warning(UiWarningCode::StorageInvalid);
      break;
    case SafetyReason::CalibrationRequired:
      add_warning(UiWarningCode::CalibrationRequired);
      break;
    case SafetyReason::InputsInvalid:
      add_warning(UiWarningCode::InputInvalid);
      break;
    case SafetyReason::InputsStale:
      add_warning(UiWarningCode::InputStale);
      break;
    case SafetyReason::ThrottleHigh:
      add_warning(UiWarningCode::ThrottleHigh);
      break;
    case SafetyReason::SwitchMismatch:
      {
        bool mismatch_found = false;
        for (std::size_t index = 0; index < 8; ++index) {
          const uint8_t bit = static_cast<uint8_t>(1U << index);
          if ((app.edit_model.required_switch_mask & bit) == 0) {
            continue;
          }
          const bool expected =
              (app.edit_model.required_switch_values & bit) != 0;
          if (controls.switches[index] == expected) {
            continue;
          }
          mismatch_found = true;
          add_warning(index == kFirstAuxSwitch
                          ? UiWarningCode::ArmSwitch
                          : UiWarningCode::SwitchPosition);
        }
        if (!mismatch_found) {
          add_warning(UiWarningCode::SwitchPosition);
        }
      }
      break;
    case SafetyReason::MixerDeadline:
      add_warning(UiWarningCode::MixerDeadline);
      break;
    case SafetyReason::WatchdogRecovery:
      add_warning(UiWarningCode::WatchdogRecovery);
      break;
    case SafetyReason::WatchdogUnavailable:
      add_warning(UiWarningCode::WatchdogUnavailable);
      break;
    case SafetyReason::ModuleOffline:
      add_warning(UiWarningCode::ModuleOffline);
      break;
    case SafetyReason::BatteryCritical:
      add_warning(UiWarningCode::BatteryCritical);
      break;
    case SafetyReason::BatterySensor:
      add_warning(UiWarningCode::BatterySensor);
      break;
    case SafetyReason::None:
    case SafetyReason::Startup:
    case SafetyReason::ManualLock:
      break;
  }
  if (!app.calibration_valid) {
    add_warning(UiWarningCode::CalibrationRequired);
  }
  if (!battery_sensor_valid) {
    add_warning(UiWarningCode::BatterySensor);
  } else if (battery_state == BatteryState::Critical) {
    add_warning(UiWarningCode::BatteryCritical);
  } else if (battery_state == BatteryState::Low) {
    add_warning(UiWarningCode::BatteryLow);
  }
  if (module_state == ModuleState::Offline) {
    add_warning(UiWarningCode::ModuleOffline);
  }
  if (link_discovered) {
    if (home.link_quality == 0) {
      add_warning(UiWarningCode::LinkLost);
    } else if (home.link_quality <= CONFIG_RIVETTX_LINK_CRITICAL_LQ) {
      add_warning(UiWarningCode::LinkCritical);
    } else if (home.link_quality <= CONFIG_RIVETTX_LINK_WARNING_LQ) {
      add_warning(UiWarningCode::LinkWeak);
    }
  }
  if (app.logging_failed.load(std::memory_order_acquire)) {
    add_warning(UiWarningCode::LoggingFailed);
  }
  if (model_dirty) {
    add_warning(UiWarningCode::ModelUnsaved);
  }
#if CONFIG_RIVETTX_OPENPOCKET_OSD
  if (!home.video_signal) {
    add_warning(UiWarningCode::VideoNoSignal);
  }
#endif
  if (app.backup_portal.running() || home.logging ||
      app.usb_simulator_enabled.load(std::memory_order_acquire) ||
      app.model_activation_state.load(std::memory_order_acquire) != 0) {
    add_warning(UiWarningCode::Maintenance);
  }
  return home;
}

const char* telemetry_name(uint16_t id)
{
  switch (id) {
    case crsf::SensorUplinkRssi1:
      return "RSSI ANT1";
    case crsf::SensorUplinkRssi2:
      return "RSSI ANT2";
    case crsf::SensorUplinkLinkQuality:
      return "UPLINK LQ";
    case crsf::SensorUplinkSnr:
      return "UPLINK SNR";
    case crsf::SensorRfMode:
      return "RF MODE";
    case crsf::SensorTxPower:
      return "TX POWER";
    case crsf::SensorDownlinkRssi:
      return "DOWN RSSI";
    case crsf::SensorDownlinkLinkQuality:
      return "DOWN LQ";
    case crsf::SensorDownlinkSnr:
      return "DOWN SNR";
    case crsf::SensorBatteryVoltage:
      return "RX BATTERY";
    case crsf::SensorBatteryCurrent:
      return "RX CURRENT";
    case crsf::SensorBatteryCapacity:
      return "RX CAPACITY";
    case crsf::SensorBatteryRemaining:
      return "RX REMAIN";
    case crsf::SensorGpsLatitude:
      return "GPS LAT";
    case crsf::SensorGpsLongitude:
      return "GPS LON";
    case crsf::SensorGpsSpeed:
      return "GPS SPEED";
    case crsf::SensorGpsHeading:
      return "GPS HEADING";
    case crsf::SensorGpsAltitude:
      return "GPS ALT";
    case crsf::SensorGpsSatellites:
      return "GPS SATS";
    case crsf::SensorActiveAntenna:
      return "ACTIVE ANT";
    case crsf::SensorUplinkRssi:
      return "UPLINK RSSI";
    default:
      return "SENSOR";
  }
}

const char* telemetry_unit(TelemetryUnit unit)
{
  switch (unit) {
    case TelemetryUnit::Percent:
      return "%";
    case TelemetryUnit::Dbm:
      return "DBM";
    case TelemetryUnit::Db:
      return "DB";
    case TelemetryUnit::Milliwatt:
      return "MW";
    case TelemetryUnit::Millivolt:
      return "MV";
    case TelemetryUnit::Milliamp:
      return "MA";
    case TelemetryUnit::MilliampHour:
      return "MAH";
    case TelemetryUnit::DegreesE7:
      return "E7";
    case TelemetryUnit::Centimeters:
      return "CM";
    case TelemetryUnit::CentimetersPerSecond:
      return "CM/S";
    case TelemetryUnit::Raw:
      return "";
  }
  return "";
}

UiScreen current_screen(
    AppScreen index, const ChannelFrame& frame,
    const std::array<TimerState, kMaxTimers>& timers,
    const std::array<TelemetryEntry, kMaxTelemetrySensors>& telemetry,
    const ElrsManagerStatus& elrs, const ElrsFinderStatus& finder,
    const SafetyStatus& safety, BatteryState battery_state,
    uint16_t battery_mv, bool battery_sensor_valid,
    const UiHomeStatus& home)
{
  switch (index) {
    case AppScreen::Home:
      return make_oled_home_screen(app.edit_model, home);
    case AppScreen::Menu:
      return make_main_menu_screen();
    case AppScreen::Warnings:
      return make_warnings_screen(home);
    case AppScreen::Models: {
      UiScreen screen{"models", "Models", {}};
      for (const auto& summary : app.model_summaries) {
        if (!summary.present) {
          continue;
        }
        const std::string slot = std::to_string(summary.slot);
        screen.fields.push_back(
            {"select." + slot, summary.name.data(),
             summary.slot == app.model_library.active_slot()
                 ? "ACTIVE"
                 : "ID " + std::to_string(summary.model_id),
             UiFieldKind::Action, summary.slot, 0,
             static_cast<int32_t>(kMaximumStoredModels - 1),
             false, true});
        if (summary.slot != app.model_library.active_slot()) {
          screen.fields.push_back(
              {"delete." + slot, "DELETE " + std::string(summary.name.data()),
               "ENTER", UiFieldKind::Action, summary.slot, 0,
               static_cast<int32_t>(kMaximumStoredModels - 1),
               false, true});
        }
      }
      screen.fields.push_back(
          {"new", "NEW MODEL", "ENTER", UiFieldKind::Action,
           0, 0, 1, false, true});
      screen.fields.push_back(
          {"copy", "COPY ACTIVE", "ENTER", UiFieldKind::Action,
           0, 0, 1, false, true});
      return screen;
    }
    case AppScreen::Outputs:
      return make_outputs_screen(frame);
    case AppScreen::ModelSetup:
      return make_model_setup_screen(app.edit_model);
    case AppScreen::Inputs:
      return make_inputs_screen(app.edit_model);
    case AppScreen::Mixes:
      return make_mixes_screen(app.edit_model);
    case AppScreen::Limits:
      return make_output_limits_screen(app.edit_model);
    case AppScreen::FlightModes:
      return make_flight_modes_screen(app.edit_model);
    case AppScreen::Curves:
      return make_curves_screen(app.edit_model);
    case AppScreen::Logical:
      return make_logical_switches_screen(app.edit_model);
    case AppScreen::Special:
      return make_special_functions_screen(app.edit_model);
    case AppScreen::Timers:
      return make_timers_screen(app.edit_model, timers);
    case AppScreen::Telemetry: {
      std::vector<UiField> sensors;
      for (const auto& sensor : telemetry) {
        if (sensor.discovered) {
          sensors.push_back(
              {"sensor." + std::to_string(sensor.id),
               telemetry_name(sensor.id),
               std::to_string(sensor.value) + telemetry_unit(sensor.unit),
               UiFieldKind::Label, sensor.value, 0, 0, false, true});
        }
      }
      if (sensors.empty()) {
        sensors.push_back(
            {"none", "NO SENSORS", "", UiFieldKind::Label, 0, 0, 0,
             false, true});
      }
      return make_telemetry_screen(sensors);
    }
    case AppScreen::Finder:
      return make_elrs_finder_screen(finder);
    case AppScreen::Elrs:
      return make_elrs_screen(elrs, app.safety.maintenance_allowed());
    case AppScreen::Usb: {
      UiScreen screen{"usb", "USB Simulator", {}};
      const bool supported = app.usb_gamepad.supported();
      const bool active =
          app.usb_simulator_enabled.load(std::memory_order_acquire);
      screen.fields.push_back(
          {"support", "USB GAMEPAD",
           !supported
               ? "S3 REQUIRED"
               : (app.usb_gamepad.mounted() ? "HOST CONNECTED"
                                            : "WAITING HOST"),
           UiFieldKind::Label,
           0, 0, 0, false, true});
      screen.fields.push_back(
          {"mode", "SIM MODE", active ? "ACTIVE" : "OFF",
           UiFieldKind::Label, active ? 1 : 0, 0, 1, false, true});
      screen.fields.push_back(
          {"rf_lock", "RF SAFETY LOCK", "", UiFieldKind::Boolean,
           app.edit_model.simulator_rf_lock ? 1 : 0,
           0, 1, !active, true});
      screen.fields.push_back(
          {active ? "disable" : "enable",
           active ? "STOP SIMULATOR" : "START SIMULATOR",
           supported ? "ENTER" : "UNAVAILABLE", UiFieldKind::Action,
           0, 0, 1, false, supported});
      return screen;
    }
    case AppScreen::Web: {
      UiScreen screen{"web", "Web Config", {}};
      const bool running = app.backup_portal.running();
      screen.fields.push_back(
          {"state", "CONFIG WIFI", running ? "ACTIVE" : "OFF",
           UiFieldKind::Label, running ? 1 : 0, 0, 1, false, true});
      if (running) {
        screen.fields.push_back(
            {"ssid", "NETWORK", "RivetTX-Recovery",
             UiFieldKind::Label, 0, 0, 0, false, true});
        screen.fields.push_back(
            {"url", "OPEN", "192.168.4.1", UiFieldKind::Label,
             0, 0, 0, false, true});
      }
      screen.fields.push_back(
          {running ? "stop" : "start",
           running ? "STOP WEB CONFIG" : "START WEB CONFIG",
           "ENTER", UiFieldKind::Action, 0, 0, 1, false, true});
      return screen;
    }
    case AppScreen::Power: {
      UiScreen screen{"power", "Power", {}};
      BoardPowerStatus board_power{};
      taskENTER_CRITICAL(&app.frame_lock);
      board_power = app.latest_board_power;
      taskEXIT_CRITICAL(&app.frame_lock);
      const bool gauge_valid =
          board_power.fuel_gauge.state == BoardSensorState::Valid;
      const uint16_t effective_battery_mv =
          gauge_valid ? board_power.fuel_gauge.cell_mv : battery_mv;
      const bool effective_battery_valid = gauge_valid || battery_sensor_valid;
      const auto product_power = app.battery_estimator.estimate(
          effective_battery_mv, effective_battery_valid,
          board_power.charger.charge, board_power.vbus_present);
      screen.fields.push_back(
          {"voltage", "BATTERY",
           effective_battery_valid
               ? std::to_string(effective_battery_mv) + "MV"
               : "UNAVAILABLE",
           UiFieldKind::Label, effective_battery_mv, 0, 0, false, true});
      screen.fields.push_back(
          {"percentage", "CHARGE",
           gauge_valid
               ? std::to_string(board_power.fuel_gauge.state_of_charge) + "%"
               : (product_power.percentage_valid
                      ? std::to_string(product_power.percentage) + "%"
                      : "UNKNOWN"),
           UiFieldKind::Label,
           gauge_valid ? board_power.fuel_gauge.state_of_charge
                       : product_power.percentage,
           0, 100,
           false, true});
      screen.fields.push_back(
          {"state", "STATE",
           !battery_sensor_valid
               ? "SENSOR ERROR"
               : (battery_state == BatteryState::Critical
                      ? "CRITICAL"
                      : (battery_state == BatteryState::Low ? "LOW" : "OK")),
           UiFieldKind::Label, 0, 0, 0, false, true});
      screen.fields.push_back(
          {"hardware", "FUEL GAUGE",
           gauge_valid ? "MAX17048 OK" : "UNAVAILABLE",
           UiFieldKind::Label, gauge_valid ? 1 : 0, 0, 1, false, true});
      screen.fields.push_back(
          {"usb", "USB VBUS", board_power.vbus_present ? "PRESENT" : "OFF",
           UiFieldKind::Label, board_power.vbus_present ? 1 : 0,
           0, 1, false, true});
      screen.fields.push_back(
          {"display", "DISPLAY POWER", "", UiFieldKind::Boolean,
           app.display_power_requested.load(std::memory_order_acquire) ? 1 : 0,
           0, 1, true, true});
      screen.fields.push_back(
          {"backlight", "BACKLIGHT", "", UiFieldKind::Progress,
           app.backlight_requested.load(std::memory_order_acquire),
           0, 100, true, true});
      return screen;
    }
    case AppScreen::System:
      return make_system_screen(
          battery_mv, esp_get_free_heap_size(),
          safety.missed_deadlines, esp_app_get_description()->version);
  }
  return make_oled_home_screen(app.edit_model, home);
}

#if CONFIG_RIVETTX_OPENPOCKET_OSD
class LiveOpenPocketScreens final : public IOpenPocketScreenProvider {
 public:
  UiScreen screen(OpenPocketPage page) override
  {
    if (page == OpenPocketPage::Home) {
      return make_openpocket_home_screen(app.edit_model, home);
    }
    if (page == OpenPocketPage::Video) {
      return make_openpocket_video_screen(vrx);
    }
    AppScreen target = AppScreen::Home;
    switch (page) {
      case OpenPocketPage::Warnings:
        target = AppScreen::Warnings;
        break;
      case OpenPocketPage::Models:
        target = AppScreen::Models;
        break;
      case OpenPocketPage::Outputs:
        target = AppScreen::Outputs;
        break;
      case OpenPocketPage::ModelSetup:
        target = AppScreen::ModelSetup;
        break;
      case OpenPocketPage::Inputs:
        target = AppScreen::Inputs;
        break;
      case OpenPocketPage::Mixes:
        target = AppScreen::Mixes;
        break;
      case OpenPocketPage::Limits:
        target = AppScreen::Limits;
        break;
      case OpenPocketPage::FlightModes:
        target = AppScreen::FlightModes;
        break;
      case OpenPocketPage::Curves:
        target = AppScreen::Curves;
        break;
      case OpenPocketPage::LogicalSwitches:
        target = AppScreen::Logical;
        break;
      case OpenPocketPage::SpecialFunctions:
        target = AppScreen::Special;
        break;
      case OpenPocketPage::Timers:
        target = AppScreen::Timers;
        break;
      case OpenPocketPage::Elrs:
        target = AppScreen::Elrs;
        break;
      case OpenPocketPage::Finder:
        target = AppScreen::Finder;
        break;
      case OpenPocketPage::Usb:
        target = AppScreen::Usb;
        break;
      case OpenPocketPage::Web:
        target = AppScreen::Web;
        break;
      case OpenPocketPage::Telemetry:
        target = AppScreen::Telemetry;
        break;
      case OpenPocketPage::Power:
        target = AppScreen::Power;
        break;
      case OpenPocketPage::System:
        target = AppScreen::System;
        break;
      case OpenPocketPage::Video:
      case OpenPocketPage::Home:
      case OpenPocketPage::MainMenu:
      case OpenPocketPage::ModelMenu:
      case OpenPocketPage::RadioMenu:
      case OpenPocketPage::ElrsMenu:
      case OpenPocketPage::VideoMenu:
      case OpenPocketPage::UsbMenu:
      case OpenPocketPage::DiagnosticsMenu:
      case OpenPocketPage::SystemMenu:
        return {};
    }
    return current_screen(target, frame, timers, telemetry, elrs, finder,
                          safety, battery_state, battery_mv,
                          battery_sensor_valid, home);
  }

  ControlInputs controls{};
  ChannelFrame frame{};
  std::array<TimerState, kMaxTimers> timers{};
  std::array<TelemetryEntry, kMaxTelemetrySensors> telemetry{};
  ElrsManagerStatus elrs{};
  ElrsFinderStatus finder{};
  SafetyStatus safety{};
  UiHomeStatus home{};
  VrxStatus vrx{};
  BatteryState battery_state = BatteryState::Unknown;
  uint16_t battery_mv = 0;
  bool battery_sensor_valid = false;
};

AppScreen app_screen_for(OpenPocketPage page)
{
  switch (page) {
    case OpenPocketPage::Warnings: return AppScreen::Warnings;
    case OpenPocketPage::Models: return AppScreen::Models;
    case OpenPocketPage::Outputs: return AppScreen::Outputs;
    case OpenPocketPage::ModelSetup: return AppScreen::ModelSetup;
    case OpenPocketPage::Inputs: return AppScreen::Inputs;
    case OpenPocketPage::Mixes: return AppScreen::Mixes;
    case OpenPocketPage::Limits: return AppScreen::Limits;
    case OpenPocketPage::FlightModes: return AppScreen::FlightModes;
    case OpenPocketPage::Curves: return AppScreen::Curves;
    case OpenPocketPage::LogicalSwitches: return AppScreen::Logical;
    case OpenPocketPage::SpecialFunctions: return AppScreen::Special;
    case OpenPocketPage::Timers: return AppScreen::Timers;
    case OpenPocketPage::Elrs: return AppScreen::Elrs;
    case OpenPocketPage::Finder: return AppScreen::Finder;
    case OpenPocketPage::Usb: return AppScreen::Usb;
    case OpenPocketPage::Web: return AppScreen::Web;
    case OpenPocketPage::Telemetry: return AppScreen::Telemetry;
    case OpenPocketPage::Power: return AppScreen::Power;
    case OpenPocketPage::System: return AppScreen::System;
    case OpenPocketPage::Video: return AppScreen::Menu;
    case OpenPocketPage::Home: return AppScreen::Home;
    default: return AppScreen::Menu;
  }
}
#endif

bool run_startup_calibration()
{
  CalibrationWizard wizard;
  wizard.begin(app.board.configured_axis_count());
#if CONFIG_RIVETTX_OPENPOCKET_OSD
  CharacterOsdComposer calibration_osd;
  UiHomeStatus calibration_home{};
  VrxStatus calibration_vrx{};
  app.osd.start(now_us());
#endif
  bool previous_enter = false;
  bool previous_back = false;
  bool buttons_released = false;

  while (wizard.step() != CalibrationStep::Complete &&
         wizard.step() != CalibrationStep::Cancelled) {
    const RawInputs raw = app.board.sample_inputs(now_us());
    const bool enter = raw.switches[2];
    const bool back = raw.switches[3];
    buttons_released =
        buttons_released || (!raw.switches[0] && !raw.switches[1]);

    if (buttons_released) {
      wizard.sample(raw);
      if (back && !previous_back) {
        wizard.cancel();
      } else if (enter && !previous_enter) {
        (void)wizard.next();
      }
    }
    previous_enter = enter;
    previous_back = back;

    const auto step = wizard.step();
    const uint8_t progress =
        step == CalibrationStep::Center
            ? 25
            : (step == CalibrationStep::MoveExtremes
                   ? 60
                   : (step == CalibrationStep::Review ? 90 : 100));
    const UiScreen calibration_screen =
        make_calibration_screen(static_cast<uint8_t>(step), progress);
#if CONFIG_RIVETTX_OPENPOCKET_OSD
    calibration_osd.compose(calibration_screen, calibration_home,
                            calibration_vrx, 0, 0, false);
    app.osd.submit(calibration_osd.frame());
    app.osd.tick(now_us());
#else
    app.ui.set_screen(calibration_screen);
    (void)app.ui.render();
#endif
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  if (wizard.step() != CalibrationStep::Complete ||
      !app.calibration_store.save(wizard.result())) {
    return false;
  }
  app.input_processor.set_calibration(wizard.result());
  return true;
}

bool menu_target(const std::string& id, AppScreen& target)
{
  const std::array<std::pair<const char*, AppScreen>, 19> targets{{
      {"warnings", AppScreen::Warnings},
      {"models", AppScreen::Models},
      {"model", AppScreen::ModelSetup},
      {"inputs", AppScreen::Inputs},
      {"mixes", AppScreen::Mixes},
      {"outputs", AppScreen::Outputs},
      {"limits", AppScreen::Limits},
      {"flight_modes", AppScreen::FlightModes},
      {"curves", AppScreen::Curves},
      {"logical", AppScreen::Logical},
      {"special", AppScreen::Special},
      {"timers", AppScreen::Timers},
      {"elrs", AppScreen::Elrs},
      {"finder", AppScreen::Finder},
      {"usb", AppScreen::Usb},
      {"web", AppScreen::Web},
      {"telemetry", AppScreen::Telemetry},
      {"power", AppScreen::Power},
      {"system", AppScreen::System},
  }};
  for (const auto& item : targets) {
    if (id == item.first) {
      target = item.second;
      return true;
    }
  }
  return false;
}

bool parse_slot_action(const std::string& id, const char* prefix,
                       uint8_t& slot)
{
  const std::string marker = std::string(prefix) + ".";
  if (id.rfind(marker, 0) != 0) {
    return false;
  }
  const char* value = id.c_str() + marker.size();
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed >= kMaximumStoredModels) {
    return false;
  }
  slot = static_cast<uint8_t>(parsed);
  return true;
}

void ui_task(void*)
{
  AppScreen screen = AppScreen::Home;
#if CONFIG_RIVETTX_OPENPOCKET_OSD
  LiveOpenPocketScreens openpocket_screens;
  OpenPocketMenuController openpocket_ui(openpocket_screens);
  bool openpocket_started = false;
#endif
  uint8_t previous_buttons = 0;
  bool previous_encoder_pressed = false;
  bool model_dirty = false;
  bool activation_needed = false;
  bool activation_maintenance = false;
  bool runtime_update_during_activation = false;
  bool runtime_update_requires_activation = false;
  bool usb_maintenance = false;
#if !CONFIG_RIVETTX_OPENPOCKET_OSD
  bool screen_rendered = false;
  bool display_failed = false;
#endif
  bool force_screen_rebuild = true;
#if CONFIG_RIVETTX_OPENPOCKET_OSD
  (void)force_screen_rebuild;
#endif
#if !CONFIG_RIVETTX_OPENPOCKET_OSD
  AppScreen rendered_screen = AppScreen::Home;
  TimeUs next_live_refresh_us = 0;
#endif
  TimeUs dirty_since_us = 0;
  while (true) {
    ControlInputs controls{};
    ChannelFrame frame{};
    std::array<TimerState, kMaxTimers> timers{};
    std::array<TelemetryEntry, kMaxTelemetrySensors> telemetry{};
    ElrsManagerStatus elrs{};
    ElrsFinderStatus finder{};
    SafetyStatus safety{};
#if CONFIG_RIVETTX_OPENPOCKET_OSD
    VrxStatus vrx_status{};
#endif
    const BatterySnapshot battery_snapshot =
        app.battery_snapshot.read();
    BatteryState battery_state = battery_snapshot.state;
    ModuleState module_state = ModuleState::Starting;
    uint16_t battery_mv = battery_snapshot.millivolts;
    bool battery_sensor_valid = battery_snapshot.sensor_valid;
    uint8_t buttons = 0;
    int8_t encoder_delta = 0;
    bool encoder_pressed = false;
    taskENTER_CRITICAL(&app.frame_lock);
    controls = app.latest_controls;
    frame = app.latest_frame;
    timers = app.latest_timers;
    telemetry = app.latest_telemetry;
    elrs = app.latest_elrs;
    finder = app.latest_finder;
    safety = app.latest_safety;
#if CONFIG_RIVETTX_RX5808_ENABLED
    vrx_status = app.latest_vrx;
#endif
    module_state = app.latest_module_state;
    buttons = app.latest_buttons;
    encoder_delta = app.latest_encoder_delta;
    app.latest_encoder_delta = 0;
    encoder_pressed = app.latest_encoder_pressed;
    taskEXIT_CRITICAL(&app.frame_lock);

#if CONFIG_RIVETTX_RX5808_ENABLED
    const int16_t scan_result = app.vrx_scan_result.exchange(
        -1, std::memory_order_acq_rel);
    if (scan_result >= 0 &&
        scan_result <
            static_cast<int16_t>(kVrxBandCount * kVrxChannelsPerBand)) {
      app.edit_model.vrx_band = static_cast<uint8_t>(
          scan_result / static_cast<int16_t>(kVrxChannelsPerBand));
      app.edit_model.vrx_channel = static_cast<uint8_t>(
          scan_result % static_cast<int16_t>(kVrxChannelsPerBand));
      model_dirty = true;
      force_screen_rebuild = true;
      dirty_since_us = now_us();
    }
#endif

    const uint8_t pressed =
        static_cast<uint8_t>(buttons & ~previous_buttons);
    previous_buttons = buttons;
    const bool encoder_press =
        encoder_pressed && !previous_encoder_pressed;
    previous_encoder_pressed = encoder_pressed;
    if (pressed != 0 || encoder_delta != 0 || encoder_press) {
      app.last_user_activity_ms.store(
          static_cast<uint32_t>(now_us() / 1000),
          std::memory_order_release);
    }
    if (app.persist_runtime_model.exchange(false,
                                           std::memory_order_acq_rel)) {
      Model runtime_model{};
      uint8_t runtime_slot = 0;
      uint32_t runtime_generation = 0;
      {
        const std::lock_guard<std::mutex> lock(app.runtime_model_mutex);
        runtime_model = app.runtime_model_to_persist;
        runtime_slot = app.runtime_model_slot_to_persist;
        runtime_generation = app.runtime_model_generation_to_persist;
      }
      if (runtime_slot != app.model_library.active_slot()) {
        std::string error;
        if (!app.model_library.save_slot(
                runtime_slot, runtime_model, runtime_generation + 1,
                error)) {
          ESP_LOGE(kTag, "previous model runtime save failed: %s",
                   error.c_str());
          const std::lock_guard<std::mutex> lock(app.runtime_model_mutex);
          app.runtime_model_to_persist = runtime_model;
          app.runtime_model_slot_to_persist = runtime_slot;
          app.runtime_model_generation_to_persist = runtime_generation;
          app.persist_runtime_model.store(true,
                                          std::memory_order_release);
        } else {
          app.model_summaries = app.model_library.summaries();
        }
      } else if (activation_needed ||
                 runtime_generation != app.generation) {
        for (std::size_t mode = 0; mode < kMaxFlightModes; ++mode) {
          app.edit_model.flight_modes[mode].trims =
              runtime_model.flight_modes[mode].trims;
        }
        for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
          app.edit_model.outputs[channel].failsafe =
              runtime_model.outputs[channel].failsafe;
        }
        if (runtime_generation != app.generation) {
          if (activation_maintenance) {
            runtime_update_requires_activation = true;
          } else {
            activation_needed = true;
          }
        }
      } else {
        app.edit_model = runtime_model;
      }
      if (runtime_slot == app.model_library.active_slot()) {
        runtime_update_during_activation =
            runtime_update_during_activation || activation_maintenance;
        model_dirty = true;
        force_screen_rebuild = true;
        dirty_since_us = now_us();
      }
    }
    const int8_t activation_state =
        app.model_activation_state.load(std::memory_order_acquire);
    if (activation_maintenance &&
        (activation_state == 2 || activation_state == -1)) {
      if (activation_state == 2) {
        model_dirty = runtime_update_during_activation;
        activation_needed = runtime_update_requires_activation;
        force_screen_rebuild = true;
        if (runtime_update_during_activation) {
          dirty_since_us = now_us();
        }
        runtime_update_during_activation = false;
        runtime_update_requires_activation = false;
      } else {
        ESP_LOGE(kTag, "runtime model activation failed");
        activation_needed =
            activation_needed || runtime_update_requires_activation;
        runtime_update_requires_activation = false;
        dirty_since_us = now_us();
      }
      app.model_activation_state.store(0, std::memory_order_release);
      app.safety.end_maintenance();
      activation_maintenance = false;
    }
    if ((buttons & 0x0CU) != 0x0CU) {
      if ((pressed & 0x01U) != 0) {
#if CONFIG_RIVETTX_OPENPOCKET_OSD
        (void)openpocket_ui.handle({UiEventType::Up});
#else
        (void)app.ui.handle({UiEventType::Up});
#endif
      }
      if ((pressed & 0x02U) != 0) {
#if CONFIG_RIVETTX_OPENPOCKET_OSD
        (void)openpocket_ui.handle({UiEventType::Down});
#else
        (void)app.ui.handle({UiEventType::Down});
#endif
      }
      if ((pressed & 0x04U) != 0) {
#if CONFIG_RIVETTX_OPENPOCKET_OSD
        (void)openpocket_ui.handle({UiEventType::Enter});
#else
        if (screen == AppScreen::Home) {
          screen = AppScreen::Menu;
          force_screen_rebuild = true;
        } else {
          (void)app.ui.handle({UiEventType::Enter});
        }
#endif
      }
      if ((pressed & 0x08U) != 0) {
#if CONFIG_RIVETTX_OPENPOCKET_OSD
        (void)openpocket_ui.handle({UiEventType::Back});
#else
        if (app.ui.editing()) {
          (void)app.ui.handle({UiEventType::Back});
        } else if (screen == AppScreen::Menu) {
          screen = AppScreen::Home;
          force_screen_rebuild = true;
        } else {
          screen = AppScreen::Menu;
          force_screen_rebuild = true;
        }
#endif
      }
    }
    if (encoder_delta != 0) {
#if CONFIG_RIVETTX_OPENPOCKET_OSD
      (void)openpocket_ui.handle({UiEventType::Rotate, encoder_delta});
#else
      (void)app.ui.handle({UiEventType::Rotate, encoder_delta});
#endif
    }
    if (encoder_press) {
#if CONFIG_RIVETTX_OPENPOCKET_OSD
      (void)openpocket_ui.handle({UiEventType::Enter});
#else
      if (screen == AppScreen::Home) {
        screen = AppScreen::Menu;
        force_screen_rebuild = true;
      } else {
        (void)app.ui.handle({UiEventType::Enter});
      }
#endif
    }
#if CONFIG_RIVETTX_OPENPOCKET_OSD
    screen = app_screen_for(openpocket_ui.page());
#endif
    app.finder_enabled.store(
        screen == AppScreen::Finder, std::memory_order_release);

    UiChange change{};
    while (
#if CONFIG_RIVETTX_OPENPOCKET_OSD
        openpocket_ui.take_change(change)
#else
        app.ui.take_change(change)
#endif
    ) {
      if (change.screen_id == "menu" &&
          menu_target(change.field_id, screen)) {
        force_screen_rebuild = true;
        continue;
      }
      if (change.screen_id == "web" &&
          (change.field_id == "start" ||
           change.field_id == "stop")) {
        if (change.field_id == "start") {
          if (!app.backup_portal.start()) {
            ESP_LOGW(kTag, "web configurator start rejected");
          }
        } else {
          Model restored{};
          uint32_t restored_generation = 0;
          std::string error;
          const bool loaded = app.model_library.select(
              app.model_library.active_slot(), restored,
              restored_generation, error);
          app.safety.request_lock();
          app.backup_portal.stop();
          if (loaded && app.safety.begin_maintenance()) {
            app.edit_model = restored;
            app.generation = restored_generation;
            {
              const std::lock_guard<std::mutex> lock(
                  app.runtime_model_mutex);
              app.pending_model_activation = restored;
              app.pending_model_slot = app.model_library.active_slot();
              app.pending_model_generation = restored_generation;
            }
            app.model_activation_state.store(
                1, std::memory_order_release);
            activation_maintenance = true;
          } else if (!loaded) {
            ESP_LOGE(kTag, "restored model load failed: %s",
                     error.c_str());
          }
          app.model_summaries = app.model_library.summaries();
        }
        force_screen_rebuild = true;
        continue;
      }
      if (change.screen_id == "usb" &&
          (change.field_id == "enable" ||
           change.field_id == "disable")) {
        if (change.field_id == "enable") {
          if (!usb_maintenance && app.usb_gamepad.supported() &&
              !app.backup_portal.running() &&
              app.safety.begin_maintenance()) {
            app.usb_rf_lock.store(
                app.edit_model.simulator_rf_lock,
                std::memory_order_release);
            app.usb_simulator_enabled.store(
                true, std::memory_order_release);
            usb_maintenance = true;
          } else {
            ESP_LOGW(kTag, "USB simulator start rejected");
          }
        } else {
          app.usb_simulator_enabled.store(
              false, std::memory_order_release);
          if (usb_maintenance) {
            app.safety.end_maintenance();
            usb_maintenance = false;
          }
        }
        continue;
      }
      if (change.screen_id == "power" && change.field_id == "display") {
        app.display_power_requested.store(change.value != 0,
                                          std::memory_order_release);
        continue;
      }
      if (change.screen_id == "power" && change.field_id == "backlight") {
        app.backlight_requested.store(
            static_cast<uint8_t>(clamp<int32_t>(0, change.value, 100)),
            std::memory_order_release);
        continue;
      }
      if (change.screen_id == "models") {
        if (activation_maintenance ||
            app.backup_portal.running() ||
            !app.safety.begin_maintenance()) {
          ESP_LOGW(kTag, "model library action rejected while busy");
          continue;
        }
        bool hold_maintenance = false;
        std::string error;
        uint8_t slot = 0;
        if (parse_slot_action(change.field_id, "select", slot)) {
          Model selected{};
          uint32_t selected_generation = 0;
          if (app.model_library.select(
                  slot, selected, selected_generation, error)) {
            app.edit_model = selected;
            app.generation = selected_generation;
            {
              const std::lock_guard<std::mutex> lock(
                  app.runtime_model_mutex);
              app.pending_model_activation = selected;
              app.pending_model_slot = slot;
              app.pending_model_generation = selected_generation;
            }
            app.model_activation_state.store(
                1, std::memory_order_release);
            activation_maintenance = true;
            hold_maintenance = true;
            model_dirty = false;
            activation_needed = false;
            std::string mirror_error;
            if (!app.model_store.save(selected, selected_generation,
                                      mirror_error)) {
              ESP_LOGW(kTag, "active model mirror failed: %s",
                       mirror_error.c_str());
            }
          }
        } else if (parse_slot_action(
                       change.field_id, "delete", slot)) {
          if (!app.model_library.remove(slot, error)) {
            ESP_LOGW(kTag, "model delete failed: %s", error.c_str());
          }
        } else if (change.field_id == "new" ||
                   change.field_id == "copy") {
          Model candidate =
              change.field_id == "copy" ? app.edit_model
                                         : make_default_model();
          uint8_t expected_slot = 0;
          while (expected_slot < app.model_summaries.size() &&
                 app.model_summaries[expected_slot].present) {
            ++expected_slot;
          }
          if (expected_slot < kMaximumStoredModels) {
            (void)std::snprintf(
                candidate.name.data(), candidate.name.size(),
                change.field_id == "copy" ? "Copy %u" : "Model %u",
                static_cast<unsigned>(expected_slot + 1));
            candidate.model_id = expected_slot;
          }
          uint8_t created_slot = 0;
          if (!app.model_library.create(
                  candidate, 1, created_slot, error)) {
            ESP_LOGW(kTag, "model create failed: %s", error.c_str());
          }
        }
        app.model_summaries = app.model_library.summaries();
        force_screen_rebuild = true;
        if (!hold_maintenance) {
          app.safety.end_maintenance();
        }
        continue;
      }
#if CONFIG_RIVETTX_RX5808_ENABLED
      if (change.screen_id == "video") {
        bool changed_channel = false;
        if (change.field_id == "band" && change.value >= 1 &&
            change.value <= static_cast<int32_t>(kVrxBandCount)) {
          app.edit_model.vrx_band =
              static_cast<uint8_t>(change.value - 1);
          changed_channel = true;
        } else if (change.field_id == "channel" &&
                   change.value >= 1 &&
                   change.value <=
                       static_cast<int32_t>(kVrxChannelsPerBand)) {
          app.edit_model.vrx_channel =
              static_cast<uint8_t>(change.value - 1);
          changed_channel = true;
        } else if (change.field_id == "scan") {
          queue_vrx_command(vrx_status.scanning
                                ? VrxCommandType::CancelScan
                                : VrxCommandType::StartScan);
          force_screen_rebuild = true;
          continue;
        }
        if (changed_channel) {
          queue_vrx_command(VrxCommandType::Tune,
                            app.edit_model.vrx_band,
                            app.edit_model.vrx_channel);
          model_dirty = true;
          force_screen_rebuild = true;
          dirty_since_us = now_us();
          continue;
        }
      }
#endif
      const bool maintenance =
          !app.backup_portal.running() &&
          app.safety.maintenance_allowed();
      bool elrs_change = false;
      if (change.screen_id == "elrs" && maintenance) {
        if (change.field_id == "packet_rate") {
          elrs_change = app.elrs.request_packet_rate(
              static_cast<uint8_t>(change.value));
        } else if (change.field_id == "power") {
          elrs_change =
              app.elrs.request_power(static_cast<uint8_t>(change.value));
        } else if (change.field_id == "dynamic") {
          elrs_change = app.elrs.request_dynamic_power(
              static_cast<uint8_t>(change.value));
        } else if (change.field_id == "switch_mode") {
          elrs_change = app.elrs.request_switch_mode(
              static_cast<uint8_t>(change.value));
        } else if (change.field_id == "telemetry_ratio") {
          elrs_change = app.elrs.request_telemetry_ratio(
              static_cast<uint8_t>(change.value));
        } else if (change.field_id == "model_match") {
          elrs_change = app.elrs.request_model_match(
              static_cast<uint8_t>(change.value));
        } else if (change.field_id == "bind") {
          elrs_change = app.elrs.request_bind();
        } else if (change.field_id == "wifi_update") {
          elrs_change = app.elrs.request_wifi_update();
        }
      }
      if (!elrs_change && maintenance &&
          ModelEditor::apply(app.edit_model, change)) {
        model_dirty = true;
        activation_needed = true;
        force_screen_rebuild = true;
        dirty_since_us = now_us();
      }
    }
    if (model_dirty && !app.backup_portal.running() &&
        app.safety.maintenance_allowed() &&
        now_us() - dirty_since_us >= 1000000) {
      std::string error;
      const bool maintenance_started = app.safety.begin_maintenance();
      if (!maintenance_started) {
        dirty_since_us = now_us();
      } else if (app.model_library.save_active(
                     app.edit_model, app.generation + 1, error)) {
        std::string mirror_error;
        if (!app.model_store.save(app.edit_model, app.generation + 1,
                                  mirror_error)) {
          ESP_LOGW(kTag, "active model mirror failed: %s",
                   mirror_error.c_str());
        }
        ++app.generation;
        app.model_summaries = app.model_library.summaries();
        if (activation_needed) {
          {
            const std::lock_guard<std::mutex> lock(
                app.runtime_model_mutex);
            app.pending_model_activation = app.edit_model;
            app.pending_model_slot = app.model_library.active_slot();
            app.pending_model_generation = app.generation;
          }
          app.model_activation_state.store(1, std::memory_order_release);
          activation_maintenance = true;
        } else {
          {
            const std::lock_guard<std::mutex> lock(
                app.runtime_model_mutex);
            app.active_runtime_model_slot =
                app.model_library.active_slot();
            app.active_runtime_model_generation = app.generation;
          }
          model_dirty = false;
        }
      } else {
        ESP_LOGE(kTag, "model save failed: %s", error.c_str());
        dirty_since_us = now_us();
      }
      if (maintenance_started && !activation_maintenance) {
        app.safety.end_maintenance();
      }
    }

    const TimeUs render_time = now_us();
    const UiHomeStatus home = current_home_status(
        controls, frame, telemetry, safety, battery_state, battery_mv,
        battery_sensor_valid, module_state, model_dirty);
#if CONFIG_RIVETTX_OPENPOCKET_OSD
    openpocket_screens.controls = controls;
    openpocket_screens.frame = frame;
    openpocket_screens.timers = timers;
    openpocket_screens.telemetry = telemetry;
    openpocket_screens.elrs = elrs;
    openpocket_screens.finder = finder;
    openpocket_screens.safety = safety;
    openpocket_screens.home = home;
    openpocket_screens.battery_state = battery_state;
    openpocket_screens.battery_mv = battery_mv;
    openpocket_screens.battery_sensor_valid = battery_sensor_valid;
#if CONFIG_RIVETTX_RX5808_ENABLED
    openpocket_screens.vrx = vrx_status;
    openpocket_screens.vrx.signal_fresh =
        app.osd_healthy.load(std::memory_order_acquire);
    openpocket_screens.vrx.video_signal = home.video_signal;
#else
    openpocket_screens.vrx.available = false;
    openpocket_screens.vrx.signal_fresh = true;
    openpocket_screens.vrx.video_signal = home.video_signal;
    openpocket_screens.vrx.band = app.edit_model.vrx_band;
    openpocket_screens.vrx.channel = app.edit_model.vrx_channel;
    openpocket_screens.vrx.frequency_mhz = vrx_frequency_mhz(
        app.edit_model.vrx_band, app.edit_model.vrx_channel);
#endif
    if (!openpocket_started) {
      openpocket_ui.start(home, render_time);
      openpocket_started = true;
    } else {
      openpocket_ui.refresh(home, render_time);
    }
    (void)openpocket_ui.render(openpocket_screens.vrx);
    taskENTER_CRITICAL(&app.osd_frame_lock);
    app.pending_osd_frame = openpocket_ui.frame();
    taskEXIT_CRITICAL(&app.osd_frame_lock);
    app.osd_frame_generation.fetch_add(1, std::memory_order_release);
    screen = app_screen_for(openpocket_ui.page());
    app.finder_enabled.store(
        screen == AppScreen::Finder, std::memory_order_release);
    force_screen_rebuild = false;
#else
    const bool screen_changed =
        !screen_rendered || rendered_screen != screen;
    bool rebuild = force_screen_rebuild || screen_changed;
    if (!rebuild && render_time >= next_live_refresh_us) {
      rebuild = screen == AppScreen::Warnings ||
                screen == AppScreen::Timers ||
                screen == AppScreen::Telemetry ||
                screen == AppScreen::Finder ||
                screen == AppScreen::Elrs ||
                screen == AppScreen::Usb ||
                screen == AppScreen::Web ||
                screen == AppScreen::Power ||
                screen == AppScreen::System;
    }
    if (screen == AppScreen::Home && !screen_changed) {
      app.ui.update_home(home);
    } else if (screen == AppScreen::Outputs && !screen_changed) {
      app.ui.update_outputs(frame);
    } else if (rebuild || screen_changed) {
      app.ui.set_screen(
          current_screen(screen, frame, timers, telemetry, elrs, finder,
                         safety, battery_state, battery_mv,
                         battery_sensor_valid, home));
    }
    if (rebuild || screen_changed) {
      next_live_refresh_us = render_time + 500000;
    }
    rendered_screen = screen;
    screen_rendered = true;
    force_screen_rebuild = false;
    const bool rendered = app.ui.render();
    if (!rendered && !display_failed) {
      ESP_LOGE(kTag, "display refresh failed; UI task will retry");
    } else if (rendered && display_failed) {
      ESP_LOGI(kTag, "display refresh recovered");
    }
    display_failed = !rendered;
#endif
    if (app.screenshot_requested.exchange(false,
                                          std::memory_order_acq_rel) &&
        app.safety.begin_maintenance()) {
#if CONFIG_RIVETTX_OPENPOCKET_OSD
      const auto& cells = openpocket_ui.frame().cells;
      const std::vector<uint8_t> image(cells.begin(), cells.end());
#else
      const auto& buffer = app.canvas.buffer();
      const std::vector<uint8_t> image(buffer.begin(), buffer.end());
#endif
      if (!app.files.write("screenshot.mono", image) ||
          !app.files.sync("screenshot.mono")) {
        ESP_LOGE(kTag, "screenshot write or sync failed");
      }
      app.safety.end_maintenance();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void service_task(void*)
{
  PowerDecision previous_power_decision = PowerDecision::StayOn;
  bool logging_maintenance = false;
  while (true) {
    const TimeUs current = now_us();
    const int8_t logging =
        app.logging_request.exchange(-1, std::memory_order_acq_rel);
    if (logging == 1) {
      if (!logging_maintenance && app.safety.begin_maintenance()) {
        app.telemetry_logger.start();
        logging_maintenance = true;
        app.logging_active.store(true, std::memory_order_release);
        app.logging_failed.store(false, std::memory_order_release);
      } else {
        app.logging_failed.store(true, std::memory_order_release);
      }
    } else if (logging == 0) {
      const bool stopped = app.telemetry_logger.stop();
      app.logging_active.store(false, std::memory_order_release);
      app.logging_failed.store(!stopped, std::memory_order_release);
      if (logging_maintenance) {
        app.safety.end_maintenance();
        logging_maintenance = false;
      }
    }
    const uint32_t user_activity_ms =
        app.last_user_activity_ms.exchange(0, std::memory_order_acq_rel);
    if (user_activity_ms != 0) {
      app.power.note_activity(
          static_cast<TimeUs>(user_activity_ms) * 1000);
    }
    if (app.snapshot_pending.exchange(false, std::memory_order_acq_rel)) {
      CrashSnapshot snapshot{};
      taskENTER_CRITICAL(&app.frame_lock);
      snapshot = app.pending_snapshot;
      taskEXIT_CRITICAL(&app.frame_lock);
      if (!app.crash_store.write(snapshot)) {
        ESP_LOGE(kTag, "crash snapshot persistence failed");
      }
    }
    std::array<TelemetryEntry, kMaxTelemetrySensors> telemetry{};
    const BatterySnapshot battery_snapshot =
        app.battery_snapshot.read();
    BatteryState battery_state = battery_snapshot.state;
    ModuleState module_state = ModuleState::Starting;
    SafetyState safety_state = SafetyState::Booting;
    taskENTER_CRITICAL(&app.frame_lock);
    telemetry = app.latest_telemetry;
    module_state = app.latest_module_state;
    safety_state = app.latest_safety_state;
    taskEXIT_CRITICAL(&app.frame_lock);
    app.service_telemetry.clear();
    for (const auto& sensor : telemetry) {
      if (sensor.discovered) {
        app.service_telemetry.update(
            sensor.id, sensor.value, sensor.unit, sensor.updated_at_us);
      }
    }
    if (app.logging_active.load(std::memory_order_acquire) &&
        !app.telemetry_logger.sample(app.service_telemetry, current)) {
      (void)app.telemetry_logger.stop();
      app.logging_active.store(false, std::memory_order_release);
      app.logging_failed.store(true, std::memory_order_release);
      if (logging_maintenance) {
        app.safety.end_maintenance();
        logging_maintenance = false;
      }
      ESP_LOGE(kTag, "telemetry logging stopped after storage failure");
    }
    AlarmEvent alarm{};
    if (app.alarms.evaluate(app.service_telemetry, current, alarm)) {
      // This task never writes the diagnostic ring directly. The control task
      // is its single writer; platform alarm output can consume this event.
      ESP_LOGW(kTag, "telemetry alarm sensor=%u value=%ld active=%d",
               static_cast<unsigned>(alarm.sensor_id),
               static_cast<long>(alarm.value), alarm.active);
      app.audio_warnings.telemetry_alarm(alarm.active, app.audio);
    }
    if (app.lua.loaded()) {
      (void)app.scripts.tick(current);
    }
    app.finder.set_active(
        app.finder_enabled.load(std::memory_order_acquire));
    app.finder.tick(app.service_telemetry, current);
    app.audio_warnings.tick(app.service_telemetry, battery_state,
                            module_state, safety_state, current,
                            app.audio);
    app.audio.tick(current);
    app.speaker.tick(current);
    const PowerDecision power_decision = app.power.evaluate(
        battery_state, safety_state == SafetyState::Enabled, current);
    if (power_decision == PowerDecision::LockAndShutdown &&
        previous_power_decision != PowerDecision::LockAndShutdown) {
      app.safety.request_lock();
    }
    previous_power_decision = power_decision;
    taskENTER_CRITICAL(&app.frame_lock);
    app.latest_finder = app.finder.status();
    taskEXIT_CRITICAL(&app.frame_lock);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

#if CONFIG_RIVETTX_RX5808_ENABLED
const char* vrx_failure_name(VrxFailure failure)
{
  switch (failure) {
    case VrxFailure::None: return "none";
    case VrxFailure::InvalidFrequency: return "invalid frequency";
    case VrxFailure::TuneRejected: return "tune rejected";
    case VrxFailure::TuneTimeout: return "tune timeout";
    case VrxFailure::Communication: return "GPIO communication";
    case VrxFailure::RssiCalibration: return "RSSI calibration";
  }
  return "unknown";
}

void vrx_task(void*)
{
  VrxFailure previous_failure = VrxFailure::None;
  VrxRssiState previous_rssi_state = VrxRssiState::Unavailable;
  bool previous_available = false;
  bool previous_scanning = false;
  while (true) {
    const TimeUs current = now_us();
    VrxCommand command{};
    if (take_vrx_command(command)) {
      switch (command.type) {
        case VrxCommandType::Tune:
          if (!app.vrx.select(command.band, command.channel, current)) {
            ESP_LOGE(kTag, "RX5808 tune rejected band=%u channel=%u",
                     static_cast<unsigned>(command.band + 1),
                     static_cast<unsigned>(command.channel + 1));
          }
          break;
        case VrxCommandType::StartScan:
          if (!app.vrx.begin_scan(current)) {
            ESP_LOGW(kTag, "RX5808 scan start rejected");
          } else {
            ESP_LOGI(kTag, "RX5808 scan started");
          }
          break;
        case VrxCommandType::CancelScan:
          if (app.vrx.cancel_scan(current)) {
            ESP_LOGI(kTag, "RX5808 scan cancelled; restoring channel");
          } else {
            ESP_LOGW(kTag, "RX5808 scan cancellation ignored");
          }
          break;
      }
    }

    app.vrx.set_video_signal(
        app.osd_video_present.load(std::memory_order_acquire),
        app.osd_healthy.load(std::memory_order_acquire));
    app.vrx.tick(current);
    uint8_t result_band = 0;
    uint8_t result_channel = 0;
    if (app.vrx.take_scan_result(result_band, result_channel)) {
      app.vrx_scan_result.store(
          static_cast<int16_t>(result_band * kVrxChannelsPerBand +
                               result_channel),
          std::memory_order_release);
      ESP_LOGI(kTag,
               "RX5808 scan selected band=%u channel=%u frequency=%uMHz",
               static_cast<unsigned>(result_band + 1),
               static_cast<unsigned>(result_channel + 1),
               static_cast<unsigned>(
                   vrx_frequency_mhz(result_band, result_channel)));
    }

    const VrxStatus status = app.vrx.status();
    if (status.failure != VrxFailure::None &&
        status.failure != previous_failure) {
      ESP_LOGE(kTag, "RX5808 failure: %s frequency=%uMHz",
               vrx_failure_name(status.failure),
               static_cast<unsigned>(status.frequency_mhz));
    }
    if (status.rssi_state == VrxRssiState::SensorFault &&
        previous_rssi_state != VrxRssiState::SensorFault) {
      ESP_LOGE(kTag, "RX5808 RSSI ADC sensor fault");
    }
    if (status.available && !previous_available &&
        status.failure == VrxFailure::None) {
      ESP_LOGI(kTag, "RX5808 tuned to %uMHz",
               static_cast<unsigned>(status.frequency_mhz));
    }
    if (previous_scanning && !status.scanning &&
        status.failure != VrxFailure::None) {
      ESP_LOGW(kTag, "RX5808 scan stopped after failure");
    }
    previous_failure = status.failure;
    previous_rssi_state = status.rssi_state;
    previous_available = status.available;
    previous_scanning = status.scanning;
    taskENTER_CRITICAL(&app.frame_lock);
    app.latest_vrx = status;
    taskEXIT_CRITICAL(&app.frame_lock);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
#endif

#if CONFIG_RIVETTX_OPENPOCKET_OSD
void osd_task(void*)
{
  uint32_t rendered_generation = 0;
  bool previous_healthy = false;
  app.osd.start(now_us());
  while (true) {
    const uint32_t generation =
        app.osd_frame_generation.load(std::memory_order_acquire);
    if (generation != 0 && generation != rendered_generation) {
      CharacterOsdFrame frame{};
      taskENTER_CRITICAL(&app.osd_frame_lock);
      frame = app.pending_osd_frame;
      taskEXIT_CRITICAL(&app.osd_frame_lock);
      app.osd.submit(frame);
      rendered_generation = generation;
    }
    app.osd.tick(now_us());
    const At7456eStatus& status = app.osd.status();
    app.osd_healthy.store(status.healthy, std::memory_order_release);
    app.osd_video_present.store(status.video_present,
                                std::memory_order_release);
    app.osd_video_standard.store(
        static_cast<uint8_t>(status.standard), std::memory_order_release);
    if (status.healthy != previous_healthy) {
      ESP_LOGI(kTag, "AT7456E %s (video=%s, standard=%u)",
               status.healthy ? "ready" : "unavailable",
               status.video_present ? "present" : "missing",
               static_cast<unsigned>(status.standard));
      previous_healthy = status.healthy;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
#endif

void usb_task(void*)
{
  UsbSimulator simulator;
  bool active = false;
  while (true) {
    const bool requested =
        app.usb_simulator_enabled.load(std::memory_order_acquire);
    const bool rf_lock =
        app.usb_rf_lock.load(std::memory_order_acquire);
    if (requested && !active) {
      active = simulator.enter(true, rf_lock);
    } else if (!requested && active) {
      simulator.leave();
      active = false;
    }
    if (active) {
      ControlInputs controls{};
      ChannelFrame channels{};
      taskENTER_CRITICAL(&app.frame_lock);
      controls = app.latest_controls;
      channels = app.latest_frame;
      taskEXIT_CRITICAL(&app.frame_lock);
      (void)app.usb_gamepad.send(simulator.report(controls, channels));
    }
    vTaskDelay(pdMS_TO_TICKS(4));
  }
}

#if CONFIG_RIVETTX_OPENPOCKET_REV_A
void board_io_task(void*)
{
  bool previous_simulator = false;
  bool previous_display = true;
  uint8_t previous_backlight = CONFIG_RIVETTX_BACKLIGHT_DEFAULT_PERCENT;
  Amt630aState previous_controller_state = Amt630aState::Unavailable;
  TimeUs next_controller_recovery_us = 0;
  bool provisioning_requested = false;
  while (true) {
    const TimeUs current = now_us();
    uint32_t controls = 0;
    const bool controls_valid =
        app.board_power_io.read_expanded_controls(controls);
    app.board.publish_rev_a_controls(controls, current, controls_valid);
#if CONFIG_RIVETTX_SDMMC_ENABLED
    app.sd_card_present.store(
        controls_valid &&
            (controls & (1UL << CONFIG_RIVETTX_SD_DETECT_EXPANDER_BIT)) != 0,
        std::memory_order_release);
#endif

    const bool simulator =
        app.usb_simulator_enabled.load(std::memory_order_acquire);
    const bool display =
        app.display_power_requested.load(std::memory_order_acquire);
    const uint8_t backlight =
        app.backlight_requested.load(std::memory_order_acquire);
    if (simulator != previous_simulator) {
      if (!app.board_power.set_simulator_mode(simulator)) {
        ESP_LOGE(kTag, "Revision-A simulator power policy failed");
      } else if (!simulator) {
        (void)app.board_power.request_video(true);
        (void)app.board_power.request_elrs(true);
      }
      previous_simulator = simulator;
    }
    const bool display_changed = display != previous_display;
    if (display_changed || backlight != previous_backlight) {
      if (!app.board_power.request_display(display, backlight)) {
        ESP_LOGE(kTag, "Revision-A display power request failed");
      }
      previous_display = display;
      previous_backlight = backlight;
    }
    app.board_power.tick(current);
    BoardPowerStatus power_status = app.board_power.status();
    Amt630aStatus controller_status = app.display_controller.status();
    if (power_status.display_5v &&
        !power_status.display_controller_reset_asserted) {
      if (controller_status.state == Amt630aState::Unavailable &&
          !provisioning_requested) {
        const std::size_t image_size =
            static_cast<std::size_t>(amt_image_end - amt_image_start);
        provisioning_requested = app.display_controller.ensure_program(
            amt_image_start, image_size, kAmtImageSha256, current);
      } else if (controller_status.state == Amt630aState::Fault &&
                 (display_changed || current >= next_controller_recovery_us)) {
        if (app.display_controller.recover(current)) {
          next_controller_recovery_us = current + 1000000U;
          provisioning_requested = false;
        }
      }
      app.display_controller.tick(current);
      controller_status = app.display_controller.status();
    }
    app.board_power.set_display_controller_ready(
        controller_status.runtime.booted &&
        controller_status.runtime.panel_timing_active);
    power_status = app.board_power.status();
    if (controller_status.state != previous_controller_state) {
      ESP_LOGI(kTag, "AMT630A state=%u fault=%u video=%u standard=%u",
               static_cast<unsigned>(controller_status.state),
               static_cast<unsigned>(controller_status.fault),
               controller_status.runtime.video_present ? 1U : 0U,
               static_cast<unsigned>(controller_status.runtime.standard));
      previous_controller_state = controller_status.state;
    }
    taskENTER_CRITICAL(&app.frame_lock);
    app.latest_board_power = power_status;
    app.latest_display_controller = controller_status;
    taskEXIT_CRITICAL(&app.frame_lock);
    vTaskDelay(pdMS_TO_TICKS(4));
  }
}
#endif

#if CONFIG_RIVETTX_SDMMC_ENABLED
void removable_storage_task(void*)
{
  RemovableStorageState previous = RemovableStorageState::Absent;
  while (true) {
    const TimeUs current = now_us();
    app.removable_storage.set_card_detect(
        app.sd_card_present.load(std::memory_order_acquire), current);
    app.removable_storage.tick(current);
    const auto& status = app.removable_storage.status();
    if (status.state != previous) {
      ESP_LOGI(kTag,
               "microSD state=%u present=%u mounted=%u writable=%u queued=%u",
               static_cast<unsigned>(status.state),
               status.card_present ? 1U : 0U,
               status.mounted ? 1U : 0U,
               status.writable ? 1U : 0U,
               static_cast<unsigned>(status.queued));
      previous = status.state;
    }
    // All FatFS/SDMMC calls live only in this low-priority task. At most one
    // bounded 512-byte request is serviced per tick.
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
#endif

bool initialize_storage(bool explicit_format_requested)
{
  if (explicit_format_requested) {
    ESP_LOGW(kTag, "explicit storage format chord accepted");
  }
  if (!mount_model_filesystem(explicit_format_requested)) {
    return false;
  }
  const auto loaded = app.model_store.load(app.model);
  if (loaded.success) {
    app.generation = loaded.generation;
    if (!loaded.error.empty()) {
      ESP_LOGW(kTag, "legacy model mirror recovery failed: %s",
               loaded.error.c_str());
    }
    if (loaded.recovered) {
      app.diagnostics.push(
          {now_us(), LogSeverity::Warning, LogCode::StorageRecovered, 0, 0});
    }
  } else {
    app.model = make_default_model();
    app.generation = 1;
  }
  std::string library_error;
  if (!app.model_library.bootstrap(app.model, app.generation,
                                   library_error)) {
    ESP_LOGE(kTag, "cannot initialize model library: %s",
             library_error.c_str());
    return false;
  }
  app.active_runtime_model_slot = app.model_library.active_slot();
  app.active_runtime_model_generation = app.generation;
  std::vector<uint8_t> active_mirror;
  const auto expected_mirror =
      ModelCodec::encode(app.model, app.generation);
  if ((!app.model_store.export_active(active_mirror) ||
       active_mirror != expected_mirror) &&
      !app.model_store.save(app.model, app.generation, library_error)) {
    ESP_LOGW(kTag, "cannot mirror active model: %s",
             library_error.c_str());
  }
  app.model_summaries = app.model_library.summaries();
  app.edit_model = app.model;

  std::array<AxisCalibration, kMaxAxes> calibration{};
  app.calibration_valid = app.calibration_store.load(calibration);
  if (app.calibration_valid) {
    app.input_processor.set_calibration(calibration);
  }

  (void)mkdir("/models/scripts", 0755);
  if (!app.files.exists("scripts/startup.lua")) {
    static constexpr char startup_script[] =
        "return {\n"
        "  init = function() end,\n"
        "  run = function(event)\n"
        "    local lq = getValue(3)\n"
        "    return lq or 0\n"
        "  end\n"
        "}\n";
    const std::vector<uint8_t> script(
        startup_script, startup_script + sizeof(startup_script) - 1);
    if (!app.files.write("scripts/startup.lua", script) ||
        !app.files.sync("scripts/startup.lua")) {
      ESP_LOGE(kTag, "default Lua startup script persistence failed");
    }
  }
  std::string lua_error;
  if (!app.lua.load_file("/models/scripts/startup.lua", lua_error)) {
    ESP_LOGW(kTag, "Lua startup script disabled: %s", lua_error.c_str());
  }
  return true;
}

}  // namespace

extern "C" void app_main()
{
  ESP_LOGI(kTag, "booting RivetTX target=%s cores=%d", CONFIG_IDF_TARGET,
           CONFIG_FREERTOS_NUMBER_OF_CORES);

  esp_err_t nvs_result = nvs_flash_init();
  if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
      nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_result = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs_result);
  (void)app.crash_store.initialize();
  (void)app.boot_state.initialize();
  const uint32_t boot_attempt = app.boot_state.begin_attempt();

  const bool pins_ok = validate_pin_configuration();
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  const bool board_power_ok = pins_ok && app.board_power.initialize(false);
  uint32_t startup_controls = 0;
  const bool startup_controls_ok =
      board_power_ok &&
      app.board_power_io.read_expanded_controls(startup_controls);
  app.board.publish_rev_a_controls(startup_controls, now_us(),
                                   startup_controls_ok);
  if (board_power_ok) {
    (void)app.board_power.request_video(true);
    (void)app.board_power.request_display(
        true, CONFIG_RIVETTX_BACKLIGHT_DEFAULT_PERCENT);
    (void)app.board_power.request_elrs(true);
    app.latest_board_power = app.board_power.status();
  } else {
    ESP_LOGE(kTag, "Revision-A board power initialization failed");
  }
#endif
  const bool inputs_ok = pins_ok && app.board.initialize();
  const RawInputs startup_inputs =
      inputs_ok ? app.board.sample_inputs(now_us()) : RawInputs{};
  const bool calibration_requested =
      startup_inputs.switches[0] && startup_inputs.switches[1];
  const bool recovery_requested =
      inputs_ok && app.boot.enter_recovery(
                       app.board.recovery_button_pressed(), boot_attempt);
  const bool storage_format_requested =
      inputs_ok && startup_inputs.valid &&
      startup_inputs.switches[0] && startup_inputs.switches[1] &&
      startup_inputs.switches[3];
  bool storage_ok = initialize_storage(storage_format_requested);
#if CONFIG_RIVETTX_RX5808_ENABLED
  const bool vrx_config_ok = validate_rx5808_configuration();
  app.vrx_initialized = inputs_ok && vrx_config_ok &&
                        app.vrx_hardware.initialize();
  if (!app.vrx_initialized) {
    ESP_LOGW(kTag,
             "RX5808 unavailable; control and CRSF will continue without VRX");
  }
  VrxStatus startup_vrx = app.vrx.status();
  if (!app.vrx_initialized) {
    startup_vrx.band = app.model.vrx_band;
    startup_vrx.channel = app.model.vrx_channel;
    startup_vrx.frequency_mhz =
        vrx_frequency_mhz(app.model.vrx_band, app.model.vrx_channel);
    startup_vrx.failure = VrxFailure::TuneRejected;
  }
  taskENTER_CRITICAL(&app.frame_lock);
  app.latest_vrx = startup_vrx;
  taskEXIT_CRITICAL(&app.frame_lock);
#endif
#if CONFIG_RIVETTX_OPENPOCKET_OSD
  const bool display_ok = pins_ok && app.osd_spi.initialize();
#else
  const bool display_ok = pins_ok && app.display.initialize();
#endif
  const bool usb_ok =
      !app.usb_gamepad.supported() || app.usb_gamepad.initialize();
  if (pins_ok) {
    (void)app.tones.initialize();
  }
  (void)app.audio.configure(
      {AudioSettings::kVersion, true, true, true, true, false, false,
       CONFIG_RIVETTX_BUZZER_VOLUME});
  (void)app.speaker.initialize(false, {false, 50});
  app.audio.notify(AudioAlert::Startup);
  app.alarms.set_alarm(
      0, {true, crsf::SensorUplinkLinkQuality, AlarmComparison::Below,
          CONFIG_RIVETTX_LINK_WARNING_LQ, 5, 10});
  if ((calibration_requested || !app.calibration_valid) && display_ok) {
    ESP_LOGI(kTag, "%s calibration wizard",
             calibration_requested ? "requested" : "first-run");
    if (run_startup_calibration()) {
      app.calibration_valid = true;
    } else {
      ESP_LOGW(kTag, "stick calibration cancelled or failed");
    }
  }
  const bool crsf_ok = pins_ok && app.transport.initialize();

  const esp_reset_reason_t reset_reason = esp_reset_reason();
  const bool watchdog_recovery =
      reset_reason == ESP_RST_TASK_WDT || reset_reason == ESP_RST_WDT ||
      reset_reason == ESP_RST_INT_WDT;
  app.safety.boot_complete(storage_ok, watchdog_recovery,
                           app.calibration_valid);
  if (recovery_requested) {
    app.safety.request_lock();
  }
  if (crsf_ok) {
    app.module.start(app.model.model_id, now_us());
    app.elrs.start(now_us());
  }

  const BaseType_t control_created =
      inputs_ok && crsf_ok
          ? xTaskCreatePinnedToCore(control_task, "rivet-control", 6144,
                                    nullptr, 20, nullptr, kControlCore)
          : pdFAIL;
  const BaseType_t ui_created =
      display_ok
          ? xTaskCreatePinnedToCore(ui_task, "rivet-ui", 6144, nullptr, 5,
                                    nullptr, kServiceCore)
          : pdFAIL;
  const BaseType_t service_created = xTaskCreatePinnedToCore(
      service_task, "rivet-service", 5120, nullptr, 3, nullptr, kServiceCore);
#if CONFIG_RIVETTX_OPENPOCKET_REV_A
  const BaseType_t board_io_created =
      board_power_ok
          ? xTaskCreatePinnedToCore(board_io_task, "rivet-board-io", 3072,
                                    nullptr, 6, nullptr, kServiceCore)
          : pdFAIL;
#else
  const BaseType_t board_io_created = pdPASS;
#endif
#if CONFIG_RIVETTX_SDMMC_ENABLED
  const BaseType_t removable_storage_created =
      board_power_ok
          ? xTaskCreatePinnedToCore(removable_storage_task, "rivet-sd", 4096,
                                    nullptr, 2, nullptr, kServiceCore)
          : pdFAIL;
#else
  const BaseType_t removable_storage_created = pdPASS;
#endif
#if CONFIG_RIVETTX_RX5808_ENABLED
  const BaseType_t vrx_created =
      app.vrx_initialized
          ? xTaskCreatePinnedToCore(vrx_task, "rivet-vrx", 3072, nullptr, 4,
                                    nullptr, kServiceCore)
          : pdPASS;
  if (app.vrx_initialized && vrx_created == pdPASS &&
      !queue_vrx_command(VrxCommandType::Tune, app.model.vrx_band,
                         app.model.vrx_channel)) {
    ESP_LOGE(kTag, "RX5808 startup tune could not be queued");
  }
#else
  const BaseType_t vrx_created = pdPASS;
#endif
#if CONFIG_RIVETTX_OPENPOCKET_OSD
  const BaseType_t osd_created =
      display_ok
          ? xTaskCreatePinnedToCore(osd_task, "rivet-osd", 3072, nullptr, 4,
                                    nullptr, kServiceCore)
          : pdFAIL;
#else
  const BaseType_t osd_created = pdPASS;
#endif
  const BaseType_t usb_created =
      app.usb_gamepad.supported()
          ? (usb_ok
                 ? xTaskCreatePinnedToCore(
                       usb_task, "rivet-usb", 3072, nullptr, 4, nullptr,
                       kServiceCore)
                 : pdFAIL)
          : pdPASS;

  SelfTestResult self_test{};
  self_test.storage = storage_ok;
  self_test.inputs = inputs_ok && app.calibration_valid;
  self_test.display = display_ok;
  self_test.crsf_uart = crsf_ok;
  self_test.control_task = control_created == pdPASS &&
                           ui_created == pdPASS &&
                           service_created == pdPASS &&
                           board_io_created == pdPASS &&
                           removable_storage_created == pdPASS &&
                           vrx_created == pdPASS &&
                           osd_created == pdPASS &&
                           usb_created == pdPASS;
  ModuleState runtime_module_state = ModuleState::Starting;
  if (self_test.control_task) {
    const TimeUs runtime_deadline = now_us() + 3000000;
    while (now_us() < runtime_deadline) {
      taskENTER_CRITICAL(&app.frame_lock);
      runtime_module_state = app.latest_module_state;
      taskEXIT_CRITICAL(&app.frame_lock);
      if (app.healthy_control_cycles.load(std::memory_order_relaxed) >= 20) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
#if CONFIG_RIVETTX_OPENPOCKET_OSD
  self_test.display = self_test.display &&
                      app.osd_healthy.load(std::memory_order_acquire);
#endif
  self_test.control_runtime =
      app.healthy_control_cycles.load(std::memory_order_relaxed) >= 20;
  switch (runtime_module_state) {
    case ModuleState::Online:
      self_test.module = ModuleBootCondition::Online;
      break;
    case ModuleState::Starting:
      self_test.module = ModuleBootCondition::Starting;
      break;
    case ModuleState::Offline:
      self_test.module = ModuleBootCondition::Absent;
      break;
    case ModuleState::Passthrough:
      self_test.module = ModuleBootCondition::Reconnecting;
      break;
  }

  if (!app.boot.finish_startup(self_test, now_us())) {
    ESP_LOGE(kTag, "startup self-test failed");
    app.safety.request_lock();
  } else if (!app.boot_state.mark_success()) {
    ESP_LOGE(kTag, "boot success state persistence failed");
  }

  if (recovery_requested) {
    if (app.backup_portal.start()) {
      ESP_LOGW(kTag,
               "recovery portal active: Wi-Fi RivetTX-Recovery, "
               "GET /backup, POST /restore");
    } else {
      ESP_LOGE(kTag, "failed to start recovery portal");
    }
  }

  ESP_LOGI(kTag,
           "RivetTX started; hold ENTER+BACK with throttle low to enable");
}
