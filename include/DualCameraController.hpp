#pragma once

#include "DualCameraModel.hpp"
#include "DualCameraView.hpp"

#include <QObject>
#include <QTimer>
#include <memory>

// ==============================================================
// Controller
// ==============================================================
class DualCameraController : public QObject {
    Q_OBJECT
public:
    DualCameraController(DualCameraModel &model, DualCameraView &view);

public slots:
    void handleCapture();
    void updateFrames();
    void showInfo(QString msg);

private:
    DualCameraModel &m_model;
    DualCameraView &m_view;
    std::unique_ptr<QTimer> m_timer;
};