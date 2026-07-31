#include "rivettx/at7456e.hpp"

#include <algorithm>

namespace rivettx {

namespace {

constexpr uint8_t kVm0 = 0x00;
constexpr uint8_t kVm0Read = 0x80;
constexpr uint8_t kDmm = 0x04;
constexpr uint8_t kDmah = 0x05;
constexpr uint8_t kDmal = 0x06;
constexpr uint8_t kCmm = 0x08;
constexpr uint8_t kCmah = 0x09;
constexpr uint8_t kCmal = 0x0A;
constexpr uint8_t kCmdi = 0x0B;
constexpr uint8_t kOsdbl = 0x6C;
constexpr uint8_t kOsdblRead = 0xEC;
constexpr uint8_t kStatusRead = 0xA0;

constexpr uint8_t kVm0Pal = 0x40;
constexpr uint8_t kVm0OsdEnable = 0x08;
constexpr uint8_t kVm0Reset = 0x02;
constexpr uint8_t kDmmAutoIncrement = 0x01;
constexpr uint8_t kAutoIncrementEnd = 0xFF;
constexpr uint8_t kCmmWriteNvm = 0xA0;

constexpr uint8_t kStatusResetBusy = 0x40;
constexpr uint8_t kStatusNvmBusy = 0x20;
constexpr uint8_t kStatusLossOfSync = 0x04;
constexpr uint8_t kStatusPal = 0x02;
constexpr uint8_t kStatusNtsc = 0x01;

uint8_t character_code(char value)
{
  const uint8_t code = static_cast<uint8_t>(
      static_cast<unsigned char>(value));
  return code == kAutoIncrementEnd ? static_cast<uint8_t>('?') : code;
}

}  // namespace

At7456eDriver::At7456eDriver(IAt7456eSpi& spi,
                             At7456eVideoStandard no_video_fallback)
    : spi_(spi),
      fallback_(no_video_fallback == At7456eVideoStandard::Ntsc
                    ? At7456eVideoStandard::Ntsc
                    : At7456eVideoStandard::Pal)
{
  desired_.cells.fill(' ');
  invalidate_shadow();
}

void At7456eDriver::start(TimeUs now_us)
{
  if (transfer_pending_) {
    spi_.abort();
  }
  transfer_pending_ = false;
  operation_ = Operation::None;
  phase_ = Phase::Reset;
  probe_attempts_ = 0;
  phase_deadline_us_ = now_us;
  next_status_us_ = now_us;
  active_standard_ = At7456eVideoStandard::Unknown;
  status_.standard = At7456eVideoStandard::Unknown;
  status_.visible_rows = 0;
  status_.initialized = false;
  status_.healthy = false;
  status_.video_present = false;
  status_.character_upload_active = false;
  invalidate_shadow();
}

void At7456eDriver::submit(const CharacterOsdFrame& frame)
{
  desired_ = frame;
  update_pending_count();
}

bool At7456eDriver::upload(const At7456eCustomCharacter& character)
{
  if (glyph_count_ >= glyphs_.size()) {
    return false;
  }
  glyphs_[glyph_write_] = character;
  glyph_write_ = (glyph_write_ + 1) % glyphs_.size();
  ++glyph_count_;
  status_.character_upload_active = true;
  return true;
}

void At7456eDriver::tick(TimeUs now_us)
{
  if (phase_ == Phase::Stopped || now_us < phase_deadline_us_) {
    return;
  }

  if (transfer_pending_) {
    const At7456eTransferState transfer = spi_.poll();
    if (transfer == At7456eTransferState::Busy) {
      if (now_us - transfer_started_us_ >= kTransferTimeoutUs) {
        fail(now_us);
      }
      return;
    }
    transfer_pending_ = false;
    if (transfer == At7456eTransferState::Failed) {
      fail(now_us);
      return;
    }
    complete_operation(now_us);
    return;
  }

  switch (phase_) {
    case Phase::Stopped:
      return;
    case Phase::Reset:
      (void)queue_write(kVm0, kVm0Reset, Operation::Reset, now_us);
      return;
    case Phase::ResetDelay:
      phase_ = Phase::ReadBlackLevel;
      (void)queue_read(kOsdblRead, Operation::ReadBlackLevel, now_us);
      return;
    case Phase::ReadBlackLevel:
      (void)queue_read(kOsdblRead, Operation::ReadBlackLevel, now_us);
      return;
    case Phase::WriteBlackLevel:
      (void)queue_write(kOsdbl, black_level_,
                        Operation::WriteBlackLevel, now_us);
      return;
    case Phase::Probe:
      (void)queue_status(Operation::Status, now_us);
      return;
    case Phase::Configure:
      (void)queue_write(kVm0, video_mode(true), Operation::Configure,
                        now_us);
      return;
    case Phase::VerifyConfigure:
      (void)queue_read(kVm0Read, Operation::VerifyConfigure, now_us);
      return;
    case Phase::Ready:
      if (now_us >= next_status_us_) {
        (void)queue_status(Operation::Status, now_us);
      } else if (glyph_count_ != 0) {
        phase_ = Phase::GlyphDisable;
        status_.character_upload_active = true;
      } else {
        (void)prepare_cells();
      }
      return;
    case Phase::CellAddressHigh: {
      const uint16_t address = static_cast<uint16_t>(sent_begin_);
      (void)queue_write(kDmah,
                        static_cast<uint8_t>((address >> 8) & 0x01U),
                        Operation::CellAddressHigh, now_us);
      return;
    }
    case Phase::CellAddressLow:
      (void)queue_write(kDmal, static_cast<uint8_t>(sent_begin_ & 0xFFU),
                        Operation::CellAddressLow, now_us);
      return;
    case Phase::CellMode:
      (void)queue_write(kDmm, kDmmAutoIncrement,
                        Operation::CellMode, now_us);
      return;
    case Phase::CellData:
      (void)queue_raw(sent_cells_[sent_progress_],
                      Operation::CellData, now_us);
      return;
    case Phase::CellEnd:
      (void)queue_raw(kAutoIncrementEnd, Operation::CellEnd, now_us);
      return;
    case Phase::GlyphDisable:
      (void)queue_write(kVm0, video_mode(false),
                        Operation::GlyphDisable, now_us);
      return;
    case Phase::GlyphAddress:
      (void)queue_write(kCmah, glyphs_[glyph_read_].index,
                        Operation::GlyphAddress, now_us);
      return;
    case Phase::GlyphDataAddress:
      (void)queue_write(kCmal, static_cast<uint8_t>(glyph_byte_),
                        Operation::GlyphDataAddress, now_us);
      return;
    case Phase::GlyphDataValue:
      (void)queue_write(kCmdi, glyphs_[glyph_read_].pixels[glyph_byte_],
                        Operation::GlyphDataValue, now_us);
      return;
    case Phase::GlyphCommit:
      (void)queue_write(kCmm, kCmmWriteNvm,
                        Operation::GlyphCommit, now_us);
      return;
    case Phase::GlyphWait:
      (void)queue_status(Operation::GlyphStatus, now_us);
      return;
    case Phase::GlyphEnable:
      (void)queue_write(kVm0, video_mode(true),
                        Operation::GlyphEnable, now_us);
      return;
  }
}

const At7456eStatus& At7456eDriver::status() const
{
  return status_;
}

bool At7456eDriver::queue_write(uint8_t address, uint8_t value,
                                Operation operation, TimeUs now_us)
{
  transmit_[0] = address;
  transmit_[1] = value;
  receive_.fill(0);
  transfer_size_ = 2;
  operation_ = operation;
  if (!spi_.queue(transmit_.data(), receive_.data(), transfer_size_)) {
    fail(now_us);
    return false;
  }
  transfer_pending_ = true;
  transfer_started_us_ = now_us;
  return true;
}

bool At7456eDriver::queue_status(Operation operation, TimeUs now_us)
{
  return queue_read(kStatusRead, operation, now_us);
}

bool At7456eDriver::queue_read(uint8_t address, Operation operation,
                               TimeUs now_us)
{
  transmit_[0] = address;
  transmit_[1] = 0;
  receive_.fill(0);
  transfer_size_ = 2;
  operation_ = operation;
  if (!spi_.queue(transmit_.data(), receive_.data(), transfer_size_)) {
    fail(now_us);
    return false;
  }
  transfer_pending_ = true;
  transfer_started_us_ = now_us;
  return true;
}

bool At7456eDriver::queue_raw(uint8_t value, Operation operation,
                              TimeUs now_us)
{
  transmit_[0] = value;
  receive_.fill(0);
  transfer_size_ = 1;
  operation_ = operation;
  if (!spi_.queue(transmit_.data(), receive_.data(), transfer_size_)) {
    fail(now_us);
    return false;
  }
  transfer_pending_ = true;
  transfer_started_us_ = now_us;
  return true;
}

bool At7456eDriver::prepare_cells()
{
  const std::size_t limit = kOsdColumns * visible_rows();
  std::size_t begin = limit;
  for (std::size_t index = 0; index < limit; ++index) {
    const uint8_t desired = character_code(desired_.cells[index]);
    if (!shadow_valid_[index] || shadow_[index] != desired) {
      begin = index;
      break;
    }
  }
  if (begin == limit) {
    status_.pending_characters = 0;
    return false;
  }

  std::size_t count = 0;
  while (begin + count < limit && count < kMaximumRun) {
    const std::size_t index = begin + count;
    const uint8_t desired = character_code(desired_.cells[index]);
    if (count != 0 && shadow_valid_[index] && shadow_[index] == desired) {
      break;
    }
    sent_cells_[count++] = desired;
  }

  sent_begin_ = begin;
  sent_count_ = count;
  sent_progress_ = 0;
  phase_ = Phase::CellAddressHigh;
  return true;
}

void At7456eDriver::complete_operation(TimeUs now_us)
{
  const Operation completed = operation_;
  operation_ = Operation::None;
  switch (completed) {
    case Operation::None:
      return;
    case Operation::Reset:
      phase_ = Phase::ResetDelay;
      phase_deadline_us_ = now_us + 200;
      return;
    case Operation::ReadBlackLevel:
      black_level_ = static_cast<uint8_t>(receive_[1] & ~0x10U);
      phase_ = Phase::WriteBlackLevel;
      return;
    case Operation::WriteBlackLevel:
      phase_ = Phase::Probe;
      return;
    case Operation::Status:
      accept_status(receive_[1], now_us);
      return;
    case Operation::Configure:
      phase_ = Phase::VerifyConfigure;
      return;
    case Operation::VerifyConfigure:
      if ((receive_[1] & 0x49U) != video_mode(true)) {
        fail(now_us);
        return;
      }
      status_.initialized = true;
      status_.healthy = true;
      status_.visible_rows = visible_rows();
      phase_ = Phase::Ready;
      phase_deadline_us_ = now_us;
      next_status_us_ = now_us + kStatusPeriodUs;
      invalidate_shadow();
      return;
    case Operation::CellAddressHigh:
      phase_ = Phase::CellAddressLow;
      return;
    case Operation::CellAddressLow:
      phase_ = Phase::CellMode;
      return;
    case Operation::CellMode:
      phase_ = Phase::CellData;
      return;
    case Operation::CellData:
      ++sent_progress_;
      phase_ = sent_progress_ < sent_count_ ? Phase::CellData
                                           : Phase::CellEnd;
      return;
    case Operation::CellEnd:
      for (std::size_t index = 0; index < sent_count_; ++index) {
        shadow_[sent_begin_ + index] = sent_cells_[index];
        shadow_valid_[sent_begin_ + index] = true;
      }
      update_pending_count();
      phase_ = Phase::Ready;
      return;
    case Operation::GlyphDisable:
      glyph_byte_ = 0;
      phase_ = Phase::GlyphAddress;
      return;
    case Operation::GlyphAddress:
      phase_ = Phase::GlyphDataAddress;
      return;
    case Operation::GlyphDataAddress:
      phase_ = Phase::GlyphDataValue;
      return;
    case Operation::GlyphDataValue:
      ++glyph_byte_;
      phase_ = glyph_byte_ < kAt7456eGlyphBytes
                   ? Phase::GlyphDataAddress
                   : Phase::GlyphCommit;
      return;
    case Operation::GlyphCommit:
      phase_ = Phase::GlyphWait;
      phase_deadline_us_ = now_us + 1000;
      return;
    case Operation::GlyphStatus:
      if ((receive_[1] & kStatusNvmBusy) != 0) {
        phase_ = Phase::GlyphWait;
        phase_deadline_us_ = now_us + 1000;
      } else {
        glyph_read_ = (glyph_read_ + 1) % glyphs_.size();
        --glyph_count_;
        glyph_byte_ = 0;
        phase_ = glyph_count_ == 0 ? Phase::GlyphEnable
                                   : Phase::GlyphAddress;
      }
      return;
    case Operation::GlyphEnable:
      phase_ = Phase::Ready;
      status_.character_upload_active = glyph_count_ != 0;
      next_status_us_ = now_us;
      return;
  }
}

void At7456eDriver::accept_status(uint8_t value, TimeUs now_us)
{
  if (value == 0xFF) {
    fail(now_us);
    return;
  }
  if ((value & kStatusResetBusy) != 0) {
    if (++probe_attempts_ > 100) {
      fail(now_us);
      return;
    }
    phase_ = Phase::Probe;
    phase_deadline_us_ = now_us + 1000;
    return;
  }
  probe_attempts_ = 0;

  const bool video_present = (value & kStatusLossOfSync) == 0;
  At7456eVideoStandard detected = At7456eVideoStandard::Unknown;
  if (video_present && (value & kStatusPal) != 0) {
    detected = At7456eVideoStandard::Pal;
  } else if (video_present && (value & kStatusNtsc) != 0) {
    detected = At7456eVideoStandard::Ntsc;
  }

  const bool recovered = video_present && !status_.video_present;
  status_.video_present = video_present;
  status_.standard = detected;
  const At7456eVideoStandard selected =
      detected == At7456eVideoStandard::Unknown
          ? (active_standard_ == At7456eVideoStandard::Unknown
                 ? fallback_
                 : active_standard_)
          : detected;
  if (selected != active_standard_) {
    active_standard_ = selected;
    status_.visible_rows = visible_rows();
    phase_ = Phase::Configure;
    invalidate_shadow();
    return;
  }
  if (!status_.initialized) {
    phase_ = Phase::Configure;
    return;
  }
  if (recovered) {
    invalidate_shadow();
  }
  status_.healthy = true;
  phase_ = Phase::Ready;
  next_status_us_ = now_us + kStatusPeriodUs;
}

void At7456eDriver::fail(TimeUs now_us)
{
  spi_.abort();
  transfer_pending_ = false;
  operation_ = Operation::None;
  ++status_.communication_failures;
  status_.initialized = false;
  status_.healthy = false;
  status_.video_present = false;
  status_.standard = At7456eVideoStandard::Unknown;
  status_.visible_rows = 0;
  active_standard_ = At7456eVideoStandard::Unknown;
  probe_attempts_ = 0;
  phase_ = Phase::Reset;
  phase_deadline_us_ = now_us + kRetryDelayUs;
  invalidate_shadow();
}

void At7456eDriver::invalidate_shadow()
{
  shadow_valid_.fill(false);
  update_pending_count();
}

void At7456eDriver::update_pending_count()
{
  const std::size_t rows = visible_rows();
  if (rows == 0) {
    status_.pending_characters = kOsdColumns * kOsdRows;
    return;
  }
  std::size_t pending = 0;
  const std::size_t limit = kOsdColumns * rows;
  for (std::size_t index = 0; index < limit; ++index) {
    const uint8_t desired = character_code(desired_.cells[index]);
    if (!shadow_valid_[index] || shadow_[index] != desired) {
      ++pending;
    }
  }
  status_.pending_characters = pending;
}

uint8_t At7456eDriver::video_mode(bool display_enabled) const
{
  const bool pal = configured_standard() == At7456eVideoStandard::Pal;
  return static_cast<uint8_t>((pal ? kVm0Pal : 0) |
                              (display_enabled ? kVm0OsdEnable : 0));
}

At7456eVideoStandard At7456eDriver::configured_standard() const
{
  return active_standard_ == At7456eVideoStandard::Unknown
             ? fallback_
             : active_standard_;
}

std::size_t At7456eDriver::visible_rows() const
{
  if (configured_standard() == At7456eVideoStandard::Ntsc) {
    return kAt7456eNtscRows;
  }
  return kOsdRows;
}

}  // namespace rivettx
