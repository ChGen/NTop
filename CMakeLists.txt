cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)

project(NTop C)

option(ENABLE_UNICODE "Compile with Unicode support" OFF)

if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif ()

if(ENABLE_UNICODE)
	add_definitions(-DUNICODE -D_UNICODE)
endif()

add_executable(NTop ntop.c util.c vi.c)
