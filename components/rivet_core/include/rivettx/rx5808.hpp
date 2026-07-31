#pragma once

#include "rivettx/types.hpp"

#include <cstddef>
#include <cstdint>
#include <array>

namespace rivettx {

constexpr std::size_t kVrxBandCount = 6;
constexpr std::size_t kVrxChannelsPerBand = 8;
constexpr std::size_t kVrxCommandQueueCapacity = 8;

uint16_t vrx_frequency_mhz(uint8_t band, uint8_t channel);
bool vrx_frequency_supported(uint16_t frequency_mhz);

enum class VrxCommandType : uint8_t {
  Tune,
  StartScan,
  CancelScan,
};

struct VrxCommand {
  VrxCommandType type = VrxCommandType::Tune;
  uint8_t band = 0;
  uint8_t channel = 0;
};

// A fixed-capacity mailbox used between UI/control code and the low-priority
// VRX task. The caller supplies platform locking; the container itself never
// allocates, waits, or overwrites an older command.
class VrxCommandQueue {
 public:
  bool push(VrxCommand command);
  bool pop(VrxCommand& command);
  std::size_t size() const;
  bool empty() const;

 private:
  std::array<VrxCommand, kVrxCommandQueueCapacity> commands_{};
  std::size_t read_ = 0;
  std::size_t write_ = 0;
  std::size_t count_ = 0;
};

enum class VrxTuneState : uint8_t {
  Idle,
  Pending,
  Complete,
  Failed,
};

enum class VrxAdcSampleState : uint8_t {
  Valid,
  Unavailable,
  SensorFault,
};

struct VrxAdcSample {
  VrxAdcSampleState state = VrxAdcSampleState::Unavailable;
  int16_t raw = 0;
};

class IVrxHardware {
 public:
  virtual ~IVrxHardware() = default;
  virtual bool start_tune(uint16_t frequency_mhz, TimeUs now_us) = 0;
  virtual void tick(TimeUs now_us) = 0;
  virtual VrxTuneState tune_state() const = 0;
  virtual void cancel_tune() = 0;
  virtual VrxAdcSample sample_rssi() = 0;
};

struct Rtc6715Program {
  uint16_t synthesizer_b = 0;
  uint32_t frame = 0;
};

bool rtc6715_program_for_frequency(uint16_t frequency_mhz,
                                   Rtc6715Program& program);

class IRtc6715Io {
 public:
  virtual ~IRtc6715Io() = default;
  virtual bool initialize() = 0;
  virtual bool set_data(bool high) = 0;
  virtual bool set_clock(bool high) = 0;
  virtual bool set_latch(bool high) = 0;
  virtual bool read_rssi(int& raw_adc) = 0;
};

struct Rtc6715Config {
  uint32_t transition_interval_us = 2;
  uint32_t tune_timeout_ms = 100;
};

class Rtc6715Backend final : public IVrxHardware {
 public:
  explicit Rtc6715Backend(IRtc6715Io& io,
                          Rtc6715Config config = {});

  bool initialize();
  bool start_tune(uint16_t frequency_mhz, TimeUs now_us) override;
  void tick(TimeUs now_us) override;
  VrxTuneState tune_state() const override;
  void cancel_tune() override;
  VrxAdcSample sample_rssi() override;
  uint16_t tuned_frequency_mhz() const;

 private:
  bool advance_transition();
  void fail();
  void idle_lines();

  IRtc6715Io& io_;
  Rtc6715Config config_{};
  VrxTuneState state_ = VrxTuneState::Idle;
  uint32_t frame_ = 0;
  uint8_t transition_ = 0;
  TimeUs next_transition_us_ = 0;
  TimeUs deadline_us_ = 0;
  uint16_t requested_frequency_mhz_ = 0;
  uint16_t tuned_frequency_mhz_ = 0;
  bool initialized_ = false;
};

}  // namespace rivettx
