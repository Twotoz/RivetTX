#include "rivettx/product.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace rivettx {

VrxController::VrxController(IVrxHardware& hardware,
                             uint32_t scan_dwell_ms)
    : VrxController(hardware, VrxControllerConfig{})
{
  config_.scan_dwell_ms = std::max<uint32_t>(20, scan_dwell_ms);
}

VrxController::VrxController(IVrxHardware& hardware,
                             VrxControllerConfig config)
    : hardware_(hardware), config_(config)
{
  config_.scan_dwell_ms = std::max<uint32_t>(20, config_.scan_dwell_ms);
  config_.tune_timeout_ms =
      std::max<uint32_t>(10, config_.tune_timeout_ms);
  config_.rssi_sample_interval_ms =
      std::max<uint32_t>(5, config_.rssi_sample_interval_ms);
  config_.rssi_stale_ms = std::max<uint32_t>(
      config_.rssi_sample_interval_ms, config_.rssi_stale_ms);
  config_.rssi_filter_shift =
      static_cast<uint8_t>(clamp<int32_t>(0, config_.rssi_filter_shift, 8));
  config_.rssi_hysteresis_percent = static_cast<uint8_t>(
      clamp<int32_t>(0, config_.rssi_hysteresis_percent, 20));
  rssi_calibration_valid_ =
      config_.rssi_min_adc >= 0 &&
      config_.rssi_max_adc > config_.rssi_min_adc &&
      config_.rssi_max_adc <= 4095;
  if (!rssi_calibration_valid_) {
    status_.rssi_state = VrxRssiState::SensorFault;
    status_.failure = VrxFailure::RssiCalibration;
  }
}

bool VrxController::select(uint8_t band, uint8_t channel, TimeUs now_us)
{
  status_.scanning = false;
  finishing_scan_ = false;
  restoring_after_scan_ = false;
  scan_result_pending_ = false;
  hardware_.cancel_tune();
  return request_tune(band, channel, now_us);
}

bool VrxController::begin_scan(TimeUs now_us)
{
  if (status_.scanning) {
    return false;
  }
  scan_origin_band_ = status_.band;
  scan_origin_channel_ = status_.channel;
  scan_index_ = 0;
  best_index_ = 0;
  best_rssi_ = INT16_MIN;
  status_.scanning = true;
  finishing_scan_ = false;
  restoring_after_scan_ = false;
  scan_result_pending_ = false;
  hardware_.cancel_tune();
  return tune_scan_candidate(now_us);
}

bool VrxController::cancel_scan(TimeUs now_us)
{
  if (!status_.scanning) {
    return false;
  }
  status_.scanning = false;
  finishing_scan_ = false;
  restoring_after_scan_ = true;
  hardware_.cancel_tune();
  return request_tune(scan_origin_band_, scan_origin_channel_, now_us);
}

void VrxController::set_video_signal(bool present, bool fresh)
{
  status_.signal_fresh = fresh;
  status_.video_signal = fresh && present;
}

bool VrxController::request_tune(uint8_t band, uint8_t channel,
                                 TimeUs now_us)
{
  const uint16_t frequency = vrx_frequency_mhz(band, channel);
  if (frequency == 0 || !vrx_frequency_supported(frequency)) {
    fail_tune(VrxFailure::InvalidFrequency);
    return false;
  }
  status_.band = band;
  status_.channel = channel;
  status_.frequency_mhz = frequency;
  if (!hardware_.start_tune(frequency, now_us)) {
    fail_tune(VrxFailure::TuneRejected);
    return false;
  }
  requested_band_ = band;
  requested_channel_ = channel;
  status_.tuning = true;
  status_.available = false;
  status_.failure = VrxFailure::None;
  if (status_.rssi_state == VrxRssiState::Valid) {
    status_.rssi_state = VrxRssiState::Stale;
  }
  tune_deadline_us_ =
      now_us + static_cast<TimeUs>(config_.tune_timeout_ms) * 1000U;
  return true;
}

bool VrxController::tune_scan_candidate(TimeUs now_us)
{
  if (scan_index_ >= kVrxBandCount * kVrxChannelsPerBand) {
    finish_scan(now_us);
    return status_.tuning;
  }
  const uint8_t band =
      static_cast<uint8_t>(scan_index_ / kVrxChannelsPerBand);
  const uint8_t channel =
      static_cast<uint8_t>(scan_index_ % kVrxChannelsPerBand);
  return request_tune(band, channel, now_us);
}

void VrxController::tick(TimeUs now_us)
{
  hardware_.tick(now_us);
  if (status_.tuning) {
    const VrxTuneState tune_state = hardware_.tune_state();
    if (tune_state == VrxTuneState::Complete) {
      finish_tune(now_us);
    } else if (tune_state == VrxTuneState::Failed) {
      fail_tune(VrxFailure::Communication);
    } else if (now_us >= tune_deadline_us_) {
      hardware_.cancel_tune();
      fail_tune(VrxFailure::TuneTimeout);
    }
  }

  if (status_.scanning && !status_.tuning &&
      !finishing_scan_ && !restoring_after_scan_ &&
      now_us >= next_scan_step_us_) {
    (void)sample_rssi(now_us, true);
    ++scan_index_;
    (void)tune_scan_candidate(now_us);
  } else if (!status_.scanning && !status_.tuning &&
             status_.available && now_us >= next_sample_us_) {
    (void)sample_rssi(now_us, false);
    next_sample_us_ =
        now_us +
        static_cast<TimeUs>(config_.rssi_sample_interval_ms) * 1000U;
  }

  if (status_.rssi_state == VrxRssiState::Valid &&
      last_sample_us_ != 0 &&
      now_us - last_sample_us_ >
          static_cast<TimeUs>(config_.rssi_stale_ms) * 1000U) {
    status_.rssi_state = VrxRssiState::Stale;
  }
}

