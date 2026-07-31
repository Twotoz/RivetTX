#include "rivettx/core.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rivettx {

namespace {

constexpr TimeUs kSwitchDebounceUs = 20000;
constexpr TimeUs kTrimInitialRepeatUs = 500000;
constexpr TimeUs kTrimRepeatUs = 100000;
constexpr int16_t kTrimStep = 8;
constexpr int16_t kTrimLimit = 512;

constexpr int16_t scale_percent(int32_t value, int16_t percent)
{
  return static_cast<int16_t>(
      clamp<int32_t>(-kResolution,
                     (value * static_cast<int32_t>(percent)) / 100,
                     kResolution));
}

bool flight_mode_enabled(uint8_t mask, uint8_t flight_mode)
{
  return (mask & static_cast<uint8_t>(1U << flight_mode)) != 0;
}

}  // namespace

Model make_default_model()
{
  Model model{};
  constexpr char name[] = "Default";
  std::copy(name, name + sizeof(name), model.name.begin());
  model.input_count = 8;
  model.mix_count = 12;
  model.flight_mode_count = 1;
  model.flight_modes[0].enabled = true;
  model.flight_modes[0].condition.index = -1;

  for (uint8_t axis = 0; axis < 4; ++axis) {
    model.inputs[axis].enabled = true;
    model.inputs[axis].source_axis = axis;
    model.inputs[axis].destination = axis;
    model.inputs[axis].weight_percent = 100;
    model.mixes[axis].enabled = true;
    model.mixes[axis].destination = axis;
    model.mixes[axis].source = {SourceKind::Input, axis, 0};
    model.mixes[axis].weight_percent = 100;
  }
  for (uint8_t aux = 0; aux < kAuxSwitchCount; ++aux) {
    const auto mix = static_cast<uint8_t>(4 + aux);
    model.mixes[mix].enabled = true;
    model.mixes[mix].destination = mix;
    model.mixes[mix].source = {
        SourceKind::Switch,
        static_cast<uint8_t>(kFirstAuxSwitch + aux), 0};
    model.mixes[mix].weight_percent = 100;
  }
  for (uint8_t axis = 4; axis < kMaxAxes; ++axis) {
    model.inputs[axis].enabled = true;
    model.inputs[axis].source_axis = axis;
    model.inputs[axis].destination = axis;
    model.inputs[axis].weight_percent = 100;
    const auto mix = static_cast<uint8_t>(8 + axis - 4);
    model.mixes[mix].enabled = true;
    model.mixes[mix].destination = mix;
    model.mixes[mix].source = {SourceKind::Input, axis, 0};
    model.mixes[mix].weight_percent = 100;
  }
  model.required_switch_mask =
      static_cast<uint8_t>(1U << kFirstAuxSwitch);
  model.outputs[model.throttle_channel].failsafe = -1024;
  model.outputs[4].failsafe = -1024;
  return model;
}

InputProcessor::InputProcessor()
{
  for (auto& item : calibration_) {
    item = AxisCalibration{};
  }
}

void InputProcessor::set_calibration(
    const std::array<AxisCalibration, kMaxAxes>& calibration)
{
  calibration_ = calibration;
  initialized_ = false;
}

const std::array<AxisCalibration, kMaxAxes>& InputProcessor::calibration() const
{
  return calibration_;
}

ControlInputs InputProcessor::process(const RawInputs& raw)
{
  ControlInputs result{};
  result.sampled_at_us = raw.sampled_at_us;
  result.valid = raw.valid;

  std::array<int8_t, kMaxSwitches> sampled_positions{};
  for (std::size_t i = 0; i < sampled_positions.size(); ++i) {
    sampled_positions[i] =
        raw.switch_positions_valid
            ? clamp<int8_t>(-1, raw.switch_positions[i], 1)
            : static_cast<int8_t>(raw.switches[i] ? 1 : -1);
  }
  if (!switches_initialized_) {
    switch_candidates_ = sampled_positions;
    stable_switches_ = sampled_positions;
    switch_changed_at_us_.fill(raw.sampled_at_us);
    switches_initialized_ = true;
  } else if (raw.valid) {
    for (std::size_t i = 0; i < sampled_positions.size(); ++i) {
      if (sampled_positions[i] != switch_candidates_[i]) {
        switch_candidates_[i] = sampled_positions[i];
        switch_changed_at_us_[i] = raw.sampled_at_us;
      } else if (stable_switches_[i] != switch_candidates_[i] &&
                 raw.sampled_at_us >= switch_changed_at_us_[i] &&
                 raw.sampled_at_us - switch_changed_at_us_[i] >=
                     kSwitchDebounceUs) {
        stable_switches_[i] = switch_candidates_[i];
      }
    }
  }
  result.switch_positions = stable_switches_;
  for (std::size_t i = 0; i < result.switches.size(); ++i) {
    result.switches[i] = result.switch_positions[i] > 0;
  }

  for (std::size_t i = 0; i < kMaxAxes; ++i) {
    const auto& cal = calibration_[i];
    const int32_t sample = raw.axes[i];
    const int32_t distance = sample - cal.center;
    const int32_t span =
        distance >= 0 ? cal.maximum - cal.center : cal.center - cal.minimum;

    if (span <= static_cast<int32_t>(cal.deadband) ||
        std::abs(distance) <= static_cast<int32_t>(cal.deadband)) {
      result.axes[i] = 0;
      continue;
    }

    const int32_t adjusted =
        distance > 0 ? distance - cal.deadband : distance + cal.deadband;
    const int32_t usable_span = span - cal.deadband;
    int32_t normalized =
        usable_span > 0 ? adjusted * kResolution / usable_span : 0;
    normalized = clamp<int32_t>(-kResolution, normalized, kResolution);
    if (cal.inverted) {
      normalized = -normalized;
    }

    if (!initialized_) {
      filtered_[i] = normalized;
    } else {
      const int32_t alpha = clamp<int32_t>(0, cal.filter_percent, 100);
      filtered_[i] += (normalized - filtered_[i]) * alpha / 100;
    }
    result.axes[i] = static_cast<int16_t>(filtered_[i]);
  }

  initialized_ = true;
  return result;
}

