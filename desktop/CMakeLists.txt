add_executable(vacuum src/main.cpp)
target_link_libraries(vacuum PRIVATE vacuum_io)
target_include_directories(vacuum PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(vacuum PROPERTIES OUTPUT_NAME vacuum)
