cmake_minimum_required(VERSION 3.5)

project(testapp LANGUAGES C)
project(prodapp LANGUAGES C)

add_executable(testapp
    src/sculpt.h
    src/sculpt_mgr.c
    src/sculpt_util.c
    src/sculpt_header.c
    src/sculpt_conn.c
    app.c
)

add_executable(prodapp
    prod/sculpt.h
    prod/sculpt.c
    app.c
)

install(TARGETS testapp prodapp RUNTIME DESTINATION bin)
