#pragma once

#include "rivettx/crsf.hpp"
#include "rivettx/services.hpp"
#include "rivettx/ui.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

struct lua_State;
struct lua_Debug;

namespace rivettx {

class LuaVm final : public IScriptVm {
 public:
  LuaVm(TelemetryRegistry& telemetry, CrsfParser& parser,
        ICrsfTransport& transport, Canvas& canvas,
        uint32_t memory_limit_bytes = 96 * 1024);
  ~LuaVm() override;

  bool initialize();
  bool load_file(const std::string& path, std::string& error);
  void set_event(int32_t event);
  ScriptSliceResult run_slice(uint32_t instruction_budget) override;
  void terminate() override;
  bool loaded() const;

 private:
  struct AllocatorState {
    std::size_t current = 0;
    std::size_t maximum = 0;
    bool denied = false;
  };

  static LuaVm* self(lua_State* state);
  static void* allocator(void* user, void* pointer, std::size_t old_size,
                         std::size_t new_size);
  static void instruction_hook(lua_State* state, lua_Debug* debug);
  static int api_get_time(lua_State* state);
  static int api_get_value(lua_State* state);
  static int api_get_field_info(lua_State* state);
  static int api_get_version(lua_State* state);
  static int api_load_script(lua_State* state);
  static int api_kill_events(lua_State* state);
  static int api_crossfire_push(lua_State* state);
  static int api_crossfire_pop(lua_State* state);
  static int api_lcd_clear(lua_State* state);
  static int api_lcd_draw_text(lua_State* state);
  static int api_lcd_draw_number(lua_State* state);
  static int api_lcd_draw_line(lua_State* state);
  static int api_lcd_draw_rectangle(lua_State* state);
  static int api_lcd_draw_filled_rectangle(lua_State* state);
  static int api_lcd_refresh(lua_State* state);
  static int api_model_get_info(lua_State* state);

  void register_api();
  void open_safe_libraries();
  bool protected_call(int arguments, int results, std::string& error);

  TelemetryRegistry& telemetry_;
  CrsfParser& parser_;
  ICrsfTransport& transport_;
  Canvas& canvas_;
  AllocatorState allocator_state_{};
  lua_State* state_ = nullptr;
  int run_reference_ = -2;
  int init_reference_ = -2;
  int32_t event_ = 0;
  uint32_t instruction_budget_ = 0;
  uint32_t instructions_ = 0;
  bool loaded_ = false;
};

}  // namespace rivettx
