#include "GpioThread.hpp"

#include <QDebug>
#include <chrono>

GpioThread::GpioThread(const std::string &chipName, int lineNum, QObject *parent)
    : QThread(parent), m_chipName(chipName), m_lineNum(lineNum), m_running(true) {}

void GpioThread::stop() {
    m_running = false;
    requestInterruption();
    wait();
}

void GpioThread::run() {
    try {
        gpiod::chip chip(m_chipName);
        gpiod::line line = chip.get_line(m_lineNum);
        
        gpiod::line_request config;
        config.consumer = "DualCamCpp";
        config.request_type = gpiod::line_request::EVENT_RISING_EDGE;
        
        line.request(config);
        
        while (m_running && !isInterruptionRequested()) {
            if (line.event_wait(std::chrono::seconds(1))) {
                gpiod::line_event event = line.event_read();
                if (event.event_type == gpiod::line_event::RISING_EDGE) {
                    emit buttonPressed();
                }
            }
        }
    } catch (const std::exception &e) {
        qWarning() << "GPIO Error:" << e.what();
    }
}