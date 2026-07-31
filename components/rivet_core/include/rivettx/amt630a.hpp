#pragma once

#include "rivettx/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace rivettx {

constexpr std::size_t kAmt630aFlashSize = 64U * 1024U;
constexpr std::size_t kAmt630aFlashPageSize = 256U;
constexpr uint16_t kAmt630aExpectedIdentity = 0x630a;
constexpr uint32_t kAmt630aExpectedFlashJedec = 0xef3010;

enum class Amt630aVideoStandard : uint8_t {
  None,
  Ntsc,
  Pal,
  Secam,
  Unstable,
};

enum class Amt630aState : uint8_t {
  Unavailable,
  Detecting,
  Ready,
  AcquiringFlash,
  CheckingFlash,
  EnablingErase,
  Erasing,
  WaitingForErase,
  EnablingProgram,
  Programming,
  WaitingForProgram,
  Verifying,
  ReleasingFlash,
  WaitingForBoot,
  Complete,
  Fault,
};

enum class Amt630aFault : uint8_t {
  None,
  Configuration,
  Communication,
  Identity,
  FlashIdentity,
  Timeout,
  Readback,
  Checksum,
  Boot,
};

struct Amt630aRuntimeStatus {
  bool booted = false;
  bool video_present = false;
  bool panel_timing_active = false;
  uint8_t firmware_major = 0;
  uint8_t firmware_minor = 0;
  Amt630aVideoStandard standard = Amt630aVideoStandard::None;
};

struct Amt630aStatus {
  Amt630aState state = Amt630aState::Unavailable;
  Amt630aFault fault = Amt630aFault::None;
  Amt630aRuntimeStatus runtime{};
  uint16_t identity = 0;
  uint32_t flash_jedec = 0;
  uint32_t programmed_bytes = 0;
  uint32_t image_size = 0;
  uint8_t progress_percent = 0;
};

class IAmt630aHardware {
 public:
  virtual ~IAmt630aHardware() = default;
  virtual bool initialize() = 0;
  virtual bool read_identity(uint16_t& identity) = 0;
  // The implementation must assert AMT RESET before selecting the ESP32 side
  // of the four-channel flash mux. The mux has a hardware pull selecting AMT.
  virtual bool acquire_flash(uint32_t& jedec_identity) = 0;
  virtual bool flash_write_enable() = 0;
  virtual bool flash_chip_erase() = 0;
  virtual bool flash_busy(bool& busy) = 0;
  virtual bool flash_program_page(uint32_t address, const uint8_t* data,
                                  std::size_t size) = 0;
  virtual bool flash_read(uint32_t address, uint8_t* data,
                          std::size_t size) = 0;
  // Select AMT ownership before releasing RESET. This is also the recovery
  // path after a cancelled or interrupted update.
  virtual bool release_flash_and_reset() = 0;
  virtual bool read_runtime_status(Amt630aRuntimeStatus& status) = 0;
};

struct Amt630aConfig {
  uint32_t operation_timeout_ms = 10000;
  uint32_t status_interval_ms = 100;
};

struct Amt630aSha256Context {
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

// Service-task state machine. A tick performs one bounded SPI/I2C operation;
// erase/program completion is polled and never delays control or CRSF tasks.
class Amt630aController {
 public:
  explicit Amt630aController(IAmt630aHardware& hardware,
                             Amt630aConfig config = {});

  bool initialize(TimeUs now_us);
  bool start_program(const uint8_t* image, std::size_t size,
                     const std::array<uint8_t, 32>& expected_sha256,
                     TimeUs now_us);
  // Factory/startup path: read and hash the installed image first. A matching
  // flash is left untouched; a blank, corrupt, or old image is programmed.
  bool ensure_program(const uint8_t* image, std::size_t size,
                      const std::array<uint8_t, 32>& expected_sha256,
                      TimeUs now_us);
  bool cancel_program();
  bool recover(TimeUs now_us);
  void tick(TimeUs now_us);
  const Amt630aStatus& status() const;

 private:
  void fail(Amt630aFault fault);
  void set_deadline(TimeUs now_us);
  bool timed_out(TimeUs now_us) const;
  void update_progress();

  IAmt630aHardware& hardware_;
  Amt630aConfig config_{};
  Amt630aStatus status_{};
  const uint8_t* image_ = nullptr;
  std::array<uint8_t, 32> expected_sha256_{};
  std::array<uint8_t, kAmt630aFlashPageSize> readback_{};
  uint32_t offset_ = 0;
  uint32_t verify_offset_ = 0;
  std::size_t page_size_ = 0;
  TimeUs deadline_us_ = 0;
  TimeUs next_status_us_ = 0;
  bool verify_started_ = false;
  bool checking_installed_ = false;
  uint8_t runtime_read_failures_ = 0;
  Amt630aSha256Context sha256_{};
};

std::array<uint8_t, 32> amt630a_sha256(const uint8_t* data,
                                      std::size_t size);

}  // namespace rivettx
