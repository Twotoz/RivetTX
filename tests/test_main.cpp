#include "rivettx/core.hpp"
#include "rivettx/crsf.hpp"
#include "rivettx/elrs.hpp"
#include "rivettx/services.hpp"
#include "rivettx/storage.hpp"
#include "rivettx/ui.hpp"
#include "virtual_hardware.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace rivettx;

int failures = 0;
int checks = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    ++checks;                                                                \
    if (!(condition)) {                                                      \
      ++failures;                                                            \
      std::cerr << __FILE__ << ":" << __LINE__                              \
                << " check failed: " #condition "\n";                       \
    }                                                                        \
  } while (false)

class FakeWatchdog final : public IWatchdog {
 public:
  void kick() override
  {
    ++count;
  }
  uint32_t count = 0;
};

class FakeToneOutput final : public IToneOutput {
 public:
  bool play_tone(uint16_t frequency_hz,
                 uint16_t duration_ms) override
  {
    last_frequency = frequency_hz;
    last_duration = duration_ms;
    ++plays;
    return enabled;
  }
  void stop_tone() override
  {
    ++stops;
  }
  bool available() const override
  {
    return enabled;
  }

  bool enabled = true;
  uint16_t last_frequency = 0;
  uint16_t last_duration = 0;
  uint32_t plays = 0;
  uint32_t stops = 0;
};

class FakeDisplay final : public IDisplaySink {
 public:
  explicit FakeDisplay(DisplayCapabilities capabilities)
      : capabilities_(capabilities)
  {
  }
  const DisplayCapabilities& capabilities() const override
  {
    return capabilities_;
  }
  bool flush(const MonoCanvas&) override
  {
    ++flushes;
    return true;
  }
  DisplayCapabilities capabilities_;
  uint32_t flushes = 0;
};

class FakeTransport final : public ICrsfTransport {
 public:
  bool write(const uint8_t* data, std::size_t size) override
  {
    writes.emplace_back(data, data + size);
    return write_ok;
  }
  std::size_t read(uint8_t* data, std::size_t capacity) override
  {
    const std::size_t count = std::min(capacity, receive.size());
    std::copy(receive.begin(), receive.begin() + count, data);
    receive.erase(receive.begin(), receive.begin() + count);
    return count;
  }
  void set_baud_rate(uint32_t value) override
  {
    baud = value;
  }
  void reset_module() override
  {
    ++resets;
  }
  bool write_ok = true;
  uint32_t baud = 0;
  uint32_t resets = 0;
  std::vector<std::vector<uint8_t>> writes;
  std::vector<uint8_t> receive;
};

class FakeScript final : public IScriptVm {
 public:
  ScriptSliceResult run_slice(uint32_t) override
  {
    return next;
  }
  void terminate() override
  {
    terminated = true;
  }
  ScriptSliceResult next{};
  bool terminated = false;
};

class FakeOta final : public IOtaBackend {
 public:
  bool running_image_pending_verification() const override
  {
    return pending;
  }
  bool mark_running_image_valid() override
  {
    marked = true;
    return mark_result;
  }
  bool request_rollback() override
  {
    rollback = true;
    return true;
  }
  bool begin_https_update(const std::string& url) override
  {
    updated_url = url;
    return update_result;
  }
  bool pending = false;
  bool marked = false;
  bool rollback = false;
  bool mark_result = true;
  bool update_result = true;
  std::string updated_url;
};

class MemoryTelemetrySink final : public ITelemetryLogSink {
 public:
  bool append(TimeUs time_us, uint16_t sensor_id, int32_t value) override
  {
    samples.push_back({time_us, sensor_id, value});
    return true;
  }
  bool flush() override
  {
    ++flushes;
    return true;
  }
  struct Sample {
    TimeUs time_us;
    uint16_t sensor_id;
    int32_t value;
  };
  std::vector<Sample> samples;
  uint32_t flushes = 0;
};

