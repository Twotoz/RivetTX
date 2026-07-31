#include "rivettx/removable_storage.hpp"

#include <algorithm>
#include <cstring>

namespace rivettx {

namespace {

constexpr const char kRoot[] = "/OPENPOCKET/";

TimeUs milliseconds(uint32_t value)
{
  return static_cast<TimeUs>(value) * 1000U;
}

}  // namespace

bool valid_openpocket_path(const char* path)
{
  if (path == nullptr) return false;
  std::size_t length = 0;
  while (length < kRemovableStoragePathCapacity && path[length] != '\0') {
    ++length;
  }
  if (length == 0 || length >= kRemovableStoragePathCapacity ||
      length < sizeof(kRoot) - 1 ||
      std::strncmp(path, kRoot, sizeof(kRoot) - 1) != 0) {
    return false;
  }
  if (std::strstr(path, "..") != nullptr || std::strstr(path, "//") != nullptr ||
      std::strchr(path, '\\') != nullptr || std::strchr(path, ':') != nullptr) {
    return false;
  }
  for (std::size_t index = 0; index < length; ++index) {
    const unsigned char value = static_cast<unsigned char>(path[index]);
    if (value < 0x20 || value == 0x7f) return false;
  }
  return true;
}

bool removable_update_approved(const RemovableUpdateApproval& approval)
{
  return approval.manifest_signature_or_hash_valid &&
         approval.user_confirmed && approval.hardware_compatible &&
         approval.version_allowed && approval.post_write_readback_required;
}

RemovableStorageService::RemovableStorageService(
    IRemovableStorageBackend& backend, RemovableStorageConfig config)
    : backend_(backend), config_(config)
{
  config_.detect_debounce_ms =
      std::max<uint16_t>(config_.detect_debounce_ms, 20);
  config_.power_settle_ms = std::max<uint16_t>(config_.power_settle_ms, 1);
  config_.retry_cooldown_ms =
      std::max<uint16_t>(config_.retry_cooldown_ms, 100);
  config_.initial_clock_khz =
      std::clamp<uint32_t>(config_.initial_clock_khz, 100, 1000);
  config_.maximum_clock_khz =
      std::clamp<uint32_t>(config_.maximum_clock_khz,
                           config_.initial_clock_khz, 20000);
}

void RemovableStorageService::set_card_detect(bool present, TimeUs now_us)
{
  if (present != raw_present_) {
    raw_present_ = present;
    detect_changed_at_us_ = now_us;
    if (present && status_.state == RemovableStorageState::Absent) {
      status_.state = RemovableStorageState::Debouncing;
    }
  }
}

bool RemovableStorageService::push(const RemovableStorageRequest& request)
{
  if (count_ == queue_.size()) return false;
  queue_[write_] = request;
  write_ = (write_ + 1) % queue_.size();
  ++count_;
  status_.queued = static_cast<uint8_t>(count_);
  return true;
}

bool RemovableStorageService::pop(RemovableStorageRequest& request)
{
  if (count_ == 0) return false;
  request = queue_[read_];
  read_ = (read_ + 1) % queue_.size();
  --count_;
  status_.queued = static_cast<uint8_t>(count_);
  return true;
}

bool RemovableStorageService::enqueue(const RemovableStorageRequest& request)
{
  if (!valid_openpocket_path(request.path.data()) ||
      request.size > kRemovableStorageChunkBytes ||
      request.payload_size > request.payload.size()) {
    return false;
  }
  if (status_.state != RemovableStorageState::Ready) {
    if (request.operation == RemovableStorageOperation::LogRecord) {
      ++status_.dropped_logs;
    }
    return false;
  }
  if (!push(request)) {
    if (request.operation == RemovableStorageOperation::LogRecord) {
      ++status_.dropped_logs;
    }
    return false;
  }
  return true;
}

bool RemovableStorageService::push_completion(
    const RemovableStorageCompletion& completion)
{
  if (completion_count_ == completions_.size()) return false;
  completions_[completion_write_] = completion;
  completion_write_ = (completion_write_ + 1) % completions_.size();
  ++completion_count_;
  return true;
}

bool RemovableStorageService::take_completion(
    RemovableStorageCompletion& completion)
{
  if (completion_count_ == 0) return false;
  completion = completions_[completion_read_];
  completion_read_ = (completion_read_ + 1) % completions_.size();
  --completion_count_;
  return true;
}

void RemovableStorageService::fail(RemovableStorageState state, TimeUs now_us)
{
  if (status_.mounted) (void)backend_.unmount();
  if (status_.powered) (void)backend_.set_power(false);
  status_.state = state;
  status_.powered = false;
  status_.mounted = false;
  status_.writable = false;
  ++status_.failed;
  state_deadline_us_ = now_us + milliseconds(config_.retry_cooldown_ms);
}

void RemovableStorageService::remove_card()
{
  if (status_.mounted) (void)backend_.unmount();
  if (status_.powered) (void)backend_.set_power(false);
  read_ = 0;
  write_ = 0;
  count_ = 0;
  completion_read_ = 0;
  completion_write_ = 0;
  completion_count_ = 0;
  status_.queued = 0;
  status_.card_present = false;
  status_.powered = false;
  status_.mounted = false;
  status_.writable = false;
  status_.state = RemovableStorageState::Absent;
  ++status_.removals;
}

void RemovableStorageService::tick(TimeUs now_us)
{
  if (raw_present_ != debounced_present_ &&
      now_us - detect_changed_at_us_ >=
          milliseconds(config_.detect_debounce_ms)) {
    debounced_present_ = raw_present_;
    if (!debounced_present_) {
      remove_card();
      return;
    }
    status_.card_present = true;
    status_.state = RemovableStorageState::Powering;
    if (!backend_.set_power(true)) {
      fail(RemovableStorageState::IoFault, now_us);
      return;
    }
    status_.powered = true;
    state_deadline_us_ = now_us + milliseconds(config_.power_settle_ms);
    return;
  }
  if (!debounced_present_) return;

  switch (status_.state) {
    case RemovableStorageState::Powering:
      if (now_us >= state_deadline_us_) {
        status_.state = RemovableStorageState::Identifying;
      }
      break;
    case RemovableStorageState::Identifying: {
      const auto result = backend_.identify(config_.initial_clock_khz,
                                             config_.maximum_clock_khz);
      if (result == RemovableStorageMediaResult::Ok) {
        const auto mounted = backend_.mount(true);
        if (mounted == RemovableStorageMediaResult::Ok) {
          status_.state = RemovableStorageState::MountedReadOnly;
          status_.mounted = true;
        } else {
          fail(mounted == RemovableStorageMediaResult::Corrupt
                   ? RemovableStorageState::Corrupt
                   : RemovableStorageState::IoFault,
               now_us);
        }
      } else {
        fail(result == RemovableStorageMediaResult::Unsupported
                 ? RemovableStorageState::Unsupported
                 : RemovableStorageState::IoFault,
             now_us);
      }
      break;
    }
    case RemovableStorageState::MountedReadOnly:
      status_.state = RemovableStorageState::Validating;
      break;
    case RemovableStorageState::Validating: {
      const auto result = backend_.validate_fat32();
      if (result != RemovableStorageMediaResult::Ok) {
        (void)backend_.unmount();
        status_.mounted = false;
        fail(result == RemovableStorageMediaResult::Unsupported
                 ? RemovableStorageState::Unsupported
                 : (result == RemovableStorageMediaResult::Corrupt
                        ? RemovableStorageState::Corrupt
                        : RemovableStorageState::IoFault),
             now_us);
        break;
      }
      if (!backend_.unmount()) {
        fail(RemovableStorageState::IoFault, now_us);
        break;
      }
      status_.mounted = false;
      const auto mounted = backend_.mount(false);
      status_.mounted = mounted == RemovableStorageMediaResult::Ok;
      if (!status_.mounted || !backend_.ensure_openpocket_directories()) {
        fail(mounted == RemovableStorageMediaResult::Corrupt
                 ? RemovableStorageState::Corrupt
                 : RemovableStorageState::IoFault,
             now_us);
        break;
      }
      status_.state = RemovableStorageState::Ready;
      status_.mounted = true;
      status_.writable = true;
      break;
    }
    case RemovableStorageState::Ready: {
      RemovableStorageRequest request{};
      if (pop(request)) {
        RemovableStorageCompletion completion{};
        completion.token = request.token;
        completion.success = backend_.execute(request, completion);
        const bool completion_required =
            request.operation != RemovableStorageOperation::LogRecord;
        if (completion.success &&
            (!completion_required || push_completion(completion))) {
          ++status_.completed;
        } else {
          ++status_.failed;
        }
      }
      break;
    }
    case RemovableStorageState::Unsupported:
    case RemovableStorageState::Corrupt:
    case RemovableStorageState::IoFault:
      // Faulted media stays isolated. A physical remove/reinsert is the
      // explicit retry boundary, avoiding brownout-like retry loops.
      break;
    case RemovableStorageState::Absent:
    case RemovableStorageState::Debouncing:
    case RemovableStorageState::Removing:
      break;
  }
}

const RemovableStorageStatus& RemovableStorageService::status() const
{
  return status_;
}

}  // namespace rivettx
