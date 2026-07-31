#include "rivettx/audio.hpp"
#include "rivettx/core.hpp"
#include "rivettx/crsf.hpp"
#include "rivettx/elrs.hpp"
#include "rivettx/product.hpp"
#include "rivettx/services.hpp"
#include "rivettx/storage.hpp"
#include "rivettx/ui.hpp"
#include "virtual_hardware.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace rivettx;

constexpr TimeUs kPeriodUs = 4000;
constexpr uint32_t kCycles = 1000;

class SimulatorWatchdog final : public IWatchdog {
 public:
  void kick() override
  {
    ++kicks;
  }

  uint32_t kicks = 0;
};

class SimulatorToneOutput final : public IToneOutput {
 public:
  bool play_tone(uint16_t, uint16_t) override
  {
    ++tones;
    return true;
  }

  void stop_tone() override
  {
    ++stops;
  }

  bool available() const override
  {
    return true;
  }

  uint32_t tones = 0;
  uint32_t stops = 0;
};

enum class ScenarioKind {
  Nominal,
  PacketLoss,
  Corruption,
  Disconnect,
  StaleInput,
  MissedDeadline,
};

struct ScenarioResult {
  std::string name;
  bool passed = true;
  std::vector<std::string> failures;
  uint32_t control_cycles = 0;
  uint32_t channel_frames = 0;
  uint32_t valid_telemetry_frames = 0;
  uint32_t crc_errors = 0;
  uint32_t dropped_telemetry_frames = 0;
  uint32_t corrupted_telemetry_frames = 0;
  uint32_t failed_writes = 0;
  uint32_t model_id_frames = 0;
  uint32_t missed_deadlines = 0;
  uint32_t stale_frames = 0;
  bool failsafe_observed = false;
  bool module_offline_observed = false;
  bool module_recovered = false;
  bool audio_warning_observed = false;
};

struct Options {
  std::string scenario = "all";
  std::string display = "all";
  std::string output_directory = "build";
  bool help = false;
};

const char* scenario_name(ScenarioKind kind)
{
  switch (kind) {
    case ScenarioKind::Nominal:
      return "nominal";
    case ScenarioKind::PacketLoss:
      return "packet-loss";
    case ScenarioKind::Corruption:
      return "corruption";
    case ScenarioKind::Disconnect:
      return "disconnect";
    case ScenarioKind::StaleInput:
      return "stale-input";
    case ScenarioKind::MissedDeadline:
      return "missed-deadline";
  }
  return "unknown";
}

bool parse_options(int argc, char** argv, Options& options)
{
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      options.help = true;
      continue;
    }
    if ((argument == "--scenario" || argument == "--display" ||
         argument == "--output-dir") &&
        index + 1 < argc) {
      const std::string value = argv[++index];
      if (argument == "--scenario") {
        options.scenario = value;
      } else if (argument == "--display") {
        options.display = value;
      } else {
        options.output_directory = value;
      }
      continue;
    }
    std::cerr << "unknown or incomplete option: " << argument << "\n";
    return false;
  }
  return true;
}

bool selected_scenario(const std::string& selection, ScenarioKind kind)
{
  return selection == "all" || selection == scenario_name(kind);
}

void require(ScenarioResult& result, bool condition,
             const std::string& description)
{
  if (!condition) {
    result.passed = false;
    result.failures.push_back(description);
  }
}

bool has_diagnostic(const DiagnosticLog& log, LogCode code)
{
  for (std::size_t index = 0; index < log.size(); ++index) {
    LogEvent event{};
    if (log.event_at(index, event) && event.code == code) {
      return true;
    }
  }
  return false;
}

sim::LinkFaultPlan fault_plan(ScenarioKind kind)
{
  sim::LinkFaultPlan plan{};
  if (kind == ScenarioKind::PacketLoss) {
    plan.drop_every_nth_telemetry_frame = 2;
  } else if (kind == ScenarioKind::Corruption) {
    plan.corrupt_every_nth_telemetry_frame = 3;
  } else if (kind == ScenarioKind::Disconnect) {
    plan.disconnect_start_us = 800000;
    plan.disconnect_end_us = 2400000;
  }
  return plan;
}