class MemoryBackup final : public IBackupEndpoint {
 public:
  bool publish(const std::string& name,
               const std::vector<uint8_t>& data) override
  {
    stored_name = name;
    stored = data;
    return true;
  }
  bool receive(const std::string& name,
               std::vector<uint8_t>& data) override
  {
    if (name != stored_name) {
      return false;
    }
    data = stored;
    return true;
  }
  std::string stored_name;
  std::vector<uint8_t> stored;
};

class FakeSpecialActions final : public ISpecialActionHandler {
 public:
  void execute(SpecialAction action, int16_t parameter,
               TimeUs) override
  {
    last_action = action;
    last_parameter = parameter;
    ++calls;
  }
  SpecialAction last_action = SpecialAction::None;
  int16_t last_parameter = 0;
  uint32_t calls = 0;
};

void test_inputs_and_mixer()
{
  InputProcessor processor;
  RawInputs raw{};
  raw.valid = true;
  raw.sampled_at_us = 1000;
  raw.axes = {0, 2048, 4095, 2048, 2048, 2048, 2048, 2048};
  const auto controls = processor.process(raw);
  CHECK(controls.axes[0] <= -1000);
  CHECK(controls.axes[1] == 0);
  CHECK(controls.axes[2] >= 1000);

  Model model = make_default_model();
  TelemetryRegistry telemetry;
  MixerEngine mixer;
  const auto frame = mixer.evaluate(model, controls, telemetry, 1000);
  CHECK(frame.channels[0] <= -1000);
  CHECK(frame.channels[1] == 0);
  CHECK(frame.channels[2] >= 1000);
}

void test_mixer_features()
{
  Model model = make_default_model();
  model.curve_count = 1;
  model.curves[0].enabled = true;
  model.curves[0].points = {-1024, -900, -700, -350, 0,
                            350,   700,  900,  1024};
  model.inputs[0].expo_percent = 50;
  model.inputs[0].curve_index = 0;
  model.mix_count = 2;
  model.mixes[1].enabled = true;
  model.mixes[1].destination = 0;
  model.mixes[1].source = {SourceKind::Constant, 0, 100};
  model.mixes[1].mode = MixMode::Add;
  model.mixes[1].speed_up_per_second = 100;

  ControlInputs inputs{};
  inputs.valid = true;
  inputs.sampled_at_us = 1000;
  inputs.axes[0] = 512;
  TelemetryRegistry telemetry;
  MixerEngine mixer;
  const auto first = mixer.evaluate(model, inputs, telemetry, 1000);
  const auto second = mixer.evaluate(model, inputs, telemetry, 101000);
  CHECK(first.channels[0] > 0);
  CHECK(second.channels[0] >= first.channels[0]);

  model.logical_switch_count = 1;
  model.logical_switches[0].operation = LogicalSwitchOp::Greater;
  model.logical_switches[0].lhs = {SourceKind::Axis, 0, 0};
  model.logical_switches[0].rhs = {SourceKind::Constant, 0, 0};
  model.logical_switches[0].threshold = 100;
  (void)mixer.evaluate(model, inputs, telemetry, 201000);
  CHECK(mixer.logical_switch_values()[0]);
}

void test_safety()
{
  Model model = make_default_model();
  InputProcessor input;
  MixerEngine mixer;
  TelemetryRegistry telemetry;
  SafetyConfig config{};
  config.healthy_cycles_before_ready = 2;
  SafetyManager safety(config);
  FakeWatchdog watchdog;
  ControlLoop loop(input, mixer, safety, telemetry, watchdog);
  safety.boot_complete(true, false);
  safety.request_enable();

  RawInputs raw{};
  raw.valid = true;
  raw.axes[2] = 0;
  raw.sampled_at_us = 1000;
  auto one = loop.run(model, raw, 3800, 1000, 1100);
  CHECK(one.frame.safe);
  raw.sampled_at_us = 5000;
  auto two = loop.run(model, raw, 3800, 5000, 5100);
  CHECK(!two.frame.safe);
  CHECK(two.safety.state == SafetyState::Enabled);

  raw.sampled_at_us = 0;
  auto stale = loop.run(model, raw, 3800, 100000, 100100);
  CHECK(stale.frame.safe);
  CHECK(stale.safety.reason == SafetyReason::InputsStale);

  safety.report_battery(3000);
  CHECK(safety.status().state == SafetyState::Fault);
  CHECK(watchdog.count == 3);
}

