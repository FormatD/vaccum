add_library(vacuum_io STATIC
  src/wav.cpp
  src/ffmpeg.cpp
  src/mp3.cpp
)

target_include_directories(vacuum_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vacuum_io PUBLIC vacuum_core)
target_compile_features(vacuum_io PUBLIC cxx_std_20)
set_target_properties(vacuum_io PROPERTIES POSITION_INDEPENDENT_CODE ON)

# libmp3lame is used for MP3 encoding. Optional: without it the CLI can still
# read MP3 (via ffmpeg) and write WAV, and reports a clear error on encode.
find_path(LAME_INCLUDE_DIR NAMES lame/lame.h
  HINTS /opt/homebrew/include /usr/local/include /opt/local/include)
find_library(LAME_LIBRARY NAMES mp3lame
  HINTS /opt/homebrew/lib /usr/local/lib /opt/local/lib)

if(LAME_INCLUDE_DIR AND LAME_LIBRARY)
  target_include_directories(vacuum_io PRIVATE ${LAME_INCLUDE_DIR})
  target_link_libraries(vacuum_io PUBLIC ${LAME_LIBRARY})
  target_compile_definitions(vacuum_io PUBLIC VACUUM_HAVE_LAME=1)
  message(STATUS "vacuum: MP3 encoding enabled via ${LAME_LIBRARY}")
else()
  message(STATUS "vacuum: libmp3lame not found, MP3 encoding disabled")
endif()
