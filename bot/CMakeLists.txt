cmake_minimum_required(VERSION 3.20)
project(bot CXX)

set(BOTCRAFT_GAME_VERSION "26.3" CACHE STRING "" FORCE) # must match the server
set(BOTCRAFT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BOTCRAFT_BUILD_DOC OFF CACHE BOOL "" FORCE)
# MC assets (blocks, items, textures) are needed by bot_mcp for world, physics, inventory and rendering
set(BOTCRAFT_INSTALL_MC_ASSETS ON CACHE BOOL "" FORCE)
# Renderer (bot's view, snapshots) and ImGui (behaviour tree flow window)
set(BOTCRAFT_USE_OPENGL_GUI ON CACHE BOOL "" FORCE)
set(BOTCRAFT_USE_IMGUI ON CACHE BOOL "" FORCE)
add_subdirectory(../Botcraft botcraft)

# Botcraft looks for ./Assets relative to the working directory, and installs them in bin/:
# put the executables there too (they chdir next to themselves at startup)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

add_executable(bot main.cpp)
target_link_libraries(bot PRIVATE botcraft)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua5.4)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(CURSES REQUIRED IMPORTED_TARGET ncursesw)
# The bots' stores (store.cpp): SQLite always; PostgreSQL (libpq) and Valkey (hiredis) when found, picked at run
# time with BOTCRAFT_DB (see store.hpp)
pkg_check_modules(PQ IMPORTED_TARGET libpq)
pkg_check_modules(HIREDIS IMPORTED_TARGET hiredis)
add_library(bot_store STATIC store.cpp)
target_link_libraries(bot_store PUBLIC botcraft PkgConfig::SQLITE)
if(PQ_FOUND)
    target_link_libraries(bot_store PUBLIC PkgConfig::PQ)
    target_compile_definitions(bot_store PRIVATE BOTCRAFT_WITH_POSTGRES)
endif()
if(HIREDIS_FOUND)
    target_link_libraries(bot_store PUBLIC PkgConfig::HIREDIS)
    target_compile_definitions(bot_store PRIVATE BOTCRAFT_WITH_VALKEY)
endif()
# Copies the SQLite stores into the one BOTCRAFT_DB names
add_executable(bot_import bot_import.cpp)
target_link_libraries(bot_import PRIVATE bot_store)
add_executable(bot_mcp mcp.cpp)
target_link_libraries(bot_mcp PRIVATE botcraft bot_store PkgConfig::LUA PkgConfig::SQLITE)
# The terminal panel: its own program, talks to bot_mcp through unix sockets (run/*.sock)
add_executable(bot_tui tui.cpp)
target_link_libraries(bot_tui PRIVATE protocolCraft PkgConfig::CURSES)

add_executable(test_construction_policy EXCLUDE_FROM_ALL test_construction_policy.cpp)
target_compile_features(test_construction_policy PRIVATE cxx_std_17)

add_executable(test_spawn_names EXCLUDE_FROM_ALL test_spawn_names.cpp)
target_compile_features(test_spawn_names PRIVATE cxx_std_17)
target_compile_options(test_spawn_names PRIVATE -UNDEBUG)

add_executable(test_site_index EXCLUDE_FROM_ALL test_site_index.cpp)
target_link_libraries(test_site_index PRIVATE botcraft)
target_compile_options(test_site_index PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic -Werror)

add_executable(test_world_lock EXCLUDE_FROM_ALL test_world_lock.cpp)
target_link_libraries(test_world_lock PRIVATE botcraft)
target_compile_options(test_world_lock PRIVATE -UNDEBUG)

add_executable(test_build_quality EXCLUDE_FROM_ALL test_build_quality.cpp)
target_compile_features(test_build_quality PRIVATE cxx_std_17)
target_compile_options(test_build_quality PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic -Werror)

add_executable(test_idle_time EXCLUDE_FROM_ALL test_idle_time.cpp)
target_compile_features(test_idle_time PRIVATE cxx_std_17)
target_compile_options(test_idle_time PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic -Werror)

add_executable(test_weather EXCLUDE_FROM_ALL test_weather.cpp)
target_compile_features(test_weather PRIVATE cxx_std_17)
target_compile_options(test_weather PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic -Werror)

# The stores, same checks on each: lua-free, BOTCRAFT_DB picks the store (see test_store.sh)
add_executable(test_store EXCLUDE_FROM_ALL test_store.cpp)
target_link_libraries(test_store PRIVATE bot_store)
target_compile_options(test_store PRIVATE -UNDEBUG)
