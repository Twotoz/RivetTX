#pragma once

#include "rivettx/core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace rivettx {

constexpr std::size_t kRemovableStorageQueueDepth = 16;
constexpr std::size_t kRemovableStorageCompletionDepth = 4;
constexpr std::size_t kRemovableStoragePathCapacity = 128;
constexpr std::size_t kRemovableStorageChunkBytes = 512;

enum class RemovableStorageState : uint8_t {
  Absent,
  Debouncing,
  Powering,
  Identifying,
  MountedReadOnly,
  Validating,
  Ready,
  Unsupported,
  Corrupt,
  IoFault,
  Removing,
};

enum class RemovableStorageOperation : uint8_t {
  LogRecord,
  AudioRead,
  ModelExport,
  UpdateRead,
  DiagnosticWrite,
};

enum class RemovableStorageMediaResult : uint8_t {
  Ok,
  Unsupported,
  Corrupt,
  IoError,
};

struct RemovableStorageRequest {
  RemovableStorageOperation operation = RemovableStorageOperation::LogRecord;
  std::array<char, kRemovableStoragePathCapacity> path{};
  uint32_t offset = 0;
  uint32_t size = 0;
  uint32_t token = 0;
  uint16_t payload_size = 0;
  std::array<uint8_t, kRemovableStorageChunkBytes> payload{};
};

struct RemovableStorageCompletion {
  uint32_t token = 0;
  bool success = false;
  uint16_t bytes = 0;
  std::array<uint8_t, kRemovableStorageChunkBytes> data{};
};

struct RemovableUpdateApproval {
  bool manifest_signature_or_hash_valid = false;
  bool user_confirmed = false;
  bool hardware_compatible = false;
  bool version_allowed = false;
  bool post_write_readback_required = true;
};

struct RemovableStorageConfig {
  uint16_t detect_debounce_ms = 80;
  uint16_t power_settle_ms = 10;
  uint16_t retry_cooldown_ms = 1000;
  uint32_t initial_clock_khz = 400;
  uint32_t maximum_clock_khz = 20000;
};

struct RemovableStorageStatus {
  RemovableStorageState state = RemovableStorageState::Absent;
  bool card_present = false;
  bool powered = false;
  bool mounted = false;
  bool writable = false;
  uint8_t queued = 0;
  uint32_t completed = 0;
  uint32_t failed = 0;
  uint32_t dropped_logs = 0;
  uint32_t removals = 0;
};

class IRemovableStorageBackend {
 public:
  virtual ~IRemovableStorageBackend() = default;
  virtual bool set_power(bool enabled) = 0;
  virtual RemovableStorageMediaResult identify(uint32_t initial_clock_khz,
                                                uint32_t maximum_clock_khz) = 0;
  virtual RemovableStorageMediaResult mount(bool read_only) = 0;
  virtual RemovableStorageMediaResult validate_fat32() = 0;
  virtual bool ensure_openpocket_directories() = 0;
  virtual bool unmount() = 0;
  virtual bool execute(const RemovableStorageRequest& request,
                       RemovableStorageCompletion& completion) = 0;
};

bool valid_openpocket_path(const char* path);
bool removable_update_approved(const RemovableUpdateApproval& approval);

class RemovableStorageService {
 public:
  explicit RemovableStorageService(
      IRemovableStorageBackend& backend,
      RemovableStorageConfig config = {});

  void set_card_detect(bool present, TimeUs now_us);
  void tick(TimeUs now_us);
  bool enqueue(const RemovableStorageRequest& request);
  bool take_completion(RemovableStorageCompletion& completion);
  const RemovableStorageStatus& status() const;

 private:
  bool push(const RemovableStorageRequest& request);
  bool pop(RemovableStorageRequest& request);
  bool push_completion(const RemovableStorageCompletion& completion);
  void fail(RemovableStorageState state, TimeUs now_us);
  void remove_card();

  IRemovableStorageBackend& backend_;
  RemovableStorageConfig config_{};
  RemovableStorageStatus status_{};
  std::array<RemovableStorageRequest, kRemovableStorageQueueDepth> queue_{};
  std::size_t read_ = 0;
  std::size_t write_ = 0;
  std::size_t count_ = 0;
  std::array<RemovableStorageCompletion,
             kRemovableStorageCompletionDepth> completions_{};
  std::size_t completion_read_ = 0;
  std::size_t completion_write_ = 0;
  std::size_t completion_count_ = 0;
  bool raw_present_ = false;
  bool debounced_present_ = false;
  TimeUs detect_changed_at_us_ = 0;
  TimeUs state_deadline_us_ = 0;
};

}  // namespace rivettx