RawInputs make_inputs(ScenarioKind kind, uint32_t cycle, TimeUs now_us)
{
  RawInputs raw{};
  raw.valid = true;
  raw.sampled_at_us = now_us;
  raw.axes.fill(2048);
  raw.axes[0] = static_cast<int16_t>(
      2048 + std::sin(static_cast<double>(cycle) / 50.0) * 1700);

  const bool startup = now_us < 400000;
  const bool recovery =
      ((kind == ScenarioKind::Disconnect && now_us >= 2400000) ||
       (kind == ScenarioKind::StaleInput && now_us >= 2300000) ||
       (kind == ScenarioKind::MissedDeadline && now_us >= 2200000)) &&
      now_us < 3200000;
  raw.axes[2] = startup || recovery ? 100 : 2048;

  if (kind == ScenarioKind::StaleInput &&
      now_us >= 2200000 && now_us < 2300000) {
    raw.sampled_at_us = now_us - 100000;
  }
  return raw;
}

ScenarioResult run_scenario(ScenarioKind kind, const Model& model)
{
  ScenarioResult result{};
  result.name = scenario_name(kind);

  InputProcessor input_processor;
  MixerEngine mixer;
  SafetyManager safety;
  TelemetryRegistry telemetry;
  SimulatorWatchdog watchdog;
  ControlLoop control(input_processor, mixer, safety, telemetry, watchdog);
  DiagnosticLog diagnostics;
  CrsfParser parser(telemetry);
  sim::VirtualElrsModule transport(fault_plan(kind));
  ModuleSupervisor module(transport, parser, diagnostics);
  ElrsDeviceManager management(transport, parser);
  SimulatorToneOutput tone_output;
  AudioAlertScheduler audio(tone_output);
  AudioWarningMonitor warnings;

  safety.boot_complete(true, false);
  safety.request_enable();
  module.start(model.model_id, 0);
  management.start(0);

  ChannelFrame latest{};
  bool ever_enabled = false;
  bool saw_safe_after_enabled = false;
  bool saw_offline = false;
  bool saw_online_after_offline = false;
  bool saw_module_safety_lock = false;
  bool enable_reissued = false;
  bool previous_module_ready = false;
  for (uint32_t cycle = 0; cycle < kCycles; ++cycle) {
    const TimeUs now_us = static_cast<TimeUs>(cycle) * kPeriodUs;
    if (!enable_reissued && now_us >= 3000000 &&
        (kind == ScenarioKind::StaleInput ||
         kind == ScenarioKind::MissedDeadline)) {
      safety.request_enable();
      enable_reissued = true;
    }
    transport.advance(now_us);
    const bool module_ready =
        module.status().state == ModuleState::Online;
    safety.report_module_ready(module_ready);
    if (module_ready && !previous_module_ready) {
      safety.request_enable();
    }
    previous_module_ready = module_ready;
    const RawInputs raw = make_inputs(kind, cycle, now_us);
    const uint32_t duration =
        kind == ScenarioKind::MissedDeadline &&
                now_us >= 2200000 && now_us < 2200000 + kPeriodUs
            ? 2000
            : 250;
    const auto cycle_result =
        control.run(model, raw, 3800, now_us, now_us + duration);
    latest = cycle_result.frame;
    ever_enabled =
        ever_enabled || cycle_result.safety.state == SafetyState::Enabled;
    if (ever_enabled && latest.safe) {
      saw_safe_after_enabled = true;
    }
    if (ever_enabled &&
        cycle_result.safety.reason == SafetyReason::ModuleOffline) {
      saw_module_safety_lock = true;
    }
    (void)module.send_channels(latest, now_us + duration + 50);
    module.poll(now_us + duration + 100);
    management.tick(now_us + duration + 150);
    warnings.tick(telemetry, BatteryState::Normal, module.status().state,
                  cycle_result.safety.state, now_us + duration + 200,
                  audio);
    audio.tick(now_us + duration + 200);
    result.audio_warning_observed =
        result.audio_warning_observed ||
        audio.current_alert() == AudioAlert::ModuleOffline ||
        audio.current_alert() == AudioAlert::TelemetryLost ||
        audio.current_alert() == AudioAlert::LinkCritical;
    if (module.status().state == ModuleState::Offline) {
      saw_offline = true;
    }
    if (saw_offline && module.status().state == ModuleState::Online) {
      saw_online_after_offline = true;
    }
  }

  const auto& transport_stats = transport.stats();
  const auto& parser_stats = parser.stats();
  result.control_cycles = watchdog.kicks;
  result.channel_frames = transport_stats.channel_frames_received;
  result.valid_telemetry_frames = parser_stats.valid_frames;
  result.crc_errors = parser_stats.crc_errors;
  result.dropped_telemetry_frames =
      transport_stats.telemetry_frames_dropped;
  result.corrupted_telemetry_frames =
      transport_stats.telemetry_frames_corrupted;
  result.failed_writes = transport_stats.failed_writes;
  result.model_id_frames = transport_stats.model_id_frames_received;
  result.missed_deadlines = safety.status().missed_deadlines;
  result.stale_frames = safety.status().stale_frames;
  result.failsafe_observed = saw_safe_after_enabled;
  result.module_offline_observed = saw_offline;
  result.module_recovered = saw_online_after_offline;

  int32_t value = 0;
  require(result, watchdog.kicks == kCycles,
          "watchdog was not kicked for every control cycle");
  require(result, ever_enabled, "safety never enabled outputs");
  require(result, safety.status().state == SafetyState::Enabled,
          "safety did not recover to Enabled");
  require(result, !latest.safe, "final channel frame is failsafe");
  require(result, module.status().state == ModuleState::Online,
          "ELRS module did not finish Online");
  require(result, management.status().state == ElrsManagerState::Ready,
          "ELRS parameter discovery did not finish Ready");
  require(result, management.status().power.available,
          "ELRS power options were not discovered");
  require(result, transport.baud_rate() == 400000,
          "CRSF UART baud is not 400000");
  require(result, transport.model_id() == model.model_id,
          "ELRS module received the wrong model ID");
  const uint32_t minimum_channel_frames =
      kind == ScenarioKind::Disconnect ? 550 : 900;
  require(result,
          transport_stats.channel_frames_received > minimum_channel_frames,
          "too few valid channel frames reached ELRS");
  require(result, transport_stats.invalid_radio_frames == 0,
          "radio emitted an invalid CRSF frame");
  const uint32_t minimum_pings =
      kind == ScenarioKind::Disconnect ? 2 : 3;
  require(result, transport_stats.device_pings_received >= minimum_pings,
          "device discovery pings were not exchanged");
  require(result, transport_stats.queue_overflows == 0,
          "virtual UART receive queue overflowed");
  require(result,
          std::abs(static_cast<int>(transport.channels()[0]) -
                   static_cast<int>(latest.channels[0])) <= 3,
          "decoded ELRS channel differs from the transmitted channel");
  require(result,
          telemetry.value(crsf::SensorUplinkLinkQuality, value) &&
              value == 96,
          "link-quality telemetry was not decoded");
  require(result,
          telemetry.value(crsf::SensorBatteryVoltage, value) &&
              value == 3800,
          "battery telemetry was not decoded");
  require(result,
          telemetry.value(crsf::SensorGpsSatellites, value) && value == 14,
          "GPS telemetry was not decoded");

  if (kind == ScenarioKind::PacketLoss) {
    require(result, transport_stats.telemetry_frames_dropped > 0,
            "packet-loss injection did not drop telemetry");
    require(result, parser_stats.crc_errors == 0,
            "dropped packets unexpectedly caused CRC errors");
  } else if (kind == ScenarioKind::Corruption) {
    require(result, transport_stats.telemetry_frames_corrupted > 0,
            "corruption injection did not alter telemetry");
    require(result,
            parser_stats.crc_errors ==
                transport_stats.telemetry_frames_corrupted,
            "parser did not reject every corrupted frame");
  } else if (kind == ScenarioKind::Disconnect) {
    require(result, saw_offline, "module disconnect was not detected");
    require(result, saw_module_safety_lock,
            "module disconnect did not lock channel outputs");
    require(result, saw_online_after_offline,
            "module did not recover after reconnect");
    require(result, transport_stats.failed_writes > 0,
            "UART writes did not fail during disconnect");
    require(result, transport_stats.model_id_frames_received >= 2,
            "model ID was not restored after reconnect");
    require(result, has_diagnostic(diagnostics, LogCode::ModuleLost),
            "module-lost diagnostic is missing");
    require(result, has_diagnostic(diagnostics, LogCode::ModuleRecovered),
            "module-recovered diagnostic is missing");
    require(result, result.audio_warning_observed,
            "disconnect did not trigger an audible warning");
  } else if (kind == ScenarioKind::StaleInput) {
    require(result, safety.status().stale_frames > 0,
            "stale-input fault was not detected");
    require(result, saw_safe_after_enabled,
            "stale input did not force a failsafe frame");
  } else if (kind == ScenarioKind::MissedDeadline) {
    require(result, safety.status().missed_deadlines == 1,
            "mixer deadline fault was not counted");
    require(result, saw_safe_after_enabled,
            "missed deadline did not force a failsafe frame");
  }
  return result;
}

