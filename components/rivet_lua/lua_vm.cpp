#include "rivettx/lua_vm.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>

#include "esp_timer.h"
#include "sdkconfig.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

static_assert(sizeof(lua_Integer) == 4,
              "RivetTX and Lua must use the same 32-bit integer ABI");
static_assert(sizeof(lua_Number) == 4,
              "RivetTX and Lua must use the same 32-bit number ABI");

namespace rivettx {

namespace {

constexpr int kHookInterval = 100;
constexpr int kNoReference = LUA_NOREF;

bool allowed_script_path(const std::string& path)
{
  return path.rfind("/models/scripts/", 0) == 0 &&
         path.find("..") == std::string::npos &&
         path.size() > std::strlen("/models/scripts/");
}

uint16_t sensor_id_for_name(const char* name)
{
  struct SensorName {
    const char* name;
    uint16_t id;
  };
  static constexpr std::array<SensorName, 20> names{{
      {"RSSI", crsf::SensorUplinkRssi},
      {"1RSS", crsf::SensorUplinkRssi1},
      {"2RSS", crsf::SensorUplinkRssi2},
      {"RQly", crsf::SensorUplinkLinkQuality},
      {"RSNR", crsf::SensorUplinkSnr},
      {"RFMD", crsf::SensorRfMode},
      {"TPWR", crsf::SensorTxPower},
      {"TRSS", crsf::SensorDownlinkRssi},
      {"TQly", crsf::SensorDownlinkLinkQuality},
      {"TSNR", crsf::SensorDownlinkSnr},
      {"VFAS", crsf::SensorBatteryVoltage},
      {"Curr", crsf::SensorBatteryCurrent},
      {"Capa", crsf::SensorBatteryCapacity},
      {"Bat%", crsf::SensorBatteryRemaining},
      {"GPS", crsf::SensorGpsLatitude},
      {"GSpd", crsf::SensorGpsSpeed},
      {"Hdg", crsf::SensorGpsHeading},
      {"Alt", crsf::SensorGpsAltitude},
      {"Sats", crsf::SensorGpsSatellites},
      {"TxBat", crsf::SensorBatteryVoltage},
  }};
  for (const auto& entry : names) {
    if (std::strcmp(entry.name, name) == 0) {
      return entry.id;
    }
  }
  return 0;
}

std::string translate_script_path(const char* requested)
{
  std::string path = requested == nullptr ? "" : requested;
  if (path.find("..") != std::string::npos) {
    return {};
  }
  constexpr char tools_prefix[] = "/SCRIPTS/TOOLS/";
  constexpr char scripts_prefix[] = "/SCRIPTS/";
  if (path.rfind(tools_prefix, 0) == 0) {
    return "/models/scripts/" +
           path.substr(std::strlen(tools_prefix));
  }
  if (path.rfind(scripts_prefix, 0) == 0) {
    return "/models/scripts/" +
           path.substr(std::strlen(scripts_prefix));
  }
  return allowed_script_path(path) ? path : std::string{};
}

void set_function(lua_State* state, const char* name,
                  lua_CFunction function)
{
  lua_pushcfunction(state, function);
  lua_setfield(state, -2, name);
}

}  // namespace

LuaVm::LuaVm(TelemetryRegistry& telemetry, CrsfParser& parser,
             ICrsfTransport& transport, Canvas& canvas,
             IToneOutput* tones,
             uint32_t memory_limit_bytes)
    : telemetry_(telemetry),
      parser_(parser),
      transport_(transport),
      canvas_(canvas),
      tones_(tones)
{
  allocator_state_.maximum = memory_limit_bytes;
}

LuaVm::~LuaVm()
{
  terminate();
}

LuaVm* LuaVm::self(lua_State* state)
{
  return *static_cast<LuaVm**>(lua_getextraspace(state));
}

void* LuaVm::allocator(void* user, void* pointer, std::size_t old_size,
                       std::size_t new_size)
{
  auto* allocation = static_cast<AllocatorState*>(user);
  const std::size_t allocated_old_size =
      pointer == nullptr ? 0 : old_size;
  if (new_size == 0) {
    std::free(pointer);
    allocation->current =
        allocated_old_size > allocation->current
            ? 0
            : allocation->current - allocated_old_size;
    return nullptr;
  }
  const std::size_t base =
      allocated_old_size > allocation->current
          ? 0
          : allocation->current - allocated_old_size;
  if (new_size > allocation->maximum ||
      base > allocation->maximum - new_size) {
    allocation->denied = true;
    return nullptr;
  }
  void* result = std::realloc(pointer, new_size);
  if (result != nullptr) {
    allocation->current = base + new_size;
  }
  return result;
}

void LuaVm::instruction_hook(lua_State* state, lua_Debug*)
{
  LuaVm* runtime = self(state);
  runtime->instructions_ += kHookInterval;
  if (runtime->instructions_ >= runtime->instruction_budget_) {
    luaL_error(state, "RivetTX script instruction budget exceeded");
  }
}

void LuaVm::open_safe_libraries()
{
  struct Library {
    const char* name;
    lua_CFunction open;
  };
  constexpr Library libraries[]{
      {LUA_GNAME, luaopen_base},
      {LUA_TABLIBNAME, luaopen_table},
      {LUA_STRLIBNAME, luaopen_string},
      {LUA_MATHLIBNAME, luaopen_math},
      {LUA_UTF8LIBNAME, luaopen_utf8},
      {LUA_COLIBNAME, luaopen_coroutine},
  };
  for (const auto& library : libraries) {
    luaL_requiref(state_, library.name, library.open, 1);
    lua_pop(state_, 1);
  }
}

bool LuaVm::initialize()
{
  if (state_ != nullptr) {
    return true;
  }
  allocator_state_.current = 0;
  allocator_state_.denied = false;
  state_ = lua_newstate(
      allocator, &allocator_state_,
      static_cast<unsigned>(esp_timer_get_time()));
  if (state_ == nullptr) {
    return false;
  }
  *static_cast<LuaVm**>(lua_getextraspace(state_)) = this;
  open_safe_libraries();
  register_api();
  return true;
}

void LuaVm::register_api()
{
  lua_pushcfunction(state_, api_get_time);
  lua_setglobal(state_, "getTime");
  lua_pushcfunction(state_, api_get_value);
  lua_setglobal(state_, "getValue");
  lua_pushcfunction(state_, api_get_value_age);
  lua_setglobal(state_, "getValueAge");
  lua_pushcfunction(state_, api_play_tone);
  lua_setglobal(state_, "playTone");
  lua_pushcfunction(state_, api_get_field_info);
  lua_setglobal(state_, "getFieldInfo");
  lua_pushcfunction(state_, api_get_version);
  lua_setglobal(state_, "getVersion");
  lua_pushcfunction(state_, api_load_script);
  lua_setglobal(state_, "loadScript");
  lua_pushcfunction(state_, api_kill_events);
  lua_setglobal(state_, "killEvents");
  lua_pushcfunction(state_, api_crossfire_push);
  lua_setglobal(state_, "crossfireTelemetryPush");
  lua_pushcfunction(state_, api_crossfire_pop);
  lua_setglobal(state_, "crossfireTelemetryPop");

  lua_newtable(state_);
  set_function(state_, "clear", api_lcd_clear);
  set_function(state_, "drawText", api_lcd_draw_text);
  set_function(state_, "drawNumber", api_lcd_draw_number);
  set_function(state_, "drawLine", api_lcd_draw_line);
  set_function(state_, "drawRectangle", api_lcd_draw_rectangle);
  set_function(state_, "drawFilledRectangle",
               api_lcd_draw_filled_rectangle);
  set_function(state_, "refresh", api_lcd_refresh);
  lua_setglobal(state_, "lcd");

  lua_newtable(state_);
  set_function(state_, "getInfo", api_model_get_info);
  lua_setglobal(state_, "model");

  lua_pushinteger(state_, 1);
  lua_setglobal(state_, "EVT_VIRTUAL_NEXT");
  lua_pushinteger(state_, 2);
  lua_setglobal(state_, "EVT_VIRTUAL_PREV");
  lua_pushinteger(state_, 3);
  lua_setglobal(state_, "EVT_VIRTUAL_ENTER");
  lua_pushinteger(state_, 4);
  lua_setglobal(state_, "EVT_VIRTUAL_EXIT");
  lua_pushinteger(state_, canvas_.width());
  lua_setglobal(state_, "LCD_W");
  lua_pushinteger(state_, canvas_.height());
  lua_setglobal(state_, "LCD_H");

  struct Constant {
    const char* name;
    int value;
  };
  constexpr Constant constants[]{
      {"INVERS", 0x01}, {"BLINK", 0x02},   {"RIGHT", 0x04},
      {"CENTER", 0x08}, {"SMLSIZE", 0x10}, {"MIDSIZE", 0x20},
      {"DBLSIZE", 0x40}, {"PREC1", 0x80},  {"PREC2", 0x100},
  };
  for (const auto& constant : constants) {
    lua_pushinteger(state_, constant.value);
    lua_setglobal(state_, constant.name);
  }
}

bool LuaVm::protected_call(int arguments, int results, std::string& error)
{
  if (lua_pcall(state_, arguments, results, 0) == LUA_OK) {
    return true;
  }
  const char* message = lua_tostring(state_, -1);
  error = message != nullptr ? message : "unknown Lua error";
  lua_pop(state_, 1);
  return false;
}

bool LuaVm::load_file(const std::string& path, std::string& error)
{
  if (!allowed_script_path(path)) {
    error = "script path is outside /models/scripts";
    return false;
  }
  if (!initialize()) {
    error = "cannot allocate Lua state";
    return false;
  }
  if (run_reference_ != kNoReference) {
    luaL_unref(state_, LUA_REGISTRYINDEX, run_reference_);
    run_reference_ = kNoReference;
  }
  if (init_reference_ != kNoReference) {
    luaL_unref(state_, LUA_REGISTRYINDEX, init_reference_);
    init_reference_ = kNoReference;
  }
  loaded_ = false;
  if (luaL_loadfile(state_, path.c_str()) != LUA_OK) {
    error = lua_tostring(state_, -1);
    lua_pop(state_, 1);
    return false;
  }
  if (!protected_call(0, 1, error) || !lua_istable(state_, -1)) {
    if (lua_gettop(state_) > 0) {
      lua_pop(state_, 1);
    }
    error = error.empty() ? "script must return a table" : error;
    return false;
  }

  lua_getfield(state_, -1, "init");
  if (lua_isfunction(state_, -1)) {
    init_reference_ = luaL_ref(state_, LUA_REGISTRYINDEX);
  } else {
    lua_pop(state_, 1);
  }
  lua_getfield(state_, -1, "run");
  if (!lua_isfunction(state_, -1)) {
    lua_pop(state_, 2);
    error = "script table has no run function";
    return false;
  }
  run_reference_ = luaL_ref(state_, LUA_REGISTRYINDEX);
  lua_pop(state_, 1);

  if (init_reference_ != kNoReference) {
    lua_rawgeti(state_, LUA_REGISTRYINDEX, init_reference_);
    if (!protected_call(0, 0, error)) {
      return false;
    }
  }
  loaded_ = true;
  return true;
}

void LuaVm::set_event(int32_t event)
{
  event_ = event;
}

ScriptSliceResult LuaVm::run_slice(uint32_t instruction_budget)
{
  if (!loaded_ || state_ == nullptr) {
    return {ScriptRunStatus::Error, 0, 0,
            static_cast<uint32_t>(allocator_state_.current)};
  }

  allocator_state_.denied = false;
  instruction_budget_ = std::max<uint32_t>(kHookInterval,
                                           instruction_budget);
  instructions_ = 0;
  lua_sethook(state_, instruction_hook, LUA_MASKCOUNT, kHookInterval);
  lua_rawgeti(state_, LUA_REGISTRYINDEX, run_reference_);
  lua_pushinteger(state_, event_);
  const int64_t started = esp_timer_get_time();
  const int status = lua_pcall(state_, 1, 1, 0);
  const uint32_t elapsed =
      static_cast<uint32_t>(esp_timer_get_time() - started);
  lua_sethook(state_, nullptr, 0, 0);

  if (status != LUA_OK) {
    lua_pop(state_, 1);
    return {allocator_state_.denied ? ScriptRunStatus::OutOfMemory
                                    : ScriptRunStatus::Error,
            instructions_, elapsed,
            static_cast<uint32_t>(allocator_state_.current)};
  }
  lua_pop(state_, 1);
  return {ScriptRunStatus::Yielded, instructions_, elapsed,
          static_cast<uint32_t>(allocator_state_.current)};
}

void LuaVm::terminate()
{
  if (state_ != nullptr) {
    lua_close(state_);
    state_ = nullptr;
  }
  run_reference_ = kNoReference;
  init_reference_ = kNoReference;
  loaded_ = false;
}

bool LuaVm::loaded() const
{
  return loaded_;
}

int LuaVm::api_get_time(lua_State* state)
{
  lua_pushinteger(state, esp_timer_get_time() / 10000);
  return 1;
}

int LuaVm::api_get_value(lua_State* state)
{
  LuaVm* runtime = self(state);
  uint16_t sensor = 0;
  if (lua_type(state, 1) == LUA_TSTRING) {
    sensor = sensor_id_for_name(lua_tostring(state, 1));
  } else {
    sensor = static_cast<uint16_t>(luaL_checkinteger(state, 1));
  }
  int32_t value = 0;
  if (sensor == 0 || !runtime->telemetry_.value(sensor, value)) {
    lua_pushnil(state);
  } else {
    lua_pushinteger(state, value);
  }
  return 1;
}

int LuaVm::api_get_value_age(lua_State* state)
{
  LuaVm* runtime = self(state);
  uint16_t sensor = 0;
  if (lua_type(state, 1) == LUA_TSTRING) {
    sensor = sensor_id_for_name(lua_tostring(state, 1));
  } else {
    sensor = static_cast<uint16_t>(luaL_checkinteger(state, 1));
  }
  const TelemetryEntry* entry = runtime->telemetry_.find(sensor);
  if (entry == nullptr) {
    lua_pushnil(state);
  } else {
    const TimeUs current =
        static_cast<TimeUs>(esp_timer_get_time());
    const TimeUs age =
        current >= entry->updated_at_us ? current - entry->updated_at_us : 0;
    lua_pushinteger(state, static_cast<lua_Integer>(age / 1000));
  }
  return 1;
}

int LuaVm::api_play_tone(lua_State* state)
{
  LuaVm* runtime = self(state);
  const uint16_t frequency = static_cast<uint16_t>(
      clamp<lua_Integer>(100, luaL_checkinteger(state, 1), 5000));
  const uint16_t duration = static_cast<uint16_t>(
      clamp<lua_Integer>(1, luaL_checkinteger(state, 2), 5000));
  lua_pushboolean(
      state, runtime->tones_ != nullptr &&
                 runtime->tones_->play_tone(frequency, duration));
  return 1;
}

int LuaVm::api_get_field_info(lua_State* state)
{
  const uint16_t sensor =
      sensor_id_for_name(luaL_checkstring(state, 1));
  if (sensor == 0) {
    lua_pushnil(state);
    return 1;
  }
  lua_newtable(state);
  lua_pushinteger(state, sensor);
  lua_setfield(state, -2, "id");
  lua_pushstring(state, lua_tostring(state, 1));
  lua_setfield(state, -2, "name");
  return 1;
}

int LuaVm::api_get_version(lua_State* state)
{
  lua_pushstring(state, "RivetTX");
  lua_pushinteger(state, 0);
  lua_pushinteger(state, 1);
  lua_pushinteger(state, 0);
  lua_pushstring(state, CONFIG_IDF_TARGET);
  return 5;
}

int LuaVm::api_load_script(lua_State* state)
{
  const std::string path =
      translate_script_path(luaL_checkstring(state, 1));
  if (path.empty()) {
    lua_pushnil(state);
    lua_pushstring(state, "script path is outside /models/scripts");
    return 2;
  }
  if (luaL_loadfile(state, path.c_str()) != LUA_OK) {
    lua_pushnil(state);
    lua_insert(state, -2);
    return 2;
  }
  return 1;
}

int LuaVm::api_kill_events(lua_State*)
{
  return 0;
}

int LuaVm::api_crossfire_push(lua_State* state)
{
  LuaVm* runtime = self(state);
  const uint8_t command =
      static_cast<uint8_t>(luaL_checkinteger(state, 1));
  luaL_checktype(state, 2, LUA_TTABLE);

  crsf::Frame frame{};
  frame.bytes[0] = crsf::kAddressModule;
  frame.bytes[2] = command;
  const std::size_t length = lua_rawlen(state, 2);
  if (length > crsf::kMaximumFrameSize - 4) {
    return luaL_error(state, "CRSF payload too large");
  }
  for (std::size_t i = 0; i < length; ++i) {
    lua_rawgeti(state, 2, static_cast<lua_Integer>(i + 1));
    frame.bytes[3 + i] =
        static_cast<uint8_t>(luaL_checkinteger(state, -1));
    lua_pop(state, 1);
  }
  frame.bytes[1] = static_cast<uint8_t>(length + 2);
  frame.bytes[3 + length] =
      crsf::crc8_dvb_s2(frame.bytes.data() + 2, length + 1);
  frame.size = static_cast<uint8_t>(length + 4);
  lua_pushboolean(
      state,
      runtime->transport_.write(frame.bytes.data(), frame.size));
  return 1;
}

int LuaVm::api_crossfire_pop(lua_State* state)
{
  LuaVm* runtime = self(state);
  crsf::Frame frame{};
  if (!runtime->parser_.pop_frame(frame) || frame.size < 4) {
    return 0;
  }
  lua_pushinteger(state, frame.bytes[2]);
  lua_newtable(state);
  const std::size_t payload_size = frame.bytes[1] - 2;
  for (std::size_t i = 0; i < payload_size; ++i) {
    lua_pushinteger(state, frame.bytes[3 + i]);
    lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
  }
  return 2;
}

int LuaVm::api_lcd_clear(lua_State* state)
{
  self(state)->canvas_.clear(false);
  return 0;
}

int LuaVm::api_lcd_draw_text(lua_State* state)
{
  LuaVm* runtime = self(state);
  const int16_t x = static_cast<int16_t>(luaL_checkinteger(state, 1));
  const int16_t y = static_cast<int16_t>(luaL_checkinteger(state, 2));
  const char* text = luaL_checkstring(state, 3);
  runtime->canvas_.text(x, y, text);
  return 0;
}

int LuaVm::api_lcd_draw_number(lua_State* state)
{
  LuaVm* runtime = self(state);
  const int16_t x = static_cast<int16_t>(luaL_checkinteger(state, 1));
  const int16_t y = static_cast<int16_t>(luaL_checkinteger(state, 2));
  const lua_Integer value = luaL_checkinteger(state, 3);
  runtime->canvas_.text(x, y, std::to_string(value));
  return 0;
}

int LuaVm::api_lcd_draw_line(lua_State* state)
{
  LuaVm* runtime = self(state);
  const int16_t x = static_cast<int16_t>(luaL_checkinteger(state, 1));
  const int16_t y = static_cast<int16_t>(luaL_checkinteger(state, 2));
  const int16_t width =
      static_cast<int16_t>(luaL_checkinteger(state, 3));
  const int16_t height =
      static_cast<int16_t>(luaL_checkinteger(state, 4));
  const int16_t steps = std::max<int16_t>(std::abs(width),
                                         std::abs(height));
  for (int16_t i = 0; i <= steps; ++i) {
    runtime->canvas_.pixel(
        static_cast<int16_t>(x + (steps == 0 ? 0 : width * i / steps)),
        static_cast<int16_t>(y + (steps == 0 ? 0 : height * i / steps)));
  }
  return 0;
}

int LuaVm::api_lcd_draw_rectangle(lua_State* state)
{
  LuaVm* runtime = self(state);
  runtime->canvas_.rectangle(
      {static_cast<int16_t>(luaL_checkinteger(state, 1)),
       static_cast<int16_t>(luaL_checkinteger(state, 2)),
       static_cast<int16_t>(luaL_checkinteger(state, 3)),
       static_cast<int16_t>(luaL_checkinteger(state, 4))},
      false);
  return 0;
}

int LuaVm::api_lcd_draw_filled_rectangle(lua_State* state)
{
  LuaVm* runtime = self(state);
  runtime->canvas_.rectangle(
      {static_cast<int16_t>(luaL_checkinteger(state, 1)),
       static_cast<int16_t>(luaL_checkinteger(state, 2)),
       static_cast<int16_t>(luaL_checkinteger(state, 3)),
       static_cast<int16_t>(luaL_checkinteger(state, 4))},
      true);
  return 0;
}

int LuaVm::api_lcd_refresh(lua_State*)
{
  // The low-priority display task owns physical refresh.
  return 0;
}

int LuaVm::api_model_get_info(lua_State* state)
{
  lua_newtable(state);
  lua_pushstring(state, "RivetTX model");
  lua_setfield(state, -2, "name");
  lua_pushinteger(state, Model::kSchemaVersion);
  lua_setfield(state, -2, "schema");
  return 1;
}

}  // namespace rivettx