const VrxStatus& VrxController::status() const
{
  return status_;
}

bool VrxController::take_scan_result(uint8_t& band, uint8_t& channel)
{
  if (!scan_result_pending_) {
    return false;
  }
  band = status_.band;
  channel = status_.channel;
  scan_result_pending_ = false;
  return true;
}

void VrxController::finish_tune(TimeUs now_us)
{
  status_.tuning = false;
  status_.available = true;
  status_.failure = VrxFailure::None;
  status_.band = requested_band_;
  status_.channel = requested_channel_;
  status_.frequency_mhz =
      vrx_frequency_mhz(requested_band_, requested_channel_);
  if (status_.scanning && finishing_scan_) {
    status_.scanning = false;
    finishing_scan_ = false;
    scan_result_pending_ = true;
    next_sample_us_ = now_us;
  } else if (restoring_after_scan_) {
    status_.scanning = false;
    restoring_after_scan_ = false;
    next_sample_us_ = now_us;
  } else if (status_.scanning) {
    next_scan_step_us_ =
        now_us + static_cast<TimeUs>(config_.scan_dwell_ms) * 1000U;
  } else {
    next_sample_us_ = now_us;
  }
}

void VrxController::fail_tune(VrxFailure failure)
{
  status_.tuning = false;
  status_.available = false;
  status_.scanning = false;
  status_.failure = failure;
  finishing_scan_ = false;
  restoring_after_scan_ = false;
}

bool VrxController::sample_rssi(TimeUs now_us, bool scan_sample)
{
  if (!rssi_calibration_valid_) {
    status_.rssi_state = VrxRssiState::SensorFault;
    status_.failure = VrxFailure::RssiCalibration;
    return false;
  }
  const VrxAdcSample sample = hardware_.sample_rssi();
  if (sample.state == VrxAdcSampleState::Unavailable) {
    status_.rssi_state = VrxRssiState::Unavailable;
    return false;
  }
  if (sample.state == VrxAdcSampleState::SensorFault || sample.raw < 0 ||
      sample.raw > 4095) {
    status_.rssi_state = VrxRssiState::SensorFault;
    return false;
  }

  const bool first_filtered_sample = !filter_initialized_;
  if (first_filtered_sample) {
    filtered_rssi_adc_ = sample.raw;
    filter_initialized_ = true;
  } else {
    const int32_t divisor = 1 << config_.rssi_filter_shift;
    filtered_rssi_adc_ +=
        (static_cast<int32_t>(sample.raw) - filtered_rssi_adc_) / divisor;
  }
  status_.rssi = static_cast<int16_t>(filtered_rssi_adc_);
  const int32_t calibrated = clamp<int32_t>(
      0,
      (filtered_rssi_adc_ - config_.rssi_min_adc) * 100 /
          (config_.rssi_max_adc - config_.rssi_min_adc),
      100);
  const int32_t difference =
      std::abs(calibrated - static_cast<int32_t>(status_.strength_percent));
  if (first_filtered_sample ||
      difference >= config_.rssi_hysteresis_percent) {
    status_.strength_percent = static_cast<uint8_t>(calibrated);
  }
  status_.rssi_state = VrxRssiState::Valid;
  last_sample_us_ = now_us;
  if (scan_sample && sample.raw > best_rssi_) {
    best_rssi_ = sample.raw;
    best_index_ = scan_index_;
  }
  return true;
}

void VrxController::finish_scan(TimeUs now_us)
{
  hardware_.cancel_tune();
  if (best_rssi_ != INT16_MIN) {
    finishing_scan_ = true;
    const uint8_t band =
        static_cast<uint8_t>(best_index_ / kVrxChannelsPerBand);
    const uint8_t channel =
        static_cast<uint8_t>(best_index_ % kVrxChannelsPerBand);
    if (!request_tune(band, channel, now_us)) {
      finishing_scan_ = false;
    }
    return;
  }
  restoring_after_scan_ = true;
  if (!request_tune(scan_origin_band_, scan_origin_channel_, now_us)) {
    restoring_after_scan_ = false;
  }
}

char CharacterOsdFrame::at(std::size_t column, std::size_t row) const
{
  return column < kOsdColumns && row < kOsdRows
             ? cells[row * kOsdColumns + column]
             : '\0';
}

UiScreen make_openpocket_home_screen(const Model& model,
                                     const UiHomeStatus& home)
{
  UiScreen screen{};
  screen.id = "openpocket.home";
  screen.title = model.name.data();
  screen.scrollable = false;
  screen.kind = UiScreenKind::Home;
  screen.home = home;
  return screen;
}

UiScreen make_openpocket_main_menu_screen()
{
  UiScreen screen{"openpocket.menu", "OpenPocket", {}};
  const std::array<std::pair<const char*, const char*>, 7> groups{{
      {"group.model", "MODEL"},
      {"group.radio", "RADIO"},
      {"group.elrs", "EXPRESSLRS"},
      {"group.video", "VIDEO"},
      {"group.usb", "USB"},
      {"group.diagnostics", "DIAGNOSTICS"},
      {"group.system", "SYSTEM"},
  }};
  for (const auto& group : groups) {
    screen.fields.push_back(
        {group.first, group.second, "OPEN", UiFieldKind::Action,
         0, 0, 1, false, true});
  }
  return screen;
}