struct DisplayProfile {
  const char* name;
  DisplayCapabilities capabilities;
  const char* filename;
};

class SimulatorOpenPocketScreens final : public IOpenPocketScreenProvider {
 public:
  SimulatorOpenPocketScreens(const Model& model, const UiHomeStatus& home,
                             const ChannelFrame& channels,
                             const VrxStatus& vrx)
      : model_(model), home_(home), channels_(channels), vrx_(vrx)
  {
  }

  UiScreen screen(OpenPocketPage page) override
  {
    switch (page) {
      case OpenPocketPage::Home:
        return make_openpocket_home_screen(model_, home_);
      case OpenPocketPage::Warnings:
        return make_warnings_screen(home_);
      case OpenPocketPage::Models:
        return {"models", "Models",
                {{"select.0", model_.name.data(), "ACTIVE",
                  UiFieldKind::Action, 0, 0, 31, false, true},
                 {"copy", "COPY ACTIVE", "ENTER", UiFieldKind::Action,
                  0, 0, 1, false, true}}};
      case OpenPocketPage::Outputs:
        return make_outputs_screen(channels_);
      case OpenPocketPage::ModelSetup:
        return make_model_setup_screen(model_);
      case OpenPocketPage::Inputs:
        return make_inputs_screen(model_);
      case OpenPocketPage::Mixes:
        return make_mixes_screen(model_);
      case OpenPocketPage::Limits:
        return make_output_limits_screen(model_);
      case OpenPocketPage::FlightModes:
        return make_flight_modes_screen(model_);
      case OpenPocketPage::Curves:
        return make_curves_screen(model_);
      case OpenPocketPage::LogicalSwitches:
        return make_logical_switches_screen(model_);
      case OpenPocketPage::SpecialFunctions:
        return make_special_functions_screen(model_);
      case OpenPocketPage::Timers:
        return make_timers_screen(model_, timers_);
      case OpenPocketPage::Elrs:
        return make_elrs_screen(elrs_, true);
      case OpenPocketPage::Finder:
        return make_elrs_finder_screen(finder_);
      case OpenPocketPage::Video:
        return make_openpocket_video_screen(vrx_);
      case OpenPocketPage::Usb:
        return {"usb", "USB Simulator",
                {{"state", "USB GAMEPAD", "S3 READY", UiFieldKind::Label,
                  0, 0, 0, false, true},
                 {"enable", "START SIMULATOR", "ENTER",
                  UiFieldKind::Action, 0, 0, 1, false, true}}};
      case OpenPocketPage::Web:
        return {"web", "Web Config",
                {{"state", "CONFIG WIFI", "OFF", UiFieldKind::Label,
                  0, 0, 0, false, true},
                 {"start", "START WEB CONFIG", "ENTER",
                  UiFieldKind::Action, 0, 0, 1, false, true}}};
      case OpenPocketPage::Telemetry:
        return make_telemetry_screen(
            {{"lq", "UPLINK LQ", "96%", UiFieldKind::Label,
              96, 0, 100, false, true},
             {"rssi", "UPLINK RSSI", "-42DBM", UiFieldKind::Label,
              -42, -140, 0, false, true}});
      case OpenPocketPage::Power:
        return {"power", "Power",
                {{"battery", "BATTERY", "3800MV", UiFieldKind::Label,
                  3800, 0, 0, false, true},
                 {"state", "STATE", "OK", UiFieldKind::Label,
                  0, 0, 0, false, true}}};
      case OpenPocketPage::System:
        return make_system_screen(3800, 65536, 0, "sim");
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
    return {};
  }

 private:
  const Model& model_;
  const UiHomeStatus& home_;
  const ChannelFrame& channels_;
  const VrxStatus& vrx_;
  std::array<TimerState, kMaxTimers> timers_{};
  ElrsManagerStatus elrs_{};
  ElrsFinderStatus finder_{};
};

bool render_displays(const Options& options, const Model& model,
                     const ChannelFrame& channels)
{
  const std::vector<DisplayProfile> profiles{
      {"compact", {128, 64, 1, false, false, 8}, "sim-screen.pbm"},
      {"medium", {240, 135, 1, false, true, 12},
       "sim-screen-medium.pbm"},
      {"large", {480, 320, 1, true, true, 16},
       "sim-screen-large.pbm"}};
  bool success = true;
  for (const auto& profile : profiles) {
    if (options.display != "all" && options.display != profile.name) {
      continue;
    }
    const std::filesystem::path path =
        std::filesystem::path(options.output_directory) / profile.filename;
    sim::PbmDisplay display(profile.capabilities, path.string());
    MonoCanvas canvas(profile.capabilities.width, profile.capabilities.height);
    UiController ui(display, canvas);
    UiHomeStatus home{};
    for (std::size_t axis = 0; axis < home.axes.size(); ++axis) {
      home.axes[axis] = channels.channels[axis];
    }
    home.channels = channels.channels;
    home.battery_mv = 3800;
    home.battery_percent = 67;
    home.battery_percent_valid = true;
    home.link_quality = 96;
    home.outputs_enabled = true;
    home.module_online = true;
    home.video_signal = true;
    ui.set_screen(make_oled_home_screen(model, home));
    const bool rendered = ui.render();
    if (!rendered || display.flushes() != 1 ||
        display.last_lit_pixels() == 0) {
      std::cerr << "[FAIL] display " << profile.name << "\n";
      success = false;
    } else {
      std::cout << "[PASS] display " << profile.name << " "
                << profile.capabilities.width << "x"
                << profile.capabilities.height << " -> " << path.string()
                << "\n";
    }
  }
  return success;
}

bool render_openpocket_osd(const Options& options, const Model& model,
                           const ChannelFrame& channels)
{
  if (options.display != "all" && options.display != "openpocket") {
    return true;
  }
  const std::filesystem::path path =
      std::filesystem::path(options.output_directory) /
      "sim-openpocket-osd.txt";
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    std::cerr << "[FAIL] display openpocket: cannot open "
              << path.string() << "\n";
    return false;
  }

