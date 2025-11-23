cmake_minimum_required(VERSION 3.10)
project(DualCamCpp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==============================================================================
# Qt Configuration
# ==============================================================================
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 REQUIRED COMPONENTS Widgets Core Gui)

# ==============================================================================
# Dependencies
# ==============================================================================
find_package(PkgConfig REQUIRED)

# Find libcamera
pkg_check_modules(LIBCAMERA REQUIRED libcamera)

# Find libgpiod
pkg_check_modules(GPIODCXX libgpiodcxx)
if (NOT GPIODCXX_FOUND)
    find_library(GPIODCXX_LIBRARIES gpiodcxx REQUIRED)
endif()

# ==============================================================================
# Include Directories
# ==============================================================================
include_directories(
    include
    ${LIBCAMERA_INCLUDE_DIRS}
    ${GPIODCXX_INCLUDE_DIRS}
)

# ==============================================================================
# Link Directories
# ==============================================================================
link_directories(
    ${LIBCAMERA_LIBRARY_DIRS}
    ${GPIODCXX_LIBRARY_DIRS}
)

# ==============================================================================
# Files
# ==============================================================================

# Explicitly list headers so AUTOMOC processes them for Q_OBJECT
set(HEADERS
    include/ScopedMapping.hpp
    include/CameraNode.hpp
    include/GpioThread.hpp
    include/DualCameraModel.hpp
    include/DualCameraView.hpp
    include/DualCameraController.hpp
)

set(SOURCES
    src/main.cpp
    src/ScopedMapping.cpp
    src/CameraNode.cpp
    src/GpioThread.cpp
    src/DualCameraModel.cpp
    src/DualCameraView.cpp
    src/DualCameraController.cpp
)

# ==============================================================================
# Build Target
# ==============================================================================
# We add both SOURCES and HEADERS to the executable. 
# This ensures CMake knows these headers belong to this target for MOC generation.
add_executable(DualCamCpp ${SOURCES} ${HEADERS})

target_link_libraries(DualCamCpp PRIVATE
    Qt5::Widgets
    Qt5::Core
    Qt5::Gui
    ${LIBCAMERA_LIBRARIES}
    ${GPIODCXX_LIBRARIES}
)