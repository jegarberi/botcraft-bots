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
add_executable(bot_mcp mcp.cpp)
target_link_libraries(bot_mcp PRIVATE botcraft PkgConfig::LUA PkgConfig::SQLITE)
# The terminal panel: its own program, talks to bot_mcp through unix sockets (run/*.sock)
add_executable(bot_tui tui.cpp)
target_link_libraries(bot_tui PRIVATE protocolCraft PkgConfig::CURSES)