UiScreen make_openpocket_group_menu_screen(OpenPocketMenuGroup group)
{
  UiScreen screen{};
  const auto add_action = [&screen](const char* id, const char* label) {
    screen.fields.push_back(
        {id, label, "OPEN", UiFieldKind::Action,
         0, 0, 1, false, true});
  };
  switch (group) {
    case OpenPocketMenuGroup::Model:
      screen = {"openpocket.group.model", "Model", {}};
      add_action("models", "MODELS");
      add_action("model", "MODEL SETUP");
      add_action("inputs", "INPUTS");
      add_action("mixes", "MIXES");
      add_action("limits", "OUTPUT LIMITS");
      add_action("flight_modes", "FLIGHT MODES");
      add_action("curves", "CURVES");
      add_action("logical", "LOGICAL SWITCHES");
      add_action("special", "SPECIAL FUNCTIONS");
      add_action("timers", "TIMERS");
      break;
    case OpenPocketMenuGroup::Radio:
      screen = {"openpocket.group.radio", "Radio", {}};
      add_action("outputs", "CHANNEL OUTPUTS");
      add_action("power", "POWER");
      break;
    case OpenPocketMenuGroup::Elrs:
      screen = {"openpocket.group.elrs", "ExpressLRS", {}};
      add_action("elrs", "ELRS SETTINGS");
      add_action("finder", "ELRS FINDER");
      break;
    case OpenPocketMenuGroup::Video:
      screen = {"openpocket.group.video", "Video", {}};
      add_action("video", "VIDEO RECEIVER");
      break;
    case OpenPocketMenuGroup::Usb:
      screen = {"openpocket.group.usb", "USB", {}};
      add_action("usb", "USB SIMULATOR");
      break;
    case OpenPocketMenuGroup::Diagnostics:
      screen = {"openpocket.group.diagnostics", "Diagnostics", {}};
      add_action("warnings", "WARNINGS");
      add_action("telemetry", "TELEMETRY");
      break;
    case OpenPocketMenuGroup::System:
      screen = {"openpocket.group.system", "System", {}};
      add_action("web", "WEB CONFIG");
      add_action("system", "RADIO SYSTEM");
      break;
  }
  return screen;
}

UiScreen make_openpocket_video_screen(const VrxStatus& vrx)
{
  UiScreen screen{"video", "Video receiver", {}};
  const char* rssi_text = "UNAVAILABLE";
  switch (vrx.rssi_state) {
    case VrxRssiState::Valid:
      break;
    case VrxRssiState::Stale:
      rssi_text = "STALE";
      break;
    case VrxRssiState::SensorFault:
      rssi_text = "SENSOR FAULT";
      break;
    case VrxRssiState::Unavailable:
      break;
  }
  screen.fields.push_back(
      {"band", "BAND", "", UiFieldKind::Choice,
       static_cast<int32_t>(vrx.band + 1), 1,
       static_cast<int32_t>(kVrxBandCount), true, true});
  screen.fields.push_back(
      {"channel", "CHANNEL", "", UiFieldKind::Choice,
       static_cast<int32_t>(vrx.channel + 1), 1,
       static_cast<int32_t>(kVrxChannelsPerBand), true, true});
  screen.fields.push_back(
      {"frequency", "FREQUENCY",
       vrx.frequency_mhz == 0
           ? "NOT TUNED"
           : std::to_string(vrx.frequency_mhz) + "MHZ",
       UiFieldKind::Label, vrx.frequency_mhz, 0, 0, false, true});
  screen.fields.push_back(
      {"signal", "VIDEO",
       !vrx.signal_fresh
           ? "UNKNOWN"
           : (vrx.video_signal ? "SIGNAL OK" : "NO SIGNAL"),
       UiFieldKind::Label, vrx.video_signal ? 1 : 0, 0, 1, false, true});
  screen.fields.push_back(
      {"rssi", "VRX STRENGTH",
       vrx.rssi_state == VrxRssiState::Valid
           ? std::to_string(vrx.strength_percent) + "%"
           : rssi_text,
       UiFieldKind::Progress,
       vrx.rssi_state == VrxRssiState::Valid ? vrx.strength_percent : 0,
       0, 100, false, true});
  screen.fields.push_back(
      {"scan", vrx.scanning ? "CANCEL SCAN" : "START SCAN",
       vrx.tuning ? "TUNING" : "ENTER", UiFieldKind::Action,
       0, 0, 1, false, true});
  return screen;
}

bool openpocket_warning_is_persistent(UiWarningCode warning)
{
  switch (warning) {
    case UiWarningCode::StorageInvalid:
    case UiWarningCode::CalibrationRequired:
    case UiWarningCode::InputInvalid:
    case UiWarningCode::InputStale:
    case UiWarningCode::ThrottleHigh:
    case UiWarningCode::ArmSwitch:
    case UiWarningCode::SwitchPosition:
    case UiWarningCode::MixerDeadline:
    case UiWarningCode::WatchdogRecovery:
    case UiWarningCode::WatchdogUnavailable:
    case UiWarningCode::BatteryCritical:
    case UiWarningCode::BatterySensor:
    case UiWarningCode::ModuleOffline:
    case UiWarningCode::LinkLost:
    case UiWarningCode::LinkCritical:
      return true;
    case UiWarningCode::None:
    case UiWarningCode::BatteryLow:
    case UiWarningCode::LinkWeak:
    case UiWarningCode::LoggingFailed:
    case UiWarningCode::ModelUnsaved:
    case UiWarningCode::Maintenance:
    case UiWarningCode::VideoNoSignal:
      return false;
  }
  return false;
}