MixerEngine::MixerEngine()
{
  reset();
}

void MixerEngine::reset()
{
  std::array<TimerState, kMaxTimers> persistent_states{};
  for (std::size_t i = 0; i < timer_states_.size(); ++i) {
    if (timer_persistent_[i]) {
      persistent_states[i] = timer_states_[i];
    }
  }
  mix_runtime_ = {};
  logical_runtime_ = {};
  logical_values_ = {};
  timer_states_ = persistent_states;
  timer_initialized_ = timer_persistent_;
  previous_channels_ = {};
  previous_evaluation_us_ = 0;
  previous_timer_us_ = 0;
  sequence_ = 0;
  active_flight_mode_ = 0;
  previous_flight_mode_ = 0;
  flight_mode_transition_us_ = 0;
  flight_mode_fade_duration_us_ = 0;
  flight_mode_initialized_ = false;
}

void MixerEngine::reset_for_model_change()
{
  timer_states_ = {};
  timer_initialized_ = {};
  timer_persistent_ = {};
  timer_start_ms_ = {};
  reset();
}

int16_t MixerEngine::apply_expo(int16_t input, int8_t percent) const
{
  const int32_t x = clamp<int32_t>(-kResolution, input, kResolution);
  const int32_t cubic =
      ((x * x) / kResolution * x) / kResolution;
  const int32_t amount = clamp<int32_t>(-100, percent, 100);
  return static_cast<int16_t>(
      clamp<int32_t>(-kResolution,
                     x + ((cubic - x) * amount) / 100,
                     kResolution));
}

int16_t MixerEngine::apply_curve(int16_t input, const Curve& curve) const
{
  if (!curve.enabled) {
    return input;
  }

  const int32_t clamped = clamp<int32_t>(-kResolution, input, kResolution);
  constexpr int32_t segments = static_cast<int32_t>(kCurvePoints - 1);
  constexpr int32_t range = kResolution * 2;
  const int32_t position = (clamped + kResolution) * segments;
  const int32_t index = clamp<int32_t>(0, position / range, segments - 1);
  const int32_t remainder = position - index * range;
  const int32_t a = curve.points[static_cast<std::size_t>(index)];
  const int32_t b = curve.points[static_cast<std::size_t>(index + 1)];
  return static_cast<int16_t>(
      clamp<int32_t>(-kResolution,
                     a + ((b - a) * remainder) / range,
                     kResolution));
}

bool MixerEngine::switch_value(const SwitchRef& ref,
                               const ControlInputs& controls) const
{
  bool value = true;
  if (ref.index >= 0) {
    const auto index = static_cast<std::size_t>(ref.index);
    if (index >= controls.switches.size()) {
      value = false;
    } else {
      switch (ref.position) {
        case SwitchPosition::Active:
          value = controls.switches[index];
          break;
        case SwitchPosition::Low:
          value = controls.switch_positions[index] < 0;
          break;
        case SwitchPosition::Middle:
          value = controls.switch_positions[index] == 0;
          break;
        case SwitchPosition::High:
          value = controls.switch_positions[index] > 0;
          break;
      }
    }
  }
  return ref.inverted ? !value : value;
}

