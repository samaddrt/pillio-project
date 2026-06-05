cmake_minimum_required(VERSION 3.19)
project(pillio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00)
    if(MINGW)
        # httplib порождает крупные объектные файлы — нужен big-obj
        add_compile_options(-Wa,-mbig-obj)
    endif()
endif()

# ── Сторонние библиотеки ───────────────────────────────────────────
# Основной путь — пакеты из vcpkg (см. vcpkg.json) через find_package.
# Запасной путь (если vcpkg недоступен) — вкомплектованные заголовки в
# third_party/, обёрнутые в такие же интерфейсные таргеты. Это позволяет
# собрать проект и с vcpkg, и офлайн, не меняя остальной CMake.
set(THIRD_PARTY_DIR ${CMAKE_SOURCE_DIR}/third_party)

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE ${THIRD_PARTY_DIR})
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
endif()

find_package(httplib CONFIG QUIET)
if(NOT httplib_FOUND)
    add_library(httplib INTERFACE)
    target_include_directories(httplib INTERFACE ${THIRD_PARTY_DIR})
    add_library(httplib::httplib ALIAS httplib)
endif()

find_package(doctest CONFIG QUIET)
if(NOT doctest_FOUND)
    add_library(doctest INTERFACE)
    target_include_directories(doctest INTERFACE ${THIRD_PARTY_DIR})
    add_library(doctest::doctest ALIAS doctest)
endif()

# ── Общая доменная логика (используется и приложением, и тестами) ──
set(PILLIO_CORE_SOURCES
    src/tracker.cpp
    src/storage.cpp
    src/family.cpp
)

# ── Основное приложение ────────────────────────────────────────────
add_executable(pillio src/main.cpp ${PILLIO_CORE_SOURCES})
target_include_directories(pillio PRIVATE src)
target_link_libraries(pillio PRIVATE httplib::httplib nlohmann_json::nlohmann_json)

if(UNIX)
    target_link_libraries(pillio PRIVATE pthread)
elseif(WIN32)
    target_link_libraries(pillio PRIVATE ws2_32)
endif()

# ── Тесты (тот же код, что и в приложении) ─────────────────────────
add_executable(pillio_tests tests/tests.cpp ${PILLIO_CORE_SOURCES})
target_include_directories(pillio_tests PRIVATE src)
target_link_libraries(pillio_tests PRIVATE doctest::doctest nlohmann_json::nlohmann_json)

enable_testing()
add_test(NAME pillio_tests COMMAND pillio_tests)