void CharacterOsdComposer::clear()
{
  frame_.cells.fill(' ');
}

void CharacterOsdComposer::text(std::size_t column, std::size_t row,
                                const char* value)
{
  if (row >= kOsdRows || value == nullptr) {
    return;
  }
  for (std::size_t index = 0;
       value[index] != '\0' && column + index < kOsdColumns; ++index) {
    frame_.cells[row * kOsdColumns + column + index] = value[index];
  }
}

void CharacterOsdComposer::text(std::size_t column, std::size_t row,
                                const std::string& value,
                                std::size_t maximum)
{
  if (row >= kOsdRows || column >= kOsdColumns) {
    return;
  }
  const std::size_t count =
      std::min({value.size(), maximum, kOsdColumns - column});
  for (std::size_t index = 0; index < count; ++index) {
    frame_.cells[row * kOsdColumns + column + index] = value[index];
  }
}

void CharacterOsdComposer::center_text(std::size_t row,
                                       const std::string& value)
{
  const std::size_t length = std::min(value.size(), kOsdColumns);
  text((kOsdColumns - length) / 2, row, value, length);
}

void CharacterOsdComposer::right_text(std::size_t row, const char* value)
{
  if (row >= kOsdRows || value == nullptr) {
    return;
  }
  const std::size_t length = std::min<std::size_t>(
      std::strlen(value), kOsdColumns);
  text(kOsdColumns - length, row, value);
}

void CharacterOsdComposer::number(std::size_t column, std::size_t row,
                                  int32_t value)
{
  char buffer[16]{};
  (void)std::snprintf(buffer, sizeof(buffer), "%ld",
                      static_cast<long>(value));
  text(column, row, buffer);
}

void CharacterOsdComposer::compose(const Model& model,
                                   const UiHomeStatus& home,
                                   const VrxStatus& vrx)
{
  const UiScreen screen = make_openpocket_home_screen(model, home);
  compose(screen, home, vrx, 0, 0, false);
}

void CharacterOsdComposer::compose(
    const UiScreen& screen, const UiHomeStatus& home,
    const VrxStatus& vrx, std::size_t selected_index,
    std::size_t scroll_offset, bool editing)
{
  clear();
  if (screen.kind == UiScreenKind::Home) {
    compose_home(screen, home, vrx);
  } else {
    compose_list(screen, home, selected_index, scroll_offset, editing);
  }
}

void CharacterOsdComposer::compose_home(
    const UiScreen& screen, const UiHomeStatus& home,
    const VrxStatus& vrx)
{
  (void)screen;
  text(0, 0, "BAT ");
  if (home.battery_percent_valid) {
    number(4, 0, home.battery_percent);
    text(home.battery_percent >= 100 ? 7 : (home.battery_percent >= 10 ? 6 : 5),
         0, "%");
  } else {
    number(4, 0, home.battery_mv);
    text(8, 0, "MV");
  }

  char link[12]{};
  if (home.module_online) {
    (void)std::snprintf(link, sizeof(link), "LQ %u%%",
                        static_cast<unsigned>(home.link_quality));
  } else {
    (void)std::snprintf(link, sizeof(link), "LQ --");
  }
  right_text(0, link);

  if (home.warning_count != 0) {
    center_text(7, std::string("! ") +
                       ui_warning_text(home.warnings[0]) + " !");
    if (home.warning_count > 1) {
      center_text(8, "+" + std::to_string(home.warning_count - 1) +
                         " MORE");
    }
  }

  text(0, 12, home.channels[4] > 0 ? "ARMED" : "DISARMED");
  char vrx_channel[16]{};
  if (vrx.available) {
    (void)std::snprintf(vrx_channel, sizeof(vrx_channel), "VRX B%u CH%u",
                        static_cast<unsigned>(vrx.band + 1),
                        static_cast<unsigned>(vrx.channel + 1));
  } else {
    (void)std::snprintf(vrx_channel, sizeof(vrx_channel), "VRX --");
  }
  right_text(12, vrx_channel);
}

