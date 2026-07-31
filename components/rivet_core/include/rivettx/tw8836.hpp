#pragma once

#include "rivettx/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace rivettx {

constexpr std::size_t kTw8836FlashSize = 2U * 1024U * 1024U;
constexpr std::size_t kTw8836IspBlockSize = 256;
constexpr uint16_t kTw8836ExpectedIdentity = 0x8836;

enum class Tw8836VideoStandard : uint8_t {
  None,
  Ntsc,
  Pal,
  Secam,
  Unstable,
};

enum class Tw8836State : uint8_t {
  Unavailable,
  Detecting,
  Ready,
  EnteringIsp,
  EnablingErase,
  WaitingForEraseEnable,
  Erasing,
  WaitingForErase,
  LoadingBlock,
  EnablingProgram,
  WaitingForProgramEnable,
  Programming,
  WaitingForProgram,
  Verifying,
  WaitingForReadback,
  Rebooting,
  WaitingForBoot,
  Complete,
  Fault,
};

enum class Tw8836Fault : uint8_t {
  None,
  Configuration,
  Communication,
  Identity,
  Timeout,
  Readback,
  Checksum,
  Boot,
};

struct Tw8836RuntimeStatus {
  bool booted = false;
  bool video_present = false;
  bool panel_timing_active = false;
  Tw8836VideoStandard standard = Tw8836VideoStandard::None;
};

struct Tw8836Status {
  Tw8836State state = Tw8836State::Unavailable;
  Tw8836Fault fault = Tw8836Fault::None;
  Tw8836RuntimeStatus runtime{};
  uint16_t identity = 0;
  uint32_t programmed_bytes = 0;
  uint32_t image_size = 0;
  uint8_t progress_percent = 0;
};

class ITw8836Hardware {
 public:
  virtual ~ITw8836Hardware() = default;
  virtual bool initialize() = 0;
  virtual bool read_identity(uint16_t& identity) = 0;
  virtual bool enter_isp() = 0;
  virtual bool write_enable() = 0;
  virtual bool start_erase() = 0;
  virtual bool flash_busy(bool& busy) = 0;
  virtual bool load_xram_block(const uint8_t* data, std::size_t size) = 0;
  virtual bool start_program_flash_block(uint32_t address,
                                         std::size_t size) = 0;
  virtual bool begin_read_flash_block(uint32_t address,
                                      std::size_t size) = 0;
  virtual bool read_xram_block(uint8_t* data, std::size_t size) = 0;
  virtual bool exit_isp_and_reset() = 0;
  virtual bool read_runtime_status(Tw8836RuntimeStatus& status) = 0;
};

struct Tw8836Config {
  uint32_t operation_timeout_ms = 10000;
  uint32_t status_interval_ms = 100;
};

struct Tw8836Sha256Context {
  std::array<uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                0xa54ff53a, 0x510e527f, 0x9b05688c,
                                0x1f83d9ab, 0x5be0cd19};
  std::array<uint8_t, 64> buffer{};
  uint64_t bytes = 0;
  std::size_t buffered = 0;
  void transform(const uint8_t* block);
  void update(const uint8_t* data, std::size_t size);
  std::array<uint8_t, 32> finish();
};

// A bounded service-task state machine. Each tick performs at most one
// hardware operation and never waits for an erase, program, boot, or I2C
// transaction to complete.
class Tw8836Controller {
 public:
  explicit Tw8836Controller(ITw8836Hardware& hardware,
                            Tw8836Config config = {});

  bool initialize(TimeUs now_us);
  bool start_program(const uint8_t* image, std::size_t size,
                     const std::array<uint8_t, 32>& expected_sha256,
                     TimeUs now_us);
  bool cancel_program();
  bool recover(TimeUs now_us);
  void tick(TimeUs now_us);
  const Tw8836Status& status() const;

 private:
  void fail(Tw8836Fault fault);
  void set_deadline(TimeUs now_us);
  bool timed_out(TimeUs now_us) const;
  void update_progress();

  ITw8836Hardware& hardware_;
  Tw8836Config config_{};
  Tw8836Status status_{};
  const uint8_t* image_ = nullptr;
  std::array<uint8_t, 32> expected_sha256_{};
  std::array<uint8_t, kTw8836IspBlockSize> readback_{};
  uint32_t offset_ = 0;
  uint32_t verify_offset_ = 0;
  std::size_t verify_block_size_ = 0;
  std::size_t program_block_size_ = 0;
  TimeUs deadline_us_ = 0;
  TimeUs next_status_us_ = 0;
  bool verify_started_ = false;
  uint8_t runtime_read_failures_ = 0;
  Tw8836Sha256Context sha256_{};
};

std::array<uint8_t, 32> tw8836_sha256(const uint8_t* data, std::size_t size);

}  // namespace rivettx
