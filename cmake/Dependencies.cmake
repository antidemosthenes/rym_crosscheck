include(FetchContent)
include(ExternalProject)
set(CMAKE_TLS_VERIFY ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_compile_options(/utf-8)
set(RYM_DOWNLOADS "${CMAKE_SOURCE_DIR}/build/downloads")
function(rym_archive name url hash extension)
  set(archive "${RYM_DOWNLOADS}/${name}.${extension}")
  if(EXISTS "${archive}")
    set(url "${archive}")
  endif()
  FetchContent_Declare(${name} URL "${url}" URL_HASH "SHA256=${hash}"
    DOWNLOAD_DIR "${RYM_DOWNLOADS}" DOWNLOAD_NAME "${name}.${extension}"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE EXCLUDE_FROM_ALL SYSTEM)
endfunction()
set(BOOST_INCLUDE_LIBRARIES "unordered;container;json" CACHE STRING "" FORCE)
set(BOOST_ENABLE_CMAKE ON CACHE BOOL "" FORCE)
set(BOOST_SKIP_INSTALL_RULES ON CACHE BOOL "" FORCE)
set(UTF8PROC_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(UTF8PROC_INSTALL OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TOMLPLUSPLUS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
rym_archive(boost "https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.xz" 9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d tar.xz)
rym_archive(utf8proc "https://github.com/JuliaStrings/utf8proc/archive/refs/tags/v2.12.0.tar.gz" f564011d38b2888d583d510b08e69ffa15aa117155db1b9b49ef1dfe1fa25111 tar.gz)
rym_archive(lexbor "https://github.com/lexbor/lexbor/archive/refs/tags/v3.0.0.tar.gz" eafaa79ef9871f0bbb1978eda8677d184f7ecdcaa203d7cd25b3f86e32c014c2 tar.gz)
rym_archive(cli11 "https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.7.2.tar.gz" 46eef3101da70852ec7af026e09d485ccee81813331c8c6052d39344443b83da tar.gz)
rym_archive(tomlplusplus "https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz" 8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155 tar.gz)
FetchContent_MakeAvailable(boost utf8proc lexbor cli11 tomlplusplus)
set(RYM_STAGE "${CMAKE_BINARY_DIR}/stage")
set(RYM_ZLIB_NAME zs.lib)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(RYM_ZLIB_NAME zsd.lib)
endif()
file(MAKE_DIRECTORY "${RYM_STAGE}/include" "${RYM_STAGE}/include/taglib")
set(common_args -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DCMAKE_INSTALL_PREFIX=${RYM_STAGE}
  -DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY} "-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /utf-8" "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc /utf-8" -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF)
function(rym_external name url hash extension)
  set(archive "${RYM_DOWNLOADS}/${name}.${extension}")
  if(EXISTS "${archive}")
    set(url "${archive}")
  endif()
  ExternalProject_Add(${name}_project URL "${url}" URL_HASH SHA256=${hash}
    DOWNLOAD_DIR "${RYM_DOWNLOADS}" DOWNLOAD_NAME "${name}.${extension}"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE ${ARGN})
endfunction()
rym_external(zlib "https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz" b99a0b86c0ba9360ec7e78c4f1e43b1cbdf1e6936c8fa0f6835c0cd694a495a1 tar.gz
  CMAKE_ARGS ${common_args} -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON -DZLIB_BUILD_TESTING=OFF
  BUILD_BYPRODUCTS "${RYM_STAGE}/lib/${RYM_ZLIB_NAME}")
rym_external(utf8cpp "https://github.com/nemtrif/utfcpp/archive/refs/tags/v4.2.1.tar.gz" 6d6a5493a111884cc085ee31babfe6d9960c8fb08fc80a64852eaeea8323dbc1 tar.gz
  CONFIGURE_COMMAND ${CMAKE_COMMAND} -E true BUILD_COMMAND ${CMAKE_COMMAND} -E true INSTALL_COMMAND ${CMAKE_COMMAND} -E true)
ExternalProject_Get_Property(utf8cpp_project SOURCE_DIR)
set(utf8cpp_headers "${SOURCE_DIR}/source")
rym_external(taglib "https://github.com/taglib/taglib/archive/refs/tags/v2.3.2.tar.gz" cda2d6dac7f12a84e46fbb0907bb1a1ec0679e4722b5237576be5563e295244e tar.gz
  DEPENDS zlib_project utf8cpp_project
  CMAKE_ARGS ${common_args} -DBUILD_BINDINGS=OFF -DBUILD_EXAMPLES=OFF -DENABLE_STATIC_RUNTIME=OFF
    -Dutf8cpp_INCLUDE_DIR=${utf8cpp_headers} -DZLIB_INCLUDE_DIR=${RYM_STAGE}/include -DZLIB_LIBRARY=${RYM_STAGE}/lib/${RYM_ZLIB_NAME}
  BUILD_BYPRODUCTS "${RYM_STAGE}/lib/tag.lib")
add_library(rym_taglib STATIC IMPORTED GLOBAL)
set_target_properties(rym_taglib PROPERTIES IMPORTED_LOCATION "${RYM_STAGE}/lib/tag.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${RYM_STAGE}/include;${RYM_STAGE}/include/taglib")
add_dependencies(rym_taglib taglib_project)
add_library(rym_zlib STATIC IMPORTED GLOBAL)
set_target_properties(rym_zlib PROPERTIES IMPORTED_LOCATION "${RYM_STAGE}/lib/${RYM_ZLIB_NAME}")
add_dependencies(rym_zlib zlib_project)
set_property(TARGET rym_taglib APPEND PROPERTY INTERFACE_LINK_LIBRARIES rym_zlib)
