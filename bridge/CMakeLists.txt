cmake_minimum_required(VERSION 3.10)
project(ssc_bridge C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

# Path to the upstream open-source encoder (fetched by scripts/fetch-upstream.sh).
set(SSC_ENCODER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/samsung-ssc-encoder" CACHE PATH
    "samsung-ssc-encoder checkout")

set(SSC_ENCODER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SSC_ENCODER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${SSC_ENCODER_DIR} samsung-ssc-encoder)

add_library(ssc_bridge STATIC src/ssc_bridge.c)
target_include_directories(ssc_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

enable_testing()
add_executable(test_bridge tests/test_bridge.c)
target_link_libraries(test_bridge PRIVATE ssc_bridge ssc_encoder)
add_test(NAME bridge COMMAND test_bridge ${SSC_ENCODER_DIR}/tests/vectors)
