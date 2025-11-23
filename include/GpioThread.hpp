#pragma once

#include <QThread>
#include <string>
#include <gpiod.hpp>

// ==============================================================
// Class: GpioThread
// ==============================================================
class GpioThread : public QThread {
    Q_OBJECT
public:
    GpioThread(const std::string &chipName, int lineNum, QObject *parent = nullptr);

    void stop();

signals:
    void buttonPressed();

protected:
    void run() override;

private:
    std::string m_chipName;
    int m_lineNum;
    bool m_running;
};