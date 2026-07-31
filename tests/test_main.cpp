#include "rivettx/audio.hpp"
#include "rivettx/at7456e.hpp"
#include "rivettx/board_power.hpp"
#include "rivettx/core.hpp"
#include "rivettx/crsf.hpp"
#include "rivettx/elrs.hpp"
#include "rivettx/product.hpp"
#include "rivettx/removable_storage.hpp"
#include "rivettx/services.hpp"
#include "rivettx/speaker.hpp"
#include "rivettx/storage.hpp"
#include "rivettx/amt630a.hpp"
#include "rivettx/ui.hpp"
#include "virtual_hardware.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
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

class FakeRemovableStorageBackend final : public IRemovableStorageBackend {
 public:
  bool set_power(bool enabled) override
  {
    powered = enabled;
    ++power_changes;
    return power_ok;
  }
  RemovableStorageMediaResult identify(uint32_t initial_clock_khz,
                                        uint32_t maximum_clock_khz) override
  {
    initial_clock = initial_clock_khz;
    maximum_clock = maximum_clock_khz;
    ++identify_calls;
    return identify_result;
  }
  RemovableStorageMediaResult mount(bool read_only) override
  {
    ++mount_calls;
    last_mount_read_only = read_only;
    return mount_result;
  }
  RemovableStorageMediaResult validate_fat32() override
  {
    ++validate_calls;
    return validate_result;
  }
  bool ensure_openpocket_directories() override
  {
    ++directory_calls;
    return directories_ok;
  }
  bool unmount() override
  {
    ++unmount_calls;
    return unmount_ok;
  }
  bool execute(const RemovableStorageRequest& request,
               RemovableStorageCompletion& completion) override
  {
    last_request = request;
    ++execute_calls;
    completion.token = request.token;
    completion.bytes = execute_ok ? 3 : 0;
    completion.data[0] = 'O';
    completion.data[1] = 'K';
    completion.data[2] = '!';
    return execute_ok;
  }

  bool powered = false;
  bool power_ok = true;
  bool directories_ok = true;
  bool unmount_ok = true;
  bool execute_ok = true;
  bool last_mount_read_only = false;
  uint32_t initial_clock = 0;
  uint32_t maximum_clock = 0;
  uint32_t power_changes = 0;
  uint32_t identify_calls = 0;
  uint32_t mount_calls = 0;
  uint32_t validate_calls = 0;
  uint32_t directory_calls = 0;
  uint32_t unmount_calls = 0;
  uint32_t execute_calls = 0;
  RemovableStorageMediaResult identify_result =
      RemovableStorageMediaResult::Ok;
  RemovableStorageMediaResult mount_result =
      RemovableStorageMediaResult::Ok;
  RemovableStorageMediaResult validate_result =
      RemovableStorageMediaResult::Ok;
  RemovableStorageRequest last_request{};
};

class FakeBoardPowerHardware final : public IBoardPowerHardware {
 public:
  bool initialize() override { initialized = true; return calls_ok; }
  bool set_video_5v(bool value) override { video = value; return calls_ok; }
  bool set_display_5v(bool value) override { display = value; return calls_ok; }
  bool set_elrs_5v(bool value) override { elrs = value; return calls_ok; }
  bool set_backlight(uint8_t value) override { backlight = value; return calls_ok; }
  bool set_display_controller_reset(bool value) override { reset = value; return calls_ok; }
  bool read_vbus_present(bool& value) override { value = vbus; return calls_ok; }
  ChargerTelemetry read_charger() override { return charger; }
  FuelGaugeTelemetry read_fuel_gauge() override { return gauge; }

  bool initialized = false;
  bool calls_ok = true;
  bool video = false;
  bool display = false;
  bool elrs = false;
  bool reset = false;
  bool vbus = true;
  uint8_t backlight = 0;
  ChargerTelemetry charger{BoardSensorState::Valid, ChargeState::Charging,
                           3880, true, true, false};
  FuelGaugeTelemetry gauge{BoardSensorState::Valid, 3890, 67, false};
};

class FakeAmt630aHardware final : public IAmt630aHardware {
 public:
  bool initialize() override { initialized = true; return calls_ok; }
  bool read_identity(uint16_t& value) override
  {
    value = identity;
    return calls_ok;
  }
  bool acquire_flash(uint32_t& jedec) override
  {
    entered_isp = true;
    jedec = flash_jedec;
    return calls_ok;
  }
  bool flash_write_enable() override
  {
    if (!calls_ok) return false;
    program_busy_polls = busy_polls;
    return true;
  }
  bool flash_chip_erase() override
  {
    if (!calls_ok) return false;
    ++erase_count;
    std::fill(flash.begin(), flash.end(), 0xff);
    erase_busy_polls = busy_polls;
    return true;
  }
  bool flash_busy(bool& value) override
  {
    if (!calls_ok) return false;
    if (erase_busy_polls != 0) {
      --erase_busy_polls;
      value = true;
    } else if (program_busy_polls != 0) {
      --program_busy_polls;
      value = true;
    } else {
      value = false;
    }
    return true;
  }
  bool flash_program_page(uint32_t address, const uint8_t* data,
                          std::size_t size) override
  {
    if (!calls_ok || data == nullptr || address + size > flash.size()) {
      return false;
    }
    std::copy(data, data + size, flash.begin() + address);
    programmed_addresses.push_back(address);
    program_busy_polls = busy_polls;
    return true;
  }
  bool flash_read(uint32_t address, uint8_t* data,
                  std::size_t size) override
  {
    if (!calls_ok || readback_fails || data == nullptr ||
        address + size > flash.size()) {
      return false;
    }
    std::copy(flash.begin() + address, flash.begin() + address + size, data);
    return true;
  }
  bool release_flash_and_reset() override
  {
    if (!calls_ok) return false;
    entered_isp = false;
    runtime.booted = true;
    runtime.panel_timing_active = true;
    return true;
  }
  bool read_runtime_status(Amt630aRuntimeStatus& value) override
  {
    value = runtime;
    return calls_ok;
  }

  bool initialized = false;
  bool calls_ok = true;
  bool entered_isp = false;
  bool readback_fails = false;
  uint16_t identity = kAmt630aExpectedIdentity;
  uint32_t flash_jedec = kAmt630aExpectedFlashJedec;
  uint8_t busy_polls = 1;
  uint8_t erase_busy_polls = 0;
  uint8_t program_busy_polls = 0;
  uint32_t erase_count = 0;
  Amt630aRuntimeStatus runtime{true, true, true, 1, 0,
                               Amt630aVideoStandard::Pal};
  std::array<uint8_t, kAmt630aFlashSize> flash{};
  std::vector<uint32_t> programmed_addresses{};
};

class FakeVrxHardware final : public IVrxHardware {
 public:
  bool start_tune(uint16_t frequency_mhz, TimeUs) override
  {
    tuned = frequency_mhz;
    ++tunes;
    state = available ? VrxTuneState::Complete : VrxTuneState::Failed;
    return available;
  }

  void tick(TimeUs) override {}

  VrxTuneState tune_state() const override
  {
    return state;
  }

  void cancel_tune() override
  {
    state = VrxTuneState::Idle;
  }

  VrxAdcSample sample_rssi() override
  {
    if (rssi_state != VrxAdcSampleState::Valid) {
      return {rssi_state, 0};
    }
    if (!available) {
      return {VrxAdcSampleState::SensorFault, 0};
    }
    const int16_t value = fixed_rssi >= 0
                              ? fixed_rssi
                              : static_cast<int16_t>(
                                    tuned == strongest_frequency
                                        ? strongest_rssi
                                        : default_rssi);
    return {VrxAdcSampleState::Valid, value};
  }

  uint16_t tuned = 0;
  uint32_t tunes = 0;
  bool available = true;
  VrxTuneState state = VrxTuneState::Idle;
  VrxAdcSampleState rssi_state = VrxAdcSampleState::Valid;
  uint16_t strongest_frequency = 5800;
  int16_t strongest_rssi = 3000;
  int16_t default_rssi = 500;
  int16_t fixed_rssi = -1;
};

class FakeRtc6715Io final : public IRtc6715Io {
 public:
  bool initialize() override
  {
    return initialize_ok;
  }

  bool set_data(bool high) override
  {
    if (fail_writes) return false;
    data = high;
    return true;
  }

  bool set_clock(bool high) override
  {
    if (fail_writes) return false;
    if (!clock && high && !latch && captured_bits < 25) {
      if (data) captured_frame |= 1U << captured_bits;
      ++captured_bits;
    }
    clock = high;
    return true;
  }

  bool set_latch(bool high) override
  {
    if (fail_writes) return false;
    if (!latch && high && captured_bits == 25) {
      frames.push_back(captured_frame);
      captured_bits = 0;
      captured_frame = 0;
    } else if (latch && !high) {
      captured_bits = 0;
      captured_frame = 0;
    }
    latch = high;
    return true;
  }

  bool read_rssi(int& raw_adc) override
  {
    if (fail_adc) return false;
    raw_adc = rssi;
    return true;
  }

  bool initialize_ok = true;
  bool fail_writes = false;
  bool fail_adc = false;
  bool data = false;
  bool clock = false;
  bool latch = true;
  uint8_t captured_bits = 0;
  uint32_t captured_frame = 0;
  int rssi = 1000;
  std::vector<uint32_t> frames;
};

class FakeAt7456eSpi final : public IAt7456eSpi {
 public:
  bool queue(const uint8_t* transmit, uint8_t* receive,
             std::size_t size) override
  {
    if (queued || fail_next_queue) {
      fail_next_queue = false;
      return false;
    }
    last.assign(transmit, transmit + size);
    transactions.push_back(last);
    receive_ = receive;
    receive_size_ = size;
    queued = true;
    busy_polls_remaining = busy_polls;
    return true;
  }

  At7456eTransferState poll() override
  {
    if (!queued) {
      return At7456eTransferState::Failed;
    }
    if (busy_polls_remaining != 0) {
      --busy_polls_remaining;
      return At7456eTransferState::Busy;
    }
    queued = false;
    if (fail_next_poll) {
      fail_next_poll = false;
      return At7456eTransferState::Failed;
    }
    if (last.size() == 2 && last[0] == 0xA0 && receive_size_ >= 2) {
      receive_[1] = status_register;
    } else if (last.size() == 2 && last[0] == 0x80 &&
               receive_size_ >= 2) {
      receive_[1] = vm0;
    } else {
      apply(last);
    }
    return At7456eTransferState::Complete;
  }

  void abort() override
  {
    if (queued) {
      queued = false;
      ++aborts;
    }
  }

  std::size_t display_write_transactions() const
  {
    return static_cast<std::size_t>(std::count_if(
        transactions.begin(), transactions.end(),
        [](const std::vector<uint8_t>& transaction) {
          return transaction.size() == 2 && transaction[0] == 0x04 &&
                 (transaction[1] & 0x01U) != 0;
        }));
  }

  void apply(const std::vector<uint8_t>& transaction)
  {
    if (transaction.size() == 1) {
      const uint8_t value = transaction[0];
      if (auto_increment && value == 0xFF) {
        auto_increment = false;
      } else if (auto_increment) {
        if (display_address < display.size()) {
          display[display_address] = value;
          display_touched[display_address] = true;
        }
        ++display_address;
      }
      return;
    }
    for (std::size_t offset = 0; offset + 1 < transaction.size();
         offset += 2) {
      const uint8_t address = transaction[offset];
      const uint8_t value = transaction[offset + 1];
      if (address == 0x00) {
        vm0 = value;
      } else if (address == 0x05) {
        display_address = static_cast<uint16_t>(
            (display_address & 0xFFU) | ((value & 1U) << 8));
      } else if (address == 0x06) {
        display_address = static_cast<uint16_t>(
            (display_address & 0x100U) | value);
      } else if (address == 0x04) {
        auto_increment = (value & 0x01U) != 0;
      } else if (address == 0x09) {
        glyph_address = value;
      } else if (address == 0x0A) {
        glyph_byte = value;
      } else if (address == 0x0B && glyph_byte < kAt7456eGlyphBytes) {
        glyph_memory[glyph_address][glyph_byte] = value;
      } else if (address == 0x08 && (value & 0xF0U) == 0xA0U) {
        ++glyph_commits;
      }
    }
  }

  uint8_t status_register = 0x02;
  uint8_t vm0 = 0;
  std::array<uint8_t, kOsdColumns * kOsdRows> display{};
  std::array<bool, kOsdColumns * kOsdRows> display_touched{};
  std::array<std::array<uint8_t, kAt7456eGlyphBytes>, 256> glyph_memory{};
  std::vector<std::vector<uint8_t>> transactions;
  std::vector<uint8_t> last;
  uint8_t* receive_ = nullptr;
  std::size_t receive_size_ = 0;
  uint16_t display_address = 0;
  uint8_t glyph_address = 0;
  uint8_t glyph_byte = 0;
  uint32_t glyph_commits = 0;
  uint32_t busy_polls = 0;
  uint32_t busy_polls_remaining = 0;
  uint32_t aborts = 0;
  bool queued = false;
  bool auto_increment = false;
  bool fail_next_queue = false;
  bool fail_next_poll = false;
};

