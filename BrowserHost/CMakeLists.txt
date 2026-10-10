cmake_minimum_required(VERSION 3.21)

project(VeloWebViewBrowserHost LANGUAGES CXX)

# The CEF wrapper in the standard distribution is built with the static MSVC
# runtime.  Match it here so the host does not fail with LNK2038/LNK2005 when
# linking Release builds.  The host is self-contained and does not need the
# Unreal CRT settings.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)

# CEF is intentionally an external, versioned dependency. Do not commit the
# downloaded distribution; configure this project with e.g.
#   -DCEF_ROOT=C:/sdk/cef_binary_131..._windows64
# and stage the resulting BrowserHost next to its CEF resources at packaging
# time.  CEF_ROOT may also point at a CEF checkout that has already built the
# libcef_dll_wrapper target.
set(CEF_ROOT "" CACHE PATH "Path to a Windows x64 CEF standard distribution")
option(VELO_CEF_LEGACY_ACCELERATED_PAINT
       "Use the pre-CEF-131 OnAcceleratedPaint(void*) signature" OFF)

if(NOT WIN32)
  message(FATAL_ERROR "VeloWebView BrowserHost currently supports Windows x64 only")
endif()

if(NOT CEF_ROOT OR NOT EXISTS "${CEF_ROOT}/include/cef_app.h")
  message(FATAL_ERROR
    "CEF_ROOT must point to a CEF Windows x64 standard distribution (include/cef_app.h missing)")
endif()

set(BROWSER_HOST_SOURCES
  src/main.cpp
)

add_executable(VeloWebViewBrowserHost WIN32 ${BROWSER_HOST_SOURCES})
# CEF 154's scoped_refptr constraints use std::convertible_to, which requires
# C++20 on MSVC.  Keeping the host at the CEF language level avoids silently
# compiling against a different ABI than the downloaded distribution.
target_compile_features(VeloWebViewBrowserHost PRIVATE cxx_std_20)
target_compile_definitions(VeloWebViewBrowserHost PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  $<$<BOOL:${VELO_CEF_LEGACY_ACCELERATED_PAINT}>:VELO_CEF_LEGACY_ACCELERATED_PAINT=1>
)
target_include_directories(VeloWebViewBrowserHost PRIVATE
  "${CEF_ROOT}"
  "${CMAKE_CURRENT_LIST_DIR}/../Shared"
)

# Standard CEF distributions provide Release/libcef.lib.  The CEF C++ wrapper
# is usually built separately under Release/ or libcef_dll_wrapper/; callers
# may override CEF_WRAPPER_LIBRARY when using a prebuilt wrapper.
set(CEF_LIBRARY "${CEF_ROOT}/Release/libcef.lib" CACHE FILEPATH "CEF import library")
set(CEF_WRAPPER_LIBRARY "" CACHE FILEPATH "Optional prebuilt libcef_dll_wrapper library")
if(NOT EXISTS "${CEF_LIBRARY}")
  message(FATAL_ERROR "CEF_LIBRARY does not exist: ${CEF_LIBRARY}")
endif()
set(CEF_WRAPPER_TARGET "")
if(NOT CEF_WRAPPER_LIBRARY AND EXISTS "${CEF_ROOT}/libcef_dll/CMakeLists.txt")
  # Official standard distributions ship the wrapper sources.  Building the
  # wrapper here avoids relying on a prebuilt .lib from another toolchain.
  list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
  find_package(CEF REQUIRED)
  add_subdirectory("${CEF_ROOT}/libcef_dll" "${CMAKE_BINARY_DIR}/libcef_dll")
  if(TARGET libcef_dll_wrapper)
    set(CEF_WRAPPER_TARGET libcef_dll_wrapper)
  endif()
endif()
if(CEF_WRAPPER_LIBRARY)
  if(NOT EXISTS "${CEF_WRAPPER_LIBRARY}")
    message(FATAL_ERROR "CEF_WRAPPER_LIBRARY does not exist: ${CEF_WRAPPER_LIBRARY}")
  endif()
  target_link_libraries(VeloWebViewBrowserHost PRIVATE "${CEF_WRAPPER_LIBRARY}")
elseif(CEF_WRAPPER_TARGET)
  target_link_libraries(VeloWebViewBrowserHost PRIVATE ${CEF_WRAPPER_TARGET})
else()
  message(FATAL_ERROR "CEF C++ wrapper is unavailable. Set CEF_WRAPPER_LIBRARY or use a standard CEF distribution with libcef_dll/CMakeLists.txt.")
endif()
if(TARGET libcef_lib)
  target_link_libraries(VeloWebViewBrowserHost PRIVATE libcef_lib)
else()
  target_link_libraries(VeloWebViewBrowserHost PRIVATE "${CEF_LIBRARY}")
endif()
if(CEF_STANDARD_LIBS)
  target_link_libraries(VeloWebViewBrowserHost PRIVATE ${CEF_STANDARD_LIBS})
endif()

target_link_libraries(VeloWebViewBrowserHost PRIVATE
  d3d11 dxgi d3dcompiler
  advapi32 bcrypt comdlg32 gdi32 imm32 ole32 oleaut32 propsys
  shell32 shlwapi user32 version winmm ws2_32
)

# These files are required by CEF's subprocess loader.  CEF distributions add
# GPU/video helper DLLs over time, so stage all runtime artifacts rather than
# silently omitting a newly introduced dependency.
file(GLOB CEF_RUNTIME_FILES
  "${CEF_ROOT}/Release/*.dll"
  "${CEF_ROOT}/Release/*.dat"
  "${CEF_ROOT}/Release/*.bin"
  "${CEF_ROOT}/Release/*.json"
  "${CEF_ROOT}/Release/*.pak")
foreach(CEF_FILE IN LISTS CEF_RUNTIME_FILES)
  if(EXISTS "${CEF_FILE}")
    add_custom_command(TARGET VeloWebViewBrowserHost POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CEF_FILE}" "$<TARGET_FILE_DIR:VeloWebViewBrowserHost>")
  endif()
endforeach()
if(EXISTS "${CEF_ROOT}/Resources")
  add_custom_command(TARGET VeloWebViewBrowserHost POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CEF_ROOT}/Resources"
      "$<TARGET_FILE_DIR:VeloWebViewBrowserHost>")
endif()

install(TARGETS VeloWebViewBrowserHost RUNTIME DESTINATION .)
