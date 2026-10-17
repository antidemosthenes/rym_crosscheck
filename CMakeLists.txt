cmake_minimum_required(VERSION 4.0)
project(rym_crosscheck VERSION 1.0.0 LANGUAGES C CXX RC)
if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "This release requires Windows and MSVC.")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()
option(RYM_BUILD_TESTS "Build correctness and lifecycle tests" ON)
include(cmake/Dependencies.cmake)
add_library(rym_core STATIC
  src/domain/runtime.cpp src/matching/normalize.cpp src/matching/index.cpp
  src/storage/files.cpp src/storage/cache.cpp src/storage/whitelist.cpp
  src/library/scanner.cpp src/rym/parser.cpp
  src/rym/browser.cpp src/rym/socket.cpp src/rym/chart.cpp src/report/report.cpp src/report/store.cpp
  src/cli/options.cpp src/cli/launcher.cpp src/cli/application.cpp)
target_include_directories(rym_core PUBLIC include)
target_compile_definitions(rym_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE TAGLIB_STATIC)
target_link_libraries(rym_core PUBLIC Boost::json Boost::unordered Boost::container
  utf8proc lexbor_static CLI11::CLI11 tomlplusplus::tomlplusplus rym_taglib winhttp ole32 shell32)
target_compile_options(rym_core PRIVATE /W4 /permissive- /Zc:__cplusplus /Zc:inline /Zc:lambda /utf-8
  $<$<CONFIG:Release>:/Ox;/Oi;/Gy;/favor:AMD64;/arch:AVX2;/fp:fast>)
target_link_options(rym_core INTERFACE $<$<CONFIG:Release>:/OPT:REF;/OPT:ICF;/MACHINE:X64>)
include(CheckIPOSupported)
check_ipo_supported(RESULT rym_ipo OUTPUT rym_ipo_error)
if(rym_ipo)
  set_property(TARGET rym_core PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  target_link_options(rym_core INTERFACE $<$<CONFIG:Release>:/LTCG>)
endif()
add_executable(rym-crosscheck src/main.cpp)
target_link_libraries(rym-crosscheck PRIVATE rym_core)
target_compile_options(rym-crosscheck PRIVATE /utf-8)
set_target_properties(rym-crosscheck PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/portable/bin"
  INTERPROCEDURAL_OPTIMIZATION_RELEASE "${rym_ipo}")
configure_file(cmake/reviewer.rc.in "${CMAKE_BINARY_DIR}/generated/reviewer.rc" @ONLY)
set_source_files_properties("${CMAKE_BINARY_DIR}/generated/reviewer.rc" PROPERTIES
  OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ui/reviewer.html;${CMAKE_CURRENT_SOURCE_DIR}/ui/reviewer.manifest")
add_executable(missing-report WIN32 src/reviewer/main.cpp src/reviewer/application.cpp
  "${CMAKE_BINARY_DIR}/generated/reviewer.rc")
target_link_libraries(missing-report PRIVATE rym_core rym_webview2 version user32 comctl32)
target_link_options(missing-report PRIVATE /MANIFEST:NO)
target_compile_options(missing-report PRIVATE /W4 /permissive- /utf-8
  $<$<CONFIG:Release>:/Ox;/Oi;/Gy;/favor:AMD64;/arch:AVX2;/fp:fast>)
set_target_properties(missing-report PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/portable"
  INTERPROCEDURAL_OPTIMIZATION_RELEASE "${rym_ipo}")
if(RYM_BUILD_TESTS)
  enable_testing()
  add_executable(rym-tests tests/test_main.cpp tests/review_tests.cpp tests/launcher_tests.cpp
    tests/browser_socket_tests.cpp)
  target_link_libraries(rym-tests PRIVATE rym_core ws2_32 bcrypt crypt32)
  add_dependencies(rym-tests rym-crosscheck)
  target_compile_options(rym-tests PRIVATE /utf-8)
  target_compile_definitions(rym-tests PRIVATE RYM_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures")
  add_test(NAME rym-correctness COMMAND rym-tests)
  add_test(NAME rym-cli-help COMMAND rym-crosscheck --help)
  add_executable(rym-browser-visibility tests/browser_visibility.cpp)
  target_link_libraries(rym-browser-visibility PRIVATE rym_core)
endif()
