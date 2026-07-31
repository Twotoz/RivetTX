#pragma once

#include "rivettx/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace rivettx {

class IPcmOutput {
 public:
  virtual ~IPcmOutput() = default;
  virtual bool initialize(uint32_t sample_rate_hz) = 0;
  virtual bool set_amplifier_enabled(bool enabled) = 0;
  // Must return immediately. written may be zero when the DMA queue is full.
  virtual bool write_nonblocking(const int16_t* samples, std::size_t count,
                                 std::size_t& written) = 0;
};

struct SpeakerSettings {
  bool enabled = false;
  uint8_t volume_percent = 50;
};

struct SpeakerStatus {
  bool available = false;
  bool enabled = false;
  bool playing = false;
  bool underrun = false;
  uint32_t completed_tones = 0;
  uint32_t rejected_tones = 0;
};

// Bounded single-producer audio service for the optional NS4168. PCM is
// generated in short chunks and offered to DMA without waiting.
class SpeakerService {
 public:
  explicit SpeakerService(IPcmOutput& output);
  bool initialize(bool simulator_mode, SpeakerSettings settings = {});
  bool configure(SpeakerSettings settings);
  bool play_test_tone(uint16_t frequency_hz, uint16_t duration_ms);
  void stop();
  void tick(TimeUs now_us);
  const SpeakerStatus& status() const;

 private:
  static constexpr uint32_t kSampleRate = 16000;
  static constexpr std::size_t kChunkSamples = 128;
  IPcmOutput& output_;
  SpeakerSettings settings_{};
  SpeakerStatus status_{};
  std::array<int16_t, kChunkSamples> samples_{};
  uint32_t phase_ = 0;
  uint32_t phase_step_ = 0;
  uint32_t remaining_samples_ = 0;
  bool simulator_mode_ = false;
};

}  // namespace rivettx
