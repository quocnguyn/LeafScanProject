#pragma once

#include <QString>
#include <array>

namespace Config {

    // Camera Settings
    namespace Camera {
        // Preview: 1280x1024 (5:4 aspect ratio)
        static constexpr int PreviewWidth = 1280;
        static constexpr int PreviewHeight = 1024;
        
        // Capture: 2560x2048 (5:4 aspect ratio, 2K Width)
        static constexpr int CaptureWidth = 2560;
        static constexpr int CaptureHeight = 2048;

        // Manual Gains for Red and Blue channels
        static constexpr std::array<float, 2> DefaultGains = {2.1f, 2.1f};
    }

    // Hardware / GPIO Settings
    namespace Gpio {
        static constexpr const char* ChipName = "gpiochip0";
        static constexpr int TriggerButtonLine = 27; 
        static constexpr int DebounceTimeSec = 1;
    }

    // User Interface Settings
    namespace Ui {
        static constexpr int RefreshIntervalMs = 50; // 20 FPS
        static constexpr int ViewLabelWidth = 640;
        static constexpr int ViewLabelHeight = 512;
    }
}