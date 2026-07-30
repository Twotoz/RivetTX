#include "rivettx/audio.hpp"

#include <algorithm>

namespace rivettx {

namespace {

constexpr uint8_t alert_index(AudioAlert alert)
{
  return static_cast<uint8_t>(alert);
}

constexpr uint32_t alert_bit(AudioAlert alert)
{
  return 1U << alert_index(alert);
}

}  // namespace

AudioAlertScheduler::AudioAlertScheduler(IToneOutput& output)
    : output_(output)
{
}

void AudioAlertScheduler::notify(AudioAlert alert)
{
  if (alert < AudioAlert::Count) {
    pending_.fetch_or(alert_bit(alert), std::memory_order_release);
  }
}

AudioAlertScheduler::Pattern AudioAlertScheduler::pattern_for(
    AudioAlert alert, uint16_t custom_frequency, uint16_t custom_duration)
{
  Pattern result{};
  auto set = [&result](std::initializer_list<Note> notes) {
    result.count =
        static_cast<uint8_t>(std::min(notes.size(), result.notes.size()));
    std::copy_n(notes.begin(), result.count, result.notes.begin());
  };
  switch (alert) {
    case AudioAlert::CustomTone:
      set({{custom_frequency, custom_duration, 0}});
      break;
    case AudioAlert::Startup:
      set({{650, 70, 50}, {900, 70, 50}, {1200, 100, 0}});
      break;
    case AudioAlert::OutputsEnabled:
      set({{700, 90, 50}, {1100, 130, 0}});
      break;
    case AudioAlert::OutputsLocked:
      set({{1100, 90, 50}, {650, 150, 0}});
      break;
    case AudioAlert::TelemetryRecovered:
      set({{750, 60, 40}, {1000, 80, 0}});
      break;
    case AudioAlert::BatteryRecovered:
      set({{550, 70, 40}, {900, 100, 0}});
      break;
    case AudioAlert::LinkRecovered:
      set({{850, 60, 35}, {1150, 90, 0}});
      break;
    case AudioAlert::ModuleRecovered:
      set({{600, 70, 40}, {850, 70, 40}, {1150, 100, 0}});
      break;
    case AudioAlert::BatteryLow:
      set({{520, 140, 100}, {520, 140, 0}});
      break;
    case AudioAlert::TelemetryWarning:
      set({{780, 100, 80}, {780, 100, 0}});
      break;
    case AudioAlert::SafetyFault:
      set({{280, 180, 80}, {280, 180, 80}, {280, 260, 0}});
      break;
    case AudioAlert::LinkWeak:
      set({{950, 70, 70}, {950, 70, 0}});
      break;
    case AudioAlert::ModuleOffline:
      set({{450, 180, 100}, {450, 180, 100}, {450, 180, 0}});
      break;
    case AudioAlert::TelemetryLost:
      set({{400, 300, 150}, {400, 300, 0}});
      break;
    case AudioAlert::BatteryCritical:
      set({{330, 260, 100}, {330, 260, 100}, {330, 260, 0}});
      break;
    case AudioAlert::LinkCritical:
      set({{1250, 70, 45}, {1250, 70, 45}, {1250, 70, 45},
           {1250, 140, 0}});
      break;
    case AudioAlert::Count:
      break;
  }
  return result;
}

AudioAlert AudioAlertScheduler::take_highest_pending()
{
  uint32_t pending = pending_.load(std::memory_order_acquire);
  for (int index = alert_index(AudioAlert::Count) - 1; index >= 0;
       --index) {
    const uint32_t bit = 1U << static_cast<uint8_t>(index);
    if ((pending & bit) == 0) {
      continue;
    }
    pending_.fetch_and(~bit, std::memory_order_acq_rel);
    return static_cast<AudioAlert>(index);
  }
  return AudioAlert::Count;
}

void AudioAlertScheduler::begin(AudioAlert alert, TimeUs now_us)
{
  if (current_ != AudioAlert::Count && tone_active_) {
    output_.stop_tone();
  }
  current_ = alert;
  pattern_ = pattern_for(
      alert, custom_frequency_hz_.load(std::memory_order_acquire),
      custom_duration_ms_.load(std::memory_order_acquire));
  note_index_ = 0;
  tone_active_ = false;
  next_transition_us_ = now_us;
}

void AudioAlertScheduler::tick(TimeUs now_us)
{
  const uint32_t pending = pending_.load(std::memory_order_acquire);
  int highest = -1;
  for (int index = alert_index(AudioAlert::Count) - 1; index >= 0;
       --index) {
    if ((pending & (1U << static_cast<uint8_t>(index))) != 0) {
      highest = index;
      break;
    }
  }
  if (highest >= 0 &&
      (current_ == AudioAlert::Count ||
       highest > static_cast<int>(alert_index(current_)))) {
    begin(take_highest_pending(), now_us);
  }

  if (current_ == AudioAlert::Count || now_us < next_transition_us_) {
    return;
  }
  if (tone_active_) {
    output_.stop_tone();
    tone_active_ = false;
    const uint16_t gap = pattern_.notes[note_index_].gap_ms;
    ++note_index_;
    if (note_index_ >= pattern_.count) {
      current_ = AudioAlert::Count;
      const AudioAlert next = take_highest_pending();
      if (next != AudioAlert::Count) {
        begin(next, now_us);
      }
      return;
    }
    next_transition_us_ =
        now_us + static_cast<TimeUs>(gap) * 1000;
    return;
  }

  if (note_index_ >= pattern_.count) {
    current_ = AudioAlert::Count;
    return;
  }
  const Note& note = pattern_.notes[note_index_];
  (void)output_.play_tone(note.frequency_hz, note.duration_ms);
  tone_active_ = true;
  next_transition_us_ =
      now_us + static_cast<TimeUs>(note.duration_ms) * 1000;
}

bool AudioAlertScheduler::play_tone(uint16_t frequency_hz,
                                    uint16_t duration_ms)
{
  custom_frequency_hz_.store(
      clamp<uint16_t>(100, frequency_hz, 5000),
      std::memory_order_release);
  custom_duration_ms_.store(
      clamp<uint16_t>(1, duration_ms, 5000),
      std::memory_order_release);
  notify(AudioAlert::CustomTone);
  return available();
}

void AudioAlertScheduler::stop_tone()
{
  pending_.fetch_and(~alert_bit(AudioAlert::CustomTone),
                     std::memory_order_acq_rel);
  if (current_ == AudioAlert::CustomTone) {
    if (tone_active_) {
      output_.stop_tone();
    }
    current_ = AudioAlert::Count;
    tone_active_ = false;
  }
}

bool AudioAlertScheduler::available() const
{
  return output_.available();
}

AudioAlert AudioAlertScheduler::current_alert() const
{
  return current_;
}

AudioWarningMonitor::AudioWarningMonitor(AudioWarningConfig config)
    : config_(config)
{
}

bool AudioWarningMonitor::repeat_due(TimeUs now_us, TimeUs& last_us,
                                     uint16_t seconds)
{
  if (last_us == 0 ||
      now_us - last_us >= static_cast<TimeUs>(seconds) * 1000000) {
    last_us = now_us;
    return true;
  }
  return false;
}

void AudioWarningMonitor::evaluate_battery(
    BatteryState battery, TimeUs now_us, AudioAlertScheduler& audio)
{
  if (battery == BatteryState::Low &&
      (battery != previous_battery_ ||
       repeat_due(now_us, last_battery_low_us_,
                  config_.battery_low_repeat_seconds))) {
    last_battery_low_us_ = now_us;
    audio.notify(AudioAlert::BatteryLow);
  } else if (battery == BatteryState::Critical &&
             (battery != previous_battery_ ||
              repeat_due(now_us, last_battery_critical_us_,
                         config_.battery_critical_repeat_seconds))) {
    last_battery_critical_us_ = now_us;
    audio.notify(AudioAlert::BatteryCritical);
  } else if (battery == BatteryState::Normal &&
             (previous_battery_ == BatteryState::Low ||
              previous_battery_ == BatteryState::Critical)) {
    audio.notify(AudioAlert::BatteryRecovered);
  }
  previous_battery_ = battery;
}

void AudioWarningMonitor::evaluate_module(
    ModuleState module, TimeUs now_us, AudioAlertScheduler& audio)
{
  if (module == ModuleState::Online) {
    if (module_seen_online_ && previous_module_ == ModuleState::Offline) {
      audio.notify(AudioAlert::ModuleRecovered);
    }
    module_seen_online_ = true;
  } else if (module == ModuleState::Offline && module_seen_online_ &&
             (previous_module_ != ModuleState::Offline ||
              repeat_due(now_us, last_module_offline_us_,
                         config_.module_offline_repeat_seconds))) {
    last_module_offline_us_ = now_us;
    audio.notify(AudioAlert::ModuleOffline);
  }
  previous_module_ = module;
}

void AudioWarningMonitor::evaluate_safety(
    SafetyState safety, AudioAlertScheduler& audio)
{
  if (safety != previous_safety_) {
    if (safety == SafetyState::Enabled) {
      audio.notify(AudioAlert::OutputsEnabled);
    } else if (safety == SafetyState::Fault) {
      audio.notify(AudioAlert::SafetyFault);
    } else if (previous_safety_ == SafetyState::Enabled) {
      audio.notify(AudioAlert::OutputsLocked);
    }
  }
  previous_safety_ = safety;
}

void AudioWarningMonitor::evaluate_link(
    const TelemetryRegistry& telemetry, bool outputs_enabled,
    TimeUs now_us, AudioAlertScheduler& audio)
{
  const TelemetryEntry* quality =
      telemetry.find(crsf::SensorUplinkLinkQuality);
  const bool fresh =
      quality != nullptr && quality->discovered &&
      now_us >= quality->updated_at_us &&
      now_us - quality->updated_at_us <=
          static_cast<TimeUs>(config_.telemetry_freshness_ms) * 1000;
  if (!outputs_enabled) {
    link_state_ = fresh ? LinkState::Healthy : LinkState::Unknown;
    last_link_warning_us_ = 0;
    outputs_enabled_since_us_ = 0;
    return;
  }
  if (outputs_enabled_since_us_ == 0) {
    outputs_enabled_since_us_ = now_us;
  }

  LinkState next = LinkState::Lost;
  if (fresh) {
    const int32_t link_quality = clamp<int32_t>(0, quality->value, 100);
    const int32_t critical_clear =
        config_.link_critical_percent + config_.link_hysteresis_percent;
    const int32_t weak_clear =
        config_.link_weak_percent + config_.link_hysteresis_percent;
    if (link_quality < config_.link_critical_percent ||
        (link_state_ == LinkState::Critical &&
         link_quality < critical_clear)) {
      next = LinkState::Critical;
    } else if (link_quality < config_.link_weak_percent ||
               (link_state_ == LinkState::Weak &&
                link_quality < weak_clear)) {
      next = LinkState::Weak;
    } else {
      next = LinkState::Healthy;
    }
  } else if (now_us - outputs_enabled_since_us_ <=
             static_cast<TimeUs>(config_.telemetry_freshness_ms) * 1000) {
    next = LinkState::Unknown;
  }

  if (next != link_state_) {
    if (next == LinkState::Healthy &&
        link_state_ != LinkState::Unknown) {
      audio.notify(AudioAlert::LinkRecovered);
    } else if (next == LinkState::Weak) {
      audio.notify(AudioAlert::LinkWeak);
      last_link_warning_us_ = now_us;
    } else if (next == LinkState::Critical) {
      audio.notify(AudioAlert::LinkCritical);
      last_link_warning_us_ = now_us;
    } else if (next == LinkState::Lost) {
      audio.notify(AudioAlert::TelemetryLost);
      last_link_warning_us_ = now_us;
    }
    link_state_ = next;
    return;
  }

  uint16_t repeat_seconds = 0;
  AudioAlert repeated = AudioAlert::Count;
  if (next == LinkState::Weak) {
    repeat_seconds = config_.weak_repeat_seconds;
    repeated = AudioAlert::LinkWeak;
  } else if (next == LinkState::Critical) {
    repeat_seconds = config_.critical_repeat_seconds;
    repeated = AudioAlert::LinkCritical;
  } else if (next == LinkState::Lost) {
    repeat_seconds = config_.lost_repeat_seconds;
    repeated = AudioAlert::TelemetryLost;
  }
  if (repeated != AudioAlert::Count &&
      repeat_due(now_us, last_link_warning_us_, repeat_seconds)) {
    audio.notify(repeated);
  }
}

void AudioWarningMonitor::tick(
    const TelemetryRegistry& telemetry, BatteryState battery,
    ModuleState module, SafetyState safety, TimeUs now_us,
    AudioAlertScheduler& audio)
{
  evaluate_battery(battery, now_us, audio);
  evaluate_module(module, now_us, audio);
  evaluate_safety(safety, audio);
  evaluate_link(telemetry, safety == SafetyState::Enabled, now_us, audio);
}

void AudioWarningMonitor::telemetry_alarm(
    bool active, AudioAlertScheduler& audio)
{
  audio.notify(active ? AudioAlert::TelemetryWarning
                      : AudioAlert::TelemetryRecovered);
}

}  // namespace rivettx
