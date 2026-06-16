cmake_minimum_required(VERSION 3.15)
project(fastchm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(fastchm
  src/lzx.cpp
  src/lzxdecode.cpp
  src/chmwriter.cpp
  src/chmreader.cpp
  src/sitemap.cpp
  src/fifti.cpp
  src/textenc.cpp
  src/builder.cpp
  src/main.cpp
  $<$<BOOL:${WIN32}>:src/fastchm.rc>)

if(MSVC)
  target_compile_options(fastchm PRIVATE /W4 /EHsc /permissive-)
else()
  target_compile_options(fastchm PRIVATE -Wall -Wextra)
endif()

find_package(Threads REQUIRED)
target_link_libraries(fastchm PRIVATE Threads::Threads)

enable_testing()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME roundtrip
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_SOURCE_DIR}/test/run_tests.py $<TARGET_FILE:fastchm>)
endif()
