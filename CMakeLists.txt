cmake_minimum_required(VERSION 3.25 FATAL_ERROR)
project(layout)

if(NOT COMMAND add_mim_plugin)
    find_package(mim REQUIRED)
endif()

add_mim_plugin(layout
    SOURCES
        src/layout.cpp
        src/normalizers.cpp
    INSTALL
)