void test_crsf()
{
  ChannelFrame channels{};
  channels.channels[0] = -1024;
  channels.channels[1] = 0;
  channels.channels[2] = 1024;
  const auto frame = crsf::make_channels_frame(channels);
  CHECK(frame.size == 26);
  CHECK(frame.bytes[0] == crsf::kAddressModule);
  CHECK(frame.bytes[2] == crsf::kFrameRcChannelsPacked);
  CHECK(frame.bytes[frame.size - 1] ==
        crsf::crc8_dvb_s2(frame.bytes.data() + 2, frame.size - 3));

  std::array<uint8_t, 12> battery{
      crsf::kAddressRadio, 10, crsf::kFrameBattery,
      0x00, 0x7B, 0x00, 0x2D, 0x00, 0x01, 0xF4, 88, 0};
  battery.back() =
      crsf::crc8_dvb_s2(battery.data() + 2, battery.size() - 3);
  TelemetryRegistry telemetry;
  CrsfParser parser(telemetry);
  for (const auto byte : battery) {
    (void)parser.feed(byte, 5000);
  }
  int32_t voltage = 0;
  CHECK(parser.stats().valid_frames == 1);
  CHECK(telemetry.value(crsf::SensorBatteryVoltage, voltage));
  CHECK(voltage == 12300);
  crsf::Frame popped{};
  CHECK(parser.pop_frame(popped));
  CHECK(popped.bytes[2] == crsf::kFrameBattery);

  battery.back() ^= 1;
  for (const auto byte : battery) {
    (void)parser.feed(byte, 6000);
  }
  CHECK(parser.stats().crc_errors == 1);
}

void test_virtual_elrs_module()
{
  sim::LinkFaultPlan faults{};
  faults.corrupt_every_nth_telemetry_frame = 4;
  faults.maximum_read_chunk = 5;
  sim::VirtualElrsModule transport(faults);
  TelemetryRegistry telemetry;
  CrsfParser parser(telemetry);
  DiagnosticLog diagnostics;
  ModuleSupervisor module(transport, parser, diagnostics);
  module.start(17, 0);
  module.request_bind(false, 50);

  ChannelFrame channels{};
  channels.channels[0] = -512;
  channels.channels[1] = 256;
  channels.channels[2] = 1024;
  bool all_channel_writes_succeeded = true;
  for (uint32_t cycle = 0; cycle < 500; ++cycle) {
    const TimeUs now_us = static_cast<TimeUs>(cycle) * 4000;
    channels.sequence = cycle;
    transport.advance(now_us);
    all_channel_writes_succeeded =
        module.send_channels(channels, now_us + 100) &&
        all_channel_writes_succeeded;
    module.poll(now_us + 200);
  }

  int32_t value = 0;
  CHECK(all_channel_writes_succeeded);
  CHECK(transport.baud_rate() == 400000);
  CHECK(transport.model_id() == 17);
  CHECK(transport.stats().model_id_frames_received == 1);
  CHECK(transport.stats().bind_commands_received == 1);
  CHECK(transport.stats().channel_frames_received == 500);
  CHECK(transport.stats().device_pings_received == 2);
  CHECK(transport.stats().invalid_radio_frames == 0);
  CHECK(transport.stats().telemetry_frames_corrupted > 0);
  CHECK(parser.stats().crc_errors ==
        transport.stats().telemetry_frames_corrupted);
  CHECK(module.status().state == ModuleState::Online);
  CHECK(std::abs(static_cast<int>(transport.channels()[0]) + 512) <= 2);
  CHECK(telemetry.value(crsf::SensorUplinkLinkQuality, value));
  CHECK(value == 96);
  CHECK(telemetry.value(crsf::SensorBatteryVoltage, value));
  CHECK(value == 3800);
  CHECK(telemetry.value(crsf::SensorGpsSatellites, value));
  CHECK(value == 14);
  CHECK(telemetry.value(crsf::SensorUplinkRssi, value));
  CHECK(value == -50);
  CHECK(telemetry.value(crsf::SensorTxPower, value));
  CHECK(value == 100);
  const auto* tx_power = telemetry.find(crsf::SensorTxPower);
  CHECK(tx_power != nullptr);
  CHECK(tx_power->unit == TelemetryUnit::Milliwatt);
}

