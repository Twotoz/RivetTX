#include "rivettx/storage.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <type_traits>
#include <utility>

#if defined(__unix__) || defined(ESP_PLATFORM)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace rivettx {

namespace {

constexpr uint32_t kModelMagic = 0x4D565252U;  // RRVM little endian
constexpr uint32_t kCalibrationMagic = 0x4C414356U;  // VCAL
constexpr std::size_t kHeaderSize = 20;

class Writer {
 public:
  template <typename T>
  void put(T value)
  {
    static_assert(std::is_integral<T>::value || std::is_enum<T>::value,
                  "integral values only");
    using Raw = typename std::conditional<std::is_enum<T>::value, uint32_t,
                                          T>::type;
    using Unsigned = typename std::make_unsigned<Raw>::type;
    const auto raw = static_cast<Unsigned>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      bytes_.push_back(static_cast<uint8_t>((raw >> (8U * i)) & 0xFFU));
    }
  }

  void put_bool(bool value)
  {
    put<uint8_t>(value ? 1 : 0);
  }

  void put_bytes(const uint8_t* bytes, std::size_t size)
  {
    bytes_.insert(bytes_.end(), bytes, bytes + size);
  }

  const std::vector<uint8_t>& bytes() const
  {
    return bytes_;
  }

  std::vector<uint8_t> take()
  {
    return std::move(bytes_);
  }

 private:
  std::vector<uint8_t> bytes_;
};

class Reader {
 public:
  Reader(const uint8_t* data, std::size_t size) : data_(data), size_(size)
  {
  }

  template <typename T>
  bool get(T& value)
  {
    static_assert(std::is_integral<T>::value || std::is_enum<T>::value,
                  "integral values only");
    if (position_ + sizeof(T) > size_) {
      return false;
    }
    uint64_t raw = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      raw |= static_cast<uint64_t>(data_[position_++]) << (8U * i);
    }
    value = static_cast<T>(raw);
    return true;
  }

  bool get_bool(bool& value)
  {
    uint8_t raw = 0;
    if (!get(raw) || raw > 1) {
      return false;
    }
    value = raw != 0;
    return true;
  }

  bool get_bytes(uint8_t* target, std::size_t size)
  {
    if (position_ + size > size_) {
      return false;
    }
    std::memcpy(target, data_ + position_, size);
    position_ += size;
    return true;
  }

  bool exhausted() const
  {
    return position_ == size_;
  }

 private:
  const uint8_t* data_;
  std::size_t size_;
  std::size_t position_ = 0;
};

void write_source(Writer& writer, const SourceRef& source)
{
  writer.put(source.kind);
  writer.put(source.index);
  writer.put(source.constant);
}

bool read_source(Reader& reader, SourceRef& source)
{
  if (!reader.get(source.kind) || !reader.get(source.index) ||
      !reader.get(source.constant) || source.kind > SourceKind::GVar) {
    return false;
  }
  switch (source.kind) {
    case SourceKind::Axis:
      return source.index < kMaxAxes;
    case SourceKind::Input:
      return source.index < kMaxInputs;
    case SourceKind::Channel:
      return source.index < kChannelCount;
    case SourceKind::GVar:
      return source.index < kMaxGVars;
    case SourceKind::Telemetry:
      return source.index > 0 &&
             source.index <= kMaxTelemetrySensors;
    case SourceKind::Constant:
      return source.constant >= -kResolution &&
             source.constant <= kResolution;
  }
  return false;
}

void write_switch(Writer& writer, const SwitchRef& value)
{
  writer.put(value.index);
  writer.put_bool(value.inverted);
}

bool read_switch(Reader& reader, SwitchRef& value)
{
  return reader.get(value.index) && reader.get_bool(value.inverted) &&
         value.index >= -1 &&
         value.index <
             static_cast<int8_t>(kMaxSwitches + kMaxLogicalSwitches);
}

void write_curve(Writer& writer, const Curve& curve)
{
  writer.put_bool(curve.enabled);
  for (const auto value : curve.points) {
    writer.put(value);
  }
}

bool read_curve(Reader& reader, Curve& curve)
{
  if (!reader.get_bool(curve.enabled)) {
    return false;
  }
  for (auto& value : curve.points) {
    if (!reader.get(value)) {
      return false;
    }
  }
  return true;
}