bool MixerEngine::switch_value_with_logic(
    const SwitchRef& ref, const ControlInputs& controls) const
{
  bool value = true;
  if (ref.index >= 0 && ref.index < static_cast<int8_t>(kMaxSwitches)) {
    return switch_value(ref, controls);
  } else if (ref.index >= static_cast<int8_t>(kMaxSwitches)) {
    const int logical_index = ref.index - static_cast<int8_t>(kMaxSwitches);
    value = logical_index >= 0 &&
            logical_index < static_cast<int>(logical_values_.size()) &&
            logical_values_[static_cast<std::size_t>(logical_index)];
  }
  return ref.inverted ? !value : value;
}

int16_t MixerEngine::source_value(
    const SourceRef& source, const Model& model,
    const ControlInputs& controls,
    const std::array<int16_t, kMaxInputs>& virtual_inputs,
    const std::array<int32_t, kChannelCount>& channels,
    const ITelemetrySource& telemetry, uint8_t flight_mode) const
{
  switch (source.kind) {
    case SourceKind::Axis:
      return source.index < controls.axes.size() ? controls.axes[source.index]
                                                 : 0;
    case SourceKind::Input:
      return source.index < virtual_inputs.size()
                 ? virtual_inputs[source.index]
                 : 0;
    case SourceKind::Channel:
      return source.index < channels.size()
                 ? static_cast<int16_t>(clamp<int32_t>(
                       -kResolution, channels[source.index], kResolution))
                 : 0;
    case SourceKind::Constant:
      return clamp<int16_t>(-kResolution, source.constant, kResolution);
    case SourceKind::Telemetry: {
      int32_t result = 0;
      return telemetry.value(source.index, result)
                 ? static_cast<int16_t>(
                       clamp<int32_t>(-kResolution, result, kResolution))
                 : 0;
    }
    case SourceKind::GVar:
      if (source.index < kMaxGVars) {
        const auto& mode = model.flight_modes[flight_mode];
        const uint16_t bit =
            static_cast<uint16_t>(1U << source.index);
        const int16_t value =
            (mode.gvar_override_mask & bit) != 0
                ? mode.gvars[source.index]
                : model.gvars[source.index];
        return clamp<int16_t>(-kResolution, value, kResolution);
      }
      return 0;
    case SourceKind::Switch:
      return source.index < controls.switch_positions.size()
                 ? static_cast<int16_t>(
                       controls.switch_positions[source.index] * kResolution)
                 : 0;
  }
  return 0;
}

TrimUpdate TrimController::update(Model& model, uint8_t flight_mode,
                                  const ControlInputs& inputs, TimeUs now_us)
{
  TrimUpdate update{};
  if (!inputs.valid || flight_mode >= model.flight_mode_count ||
      flight_mode >= kMaxFlightModes) {
    return update;
  }
  std::array<int8_t, kTrimAxisCount> directions{};
  for (std::size_t axis = 0; axis < directions.size(); ++axis) {
    const bool negative =
        inputs.switches[kFirstTrimSwitch + axis * 2];
    const bool positive =
        inputs.switches[kFirstTrimSwitch + axis * 2 + 1];
    directions[axis] =
        negative && positive ? 2 : (negative ? -1 : (positive ? 1 : 0));
  }
  if (!initialized_) {
    previous_direction_ = directions;
    initialized_ = true;
    return update;
  }

  auto& trims = model.flight_modes[flight_mode].trims;
  for (std::size_t axis = 0; axis < directions.size(); ++axis) {
    const int8_t direction = directions[axis];
    bool apply = false;
    if (direction != previous_direction_[axis]) {
      apply = direction != 0;
      next_repeat_us_[axis] =
          direction == -1 || direction == 1
              ? now_us + kTrimInitialRepeatUs
              : 0;
    } else if ((direction == -1 || direction == 1) &&
               next_repeat_us_[axis] != 0 &&
               now_us >= next_repeat_us_[axis]) {
      apply = true;
      next_repeat_us_[axis] = now_us + kTrimRepeatUs;
    }
    previous_direction_[axis] = direction;
    if (!apply) {
      continue;
    }

    const int16_t before = trims[axis];
    const int16_t after =
        direction == 2
            ? 0
            : static_cast<int16_t>(clamp<int32_t>(
                  -kTrimLimit,
                  static_cast<int32_t>(before) + direction * kTrimStep,
                  kTrimLimit));
    trims[axis] = after;
    if (after != before) {
      const uint8_t bit = static_cast<uint8_t>(1U << axis);
      update.changed_mask |= bit;
      if (after == 0) {
        update.centered_mask |= bit;
      }
      if (after == -kTrimLimit || after == kTrimLimit) {
        update.limit_mask |= bit;
      }
    }
  }
  return update;
}

void TrimController::reset()
{
  previous_direction_ = {};
  next_repeat_us_ = {};
  initialized_ = false;
}

