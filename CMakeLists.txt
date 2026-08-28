cmake_minimum_required(VERSION 3.10)
project(JanusProject C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MINGW)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -DNDEBUG")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -DNDEBUG")

set(SEVENZIP_C_SOURCES
    C/7zCrc.c
    C/7zCrcOpt.c
    C/7zStream.c
    C/Alloc.c
    C/CpuArch.c
    C/LzFind.c
    C/LzFindMt.c
    C/LzFindOpt.c
    C/LzmaDec.c
    C/LzmaEnc.c
    C/Threads.c
)

file(GLOB BROTLI_COMMON_SOURCES "brotli-1.1.0/c/common/*.c")
file(GLOB BROTLI_ENC_SOURCES "brotli-1.1.0/c/enc/*.c")
file(GLOB BROTLI_DEC_SOURCES "brotli-1.1.0/c/dec/*.c")

set(MAIN_SOURCES main.cpp)

set(NOTATION_SOURCES
    lib/janus_notation.cpp
    lib/janus_notation_encoder.cpp
    lib/janus_notation_decoder.cpp
)

add_executable(janus
    ${SEVENZIP_C_SOURCES}
    ${BROTLI_COMMON_SOURCES}
    ${BROTLI_ENC_SOURCES}
    ${BROTLI_DEC_SOURCES}
    ${NOTATION_SOURCES}
    ${MAIN_SOURCES}
)

target_include_directories(janus PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib
    ${CMAKE_SOURCE_DIR}/C
    ${CMAKE_SOURCE_DIR}/brotli-1.1.0/c/include
)

if(WIN32)
    target_link_libraries(janus PRIVATE psapi)
endif()
