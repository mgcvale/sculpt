cmake_minimum_required(VERSION 3.5)

project(testapp LANGUAGES C)

add_executable(testapp
    cast.h
    cst_str.c
    cst_header.c
    cst_conn.c
    app.c
)

install(TARGETS testapp RUNTIME DESTINATION bin)
