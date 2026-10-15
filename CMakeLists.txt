cmake_minimum_required(VERSION 3.16)
project(kakoune-lsp-cpp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(Threads REQUIRED)

add_executable(kakoune-lsp-cpp
  src/main.cc
  src/lsp.cc)
target_include_directories(kakoune-lsp-cpp PRIVATE third_party)
target_compile_definitions(kakoune-lsp-cpp PRIVATE KLSP_VERSION="0.1.0")
target_link_libraries(kakoune-lsp-cpp PRIVATE Threads::Threads)

install(TARGETS kakoune-lsp-cpp RUNTIME DESTINATION bin)
