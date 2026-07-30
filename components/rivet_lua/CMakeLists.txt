include(FetchContent)

FetchContent_Declare(
  rivettx_lua_source
  URL https://www.lua.org/ftp/lua-5.4.8.tar.gz
  URL_HASH SHA256=4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(rivettx_lua_source)
if(NOT rivettx_lua_source_POPULATED)
  FetchContent_Populate(rivettx_lua_source)
endif()

set(LUA_DIR "${rivettx_lua_source_SOURCE_DIR}/src")
set(LUA_SOURCES
  ${LUA_DIR}/lapi.c
  ${LUA_DIR}/lauxlib.c
  ${LUA_DIR}/lbaselib.c
  ${LUA_DIR}/lcode.c
  ${LUA_DIR}/lcorolib.c
  ${LUA_DIR}/lctype.c
  ${LUA_DIR}/ldebug.c
  ${LUA_DIR}/ldo.c
  ${LUA_DIR}/ldump.c
  ${LUA_DIR}/lfunc.c
  ${LUA_DIR}/lgc.c
  ${LUA_DIR}/llex.c
  ${LUA_DIR}/lmathlib.c
  ${LUA_DIR}/lmem.c
  ${LUA_DIR}/lobject.c
  ${LUA_DIR}/lopcodes.c
  ${LUA_DIR}/lparser.c
  ${LUA_DIR}/lstate.c
  ${LUA_DIR}/lstring.c
  ${LUA_DIR}/lstrlib.c
  ${LUA_DIR}/ltable.c
  ${LUA_DIR}/ltablib.c
  ${LUA_DIR}/ltm.c
  ${LUA_DIR}/lundump.c
  ${LUA_DIR}/lutf8lib.c
  ${LUA_DIR}/lvm.c
  ${LUA_DIR}/lzio.c
)

idf_component_register(
  SRCS
    "lua_vm.cpp"
    ${LUA_SOURCES}
  INCLUDE_DIRS
    "include"
    ${LUA_DIR}
  REQUIRES
    rivet_core
    esp_timer
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LUA_USE_C89)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
