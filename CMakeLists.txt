cmake_minimum_required(VERSION 3.21)
project(folio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 DBus Concurrent Test)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
find_package(ZLIB REQUIRED)

# Everything but main.cpp, shared by the app and the tests, so a new source
# file in src/ needs no line here.
file(GLOB folio_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM folio_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(folio_core STATIC ${folio_sources})
target_include_directories(folio_core PUBLIC src)
target_link_libraries(folio_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::DBus Qt6::Concurrent
  PkgConfig::GIO ZLIB::ZLIB)

add_executable(folio src/main.cpp src/resources.qrc)
target_link_libraries(folio PRIVATE folio_core)

# The tests compile the same sources, run offscreen, and read the QML they
# check straight from src/.
add_executable(folio_tests tests/folio_tests.cpp)
target_link_libraries(folio_tests PRIVATE folio_core Qt6::Test)
# The bundled plugins, which the tests run against stubs.
target_compile_definitions(folio_tests PRIVATE FOLIO_PLUGINS="${CMAKE_SOURCE_DIR}/plugins")
enable_testing()
add_test(NAME folio_tests COMMAND folio_tests)
set_tests_properties(folio_tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