  UiHomeStatus home{};
  for (std::size_t axis = 0; axis < home.axes.size(); ++axis) {
    home.axes[axis] = channels.channels[axis];
  }
  home.channels = channels.channels;
  home.battery_mv = 3800;
  home.battery_percent = 67;
  home.battery_percent_valid = true;
  home.link_quality = 96;
  home.module_online = true;
  home.video_signal = true;
  VrxStatus vrx{};
  vrx.band = 3;
  vrx.channel = 3;
  vrx.frequency_mhz = vrx_frequency_mhz(vrx.band, vrx.channel);
  vrx.strength_percent = 88;
  vrx.available = true;
  vrx.signal_fresh = true;
  vrx.video_signal = true;

  const auto write_frame = [&output](const char* name,
                                     const CharacterOsdFrame& frame) {
    output << "=== " << name << " ===\n";
    for (std::size_t row = 0; row < kOsdRows; ++row) {
      output.write(
          frame.cells.data() + row * kOsdColumns,
          static_cast<std::streamsize>(kOsdColumns));
      output << "\n";
    }
  };

  SimulatorOpenPocketScreens screens(model, home, channels, vrx);
  OpenPocketMenuController menu(screens);
  constexpr TimeUs hud_start_us = 1000000;
  menu.start(home, hud_start_us);
  (void)menu.render(vrx);
  write_frame("HUD CLEAR", menu.frame());

