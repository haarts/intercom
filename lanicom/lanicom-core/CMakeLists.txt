# lanicom core: portable C99 protocol library.
# - As an ESP-IDF component (ESPHome adds it with add_idf_component): only the library.
# - Standalone (host): the library, unit tests (ctest) and lanicom_tool for interop tests.

set(LANICOM_SRCS src/crypto.c src/packet.c src/replay.c src/control.c src/jitter.c src/engine.c)

if(ESP_PLATFORM)
  idf_component_register(SRCS ${LANICOM_SRCS} INCLUDE_DIRS include REQUIRES mbedtls)
  return()
endif()

cmake_minimum_required(VERSION 3.16)
project(lanicom C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_path(MBEDTLS_INCLUDE_DIR mbedtls/chachapoly.h REQUIRED)
find_library(MBEDCRYPTO_LIBRARY mbedcrypto REQUIRED)

add_library(lanicom STATIC ${LANICOM_SRCS})
target_include_directories(lanicom PUBLIC include ${MBEDTLS_INCLUDE_DIR})
target_link_libraries(lanicom PUBLIC ${MBEDCRYPTO_LIBRARY})
target_compile_options(lanicom PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion)

option(LANICOM_SANITIZE "Build with ASan/UBSan" ON)
if(LANICOM_SANITIZE)
  target_compile_options(lanicom PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lanicom PUBLIC -fsanitize=address,undefined)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(VECTORS ${CMAKE_CURRENT_SOURCE_DIR}/../spec/vectors/lanicom-v1.json)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vectors.h
  COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/test/gen_vectors_h.py ${VECTORS}
          ${CMAKE_CURRENT_BINARY_DIR}/vectors.h
  DEPENDS ${VECTORS} test/gen_vectors_h.py)

add_executable(lanicom_tests test/test_main.c test/test_vectors.c test/test_jitter.c test/test_engine.c
                             ${CMAKE_CURRENT_BINARY_DIR}/vectors.h)
target_include_directories(lanicom_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR} test)
target_link_libraries(lanicom_tests lanicom)
target_compile_options(lanicom_tests PRIVATE -Wall -Wextra -Werror)

add_executable(lanicom_tool tools/lanicom_tool.c)
target_link_libraries(lanicom_tool lanicom)

enable_testing()
add_test(NAME lanicom_tests COMMAND lanicom_tests)
