#pragma once

#include <libcamera/libcamera.h>

#include "CameraNode.hpp"
#include "GpioThread.hpp"

#include <QObject>
#include <memory>

// ==============================================================
// Model: DualCameraModel
// ==============================================================
class DualCameraModel : public QObject {
    Q_OBJECT
public:
    explicit DualCameraModel(libcamera::CameraManager *camManager, QObject *parent = nullptr);

    void start();
    void stop();
    void triggerCapture();

    // Return raw pointers for observation (Standard C++ pattern for non-owning access)
    CameraNode* getRgbNode();
    CameraNode* getNoirNode();

signals:
    void imageCaptured(QString msg);
    void buttonPressed();

private slots:
    void onCaptureFinished();

private:
    std::unique_ptr<CameraNode> m_rgbCam;
    std::unique_ptr<CameraNode> m_noirCam;
    std::unique_ptr<GpioThread> m_gpio;
    int m_pendingCaptures;
};