  UiHomeStatus notification_home = home;
  notification_home.warning_count = 1;
  notification_home.warnings[0] = UiWarningCode::BatteryLow;
  menu.refresh(notification_home, hud_start_us);
  (void)menu.render(vrx);
  write_frame("HUD NOTIFICATION", menu.frame());

  menu.refresh(notification_home,
               hud_start_us + kOpenPocketNotificationDurationUs);
  (void)menu.render(vrx);
  write_frame("HUD NOTIFICATION EXPIRED", menu.frame());

  UiHomeStatus critical_home = home;
  critical_home.warning_count = 2;
  critical_home.warnings[0] = UiWarningCode::VideoNoSignal;
  critical_home.warnings[1] = UiWarningCode::BatteryCritical;
  menu.refresh(critical_home, hud_start_us + 5000000);
  (void)menu.render(vrx);
  write_frame("HUD CRITICAL", menu.frame());

  menu.refresh(critical_home, hud_start_us + 15000000);
  (void)menu.render(vrx);
  write_frame("HUD CRITICAL STILL ACTIVE", menu.frame());

  menu.refresh(home, hud_start_us + 15000001);
  (void)menu.render(vrx);
  write_frame("HUD WARNING RESOLVED", menu.frame());

  (void)menu.handle({UiEventType::Enter});
  (void)menu.render(vrx);
  write_frame("MAIN MENU", menu.frame());