int8_t RotaryEncoderDecoder::update(bool phase_a, bool phase_b)
{
  const uint8_t current = static_cast<uint8_t>(
      (phase_a ? 2U : 0U) | (phase_b ? 1U : 0U));
  if (!initialized_) {
    previous_state_ = current;
    initialized_ = true;
    return 0;
  }

  static constexpr std::array<int8_t, 16> transitions{
      0, -1, 1, 0, 1, 0, 0, -1,
      -1, 0, 0, 1, 0, 1, -1, 0};
  accumulator_ = static_cast<int8_t>(
      accumulator_ +
      transitions[static_cast<std::size_t>(previous_state_ * 4 + current)]);
  previous_state_ = current;
  if (accumulator_ >= 4) {
    accumulator_ = 0;
    return 1;
  }
  if (accumulator_ <= -4) {
    accumulator_ = 0;
    return -1;
  }
  return 0;
}

void RotaryEncoderDecoder::reset()
{
  previous_state_ = 0;
  accumulator_ = 0;
  initialized_ = false;
}

void MixerEngine::evaluate_logical_switches(
    const Model& model, const ControlInputs& controls,
    const std::array<int16_t, kMaxInputs>& virtual_inputs,
    const std::array<int32_t, kChannelCount>& previous_channels,
    const ITelemetrySource& telemetry, uint8_t flight_mode, TimeUs now_us)
{
  const auto count =
      std::min<std::size_t>(model.logical_switch_count, kMaxLogicalSwitches);
  for (std::size_t i = 0; i < count; ++i) {
    const auto& config = model.logical_switches[i];
    auto& runtime = logical_runtime_[i];
    const int16_t lhs = source_value(config.lhs, model, controls, virtual_inputs,
                                     previous_channels, telemetry, flight_mode);
    const int16_t rhs = source_value(config.rhs, model, controls, virtual_inputs,
                                     previous_channels, telemetry, flight_mode);
    bool raw = false;

    switch (config.operation) {
      case LogicalSwitchOp::None:
        raw = false;
        break;
      case LogicalSwitchOp::Greater:
        raw = lhs > (config.rhs.kind == SourceKind::Constant
                         ? config.threshold
                         : rhs);
        break;
      case LogicalSwitchOp::Less:
        raw = lhs < (config.rhs.kind == SourceKind::Constant
                         ? config.threshold
                         : rhs);
        break;
      case LogicalSwitchOp::Equal:
        raw = std::abs(lhs - rhs) <= std::max<int16_t>(1, config.threshold);
        break;
      case LogicalSwitchOp::AbsGreater:
        raw = std::abs(lhs) > std::abs(config.threshold);
        break;
      case LogicalSwitchOp::And:
        raw = switch_value_with_logic(config.first, controls) &&
              switch_value_with_logic(config.second, controls);
        break;
      case LogicalSwitchOp::Or:
        raw = switch_value_with_logic(config.first, controls) ||
              switch_value_with_logic(config.second, controls);
        break;
      case LogicalSwitchOp::Xor:
        raw = switch_value_with_logic(config.first, controls) !=
              switch_value_with_logic(config.second, controls);
        break;
      case LogicalSwitchOp::Edge: {
        const bool current = switch_value_with_logic(config.first, controls);
        if (current && !runtime.previous_input) {
          runtime.active_until_us =
              now_us + static_cast<TimeUs>(
                           std::max<uint16_t>(1, config.duration_ms)) *
                           1000;
        }
        runtime.previous_input = current;
        raw = now_us < runtime.active_until_us;
        break;
      }
      case LogicalSwitchOp::Sticky:
        if (switch_value_with_logic(config.first, controls)) {
          runtime.value = true;
        }
        if (switch_value_with_logic(config.second, controls)) {
          runtime.value = false;
        }
        raw = runtime.value;
        break;
      case LogicalSwitchOp::Timer: {
        const uint32_t period_ms =
            std::max<uint32_t>(1, config.delay_ms + config.duration_ms);
        const uint32_t position_ms =
            static_cast<uint32_t>((now_us / 1000) % period_ms);
        raw = position_ms >= config.delay_ms;
        break;
      }
    }

    if (!switch_value_with_logic(config.and_condition, controls)) {
      raw = false;
    }
    if (raw != runtime.value) {
      if (runtime.changed_at_us == 0) {
        runtime.changed_at_us = now_us;
      }
      const auto required_delay =
          raw && config.operation != LogicalSwitchOp::Timer
              ? static_cast<TimeUs>(config.delay_ms) * 1000
              : 0;
      if (now_us - runtime.changed_at_us >= required_delay) {
        runtime.value = raw;
        runtime.changed_at_us = 0;
      }
    } else {
      runtime.changed_at_us = 0;
    }
    logical_values_[i] = runtime.value;
  }
  for (std::size_t i = count; i < logical_values_.size(); ++i) {
    logical_values_[i] = false;
  }
}