void CharacterOsdComposer::compose_list(
    const UiScreen& screen, const UiHomeStatus& home,
    std::size_t selected_index, std::size_t scroll_offset,
    bool editing)
{
  constexpr std::size_t kVisibleRows = kOpenPocketListRows;
  text(0, 0, screen.title, 20);
  right_text(0, editing ? "EDIT"
                       : (home.outputs_enabled ? "LIVE" : "SAFE"));
  text(0, 1, "------------------------------");

  const std::size_t visible_count = static_cast<std::size_t>(
      std::count_if(screen.fields.begin(), screen.fields.end(),
                    [](const UiField& field) { return field.visible; }));
  std::size_t visible_index = 0;
  std::size_t rendered = 0;
  std::size_t selected_visible = 0;
  for (std::size_t index = 0; index < screen.fields.size(); ++index) {
    const auto& field = screen.fields[index];
    if (!field.visible) {
      continue;
    }
    if (index == selected_index) {
      selected_visible = visible_index;
    }
    if (visible_index++ < scroll_offset || rendered >= kVisibleRows) {
      continue;
    }

    const std::size_t row = 2 + rendered++;
    frame_.cells[row * kOsdColumns] =
        index == selected_index ? (editing ? '*' : '>') : ' ';
    text(2, row, field.label, 17);
    if (!field.value_text.empty()) {
      text(20, row, field.value_text, 10);
    } else if (field.kind == UiFieldKind::Boolean) {
      text(25, row, field.value != 0 ? "ON" : "OFF");
    } else if (field.kind == UiFieldKind::Number ||
               field.kind == UiFieldKind::Choice ||
               field.kind == UiFieldKind::Progress) {
      number(24, row, field.value);
    }
  }

  if (scroll_offset > 0) {
    frame_.cells[2 * kOsdColumns + 29] = '^';
  }
  if (scroll_offset + kVisibleRows < visible_count) {
    frame_.cells[11 * kOsdColumns + 29] = 'v';
  }
  char position[24]{};
  (void)std::snprintf(
      position, sizeof(position), "%u/%u",
      static_cast<unsigned>(visible_count == 0 ? 0 : selected_visible + 1),
      static_cast<unsigned>(visible_count));
  text(0, 12, position);
  right_text(12, home.module_online ? "ELRS OK" : "ELRS LOST");
  text(0, 15,
       editing ? "ROTATE CHANGE ENTER SAVE"
               : "ENTER SELECT  BACK");
}

const CharacterOsdFrame& CharacterOsdComposer::frame() const
{
  return frame_;
}

void CharacterOsdUi::set_screen(UiScreen screen)
{
  const bool same_screen = screen.id == screen_.id;
  std::string selected_id;
  int32_t staged_value = 0;
  std::string staged_text;
  const bool preserve_edit =
      same_screen && editing_ && selected_index_ < screen_.fields.size() &&
      screen_.fields[selected_index_].editable;
  if (same_screen && selected_index_ < screen_.fields.size()) {
    selected_id = screen_.fields[selected_index_].id;
    if (preserve_edit) {
      staged_value = screen_.fields[selected_index_].value;
      staged_text = screen_.fields[selected_index_].value_text;
    }
  }
  screen_ = std::move(screen);
  if (screen_.kind == UiScreenKind::Home) {
    home_ = screen_.home;
  }
  selected_index_ = 0;
  if (same_screen && !selected_id.empty()) {
    for (std::size_t index = 0; index < screen_.fields.size(); ++index) {
      if (screen_.fields[index].id == selected_id) {
        selected_index_ = index;
        break;
      }
    }
  }
  select_first_visible();
  editing_ = preserve_edit && selected_index_ < screen_.fields.size() &&
             screen_.fields[selected_index_].id == selected_id &&
             screen_.fields[selected_index_].editable;
  if (editing_) {
    screen_.fields[selected_index_].value = staged_value;
    screen_.fields[selected_index_].value_text = staged_text;
  }
  keep_selection_visible();
}

void CharacterOsdUi::update_home(const UiHomeStatus& home)
{
  home_ = home;
  if (screen_.kind == UiScreenKind::Home) {
    screen_.home = home;
  }
}

void CharacterOsdUi::select_first_visible()
{
  if (screen_.fields.empty()) {
    selected_index_ = 0;
    scroll_offset_ = 0;
    return;
  }
  if (selected_index_ < screen_.fields.size() &&
      screen_.fields[selected_index_].visible) {
    return;
  }
  for (std::size_t index = 0; index < screen_.fields.size(); ++index) {
    if (screen_.fields[index].visible) {
      selected_index_ = index;
      return;
    }
  }
  selected_index_ = 0;
}

void CharacterOsdUi::move_selection(int direction, int steps)
{
  if (screen_.fields.empty() || direction == 0) {
    return;
  }
  for (int step = 0; step < steps; ++step) {
    std::size_t candidate = selected_index_;
    while (true) {
      if (direction > 0) {
        if (candidate + 1 >= screen_.fields.size()) {
          return;
        }
        ++candidate;
      } else {
        if (candidate == 0) {
          return;
        }
        --candidate;
      }
      if (screen_.fields[candidate].visible) {
        selected_index_ = candidate;
        break;
      }
    }
  }
}

void CharacterOsdUi::keep_selection_visible()
{
  constexpr std::size_t kVisibleRows = kOpenPocketListRows;
  std::size_t selected_visible = 0;
  for (std::size_t index = 0; index < selected_index_ &&
                              index < screen_.fields.size(); ++index) {
    if (screen_.fields[index].visible) {
      ++selected_visible;
    }
  }
  if (selected_visible < scroll_offset_) {
    scroll_offset_ = selected_visible;
  } else if (selected_visible >= scroll_offset_ + kVisibleRows) {
    scroll_offset_ = selected_visible - kVisibleRows + 1;
  }
}

void CharacterOsdUi::update_value_text(UiField& field)
{
  if (field.kind == UiFieldKind::Boolean) {
    field.value_text = field.value != 0 ? "ON" : "OFF";
    return;
  }
  char value[16]{};
  (void)std::snprintf(value, sizeof(value), "%ld",
                      static_cast<long>(field.value));
  field.value_text = value;
}

