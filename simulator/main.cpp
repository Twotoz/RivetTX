#include "rivettx/core.hpp"
#include "rivettx/crsf.hpp"
#include "rivettx/services.hpp"
#include "rivettx/storage.hpp"
#include "rivettx/ui.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace rivettx;

class SimulatorWatchdog final : public IWatchdog {
 public:
  void kick() override
  {
    ++kicks;
  }
  uint32_t kicks = 0;
};

class SimulatorDisplay final : public IDisplaySink {
 public:
  explicit SimulatorDisplay(std::string output)
      : output_(std::move(output))
  {
  }

  const DisplayCapabilities& capabilities() const override
  {
    return capabilities_;
  }

  bool flush(const MonoCanvas& canvas) override
  {
    std::ofstream file(output_, std::ios::binary | std::ios::trunc);
    if (!file) {
      return false;
    }
    file << "P4\n" << canvas.width() << " " << canvas.height() << "\n";
    for (uint16_t y = 0; y < canvas.height(); ++y) {
      uint8_t byte = 0;
      uint8_t bits = 0;
      for (uint16_t x = 0; x < canvas.width(); ++x) {
        byte = static_cast<uint8_t>((byte << 1U) |
                                    (canvas.pixel_at(x, y) ? 1U : 0U));
        ++bits;
        if (bits == 8) {
          file.put(static_cast<char>(byte));
          byte = 0;
          bits = 0;
        }
      }
      if (bits != 0) {
        file.put(static_cast<char>(byte << (8U - bits)));
      }
    }
    return file.good();
  }

 private:
  DisplayCapabilities capabilities_{};
  std::string output_;
};

class SimulatorTransport final : public ICrsfTransport {
 public:
  bool write(const uint8_t* data, std::size_t size) override
  {
    last_frame.assign(data, data + size);
    ++writes;
    return true;
  }

  std::size_t read(uint8_t*, std::size_t) override
  {
    return 0;
  }

  void set_baud_rate(uint32_t baud) override
  {
    baud_rate = baud;
  }

  void reset_module() override
  {
    ++resets;
  }

  std::vector<uint8_t> last_frame;
  uint32_t baud_rate = 0;
  uint32_t writes = 0;
  uint32_t resets = 0;
};

}  // namespace

int main()
{
  using namespace rivettx;

  std::filesystem::create_directories("build/sim-data");
  PosixFileStore files("build/sim-data");
  TransactionalModelStore models(files, "default.rvm");
  Model model = make_default_model();
  std::string storage_error;
  if (!models.save(model, 1, storage_error)) {
    std::cerr << "model save failed: " << storage_error << "\n";
    return 1;
  }

  Model loaded{};
  const auto loaded_result = models.load(loaded);
  if (!loaded_result.success) {
    std::cerr << "model load failed: " << loaded_result.error << "\n";
    return 1;
  }

  InputProcessor input_processor;
  MixerEngine mixer;
  SafetyManager safety;
  TelemetryRegistry telemetry;
  SimulatorWatchdog watchdog;
  ControlLoop control(input_processor, mixer, safety, telemetry, watchdog);
  DiagnosticLog diagnostics;
  CrsfParser parser(telemetry);
  SimulatorTransport transport;
  ModuleSupervisor module(transport, parser, diagnostics);

  safety.boot_complete(true, false);
  safety.request_enable();
  module.start(loaded.model_id, 0);

  ChannelFrame latest{};
  constexpr TimeUs period_us = 4000;
  for (uint32_t cycle = 0; cycle < 1000; ++cycle) {
    const TimeUs now = static_cast<TimeUs>(cycle) * period_us;
    RawInputs raw{};
    raw.valid = true;
    raw.sampled_at_us = now;
    raw.axes[0] = static_cast<int16_t>(
        2048 + std::sin(static_cast<double>(cycle) / 50.0) * 1700);
    raw.axes[1] = 2048;
    raw.axes[2] = cycle < 100 ? 100 : 2048;
    raw.axes[3] = 2048;
    const auto result =
        control.run(loaded, raw, 3800, now, now + 250);
    latest = result.frame;
    (void)module.send_channels(latest, now + 300);
    module.poll(now + 350);
  }

  telemetry.update(crsf::SensorUplinkLinkQuality, 96,
                   TelemetryUnit::Percent, 4000000);
  SimulatorDisplay display("build/sim-screen.pbm");
  MonoCanvas canvas(display.capabilities().width,
                    display.capabilities().height);
  UiController ui(display, canvas);
  ui.set_screen(make_main_screen(
      loaded, latest, 3800, 96,
      safety.status().state == SafetyState::Enabled));
  if (!ui.render()) {
    std::cerr << "display render failed\n";
    return 1;
  }
  std::cout << "RivetTX simulator\n"
            << "model generation: " << loaded_result.generation << "\n"
            << "control cycles: " << watchdog.kicks << "\n"
            << "CRSF frames: " << transport.writes << "\n"
            << "safety state: "
            << static_cast<int>(safety.status().state) << "\n"
            << "screen: build/sim-screen.pbm\n";
  return 0;
}
