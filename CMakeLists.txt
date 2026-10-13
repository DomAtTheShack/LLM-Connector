cmake_minimum_required(VERSION 4.1)
project(HamProof)

set(CMAKE_CXX_STANDARD 20)

add_executable(HamProof main.cpp ansi.hpp
        options.h
        backend.h
        backend.cpp
        cat.cpp
        cat.h
        duplex.cpp
        duplex.h)

# Duplex mode reads the keyboard and minimodem's output on their own threads
find_package(Threads REQUIRED)
target_link_libraries(HamProof PRIVATE Threads::Threads)

# Small CAT check against a radio, the X6100 simulator or the dummy rig
add_executable(cat_test cat_test.cpp cat.cpp cat.h)

# backend.cpp picks the OS code; each OS needs its own audio library
if(WIN32)
    target_link_libraries(HamProof PRIVATE winmm)
else()
    find_package(ALSA REQUIRED)
    target_link_libraries(HamProof PRIVATE ALSA::ALSA)
endif()

# Hamlib: the bundled Windows build, or the one installed with `make install` on Linux
if(WIN32)
    set(HAMLIB_DIR ${CMAKE_SOURCE_DIR}/hamlib-w64-4.7.2)
    add_library(hamlib SHARED IMPORTED)
    set_target_properties(hamlib PROPERTIES
            IMPORTED_IMPLIB ${HAMLIB_DIR}/lib/gcc/libhamlib.dll.a
            IMPORTED_LOCATION ${HAMLIB_DIR}/bin/libhamlib-4.dll
            INTERFACE_INCLUDE_DIRECTORIES ${HAMLIB_DIR}/include)
    # The exe needs Hamlib's DLLs next to it to start
    file(GLOB HAMLIB_DLLS ${HAMLIB_DIR}/bin/*.dll)
    foreach(target HamProof cat_test)
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${HAMLIB_DLLS} $<TARGET_FILE_DIR:${target}>)
    endforeach()
else()
    find_library(HAMLIB_LIBRARY hamlib REQUIRED)
    find_path(HAMLIB_INCLUDE_DIR hamlib/rig.h REQUIRED)
    add_library(hamlib UNKNOWN IMPORTED)
    set_target_properties(hamlib PROPERTIES
            IMPORTED_LOCATION ${HAMLIB_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${HAMLIB_INCLUDE_DIR})
endif()

target_link_libraries(HamProof PRIVATE hamlib)
target_link_libraries(cat_test PRIVATE hamlib)