void test_elrs_management_and_finder()
{
  sim::VirtualElrsModule transport;
  TelemetryRegistry telemetry;
  CrsfParser parser(telemetry);
  DiagnosticLog diagnostics;
  ModuleSupervisor module(transport, parser, diagnostics);
  ElrsDeviceManager management(transport, parser);
  module.start(11, 0);
  management.start(0);

  ChannelFrame channels{};
  TimeUs now_us = 0;
  auto run_cycles = [&](uint32_t count) {
    for (uint32_t cycle = 0; cycle < count; ++cycle) {
      now_us += 4000;
      transport.advance(now_us);
      (void)module.send_channels(channels, now_us + 100);
      module.poll(now_us + 200);
      management.tick(now_us + 300);
    }
  };

  run_cycles(500);
  const auto& discovered = management.status();
  CHECK(discovered.state == ElrsManagerState::Ready);
  CHECK(std::string(discovered.device_name.data()) == "Virtual ELRS");
  CHECK(discovered.firmware_version == 0x00040001);
  CHECK(discovered.fields_discovered == 9);
  CHECK(discovered.power.available);
  CHECK(discovered.power.option_count == 7);
  CHECK(std::string(discovered.power.options[3].data()) == "100mW");
  CHECK(discovered.dynamic_power.available);
  CHECK(discovered.switch_mode.available);
  CHECK(discovered.telemetry_ratio.available);
  CHECK(discovered.bind_available);
  CHECK(discovered.wifi_update_available);
  CHECK(parser.stats().management_drops == 0);
  CHECK(transport.stats().parameter_reads_received >
        discovered.field_count);

  CHECK(management.request_power(4));
  run_cycles(500);
  CHECK(transport.power_option() == 4);
  CHECK(management.status().state == ElrsManagerState::Ready);
  CHECK(management.status().power.value == 4);

  CHECK(management.request_dynamic_power(1));
  run_cycles(500);
  CHECK(transport.dynamic_power_option() == 1);
  CHECK(management.request_switch_mode(1));
  run_cycles(500);
  CHECK(transport.switch_mode_option() == 1);
  CHECK(management.request_telemetry_ratio(3));
  run_cycles(500);
  CHECK(transport.telemetry_ratio_option() == 3);

  const uint32_t binds_before =
      transport.stats().bind_commands_received;
  CHECK(management.request_bind());
  run_cycles(800);
  CHECK(transport.stats().bind_commands_received == binds_before + 1);
  CHECK(management.status().state == ElrsManagerState::Ready);

  CHECK(management.request_wifi_update());
  run_cycles(100);
  CHECK(transport.stats().wifi_commands_received == 1);
  CHECK(transport.wifi_update_mode());
  CHECK(management.status().state == ElrsManagerState::WifiUpdate);

  FakeToneOutput tones;
  ElrsFinder finder(tones);
  finder.set_active(true);
  telemetry.update(crsf::SensorUplinkRssi, -100, TelemetryUnit::Dbm,
                   now_us);
  finder.tick(telemetry, now_us);
  CHECK(finder.status().signal_fresh);
  CHECK(finder.status().strength_percent == 25);
  CHECK(tones.plays == 1);
  CHECK(tones.last_duration == 30);

  const uint16_t weak_period = finder.status().beep_period_ms;
  for (int sample = 0; sample < 10; ++sample) {
    now_us += 200000;
    telemetry.update(crsf::SensorUplinkRssi, -60, TelemetryUnit::Dbm,
                     now_us);
    finder.tick(telemetry, now_us);
  }
  CHECK(finder.status().filtered_rssi_dbm > -70);
  CHECK(finder.status().beep_period_ms < weak_period);
  CHECK(tones.last_frequency > 600);
  now_us += 1100000;
  finder.tick(telemetry, now_us);
  CHECK(!finder.status().signal_fresh);
  CHECK(tones.stops > 0);

  sim::LinkFaultPlan disconnected{};
  disconnected.disconnect_start_us = 0;
  disconnected.disconnect_end_us = 3000000;
  sim::VirtualElrsModule recovering_transport(disconnected);
  TelemetryRegistry recovering_telemetry;
  CrsfParser recovering_parser(recovering_telemetry);
  DiagnosticLog recovering_diagnostics;
  ModuleSupervisor recovering_module(
      recovering_transport, recovering_parser, recovering_diagnostics);
  ElrsDeviceManager recovering_management(
      recovering_transport, recovering_parser);
  recovering_module.start(1, 0);
  recovering_management.start(0);
  TimeUs recovering_now = 0;
  for (uint32_t cycle = 0; cycle < 600; ++cycle) {
    recovering_now += 4000;
    recovering_transport.advance(recovering_now);
    recovering_module.poll(recovering_now + 100);
    recovering_management.tick(recovering_now + 200);
  }
  CHECK(recovering_management.status().state ==
        ElrsManagerState::Unavailable);
  for (uint32_t cycle = 0; cycle < 800; ++cycle) {
    recovering_now += 4000;
    recovering_transport.advance(recovering_now);
    recovering_module.poll(recovering_now + 100);
    recovering_management.tick(recovering_now + 200);
  }
  CHECK(recovering_management.status().state == ElrsManagerState::Ready);
}