bool CharacterOsdUi::handle(const UiEvent& event)
{
  if (screen_.kind == UiScreenKind::Home) {
    if (event.type == UiEventType::Enter) {
      pending_change_ = {screen_.id, "menu", 1};
      change_pending_ = true;
      return true;
    }
    return false;
  }
  if (event.type == UiEventType::Back) {
    if (editing_ && selected_index_ < screen_.fields.size()) {
      auto& field = screen_.fields[selected_index_];
      field.value = edit_original_value_;
      field.value_text = edit_original_text_;
      editing_ = false;
    } else {
      back_pending_ = true;
    }
    return true;
  }
  if (screen_.fields.empty() || selected_index_ >= screen_.fields.size()) {
    return false;
  }

  auto& field = screen_.fields[selected_index_];
  auto adjust = [this, &field](int32_t delta) {
    if (!editing_ || !field.editable || delta == 0) {
      return;
    }
    field.value =
        clamp<int32_t>(field.minimum, field.value + delta, field.maximum);
    update_value_text(field);
  };

  switch (event.type) {
    case UiEventType::Up:
      if (editing_) {
        adjust(1);
      } else {
        move_selection(-1);
      }
      break;
    case UiEventType::Down:
      if (editing_) {
        adjust(-1);
      } else {
        move_selection(1);
      }
      break;
    case UiEventType::Left:
      adjust(-1);
      break;
    case UiEventType::Right:
      adjust(1);
      break;
    case UiEventType::Rotate:
      if (editing_) {
        adjust(event.value);
      } else if (event.value != 0) {
        move_selection(event.value > 0 ? 1 : -1,
                       std::min(16, std::abs(event.value)));
      }
      break;
    case UiEventType::Enter:
      if (field.kind == UiFieldKind::Action) {
        pending_change_ = {screen_.id, field.id, 1};
        change_pending_ = true;
      } else if (field.editable && !editing_) {
        edit_original_value_ = field.value;
        edit_original_text_ = field.value_text;
        editing_ = true;
      } else if (field.editable) {
        pending_change_ = {screen_.id, field.id, field.value};
        change_pending_ = true;
        editing_ = false;
      }
      break;
    default:
      return false;
  }
  keep_selection_visible();
  return true;
}

bool CharacterOsdUi::render(const VrxStatus& vrx)
{
  composer_.compose(screen_, home_, vrx, selected_index_, scroll_offset_,
                    editing_);
  return true;
}

bool CharacterOsdUi::take_change(UiChange& change)
{
  if (!change_pending_) {
    return false;
  }
  change = pending_change_;
  change_pending_ = false;
  return true;
}

bool CharacterOsdUi::take_back_request()
{
  const bool requested = back_pending_;
  back_pending_ = false;
  return requested;
}

const UiScreen& CharacterOsdUi::screen() const
{
  return screen_;
}

const CharacterOsdFrame& CharacterOsdUi::frame() const
{
  return composer_.frame();
}

std::size_t CharacterOsdUi::selected_index() const
{
  return selected_index_;
}

std::size_t CharacterOsdUi::scroll_offset() const
{
  return scroll_offset_;
}

bool CharacterOsdUi::editing() const
{
  return editing_;
}

OpenPocketMenuController::OpenPocketMenuController(
    IOpenPocketScreenProvider& screens)
    : screens_(screens)
{
}

UiScreen OpenPocketMenuController::screen_for(OpenPocketPage page)
{
  switch (page) {
    case OpenPocketPage::MainMenu:
      return make_openpocket_main_menu_screen();
    case OpenPocketPage::ModelMenu:
      return make_openpocket_group_menu_screen(OpenPocketMenuGroup::Model);
    case OpenPocketPage::RadioMenu:
      return make_openpocket_group_menu_screen(OpenPocketMenuGroup::Radio);
    case OpenPocketPage::ElrsMenu:
      return make_openpocket_group_menu_screen(OpenPocketMenuGroup::Elrs);
    case OpenPocketPage::VideoMenu:
      return make_openpocket_group_menu_screen(OpenPocketMenuGroup::Video);
    case OpenPocketPage::UsbMenu:
      return make_openpocket_group_menu_screen(OpenPocketMenuGroup::Usb);
    case OpenPocketPage::DiagnosticsMenu:
      return make_openpocket_group_menu_screen(
          OpenPocketMenuGroup::Diagnostics);
    case OpenPocketPage::SystemMenu:
      return make_openpocket_group_menu_screen(OpenPocketMenuGroup::System);
    default:
      return screens_.screen(page);
  }
}

void OpenPocketMenuController::start(const UiHomeStatus& home,
                                     TimeUs now_us)
{
  home_ = home;
  notification_warning_ = UiWarningCode::None;
  notification_started_us_ = now_us;
  notification_seen_ = false;
  update_hud_warning(now_us);
  history_size_ = 0;
  page_ = OpenPocketPage::Home;
  ui_.set_screen(screen_for(page_));
  update_ui_home();
  change_pending_ = false;
}

void OpenPocketMenuController::refresh(const UiHomeStatus& home,
                                       TimeUs now_us)
{
  home_ = home;
  update_hud_warning(now_us);
  ui_.set_screen(screen_for(page_));
  update_ui_home();
}