  (void)menu.handle({UiEventType::Enter});
  (void)menu.handle({UiEventType::Rotate, 1});
  (void)menu.render(vrx);
  write_frame("MODEL MENU", menu.frame());

  (void)menu.handle({UiEventType::Enter});
  (void)menu.render(vrx);
  write_frame("MODEL DETAIL", menu.frame());

  (void)menu.handle({UiEventType::Enter});
  (void)menu.handle({UiEventType::Rotate, 2});
  (void)menu.render(vrx);
  write_frame("MODEL EDIT", menu.frame());

  (void)menu.handle({UiEventType::Back});
  (void)menu.render(vrx);
  write_frame("BACK TO HUD", menu.frame());

  (void)menu.handle({UiEventType::Enter});
  (void)menu.handle({UiEventType::Enter});
  (void)menu.handle({UiEventType::Home});
  (void)menu.render(vrx);
  write_frame("HOME TO HUD", menu.frame());

  CharacterOsdUi page_ui;
  const std::array<std::pair<OpenPocketMenuGroup, const char*>, 7> groups{{
      {OpenPocketMenuGroup::Model, "GROUP MODEL"},
      {OpenPocketMenuGroup::Radio, "GROUP RADIO"},
      {OpenPocketMenuGroup::Elrs, "GROUP ELRS"},
      {OpenPocketMenuGroup::Video, "GROUP VIDEO"},
      {OpenPocketMenuGroup::Usb, "GROUP USB"},
      {OpenPocketMenuGroup::Diagnostics, "GROUP DIAGNOSTICS"},
      {OpenPocketMenuGroup::System, "GROUP SYSTEM"},
  }};
  for (const auto& group : groups) {
    page_ui.set_screen(make_openpocket_group_menu_screen(group.first));
    page_ui.update_home(home);
    (void)page_ui.render(vrx);
    write_frame(group.second, page_ui.frame());
  }

