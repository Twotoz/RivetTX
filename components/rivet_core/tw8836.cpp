#include "rivettx/tw8836.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace rivettx {
namespace {

constexpr std::array<uint32_t, 64> kSha256Constants{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
    0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
    0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
    0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t rotate_right(uint32_t value, uint8_t count)
{
  return (value >> count) | (value << (32U - count));
}

}  // namespace

void Tw8836Sha256Context::transform(const uint8_t* block)
{
    std::array<uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16; ++i) {
      words[i] = (static_cast<uint32_t>(block[i * 4]) << 24U) |
                 (static_cast<uint32_t>(block[i * 4 + 1]) << 16U) |
                 (static_cast<uint32_t>(block[i * 4 + 2]) << 8U) |
                 block[i * 4 + 3];
    }
    for (std::size_t i = 16; i < words.size(); ++i) {
      const uint32_t s0 = rotate_right(words[i - 15], 7) ^
                          rotate_right(words[i - 15], 18) ^
                          (words[i - 15] >> 3U);
      const uint32_t s1 = rotate_right(words[i - 2], 17) ^
                          rotate_right(words[i - 2], 19) ^
                          (words[i - 2] >> 10U);
      words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];
    for (std::size_t i = 0; i < words.size(); ++i) {
      const uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                            rotate_right(e, 25);
      const uint32_t choose = (e & f) ^ ((~e) & g);
      const uint32_t temp1 = h + sum1 + choose + kSha256Constants[i] +
                             words[i];
      const uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                            rotate_right(a, 22);
      const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t temp2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void Tw8836Sha256Context::update(const uint8_t* data, std::size_t size)
{
    if (data == nullptr || size == 0) return;
    bytes += size;
    while (size != 0) {
      const std::size_t count = std::min(size, buffer.size() - buffered);
      std::memcpy(buffer.data() + buffered, data, count);
      buffered += count;
      data += count;
      size -= count;
      if (buffered == buffer.size()) {
        transform(buffer.data());
        buffered = 0;
      }
    }
}

std::array<uint8_t, 32> Tw8836Sha256Context::finish()
{
    const uint64_t bits = bytes * 8U;
    buffer[buffered++] = 0x80;
    if (buffered > 56) {
      std::fill(buffer.begin() + buffered, buffer.end(), 0);
      transform(buffer.data());
      buffered = 0;
    }
    std::fill(buffer.begin() + buffered, buffer.begin() + 56, 0);
    for (std::size_t i = 0; i < 8; ++i) {
      buffer[63 - i] = static_cast<uint8_t>(bits >> (i * 8U));
    }
    transform(buffer.data());
    std::array<uint8_t, 32> digest{};
    for (std::size_t i = 0; i < state.size(); ++i) {
      digest[i * 4] = static_cast<uint8_t>(state[i] >> 24U);
      digest[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16U);
      digest[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8U);
      digest[i * 4 + 3] = static_cast<uint8_t>(state[i]);
    }
  return digest;
}

Tw8836Controller::Tw8836Controller(ITw8836Hardware& hardware,
                                   Tw8836Config config)
    : hardware_(hardware), config_(config)
{
  config_.operation_timeout_ms =
      std::max<uint32_t>(100, config_.operation_timeout_ms);
  config_.status_interval_ms =
      std::max<uint32_t>(20, config_.status_interval_ms);
}

bool Tw8836Controller::initialize(TimeUs now_us)
{
  if (!hardware_.initialize()) {
    fail(Tw8836Fault::Configuration);
    return false;
  }
  status_ = {};
  status_.state = Tw8836State::Detecting;
  set_deadline(now_us);
  return true;
}

bool Tw8836Controller::start_program(
    const uint8_t* image, std::size_t size,
    const std::array<uint8_t, 32>& expected_sha256, TimeUs now_us)
{
  if (status_.state != Tw8836State::Ready || image == nullptr || size == 0 ||
      size > kTw8836FlashSize) {
    return false;
  }
  image_ = image;
  expected_sha256_ = expected_sha256;
  offset_ = 0;
  verify_offset_ = 0;
  verify_started_ = false;
  status_.image_size = static_cast<uint32_t>(size);
  status_.programmed_bytes = 0;
  status_.progress_percent = 0;
  status_.fault = Tw8836Fault::None;
  status_.state = Tw8836State::EnteringIsp;
  set_deadline(now_us);
  return true;
}

bool Tw8836Controller::cancel_program()
{
  if (status_.state < Tw8836State::EnteringIsp ||
      status_.state > Tw8836State::WaitingForBoot) {
    return false;
  }
  (void)hardware_.exit_isp_and_reset();
  fail(Tw8836Fault::Communication);
  return true;
}

bool Tw8836Controller::recover(TimeUs now_us)
{
  if (status_.state != Tw8836State::Fault &&
      status_.state != Tw8836State::Unavailable) {
    return false;
  }
  status_.fault = Tw8836Fault::None;
  status_.state = Tw8836State::Detecting;
  set_deadline(now_us);
  return true;
}

void Tw8836Controller::fail(Tw8836Fault fault)
{
  status_.fault = fault;
  status_.state = Tw8836State::Fault;
}

void Tw8836Controller::set_deadline(TimeUs now_us)
{
  deadline_us_ = now_us +
      static_cast<TimeUs>(config_.operation_timeout_ms) * 1000U;
}

bool Tw8836Controller::timed_out(TimeUs now_us) const
{
  return deadline_us_ != 0 && now_us >= deadline_us_;
}

void Tw8836Controller::update_progress()
{
  status_.programmed_bytes = offset_;
  status_.progress_percent = status_.image_size == 0
      ? 0
      : static_cast<uint8_t>(std::min<uint32_t>(
            100, offset_ * 100U / status_.image_size));
}

void Tw8836Controller::tick(TimeUs now_us)
{
  if (status_.state != Tw8836State::Ready &&
      status_.state != Tw8836State::Complete &&
      status_.state != Tw8836State::Fault && timed_out(now_us)) {
    fail(Tw8836Fault::Timeout);
    return;
  }
  switch (status_.state) {
    case Tw8836State::Detecting: {
      uint16_t identity = 0;
      if (!hardware_.read_identity(identity)) {
        fail(Tw8836Fault::Communication);
      } else if (identity != kTw8836ExpectedIdentity) {
        status_.identity = identity;
        fail(Tw8836Fault::Identity);
      } else {
        status_.identity = identity;
        status_.state = Tw8836State::Ready;
        next_status_us_ = now_us;
      }
      break;
    }
    case Tw8836State::Ready:
    case Tw8836State::Complete:
      if (now_us >= next_status_us_) {
        Tw8836RuntimeStatus runtime{};
        if (hardware_.read_runtime_status(runtime)) {
          status_.runtime = runtime;
        } else {
          fail(Tw8836Fault::Communication);
        }
        next_status_us_ = now_us +
            static_cast<TimeUs>(config_.status_interval_ms) * 1000U;
      }
      break;
    case Tw8836State::EnteringIsp:
      if (!hardware_.enter_isp()) fail(Tw8836Fault::Communication);
      else status_.state = Tw8836State::EnablingErase;
      break;
    case Tw8836State::EnablingErase:
      if (!hardware_.write_enable()) fail(Tw8836Fault::Communication);
      else status_.state = Tw8836State::WaitingForEraseEnable;
      break;
    case Tw8836State::WaitingForEraseEnable: {
      bool busy = false;
      if (!hardware_.flash_busy(busy)) fail(Tw8836Fault::Communication);
      else if (!busy) status_.state = Tw8836State::Erasing;
      break;
    }
    case Tw8836State::Erasing:
      if (!hardware_.start_erase()) fail(Tw8836Fault::Communication);
      else status_.state = Tw8836State::WaitingForErase;
      break;
    case Tw8836State::WaitingForErase: {
      bool busy = false;
      if (!hardware_.flash_busy(busy)) fail(Tw8836Fault::Communication);
      else if (!busy) {
        status_.state = Tw8836State::LoadingBlock;
        set_deadline(now_us);
      }
      break;
    }
    case Tw8836State::LoadingBlock: {
      if (offset_ >= status_.image_size) {
        status_.state = Tw8836State::Verifying;
        set_deadline(now_us);
        break;
      }
      const std::size_t size = std::min<std::size_t>(
          kTw8836IspBlockSize, status_.image_size - offset_);
      if (!hardware_.load_xram_block(image_ + offset_, size)) {
        fail(Tw8836Fault::Communication);
      } else {
        program_block_size_ = size;
        status_.state = Tw8836State::EnablingProgram;
      }
      break;
    }
    case Tw8836State::EnablingProgram:
      if (!hardware_.write_enable()) fail(Tw8836Fault::Communication);
      else status_.state = Tw8836State::WaitingForProgramEnable;
      break;
    case Tw8836State::WaitingForProgramEnable: {
      bool busy = false;
      if (!hardware_.flash_busy(busy)) fail(Tw8836Fault::Communication);
      else if (!busy) status_.state = Tw8836State::Programming;
      break;
    }
    case Tw8836State::Programming:
      if (!hardware_.start_program_flash_block(offset_,
                                               program_block_size_)) {
        fail(Tw8836Fault::Communication);
      } else {
        status_.state = Tw8836State::WaitingForProgram;
      }
      break;
    case Tw8836State::WaitingForProgram: {
      bool busy = false;
      if (!hardware_.flash_busy(busy)) fail(Tw8836Fault::Communication);
      else if (!busy) {
        offset_ += static_cast<uint32_t>(program_block_size_);
        update_progress();
        status_.state = Tw8836State::LoadingBlock;
        set_deadline(now_us);
      }
      break;
    }
    case Tw8836State::Verifying: {
      if (!verify_started_) {
        sha256_ = Tw8836Sha256Context{};
        verify_started_ = true;
      }
      if (verify_offset_ >= status_.image_size) {
        if (sha256_.finish() != expected_sha256_) {
          fail(Tw8836Fault::Checksum);
        } else {
          status_.state = Tw8836State::Rebooting;
          set_deadline(now_us);
        }
        break;
      }
      const std::size_t size = std::min<std::size_t>(
          readback_.size(), status_.image_size - verify_offset_);
      if (!hardware_.begin_read_flash_block(verify_offset_, size)) {
        fail(Tw8836Fault::Readback);
      } else {
        verify_block_size_ = size;
        status_.state = Tw8836State::WaitingForReadback;
      }
      break;
    }
    case Tw8836State::WaitingForReadback: {
      bool busy = false;
      if (!hardware_.flash_busy(busy)) {
        fail(Tw8836Fault::Communication);
      } else if (!busy) {
        if (!hardware_.read_xram_block(readback_.data(),
                                       verify_block_size_)) {
          fail(Tw8836Fault::Readback);
        } else {
          sha256_.update(readback_.data(), verify_block_size_);
          verify_offset_ += static_cast<uint32_t>(verify_block_size_);
          status_.state = Tw8836State::Verifying;
          set_deadline(now_us);
        }
      }
      break;
    }
    case Tw8836State::Rebooting:
      if (!hardware_.exit_isp_and_reset()) fail(Tw8836Fault::Communication);
      else {
        status_.state = Tw8836State::WaitingForBoot;
        set_deadline(now_us);
      }
      break;
    case Tw8836State::WaitingForBoot: {
      Tw8836RuntimeStatus runtime{};
      if (!hardware_.read_runtime_status(runtime)) {
        fail(Tw8836Fault::Communication);
      } else if (runtime.booted && runtime.panel_timing_active) {
        status_.runtime = runtime;
        status_.state = Tw8836State::Complete;
        status_.progress_percent = 100;
        next_status_us_ = now_us +
            static_cast<TimeUs>(config_.status_interval_ms) * 1000U;
      }
      break;
    }
    case Tw8836State::Unavailable:
    case Tw8836State::Fault:
      break;
  }
}

const Tw8836Status& Tw8836Controller::status() const
{
  return status_;
}

std::array<uint8_t, 32> tw8836_sha256(const uint8_t* data, std::size_t size)
{
  Tw8836Sha256Context context{};
  context.update(data, size);
  return context.finish();
}

}  // namespace rivettx
