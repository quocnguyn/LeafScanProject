cmake_minimum_required(VERSION 3.10)
project(DualCamCpp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CRITICAL FIX: Enable automatic processing of Qt macros (Q_OBJECT)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find Qt5
find_package(Qt5 REQUIRED COMPONENTS Widgets Core Gui)

# Find PkgConfig (Required to find libcamera)
find_package(PkgConfig REQUIRED)

# Find libcamera using pkg-config
pkg_check_modules(LIBCAMERA REQUIRED libcamera)

# Find libgpiod (C++ bindings)
pkg_check_modules(GPIODCXX libgpiodcxx)
if (NOT GPIODCXX_FOUND)
    find_library(GPIODCXX_LIBRARIES gpiodcxx REQUIRED)
endif()

# Include directories
include_directories(
    ${LIBCAMERA_INCLUDE_DIRS}
    ${GPIODCXX_INCLUDE_DIRS}
)

# Add link directories
link_directories(
    ${LIBCAMERA_LIBRARY_DIRS}
    ${GPIODCXX_LIBRARY_DIRS}
)

# Source files
set(SOURCES main.cpp)

add_executable(DualCamCpp ${SOURCES})

# Link libraries
target_link_libraries(DualCamCpp PRIVATE
    Qt5::Widgets
    Qt5::Core
    Qt5::Gui
    ${LIBCAMERA_LIBRARIES}
    ${GPIODCXX_LIBRARIES}
)