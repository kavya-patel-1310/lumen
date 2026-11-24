cmake_minimum_required(VERSION 3.16)
project(lumen VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LUMEN_SANITIZE "Build with AddressSanitizer + UBSan" OFF)

find_package(Threads REQUIRED)

add_library(lumen_core
  src/stemmer.cpp
  src/tokenizer.cpp
  src/document_loader.cpp
  src/index.cpp
  src/query.cpp
  src/searcher.cpp
  src/server.cpp
)
target_include_directories(lumen_core PUBLIC include)
target_link_libraries(lumen_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(lumen_core PUBLIC ws2_32)
endif()

if(MSVC)
  target_compile_options(lumen_core PUBLIC /utf-8)
  target_compile_options(lumen_core PRIVATE /W4 /permissive-)
  target_compile_definitions(lumen_core PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(lumen_core PRIVATE -Wall -Wextra -Wpedantic)
  if(LUMEN_SANITIZE)
    target_compile_options(lumen_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(lumen_core PUBLIC -fsanitize=address,undefined)
  endif()
endif()

add_executable(lumen src/main.cpp)
target_link_libraries(lumen PRIVATE lumen_core)

enable_testing()
add_executable(lumen_tests tests/tests.cpp)
target_link_libraries(lumen_tests PRIVATE lumen_core)
add_test(NAME lumen_tests COMMAND lumen_tests)
