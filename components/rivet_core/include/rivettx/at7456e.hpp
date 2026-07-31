#pragma once

#include "rivettx/product.hpp"
#include "rivettx/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace rivettx {

constexpr std::size_t kAt7456eNtscRows = 13;
static_assert(kAt7456eNtscRows == kOpenPocketSafeRows,
              "OpenPocket essential layout must fit the NTSC backend");
constexpr std::size_t kAt7456eGlyphBytes = 54;
constexpr std::size_t kAt7456eGlyphQueueDepth = 4;

enum class At7456eTransferState : uint8_t {
  Busy,
  Complete,
  Failed,
};

class IAt7456eSpi {
 public:
  virtual ~IAt7456eSpi() = default;
  virtual bool queue(const uint8_t* transmit, uint8_t* receive,
                     std::size_t size) = 0;
  virtual At7456eTransferState poll() = 0;
  virtual void abort() = 0;
};

enum class At7456eVideoStandard : uint8_t {
  Unknown,
  Ntsc,
  Pal,
};

struct At7456eCustomCharacter {
  uint8_t index = 0;
  std::array<uint8_t, kAt7456eGlyphBytes> pixels{};
};

struct At7456eStatus {
  At7456eVideoStandard standard = At7456eVideoStandard::Unknown;
  std::size_t visible_rows = 0;
  std::size_t pending_characters = 0;
  uint32_t communication_failures = 0;
  bool initialized = false;
  bool healthy = false;
  bool video_present = false;
  bool character_upload_active = false;
};

// Non-blocking AT7456E/MAX7456-compatible display-memory driver. Call tick()
// frequently from a low-priority task; it queues at most one bounded SPI
// transaction and never waits for SPI or NVM completion.
class At7456eDriver {
 public:
  explicit At7456eDriver(IAt7456eSpi& spi,
                         At7456eVideoStandard no_video_fallback =
                             At7456eVideoStandard::Pal);

  void start(TimeUs now_us);
  void submit(const CharacterOsdFrame& frame);
  bool upload(const At7456eCustomCharacter& character);
  void tick(TimeUs now_us);
  const At7456eStatus& status() const;

 private:
  enum class Phase : uint8_t {
    Stopped,
    Reset,
    ResetDelay,
    ReadBlackLevel,
    WriteBlackLevel,
    Probe,
    Configure,
    VerifyConfigure,
    Ready,
    CellAddressHigh,
    CellAddressLow,
    CellMode,
    CellData,
    CellEnd,
    GlyphDisable,
    GlyphAddress,
    GlyphDataAddress,
    GlyphDataValue,
    GlyphCommit,
    GlyphWait,
    GlyphEnable,
  };

  enum class Operation : uint8_t {
    None,
    Reset,
    ReadBlackLevel,
    WriteBlackLevel,
    Status,
    Configure,
    VerifyConfigure,
    CellAddressHigh,
    CellAddressLow,
    CellMode,
    CellData,
    CellEnd,
    GlyphDisable,
    GlyphAddress,
    GlyphDataAddress,
    GlyphDataValue,
    GlyphCommit,
    GlyphStatus,
    GlyphEnable,
  };

  static constexpr std::size_t kMaximumRun = 24;
  static constexpr TimeUs kStatusPeriodUs = 100000;
  static constexpr TimeUs kTransferTimeoutUs = 20000;
  static constexpr TimeUs kRetryDelayUs = 100000;

  bool queue_write(uint8_t address, uint8_t value,
                   Operation operation, TimeUs now_us);
  bool queue_raw(uint8_t value, Operation operation, TimeUs now_us);
  bool queue_read(uint8_t address, Operation operation, TimeUs now_us);
  bool queue_status(Operation operation, TimeUs now_us);
  bool prepare_cells();
  void complete_operation(TimeUs now_us);
  void accept_status(uint8_t value, TimeUs now_us);
  void fail(TimeUs now_us);
  void invalidate_shadow();
  void update_pending_count();
  uint8_t video_mode(bool display_enabled) const;
  At7456eVideoStandard configured_standard() const;
  std::size_t visible_rows() const;

  IAt7456eSpi& spi_;
  At7456eVideoStandard fallback_;
  At7456eStatus status_{};
  CharacterOsdFrame desired_{};
  std::array<uint8_t, kOsdColumns * kOsdRows> shadow_{};
  std::array<bool, kOsdColumns * kOsdRows> shadow_valid_{};
  std::array<At7456eCustomCharacter, kAt7456eGlyphQueueDepth> glyphs_{};
  std::size_t glyph_read_ = 0;
  std::size_t glyph_write_ = 0;
  std::size_t glyph_count_ = 0;
  std::size_t glyph_byte_ = 0;
  std::array<uint8_t, kMaximumRun> sent_cells_{};
  std::size_t sent_begin_ = 0;
  std::size_t sent_count_ = 0;
  std::size_t sent_progress_ = 0;
  std::array<uint8_t, 64> transmit_{};
  std::array<uint8_t, 64> receive_{};
  std::size_t transfer_size_ = 0;
  Phase phase_ = Phase::Stopped;
  Operation operation_ = Operation::None;
  At7456eVideoStandard active_standard_ = At7456eVideoStandard::Unknown;
  TimeUs phase_deadline_us_ = 0;
  TimeUs next_status_us_ = 0;
  TimeUs transfer_started_us_ = 0;
  bool transfer_pending_ = false;
  uint16_t probe_attempts_ = 0;
  uint8_t black_level_ = 0;
};

}  // namespace rivettx
