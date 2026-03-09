cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)

project(NTop C)

option(ENABLE_UNICODE "Compile with Unicode support" OFF)
option(ENABLE_WMI "Compile with WMI support for command line arguments" ON)

if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif ()

if(ENABLE_UNICODE)
	add_definitions(-DUNICODE -D_UNICODE)
endif()

add_executable(NTop ntop.c util.c vi.c)

if(ENABLE_WMI)
	add_definitions(-DENABLE_WMI)
	target_sources(NTop PUBLIC wmi.c)
endif()

if(ENABLE_WMI)
	target_link_libraries(NTop wbemuuid.lib ole32.lib oleaut32.lib)
endif()
