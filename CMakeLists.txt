cmake_minimum_required(VERSION 3.10)
project(DualCamCpp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable automatic processing of Qt macros (Q_OBJECT)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find Qt5 - Added Concurrent
find_package(Qt5 REQUIRED COMPONENTS Widgets Core Gui Concurrent)

# Find PkgConfig
find_package(PkgConfig REQUIRED)

# Find libcamera
pkg_check_modules(LIBCAMERA REQUIRED libcamera)

# Find libgpiod
pkg_check_modules(GPIODCXX libgpiodcxx)
if (NOT GPIODCXX_FOUND)
    find_library(GPIODCXX_LIBRARIES gpiodcxx REQUIRED)
endif()

include_directories(
    ${LIBCAMERA_INCLUDE_DIRS}
    ${GPIODCXX_INCLUDE_DIRS}
)

link_directories(
    ${LIBCAMERA_LIBRARY_DIRS}
    ${GPIODCXX_LIBRARY_DIRS}
)

set(SOURCES main.cpp)

add_executable(DualCamCpp ${SOURCES})

# Link libraries - Added Qt5::Concurrent
target_link_libraries(DualCamCpp PRIVATE
    Qt5::Widgets
    Qt5::Core
    Qt5::Gui
    Qt5::Concurrent
    ${LIBCAMERA_LIBRARIES}
    ${GPIODCXX_LIBRARIES}
)