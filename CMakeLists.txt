cmake_minimum_required(VERSION 3.5)

project(testapp LANGUAGES C)

add_executable(testapp
    sculpt.h
    sculpt_mgr.c
    sculpt_util.c
    sculpt_header.c
    sculpt_conn.c
    app.c
)

install(TARGETS testapp RUNTIME DESTINATION bin)
