cmake_minimum_required(VERSION 4.1)
project(HamProof)

set(CMAKE_CXX_STANDARD 20)

# -DHAMPROOF_STATIC=ON links the C++ runtime (and Hamlib on Linux) into the exe,
# so it runs on machines without MinGW or a Hamlib install
option(HAMPROOF_STATIC "Link libraries statically where possible" OFF)

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
elseif(APPLE)
    # Homebrew's minimodem uses PulseAudio, so the device list comes from there too
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBPULSE REQUIRED IMPORTED_TARGET libpulse)
    target_link_libraries(HamProof PRIVATE PkgConfig::LIBPULSE)
else()
    find_package(ALSA REQUIRED)
    target_link_libraries(HamProof PRIVATE ALSA::ALSA)
endif()

# Hamlib: the bundled Windows build, the one installed with `make install` on Linux,
# or Homebrew's on macOS
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
elseif(HAMPROOF_STATIC)
    # `make install` of Hamlib (and Homebrew's) puts libhamlib.a next to the shared library
    find_library(HAMLIB_STATIC_LIBRARY libhamlib.a REQUIRED)
    find_path(HAMLIB_INCLUDE_DIR hamlib/rig.h REQUIRED)
    add_library(hamlib STATIC IMPORTED)
    set_target_properties(hamlib PROPERTIES
            IMPORTED_LOCATION ${HAMLIB_STATIC_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${HAMLIB_INCLUDE_DIR})
    # A static Hamlib also needs the libraries it uses (libusb, libudev, ...)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HAMLIB_PC REQUIRED hamlib)
    list(REMOVE_ITEM HAMLIB_PC_STATIC_LIBRARIES hamlib)
    set_target_properties(hamlib PROPERTIES
            INTERFACE_LINK_DIRECTORIES "${HAMLIB_PC_STATIC_LIBRARY_DIRS}"
            INTERFACE_LINK_LIBRARIES "${HAMLIB_PC_STATIC_LIBRARIES}")
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

if(HAMPROOF_STATIC)
    foreach(target HamProof cat_test)
        if(APPLE)
            # libc++ is part of macOS and can't be linked statically; only Hamlib is
        elseif(MINGW)
            # libstdc++, libgcc and winpthread go into the exe. The bundled Hamlib
            # only comes as a DLL, so its DLLs are still copied next to the exe.
            target_link_options(${target} PRIVATE -static)
        else()
            # glibc and ALSA stay shared: a fully static glibc binary breaks
            # ALSA's plugin loading and name lookups
            target_link_options(${target} PRIVATE -static-libgcc -static-libstdc++)
        endif()
    endforeach()
endif()
