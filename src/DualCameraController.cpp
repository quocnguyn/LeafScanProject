#include "DualCameraController.hpp"

#include <QMessageBox>

DualCameraController::DualCameraController(DualCameraModel &model, DualCameraView &view) 
    : m_model(model), m_view(view) {
    
    connect(&m_model, &DualCameraModel::buttonPressed, this, &DualCameraController::handleCapture);
    connect(&m_view, &DualCameraView::captureRequested, this, &DualCameraController::handleCapture);
    connect(&m_model, &DualCameraModel::imageCaptured, this, &DualCameraController::showInfo);

    m_timer = std::make_unique<QTimer>();
    connect(m_timer.get(), &QTimer::timeout, this, &DualCameraController::updateFrames);
    m_timer->start(50); 
}

void DualCameraController::handleCapture() {
    m_model.triggerCapture();
}

void DualCameraController::updateFrames() {
    m_view.updateRgb(m_model.getRgbNode()->getLatestImage());
    m_view.updateNoir(m_model.getNoirNode()->getLatestImage());
}

void DualCameraController::showInfo(QString msg) {
    QMessageBox::information(&m_view, "Info", msg);
}