void OpenPocketMenuController::update_hud_warning(TimeUs now_us)
{
  hud_home_ = home_;
  hud_home_.warnings.fill(UiWarningCode::None);
  hud_home_.warning_count = 0;

  UiWarningCode persistent = UiWarningCode::None;
  UiWarningCode notification = UiWarningCode::None;
  const std::size_t warning_count = std::min<std::size_t>(
      home_.warning_count, home_.warnings.size());
  for (std::size_t index = 0; index < warning_count; ++index) {
    const UiWarningCode warning = home_.warnings[index];
    if (warning == UiWarningCode::None) {
      continue;
    }
    if (persistent == UiWarningCode::None &&
        openpocket_warning_is_persistent(warning)) {
      persistent = warning;
    } else if (notification == UiWarningCode::None &&
               !openpocket_warning_is_persistent(warning)) {
      notification = warning;
    }
  }

  UiWarningCode displayed = persistent;
  if (persistent != UiWarningCode::None) {
    notification_warning_ = UiWarningCode::None;
    notification_seen_ = false;
  } else if (notification == UiWarningCode::None) {
    notification_warning_ = UiWarningCode::None;
    notification_seen_ = false;
  } else {
    if (!notification_seen_ || notification != notification_warning_ ||
        now_us < notification_started_us_) {
      notification_warning_ = notification;
      notification_started_us_ = now_us;
      notification_seen_ = true;
    }
    if (now_us - notification_started_us_ <
        kOpenPocketNotificationDurationUs) {
      displayed = notification;
    }
  }

  if (displayed == UiWarningCode::None) {
    return;
  }
  hud_home_.warnings[hud_home_.warning_count++] = displayed;
  for (std::size_t index = 0;
       index < warning_count &&
       hud_home_.warning_count < hud_home_.warnings.size(); ++index) {
    const UiWarningCode warning = home_.warnings[index];
    if (warning != UiWarningCode::None && warning != displayed) {
      hud_home_.warnings[hud_home_.warning_count++] = warning;
    }
  }
}

void OpenPocketMenuController::update_ui_home()
{
  ui_.update_home(page_ == OpenPocketPage::Home ? hud_home_ : home_);
}

bool OpenPocketMenuController::route(
    const UiChange& change, OpenPocketPage& target) const
{
  if (page_ == OpenPocketPage::Home && change.field_id == "menu") {
    target = OpenPocketPage::MainMenu;
    return true;
  }
  if (page_ == OpenPocketPage::MainMenu) {
    const std::array<std::pair<const char*, OpenPocketPage>, 7> groups{{
        {"group.model", OpenPocketPage::ModelMenu},
        {"group.radio", OpenPocketPage::RadioMenu},
        {"group.elrs", OpenPocketPage::ElrsMenu},
        {"group.video", OpenPocketPage::VideoMenu},
        {"group.usb", OpenPocketPage::UsbMenu},
        {"group.diagnostics", OpenPocketPage::DiagnosticsMenu},
        {"group.system", OpenPocketPage::SystemMenu},
    }};
    for (const auto& group : groups) {
      if (change.field_id == group.first) {
        target = group.second;
        return true;
      }
    }
    return false;
  }

  struct Route {
    OpenPocketPage parent;
    const char* field;
    OpenPocketPage target;
  };
  static constexpr std::array<Route, 19> routes{{
      {OpenPocketPage::ModelMenu, "models", OpenPocketPage::Models},
      {OpenPocketPage::ModelMenu, "model", OpenPocketPage::ModelSetup},
      {OpenPocketPage::ModelMenu, "inputs", OpenPocketPage::Inputs},
      {OpenPocketPage::ModelMenu, "mixes", OpenPocketPage::Mixes},
      {OpenPocketPage::ModelMenu, "limits", OpenPocketPage::Limits},
      {OpenPocketPage::ModelMenu, "flight_modes",
       OpenPocketPage::FlightModes},
      {OpenPocketPage::ModelMenu, "curves", OpenPocketPage::Curves},
      {OpenPocketPage::ModelMenu, "logical",
       OpenPocketPage::LogicalSwitches},
      {OpenPocketPage::ModelMenu, "special",
       OpenPocketPage::SpecialFunctions},
      {OpenPocketPage::ModelMenu, "timers", OpenPocketPage::Timers},
      {OpenPocketPage::RadioMenu, "outputs", OpenPocketPage::Outputs},
      {OpenPocketPage::RadioMenu, "power", OpenPocketPage::Power},
      {OpenPocketPage::ElrsMenu, "elrs", OpenPocketPage::Elrs},
      {OpenPocketPage::ElrsMenu, "finder", OpenPocketPage::Finder},
      {OpenPocketPage::VideoMenu, "video", OpenPocketPage::Video},
      {OpenPocketPage::UsbMenu, "usb", OpenPocketPage::Usb},
      {OpenPocketPage::DiagnosticsMenu, "warnings",
       OpenPocketPage::Warnings},
      {OpenPocketPage::DiagnosticsMenu, "telemetry",
       OpenPocketPage::Telemetry},
      {OpenPocketPage::SystemMenu, "web", OpenPocketPage::Web},
  }};
  for (const auto& item : routes) {
    if (page_ == item.parent && change.field_id == item.field) {
      target = item.target;
      return true;
    }
  }
  if (page_ == OpenPocketPage::SystemMenu &&
      change.field_id == "system") {
    target = OpenPocketPage::System;
    return true;
  }
  return false;
}

void OpenPocketMenuController::navigate(OpenPocketPage target)
{
  if (history_size_ < history_.size()) {
    history_[history_size_++] = page_;
  }
  page_ = target;
  ui_.set_screen(screen_for(page_));
  update_ui_home();
}

void OpenPocketMenuController::go_home()
{
  history_size_ = 0;
  page_ = OpenPocketPage::Home;
  ui_.set_screen(screen_for(page_));
  update_ui_home();
}