void test_storage()
{
  const std::string root =
      "/tmp/rivettx-tests-" + std::to_string(
          static_cast<unsigned long long>(
              reinterpret_cast<uintptr_t>(&failures)));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  PosixFileStore files(root);
  TransactionalModelStore store(files, "plane.rvm");
  Model model = make_default_model();
  model.model_id = 17;
  model.outputs[3].subtrim = 42;
  model.required_switch_mask = 3;
  std::string error;
  CHECK(store.save(model, 7, error));

  Model decoded{};
  const auto result = store.load(decoded);
  CHECK(result.success);
  CHECK(result.generation == 7);
  CHECK(decoded.model_id == 17);
  CHECK(decoded.outputs[3].subtrim == 42);

  model.model_id = 18;
  CHECK(store.save(model, 8, error));
  std::vector<uint8_t> corrupt{1, 2, 3, 4};
  CHECK(files.write("plane.rvm", corrupt));
  const auto recovered = store.load(decoded);
  CHECK(recovered.success);
  CHECK(recovered.recovered);
  CHECK(decoded.model_id == 17);

  std::array<AxisCalibration, kMaxAxes> calibration{};
  CalibrationStore calibration_store(files, "calibration.bin");
  CHECK(calibration_store.save(calibration));
  std::array<AxisCalibration, kMaxAxes> restored{};
  CHECK(calibration_store.load(restored));
  CHECK(restored[0].center == 2048);
  std::filesystem::remove_all(root);
}