void MixerEngine::update_timers(const Model& model,
                                const ControlInputs& controls, TimeUs now_us)
{
  for (std::size_t i = 0; i < kMaxTimers; ++i) {
    const auto& config = model.timers[i];
    if (!timer_initialized_[i]) {
      timer_start_ms_[i] =
          static_cast<int64_t>(config.start_seconds) * 1000;
      timer_states_[i].elapsed_ms = timer_start_ms_[i];
      timer_initialized_[i] = true;
    }
    timer_persistent_[i] = config.persistent;
  }
  if (previous_timer_us_ == 0) {
    previous_timer_us_ = now_us;
    return;
  }
  const int64_t delta_ms =
      static_cast<int64_t>((now_us - previous_timer_us_) / 1000);
  previous_timer_us_ = now_us;

  for (std::size_t i = 0; i < kMaxTimers; ++i) {
    const auto& config = model.timers[i];
    auto& state = timer_states_[i];
    switch (config.mode) {
      case TimerMode::Off:
        state.running = false;
        break;
      case TimerMode::Absolute:
        state.running = true;
        break;
      case TimerMode::Throttle:
        state.running =
            controls.axes[model.throttle_axis] > -kResolution + 50;
        break;
      case TimerMode::Switch:
        state.running = switch_value_with_logic(config.condition, controls);
        break;
    }
    if (state.running) {
      state.elapsed_ms += config.countdown ? -delta_ms : delta_ms;
    }
  }
}

