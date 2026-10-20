add_library(vacuum_core STATIC
  src/correlate.cpp
  src/crc.cpp
  src/fft.cpp
  src/frame.cpp
  src/gf256.cpp
  src/masking.cpp
  src/physical.cpp
  src/rs.cpp
  src/ss_modem.cpp
  src/transmit.cpp
)

target_include_directories(vacuum_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(vacuum_core PUBLIC cxx_std_20)

# Position-independent so the same object code can be linked into the Android
# JNI shared library.
set_target_properties(vacuum_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  target_compile_options(vacuum_core PRIVATE /W4)
else()
  target_compile_options(vacuum_core PRIVATE -O2)
endif()