void test_ui()
{
  DisplayCapabilities compact{};
  CHECK(ResponsiveLayout::metrics(compact).columns == 1);
  DisplayCapabilities large{480, 320, 16, true, true, 16};
  CHECK(ResponsiveLayout::metrics(large).columns == 2);
  FakeDisplay display(compact);
  MonoCanvas canvas(compact.width, compact.height);
  UiController ui(display, canvas);
  ui.set_screen(make_outputs_screen(ChannelFrame{}));
  CHECK(ui.render());
  CHECK(display.flushes == 1);
  CHECK(ui.handle({UiEventType::Down, 0, 0, 0}));
  CHECK(ui.selected_index() == 1);
  CHECK(ui.render());

  Model model = make_default_model();
  ui.set_screen(make_inputs_screen(model));
  CHECK(ui.handle({UiEventType::Enter, 0, 0, 0}));
  CHECK(ui.handle({UiEventType::Left, 0, 0, 0}));
  UiChange change{};
  CHECK(ui.take_change(change));
  CHECK(ModelEditor::apply(model, change));
  CHECK(model.inputs[0].weight_percent == 99);
  ui.set_screen(make_inputs_screen(model));
  CHECK(ui.editing());
  CHECK(ui.handle({UiEventType::Down, 0, 0, 0}));
  CHECK(ui.take_change(change));
  CHECK(change.value == 98);
  CHECK(ui.handle({UiEventType::Back, 0, 0, 0}));
  CHECK(!ui.editing());

  model.curve_count = 1;
  CHECK(ModelEditor::apply(
      model, {"curves", "curve.0.point.4", 123}));
  CHECK(model.curves[0].points[4] == 123);
  CHECK(!make_model_setup_screen(model).fields.empty());
  CHECK(!make_mixes_screen(model).fields.empty());
  CHECK(!make_output_limits_screen(model).fields.empty());
  CHECK(!make_flight_modes_screen(model).fields.empty());
  CHECK(!make_curves_screen(model).fields.empty());
  CHECK(!make_timers_screen(model, {}).fields.empty());
  CHECK(!make_elrs_screen(ElrsManagerStatus{}, true).fields.empty());
  CHECK(!make_elrs_finder_screen(ElrsFinderStatus{}).fields.empty());

  UiScreen actions{"actions", "Actions", {}};
  actions.fields.push_back(
      {"go", "GO", "PRESS", UiFieldKind::Action,
       0, 0, 1, false, true});
  ui.set_screen(std::move(actions));
  CHECK(ui.handle({UiEventType::Enter, 0, 0, 0}));
  CHECK(ui.take_change(change));
  CHECK(change.field_id == "go");

}

void test_services()
{
  DiagnosticLog diagnostics;
  for (int i = 0; i < 140; ++i) {
    diagnostics.push(
        {static_cast<TimeUs>(i), LogSeverity::Info, LogCode::Boot, i, 0});
  }
  CHECK(diagnostics.size() == kDiagnosticCapacity);
  LogEvent oldest{};
  CHECK(diagnostics.event_at(0, oldest));
  CHECK(oldest.argument0 == 12);
  const auto crash = make_crash_snapshot(3, 99, SafetyStatus{}, diagnostics);
  CHECK(crash.event_count == 32);
  CHECK(crash.recent_events[31].argument0 == 139);

  BatteryMonitor battery;
  CHECK(battery.update(3600) == BatteryState::Normal);
  for (int i = 0; i < 30; ++i) {
    (void)battery.update(3000);
  }
  CHECK(battery.state() == BatteryState::Critical);

  TelemetryRegistry telemetry;
  telemetry.update(crsf::SensorUplinkLinkQuality, 20,
                   TelemetryUnit::Percent, 1000);
  AlarmEngine alarms;
  alarms.set_alarm(
      0, {true, crsf::SensorUplinkLinkQuality, AlarmComparison::Below,
          30, 5, 10});
  AlarmEvent alarm{};
  CHECK(alarms.evaluate(telemetry, 1000, alarm));
  CHECK(alarm.active);

  MemoryTelemetrySink sink;
  TelemetryLogger logger(sink, 100);
  logger.start();
  logger.sample(telemetry, 1000);
  logger.sample(telemetry, 2000);
  logger.sample(telemetry, 102000);
  CHECK(sink.samples.size() == 2);
  logger.stop();
  CHECK(sink.flushes == 1);

  FakeScript script;
  ScriptSupervisor scripts(script, diagnostics,
                           {100, 1000, 1000, 2});
  script.next = {ScriptRunStatus::Yielded, 101, 10, 10};
  (void)scripts.tick(1000);
  (void)scripts.tick(2000);
  CHECK(!scripts.alive());
  CHECK(script.terminated);
}

