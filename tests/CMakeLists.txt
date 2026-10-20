add_executable(vacuum_tests
  src/main.cpp
  src/test_crc.cpp
  src/test_fft.cpp
  src/test_frame.cpp
  src/test_gf256.cpp
  src/test_masking.cpp
  src/test_modem.cpp
  src/test_physical.cpp
  src/test_rs.cpp
)

target_link_libraries(vacuum_tests PRIVATE vacuum_core)
target_include_directories(vacuum_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_test(NAME vacuum_tests COMMAND vacuum_tests)

# Bit-error-rate benchmark under simulated channels. Not part of `ctest`
# because it takes seconds rather than milliseconds.
add_executable(vacuum_bench src/bench.cpp)
# vacuum_io already carries vacuum_core as a PUBLIC dependency.
target_link_libraries(vacuum_bench PRIVATE vacuum_io)
target_include_directories(vacuum_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
