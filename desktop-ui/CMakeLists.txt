add_executable(
  desktop-ui
  desktop-ui.cpp
  resource/resource.cpp
  input/input.cpp
  emulator/emulator.cpp
  game-browser/game-browser.cpp
  program/program.cpp
  presentation/presentation.cpp
  settings/settings.cpp
  tools/tools.cpp
)

target_sources(desktop-ui PRIVATE cmake/sources.cmake cmake/os-macos.cmake cmake/os-linux.cmake cmake/os-windows.cmake)

include(cmake/sources.cmake)

get_target_property(desktop-ui_SOURCES desktop-ui SOURCES)

set_source_files_properties(desktop-ui ${desktop-ui_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)

set_source_files_properties(
  desktop-ui
  desktop-ui.cpp
  resource/resource.cpp
  input/input.cpp
  emulator/emulator.cpp
  game-browser/game-browser.cpp
  program/program.cpp
  presentation/presentation.cpp
  settings/settings.cpp
  tools/tools.cpp
  PROPERTIES HEADER_FILE_ONLY FALSE
)

if(ARES_ENABLE_LIBRASHADER)
  find_package(slang_shaders)
endif()

if(OS_WINDOWS)
  include(cmake/os-windows.cmake)
elseif(OS_MACOS)
  include(cmake/os-macos.cmake)
elseif(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
  include(cmake/os-linux.cmake)
endif()

target_include_directories(desktop-ui PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(
  desktop-ui
  PRIVATE ares::ruby ares::hiro ares::ares ares::mia ares::nall
)

if(ARES_ENABLE_CHD)
  target_link_libraries(desktop-ui PRIVATE chdr-static)
endif()

set_target_properties(desktop-ui PROPERTIES OUTPUT_NAME ares)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${desktop-ui_SOURCES})

add_sourcery_command(desktop-ui resource)

option(ARES_ENABLE_FFV1 "Record native Super Famicom video and audio with FFmpeg" OFF)
if(ARES_ENABLE_FFV1)
  if(NOT "sfc" IN_LIST ARES_CORES)
    message(FATAL_ERROR "ARES_ENABLE_FFV1 requires the sfc core")
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFV1 REQUIRED IMPORTED_TARGET libavformat>=60 libavcodec>=60 libavutil>=58)
  target_sources(desktop-ui PRIVATE recording/movie.cpp)
  target_link_libraries(desktop-ui PRIVATE PkgConfig::FFV1)
  target_compile_definitions(desktop-ui PRIVATE ARES_ENABLE_FFV1)

  if(OS_MACOS)
    target_add_resource(desktop-ui "${CMAKE_CURRENT_SOURCE_DIR}/recording/FFmpeg-LGPL-2.1.txt" "Licenses")
    # Distributable FFmpeg dylibs use @rpath install names. Copy the ABI names used by dyld.
    foreach(component avformat avcodec avutil)
      string(REGEX MATCH "^[0-9]+" major "${FFV1_lib${component}_VERSION}")
      set(dylib "lib${component}.${major}.dylib")
      add_custom_command(TARGET desktop-ui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:desktop-ui>/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FFV1_lib${component}_LIBDIR}/${dylib}"
                "$<TARGET_BUNDLE_CONTENT_DIR:desktop-ui>/Frameworks/${dylib}" VERBATIM)
    endforeach()
  endif()
endif()

ares_configure_executable(desktop-ui)
