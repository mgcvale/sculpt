cmake_minimum_required(VERSION 3.5)

project(sculpt LANGUAGES C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(sculpt STATIC
    src/sculpt_http.c
    src/sculpt_mgr.c
    src/sculpt_util.c
    src/sculpt_header.c
    src/sculpt_conn.c
    src/sculpt_pool.c
)

target_include_directories(sculpt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(testapp app.c)
target_link_libraries(testapp PRIVATE sculpt)

add_executable(custom_protocol_example examples/custom_protocol/app.c)
target_link_libraries(custom_protocol_example PRIVATE sculpt)

install(TARGETS testapp RUNTIME DESTINATION bin)