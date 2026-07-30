#include "rivettx/product.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rivettx {

namespace {

constexpr std::array<std::array<uint16_t, kVrxChannelsPerBand>,
                     kVrxBandCount>
    kVrxFrequencies{{
        {{5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725}},
        {{5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866}},
        {{5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945}},
        {{5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880}},
        {{5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917}},
        {{5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621}},
    }};

uint8_t rssi_percent(int16_t rssi)
{
  return static_cast<uint8_t>(
      clamp<int32_t>(0, (static_cast<int32_t>(rssi) + 100) * 100 / 80,
                     100));
}

}  // namespace

uint16_t vrx_frequency_mhz(uint8_t band, uint8_t channel)
{
  if (band >= kVrxBandCount || channel >= kVrxChannelsPerBand) {
    return 0;
  }
  return kVrxFrequencies[band][channel];
}

VrxController::VrxController(IVrxHardware& hardware,
                             uint32_t scan_dwell_ms)
    : hardware_(hardware),
      scan_dwell_ms_(std::max<uint32_t>(20, scan_dwell_ms))
{
}

bool VrxController::select(uint8_t band, uint8_t channel, TimeUs now_us)
{
  const uint16_t frequency = vrx_frequency_mhz(band, channel);
  if (frequency == 0 || !hardware_.tune(frequency)) {
    status_.available = false;
    return false;
  }
  status_.band = band;
  status_.channel = channel;
  status_.frequency_mhz = frequency;
  status_.available = true;
  status_.scanning = false;
  status_.signal_fresh = false;
  last_sample_us_ = now_us;
  return true;
}

bool VrxController::begin_scan(TimeUs now_us)
{
  scan_index_ = 0;
  best_index_ = 0;
  best_rssi_ = INT16_MIN;
  status_.scanning = true;
  next_scan_step_us_ = now_us;
  return tune_scan_candidate(now_us);
}

void VrxController::cancel_scan()
{
  status_.scanning = false;
}

bool VrxController::tune_scan_candidate(TimeUs now_us)
{
  if (scan_index_ >= kVrxBandCount * kVrxChannelsPerBand) {
    status_.scanning = false;
    const uint8_t band =
        static_cast<uint8_t>(best_index_ / kVrxChannelsPerBand);
    const uint8_t channel =
        static_cast<uint8_t>(best_index_ % kVrxChannelsPerBand);
    return select(band, channel, now_us);
  }
  const uint8_t band =
      static_cast<uint8_t>(scan_index_ / kVrxChannelsPerBand);
  const uint8_t channel =
      static_cast<uint8_t>(scan_index_ % kVrxChannelsPerBand);
  const uint16_t frequency = vrx_frequency_mhz(band, channel);
  if (!hardware_.tune(frequency)) {
    status_.available = false;
    status_.scanning = false;
    return false;
  }
  status_.band = band;
  status_.channel = channel;
  status_.frequency_mhz = frequency;
  status_.available = true;
  status_.signal_fresh = false;
  next_scan_step_us_ =
      now_us + static_cast<TimeUs>(scan_dwell_ms_) * 1000;
  return true;
}

void VrxController::tick(TimeUs now_us)
{
  if (!status_.available) {
    return;
  }
  int16_t rssi = 0;
  bool video_signal = false;
  if (hardware_.sample(rssi, video_signal)) {
    status_.rssi = rssi;
    status_.strength_percent = rssi_percent(rssi);
    status_.video_signal = video_signal;
    status_.signal_fresh = true;
    last_sample_us_ = now_us;
    if (status_.scanning && rssi > best_rssi_) {
      best_rssi_ = rssi;
      best_index_ = scan_index_;
    }
  } else if (last_sample_us_ == 0 ||
             now_us - last_sample_us_ > 1000000) {
    status_.signal_fresh = false;
    status_.video_signal = false;
  }
  if (status_.scanning && now_us >= next_scan_step_us_) {
    ++scan_index_;
    (void)tune_scan_candidate(now_us);
  }
}

const VrxStatus& VrxController::status() const
{
  return status_;
}

char CharacterOsdFrame::at(std::size_t column, std::size_t row) const
{
  return column < kOsdColumns && row < kOsdRows
             ? cells[row * kOsdColumns + column]
             : '\0';
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
  clear();
  text(0, 0, model.name.data());
  text(20, 0, home.outputs_enabled ? "LIVE" : "SAFE");
  if (home.warning_count != 0) {
    text(0, 2, "WARNING");
    number(8, 2, static_cast<int32_t>(home.warnings[0]));
  }
  text(0, 4, "TX");
  number(3, 4, home.battery_mv);
  text(9, 4, "LQ");
  number(12, 4, home.link_quality);
  text(18, 4, home.module_online ? "ELRS OK" : "ELRS LOST");
  text(0, 6, "VRX");
  number(4, 6, vrx.frequency_mhz);
  text(10, 6, vrx.video_signal ? "VIDEO" : "NO SIGNAL");
  text(0, 8, "CH5");
  number(4, 8, home.channels[4]);
  text(0, 15, "RIVETTX OPENPOCKET");
}

const CharacterOsdFrame& CharacterOsdComposer::frame() const
{
  return frame_;
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