  const std::array<std::pair<OpenPocketPage, const char*>, 20> details{{
      {OpenPocketPage::Warnings, "DETAIL WARNINGS"},
      {OpenPocketPage::Models, "DETAIL MODELS"},
      {OpenPocketPage::Outputs, "DETAIL OUTPUTS"},
      {OpenPocketPage::ModelSetup, "DETAIL MODEL SETUP"},
      {OpenPocketPage::Inputs, "DETAIL INPUTS"},
      {OpenPocketPage::Mixes, "DETAIL MIXES"},
      {OpenPocketPage::Limits, "DETAIL LIMITS"},
      {OpenPocketPage::FlightModes, "DETAIL FLIGHT MODES"},
      {OpenPocketPage::Curves, "DETAIL CURVES"},
      {OpenPocketPage::LogicalSwitches, "DETAIL LOGICAL SWITCHES"},
      {OpenPocketPage::SpecialFunctions, "DETAIL SPECIAL FUNCTIONS"},
      {OpenPocketPage::Timers, "DETAIL TIMERS"},
      {OpenPocketPage::Elrs, "DETAIL ELRS"},
      {OpenPocketPage::Finder, "DETAIL FINDER"},
      {OpenPocketPage::Video, "DETAIL VIDEO"},
      {OpenPocketPage::Usb, "DETAIL USB"},
      {OpenPocketPage::Web, "DETAIL WEB"},
      {OpenPocketPage::Telemetry, "DETAIL TELEMETRY"},
      {OpenPocketPage::Power, "DETAIL POWER"},
      {OpenPocketPage::System, "DETAIL SYSTEM"},
  }};
  for (const auto& detail : details) {
    page_ui.set_screen(screens.screen(detail.first));
    page_ui.update_home(home);
    (void)page_ui.render(vrx);
    write_frame(detail.second, page_ui.frame());
  }

  UiHomeStatus error_home = home;
  error_home.warning_count = 2;
  error_home.warnings[0] = UiWarningCode::BatteryCritical;
  error_home.warnings[1] = UiWarningCode::VideoNoSignal;
  VrxStatus error_vrx = vrx;
  error_vrx.video_signal = false;
  page_ui.set_screen(make_openpocket_home_screen(model, error_home));
  page_ui.update_home(error_home);
  (void)page_ui.render(error_vrx);
  write_frame("HOME ERROR", page_ui.frame());

  UiScreen clipping{
      "clipping",
      "OPENPOCKET TITLE THAT IS TOO LONG",
      {{"long", "A VERY LONG FIELD LABEL THAT MUST CLIP",
        "A VALUE THAT MUST CLIP", UiFieldKind::Label,
        0, 0, 0, false, true}}};
  page_ui.set_screen(std::move(clipping));
  page_ui.update_home(home);
  (void)page_ui.render(vrx);
  write_frame("CLIPPING", page_ui.frame());

  output.flush();
  const bool success = output.good();
  if (success) {
    std::cout << "[PASS] display openpocket 30x16 -> "
              << path.string() << "\n";
  } else {
    std::cerr << "[FAIL] display openpocket: write failed\n";
  }
  return success;
}

void write_json_report(const std::filesystem::path& path,
                       const std::vector<ScenarioResult>& results,
                       bool passed)
{
  std::ofstream output(path, std::ios::trunc);
  output << "{\n  \"passed\": " << (passed ? "true" : "false")
         << ",\n  \"scenarios\": [\n";
  for (std::size_t index = 0; index < results.size(); ++index) {
    const auto& item = results[index];
    output << "    {\"name\":\"" << item.name << "\",\"passed\":"
           << (item.passed ? "true" : "false")
           << ",\"control_cycles\":" << item.control_cycles
           << ",\"channel_frames\":" << item.channel_frames
           << ",\"valid_telemetry_frames\":"
           << item.valid_telemetry_frames
           << ",\"crc_errors\":" << item.crc_errors
           << ",\"dropped_telemetry_frames\":"
           << item.dropped_telemetry_frames
           << ",\"corrupted_telemetry_frames\":"
           << item.corrupted_telemetry_frames
           << ",\"failed_writes\":" << item.failed_writes
           << ",\"model_id_frames\":" << item.model_id_frames
           << ",\"missed_deadlines\":" << item.missed_deadlines
           << ",\"stale_frames\":" << item.stale_frames
           << ",\"failsafe_observed\":"
           << (item.failsafe_observed ? "true" : "false")
           << ",\"module_offline_observed\":"
           << (item.module_offline_observed ? "true" : "false")
           << ",\"module_recovered\":"
           << (item.module_recovered ? "true" : "false")
           << ",\"audio_warning_observed\":"
           << (item.audio_warning_observed ? "true" : "false") << "}";
    output << (index + 1 == results.size() ? "\n" : ",\n");
  }
  output << "  ]\n}\n";
}

