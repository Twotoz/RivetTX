#include "rivettx/rx5808.hpp"

#include <algorithm>
#include <array>
#include <limits>

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

constexpr uint8_t kRtc6715RegisterSynthesizerB = 0x01;
constexpr uint32_t kRtc6715WriteBit = 1U << 4U;
constexpr uint8_t kRtc6715FrameBits = 25;
constexpr uint8_t kTransitionsPerBit = 3;
constexpr uint8_t kTransferTransitions =
    1 + kRtc6715FrameBits * kTransitionsPerBit + 1;
constexpr uint8_t kMaximumTransitionsPerTick = 8;

}  // namespace

uint16_t vrx_frequency_mhz(uint8_t band, uint8_t channel)
{
  if (band >= kVrxBandCount || channel >= kVrxChannelsPerBand) {
    return 0;
  }
  return kVrxFrequencies[band][channel];
}

bool vrx_frequency_supported(uint16_t frequency_mhz)
{
  for (const auto& band : kVrxFrequencies) {
    for (const uint16_t frequency : band) {
      if (frequency == frequency_mhz) {
        return true;
      }
    }
  }
  return false;
}

bool VrxCommandQueue::push(VrxCommand command)
{
  if (count_ == commands_.size()) {
    return false;
  }
  commands_[write_] = command;
  write_ = (write_ + 1) % commands_.size();
  ++count_;
  return true;
}

bool VrxCommandQueue::pop(VrxCommand& command)
{
  if (count_ == 0) {
    return false;
  }
  command = commands_[read_];
  read_ = (read_ + 1) % commands_.size();
  --count_;
  return true;
}

std::size_t VrxCommandQueue::size() const
{
  return count_;
}

bool VrxCommandQueue::empty() const
{
  return count_ == 0;
}

bool rtc6715_program_for_frequency(uint16_t frequency_mhz,
                                   Rtc6715Program& program)
{
  program = {};
  if (!vrx_frequency_supported(frequency_mhz) || frequency_mhz <= 479) {
    return false;
  }
  const uint16_t divider =
      static_cast<uint16_t>((frequency_mhz - 479U) / 2U);
  const uint16_t n = static_cast<uint16_t>(divider / 32U);
  const uint16_t a = static_cast<uint16_t>(divider % 32U);
  if (n > 0x7FU || a > 0x1FU) {
    return false;
  }
  program.synthesizer_b =
      static_cast<uint16_t>((n << 7U) | a);
  program.frame = kRtc6715RegisterSynthesizerB | kRtc6715WriteBit |
                  (static_cast<uint32_t>(program.synthesizer_b) << 5U);
  return (program.frame >> kRtc6715FrameBits) == 0;
}

Rtc6715Backend::Rtc6715Backend(IRtc6715Io& io, Rtc6715Config config)
    : io_(io), config_(config)
{
  config_.transition_interval_us =
      std::max<uint32_t>(1, config_.transition_interval_us);
  config_.tune_timeout_ms = std::max<uint32_t>(10, config_.tune_timeout_ms);
}

bool Rtc6715Backend::initialize()
{
  if (initialized_) {
    return true;
  }
  initialized_ = io_.initialize();
  if (!initialized_) {
    state_ = VrxTuneState::Failed;
    return false;
  }
  idle_lines();
  if (state_ == VrxTuneState::Failed) {
    initialized_ = false;
    return false;
  }
  state_ = VrxTuneState::Idle;
  return true;
}

bool Rtc6715Backend::start_tune(uint16_t frequency_mhz, TimeUs now_us)
{
  Rtc6715Program program{};
  if (!initialized_ || state_ == VrxTuneState::Pending ||
      !rtc6715_program_for_frequency(frequency_mhz, program)) {
    return false;
  }
  frame_ = program.frame;
  transition_ = 0;
  requested_frequency_mhz_ = frequency_mhz;
  next_transition_us_ = now_us;
  deadline_us_ = now_us +
                 static_cast<TimeUs>(config_.tune_timeout_ms) * 1000U;
  state_ = VrxTuneState::Pending;
  return true;
}

void Rtc6715Backend::tick(TimeUs now_us)
{
  if (state_ != VrxTuneState::Pending) {
    return;
  }
  if (now_us >= deadline_us_) {
    fail();
    return;
  }
  uint8_t advanced = 0;
  while (state_ == VrxTuneState::Pending &&
         now_us >= next_transition_us_ &&
         advanced < kMaximumTransitionsPerTick) {
    if (!advance_transition()) {
      fail();
      return;
    }
    ++advanced;
    next_transition_us_ += config_.transition_interval_us;
  }
}

VrxTuneState Rtc6715Backend::tune_state() const
{
  return state_;
}

void Rtc6715Backend::cancel_tune()
{
  idle_lines();
  if (initialized_ && state_ != VrxTuneState::Failed) {
    state_ = VrxTuneState::Idle;
  }
  requested_frequency_mhz_ = 0;
  transition_ = 0;
}

VrxAdcSample Rtc6715Backend::sample_rssi()
{
  if (!initialized_) {
    return {VrxAdcSampleState::Unavailable, 0};
  }
  int raw = 0;
  if (!io_.read_rssi(raw) || raw < 0 ||
      raw > std::numeric_limits<int16_t>::max()) {
    return {VrxAdcSampleState::SensorFault, 0};
  }
  return {VrxAdcSampleState::Valid, static_cast<int16_t>(raw)};
}

uint16_t Rtc6715Backend::tuned_frequency_mhz() const
{
  return tuned_frequency_mhz_;
}

bool Rtc6715Backend::advance_transition()
{
  if (transition_ == 0) {
    ++transition_;
    return io_.set_clock(false) && io_.set_latch(false);
  }
  if (transition_ < 1 + kRtc6715FrameBits * kTransitionsPerBit) {
    const uint8_t relative = static_cast<uint8_t>(transition_ - 1);
    const uint8_t bit = static_cast<uint8_t>(relative / kTransitionsPerBit);
    const uint8_t phase =
        static_cast<uint8_t>(relative % kTransitionsPerBit);
    bool ok = false;
    if (phase == 0) {
      ok = io_.set_data(((frame_ >> bit) & 1U) != 0);
    } else if (phase == 1) {
      ok = io_.set_clock(true);
    } else {
      ok = io_.set_clock(false);
    }
    ++transition_;
    return ok;
  }
  if (transition_ == kTransferTransitions - 1) {
    ++transition_;
    if (!io_.set_latch(true)) {
      return false;
    }
    tuned_frequency_mhz_ = requested_frequency_mhz_;
    state_ = VrxTuneState::Complete;
    return true;
  }
  return false;
}

void Rtc6715Backend::fail()
{
  state_ = VrxTuneState::Failed;
  (void)io_.set_clock(false);
  (void)io_.set_latch(true);
}

void Rtc6715Backend::idle_lines()
{
  if (!io_.set_data(false) || !io_.set_clock(false) ||
      !io_.set_latch(true)) {
    state_ = VrxTuneState::Failed;
  }
}

}  // namespace rivettx