void test_module_update_backup_and_calibration()
{
  DiagnosticLog diagnostics;
  TelemetryRegistry telemetry;
  CrsfParser parser(telemetry);
  FakeTransport transport;
  ModuleSupervisor module(transport, parser, diagnostics);
  module.start(4, 1000);
  CHECK(transport.baud == 400000);
  CHECK(!transport.writes.empty());
  CHECK(module.enter_passthrough(true));
  const uint8_t byte = 0x55;
  CHECK(module.passthrough_write(&byte, 1));
  module.leave_passthrough(4, 2000);
  CHECK(transport.resets == 1);

  Model model = make_default_model();
  ChannelFrame frame{};
  frame.channels[2] = -777;
  module.capture_failsafe(model, frame);
  CHECK(model.outputs[2].failsafe == -777);

  FakeOta ota;
  BootManager boot(ota, diagnostics);
  ota.pending = true;
  CHECK(boot.finish_startup({true, true, true, true, true}, 1000));
  CHECK(ota.marked);
  ota.marked = false;
  CHECK(!boot.finish_startup({true, false, true, true, true}, 2000));
  CHECK(ota.rollback);
  CHECK(boot.enter_recovery(false, 3));

  UpdateManager updates(ota, diagnostics, "esp32c3");
  FirmwareManifest invalid{"rivettx", "esp32c3", "1.0",
                           "http://invalid", 2, true};
  CHECK(!updates.install(invalid, true, 3000));
  FirmwareManifest valid{"rivettx", "esp32c3", "1.0",
                         "https://example.invalid/rivettx.bin", 2, true};
  CHECK(updates.install(valid, true, 4000));
  CHECK(ota.updated_url == valid.url);

  MemoryBackup endpoint;
  BackupService backups(endpoint);
  const std::vector<uint8_t> data{1, 2, 3};
  CHECK(!backups.export_file("model.rvm", data, false));
  CHECK(backups.export_file("model.rvm", data, true));
  std::vector<uint8_t> restored;
  CHECK(backups.import_file("model.rvm", restored, true));
  CHECK(restored == data);

  CalibrationWizard calibration;
  calibration.begin();
  RawInputs raw{};
  raw.valid = true;
  raw.axes.fill(2048);
  for (int i = 0; i < 10; ++i) {
    calibration.sample(raw);
  }
  CHECK(calibration.next());
  raw.axes.fill(100);
  calibration.sample(raw);
  raw.axes.fill(4000);
  calibration.sample(raw);
  CHECK(calibration.next());
  CHECK(calibration.next());
  CHECK(calibration.step() == CalibrationStep::Complete);

  SpecialFunctionEngine special_engine;
  FakeSpecialActions actions;
  ControlInputs inputs{};
  inputs.switches[0] = true;
  model.special_function_count = 1;
  model.special_functions[0].enabled = true;
  model.special_functions[0].condition.index = 0;
  model.special_functions[0].action = SpecialAction::Bind;
  model.special_functions[0].parameter = 9;
  std::array<bool, kMaxLogicalSwitches> logical{};
  special_engine.evaluate(model, inputs, logical, actions, 5000);
  special_engine.evaluate(model, inputs, logical, actions, 6000);
  CHECK(actions.calls == 1);
  CHECK(actions.last_action == SpecialAction::Bind);
  CHECK(actions.last_parameter == 9);

  PowerManager power({1, 1});
  power.note_activity(1);
  CHECK(power.evaluate(BatteryState::Normal, true, 1000) ==
        PowerDecision::StayOn);
  CHECK(power.evaluate(BatteryState::Normal, true, 61000001) ==
        PowerDecision::WarnInactivity);
  CHECK(power.evaluate(BatteryState::Critical, false, 62000000) ==
        PowerDecision::LockAndShutdown);
}

}  // namespace

int main()
{
  test_inputs_and_mixer();
  test_mixer_features();
  test_safety();
  test_crsf();
  test_virtual_elrs_module();
  test_elrs_management_and_finder();
  test_storage();
  test_ui();
  test_services();
  test_module_update_backup_and_calibration();

  if (failures != 0) {
    std::cerr << failures << " of " << checks << " checks failed\n";
    return 1;
  }
  std::cout << "all " << checks << " checks passed\n";
  return 0;
}
