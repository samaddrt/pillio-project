cmake_minimum_required(VERSION 3.19)
project(pillio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00)
    if(MINGW)
        add_compile_options(-Wa,-mbig-obj)
    endif()
endif()

# ── Local single-header dependencies ───────────────────────────────
set(THIRD_PARTY_DIR ${CMAKE_SOURCE_DIR}/third_party)

# ── Main executable ─────────────────────────────────────────────────
add_executable(pillio
    src/main.cpp
    src/tracker.cpp
    src/storage.cpp
    src/analytics.cpp
    src/checker.cpp
    src/family.cpp
)

target_include_directories(pillio PRIVATE src ${THIRD_PARTY_DIR})

if(UNIX)
    target_link_libraries(pillio PRIVATE pthread ssl crypto)
elseif(WIN32)
    target_link_libraries(pillio PRIVATE ws2_32)
endif()

# ── Test executable ─────────────────────────────────────────────────
add_executable(pillio_tests
    tests/tests.cpp
    src/tracker.cpp
    src/storage.cpp
    src/analytics.cpp
    src/checker.cpp
    src/family.cpp
)

target_include_directories(pillio_tests PRIVATE src ${THIRD_PARTY_DIR})

enable_testing()
add_test(NAME pillio_tests COMMAND pillio_tests)
