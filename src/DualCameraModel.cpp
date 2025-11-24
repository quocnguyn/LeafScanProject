#include "DualCameraModel.hpp"
#include "Config.hpp"

#include <QDebug>

using namespace libcamera;

DualCameraModel::DualCameraModel(CameraManager *camManager, QObject *parent) 
    : QObject(parent), m_pendingCaptures(0) {
    auto cameras = camManager->cameras();
    if (cameras.size() < 2) {
        qWarning() << "Not enough cameras found!";
        return;
    }
    m_noirCam = std::make_unique<CameraNode>(cameras[0], nullptr);
    m_rgbCam = std::make_unique<CameraNode>(cameras[1], nullptr);

    connect(m_noirCam.get(), &CameraNode::captureComplete, this, &DualCameraModel::onCaptureFinished);
    connect(m_rgbCam.get(), &CameraNode::captureComplete, this, &DualCameraModel::onCaptureFinished);

    m_gpio = std::make_unique<GpioThread>(Config::Gpio::ChipName, Config::Gpio::TriggerButtonLine, nullptr);
    connect(m_gpio.get(), &GpioThread::buttonPressed, this, &DualCameraModel::buttonPressed);
    m_gpio->start();
}

void DualCameraModel::start() {
    m_noirCam->startPreview();
    m_rgbCam->startPreview();
}

void DualCameraModel::stop() {
    m_gpio->stop();
    m_noirCam->stop();
    m_rgbCam->stop();
}

void DualCameraModel::triggerCapture() {
    m_pendingCaptures = 2;
    m_rgbCam->captureAndSave("rgb");
    m_noirCam->captureAndSave("noir");
}

CameraNode* DualCameraModel::getRgbNode() { return m_rgbCam.get(); }
CameraNode* DualCameraModel::getNoirNode() { return m_noirCam.get(); }

void DualCameraModel::onCaptureFinished() {
    m_pendingCaptures--;
    if (m_pendingCaptures <= 0) {
        emit imageCaptured("Captured RGB + NoIR");
    }
}