bool model_shape_valid(const Model& model)
{
  if (std::memchr(model.name.data(), '\0', model.name.size()) == nullptr ||
      model.input_count > kMaxInputs) {
    return false;
  }
  if (model.mix_count > kMaxMixes ||
      model.logical_switch_count > kMaxLogicalSwitches ||
      model.flight_mode_count == 0 ||
      model.flight_mode_count > kMaxFlightModes ||
      model.curve_count > kMaxCurves ||
      model.special_function_count > kMaxSpecialFunctions ||
      model.throttle_axis >= kMaxAxes ||
      model.throttle_channel >= kChannelCount) {
    return false;
  }
  const auto switch_valid = [](const SwitchRef& reference) {
    return reference.index >= -1 &&
           reference.index <
               static_cast<int8_t>(kMaxSwitches + kMaxLogicalSwitches);
  };
  const auto source_valid = [](const SourceRef& source) {
    switch (source.kind) {
      case SourceKind::Axis:
        return source.index < kMaxAxes;
      case SourceKind::Input:
        return source.index < kMaxInputs;
      case SourceKind::Channel:
        return source.index < kChannelCount;
      case SourceKind::Telemetry:
        return source.index > 0 && source.index <= kMaxTelemetrySensors;
      case SourceKind::GVar:
        return source.index < kMaxGVars;
      case SourceKind::Constant:
        return source.constant >= -kResolution &&
               source.constant <= kResolution;
    }
    return false;
  };
  for (std::size_t i = 0; i < model.input_count; ++i) {
    const auto& input = model.inputs[i];
    if (input.source_axis >= kMaxAxes || input.destination >= kMaxInputs ||
        input.weight_percent < -100 || input.weight_percent > 100 ||
        input.expo_percent < -100 || input.expo_percent > 100 ||
        input.curve_index < -1 ||
        input.curve_index >= static_cast<int8_t>(kMaxCurves) ||
        !switch_valid(input.condition)) {
      return false;
    }
  }
  for (std::size_t i = 0; i < model.mix_count; ++i) {
    const auto& mix = model.mixes[i];
    if (mix.destination >= kChannelCount || !source_valid(mix.source) ||
        mix.weight_percent < -100 || mix.weight_percent > 100 ||
        mix.offset < -kResolution || mix.offset > kResolution ||
        mix.curve_index < -1 ||
        mix.curve_index >= static_cast<int8_t>(kMaxCurves) ||
        !switch_valid(mix.condition) || mix.mode > MixMode::Replace) {
      return false;
    }
  }
  for (std::size_t i = 0; i < model.logical_switch_count; ++i) {
    const auto& logical = model.logical_switches[i];
    if (logical.operation > LogicalSwitchOp::Timer ||
        !source_valid(logical.lhs) || !source_valid(logical.rhs) ||
        !switch_valid(logical.first) || !switch_valid(logical.second) ||
        !switch_valid(logical.and_condition)) {
      return false;
    }
  }
  for (std::size_t i = 0; i < model.flight_mode_count; ++i) {
    const auto& mode = model.flight_modes[i];
    if (!switch_valid(mode.condition) ||
        (mode.gvar_override_mask &
         ~static_cast<uint16_t>((1U << kMaxGVars) - 1U)) != 0) {
      return false;
    }
    for (const int16_t trim : mode.trims) {
      if (trim < -kResolution || trim > kResolution) return false;
    }
    for (const int16_t gvar : mode.gvars) {
      if (gvar < -kResolution || gvar > kResolution) return false;
    }
  }
  for (std::size_t i = 0; i < model.curve_count; ++i) {
    for (const int16_t point : model.curves[i].points) {
      if (point < -kResolution || point > kResolution) return false;
    }
  }
  for (const int16_t gvar : model.gvars) {
    if (gvar < -kResolution || gvar > kResolution) return false;
  }
  for (const auto& output : model.outputs) {
    if (output.minimum < -kResolution || output.maximum > kResolution ||
        output.minimum > output.maximum ||
        output.subtrim < -kResolution || output.subtrim > kResolution ||
        output.failsafe < -kResolution || output.failsafe > kResolution) {
      return false;
    }
  }
  for (const auto& timer : model.timers) {
    if (timer.mode > TimerMode::Switch ||
        !switch_valid(timer.condition) ||
        timer.start_seconds < -86400 || timer.start_seconds > 86400) {
      return false;
    }
  }
  for (std::size_t i = 0; i < model.special_function_count; ++i) {
    const auto& special = model.special_functions[i];
    if (!switch_valid(special.condition) ||
        special.action > SpecialAction::EnterModulePassthrough) {
      return false;
    }
  }
  return true;
}