ChannelFrame MixerEngine::evaluate(const Model& model,
                                   const ControlInputs& controls,
                                   const ITelemetrySource& telemetry,
                                   TimeUs now_us)
{
  std::array<int16_t, kMaxInputs> virtual_inputs{};
  std::array<int32_t, kChannelCount> channels{};

  active_flight_mode_ = 0;
  const auto mode_count =
      std::min<std::size_t>(model.flight_mode_count, kMaxFlightModes);
  for (std::size_t i = 1; i < mode_count; ++i) {
    if (model.flight_modes[i].enabled &&
        switch_value_with_logic(model.flight_modes[i].condition, controls)) {
      active_flight_mode_ = static_cast<uint8_t>(i);
      break;
    }
  }
  if (!flight_mode_initialized_) {
    previous_flight_mode_ = active_flight_mode_;
    flight_mode_initialized_ = true;
  } else if (active_flight_mode_ != previous_flight_mode_) {
    for (std::size_t i = 0; i < fade_from_channels_.size(); ++i) {
      fade_from_channels_[i] = static_cast<int16_t>(
          clamp<int32_t>(-kResolution, previous_channels_[i], kResolution));
    }
    const auto& previous_mode = model.flight_modes[previous_flight_mode_];
    const auto& next_mode = model.flight_modes[active_flight_mode_];
    flight_mode_fade_duration_us_ =
        static_cast<TimeUs>(std::max(previous_mode.fade_out_ms,
                                    next_mode.fade_in_ms)) *
        1000;
    flight_mode_transition_us_ = now_us;
    previous_flight_mode_ = active_flight_mode_;
  }

  const auto input_count =
      std::min<std::size_t>(model.input_count, kMaxInputs);
  for (std::size_t i = 0; i < input_count; ++i) {
    const auto& line = model.inputs[i];
    if (!line.enabled || line.destination >= virtual_inputs.size() ||
        line.source_axis >= controls.axes.size() ||
        !flight_mode_enabled(line.flight_mode_mask, active_flight_mode_) ||
        !switch_value_with_logic(line.condition, controls)) {
      continue;
    }
    int16_t value = apply_expo(controls.axes[line.source_axis],
                               line.expo_percent);
    if (line.curve_index >= 0 &&
        line.curve_index < static_cast<int8_t>(kMaxCurves)) {
      value = apply_curve(
          value, model.curves[static_cast<std::size_t>(line.curve_index)]);
    }
    value = scale_percent(value, line.weight_percent);
    value = static_cast<int16_t>(clamp<int32_t>(
        -kResolution,
        value + model.flight_modes[active_flight_mode_]
                    .trims[line.source_axis],
        kResolution));
    virtual_inputs[line.destination] = value;
  }

  evaluate_logical_switches(model, controls, virtual_inputs,
                            previous_channels_, telemetry, active_flight_mode_,
                            now_us);

  const TimeUs delta_us = previous_evaluation_us_ == 0
                              ? 0
                              : now_us - previous_evaluation_us_;
  previous_evaluation_us_ = now_us;
  const auto mix_count = std::min<std::size_t>(model.mix_count, kMaxMixes);

  for (std::size_t i = 0; i < mix_count; ++i) {
    const auto& mix = model.mixes[i];
    auto& runtime = mix_runtime_[i];
    if (!mix.enabled || mix.destination >= channels.size() ||
        !flight_mode_enabled(mix.flight_mode_mask, active_flight_mode_)) {
      continue;
    }

    const bool condition = switch_value_with_logic(mix.condition, controls);
    if (condition != runtime.previous_condition) {
      runtime.previous_condition = condition;
      runtime.transition_at_us = now_us;
    }
    const uint16_t delay_ms = condition ? mix.delay_up_ms : mix.delay_down_ms;
    if (now_us - runtime.transition_at_us >=
        static_cast<TimeUs>(delay_ms) * 1000) {
      runtime.delayed_condition = condition;
    }
    if (!runtime.delayed_condition) {
      continue;
    }

    int32_t target = source_value(mix.source, model, controls, virtual_inputs,
                                  channels, telemetry, active_flight_mode_);
    if (!mix.carry_trim && mix.source.kind == SourceKind::Input) {
      for (std::size_t input_index = 0; input_index < input_count;
           ++input_index) {
        const auto& input = model.inputs[input_index];
        if (input.enabled && input.destination == mix.source.index &&
            input.source_axis < kMaxAxes) {
          target -= model.flight_modes[active_flight_mode_]
                        .trims[input.source_axis];
          break;
        }
      }
    }
    if (mix.curve_index >= 0 &&
        mix.curve_index < static_cast<int8_t>(kMaxCurves)) {
      target = apply_curve(
          static_cast<int16_t>(target),
          model.curves[static_cast<std::size_t>(mix.curve_index)]);
    }
    target = (target * mix.weight_percent) / 100 + mix.offset;
    target = clamp<int32_t>(-kResolution, target, kResolution);

    const bool rising = target > runtime.value;
    const uint16_t speed = rising ? mix.speed_up_per_second
                                  : mix.speed_down_per_second;
    if (speed == 0 || delta_us == 0) {
      runtime.value = target;
    } else {
      const int32_t maximum_delta = std::max<int32_t>(
          1, static_cast<int32_t>(
                 static_cast<uint64_t>(speed) * delta_us / 1000000ULL));
      const int32_t difference = target - runtime.value;
      runtime.value +=
          clamp<int32_t>(-maximum_delta, difference, maximum_delta);
    }

    auto& destination = channels[mix.destination];
    switch (mix.mode) {
      case MixMode::Add:
        destination += runtime.value;
        break;
      case MixMode::Multiply:
        destination = destination * runtime.value / kResolution;
        break;
      case MixMode::Replace:
        destination = runtime.value;
        break;
    }
    destination =
        clamp<int32_t>(-kResolution * 4, destination, kResolution * 4);
  }

  ChannelFrame frame{};
  for (std::size_t i = 0; i < frame.channels.size(); ++i) {
    const auto& output = model.outputs[i];
    int32_t value = clamp<int32_t>(-kResolution, channels[i], kResolution);
    if (output.reversed) {
      value = -value;
    }
    value += output.subtrim;
    value = clamp<int32_t>(output.minimum, value, output.maximum);
    frame.channels[i] = static_cast<int16_t>(value);
  }
  if (flight_mode_fade_duration_us_ != 0 &&
      now_us >= flight_mode_transition_us_) {
    const TimeUs elapsed = now_us - flight_mode_transition_us_;
    if (elapsed < flight_mode_fade_duration_us_) {
      for (std::size_t i = 0; i < frame.channels.size(); ++i) {
        const int64_t from = fade_from_channels_[i];
        const int64_t difference =
            static_cast<int64_t>(frame.channels[i]) - from;
        frame.channels[i] = static_cast<int16_t>(
            from + difference * static_cast<int64_t>(elapsed) /
                       static_cast<int64_t>(flight_mode_fade_duration_us_));
      }
    } else {
      flight_mode_fade_duration_us_ = 0;
    }
  }
  for (std::size_t i = 0; i < frame.channels.size(); ++i) {
    previous_channels_[i] = frame.channels[i];
  }
  frame.generated_at_us = now_us;
  frame.sequence = ++sequence_;
  frame.safe = false;

  update_timers(model, controls, now_us);
  return frame;
}

const std::array<bool, kMaxLogicalSwitches>&
MixerEngine::logical_switch_values() const
{
  return logical_values_;
}

const std::array<TimerState, kMaxTimers>& MixerEngine::timer_states() const
{
  return timer_states_;
}

uint8_t MixerEngine::active_flight_mode() const
{
  return active_flight_mode_;
}

void MixerEngine::reset_timer(std::size_t index)
{
  if (index < timer_states_.size()) {
    timer_states_[index].elapsed_ms = timer_start_ms_[index];
    timer_states_[index].running = false;
  }
}

SafetyManager::SafetyManager(SafetyConfig config) : config_(config)
{
}

