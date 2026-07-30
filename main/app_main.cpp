#include "esp_platform.hpp"

#include "rivettx/audio.hpp"
#include "rivettx/core.hpp"
#include "rivettx/crsf.hpp"
#include "rivettx/elrs.hpp"
#include "rivettx/services.hpp"
#include "rivettx/storage.hpp"
#include "rivettx/ui.hpp"
#include "rivettx/lua_vm.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "esp_log.h"
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
constexpr uint8_t kScreenCount = 15;

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

struct Application {
  EspBoard board;
  EspCrsfTransport transport;
  Ssd1306Display display;
  EspToneOutput tones;
  AudioAlertScheduler audio{tones};
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
  ModuleSupervisor module{transport, parser, diagnostics};
  ElrsDeviceManager elrs{transport, parser};
  ElrsFinder finder{audio};
  InputProcessor input_processor;
  MixerEngine mixer;
  SafetyManager safety{safety_config()};
  BatteryMonitor battery{battery_config()};
  PosixFileStore files{"/models"};
  TransactionalModelStore model_store{files, "active.rvm"};
  CalibrationStore calibration_store{files, "calibration.bin"};
  WifiBackupPortal backup_portal{model_store, safety};
  CsvTelemetrySink telemetry_sink{"/models/telemetry.csv"};
  TelemetryLogger telemetry_logger{telemetry_sink, 100};
  AlarmEngine alarms;
  MonoCanvas canvas{128, 64};
  LuaVm lua{service_telemetry, parser, transport, canvas, &audio};
  ScriptSupervisor scripts{lua, service_diagnostics};
  UiController ui{display, canvas};
  BootManager boot{ota, boot_diagnostics};
  Model model = make_default_model();
  Model edit_model = make_default_model();
  ChannelFrame latest_frame{};
  std::array<TimerState, kMaxTimers> latest_timers{};
  std::array<TelemetryEntry, kMaxTelemetrySensors> latest_telemetry{};
  ElrsManagerStatus latest_elrs{};
  ElrsFinderStatus latest_finder{};
  BatteryState latest_battery_state = BatteryState::Unknown;
  ModuleState latest_module_state = ModuleState::Starting;
  SafetyState latest_safety_state = SafetyState::Booting;
  portMUX_TYPE frame_lock = portMUX_INITIALIZER_UNLOCKED;
  uint8_t latest_buttons = 0;
  std::atomic<bool> finder_enabled{false};
  TimeUs safety_chord_started_us = 0;
  bool safety_chord_fired = false;
  SafetyState previous_safety_state = SafetyState::Booting;
  BatteryState previous_battery_state = BatteryState::Unknown;
  bool fault_snapshot_saved = false;
  uint32_t generation = 0;
};

Application app;

void save_crash_snapshot(uint32_t reason)
{
  const auto snapshot =
      make_crash_snapshot(reason, app.latest_frame.sequence,
                          app.safety.status(), app.diagnostics);
  (void)app.crash_store.write(snapshot);
}

void control_task(void*)
{
  (void)esp_task_wdt_add(nullptr);
  TickType_t last_wake = xTaskGetTickCount();
  const TickType_t period =
      std::max<TickType_t>(1, pdMS_TO_TICKS(kControlPeriodUs / 1000));

  while (true) {
    const TimeUs started = now_us();
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

    const ChannelFrame proposed =
        app.mixer.evaluate(app.model, controls, app.telemetry, started);
    const TimeUs mixed_at = now_us();
    const uint32_t mixer_duration =
        static_cast<uint32_t>(mixed_at - started);

    const uint16_t raw_battery = app.board.sample_battery_mv();
    const BatteryState battery_state = app.battery.update(raw_battery);
    app.safety.report_battery(app.battery.voltage_mv());
    app.safety.report_mixer_duration(mixer_duration);
    ChannelFrame frame =
        app.safety.gate(app.model, controls, proposed, mixed_at);

    if (!app.module.send_channels(frame, now_us())) {
      app.diagnostics.push(
          {now_us(), LogSeverity::Warning, LogCode::ModuleLost, 0, 0});
    }
    app.module.poll(now_us());
    app.elrs.tick(now_us());

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

    const SafetyState safety_state = app.safety.status().state;
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
        save_crash_snapshot(
            static_cast<uint32_t>(app.safety.status().reason));
        app.fault_snapshot_saved = true;
      } else if (safety_state != SafetyState::Fault) {
        app.fault_snapshot_saved = false;
      }
    }

    taskENTER_CRITICAL(&app.frame_lock);
    app.latest_frame = frame;
    app.latest_timers = app.mixer.timer_states();
    app.latest_telemetry = app.telemetry.entries();
    app.latest_elrs = app.elrs.status();
    app.latest_battery_state = battery_state;
    app.latest_module_state = app.module.status().state;
    app.latest_safety_state = safety_state;
    app.latest_buttons =
        static_cast<uint8_t>((raw.switches[0] ? 1U : 0U) |
                             (raw.switches[1] ? 2U : 0U) |
                             (raw.switches[2] ? 4U : 0U) |
                             (raw.switches[3] ? 8U : 0U));
    taskEXIT_CRITICAL(&app.frame_lock);

    app.watchdog.kick();
    vTaskDelayUntil(&last_wake, period);
  }
}