std::vector<uint8_t> encode_payload(const Model& model)
{
  Writer writer;
  writer.put_bytes(reinterpret_cast<const uint8_t*>(model.name.data()),
                   model.name.size());
  writer.put(model.model_id);
  writer.put(model.throttle_axis);
  writer.put(model.throttle_channel);
  writer.put(model.required_switch_mask);
  writer.put(model.required_switch_values);
  writer.put(model.input_count);
  writer.put(model.mix_count);
  writer.put(model.logical_switch_count);
  writer.put(model.flight_mode_count);
  writer.put(model.curve_count);
  writer.put(model.special_function_count);

  for (std::size_t i = 0; i < model.input_count; ++i) {
    const auto& input = model.inputs[i];
    writer.put_bool(input.enabled);
    writer.put(input.source_axis);
    writer.put(input.destination);
    writer.put(input.weight_percent);
    writer.put(input.expo_percent);
    writer.put(input.curve_index);
    write_switch(writer, input.condition);
    writer.put(input.flight_mode_mask);
  }

  for (std::size_t i = 0; i < model.mix_count; ++i) {
    const auto& mix = model.mixes[i];
    writer.put_bool(mix.enabled);
    writer.put(mix.destination);
    write_source(writer, mix.source);
    writer.put(mix.weight_percent);
    writer.put(mix.offset);
    writer.put(mix.curve_index);
    write_switch(writer, mix.condition);
    writer.put(mix.mode);
    writer.put(mix.delay_up_ms);
    writer.put(mix.delay_down_ms);
    writer.put(mix.speed_up_per_second);
    writer.put(mix.speed_down_per_second);
    writer.put(mix.flight_mode_mask);
    writer.put_bool(mix.carry_trim);
  }

  for (std::size_t i = 0; i < model.logical_switch_count; ++i) {
    const auto& logical = model.logical_switches[i];
    writer.put(logical.operation);
    write_source(writer, logical.lhs);
    write_source(writer, logical.rhs);
    write_switch(writer, logical.first);
    write_switch(writer, logical.second);
    write_switch(writer, logical.and_condition);
    writer.put(logical.threshold);
    writer.put(logical.delay_ms);
    writer.put(logical.duration_ms);
  }

  for (std::size_t i = 0; i < model.flight_mode_count; ++i) {
    const auto& mode = model.flight_modes[i];
    writer.put_bool(mode.enabled);
    write_switch(writer, mode.condition);
    for (const auto trim : mode.trims) {
      writer.put(trim);
    }
    for (const auto gvar : mode.gvars) {
      writer.put(gvar);
    }
    writer.put(mode.gvar_override_mask);
    writer.put(mode.fade_in_ms);
    writer.put(mode.fade_out_ms);
  }

  for (std::size_t i = 0; i < model.curve_count; ++i) {
    write_curve(writer, model.curves[i]);
  }
  for (const auto value : model.gvars) {
    writer.put(value);
  }
  for (const auto& output : model.outputs) {
    writer.put(output.minimum);
    writer.put(output.maximum);
    writer.put(output.subtrim);
    writer.put(output.failsafe);
    writer.put_bool(output.reversed);
  }
  for (const auto& timer : model.timers) {
    writer.put(timer.mode);
    write_switch(writer, timer.condition);
    writer.put(timer.start_seconds);
    writer.put_bool(timer.countdown);
    writer.put_bool(timer.persistent);
  }
  for (std::size_t i = 0; i < model.special_function_count; ++i) {
    const auto& special = model.special_functions[i];
    writer.put_bool(special.enabled);
    write_switch(writer, special.condition);
    writer.put(special.action);
    writer.put(special.parameter);
  }
  return writer.take();
}

