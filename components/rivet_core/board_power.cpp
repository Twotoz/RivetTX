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
}

bool BoardPowerController::initialize(bool simulator_mode)
{
  if (initialized_) {
    return set_simulator_mode(simulator_mode);
  }
  if (!hardware_.initialize() || !hardware_.set_backlight(0) ||
      !hardware_.set_elrs_5v(false) || !hardware_.set_video_5v(false) ||
      !hardware_.set_display_5v(false) ||
      !hardware_.set_amt630a_reset(true) ||
      !hardware_.set_amt630a_flash_owner(false)) {
    return false;
  }
  initialized_ = true;
  status_.amt630a_reset_asserted = true;
  status_.simulator_mode = simulator_mode;
  return true;
}

bool BoardPowerController::request_video(bool enabled)
{
  if (!initialized_ || status_.amt630a_flash_owned_by_esp ||
      (status_.simulator_mode && enabled)) {
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
  if (!initialized_ || status_.amt630a_flash_owned_by_esp) {
    return false;
  }
  backlight_percent = std::min<uint8_t>(100, backlight_percent);
  if (!enabled) {
    if (!hardware_.set_backlight(0) ||
        !hardware_.set_amt630a_reset(true) ||
        !hardware_.set_display_5v(false)) {
      return false;
    }
    status_.backlight_percent = 0;
    status_.amt630a_reset_asserted = true;
    status_.display_5v = false;
    return true;
  }
  if (!hardware_.set_display_5v(true) ||
      !hardware_.set_amt630a_reset(false) ||
      !hardware_.set_backlight(backlight_percent)) {
    (void)hardware_.set_backlight(0);
    (void)hardware_.set_amt630a_reset(true);
    (void)hardware_.set_display_5v(false);
    return false;
  }
  status_.display_5v = true;
  status_.amt630a_reset_asserted = false;
  status_.backlight_percent = backlight_percent;
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

bool BoardPowerController::enter_amt630a_flash_mode()
{
  if (!initialized_ || status_.amt630a_flash_owned_by_esp ||
      !hardware_.set_backlight(0) ||
      !hardware_.set_amt630a_reset(true) ||
      !hardware_.set_amt630a_flash_owner(true)) {
    return false;
  }
  status_.backlight_percent = 0;
  status_.amt630a_reset_asserted = true;
  status_.amt630a_flash_owned_by_esp = true;
  return true;
}

bool BoardPowerController::leave_amt630a_flash_mode()
{
  if (!initialized_ || !status_.amt630a_flash_owned_by_esp ||
      !hardware_.set_amt630a_flash_owner(false)) {
    return false;
  }
  status_.amt630a_flash_owned_by_esp = false;
  if (!hardware_.set_amt630a_reset(false)) {
    return false;
  }
  status_.amt630a_reset_asserted = false;
  return true;
}

void BoardPowerController::tick(TimeUs now_us)
{
  if (!initialized_ || now_us < next_sample_us_) {
    return;
  }
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
