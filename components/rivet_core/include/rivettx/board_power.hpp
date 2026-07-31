#pragma once

#include "rivettx/product.hpp"
#include "rivettx/types.hpp"

#include <cstdint>

namespace rivettx {

enum class BoardSensorState : uint8_t {
  Unavailable,
  Valid,
  Fault,
};

struct ChargerTelemetry {
  BoardSensorState state = BoardSensorState::Unavailable;
  ChargeState charge = ChargeState::Unknown;
  uint16_t battery_mv = 0;
  bool vbus_present = false;
  bool battery_present = false;
  bool thermal_regulation = false;
};

struct FuelGaugeTelemetry {
  BoardSensorState state = BoardSensorState::Unavailable;
  uint16_t cell_mv = 0;
  uint8_t state_of_charge = 0;
  bool alert = false;
};

struct BoardPowerStatus {
  ChargerTelemetry charger{};
  FuelGaugeTelemetry fuel_gauge{};
  bool vbus_present = false;
  bool video_5v = false;
  bool display_5v = false;
  bool elrs_5v = false;
  bool display_controller_reset_asserted = true;
  bool simulator_mode = false;
  uint8_t backlight_percent = 0;
};

class IBoardPowerHardware {
 public:
  virtual ~IBoardPowerHardware() = default;
  virtual bool initialize() = 0;
  virtual bool set_video_5v(bool enabled) = 0;
  virtual bool set_display_5v(bool enabled) = 0;
  virtual bool set_elrs_5v(bool enabled) = 0;
  virtual bool set_backlight(uint8_t percent) = 0;
  virtual bool set_display_controller_reset(bool asserted) = 0;
  virtual bool read_vbus_present(bool& present) = 0;
  virtual ChargerTelemetry read_charger() = 0;
  virtual FuelGaugeTelemetry read_fuel_gauge() = 0;
};

struct BoardPowerConfig {
  uint32_t sample_interval_ms = 100;
  uint8_t default_backlight_percent = 60;
};

// Runs only from a service task. Every operation is bounded to one hardware
// call and no method contains a delay or retry loop.
class BoardPowerController {
 public:
  explicit BoardPowerController(IBoardPowerHardware& hardware,
                                BoardPowerConfig config = {});

  bool initialize(bool simulator_mode = false);
  bool request_video(bool enabled);
  bool request_display(bool enabled, uint8_t backlight_percent = 60);
  bool request_elrs(bool enabled);
  bool set_simulator_mode(bool enabled);
  void tick(TimeUs now_us);
  const BoardPowerStatus& status() const;

 private:
  IBoardPowerHardware& hardware_;
  BoardPowerConfig config_{};
  BoardPowerStatus status_{};
  TimeUs next_sample_us_ = 0;
  bool initialized_ = false;
};

}  // namespace rivettx