bool decode_payload(const uint8_t* payload, std::size_t size,
                    uint16_t schema, Model& model)
{
  Reader reader(payload, size);
  model = {};
  if (!reader.get_bytes(reinterpret_cast<uint8_t*>(model.name.data()),
                        model.name.size()) ||
      !reader.get(model.model_id) || !reader.get(model.throttle_axis) ||
      !reader.get(model.throttle_channel)) {
    return false;
  }
  if (std::memchr(model.name.data(), '\0', model.name.size()) == nullptr) {
    return false;
  }
  if (schema >= 2) {
    if (!reader.get(model.required_switch_mask) ||
        !reader.get(model.required_switch_values)) {
      return false;
    }
  }
  if (!reader.get(model.input_count) || !reader.get(model.mix_count) ||
      !reader.get(model.logical_switch_count) ||
      !reader.get(model.flight_mode_count) ||
      !reader.get(model.curve_count) ||
      !reader.get(model.special_function_count) ||
      model.input_count > kMaxInputs || model.mix_count > kMaxMixes ||
      model.logical_switch_count > kMaxLogicalSwitches ||
      model.flight_mode_count == 0 ||
      model.flight_mode_count > kMaxFlightModes ||
      model.curve_count > kMaxCurves ||
      model.special_function_count > kMaxSpecialFunctions ||
      model.throttle_axis >= kMaxAxes ||
      model.throttle_channel >= kChannelCount) {
    return false;
  }

  for (std::size_t i = 0; i < model.input_count; ++i) {
    auto& input = model.inputs[i];
    if (!reader.get_bool(input.enabled) ||
        !reader.get(input.source_axis) ||
        !reader.get(input.destination) ||
        !reader.get(input.weight_percent) ||
        !reader.get(input.expo_percent) ||
        !reader.get(input.curve_index) ||
        !read_switch(reader, input.condition) ||
        !reader.get(input.flight_mode_mask) ||
        input.source_axis >= kMaxAxes || input.destination >= kMaxInputs ||
        input.curve_index < -1 ||
        input.curve_index >= static_cast<int8_t>(kMaxCurves)) {
      return false;
    }
  }

  for (std::size_t i = 0; i < model.mix_count; ++i) {
    auto& mix = model.mixes[i];
    if (!reader.get_bool(mix.enabled) ||
        !reader.get(mix.destination) || !read_source(reader, mix.source) ||
        !reader.get(mix.weight_percent) || !reader.get(mix.offset) ||
        !reader.get(mix.curve_index) ||
        !read_switch(reader, mix.condition) || !reader.get(mix.mode) ||
        !reader.get(mix.delay_up_ms) ||
        !reader.get(mix.delay_down_ms) ||
        !reader.get(mix.speed_up_per_second) ||
        !reader.get(mix.speed_down_per_second) ||
        !reader.get(mix.flight_mode_mask) ||
        !reader.get_bool(mix.carry_trim) ||
        mix.destination >= kChannelCount || mix.mode > MixMode::Replace ||
        mix.curve_index < -1 ||
        mix.curve_index >= static_cast<int8_t>(kMaxCurves)) {
      return false;
    }
  }

  for (std::size_t i = 0; i < model.logical_switch_count; ++i) {
    auto& logical = model.logical_switches[i];
    if (!reader.get(logical.operation) ||
        !read_source(reader, logical.lhs) ||
        !read_source(reader, logical.rhs) ||
        !read_switch(reader, logical.first) ||
        !read_switch(reader, logical.second) ||
        !read_switch(reader, logical.and_condition) ||
        !reader.get(logical.threshold) || !reader.get(logical.delay_ms) ||
        !reader.get(logical.duration_ms) ||
        logical.operation > LogicalSwitchOp::Timer) {
      return false;
    }
  }

  for (std::size_t i = 0; i < model.flight_mode_count; ++i) {
    auto& mode = model.flight_modes[i];
    if (!reader.get_bool(mode.enabled) ||
        !read_switch(reader, mode.condition)) {
      return false;
    }
    for (auto& trim : mode.trims) {
      if (!reader.get(trim)) {
        return false;
      }
    }
    for (auto& gvar : mode.gvars) {
      if (!reader.get(gvar)) {
        return false;
      }
    }
    if (schema >= 3 && !reader.get(mode.gvar_override_mask)) {
      return false;
    }
    if (!reader.get(mode.fade_in_ms) || !reader.get(mode.fade_out_ms)) {
      return false;
    }
  }

  for (std::size_t i = 0; i < model.curve_count; ++i) {
    if (!read_curve(reader, model.curves[i])) {
      return false;
    }
  }
  for (auto& value : model.gvars) {
    if (!reader.get(value)) {
      return false;
    }
  }
  for (auto& output : model.outputs) {
    if (!reader.get(output.minimum) || !reader.get(output.maximum) ||
        !reader.get(output.subtrim) || !reader.get(output.failsafe) ||
        !reader.get_bool(output.reversed) ||
        output.minimum > output.maximum) {
      return false;
    }
  }
  for (auto& timer : model.timers) {
    if (!reader.get(timer.mode) ||
        !read_switch(reader, timer.condition) ||
        !reader.get(timer.start_seconds) ||
        !reader.get_bool(timer.countdown) ||
        !reader.get_bool(timer.persistent) ||
        timer.mode > TimerMode::Switch) {
      return false;
    }
  }
  for (std::size_t i = 0; i < model.special_function_count; ++i) {
    auto& special = model.special_functions[i];
    if (!reader.get_bool(special.enabled) ||
        !read_switch(reader, special.condition) ||
        !reader.get(special.action) || !reader.get(special.parameter) ||
        special.action > SpecialAction::EnterModulePassthrough) {
      return false;
    }
  }
  return reader.exhausted();
}

}  // namespace