void SafetyManager::boot_complete(bool storage_valid, bool watchdog_recovery,
                                  bool calibration_valid)
{
  const std::lock_guard<std::mutex> lock(mutex_);
  storage_valid_ = storage_valid;
  calibration_valid_ = calibration_valid;
  if (!storage_valid) {
    status_.state = SafetyState::Fault;
    status_.reason = SafetyReason::StorageInvalid;
  } else if (!calibration_valid) {
    status_.state = SafetyState::Fault;
    status_.reason = SafetyReason::CalibrationRequired;
  } else if (watchdog_recovery) {
    status_.state = SafetyState::Locked;
    status_.reason = SafetyReason::WatchdogRecovery;
  } else {
    status_.state = SafetyState::Locked;
    status_.reason = SafetyReason::Startup;
  }
}

void SafetyManager::request_enable()
{
  const std::lock_guard<std::mutex> lock(mutex_);
  if (!maintenance_active_) {
    enable_requested_ = true;
  }
}

void SafetyManager::request_lock()
{
  const std::lock_guard<std::mutex> lock(mutex_);
  enable_requested_ = false;
  healthy_cycles_ = 0;
  status_.state = SafetyState::Locked;
  status_.reason = SafetyReason::ManualLock;
}

void SafetyManager::report_battery(uint16_t millivolts)
{
  const std::lock_guard<std::mutex> lock(mutex_);
  status_.battery_mv = millivolts;
  if (millivolts != 0 && millivolts < config_.minimum_battery_mv) {
    enable_requested_ = false;
    healthy_cycles_ = 0;
    status_.state = SafetyState::Fault;
    status_.reason = SafetyReason::BatteryCritical;
  }
}

void SafetyManager::report_battery_fault()
{
  const std::lock_guard<std::mutex> lock(mutex_);
  enable_requested_ = false;
  healthy_cycles_ = 0;
  status_.state = SafetyState::Fault;
  status_.reason = SafetyReason::BatterySensor;
}

void SafetyManager::report_watchdog_fault()
{
  const std::lock_guard<std::mutex> lock(mutex_);
  watchdog_available_ = false;
  enable_requested_ = false;
  healthy_cycles_ = 0;
  status_.state = SafetyState::Fault;
  status_.reason = SafetyReason::WatchdogUnavailable;
}

void SafetyManager::report_module_ready(bool ready)
{
  const std::lock_guard<std::mutex> lock(mutex_);
  module_ready_ = ready;
  if (!ready) {
    enable_requested_ = false;
    healthy_cycles_ = 0;
    if (status_.state != SafetyState::Booting &&
        status_.state != SafetyState::Fault) {
      status_.state = SafetyState::Locked;
      status_.reason = SafetyReason::ModuleOffline;
    }
  }
}

void SafetyManager::report_mixer_duration(uint32_t duration_us)
{
  const std::lock_guard<std::mutex> lock(mutex_);
  if (duration_us > config_.maximum_mixer_duration_us) {
    ++status_.missed_deadlines;
    healthy_cycles_ = 0;
    mixer_deadline_pending_ = true;
    enable_requested_ = false;
    status_.state = SafetyState::Fault;
    status_.reason = SafetyReason::MixerDeadline;
  }
}

bool SafetyManager::startup_switches_match(
    const Model& model, const ControlInputs& inputs) const
{
  for (std::size_t i = 0; i < 8; ++i) {
    const uint8_t bit = static_cast<uint8_t>(1U << i);
    if ((model.required_switch_mask & bit) == 0) {
      continue;
    }
    const bool expected = (model.required_switch_values & bit) != 0;
    if (inputs.switches[i] != expected) {
      return false;
    }
  }
  return true;
}

ChannelFrame SafetyManager::safe_frame(const Model& model, TimeUs now_us,
                                       uint32_t sequence) const
{
  ChannelFrame result{};
  for (std::size_t i = 0; i < result.channels.size(); ++i) {
    result.channels[i] = model.outputs[i].failsafe;
  }
  // A safety lockout must never preserve throttle or ExpressLRS AUX1/CH5 in
  // an armed state, even if a captured/custom failsafe value says otherwise.
  result.channels[model.throttle_channel] = -kResolution;
  result.channels[4] = -kResolution;
  result.generated_at_us = now_us;
  result.sequence = sequence;
  result.safe = true;
  return result;
}