void print_help()
{
  std::cout
      << "RivetTX virtual hardware simulator\n"
      << "usage: rivettx-sim [--scenario NAME] [--display NAME] "
         "[--output-dir PATH]\n"
      << "scenarios: all, nominal, packet-loss, corruption, disconnect, "
         "stale-input, missed-deadline\n"
      << "displays: all, compact, medium, large, openpocket\n";
}

}  // namespace

int main(int argc, char** argv)
{
  Options options{};
  if (!parse_options(argc, argv, options)) {
    print_help();
    return 2;
  }
  if (options.help) {
    print_help();
    return 0;
  }

  const std::vector<ScenarioKind> kinds{
      ScenarioKind::Nominal,       ScenarioKind::PacketLoss,
      ScenarioKind::Corruption,    ScenarioKind::Disconnect,
      ScenarioKind::StaleInput,    ScenarioKind::MissedDeadline};
  bool known_scenario = options.scenario == "all";
  for (const auto kind : kinds) {
    known_scenario =
        known_scenario || options.scenario == scenario_name(kind);
  }
  const bool known_display =
      options.display == "all" || options.display == "compact" ||
      options.display == "medium" || options.display == "large" ||
      options.display == "openpocket";
  if (!known_scenario || !known_display) {
    std::cerr << "invalid scenario or display selection\n";
    print_help();
    return 2;
  }

  std::filesystem::create_directories(options.output_directory);
  const auto model_directory =
      std::filesystem::path(options.output_directory) / "sim-data";
  std::filesystem::create_directories(model_directory);
  PosixFileStore files(model_directory.string());
  TransactionalModelStore models(files, "default.rvm");
  Model model = make_default_model();
  model.model_id = 23;
  std::string storage_error;
  if (!models.save(model, 1, storage_error)) {
    std::cerr << "model save failed: " << storage_error << "\n";
    return 1;
  }
  Model loaded{};
  const auto loaded_result = models.load(loaded);
  if (!loaded_result.success || loaded_result.generation != 1 ||
      loaded.model_id != model.model_id) {
    std::cerr << "transactional model round-trip failed: "
              << loaded_result.error << "\n";
    return 1;
  }

  std::vector<ScenarioResult> results;
  ChannelFrame display_channels{};
  for (const auto kind : kinds) {
    if (!selected_scenario(options.scenario, kind)) {
      continue;
    }
    auto result = run_scenario(kind, loaded);
    std::cout << (result.passed ? "[PASS] " : "[FAIL] ") << result.name
              << ": " << result.control_cycles << " cycles, "
              << result.channel_frames << " channel frames, "
              << result.valid_telemetry_frames << " valid RX frames\n";
    for (const auto& failure : result.failures) {
      std::cerr << "       " << failure << "\n";
    }
    results.push_back(std::move(result));
  }

  display_channels.channels[0] = 410;
  display_channels.channels[1] = -205;
  display_channels.channels[2] = -1024;
  display_channels.channels[3] = 128;
  display_channels.safe = false;
  const bool displays_passed =
      render_displays(options, loaded, display_channels) &&
      render_openpocket_osd(options, loaded, display_channels);
  bool passed = displays_passed;
  for (const auto& result : results) {
    passed = passed && result.passed;
  }
  const auto report_path =
      std::filesystem::path(options.output_directory) / "sim-report.json";
  write_json_report(report_path, results, passed);
  std::cout << (passed ? "simulation passed" : "simulation failed")
            << "; report: " << report_path.string() << "\n";
  return passed ? 0 : 1;
}
