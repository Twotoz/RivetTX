#include "rivettx/board_power.hpp"

#include <algorithm>

namespace rivettx {

BoardPowerController::BoardPowerController(IBoardPowerHardware& hardware,
                                           BoardPowerConfig config)
    : hardware_(hardware), config_(config)
{
  config_.sample_interval_ms =
      std::max<uint32_t>(20, config_.sample_interval_ms);
  config_.default_backlight_percent =
      std::min<uint8_t>(100, config_.default_backlight_percent);
  config_.display_power_settle_ms =
      std::max<uint16_t>(1, config_.display_power_settle_ms);
}

bool BoardPowerController::initialize(bool simulator_mode)
{
  if (initialized_) {
    return set_simulator_mode(simulator_mode);
  }
  if (!hardware_.initialize() || !hardware_.set_backlight(0) ||
      !hardware_.set_elrs_5v(false) || !hardware_.set_video_5v(false) ||
      !hardware_.set_display_5v(false) ||
      !hardware_.set_display_controller_reset(true)) {
    return false;
  }
  initialized_ = true;
  status_.display_controller_reset_asserted = true;
  status_.simulator_mode = simulator_mode;
  return true;
}

bool BoardPowerController::request_video(bool enabled)
{
  if (!initialized_ || (status_.simulator_mode && enabled)) {
    return false;
  }
  if (!hardware_.set_video_5v(enabled)) {
    return false;
  }
  status_.video_5v = enabled;
  return true;
}

bool BoardPowerController::request_display(bool enabled,
                                           uint8_t backlight_percent)
{
  if (!initialized_) {
    return false;
  }
  backlight_percent = std::min<uint8_t>(100, backlight_percent);
  requested_backlight_percent_ = backlight_percent;
  if (!enabled) {
    if (!hardware_.set_backlight(0) ||
        !hardware_.set_display_controller_reset(true) ||
        !hardware_.set_display_5v(false)) {
      return false;
    }
    status_.backlight_percent = 0;
    status_.display_controller_reset_asserted = true;
    status_.display_5v = false;
    status_.display_state = DisplayPowerState::Off;
    display_controller_ready_ = false;
    display_deadline_us_ = 0;
    return true;
  }
  if (status_.display_5v) {
    if (status_.display_state == DisplayPowerState::Ready &&
        hardware_.set_backlight(backlight_percent)) {
      status_.backlight_percent = backlight_percent;
      return true;
    }
    return status_.display_state != DisplayPowerState::Fault;
  }
  if (!hardware_.set_backlight(0) ||
      !hardware_.set_display_controller_reset(true) ||
      !hardware_.set_display_5v(true)) {
    (void)hardware_.set_backlight(0);
    (void)hardware_.set_display_controller_reset(true);
    (void)hardware_.set_display_5v(false);
    return false;
  }
  status_.display_5v = true;
  status_.display_controller_reset_asserted = true;
  status_.backlight_percent = 0;
  status_.display_state = DisplayPowerState::PowerSettling;
  display_deadline_us_ = 0;
  return true;
}

bool BoardPowerController::request_elrs(bool enabled)
{
  if (!initialized_ || (status_.simulator_mode && enabled) ||
      !hardware_.set_elrs_5v(enabled)) {
    return false;
  }
  status_.elrs_5v = enabled;
  return true;
}

bool BoardPowerController::set_simulator_mode(bool enabled)
{
  if (!initialized_) {
    return false;
  }
  if (enabled && (!hardware_.set_elrs_5v(false) ||
                  !hardware_.set_video_5v(false))) {
    return false;
  }
  status_.simulator_mode = enabled;
  if (enabled) {
    status_.elrs_5v = false;
    status_.video_5v = false;
  }
  return true;
}

void BoardPowerController::set_display_controller_ready(bool ready)
{
  display_controller_ready_ = ready;
  if (!ready && status_.display_state == DisplayPowerState::Ready) {
    if (hardware_.set_backlight(0)) {
      status_.backlight_percent = 0;
      status_.display_state = DisplayPowerState::WaitingForController;
    } else {
      status_.display_state = DisplayPowerState::Fault;
    }
  }
}

void BoardPowerController::tick(TimeUs now_us)
{
  if (!initialized_) {
    return;
  }
  if (status_.display_state == DisplayPowerState::PowerSettling) {
    if (display_deadline_us_ == 0) {
      display_deadline_us_ = now_us +
          static_cast<TimeUs>(config_.display_power_settle_ms) * 1000U;
    } else if (now_us >= display_deadline_us_) {
      if (hardware_.set_display_controller_reset(false)) {
        status_.display_controller_reset_asserted = false;
        status_.display_state = DisplayPowerState::WaitingForController;
      } else {
        status_.display_state = DisplayPowerState::Fault;
      }
    }
  }
  if (status_.display_state == DisplayPowerState::WaitingForController &&
      display_controller_ready_) {
    if (hardware_.set_backlight(requested_backlight_percent_)) {
      status_.backlight_percent = requested_backlight_percent_;
      status_.display_state = DisplayPowerState::Ready;
    } else {
      status_.display_state = DisplayPowerState::Fault;
    }
  }
  if (now_us < next_sample_us_) return;
  bool vbus = false;
  status_.vbus_present = hardware_.read_vbus_present(vbus) && vbus;
  status_.charger = hardware_.read_charger();
  status_.fuel_gauge = hardware_.read_fuel_gauge();
  if (status_.charger.state == BoardSensorState::Valid) {
    status_.vbus_present = status_.vbus_present ||
                           status_.charger.vbus_present;
  }
  next_sample_us_ = now_us +
      static_cast<TimeUs>(config_.sample_interval_ms) * 1000U;
}

const BoardPowerStatus& BoardPowerController::status() const
{
  return status_;
}

}  // namespace rivettx