ChannelFrame SafetyManager::gate(const Model& model,
                                 const ControlInputs& inputs,
                                 const ChannelFrame& proposed, TimeUs now_us)
{
  const std::lock_guard<std::mutex> lock(mutex_);
  if (!storage_valid_) {
    status_.state = SafetyState::Fault;
    status_.reason = SafetyReason::StorageInvalid;
    return safe_frame(model, now_us, proposed.sequence);
  }
  if (!calibration_valid_) {
    status_.state = SafetyState::Fault;
    status_.reason = SafetyReason::CalibrationRequired;
    return safe_frame(model, now_us, proposed.sequence);
  }
  if (!watchdog_available_) {
    enable_requested_ = false;
    healthy_cycles_ = 0;
    status_.state = SafetyState::Fault;
    status_.reason = SafetyReason::WatchdogUnavailable;
    return safe_frame(model, now_us, proposed.sequence);
  }
  if (mixer_deadline_pending_) {
    mixer_deadline_pending_ = false;
    return safe_frame(model, now_us, proposed.sequence);
  }
  if (!inputs.valid) {
    enable_requested_ = false;
    status_.state = SafetyState::Locked;
    status_.reason = SafetyReason::InputsInvalid;
    healthy_cycles_ = 0;
    return safe_frame(model, now_us, proposed.sequence);
  }
  if (now_us < inputs.sampled_at_us ||
      now_us - inputs.sampled_at_us > config_.maximum_input_age_us) {
    enable_requested_ = false;
    ++status_.stale_frames;
    status_.state = SafetyState::Locked;
    status_.reason = SafetyReason::InputsStale;
    healthy_cycles_ = 0;
    return safe_frame(model, now_us, proposed.sequence);
  }
  if (status_.battery_mv != 0 &&
      status_.battery_mv < config_.minimum_battery_mv) {
    enable_requested_ = false;
    healthy_cycles_ = 0;
    status_.state = SafetyState::Fault;
    status_.reason = SafetyReason::BatteryCritical;
    return safe_frame(model, now_us, proposed.sequence);
  }
  if (!module_ready_) {
    enable_requested_ = false;
    healthy_cycles_ = 0;
    if (status_.state != SafetyState::Fault) {
      status_.state = SafetyState::Locked;
      status_.reason = SafetyReason::ModuleOffline;
    }
    return safe_frame(model, now_us, proposed.sequence);
  }
  if (status_.state != SafetyState::Enabled &&
      !startup_switches_match(model, inputs)) {
    status_.state = SafetyState::Locked;
    status_.reason = SafetyReason::SwitchMismatch;
    healthy_cycles_ = 0;
    return safe_frame(model, now_us, proposed.sequence);
  }
  if (inputs.axes[model.throttle_axis] > config_.throttle_safe_value &&
      status_.state != SafetyState::Enabled) {
    status_.state = SafetyState::Locked;
    status_.reason = SafetyReason::ThrottleHigh;
    healthy_cycles_ = 0;
    return safe_frame(model, now_us, proposed.sequence);
  }

  if (healthy_cycles_ < config_.healthy_cycles_before_ready) {
    ++healthy_cycles_;
  }
  if (healthy_cycles_ >= config_.healthy_cycles_before_ready) {
    status_.state = enable_requested_ ? SafetyState::Enabled
                                      : SafetyState::Ready;
    status_.reason = SafetyReason::None;
  }
  return status_.state == SafetyState::Enabled
             ? proposed
             : safe_frame(model, now_us, proposed.sequence);
}

SafetyStatus SafetyManager::status() const
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

bool SafetyManager::maintenance_allowed() const
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return status_.state != SafetyState::Enabled;
}

bool SafetyManager::begin_maintenance()
{
  const std::lock_guard<std::mutex> lock(mutex_);
  if (maintenance_active_ || status_.state == SafetyState::Enabled) {
    return false;
  }
  maintenance_active_ = true;
  enable_requested_ = false;
  return true;
}

void SafetyManager::end_maintenance()
{
  const std::lock_guard<std::mutex> lock(mutex_);
  maintenance_active_ = false;
}

ControlLoop::ControlLoop(InputProcessor& inputs, MixerEngine& mixer,
                         SafetyManager& safety, ITelemetrySource& telemetry,
                         IWatchdog& watchdog)
    : inputs_(inputs),
      mixer_(mixer),
      safety_(safety),
      telemetry_(telemetry),
      watchdog_(watchdog)
{
}

ControlCycleResult ControlLoop::run(const Model& model, const RawInputs& raw,
                                    uint16_t battery_mv,
                                    TimeUs cycle_started_us,
                                    TimeUs cycle_finished_us)
{
  const auto controls = inputs_.process(raw);
  auto proposed =
      mixer_.evaluate(model, controls, telemetry_, cycle_started_us);
  const uint32_t duration =
      cycle_finished_us >= cycle_started_us
          ? static_cast<uint32_t>(cycle_finished_us - cycle_started_us)
          : 0;
  safety_.report_battery(battery_mv);
  safety_.report_mixer_duration(duration);
  auto gated = safety_.gate(model, controls, proposed, cycle_finished_us);
  watchdog_.kick();
  return {gated, duration, safety_.status()};
}

}  // namespace rivettx
