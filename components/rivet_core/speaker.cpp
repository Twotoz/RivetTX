#include "rivettx/speaker.hpp"

#include <algorithm>

namespace rivettx {

SpeakerService::SpeakerService(IPcmOutput& output) : output_(output) {}

bool SpeakerService::initialize(bool simulator_mode, SpeakerSettings settings)
{
  simulator_mode_ = simulator_mode;
  settings.volume_percent = std::min<uint8_t>(settings.volume_percent, 100);
  settings_ = settings;
  if (simulator_mode_) {
    status_ = {};
    return true;
  }
  status_.available = output_.initialize(kSampleRate);
  status_.enabled = status_.available && settings_.enabled &&
                    output_.set_amplifier_enabled(settings_.enabled);
  return status_.available;
}

bool SpeakerService::configure(SpeakerSettings settings)
{
  settings.volume_percent = std::min<uint8_t>(settings.volume_percent, 100);
  settings_ = settings;
  if (simulator_mode_) {
    status_.enabled = false;
    return !settings.enabled;
  }
  if (!status_.available || !output_.set_amplifier_enabled(settings.enabled)) {
    return false;
  }
  status_.enabled = settings.enabled;
  if (!settings.enabled) stop();
  return true;
}

bool SpeakerService::play_test_tone(uint16_t frequency_hz,
                                    uint16_t duration_ms)
{
  if (!status_.enabled || status_.playing || frequency_hz < 100 ||
      frequency_hz > 5000 || duration_ms == 0 || duration_ms > 2000) {
    ++status_.rejected_tones;
    return false;
  }
  phase_ = 0;
  phase_step_ = static_cast<uint32_t>(
      (static_cast<uint64_t>(frequency_hz) << 32U) / kSampleRate);
  remaining_samples_ = kSampleRate * duration_ms / 1000U;
  status_.playing = remaining_samples_ != 0;
  status_.underrun = false;
  return status_.playing;
}

void SpeakerService::stop()
{
  remaining_samples_ = 0;
  status_.playing = false;
}

void SpeakerService::tick(TimeUs)
{
  if (!status_.playing || remaining_samples_ == 0) return;
  const std::size_t count = std::min<std::size_t>(samples_.size(),
                                                  remaining_samples_);
  const int32_t amplitude = 28000 * settings_.volume_percent / 100;
  for (std::size_t index = 0; index < count; ++index) {
    samples_[index] = static_cast<int16_t>(
        (phase_ & 0x80000000U) != 0 ? amplitude : -amplitude);
    phase_ += phase_step_;
  }
  std::size_t written = 0;
  if (!output_.write_nonblocking(samples_.data(), count, written)) {
    status_.underrun = true;
    return;
  }
  written = std::min(written, count);
  remaining_samples_ -= static_cast<uint32_t>(written);
  if (written == 0) status_.underrun = true;
  if (remaining_samples_ == 0) {
    status_.playing = false;
    ++status_.completed_tones;
  }
}

const SpeakerStatus& SpeakerService::status() const { return status_; }

}  // namespace rivettx