UiScreen current_screen(uint8_t index, const ChannelFrame& frame,
                        const std::array<TimerState, kMaxTimers>& timers,
                        const std::array<TelemetryEntry,
                                         kMaxTelemetrySensors>& telemetry,
                        const ElrsManagerStatus& elrs,
                        const ElrsFinderStatus& finder)
{
  switch (index) {
    case 0: {
      int32_t link_quality = 0;
      for (const auto& sensor : telemetry) {
        if (sensor.discovered &&
            sensor.id == crsf::SensorUplinkLinkQuality) {
          link_quality = sensor.value;
          break;
        }
      }
      return make_main_screen(
          app.model, frame, app.battery.voltage_mv(),
          static_cast<uint8_t>(clamp<int32_t>(0, link_quality, 100)),
          app.safety.status().state == SafetyState::Enabled);
    }
    case 1:
      return make_outputs_screen(frame);
    case 2:
      return make_model_setup_screen(app.edit_model);
    case 3:
      return make_inputs_screen(app.edit_model);
    case 4:
      return make_mixes_screen(app.edit_model);
    case 5:
      return make_output_limits_screen(app.edit_model);
    case 6:
      return make_flight_modes_screen(app.edit_model);
    case 7:
      return make_curves_screen(app.edit_model);
    case 8:
      return make_logical_switches_screen(app.edit_model);
    case 9:
      return make_special_functions_screen(app.edit_model);
    case 10:
      return make_timers_screen(app.edit_model, timers);
    case 11: {
      std::vector<UiField> sensors;
      for (const auto& sensor : telemetry) {
        if (sensor.discovered) {
          sensors.push_back(
              {"sensor." + std::to_string(sensor.id),
               "S" + std::to_string(sensor.id), std::to_string(sensor.value),
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
    case 12:
      return make_elrs_finder_screen(finder);
    case 13:
      return make_elrs_screen(elrs, app.safety.maintenance_allowed());
    default:
      return make_system_screen(
          app.battery.voltage_mv(), esp_get_free_heap_size(),
          app.safety.status().missed_deadlines, "0.1.0");
  }
}

bool run_startup_calibration()
{
  CalibrationWizard wizard;
  wizard.begin(4);
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
    app.ui.set_screen(
        make_calibration_screen(static_cast<uint8_t>(step), progress));
    (void)app.ui.render();
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  if (wizard.step() != CalibrationStep::Complete ||
      !app.calibration_store.save(wizard.result())) {
    return false;
  }
  app.input_processor.set_calibration(wizard.result());
  return true;
}

void ui_task(void*)
{
  uint8_t screen = 0;
  uint8_t previous_buttons = 0;
  bool model_dirty = false;
  TimeUs dirty_since_us = 0;
  while (true) {
    ChannelFrame frame{};
    std::array<TimerState, kMaxTimers> timers{};
    std::array<TelemetryEntry, kMaxTelemetrySensors> telemetry{};
    ElrsManagerStatus elrs{};
    ElrsFinderStatus finder{};
    uint8_t buttons = 0;
    taskENTER_CRITICAL(&app.frame_lock);
    frame = app.latest_frame;
    timers = app.latest_timers;
    telemetry = app.latest_telemetry;
    elrs = app.latest_elrs;
    finder = app.latest_finder;
    buttons = app.latest_buttons;
    taskEXIT_CRITICAL(&app.frame_lock);

    const uint8_t pressed =
        static_cast<uint8_t>(buttons & ~previous_buttons);
    previous_buttons = buttons;
    if ((buttons & 0x0CU) != 0x0CU) {
      if ((pressed & 0x01U) != 0) {
        (void)app.ui.handle({UiEventType::Up});
      }
      if ((pressed & 0x02U) != 0) {
        (void)app.ui.handle({UiEventType::Down});
      }
      if ((pressed & 0x04U) != 0) {
        (void)app.ui.handle({UiEventType::Enter});
      }
      if ((pressed & 0x08U) != 0) {
        if (app.ui.editing()) {
          (void)app.ui.handle({UiEventType::Back});
        } else {
          screen = static_cast<uint8_t>((screen + 1) % kScreenCount);
        }
      }
    }
    app.finder_enabled.store(screen == 12, std::memory_order_release);

    UiChange change{};
    while (app.ui.take_change(change)) {
      const bool maintenance =
          !app.backup_portal.running() &&
          app.safety.maintenance_allowed();
      bool elrs_change = false;
      if (change.screen_id == "elrs" && maintenance) {
        if (change.field_id == "power") {
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
        } else if (change.field_id == "bind") {
          elrs_change = app.elrs.request_bind();
        } else if (change.field_id == "wifi_update") {
          elrs_change = app.elrs.request_wifi_update();
        }
      }
      if (!elrs_change && maintenance &&
          ModelEditor::apply(app.edit_model, change)) {
        model_dirty = true;
        dirty_since_us = now_us();
      }
    }
    if (model_dirty && !app.backup_portal.running() &&
        app.safety.maintenance_allowed() &&
        now_us() - dirty_since_us >= 1000000) {
      std::string error;
      if (app.model_store.save(app.edit_model, app.generation + 1, error)) {
        ++app.generation;
        model_dirty = false;
      } else {
        ESP_LOGE(kTag, "model save failed: %s", error.c_str());
        dirty_since_us = now_us();
      }
    }

    app.ui.set_screen(
        current_screen(screen, frame, timers, telemetry, elrs, finder));
    (void)app.ui.render();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void service_task(void*)
{
  app.telemetry_logger.start();
  while (true) {
    const TimeUs current = now_us();
    std::array<TelemetryEntry, kMaxTelemetrySensors> telemetry{};
    BatteryState battery_state = BatteryState::Unknown;
    ModuleState module_state = ModuleState::Starting;
    SafetyState safety_state = SafetyState::Booting;
    taskENTER_CRITICAL(&app.frame_lock);
    telemetry = app.latest_telemetry;
    battery_state = app.latest_battery_state;
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
    app.telemetry_logger.sample(app.service_telemetry, current);
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
    taskENTER_CRITICAL(&app.frame_lock);
    app.latest_finder = app.finder.status();
    taskEXIT_CRITICAL(&app.frame_lock);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

bool initialize_storage()
{
  if (!mount_model_filesystem()) {
    return false;
  }
  const auto loaded = app.model_store.load(app.model);
  if (loaded.success) {
    app.generation = loaded.generation;
    if (loaded.recovered) {
      app.diagnostics.push(
          {now_us(), LogSeverity::Warning, LogCode::StorageRecovered, 0, 0});
    }
  } else {
    app.model = make_default_model();
    std::string error;
    if (!app.model_store.save(app.model, 1, error)) {
      ESP_LOGE(kTag, "cannot create model: %s", error.c_str());
      return false;
    }
    app.generation = 1;
  }
  app.edit_model = app.model;

  std::array<AxisCalibration, kMaxAxes> calibration{};
  if (app.calibration_store.load(calibration)) {
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
    (void)app.files.write("scripts/startup.lua", script);
    (void)app.files.sync("scripts/startup.lua");
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

  const bool storage_ok = initialize_storage();
  const bool inputs_ok = app.board.initialize();
  const RawInputs startup_inputs =
      inputs_ok ? app.board.sample_inputs(now_us()) : RawInputs{};
  const bool calibration_requested =
      startup_inputs.switches[0] && startup_inputs.switches[1];
  const bool recovery_requested =
      inputs_ok && app.boot.enter_recovery(
                       app.board.recovery_button_pressed(), boot_attempt);
  const bool display_ok = app.display.initialize();
  (void)app.tones.initialize();
  app.audio.notify(AudioAlert::Startup);
  if (calibration_requested && display_ok) {
    if (!run_startup_calibration()) {
      ESP_LOGW(kTag, "stick calibration cancelled or failed");
    }
  }
  const bool crsf_ok = app.transport.initialize();

  const esp_reset_reason_t reset_reason = esp_reset_reason();
  const bool watchdog_recovery =
      reset_reason == ESP_RST_TASK_WDT || reset_reason == ESP_RST_WDT ||
      reset_reason == ESP_RST_INT_WDT;
  app.safety.boot_complete(storage_ok, watchdog_recovery);
  if (recovery_requested) {
    app.safety.request_lock();
  }
  app.module.start(app.model.model_id, now_us());
  app.elrs.start(now_us());

  BaseType_t control_created =
      xTaskCreate(control_task, "rivet-control", 6144, nullptr, 20, nullptr);
  BaseType_t ui_created =
      xTaskCreate(ui_task, "rivet-ui", 6144, nullptr, 5, nullptr);
  BaseType_t service_created =
      xTaskCreate(service_task, "rivet-service", 5120, nullptr, 3, nullptr);

  SelfTestResult self_test{};
  self_test.storage = storage_ok;
  self_test.inputs = inputs_ok;
  self_test.display = display_ok;
  self_test.crsf_uart = crsf_ok;
  self_test.control_task = control_created == pdPASS &&
                           ui_created == pdPASS &&
                           service_created == pdPASS;

  if (!app.boot.finish_startup(self_test, now_us())) {
    ESP_LOGE(kTag, "startup self-test failed");
    app.safety.request_lock();
  } else {
    (void)app.boot_state.mark_success();
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