bool OpenPocketMenuController::handle(const UiEvent& event)
{
  if (event.type == UiEventType::Home || event.type == UiEventType::Back) {
    go_home();
    return true;
  }
  const bool handled = ui_.handle(event);
  UiChange change{};
  if (ui_.take_change(change)) {
    OpenPocketPage target = page_;
    if (route(change, target)) {
      navigate(target);
    } else {
      pending_change_ = change;
      change_pending_ = true;
    }
  }
  return handled;
}

bool OpenPocketMenuController::render(const VrxStatus& vrx)
{
  return ui_.render(vrx);
}

bool OpenPocketMenuController::take_change(UiChange& change)
{
  if (!change_pending_) {
    return false;
  }
  change = pending_change_;
  change_pending_ = false;
  return true;
}

OpenPocketPage OpenPocketMenuController::page() const
{
  return page_;
}

std::size_t OpenPocketMenuController::depth() const
{
  return history_size_;
}

const UiScreen& OpenPocketMenuController::screen() const
{
  return ui_.screen();
}

const CharacterOsdFrame& OpenPocketMenuController::frame() const
{
  return ui_.frame();
}

std::size_t OpenPocketMenuController::selected_index() const
{
  return ui_.selected_index();
}

std::size_t OpenPocketMenuController::scroll_offset() const
{
  return ui_.scroll_offset();
}

bool OpenPocketMenuController::editing() const
{
  return ui_.editing();
}

bool UsbSimulator::enter(bool outputs_locked, bool rf_safety_lock)
{
  if (!outputs_locked) {
    return false;
  }
  active_ = true;
  rf_safety_lock_ = rf_safety_lock;
  return true;
}

void UsbSimulator::leave()
{
  active_ = false;
  rf_safety_lock_ = true;
}

bool UsbSimulator::active() const
{
  return active_;
}

bool UsbSimulator::rf_output_allowed() const
{
  return !active_ || !rf_safety_lock_;
}

UsbGamepadReport UsbSimulator::report(
    const ControlInputs& controls, const ChannelFrame& channels) const
{
  UsbGamepadReport result{};
  for (std::size_t axis = 0; axis < result.axes.size(); ++axis) {
    result.axes[axis] = controls.axes[axis];
  }
  (void)channels;
  for (std::size_t index = 0; index < kMaxSwitches; ++index) {
    if (controls.switches[index]) {
      result.buttons |= static_cast<uint32_t>(1UL << index);
    }
  }
  return result;
}

BatteryEstimator::BatteryEstimator(uint16_t empty_mv, uint16_t full_mv)
    : empty_mv_(std::min(empty_mv, full_mv)),
      full_mv_(std::max<uint16_t>(empty_mv + 1, full_mv))
{
}

ProductPowerStatus BatteryEstimator::estimate(
    uint16_t voltage_mv, bool sample_valid, ChargeState charge,
    bool external_power, uint16_t average_current_ma,
    uint16_t remaining_capacity_mah) const
{
  ProductPowerStatus result{};
  result.voltage_mv = voltage_mv;
  result.charge = charge;
  result.external_power = external_power;
  result.sensor_fault = !sample_valid;
  if (!sample_valid || voltage_mv == 0) {
    return result;
  }
  result.percentage = static_cast<uint8_t>(clamp<int32_t>(
      0, (static_cast<int32_t>(voltage_mv) - empty_mv_) * 100 /
             (full_mv_ - empty_mv_),
      100));
  result.percentage_valid = true;
  if (average_current_ma != 0 && remaining_capacity_mah != 0) {
    result.runtime_minutes = static_cast<uint16_t>(
        std::min<uint32_t>(
            UINT16_MAX,
            static_cast<uint32_t>(remaining_capacity_mah) * 60 /
                average_current_ma));
    result.runtime_valid = true;
  }
  return result;
}

void OnboardingGuide::begin()
{
  step_ = OnboardingStep::Welcome;
}

bool OnboardingGuide::evidence_satisfies_step(
    const OnboardingEvidence& evidence) const
{
  switch (step_) {
    case OnboardingStep::Welcome:
      return true;
    case OnboardingStep::StickCalibration:
      return evidence.calibration_valid;
    case OnboardingStep::ArmSwitch:
      return evidence.arm_switch_identified;
    case OnboardingStep::AuxSwitches:
      return evidence.aux_positions_verified;
    case OnboardingStep::Elrs:
      return evidence.elrs_online;
    case OnboardingStep::Video:
      return !evidence.video_required || evidence.video_verified;
    case OnboardingStep::Battery:
      return evidence.battery_profile_valid;
    case OnboardingStep::ChannelPreview:
      return evidence.channel_preview_valid && evidence.arm_channel_low;
    case OnboardingStep::Complete:
      return false;
  }
  return false;
}

bool OnboardingGuide::advance(const OnboardingEvidence& evidence)
{
  if (!evidence_satisfies_step(evidence) ||
      step_ == OnboardingStep::Complete) {
    return false;
  }
  step_ = static_cast<OnboardingStep>(
      static_cast<uint8_t>(step_) + 1);
  return true;
}

void OnboardingGuide::back()
{
  if (step_ != OnboardingStep::Welcome) {
    step_ = static_cast<OnboardingStep>(
        static_cast<uint8_t>(step_) - 1);
  }
}

OnboardingStep OnboardingGuide::step() const
{
  return step_;
}

bool OnboardingGuide::complete() const
{
  return step_ == OnboardingStep::Complete;
}

}  // namespace rivettx