uint32_t crc32(const uint8_t* data, std::size_t size)
{
  uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = -(crc & 1U);
      crc = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

std::vector<uint8_t> ModelCodec::encode(const Model& model,
                                        uint32_t generation)
{
  if (!model_shape_valid(model)) {
    return {};
  }
  const auto payload = encode_payload(model);
  Writer writer;
  writer.put(kModelMagic);
  writer.put(Model::kSchemaVersion);
  writer.put<uint16_t>(0);
  writer.put(static_cast<uint32_t>(payload.size()));
  writer.put(generation);
  writer.put(crc32(payload.data(), payload.size()));
  writer.put_bytes(payload.data(), payload.size());
  return writer.take();
}

bool ModelCodec::decode(const std::vector<uint8_t>& encoded, Model& model,
                        uint32_t& generation, std::string& error)
{
  if (encoded.size() < kHeaderSize) {
    error = "model header truncated";
    return false;
  }
  Reader header(encoded.data(), kHeaderSize);
  uint32_t magic = 0;
  uint16_t schema = 0;
  uint16_t reserved = 0;
  uint32_t payload_size = 0;
  uint32_t expected_crc = 0;
  if (!header.get(magic) || !header.get(schema) || !header.get(reserved) ||
      !header.get(payload_size) || !header.get(generation) ||
      !header.get(expected_crc) || magic != kModelMagic ||
      payload_size != encoded.size() - kHeaderSize) {
    error = "invalid model header";
    return false;
  }
  const uint8_t* payload = encoded.data() + kHeaderSize;
  if (crc32(payload, payload_size) != expected_crc) {
    error = "model CRC mismatch";
    return false;
  }
  if (schema == 0 || schema > Model::kSchemaVersion) {
    error = "unsupported model schema";
    return false;
  }
  if (!decode_payload(payload, payload_size, schema, model)) {
    error = "invalid model payload";
    return false;
  }
  if (!migrate(schema, model, error) || !model_shape_valid(model)) {
    if (error.empty()) {
      error = "invalid model values";
    }
    return false;
  }
  return true;
}

bool ModelCodec::migrate(uint16_t source_schema, Model& model,
                         std::string& error)
{
  if (source_schema == 1) {
    model.required_switch_mask = 0;
    model.required_switch_values = 0;
  }
  if (source_schema < 3) {
    for (auto& mode : model.flight_modes) {
      for (std::size_t i = 0; i < mode.gvars.size(); ++i) {
        if (mode.gvars[i] != 0) {
          mode.gvar_override_mask |= static_cast<uint16_t>(1U << i);
        }
      }
    }
  }
  if (source_schema > Model::kSchemaVersion) {
    error = "migration target unavailable";
    return false;
  }
  return true;
}

PosixFileStore::PosixFileStore(std::string root) : root_(std::move(root))
{
  if (!root_.empty() && root_.back() == '/') {
    root_.pop_back();
  }
}

std::string PosixFileStore::full_path(const std::string& path) const
{
  if (root_.empty()) {
    return path;
  }
  return root_ + "/" + path;
}

bool PosixFileStore::read(const std::string& path,
                          std::vector<uint8_t>& data) const
{
  std::ifstream stream(full_path(path), std::ios::binary);
  if (!stream) {
    return false;
  }
  stream.seekg(0, std::ios::end);
  const auto length = stream.tellg();
  if (length < 0) {
    return false;
  }
  stream.seekg(0, std::ios::beg);
  data.resize(static_cast<std::size_t>(length));
  if (!data.empty()) {
    stream.read(reinterpret_cast<char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
  }
  return stream.good() || stream.eof();
}

bool PosixFileStore::write(const std::string& path,
                           const std::vector<uint8_t>& data)
{
  std::ofstream stream(full_path(path),
                       std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  if (!data.empty()) {
    stream.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
  }
  stream.flush();
  return stream.good();
}

bool PosixFileStore::rename(const std::string& from, const std::string& to)
{
  return std::rename(full_path(from).c_str(), full_path(to).c_str()) == 0;
}

bool PosixFileStore::remove(const std::string& path)
{
  return !exists(path) || std::remove(full_path(path).c_str()) == 0;
}

bool PosixFileStore::exists(const std::string& path) const
{
  std::ifstream stream(full_path(path), std::ios::binary);
  return stream.good();
}

bool PosixFileStore::sync(const std::string& path)
{
#if defined(__unix__) || defined(ESP_PLATFORM)
  FILE* file = std::fopen(full_path(path).c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  const bool result = ::fsync(fileno(file)) == 0;
  std::fclose(file);
  return result;
#else
  return exists(path);
#endif
}

bool PosixFileStore::sync_directory()
{
#if defined(__unix__) || defined(ESP_PLATFORM)
  const int descriptor = ::open(root_.empty() ? "." : root_.c_str(),
                                O_RDONLY);
  if (descriptor < 0) {
    return false;
  }
  const bool result = ::fsync(descriptor) == 0;
  (void)::close(descriptor);
  return result;
#else
  return true;
#endif
}

TransactionalModelStore::TransactionalModelStore(IFileStore& files,
                                                 std::string base_path)
    : files_(files),
      active_(std::move(base_path)),
      temporary_(active_ + ".new"),
      backup_(active_ + ".bak")
{
}

bool TransactionalModelStore::decode_file(const std::string& path,
                                          Model& model,
                                          uint32_t& generation,
                                          std::string& error) const
{
  std::vector<uint8_t> data;
  return files_.read(path, data) &&
         ModelCodec::decode(data, model, generation, error);
}

bool TransactionalModelStore::save(const Model& model, uint32_t generation,
                                   std::string& error)
{
  const auto encoded = ModelCodec::encode(model, generation);
  if (encoded.empty()) {
    error = "invalid model shape";
    return false;
  }
  if (!files_.write(temporary_, encoded) || !files_.sync(temporary_)) {
    error = "failed to write temporary model";
    return false;
  }

  Model verified{};
  uint32_t verified_generation = 0;
  if (!decode_file(temporary_, verified, verified_generation, error) ||
      verified_generation != generation) {
    (void)files_.remove(temporary_);
    error = "temporary model verification failed: " + error;
    return false;
  }

  const bool had_active = files_.exists(active_);
  if (had_active) {
    if (!files_.remove(backup_) ||
        !files_.rename(active_, backup_) ||
        !files_.sync_directory()) {
      error = "failed to preserve previous model";
      return false;
    }
  }
  if (!files_.rename(temporary_, active_) || !files_.sync(active_) ||
      !files_.sync_directory()) {
    // A retry must retain the last known-good backup. Best effort restoration
    // also keeps normal boot paths simple after an activation failure.
    if (had_active && !files_.exists(active_) && files_.exists(backup_)) {
      (void)files_.rename(backup_, active_);
      (void)files_.sync_directory();
    }
    error = "failed to activate verified model";
    return false;
  }
  return true;
}

ModelLoadResult TransactionalModelStore::load(Model& model)
{
  ModelLoadResult result{};
  const std::array<std::string, 3> candidates{active_, temporary_, backup_};
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    std::string error;
    uint32_t generation = 0;
    Model candidate{};
    if (!decode_file(candidates[i], candidate, generation, error)) {
      result.error = error;
      continue;
    }
    model = candidate;
    result.success = true;
    result.recovered = i != 0;
    result.generation = generation;
    result.source = candidates[i];
    result.error.clear();
    if (i != 0) {
      const auto encoded = ModelCodec::encode(model, generation);
      (void)files_.write(temporary_, encoded);
      (void)files_.remove(active_);
      (void)files_.rename(temporary_, active_);
      (void)files_.sync(active_);
      (void)files_.sync_directory();
    }
    return result;
  }
  return result;
}

bool TransactionalModelStore::erase()
{
  return files_.remove(temporary_) && files_.remove(backup_) &&
         files_.remove(active_);
}

bool TransactionalModelStore::export_active(std::vector<uint8_t>& data) const
{
  return files_.read(active_, data);
}

bool TransactionalModelStore::import_candidate(
    const std::vector<uint8_t>& data, std::string& error)
{
  Model model{};
  uint32_t generation = 0;
  if (!ModelCodec::decode(data, model, generation, error)) {
    return false;
  }
  return save(model, generation + 1, error);
}

CalibrationStore::CalibrationStore(IFileStore& files, std::string path)
    : files_(files), path_(std::move(path))
{
}

bool CalibrationStore::save(
    const std::array<AxisCalibration, kMaxAxes>& calibration)
{
  Writer payload;
  for (const auto& item : calibration) {
    if (item.minimum >= item.center || item.center >= item.maximum ||
        item.maximum - item.minimum < 100 ||
        item.deadband >=
            static_cast<uint16_t>(item.maximum - item.minimum) ||
        item.filter_percent > 100) {
      return false;
    }
    payload.put(item.minimum);
    payload.put(item.center);
    payload.put(item.maximum);
    payload.put(item.deadband);
    payload.put(item.filter_percent);
    payload.put_bool(item.inverted);
  }
  Writer encoded;
  encoded.put(kCalibrationMagic);
  encoded.put<uint16_t>(1);
  encoded.put<uint16_t>(0);
  encoded.put(crc32(payload.bytes().data(), payload.bytes().size()));
  encoded.put_bytes(payload.bytes().data(), payload.bytes().size());
  const auto data = encoded.take();
  return files_.write(path_, data) && files_.sync(path_);
}

bool CalibrationStore::load(
    std::array<AxisCalibration, kMaxAxes>& calibration) const
{
  std::vector<uint8_t> data;
  if (!files_.read(path_, data) || data.size() < 12) {
    return false;
  }
  Reader header(data.data(), 12);
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t reserved = 0;
  uint32_t expected_crc = 0;
  if (!header.get(magic) || !header.get(version) || !header.get(reserved) ||
      !header.get(expected_crc) || magic != kCalibrationMagic ||
      version != 1 ||
      crc32(data.data() + 12, data.size() - 12) != expected_crc) {
    return false;
  }
  Reader payload(data.data() + 12, data.size() - 12);
  for (auto& item : calibration) {
    if (!payload.get(item.minimum) || !payload.get(item.center) ||
        !payload.get(item.maximum) || !payload.get(item.deadband) ||
        !payload.get(item.filter_percent) ||
        !payload.get_bool(item.inverted) ||
        item.minimum >= item.center || item.center >= item.maximum ||
        item.maximum - item.minimum < 100 ||
        item.deadband >=
            static_cast<uint16_t>(item.maximum - item.minimum) ||
        item.filter_percent > 100) {
      return false;
    }
  }
  return payload.exhausted();
}

}  // namespace rivettx