class FakeOpenPocketScreens final : public IOpenPocketScreenProvider {
 public:
  UiScreen screen(OpenPocketPage page) override
  {
    ++requests;
    last_page = page;
    switch (page) {
      case OpenPocketPage::Home:
        return make_openpocket_home_screen(model, home);
      case OpenPocketPage::Warnings:
        return make_warnings_screen(home);
      case OpenPocketPage::Models:
        return {"models", "Models",
                {{"select.0", "DEFAULT", "ACTIVE", UiFieldKind::Action,
                  0, 0, 31, false, true}}};
      case OpenPocketPage::Outputs:
        return make_outputs_screen(channels);
      case OpenPocketPage::ModelSetup:
        return make_model_setup_screen(model);
      case OpenPocketPage::Inputs:
        return make_inputs_screen(model);
      case OpenPocketPage::Mixes:
        return make_mixes_screen(model);
      case OpenPocketPage::Limits:
        return make_output_limits_screen(model);
      case OpenPocketPage::FlightModes:
        return make_flight_modes_screen(model);
      case OpenPocketPage::Curves:
        return make_curves_screen(model);
      case OpenPocketPage::LogicalSwitches:
        return make_logical_switches_screen(model);
      case OpenPocketPage::SpecialFunctions:
        return make_special_functions_screen(model);
      case OpenPocketPage::Timers:
        return make_timers_screen(model, timers);
      case OpenPocketPage::Elrs:
        return make_elrs_screen(elrs, true);
      case OpenPocketPage::Finder:
        return make_elrs_finder_screen(finder);
      case OpenPocketPage::Video:
        return make_openpocket_video_screen(vrx);
      case OpenPocketPage::Usb:
        return {"usb", "USB Simulator",
                {{"enable", "START SIMULATOR", "ENTER",
                  UiFieldKind::Action, 0, 0, 1, false, true}}};
      case OpenPocketPage::Web:
        return {"web", "Web Config",
                {{"start", "START WEB CONFIG", "ENTER",
                  UiFieldKind::Action, 0, 0, 1, false, true}}};
      case OpenPocketPage::Telemetry:
        return make_telemetry_screen(
            {{"lq", "UPLINK LQ", "92%", UiFieldKind::Label,
              92, 0, 100, false, true}});
      case OpenPocketPage::Power:
        return {"power", "Power",
                {{"state", "STATE", "OK", UiFieldKind::Label,
                  0, 0, 0, false, true}}};
      case OpenPocketPage::System:
        return make_system_screen(3900, 65536, 0, "test");
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

  Model model = make_default_model();
  UiHomeStatus home{};
  ChannelFrame channels{};
  std::array<TimerState, kMaxTimers> timers{};
  ElrsManagerStatus elrs{};
  ElrsFinderStatus finder{};
  VrxStatus vrx{};
  OpenPocketPage last_page = OpenPocketPage::Home;
  uint32_t requests = 0;
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

class FakePcmOutput final : public IPcmOutput {
 public:
  bool initialize(uint32_t sample_rate_hz) override
  {
    sample_rate = sample_rate_hz;
    return available;
  }
  bool set_amplifier_enabled(bool value) override
  {
    enabled = value;
    return available;
  }
  bool write_nonblocking(const int16_t* samples, std::size_t count,
                         std::size_t& written) override
  {
    if (!available || samples == nullptr) return false;
    written = accept_samples ? count : 0;
    submitted += written;
    return true;
  }
  bool available = true;
  bool enabled = false;
  bool accept_samples = true;
  uint32_t sample_rate = 0;
  std::size_t submitted = 0;
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

class FakeManifestVerifier final : public IFirmwareManifestVerifier {
 public:
  bool verify(const FirmwareManifest& manifest) const override
  {
    ++calls;
    last_signature_size = manifest.signature.size();
    return accept && !manifest.signature.empty();
  }

  bool accept = true;
  mutable uint32_t calls = 0;
  mutable std::size_t last_signature_size = 0;
};

class MemoryTelemetrySink final : public ITelemetryLogSink {
 public:
  bool append(TimeUs time_us, uint16_t sensor_id, int32_t value) override
  {
    if (fail_append) {
      return false;
    }
    samples.push_back({time_us, sensor_id, value});
    return true;
  }
  bool flush() override
  {
    ++flushes;
    return !fail_flush;
  }
  struct Sample {
    TimeUs time_us;
    uint16_t sensor_id;
    int32_t value;
  };
  std::vector<Sample> samples;
  uint32_t flushes = 0;
  bool fail_append = false;
  bool fail_flush = false;
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
  CHECK(frame.channels[4] == -kResolution);
  CHECK(frame.channels[8] == 0);
  CHECK(frame.channels[11] == 0);

  raw.switches[kFirstAuxSwitch] = true;
  raw.sampled_at_us = 5000;
  (void)processor.process(raw);
  raw.sampled_at_us = 26000;
  const auto switched = processor.process(raw);
  const auto armed = mixer.evaluate(model, switched, telemetry, 26000);
  CHECK(armed.channels[4] == kResolution);

  ControlInputs extended{};
  extended.valid = true;
  extended.axes[4] = kResolution;
  extended.switch_positions.fill(-1);
  MixerEngine extended_mixer;
  const auto extended_frame =
      extended_mixer.evaluate(model, extended, telemetry, 30000);
  CHECK(extended_frame.channels[8] == kResolution);

  InputProcessor three_position_processor;
  RawInputs three_position{};
  three_position.valid = true;
  three_position.switch_positions_valid = true;
  three_position.switch_positions.fill(-1);
  three_position.switch_positions[5] = 0;
  three_position.sampled_at_us = 1000;
  auto three_controls = three_position_processor.process(three_position);
  MixerEngine three_position_mixer;
  CHECK(three_position_mixer
            .evaluate(model, three_controls, telemetry, 1000)
            .channels[5] == 0);
  three_position.switch_positions[5] = -1;
  three_position.sampled_at_us = 5000;
  (void)three_position_processor.process(three_position);
  three_position.sampled_at_us = 26000;
  three_controls = three_position_processor.process(three_position);
  CHECK(three_position_mixer
            .evaluate(model, three_controls, telemetry, 26000)
            .channels[5] == -kResolution);
  three_position.switch_positions[5] = 1;
  three_position.sampled_at_us = 30000;
  (void)three_position_processor.process(three_position);
  three_position.sampled_at_us = 51000;
  three_controls = three_position_processor.process(three_position);
  CHECK(three_position_mixer
            .evaluate(model, three_controls, telemetry, 51000)
            .channels[5] == kResolution);
}

void test_trim_controls()
{
  Model model = make_default_model();
  ControlInputs inputs{};
  inputs.valid = true;
  TrimController trims;
  CHECK(!trims.update(model, 0, inputs, 1000).changed());

  inputs.switches[kFirstTrimSwitch] = true;
  auto first = trims.update(model, 0, inputs, 2000);
  CHECK(first.changed());
  CHECK(first.changed_mask == 1);
  CHECK(model.flight_modes[0].trims[0] == -8);

  auto repeated = trims.update(model, 0, inputs, 502000);
  CHECK(repeated.changed());
  CHECK(model.flight_modes[0].trims[0] == -16);

  inputs.switches[kFirstTrimSwitch] = false;
  (void)trims.update(model, 0, inputs, 510000);
  inputs.switches[kFirstTrimSwitch] = true;
  inputs.switches[kFirstTrimSwitch + 1] = true;
  auto centered = trims.update(model, 0, inputs, 520000);
  CHECK(centered.changed());
  CHECK(centered.centered_mask == 1);
  CHECK(model.flight_modes[0].trims[0] == 0);

  model.flight_mode_count = 2;
  model.flight_modes[1].enabled = true;
  inputs.switches[kFirstTrimSwitch] = false;
  inputs.switches[kFirstTrimSwitch + 1] = false;
  (void)trims.update(model, 1, inputs, 530000);
  inputs.switches[kFirstTrimSwitch + 3] = true;
  auto flight_mode_trim = trims.update(model, 1, inputs, 540000);
  CHECK(flight_mode_trim.changed_mask == 2);
  CHECK(model.flight_modes[0].trims[1] == 0);
  CHECK(model.flight_modes[1].trims[1] == 8);

  model.flight_modes[1].trims[1] = 510;
  inputs.switches[kFirstTrimSwitch + 3] = false;
  (void)trims.update(model, 1, inputs, 550000);
  inputs.switches[kFirstTrimSwitch + 3] = true;
  auto limited = trims.update(model, 1, inputs, 560000);
  CHECK(limited.limit_mask == 2);
  CHECK(model.flight_modes[1].trims[1] == 512);

  trims.reset();
  CHECK(!trims.update(model, 0, inputs, 600000).changed());
}

void test_rotary_encoder()
{
  RotaryEncoderDecoder encoder;
  CHECK(encoder.update(false, false) == 0);
  CHECK(encoder.update(true, false) == 0);
  CHECK(encoder.update(true, true) == 0);
  CHECK(encoder.update(false, true) == 0);
  CHECK(encoder.update(false, false) == 1);

  encoder.reset();
  CHECK(encoder.update(false, false) == 0);
  CHECK(encoder.update(false, true) == 0);
  CHECK(encoder.update(true, true) == 0);
  CHECK(encoder.update(true, false) == 0);
  CHECK(encoder.update(false, false) == -1);

  encoder.reset();
  CHECK(encoder.update(false, false) == 0);
  CHECK(encoder.update(true, true) == 0);
  CHECK(encoder.update(false, false) == 0);
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

  Model position_model = make_default_model();
  position_model.input_count = 0;
  position_model.mix_count = 1;
  position_model.mixes[0].enabled = true;
  position_model.mixes[0].destination = 0;
  position_model.mixes[0].source = {
      SourceKind::Constant, 0, kResolution};
  position_model.mixes[0].condition = {
      5, false, SwitchPosition::Middle};
  ControlInputs positioned = inputs;
  positioned.switch_positions[5] = -1;
  MixerEngine position_mixer;
  CHECK(position_mixer.evaluate(
            position_model, positioned, telemetry, 150000)
            .channels[0] == 0);
  positioned.switch_positions[5] = 0;
  CHECK(position_mixer.evaluate(
            position_model, positioned, telemetry, 154000)
            .channels[0] == kResolution);
  positioned.switch_positions[5] = 1;
  CHECK(position_mixer.evaluate(
            position_model, positioned, telemetry, 158000)
            .channels[0] == 0);

  model.logical_switch_count = 1;
  model.logical_switches[0].operation = LogicalSwitchOp::Greater;
  model.logical_switches[0].lhs = {SourceKind::Axis, 0, 0};
  model.logical_switches[0].rhs = {SourceKind::Constant, 0, 0};
  model.logical_switches[0].threshold = 100;
  (void)mixer.evaluate(model, inputs, telemetry, 201000);
  CHECK(mixer.logical_switch_values()[0]);

  Model gvar_model = make_default_model();
  gvar_model.input_count = 0;
  gvar_model.mix_count = 1;
  gvar_model.gvars[0] = 500;
  gvar_model.flight_modes[0].gvars[0] = 0;
  gvar_model.flight_modes[0].gvar_override_mask = 1;
  gvar_model.mixes[0].source = {SourceKind::GVar, 0, 0};
  MixerEngine gvar_mixer;
  CHECK(gvar_mixer.evaluate(gvar_model, inputs, telemetry, 300000)
            .channels[0] == 0);

  Model trim_model = make_default_model();
  trim_model.flight_modes[0].trims[0] = 200;
  trim_model.mixes[0].carry_trim = false;
  ControlInputs centered = inputs;
  centered.axes[0] = 0;
  MixerEngine trim_mixer;
  CHECK(trim_mixer.evaluate(trim_model, centered, telemetry, 400000)
            .channels[0] == 0);

  Model timer_model = make_default_model();
  timer_model.timers[0].mode = TimerMode::Absolute;
  timer_model.timers[0].start_seconds = 10;
  timer_model.timers[0].countdown = true;
  MixerEngine timer_mixer;
  (void)timer_mixer.evaluate(timer_model, inputs, telemetry, 1000000);
  (void)timer_mixer.evaluate(timer_model, inputs, telemetry, 2000000);
  CHECK(timer_mixer.timer_states()[0].elapsed_ms == 9000);
  timer_model.timers[0].persistent = true;
  (void)timer_mixer.evaluate(timer_model, inputs, telemetry, 2500000);
  CHECK(timer_mixer.timer_states()[0].elapsed_ms == 8500);
  timer_mixer.reset();
  (void)timer_mixer.evaluate(timer_model, inputs, telemetry, 3000000);
  CHECK(timer_mixer.timer_states()[0].elapsed_ms == 8500);
  Model other_timer_model = timer_model;
  other_timer_model.timers[0].start_seconds = 30;
  timer_mixer.reset_for_model_change();
  (void)timer_mixer.evaluate(
      other_timer_model, inputs, telemetry, 4000000);
  CHECK(timer_mixer.timer_states()[0].elapsed_ms == 30000);
  timer_mixer.reset_timer(0);
  CHECK(timer_mixer.timer_states()[0].elapsed_ms == 30000);

  Model logical_timer_model = make_default_model();
  logical_timer_model.logical_switch_count = 1;
  logical_timer_model.logical_switches[0].operation =
      LogicalSwitchOp::Timer;
  logical_timer_model.logical_switches[0].delay_ms = 100;
  logical_timer_model.logical_switches[0].duration_ms = 100;
  MixerEngine logical_timer_mixer;
  (void)logical_timer_mixer.evaluate(
      logical_timer_model, inputs, telemetry, 150000);
  CHECK(logical_timer_mixer.logical_switch_values()[0]);
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
  safety.report_module_ready(true);
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

  raw.sampled_at_us = 9000;
  auto deadline = loop.run(model, raw, 3800, 9000, 11000);
  CHECK(deadline.frame.safe);
  CHECK(deadline.safety.state == SafetyState::Fault);
  CHECK(deadline.safety.reason == SafetyReason::MixerDeadline);
  CHECK(deadline.safety.missed_deadlines == 1);

  raw.sampled_at_us = 13000;
  auto recovering = loop.run(model, raw, 3800, 13000, 13100);
  CHECK(recovering.frame.safe);
  CHECK(recovering.safety.state == SafetyState::Fault);

  raw.sampled_at_us = 17000;
  auto recovered = loop.run(model, raw, 3800, 17000, 17100);
  CHECK(recovered.frame.safe);
  CHECK(recovered.safety.state == SafetyState::Ready);
  safety.request_enable();
  raw.sampled_at_us = 21000;
  auto reenabled = loop.run(model, raw, 3800, 21000, 21100);
  CHECK(!reenabled.frame.safe);
  CHECK(reenabled.safety.state == SafetyState::Enabled);

  raw.switches[kFirstAuxSwitch] = true;
  raw.sampled_at_us = 25000;
  auto arm_pending = loop.run(model, raw, 3800, 25000, 25100);
  CHECK(!arm_pending.frame.safe);
  CHECK(arm_pending.frame.channels[4] == -kResolution);
  raw.sampled_at_us = 49000;
  auto arm_high = loop.run(model, raw, 3800, 49000, 49100);
  CHECK(!arm_high.frame.safe);
  CHECK(arm_high.frame.channels[4] == kResolution);
  CHECK(arm_high.safety.state == SafetyState::Enabled);

  model.outputs[4].failsafe = kResolution;
  model.outputs[model.throttle_channel].failsafe = kResolution;
  safety.request_lock();
  safety.request_enable();
  raw.sampled_at_us = 53000;
  auto unsafe_reenable = loop.run(model, raw, 3800, 53000, 53100);
  CHECK(unsafe_reenable.frame.safe);
  CHECK(unsafe_reenable.frame.channels[4] == -kResolution);
  CHECK(unsafe_reenable.frame.channels[model.throttle_channel] ==
        -kResolution);
  CHECK(unsafe_reenable.safety.reason == SafetyReason::SwitchMismatch);

  raw.switches[kFirstAuxSwitch] = false;
  safety.request_enable();
  raw.sampled_at_us = 57000;
  auto debounce_pending = loop.run(model, raw, 3800, 57000, 57100);
  CHECK(debounce_pending.frame.safe);
  CHECK(debounce_pending.safety.reason == SafetyReason::SwitchMismatch);
  raw.sampled_at_us = 81000;
  auto reenable_pending = loop.run(model, raw, 3800, 81000, 81100);
  CHECK(reenable_pending.frame.safe);
  raw.sampled_at_us = 85000;
  auto safe_reenable = loop.run(model, raw, 3800, 85000, 85100);
  CHECK(!safe_reenable.frame.safe);
  CHECK(safe_reenable.safety.state == SafetyState::Enabled);

  safety.report_module_ready(false);
  safety.request_enable();
  raw.sampled_at_us = 89000;
  const auto module_locked =
      loop.run(model, raw, 3800, 89000, 89100);
  CHECK(module_locked.frame.safe);
  CHECK(module_locked.frame.channels[4] == -kResolution);
  CHECK(module_locked.safety.state == SafetyState::Locked);
  CHECK(module_locked.safety.reason == SafetyReason::ModuleOffline);
  safety.report_module_ready(true);
  safety.request_enable();
  raw.sampled_at_us = 93000;
  CHECK(loop.run(model, raw, 3800, 93000, 93100).frame.safe);
  raw.sampled_at_us = 97000;
  CHECK(!loop.run(model, raw, 3800, 97000, 97100).frame.safe);

  raw.sampled_at_us = 0;
  auto stale = loop.run(model, raw, 3800, 100000, 100100);
  CHECK(stale.frame.safe);
  CHECK(stale.safety.reason == SafetyReason::InputsStale);

  safety.report_battery(3000);
  CHECK(safety.status().state == SafetyState::Fault);
  CHECK(watchdog.count == 16);

  SafetyManager uncalibrated;
  uncalibrated.boot_complete(true, false, false);
  ControlInputs calibrated_controls{};
  calibrated_controls.valid = true;
  calibrated_controls.sampled_at_us = 1000;
  ChannelFrame proposed{};
  const auto calibration_locked =
      uncalibrated.gate(model, calibrated_controls, proposed, 1000);
  CHECK(calibration_locked.safe);
  CHECK(uncalibrated.status().reason ==
        SafetyReason::CalibrationRequired);

  SafetyManager missing_watchdog;
  missing_watchdog.boot_complete(true, false, true);
  missing_watchdog.report_watchdog_fault();
  missing_watchdog.request_enable();
  for (uint8_t cycle = 0; cycle < 25; ++cycle) {
    calibrated_controls.sampled_at_us =
        static_cast<TimeUs>(cycle + 1) * 1000;
    const auto watchdog_locked = missing_watchdog.gate(
        model, calibrated_controls, proposed,
        calibrated_controls.sampled_at_us);
    CHECK(watchdog_locked.safe);
  }
  CHECK(missing_watchdog.status().state == SafetyState::Fault);
  CHECK(missing_watchdog.status().reason ==
        SafetyReason::WatchdogUnavailable);
}

void test_crsf()
{
  FakeTransport physical_transport;
  CrsfTransmitGate transmit_gate(physical_transport);
  const std::array<uint8_t, 3> outbound{1, 2, 3};
  CHECK(transmit_gate.transmit_enabled());
  CHECK(transmit_gate.write(outbound.data(), outbound.size()));
  CHECK(physical_transport.writes.size() == 1);
  transmit_gate.set_transmit_enabled(false);
  CHECK(!transmit_gate.transmit_enabled());
  CHECK(!transmit_gate.write(outbound.data(), outbound.size()));
  CHECK(physical_transport.writes.size() == 1);
  physical_transport.receive = {4, 5};
  std::array<uint8_t, 4> inbound{};
  CHECK(transmit_gate.read(inbound.data(), inbound.size()) == 2);
  CHECK(inbound[0] == 4);
  CHECK(inbound[1] == 5);
  transmit_gate.set_baud_rate(420000);
  transmit_gate.reset_module();
  CHECK(physical_transport.baud == 420000);
  CHECK(physical_transport.resets == 1);
  transmit_gate.set_transmit_enabled(true);
  CHECK(transmit_gate.write(outbound.data(), outbound.size()));
  CHECK(physical_transport.writes.size() == 2);

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

  std::array<uint16_t, kChannelCount> unpacked{};
  uint32_t accumulator = 0;
  uint8_t available_bits = 0;
  std::size_t input = 3;
  for (auto& channel : unpacked) {
    while (available_bits < 11) {
      accumulator |= static_cast<uint32_t>(frame.bytes[input++])
                     << available_bits;
      available_bits = static_cast<uint8_t>(available_bits + 8);
    }
    channel = static_cast<uint16_t>(accumulator & 0x7FFU);
    accumulator >>= 11U;
    available_bits = static_cast<uint8_t>(available_bits - 11);
  }
  CHECK(unpacked[0] == 172);
  CHECK(unpacked[1] == 992);
  CHECK(unpacked[2] == 1811);

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
  const auto dropped_before = parser.stats().dropped_bytes;
  CHECK(!parser.feed(crsf::kAddressModule, 5250));
  CHECK(parser.stats().dropped_bytes == dropped_before + 1);
  crsf::Frame popped{};
  CHECK(!parser.pop_frame(popped));

  parser.set_lua_frame_queue_enabled(true);
  std::array<uint8_t, 4> device_info{
      crsf::kAddressRadio, 2, crsf::kFrameDeviceInfo, 0};
  device_info.back() =
      crsf::crc8_dvb_s2(device_info.data() + 2,
                        device_info.size() - 3);
  for (const auto byte : device_info) {
    (void)parser.feed(byte, 5500);
  }
  CHECK(parser.pop_frame(popped));
  CHECK(popped.bytes[2] == crsf::kFrameDeviceInfo);
  parser.set_lua_frame_queue_enabled(false);

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
  CHECK(discovered.fields_discovered == 11);
  CHECK(discovered.packet_rate.available);
  CHECK(discovered.packet_rate.value == 3);
  CHECK(discovered.power.available);
  CHECK(discovered.power.option_count == 7);
  CHECK(std::string(discovered.power.options[3].data()) == "100mW");
  CHECK(discovered.dynamic_power.available);
  CHECK(discovered.switch_mode.available);
  CHECK(discovered.telemetry_ratio.available);
  CHECK(discovered.model_match.available);
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
  CHECK(management.request_packet_rate(2));
  run_cycles(500);
  CHECK(transport.packet_rate_option() == 2);
  CHECK(management.status().packet_rate.value == 2);
  CHECK(management.request_packet_rate(4));
  run_cycles(10);
  CHECK(transport.packet_rate_option() == 2);
  CHECK(std::string(management.status().message.data()) ==
        "400K UART: max 250Hz");
  CHECK(management.request_model_match(1));
  run_cycles(500);
  CHECK(transport.model_match_option() == 1);
  CHECK(management.status().model_match.value == 1);

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

void test_audio_alerts()
{
  FakeToneOutput tones;
  AudioAlertScheduler audio(tones);
  audio.notify(AudioAlert::BatteryLow);
  audio.tick(1000);
  CHECK(audio.current_alert() == AudioAlert::BatteryLow);
  CHECK(tones.last_frequency == 520);

  const uint32_t stops_before_preemption = tones.stops;
  audio.notify(AudioAlert::LinkCritical);
  audio.tick(2000);
  CHECK(audio.current_alert() == AudioAlert::LinkCritical);
  CHECK(tones.last_frequency == 1250);
  CHECK(tones.stops > stops_before_preemption);

  TimeUs now_us = 2000;
  for (int step = 0; step < 20; ++step) {
    now_us += 100000;
    audio.tick(now_us);
  }
  CHECK(audio.current_alert() == AudioAlert::Count);

  CHECK(audio.play_tone(777, 30));
  audio.tick(now_us + 1000);
  CHECK(audio.current_alert() == AudioAlert::CustomTone);
  CHECK(tones.last_frequency == 777);
  audio.stop_tone();
  CHECK(audio.current_alert() == AudioAlert::Count);

  FakeToneOutput warning_tones;
  AudioAlertScheduler warning_audio(warning_tones);
  AudioWarningMonitor warnings;
  TelemetryRegistry telemetry;
  now_us = 1000000;
  telemetry.update(crsf::SensorUplinkLinkQuality, 25,
                   TelemetryUnit::Percent, now_us);
  warnings.tick(telemetry, BatteryState::Normal, ModuleState::Online,
                SafetyState::Enabled, now_us, warning_audio);
  warning_audio.tick(now_us);
  CHECK(warning_audio.current_alert() == AudioAlert::LinkCritical);

  telemetry.update(crsf::SensorUplinkLinkQuality, 85,
                   TelemetryUnit::Percent, now_us + 100000);
  warnings.tick(telemetry, BatteryState::Normal, ModuleState::Online,
                SafetyState::Enabled, now_us + 100000, warning_audio);
  for (int step = 0; step < 20; ++step) {
    now_us += 100000;
    warning_audio.tick(now_us);
  }
  CHECK(warning_audio.current_alert() == AudioAlert::LinkRecovered ||
        warning_audio.current_alert() == AudioAlert::OutputsEnabled ||
        warning_audio.current_alert() == AudioAlert::Count);

  warnings.tick(telemetry, BatteryState::Normal, ModuleState::Online,
                SafetyState::Enabled, now_us + 2000000, warning_audio);
  warning_audio.tick(now_us + 2000000);
  CHECK(warning_audio.current_alert() == AudioAlert::TelemetryLost);

  FakeToneOutput battery_tones;
  AudioAlertScheduler battery_audio(battery_tones);
  AudioWarningMonitor battery_warnings;
  battery_warnings.tick(
      TelemetryRegistry{}, BatteryState::Low, ModuleState::Starting,
      SafetyState::Locked, 1000000, battery_audio);
  battery_audio.tick(1000000);
  CHECK(battery_audio.current_alert() == AudioAlert::BatteryLow);
  battery_warnings.tick(
      TelemetryRegistry{}, BatteryState::Critical, ModuleState::Starting,
      SafetyState::Locked, 2000000, battery_audio);
  battery_audio.tick(2000000);
  CHECK(battery_audio.current_alert() == AudioAlert::BatteryCritical);

  FakeToneOutput recovery_tones;
  AudioAlertScheduler recovery_audio(recovery_tones);
  AudioWarningMonitor recovery_warnings;
  recovery_warnings.tick(
      TelemetryRegistry{}, BatteryState::Low, ModuleState::Starting,
      SafetyState::Locked, 1000000, recovery_audio);
  for (int step = 0; step < 10; ++step) {
    recovery_audio.tick(
        1000000 + static_cast<TimeUs>(step) * 100000);
  }
  recovery_warnings.tick(
      TelemetryRegistry{}, BatteryState::Normal, ModuleState::Starting,
      SafetyState::Locked, 3000000, recovery_audio);
  recovery_audio.tick(3000000);
  CHECK(recovery_audio.current_alert() == AudioAlert::BatteryRecovered);

  FakeToneOutput no_link_tones;
  AudioAlertScheduler no_link_audio(no_link_tones);
  AudioWarningMonitor no_link_warnings;
  no_link_warnings.tick(
      TelemetryRegistry{}, BatteryState::Normal, ModuleState::Starting,
      SafetyState::Enabled, 1000000, no_link_audio);
  no_link_audio.tick(1000000);
  CHECK(no_link_audio.current_alert() == AudioAlert::OutputsEnabled);
  no_link_warnings.tick(
      TelemetryRegistry{}, BatteryState::Normal, ModuleState::Starting,
      SafetyState::Enabled, 2600000, no_link_audio);
  no_link_audio.tick(2600000);
  CHECK(no_link_audio.current_alert() == AudioAlert::TelemetryLost);

  FakeToneOutput fault_tones;
  AudioAlertScheduler fault_audio(fault_tones);
  AudioWarningMonitor fault_warnings;
  fault_warnings.tick(
      TelemetryRegistry{}, BatteryState::Normal, ModuleState::Starting,
      SafetyState::Ready, 1000000, fault_audio);
  fault_warnings.tick(
      TelemetryRegistry{}, BatteryState::Normal, ModuleState::Starting,
      SafetyState::Fault, 2000000, fault_audio);
  fault_audio.tick(2000000);
  CHECK(fault_audio.current_alert() == AudioAlert::SafetyFault);

  FakeToneOutput module_tones;
  AudioAlertScheduler module_audio(module_tones);
  AudioWarningMonitor module_warnings;
  module_warnings.tick(
      TelemetryRegistry{}, BatteryState::Normal, ModuleState::Online,
      SafetyState::Locked, 1000000, module_audio);
  module_warnings.tick(
      TelemetryRegistry{}, BatteryState::Normal, ModuleState::Offline,
      SafetyState::Locked, 2000000, module_audio);
  module_audio.tick(2000000);
  CHECK(module_audio.current_alert() == AudioAlert::ModuleOffline);
  module_warnings.tick(
      TelemetryRegistry{}, BatteryState::Normal, ModuleState::Online,
      SafetyState::Locked, 3000000, module_audio);
  module_audio.tick(3000000);
  CHECK(module_audio.current_alert() == AudioAlert::ModuleOffline);
  for (int step = 0; step < 20; ++step) {
    module_audio.tick(3100000 + static_cast<TimeUs>(step) * 100000);
  }
  CHECK(module_audio.current_alert() == AudioAlert::ModuleRecovered ||
        module_audio.current_alert() == AudioAlert::Count);

  FakeToneOutput mute_tones;
  AudioAlertScheduler mute_audio(mute_tones);
  CHECK(mute_audio.configure(
      {AudioSettings::kVersion, true, true, true, true, true, false, 60}));
  mute_audio.notify(AudioAlert::MenuConfirmation);
  mute_audio.tick(1);
  CHECK(mute_audio.current_alert() == AudioAlert::Count);
  mute_audio.notify(AudioAlert::BatteryCritical);
  mute_audio.tick(2);
  CHECK(mute_audio.current_alert() == AudioAlert::BatteryCritical);
  CHECK(mute_audio.configure(
      {AudioSettings::kVersion, true, true, true, true, false, true, 60}));
  mute_audio.notify(AudioAlert::Failsafe);
  mute_audio.tick(3);
  CHECK(mute_audio.current_alert() == AudioAlert::Count);
}

void test_speaker_service()
{
  FakePcmOutput output;
  SpeakerService speaker(output);
  CHECK(speaker.initialize(false, {true, 50}));
  CHECK(output.enabled);
  CHECK(output.sample_rate == 16000);
  CHECK(speaker.play_test_tone(1000, 100));
  for (int index = 0; index < 20 && speaker.status().playing; ++index) {
    speaker.tick(static_cast<TimeUs>(index) * 4000U);
  }
  CHECK(!speaker.status().playing);
  CHECK(speaker.status().completed_tones == 1);
  CHECK(output.submitted == 1600);

  CHECK(speaker.play_test_tone(2730, 100));
  CHECK(!speaker.play_test_tone(400, 100));
  CHECK(speaker.status().rejected_tones == 1);
  speaker.stop();
  CHECK(speaker.configure({false, 80}));
  CHECK(!output.enabled);

  FakePcmOutput simulator_output;
  SpeakerService simulator(simulator_output);
  CHECK(simulator.initialize(true, {false, 50}));
  CHECK(!simulator.status().available);
  CHECK(!simulator.play_test_tone(1000, 100));
}

RemovableStorageRequest removable_request(RemovableStorageOperation operation,
                                           const char* path,
                                           uint32_t token = 0)
{
  RemovableStorageRequest request{};
  request.operation = operation;
  std::strncpy(request.path.data(), path, request.path.size() - 1);
  request.size = 32;
  request.token = token;
  return request;
}

void make_removable_ready(RemovableStorageService& service, TimeUs& now)
{
  service.set_card_detect(true, now);
  now += 80000;
  service.tick(now);  // Power enabled.
  now += 10000;
  service.tick(now);  // Power settled.
  service.tick(++now);  // Identified and mounted read-only.
  service.tick(++now);  // Enter validation.
  service.tick(++now);  // FAT32 accepted and remounted read/write.
}

void test_removable_storage()
{
  CHECK(valid_openpocket_path("/OPENPOCKET/LOGS/session.csv"));
  CHECK(valid_openpocket_path("/OPENPOCKET/AUDIO/tone.wav"));
  CHECK(!valid_openpocket_path("/models/active.rvm"));
  CHECK(!valid_openpocket_path("/OPENPOCKET/../models/active.rvm"));
  CHECK(!valid_openpocket_path("/OPENPOCKET//LOGS/x"));
  CHECK(!valid_openpocket_path("/OPENPOCKET/UPDATES/C:evil.bin"));

  RemovableUpdateApproval approval{};
  CHECK(!removable_update_approved(approval));
  approval.manifest_signature_or_hash_valid = true;
  approval.user_confirmed = true;
  approval.hardware_compatible = true;
  approval.version_allowed = true;
  approval.post_write_readback_required = true;
  CHECK(removable_update_approved(approval));
  approval.user_confirmed = false;
  CHECK(!removable_update_approved(approval));

  FakeRemovableStorageBackend backend;
  RemovableStorageService service(backend);
  CHECK(service.status().state == RemovableStorageState::Absent);
  CHECK(!service.enqueue(removable_request(
      RemovableStorageOperation::LogRecord,
      "/OPENPOCKET/LOGS/no-card.csv")));
  CHECK(service.status().dropped_logs == 1);

  TimeUs now = 0;
  make_removable_ready(service, now);
  CHECK(service.status().state == RemovableStorageState::Ready);
  CHECK(service.status().card_present);
  CHECK(service.status().powered);
  CHECK(service.status().mounted);
  CHECK(service.status().writable);
  CHECK(backend.initial_clock == 400);
  CHECK(backend.maximum_clock == 20000);
  CHECK(backend.mount_calls == 2);
  CHECK(!backend.last_mount_read_only);
  CHECK(backend.validate_calls == 1);
  CHECK(backend.directory_calls == 1);

  const auto audio = removable_request(
      RemovableStorageOperation::AudioRead,
      "/OPENPOCKET/AUDIO/alert.wav", 42);
  CHECK(service.enqueue(audio));
  service.tick(++now);
  CHECK(backend.execute_calls == 1);
  CHECK(backend.last_request.token == 42);
  CHECK(service.status().completed == 1);
  RemovableStorageCompletion completion{};
  CHECK(service.take_completion(completion));
  CHECK(completion.success);
  CHECK(completion.token == 42);
  CHECK(completion.bytes == 3);
  CHECK(completion.data[0] == 'O');

  for (std::size_t index = 0; index < kRemovableStorageQueueDepth; ++index) {
    CHECK(service.enqueue(removable_request(
        RemovableStorageOperation::LogRecord,
        "/OPENPOCKET/LOGS/telemetry.csv",
        static_cast<uint32_t>(index))));
  }
  CHECK(!service.enqueue(removable_request(
      RemovableStorageOperation::LogRecord,
      "/OPENPOCKET/LOGS/overflow.csv")));
  CHECK(service.status().dropped_logs == 2);

  service.set_card_detect(false, ++now);
  now += 79999;
  service.tick(now);
  CHECK(service.status().state == RemovableStorageState::Ready);
  service.tick(++now);
  CHECK(service.status().state == RemovableStorageState::Absent);
  CHECK(!backend.powered);
  CHECK(service.status().queued == 0);
  CHECK(service.status().removals == 1);

  FakeRemovableStorageBackend corrupt_backend;
  corrupt_backend.validate_result = RemovableStorageMediaResult::Corrupt;
  RemovableStorageService corrupt(corrupt_backend);
  now = 0;
  make_removable_ready(corrupt, now);
  CHECK(corrupt.status().state == RemovableStorageState::Corrupt);
  CHECK(!corrupt.status().writable);
  CHECK(!corrupt_backend.powered);
  CHECK(!corrupt.enqueue(removable_request(
      RemovableStorageOperation::ModelExport,
      "/OPENPOCKET/MODELS/export.rvm")));

  FakeRemovableStorageBackend unsupported_backend;
  unsupported_backend.identify_result =
      RemovableStorageMediaResult::Unsupported;
  RemovableStorageService unsupported(unsupported_backend);
  now = 0;
  unsupported.set_card_detect(true, now);
  now += 80000;
  unsupported.tick(now);
  now += 10000;
  unsupported.tick(now);
  unsupported.tick(++now);
  CHECK(unsupported.status().state == RemovableStorageState::Unsupported);

  FakeRemovableStorageBackend io_backend;
  io_backend.execute_ok = false;
  RemovableStorageService io(io_backend);
  now = 0;
  make_removable_ready(io, now);
  CHECK(io.enqueue(removable_request(
      RemovableStorageOperation::DiagnosticWrite,
      "/OPENPOCKET/CRASH/fault.bin")));
  io.tick(++now);
  CHECK(io.status().failed == 1);
  CHECK(io.status().state == RemovableStorageState::Ready);
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
  CHECK(decoded.input_count == 8);
  CHECK(decoded.mix_count == 12);
  CHECK(decoded.mixes[4].source.kind == SourceKind::Switch);
  CHECK(decoded.mixes[4].source.index == kFirstAuxSwitch);
  CHECK(decoded.vrx_band == 0);
  CHECK(decoded.vrx_channel == 0);
  CHECK(decoded.video_overlay_enabled);
  CHECK(decoded.simulator_rf_lock);

  model.model_id = 18;
  CHECK(store.save(model, 8, error));
  std::vector<uint8_t> corrupt{1, 2, 3, 4};
  CHECK(files.write("plane.rvm", corrupt));
  const auto recovered = store.load(decoded);
  CHECK(recovered.success);
  CHECK(recovered.recovered);
  CHECK(decoded.model_id == 17);

  Model invalid = model;
  invalid.input_count = static_cast<uint8_t>(kMaxInputs + 1);
  CHECK(ModelCodec::encode(invalid, 9).empty());
  error.clear();
  CHECK(!store.save(invalid, 9, error));
  CHECK(error == "invalid model shape");
  invalid = model;
  invalid.vrx_band = 6;
  CHECK(ModelCodec::encode(invalid, 9).empty());
  invalid = model;
  invalid.mixes[0].condition = {
      -1, false, SwitchPosition::Middle};
  CHECK(ModelCodec::encode(invalid, 9).empty());

  Model product_model = make_default_model();
  product_model.vrx_band = 3;
  product_model.vrx_channel = 6;
  product_model.video_overlay_enabled = false;
  product_model.simulator_rf_lock = false;
  product_model.mixes[0].condition = {
      static_cast<int8_t>(kFirstAuxSwitch + 1), false,
      SwitchPosition::Middle};
  const auto product_encoded = ModelCodec::encode(product_model, 10);
  Model product_decoded{};
  uint32_t product_generation = 0;
  error.clear();
  CHECK(ModelCodec::decode(product_encoded, product_decoded,
                           product_generation, error));
  CHECK(product_generation == 10);
  CHECK(product_decoded.vrx_band == 3);
  CHECK(product_decoded.vrx_channel == 6);
  CHECK(!product_decoded.video_overlay_enabled);
  CHECK(!product_decoded.simulator_rf_lock);
  CHECK(product_decoded.mixes[0].condition.position ==
        SwitchPosition::Middle);

  ModelLibrary library(files);
  Model active = make_default_model();
  uint32_t active_generation = 1;
  CHECK(library.bootstrap(active, active_generation, error));
  CHECK(library.active_slot() == 0);
  Model second = make_default_model();
  std::snprintf(second.name.data(), second.name.size(), "Second");
  second.model_id = 2;
  uint8_t second_slot = 0;
  CHECK(library.create(second, 1, second_slot, error));
  CHECK(second_slot == 1);
  const auto summaries = library.summaries();
  CHECK(summaries[0].present);
  CHECK(summaries[1].present);
  CHECK(std::strcmp(summaries[1].name.data(), "Second") == 0);
  CHECK(library.select(second_slot, active, active_generation, error));
  CHECK(library.active_slot() == second_slot);
  CHECK(active.model_id == 2);
  Model updated_first = make_default_model();
  std::snprintf(updated_first.name.data(), updated_first.name.size(),
                "Updated First");
  CHECK(library.save_slot(0, updated_first, 7, error));
  Model restored_first{};
  uint32_t restored_first_generation = 0;
  CHECK(library.select(0, restored_first, restored_first_generation,
                       error));
  CHECK(restored_first_generation == 7);
  CHECK(std::strcmp(restored_first.name.data(), "Updated First") == 0);
  CHECK(library.select(second_slot, active, active_generation, error));
  CHECK(!library.save_slot(kMaximumStoredModels, updated_first, 8,
                           error));
  CHECK(error == "model slot out of range");
  CHECK(!library.remove(second_slot, error));
  CHECK(library.remove(0, error));

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
  CHECK(ui.handle({UiEventType::Rotate, 2, 0, 0}));
  CHECK(ui.selected_index() == 3);
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
  const auto oled_menu = make_main_menu_screen();
  CHECK(oled_menu.title == "RivetTX");
  CHECK(!oled_menu.fields.empty());
  CHECK(std::none_of(
      oled_menu.fields.begin(), oled_menu.fields.end(),
      [](const UiField& field) { return field.id == "video"; }));

  UiHomeStatus home{};
  home.axes = {-1024, 1024, -512, 512};
  home.battery_mv = 3800;
  home.link_quality = 87;
  home.module_online = true;
  home.warning_count = 2;
  home.warnings[0] = UiWarningCode::ThrottleHigh;
  home.warnings[1] = UiWarningCode::BatteryLow;
  ui.set_screen(make_oled_home_screen(model, home));
  CHECK(ui.render());
  CHECK(canvas.pixel_at(0, 9));
  const auto warnings = make_warnings_screen(home);
  CHECK(!warnings.fields.empty());
  CHECK(warnings.fields[0].value_text == "LOWER THROTTLE");
  home.warnings[0] = UiWarningCode::SwitchPosition;
  CHECK(make_warnings_screen(home).fields[0].value_text ==
        "SET SWITCHES TO SAFE");
  home.warnings[0] = UiWarningCode::WatchdogUnavailable;
  CHECK(make_warnings_screen(home).fields[0].value_text ==
        "RESTART RADIO");
  home.warnings[0] = UiWarningCode::ModuleOffline;
  CHECK(make_warnings_screen(home).fields[0].value_text ==
        "CHECK ELRS POWER UART");

  home.warning_count = 0;
  ui.update_home(home);
  CHECK(ui.render());
  CHECK(canvas.pixel_at(2, 20));

  ui.set_screen(make_inputs_screen(model));
  CHECK(ui.handle({UiEventType::Enter, 0, 0, 0}));
  CHECK(ui.editing());
  CHECK(ui.render());
  const int16_t edit_x =
      static_cast<int16_t>(canvas.width() - canvas.text_width("EDIT") - 1);
  bool edit_indicator_visible = false;
  for (int16_t x = edit_x; x < static_cast<int16_t>(canvas.width()); ++x) {
    for (int16_t y = 0; y < 7; ++y) {
      edit_indicator_visible =
          edit_indicator_visible || !canvas.pixel_at(x, y);
    }
  }
  CHECK(edit_indicator_visible);

  const OutputLimit unchanged = model.outputs[0];
  CHECK(!ModelEditor::apply(
      model, {"outputs", "output.0.minimum", 2000}));
  CHECK(model.outputs[0].minimum == unchanged.minimum);
  CHECK(ModelEditor::apply(
      model, {"outputs", "output.0.minimum", 1000}));
  CHECK(!ModelEditor::apply(
      model, {"outputs", "output.0.maximum", 500}));
  CHECK(model.outputs[0].minimum == 1000);
  CHECK(model.outputs[0].maximum == unchanged.maximum);

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

  BatteryMonitor battery({3500, 3200, 100, 100});
  CHECK(battery.update(3600) == BatteryState::Normal);
  BatteryMonitor zero_voltage({3500, 3200, 100, 100});
  CHECK(zero_voltage.update(0) == BatteryState::Critical);

  CHECK(!(BatterySensorSample{0, false, false}.sensor_fault()));
  CHECK(!(BatterySensorSample{0, true, true}.sensor_fault()));
  CHECK((BatterySensorSample{0, true, false}.sensor_fault()));

  SafetyManager configured_zero;
  configured_zero.report_battery(3800, true);
  configured_zero.report_battery(0, true);
  CHECK(configured_zero.status().state == SafetyState::Fault);
  CHECK(configured_zero.status().reason ==
        SafetyReason::BatteryCritical);
  SafetyManager sensor_absent;
  sensor_absent.report_battery(0, false);
  CHECK(sensor_absent.status().state == SafetyState::Booting);
  InputProcessor no_battery_inputs;
  MixerEngine no_battery_mixer;
  SafetyManager no_battery_safety;
  no_battery_safety.report_module_ready(true);
  FakeWatchdog no_battery_watchdog;
  TelemetryRegistry no_battery_telemetry;
  ControlLoop no_battery_loop(
      no_battery_inputs, no_battery_mixer, no_battery_safety,
      no_battery_telemetry, no_battery_watchdog);
  no_battery_safety.boot_complete(true, false);
  RawInputs no_battery_raw{};
  no_battery_raw.valid = true;
  no_battery_raw.axes.fill(2048);
  no_battery_raw.axes[2] = 100;
  no_battery_raw.sampled_at_us = 1000;
  const auto no_battery_cycle = no_battery_loop.run(
      make_default_model(), no_battery_raw, 0, 1000, 1100, false);
  CHECK(no_battery_cycle.safety.reason !=
        SafetyReason::BatteryCritical);

  BatterySnapshotStore snapshot_store;
  std::atomic<bool> torn_snapshot{false};
  std::thread reader([&]() {
    BatterySnapshot snapshot{};
    do {
      snapshot = snapshot_store.read();
      std::this_thread::yield();
    } while (snapshot.sequence == 0);
    for (uint32_t read = 0; read < 50000; ++read) {
      snapshot = snapshot_store.read();
      const bool normal = (snapshot.sequence & 1U) == 0;
      const bool consistent =
          normal
              ? snapshot.millivolts == 3800 &&
                    snapshot.state == BatteryState::Normal &&
                    snapshot.sensor_valid
              : snapshot.millivolts == 0 &&
                    snapshot.state == BatteryState::Unknown &&
                    !snapshot.sensor_valid;
      if (!consistent) {
        torn_snapshot.store(true, std::memory_order_release);
      }
    }
  });
  for (uint32_t sequence = 1; sequence <= 50000; ++sequence) {
    const bool normal = (sequence & 1U) == 0;
    snapshot_store.publish(
        normal ? BatterySnapshot{3800, BatteryState::Normal, true,
                                 sequence}
               : BatterySnapshot{0, BatteryState::Unknown, false,
                                 sequence});
  }
  reader.join();
  CHECK(!torn_snapshot.load(std::memory_order_acquire));
  for (int i = 0; i < 30; ++i) {
    (void)battery.update(3000);
  }
  CHECK(battery.state() == BatteryState::Critical);
  CHECK(battery.update(3250) == BatteryState::Critical);
  CHECK(battery.update(3300) == BatteryState::Low);
  CHECK(battery.update(3600) == BatteryState::Normal);

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
  CHECK(alarms.evaluate(telemetry, 3000001, alarm));
  CHECK(!alarm.active);

  MemoryTelemetrySink sink;
  TelemetryLogger logger(sink, 100);
  logger.start();
  logger.sample(telemetry, 1000);
  logger.sample(telemetry, 2000);
  logger.sample(telemetry, 102000);
  CHECK(sink.samples.size() == 2);
  logger.stop();
  CHECK(sink.flushes == 1);
  sink.fail_append = true;
  logger.start();
  CHECK(!logger.sample(telemetry, 200000));
  CHECK(logger.failed());
  CHECK(!logger.active());
  CHECK(sink.flushes == 2);
  sink.fail_append = false;
  sink.fail_flush = true;
  logger.start();
  CHECK(!logger.stop());
  CHECK(logger.failed());

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

  const std::array<BootProductProfile, 2> product_profiles{{
      BootProductProfile::StandaloneOled,
      BootProductProfile::OpenPocketOsd,
  }};
  const std::array<ModuleBootCondition, 4> optional_module_cases{{
      ModuleBootCondition::Absent,
      ModuleBootCondition::Starting,
      ModuleBootCondition::Incompatible,
      ModuleBootCondition::Reconnecting,
  }};
  for (const auto profile : product_profiles) {
    const auto requirements = startup_requirements_for(profile);
    CHECK(requirements.storage);
    CHECK(requirements.inputs);
    CHECK(requirements.presentation);
    CHECK(requirements.crsf_uart);
    CHECK(requirements.control_task);
    CHECK(requirements.control_runtime);
    CHECK(!requirements.module_online);
    for (const auto condition : optional_module_cases) {
      FakeOta pending_ota;
      pending_ota.pending = true;
      BootManager pending_boot(pending_ota, diagnostics, profile);
      SelfTestResult healthy{};
      healthy.storage = true;
      healthy.inputs = true;
      healthy.display = true;
      healthy.crsf_uart = true;
      healthy.control_task = true;
      healthy.control_runtime = true;
      healthy.module = condition;
      CHECK(pending_boot.finish_startup(healthy, 1000));
      CHECK(pending_ota.marked);
      CHECK(!pending_ota.rollback);
    }
  }

  FakeOta ota;
  BootManager boot(ota, diagnostics);
  ota.pending = true;
  SelfTestResult unhealthy{};
  unhealthy.storage = true;
  unhealthy.inputs = false;
  unhealthy.display = true;
  unhealthy.crsf_uart = true;
  unhealthy.control_task = true;
  unhealthy.control_runtime = true;
  unhealthy.module = ModuleBootCondition::Online;
  CHECK(!boot.finish_startup(unhealthy, 2000));
  CHECK(ota.rollback);
  CHECK(boot.enter_recovery(false, 3));

  FakeManifestVerifier verifier;
  UpdateManager updates(ota, diagnostics, verifier, "esp32c3", "0.1.0");
  FirmwareManifest invalid{"rivettx", "esp32c3", "1.0",
                           "http://invalid", 2, {0x01}};
  CHECK(!updates.install(invalid, true, 3000));
  FirmwareManifest valid{"rivettx", "esp32c3", "1.0",
                         "https://example.invalid/rivettx.bin", 2, {0x01}};
  CHECK(updates.install(valid, true, 4000));
  CHECK(ota.updated_url == valid.url);
  CHECK(verifier.calls == 1);
  CHECK(verifier.last_signature_size == 1);

  FirmwareManifest unsigned_manifest = valid;
  unsigned_manifest.signature.clear();
  CHECK(!updates.install(unsigned_manifest, true, 4200));
  CHECK(updates.rejection_reason() ==
        "firmware signature verification failed");
  verifier.accept = false;
  CHECK(!updates.install(valid, true, 4300));
  CHECK(ota.updated_url == valid.url);
  verifier.accept = true;

  FakeOta s3_ota;
  UpdateManager s3_updates(
      s3_ota, diagnostics, verifier, "esp32s3", "0.1.0");
  FirmwareManifest wrong_target{"rivettx", "esp32c3", "1.0",
                                "https://example.invalid/c3.bin", 2, {0x01}};
  CHECK(!s3_updates.install(wrong_target, true, 4500));
  FirmwareManifest s3_valid{"rivettx", "esp32s3", "1.0",
                            "https://example.invalid/s3.bin", 2, {0x01}};
  CHECK(s3_updates.install(s3_valid, true, 5000));
  CHECK(s3_ota.updated_url == s3_valid.url);
  FirmwareManifest downgrade = s3_valid;
  downgrade.version = "0.0.9";
  CHECK(!s3_updates.install(downgrade, true, 5100));

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

  CalibrationWizard invalid_calibration;
  invalid_calibration.begin();
  raw.axes.fill(2048);
  for (int i = 0; i < 10; ++i) {
    invalid_calibration.sample(raw);
  }
  CHECK(invalid_calibration.next());
  raw.axes.fill(2500);
  invalid_calibration.sample(raw);
  raw.axes.fill(4000);
  invalid_calibration.sample(raw);
  CHECK(!invalid_calibration.next());

  FakeTransport missing_transport;
  TelemetryRegistry missing_telemetry;
  CrsfParser missing_parser(missing_telemetry);
  DiagnosticLog missing_diagnostics;
  ModuleSupervisor missing_module(
      missing_transport, missing_parser, missing_diagnostics);
  missing_module.start(1, 1000);
  missing_module.poll(1001001);
  CHECK(missing_module.status().state == ModuleState::Offline);
  const std::size_t lost_events = missing_diagnostics.size();
  missing_module.poll(2001001);
  CHECK(missing_diagnostics.size() == lost_events);

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

void test_openpocket_product_services()
{
  const auto osd_row = [](const CharacterOsdFrame& frame, std::size_t row) {
    const auto begin =
        frame.cells.begin() + static_cast<std::ptrdiff_t>(row * kOsdColumns);
    return std::string(begin, begin + kOsdColumns);
  };

  CHECK(vrx_frequency_mhz(0, 0) == 5865);
  CHECK(vrx_frequency_mhz(5, 7) == 5621);
  CHECK(vrx_frequency_mhz(6, 0) == 0);

  FakeVrxHardware hardware;
  VrxController vrx(hardware, 80);
  CHECK(vrx.select(3, 3, 1000));
  vrx.set_video_signal(true, true);
  vrx.tick(2000);
  CHECK(vrx.status().frequency_mhz == 5800);
  CHECK(vrx.status().video_signal);
  CHECK(vrx.status().strength_percent == 100);
  CHECK(vrx.begin_scan(100000));
  TimeUs scan_time = 100000;
  for (std::size_t step = 0; step < 120 && vrx.status().scanning;
       ++step) {
    vrx.tick(scan_time);
    scan_time += 80000;
    vrx.tick(scan_time);
    scan_time += 1000;
  }
  CHECK(!vrx.status().scanning);
  CHECK(vrx.status().frequency_mhz == 5800);

  Model model = make_default_model();
  UiHomeStatus home{};
  home.battery_mv = 3900;
  home.battery_percent = 75;
  home.battery_percent_valid = true;
  home.link_quality = 92;
  home.module_online = true;
  home.channels[4] = -kResolution;
  home.video_signal = true;
  CharacterOsdComposer osd;
  osd.compose(model, home, vrx.status());
  CHECK(osd_row(osd.frame(), 0).find("BAT 75%") == 0);
  CHECK(osd_row(osd.frame(), 0).find("LQ 92%") == 24);
  for (std::size_t row = 1; row < 12; ++row) {
    CHECK(osd_row(osd.frame(), row) == std::string(kOsdColumns, ' '));
  }
  CHECK(osd_row(osd.frame(), 12).find("DISARMED") == 0);
  CHECK(osd_row(osd.frame(), 12).find("VRX B4 CH4") == 20);

  UiHomeStatus warning_home = home;
  warning_home.warning_count = 2;
  warning_home.warnings[0] = UiWarningCode::BatteryCritical;
  warning_home.warnings[1] = UiWarningCode::VideoNoSignal;
  VrxStatus no_signal = vrx.status();
  no_signal.video_signal = false;
  osd.compose(model, warning_home, no_signal);
  CHECK(osd_row(osd.frame(), 7).find("! BATTERY CRITICAL !") == 5);
  CHECK(osd_row(osd.frame(), 8).find("+1 MORE") == 11);

  CharacterOsdUi osd_ui;
  osd_ui.set_screen(make_openpocket_home_screen(model, home));
  CHECK(osd_ui.render(vrx.status()));
  CHECK(osd_row(osd_ui.frame(), 0).find("BAT 75%") == 0);
  CHECK(osd_ui.handle({UiEventType::Enter}));
  osd_ui.set_screen(make_openpocket_home_screen(model, home));
  UiChange osd_change{};
  CHECK(osd_ui.take_change(osd_change));
  CHECK(osd_change.screen_id == "openpocket.home");
  CHECK(osd_change.field_id == "menu");

  osd_ui.set_screen(make_openpocket_main_menu_screen());
  CHECK(osd_ui.render(vrx.status()));
  CHECK(osd_row(osd_ui.frame(), 2).find("> MODEL") == 0);
  CHECK(osd_ui.handle({UiEventType::Enter}));
  CHECK(osd_ui.take_change(osd_change));
  CHECK(osd_change.screen_id == "openpocket.menu");
  CHECK(osd_change.field_id == "group.model");

  osd_ui.set_screen(
      make_openpocket_group_menu_screen(OpenPocketMenuGroup::Model));
  CHECK(osd_ui.handle({UiEventType::Rotate, 1}));
  CHECK(osd_ui.handle({UiEventType::Enter}));
  CHECK(osd_ui.take_change(osd_change));
  CHECK(osd_change.screen_id == "openpocket.group.model");
  CHECK(osd_change.field_id == "model");

  osd_ui.set_screen(make_main_menu_screen());
  CHECK(osd_ui.handle({UiEventType::Rotate, 13}));
  CHECK(osd_ui.selected_index() == 13);
  CHECK(osd_ui.scroll_offset() == 4);
  CHECK(osd_ui.render(vrx.status()));
  CHECK(osd_ui.frame().at(0, 11) == '>');
  CHECK(osd_ui.handle({UiEventType::Enter}));
  CHECK(osd_ui.take_change(osd_change));
  CHECK(osd_change.screen_id == "menu");
  CHECK(osd_change.field_id == "finder");

  osd_ui.set_screen(make_openpocket_video_screen(vrx.status()));
  CHECK(osd_ui.render(vrx.status()));
  CHECK(osd_row(osd_ui.frame(), 2).find("> BAND") == 0);
  CHECK(osd_row(osd_ui.frame(), 4).find("FREQUENCY") !=
        std::string::npos);
  CHECK(osd_row(osd_ui.frame(), 5).find("VIDEO") != std::string::npos);
  CHECK(osd_ui.handle({UiEventType::Enter}));
  CHECK(osd_ui.handle({UiEventType::Rotate, 99}));
  CHECK(osd_ui.handle({UiEventType::Enter}));
  CHECK(osd_ui.take_change(osd_change));
  CHECK(osd_change.field_id == "band");
  CHECK(osd_change.value == static_cast<int32_t>(kVrxBandCount));

  osd_ui.set_screen(make_model_setup_screen(model));
  const int32_t original_model_id = osd_ui.screen().fields[0].value;
  CHECK(osd_ui.handle({UiEventType::Enter}));
  CHECK(osd_ui.editing());
  CHECK(osd_ui.handle({UiEventType::Rotate, 4}));
  const int32_t staged_model_id = original_model_id + 4;
  osd_ui.set_screen(make_model_setup_screen(model));
  CHECK(osd_ui.editing());
  CHECK(osd_ui.screen().fields[0].value == staged_model_id);
  CHECK(osd_ui.render(vrx.status()));
  CHECK(osd_row(osd_ui.frame(), 0).find("EDIT") != std::string::npos);
  CHECK(osd_ui.handle({UiEventType::Back}));
  CHECK(!osd_ui.editing());
  CHECK(osd_ui.screen().fields[0].value == original_model_id);
  CHECK(!osd_ui.take_change(osd_change));

  CHECK(osd_ui.handle({UiEventType::Enter}));
  CHECK(osd_ui.handle({UiEventType::Rotate, 2}));
  CHECK(osd_ui.handle({UiEventType::Enter}));
  CHECK(!osd_ui.editing());
  CHECK(osd_ui.take_change(osd_change));
  CHECK(osd_change.screen_id == "model");
  CHECK(osd_change.field_id == "model_id");
  CHECK(osd_change.value == original_model_id + 2);
  CHECK(osd_ui.handle({UiEventType::Back}));
  CHECK(osd_ui.take_back_request());
  CHECK(!osd_ui.take_back_request());

  UsbSimulator usb;
  CHECK(!usb.enter(false, true));
  CHECK(usb.enter(true, true));
  CHECK(usb.active());
  CHECK(!usb.rf_output_allowed());
  ControlInputs controls{};
  controls.axes[0] = 512;
  controls.axes[4] = -333;
  controls.switches[4] = true;
  ChannelFrame channels{};
  channels.channels[4] = 1024;
  const auto report = usb.report(controls, channels);
  CHECK(report.axes[0] == 512);
  CHECK(report.axes[4] == -333);
  CHECK((report.buttons & (1UL << 4)) != 0);
  usb.leave();
  CHECK(usb.rf_output_allowed());

  BatteryEstimator estimator;
  const auto power =
      estimator.estimate(3750, true, ChargeState::Charging, true, 500, 750);
  CHECK(power.percentage_valid);
  CHECK(power.percentage == 50);
  CHECK(power.runtime_valid);
  CHECK(power.runtime_minutes == 90);
  CHECK(estimator.estimate(0, false, ChargeState::Fault, false)
            .sensor_fault);

  OnboardingGuide guide;
  OnboardingEvidence evidence{};
  guide.begin();
  CHECK(guide.advance(evidence));
  evidence.calibration_valid = true;
  CHECK(guide.advance(evidence));
  evidence.arm_switch_identified = true;
  CHECK(guide.advance(evidence));
  evidence.aux_positions_verified = true;
  CHECK(guide.advance(evidence));
  evidence.elrs_online = true;
  CHECK(guide.advance(evidence));
  CHECK(guide.advance(evidence));
  evidence.battery_profile_valid = true;
  CHECK(guide.advance(evidence));
  evidence.channel_preview_valid = true;
  CHECK(!guide.advance(evidence));
  evidence.arm_channel_low = true;
  CHECK(guide.advance(evidence));
  CHECK(guide.complete());
}

void test_rx5808_backend()
{
  VrxCommandQueue startup_commands;
  CHECK(startup_commands.empty());
  CHECK(startup_commands.push({VrxCommandType::Tune, 4, 5}));
  // An immediate model switch must be queued behind, not overwrite, the
  // active model restored from storage.
  CHECK(startup_commands.push({VrxCommandType::Tune, 2, 7}));
  VrxCommand startup_command{};
  CHECK(startup_commands.pop(startup_command));
  CHECK(startup_command.type == VrxCommandType::Tune);
  CHECK(startup_command.band == 4);
  CHECK(startup_command.channel == 5);
  CHECK(startup_commands.pop(startup_command));
  CHECK(startup_command.band == 2);
  CHECK(startup_command.channel == 7);
  CHECK(startup_commands.empty());

  Rtc6715Program program{};
  CHECK(!rtc6715_program_for_frequency(0, program));
  CHECK(!rtc6715_program_for_frequency(5801, program));
  CHECK(!rtc6715_program_for_frequency(6000, program));
  CHECK(rtc6715_program_for_frequency(5865, program));
  CHECK(program.synthesizer_b == 0x2A05);
  CHECK(program.frame == 0x00540B1);
  CHECK(rtc6715_program_for_frequency(5800, program));
  CHECK(program.synthesizer_b == 0x2984);
  CHECK(program.frame == 0x0053091);
  CHECK(rtc6715_program_for_frequency(5362, program));
  CHECK(program.synthesizer_b == 0x2609);
  CHECK(program.frame == 0x004C131);
  CHECK(rtc6715_program_for_frequency(5945, program));
  CHECK(program.synthesizer_b == 0x2A8D);
  CHECK(program.frame == 0x00551B1);

  FakeRtc6715Io io;
  Rtc6715Backend backend(io, {1, 100});
  CHECK(backend.initialize());
  CHECK(backend.tune_state() == VrxTuneState::Idle);
  backend.tick(10000000);
  CHECK(backend.tune_state() == VrxTuneState::Idle);

  const auto check_startup = [](TimeUs pre_task_delay_us,
                                uint8_t stored_band,
                                uint8_t stored_channel) {
    FakeRtc6715Io startup_io;
    Rtc6715Backend startup_backend(startup_io, {1, 100});
    CHECK(startup_backend.initialize());
    startup_backend.tick(pre_task_delay_us);
    CHECK(startup_backend.tune_state() == VrxTuneState::Idle);
    const uint16_t frequency =
        vrx_frequency_mhz(stored_band, stored_channel);
    CHECK(startup_backend.start_tune(frequency, pre_task_delay_us));
    TimeUs transaction_now = pre_task_delay_us;
    for (std::size_t tick = 0;
         tick < 100 &&
         startup_backend.tune_state() == VrxTuneState::Pending;
         ++tick) {
      startup_backend.tick(transaction_now);
      transaction_now += 10;
    }
    CHECK(startup_backend.tune_state() == VrxTuneState::Complete);
    CHECK(startup_backend.tuned_frequency_mhz() == frequency);
    CHECK(startup_io.frames.size() == 1);
  };
  check_startup(0, 0, 0);          // normal startup
  check_startup(10000000, 4, 5);   // ten-second first-run calibration
  check_startup(4000000, 1, 2);    // cancelled calibration
  check_startup(2000000, 2, 3);    // recovery startup
  check_startup(3000000, 3, 4);    // recovered storage mirror
  check_startup(8000000, 5, 6);    // restored active model

  TimeUs now = 1000;
  std::size_t programmed = 0;
  for (uint8_t band = 0; band < kVrxBandCount; ++band) {
    for (uint8_t channel = 0; channel < kVrxChannelsPerBand; ++channel) {
      const uint16_t frequency = vrx_frequency_mhz(band, channel);
      CHECK(frequency != 0);
      CHECK(vrx_frequency_supported(frequency));
      CHECK(rtc6715_program_for_frequency(frequency, program));
      CHECK(backend.start_tune(frequency, now));
      for (std::size_t tick = 0;
           tick < 100 && backend.tune_state() == VrxTuneState::Pending;
           ++tick) {
        backend.tick(now);
        now += 10;
      }
      CHECK(backend.tune_state() == VrxTuneState::Complete);
      CHECK(backend.tuned_frequency_mhz() == frequency);
      CHECK(io.frames.size() == programmed + 1);
      if (io.frames.size() == programmed + 1) {
        CHECK(io.frames.back() == program.frame);
      }
      ++programmed;
    }
  }
  CHECK(programmed == kVrxBandCount * kVrxChannelsPerBand);
  CHECK(!backend.start_tune(5801, now));

  io.fail_writes = true;
  CHECK(backend.start_tune(5800, now));
  backend.tick(now);
  CHECK(backend.tune_state() == VrxTuneState::Failed);
  io.fail_writes = false;
  CHECK(backend.start_tune(5800, now + 100));
  for (std::size_t tick = 0;
       tick < 100 && backend.tune_state() == VrxTuneState::Pending;
       ++tick) {
    now += 10;
    backend.tick(now);
  }
  CHECK(backend.tune_state() == VrxTuneState::Complete);

  FakeRtc6715Io timeout_io;
  Rtc6715Backend timeout_backend(timeout_io, {100, 10});
  CHECK(timeout_backend.initialize());
  CHECK(timeout_backend.start_tune(5800, 0));
  timeout_backend.tick(10000);
  CHECK(timeout_backend.tune_state() == VrxTuneState::Failed);

  FakeVrxHardware hardware;
  VrxControllerConfig config{};
  config.scan_dwell_ms = 20;
  config.tune_timeout_ms = 100;
  config.rssi_sample_interval_ms = 10;
  config.rssi_stale_ms = 100;
  config.rssi_min_adc = 100;
  config.rssi_max_adc = 1100;
  config.rssi_filter_shift = 1;
  config.rssi_hysteresis_percent = 5;
  hardware.fixed_rssi = 600;
  VrxController vrx(hardware, config);
  CHECK(vrx.select(0, 0, 0));
  vrx.tick(0);
  CHECK(vrx.status().available);
  CHECK(vrx.status().rssi_state == VrxRssiState::Valid);
  CHECK(vrx.status().strength_percent == 50);
  vrx.set_video_signal(false, true);
  CHECK(vrx.status().signal_fresh);
  CHECK(!vrx.status().video_signal);
  CHECK(vrx.status().rssi_state == VrxRssiState::Valid);

  hardware.fixed_rssi = 620;
  vrx.tick(10000);
  CHECK(vrx.status().strength_percent == 50);
  hardware.fixed_rssi = 1100;
  vrx.tick(20000);
  CHECK(vrx.status().strength_percent == 75);
  CHECK(vrx.select(0, 1, 21000));
  CHECK(vrx.status().rssi_state == VrxRssiState::Stale);
  vrx.tick(21000);
  CHECK(vrx.status().rssi_state == VrxRssiState::Valid);
  hardware.rssi_state = VrxAdcSampleState::SensorFault;
  vrx.tick(31000);
  CHECK(vrx.status().rssi_state == VrxRssiState::SensorFault);
  hardware.rssi_state = VrxAdcSampleState::Unavailable;
  vrx.tick(41000);
  CHECK(vrx.status().rssi_state == VrxRssiState::Unavailable);

  VrxControllerConfig malformed = config;
  malformed.rssi_min_adc = 2000;
  malformed.rssi_max_adc = 1000;
  VrxController invalid_calibration(hardware, malformed);
  CHECK(invalid_calibration.status().rssi_state ==
        VrxRssiState::SensorFault);
  CHECK(invalid_calibration.status().failure ==
        VrxFailure::RssiCalibration);

  hardware.rssi_state = VrxAdcSampleState::Valid;
  hardware.fixed_rssi = -1;
  hardware.strongest_frequency = 5800;
  hardware.strongest_rssi = 1090;
  hardware.default_rssi = 150;
  VrxController scanner(hardware, config);
  CHECK(scanner.select(0, 0, 0));
  scanner.tick(0);
  CHECK(scanner.begin_scan(1000));
  TimeUs scan_now = 1000;
  std::size_t scan_steps = 0;
  while (scanner.status().scanning && scan_steps < 120) {
    scanner.tick(scan_now);
    scan_now += 20000;
    scanner.tick(scan_now);
    scan_now += 1000;
    ++scan_steps;
  }
  CHECK(scan_steps <= 50);
  CHECK(!scanner.status().scanning);
  CHECK(scanner.status().band == 3);
  CHECK(scanner.status().channel == 3);
  CHECK(scanner.status().frequency_mhz == 5800);
  uint8_t result_band = 0;
  uint8_t result_channel = 0;
  CHECK(scanner.take_scan_result(result_band, result_channel));
  CHECK(result_band == 3);
  CHECK(result_channel == 3);

  CHECK(scanner.begin_scan(scan_now));
  scanner.tick(scan_now);
  CHECK(scanner.cancel_scan(scan_now + 1));
  scanner.tick(scan_now + 1);
  CHECK(!scanner.status().scanning);
  CHECK(scanner.status().frequency_mhz == 5800);

  Model startup_model = make_default_model();
  startup_model.vrx_band = 4;
  startup_model.vrx_channel = 5;
  CHECK(scanner.select(startup_model.vrx_band,
                       startup_model.vrx_channel, scan_now + 2));
  scanner.tick(scan_now + 2);
  CHECK(scanner.status().frequency_mhz ==
        vrx_frequency_mhz(4, 5));
  Model switched_model = make_default_model();
  switched_model.vrx_band = 2;
  switched_model.vrx_channel = 7;
  CHECK(scanner.select(switched_model.vrx_band,
                       switched_model.vrx_channel, scan_now + 3));
  scanner.tick(scan_now + 3);
  CHECK(scanner.status().frequency_mhz ==
        vrx_frequency_mhz(2, 7));

  hardware.available = false;
  CHECK(!scanner.select(0, 0, scan_now + 4));
  CHECK(scanner.status().failure == VrxFailure::TuneRejected);
  CHECK(!scanner.status().available);
  hardware.available = true;
  CHECK(scanner.select(0, 0, scan_now + 5));
  scanner.tick(scan_now + 5);
  CHECK(scanner.status().available);
  CHECK(scanner.status().failure == VrxFailure::None);

  FakeVrxHardware dwell_hardware;
  VrxController dwell_controller(dwell_hardware, config);
  CHECK(dwell_controller.select(0, 0, 0));
  dwell_controller.tick(0);
  CHECK(dwell_controller.begin_scan(100));
  dwell_controller.tick(100);
  const uint32_t tunes_after_first_candidate = dwell_hardware.tunes;
  dwell_controller.tick(20099);
  CHECK(dwell_hardware.tunes == tunes_after_first_candidate);
  dwell_controller.tick(20100);
  CHECK(dwell_hardware.tunes == tunes_after_first_candidate + 1);

  VrxStatus display_status = scanner.status();
  display_status.band = 3;
  display_status.channel = 3;
  display_status.frequency_mhz = 5800;
  display_status.rssi_state = VrxRssiState::Valid;
  display_status.strength_percent = 87;
  display_status.signal_fresh = true;
  display_status.video_signal = false;
  UiScreen video_screen = make_openpocket_video_screen(display_status);
  CHECK(video_screen.fields[0].value == 4);
  CHECK(video_screen.fields[1].value == 4);
  CHECK(video_screen.fields[2].value_text == "5800MHZ");
  CHECK(video_screen.fields[3].value_text == "NO SIGNAL");
  CHECK(video_screen.fields[4].value_text == "87%");
  display_status.rssi_state = VrxRssiState::Stale;
  video_screen = make_openpocket_video_screen(display_status);
  CHECK(video_screen.fields[4].value_text == "STALE");
  display_status.rssi_state = VrxRssiState::Unavailable;
  video_screen = make_openpocket_video_screen(display_status);
  CHECK(video_screen.fields[4].value_text == "UNAVAILABLE");
  display_status.rssi_state = VrxRssiState::SensorFault;
  video_screen = make_openpocket_video_screen(display_status);
  CHECK(video_screen.fields[4].value_text == "SENSOR FAULT");
}

void test_openpocket_board_power()
{
  FakeBoardPowerHardware hardware;
  BoardPowerController power(hardware, {50, 60});
  CHECK(power.initialize(false));
  CHECK(hardware.initialized);
  CHECK(!hardware.video);
  CHECK(!hardware.display);
  CHECK(!hardware.elrs);
  CHECK(hardware.reset);
  CHECK(hardware.backlight == 0);

  CHECK(power.request_video(true));
  CHECK(power.request_display(true, 70));
  CHECK(power.request_elrs(true));
  CHECK(power.status().video_5v);
  CHECK(power.status().display_5v);
  CHECK(power.status().elrs_5v);
  CHECK(power.status().backlight_percent == 0);
  CHECK(power.status().display_controller_reset_asserted);
  CHECK(power.status().display_state == DisplayPowerState::PowerSettling);

  power.tick(0);
  CHECK(power.status().vbus_present);
  CHECK(power.status().charger.state == BoardSensorState::Valid);
  CHECK(power.status().charger.battery_mv == 3880);
  CHECK(power.status().fuel_gauge.cell_mv == 3890);
  CHECK(power.status().fuel_gauge.state_of_charge == 67);
  power.tick(20000);
  CHECK(!power.status().display_controller_reset_asserted);
  CHECK(power.status().display_state ==
        DisplayPowerState::WaitingForController);
  power.set_display_controller_ready(true);
  power.tick(20001);
  CHECK(power.status().backlight_percent == 70);
  CHECK(power.status().display_state == DisplayPowerState::Ready);
  power.set_display_controller_ready(false);
  CHECK(power.status().backlight_percent == 0);
  CHECK(power.status().display_state ==
        DisplayPowerState::WaitingForController);

  CHECK(power.set_simulator_mode(true));
  CHECK(!hardware.elrs);
  CHECK(!hardware.video);
  CHECK(!power.request_elrs(true));
  CHECK(!power.request_video(true));
  // USB-only simulator operation is valid without a battery or gauge.
  hardware.gauge = {};
  hardware.charger = {BoardSensorState::Valid, ChargeState::Disconnected,
                      0, true, false, false};
  power.tick(50000);
  CHECK(power.status().vbus_present);
  CHECK(power.status().fuel_gauge.state == BoardSensorState::Unavailable);
  CHECK(power.request_display(false));
}

void test_amt630a_flash_controller()
{
  static constexpr std::array<uint8_t, 3> abc{'a', 'b', 'c'};
  const std::array<uint8_t, 32> abc_sha{
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
      0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
      0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
      0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  CHECK(amt630a_sha256(abc.data(), abc.size()) == abc_sha);

  std::array<uint8_t, 700> image{};
  for (std::size_t index = 0; index < image.size(); ++index) {
    image[index] = static_cast<uint8_t>((index * 29U + 7U) & 0xffU);
  }
  const auto digest = amt630a_sha256(image.data(), image.size());
  FakeAmt630aHardware hardware;
  Amt630aController controller(hardware, {1000, 20});
  TimeUs now = 100;
  CHECK(controller.initialize(now));
  CHECK(controller.status().state == Amt630aState::Detecting);
  controller.tick(now);
  CHECK(controller.status().state == Amt630aState::Ready);
  CHECK(controller.status().identity == kAmt630aExpectedIdentity);
  CHECK(controller.start_program(image.data(), image.size(), digest, now));
  for (std::size_t ticks = 0; ticks < 100 &&
       controller.status().state != Amt630aState::Complete; ++ticks) {
    now += 1000;
    controller.tick(now);
  }
  CHECK(controller.status().state == Amt630aState::Complete);
  CHECK(controller.status().fault == Amt630aFault::None);
  CHECK(controller.status().progress_percent == 100);
  CHECK(controller.status().runtime.panel_timing_active);
  CHECK(hardware.programmed_addresses.size() == 3);
  CHECK(hardware.programmed_addresses[0] == 0);
  CHECK(hardware.programmed_addresses[1] == 256);
  CHECK(hardware.programmed_addresses[2] == 512);
  CHECK(std::equal(image.begin(), image.end(), hardware.flash.begin()));

  FakeAmt630aHardware current_flash;
  std::copy(image.begin(), image.end(), current_flash.flash.begin());
  Amt630aController ensure_current(current_flash, {1000, 20});
  CHECK(ensure_current.ensure_program(image.data(), image.size(), digest, 0));
  for (std::size_t ticks = 0; ticks < 20 &&
       ensure_current.status().state != Amt630aState::Complete; ++ticks) {
    ensure_current.tick(static_cast<TimeUs>(ticks + 1) * 1000U);
  }
  CHECK(ensure_current.status().state == Amt630aState::Complete);
  CHECK(current_flash.erase_count == 0);
  CHECK(current_flash.programmed_addresses.empty());

  FakeAmt630aHardware corrupt_flash;
  Amt630aController ensure_corrupt(corrupt_flash, {1000, 20});
  CHECK(ensure_corrupt.ensure_program(image.data(), image.size(), digest, 0));
  for (std::size_t ticks = 0; ticks < 120 &&
       ensure_corrupt.status().state != Amt630aState::Complete; ++ticks) {
    ensure_corrupt.tick(static_cast<TimeUs>(ticks + 1) * 1000U);
  }
  CHECK(ensure_corrupt.status().state == Amt630aState::Complete);
  CHECK(corrupt_flash.erase_count == 1);
  CHECK(std::equal(image.begin(), image.end(), corrupt_flash.flash.begin()));

  FakeAmt630aHardware identity_fault;
  identity_fault.identity = 0x1234;
  Amt630aController bad_identity(identity_fault);
  CHECK(bad_identity.initialize(0));
  bad_identity.tick(0);
  CHECK(bad_identity.status().fault == Amt630aFault::Identity);
  identity_fault.identity = kAmt630aExpectedIdentity;
  CHECK(bad_identity.recover(1000));
  bad_identity.tick(1000);
  CHECK(bad_identity.status().state == Amt630aState::Ready);

  FakeAmt630aHardware bad_readback;
  Amt630aController verify_fault(bad_readback, {1000, 20});
  CHECK(verify_fault.initialize(0));
  verify_fault.tick(0);
  CHECK(verify_fault.start_program(image.data(), image.size(), digest, 0));
  bad_readback.readback_fails = true;
  for (std::size_t ticks = 0; ticks < 30 &&
       verify_fault.status().state != Amt630aState::Fault; ++ticks) {
    verify_fault.tick(static_cast<TimeUs>(ticks + 1) * 1000U);
  }
  CHECK(verify_fault.status().fault == Amt630aFault::Readback);

  FakeAmt630aHardware timeout_hardware;
  timeout_hardware.busy_polls = 255;
  Amt630aController timeout(timeout_hardware, {100, 20});
  CHECK(timeout.initialize(0));
  timeout.tick(0);
  CHECK(timeout.start_program(image.data(), image.size(), digest, 0));
  timeout.tick(1000);    // enter ISP
  timeout.tick(2000);    // start erase
  timeout.tick(101000);  // bounded timeout
  CHECK(timeout.status().fault == Amt630aFault::Timeout);
}

void test_complete_openpocket_menu()
{
  FakeOpenPocketScreens screens;
  screens.home.battery_mv = 3900;
  screens.home.battery_percent = 75;
  screens.home.battery_percent_valid = true;
  screens.home.link_quality = 92;
  screens.home.module_online = true;
  screens.home.channels[4] = -kResolution;
  screens.vrx.available = true;
  screens.vrx.signal_fresh = true;
  screens.vrx.video_signal = true;
  screens.vrx.band = 3;
  screens.vrx.channel = 3;
  screens.vrx.frequency_mhz = 5800;

  struct GroupExpectation {
    OpenPocketPage group;
    std::vector<OpenPocketPage> details;
  };
  const std::array<GroupExpectation, 7> expectations{{
      {OpenPocketPage::ModelMenu,
       {OpenPocketPage::Models, OpenPocketPage::ModelSetup,
        OpenPocketPage::Inputs, OpenPocketPage::Mixes,
        OpenPocketPage::Limits, OpenPocketPage::FlightModes,
        OpenPocketPage::Curves, OpenPocketPage::LogicalSwitches,
        OpenPocketPage::SpecialFunctions, OpenPocketPage::Timers}},
      {OpenPocketPage::RadioMenu,
       {OpenPocketPage::Outputs, OpenPocketPage::Power}},
      {OpenPocketPage::ElrsMenu,
       {OpenPocketPage::Elrs, OpenPocketPage::Finder}},
      {OpenPocketPage::VideoMenu, {OpenPocketPage::Video}},
      {OpenPocketPage::UsbMenu, {OpenPocketPage::Usb}},
      {OpenPocketPage::DiagnosticsMenu,
       {OpenPocketPage::Warnings, OpenPocketPage::Telemetry}},
      {OpenPocketPage::SystemMenu,
       {OpenPocketPage::Web, OpenPocketPage::System}},
  }};

  OpenPocketMenuController menu(screens);
  const auto navigate_to = [&menu, &screens, &expectations](
                               std::size_t group_index,
                               std::size_t detail_index) {
    menu.start(screens.home);
    CHECK(menu.page() == OpenPocketPage::Home);
    CHECK(menu.depth() == 0);
    CHECK(menu.handle({UiEventType::Enter}));
    CHECK(menu.page() == OpenPocketPage::MainMenu);
    CHECK(menu.depth() == 1);
    if (group_index != 0) {
      CHECK(menu.handle(
          {UiEventType::Rotate, static_cast<int16_t>(group_index)}));
    }
    CHECK(menu.handle({UiEventType::Enter}));
    CHECK(menu.page() == expectations[group_index].group);
    CHECK(menu.depth() == 2);
    if (detail_index != 0) {
      CHECK(menu.handle(
          {UiEventType::Rotate, static_cast<int16_t>(detail_index)}));
    }
    CHECK(menu.handle({UiEventType::Enter}));
  };

  for (std::size_t group_index = 0;
       group_index < expectations.size(); ++group_index) {
    for (std::size_t detail_index = 0;
         detail_index < expectations[group_index].details.size();
         ++detail_index) {
      navigate_to(group_index, detail_index);
      CHECK(menu.page() ==
            expectations[group_index].details[detail_index]);
      CHECK(menu.depth() == 3);
      CHECK(screens.last_page ==
            expectations[group_index].details[detail_index]);
      CHECK(menu.render(screens.vrx));
      CHECK(menu.handle({UiEventType::Back}));
      CHECK(menu.page() == OpenPocketPage::Home);
      CHECK(menu.depth() == 0);
    }
  }

  navigate_to(0, 1);
  CHECK(menu.page() == OpenPocketPage::ModelSetup);
  const int32_t original_model_id = menu.screen().fields[0].value;
  CHECK(menu.handle({UiEventType::Enter}));
  CHECK(menu.editing());
  CHECK(menu.handle({UiEventType::Rotate, 3}));
  CHECK(menu.handle({UiEventType::Back}));
  CHECK(menu.page() == OpenPocketPage::Home);
  CHECK(menu.depth() == 0);
  CHECK(!menu.editing());
  UiChange change{};
  CHECK(!menu.take_change(change));

  navigate_to(0, 1);
  CHECK(menu.handle({UiEventType::Enter}));
  CHECK(menu.handle({UiEventType::Rotate, 2}));
  CHECK(menu.handle({UiEventType::Enter}));
  CHECK(menu.take_change(change));
  CHECK(change.screen_id == "model");
  CHECK(change.field_id == "model_id");
  CHECK(change.value == original_model_id + 2);
  CHECK(menu.page() == OpenPocketPage::ModelSetup);

  CHECK(menu.handle({UiEventType::Home}));
  CHECK(menu.page() == OpenPocketPage::Home);
  CHECK(menu.depth() == 0);
  CHECK(menu.render(screens.vrx));
  CHECK(menu.frame().at(0, 12) == 'D');

  menu.start(screens.home);
  CHECK(menu.handle({UiEventType::Enter}));
  CHECK(menu.page() == OpenPocketPage::MainMenu);
  CHECK(menu.handle({UiEventType::Back}));
  CHECK(menu.page() == OpenPocketPage::Home);

  const auto osd_row = [](const CharacterOsdFrame& frame,
                          std::size_t row) {
    const auto begin =
        frame.cells.begin() + static_cast<std::ptrdiff_t>(row * kOsdColumns);
    return std::string(begin, begin + kOsdColumns);
  };
  UiHomeStatus notice_home = screens.home;
  notice_home.warning_count = 1;
  notice_home.warnings[0] = UiWarningCode::BatteryLow;
  constexpr TimeUs notice_start = 1000000;
  menu.start(notice_home, notice_start);
  CHECK(menu.render(screens.vrx));
  CHECK(osd_row(menu.frame(), 7).find("! BATTERY LOW !") !=
        std::string::npos);
  menu.refresh(notice_home,
               notice_start + kOpenPocketNotificationDurationUs - 1);
  CHECK(menu.render(screens.vrx));
  CHECK(osd_row(menu.frame(), 7).find("! BATTERY LOW !") !=
        std::string::npos);
  menu.refresh(notice_home,
               notice_start + kOpenPocketNotificationDurationUs);
  CHECK(menu.render(screens.vrx));
  CHECK(osd_row(menu.frame(), 7) == std::string(kOsdColumns, ' '));

  UiHomeStatus critical_home = screens.home;
  critical_home.warning_count = 2;
  critical_home.warnings[0] = UiWarningCode::VideoNoSignal;
  critical_home.warnings[1] = UiWarningCode::LinkCritical;
  menu.refresh(critical_home, notice_start + 10000000);
  CHECK(menu.render(screens.vrx));
  CHECK(osd_row(menu.frame(), 7).find("! LINK CRITICAL !") !=
        std::string::npos);
  menu.refresh(critical_home, notice_start + 20000000);
  CHECK(menu.render(screens.vrx));
  CHECK(osd_row(menu.frame(), 7).find("! LINK CRITICAL !") !=
        std::string::npos);

  UiHomeStatus startup_home = screens.home;
  startup_home.warning_count = 1;
  startup_home.warnings[0] = UiWarningCode::ThrottleHigh;
  menu.refresh(startup_home, notice_start + 30000000);
  CHECK(menu.render(screens.vrx));
  CHECK(osd_row(menu.frame(), 7).find("! THROTTLE HIGH !") !=
        std::string::npos);
  menu.refresh(startup_home, notice_start + 40000000);
  CHECK(menu.render(screens.vrx));
  CHECK(osd_row(menu.frame(), 7).find("! THROTTLE HIGH !") !=
        std::string::npos);

  menu.refresh(screens.home, notice_start + 40000001);
  CHECK(menu.render(screens.vrx));
  CHECK(osd_row(menu.frame(), 7) == std::string(kOsdColumns, ' '));
  CHECK(openpocket_warning_is_persistent(UiWarningCode::ThrottleHigh));
  CHECK(openpocket_warning_is_persistent(UiWarningCode::BatteryCritical));
  CHECK(openpocket_warning_is_persistent(UiWarningCode::LinkLost));
  CHECK(!openpocket_warning_is_persistent(UiWarningCode::BatteryLow));
  CHECK(!openpocket_warning_is_persistent(UiWarningCode::VideoNoSignal));
}

void test_at7456e_backend()
{
  const auto pump = [](At7456eDriver& driver, TimeUs& now_us,
                       std::size_t ticks) {
    for (std::size_t tick = 0; tick < ticks; ++tick) {
      driver.tick(now_us);
      now_us += 1000;
    }
  };
  const auto settle = [&pump](At7456eDriver& driver, TimeUs& now_us) {
    for (std::size_t tick = 0;
         tick < 2000 && driver.status().pending_characters != 0; ++tick) {
      pump(driver, now_us, 1);
    }
  };

  FakeAt7456eSpi pal_spi;
  At7456eDriver pal(pal_spi);
  CharacterOsdFrame frame{};
  frame.cells.fill(' ');
  frame.cells[0] = 'P';
  frame.cells[kOsdColumns * 15 + 29] = 'Z';
  TimeUs pal_now = 0;
  pal.submit(frame);
  pal.start(pal_now);
  pump(pal, pal_now, 10);
  settle(pal, pal_now);
  CHECK(pal.status().initialized);
  CHECK(pal.status().healthy);
  CHECK(pal.status().video_present);
  CHECK(pal.status().standard == At7456eVideoStandard::Pal);
  CHECK(pal.status().visible_rows == 16);
  CHECK(pal.status().pending_characters == 0);
  CHECK(pal_spi.display[0] == 'P');
  CHECK(pal_spi.display[kOsdColumns * 15 + 29] == 'Z');

  const std::size_t writes_before = pal_spi.display_write_transactions();
  pal.submit(frame);
  pump(pal, pal_now, 50);
  CHECK(pal_spi.display_write_transactions() == writes_before);
  frame.cells[7] = 'X';
  pal.submit(frame);
  settle(pal, pal_now);
  CHECK(pal_spi.display[7] == 'X');
  CHECK(pal_spi.display_write_transactions() == writes_before + 1);

  FakeAt7456eSpi ntsc_spi;
  ntsc_spi.status_register = 0x01;
  At7456eDriver ntsc(ntsc_spi);
  CharacterOsdFrame ntsc_frame{};
  ntsc_frame.cells.fill(' ');
  ntsc_frame.cells[kOsdColumns * 12] = 'N';
  ntsc_frame.cells[kOsdColumns * 13] = 'H';
  TimeUs ntsc_now = 0;
  ntsc.submit(ntsc_frame);
  ntsc.start(ntsc_now);
  pump(ntsc, ntsc_now, 10);
  settle(ntsc, ntsc_now);
  CHECK(ntsc.status().standard == At7456eVideoStandard::Ntsc);
  CHECK(ntsc.status().visible_rows == kAt7456eNtscRows);
  CHECK(ntsc_spi.display[kOsdColumns * 12] == 'N');
  CHECK(!ntsc_spi.display_touched[kOsdColumns * 13]);

  pal_spi.status_register = 0x01;
  pump(pal, pal_now, 150);
  settle(pal, pal_now);
  CHECK(pal.status().standard == At7456eVideoStandard::Ntsc);
  CHECK(pal.status().visible_rows == kAt7456eNtscRows);

  pal_spi.status_register = 0x04;
  pump(pal, pal_now, 150);
  CHECK(!pal.status().video_present);
  CHECK(pal.status().standard == At7456eVideoStandard::Unknown);
  CHECK(pal.status().healthy);
  pal_spi.status_register = 0x02;
  pump(pal, pal_now, 150);
  settle(pal, pal_now);
  CHECK(pal.status().video_present);
  CHECK(pal.status().standard == At7456eVideoStandard::Pal);
  CHECK(pal.status().pending_characters == 0);

  FakeAt7456eSpi missing_spi;
  missing_spi.status_register = 0x04;
  At7456eDriver missing(missing_spi);
  TimeUs missing_now = 0;
  missing.start(missing_now);
  pump(missing, missing_now, 20);
  CHECK(missing.status().initialized);
  CHECK(missing.status().healthy);
  CHECK(!missing.status().video_present);
  CHECK(missing.status().visible_rows == kOsdRows);

  At7456eCustomCharacter warning{};
  warning.index = 0xF1;
  for (std::size_t index = 0; index < warning.pixels.size(); ++index) {
    warning.pixels[index] = static_cast<uint8_t>(index ^ 0x5A);
  }
  CHECK(pal.upload(warning));
  pump(pal, pal_now, 300);
  CHECK(!pal.status().character_upload_active);
  CHECK(pal_spi.glyph_commits == 1);
  CHECK(pal_spi.glyph_memory[warning.index] == warning.pixels);
  CHECK(pal.status().healthy);

  FakeAt7456eSpi failing_spi;
  At7456eDriver failing(failing_spi);
  TimeUs failing_now = 0;
  failing.start(failing_now);
  failing_spi.fail_next_queue = true;
  failing.tick(failing_now);
  CHECK(!failing.status().healthy);
  CHECK(failing.status().communication_failures == 1);
  failing_now += 100001;
  pump(failing, failing_now, 20);
  CHECK(failing.status().healthy);

  FakeAt7456eSpi stalled_spi;
  stalled_spi.busy_polls = 100;
  At7456eDriver stalled(stalled_spi);
  TimeUs stalled_now = 0;
  stalled.start(stalled_now);
  pump(stalled, stalled_now, 25);
  CHECK(stalled_spi.aborts == 1);
  CHECK(stalled.status().communication_failures == 1);
}

}  // namespace

int main()
{
  test_inputs_and_mixer();
  test_trim_controls();
  test_rotary_encoder();
  test_mixer_features();
  test_safety();
  test_crsf();
  test_virtual_elrs_module();
  test_elrs_management_and_finder();
  test_audio_alerts();
  test_speaker_service();
  test_removable_storage();
  test_storage();
  test_ui();
  test_services();
  test_module_update_backup_and_calibration();
  test_rx5808_backend();
  test_openpocket_board_power();
  test_amt630a_flash_controller();
  test_openpocket_product_services();
  test_complete_openpocket_menu();
  test_at7456e_backend();

  if (failures != 0) {
    std::cerr << failures << " of " << checks << " checks failed\n";
    return 1;
  }
  std::cout << "all " << checks << " checks passed\n";
  return 0;
}
