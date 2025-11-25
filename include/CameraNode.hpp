#pragma once

#include <libcamera/libcamera.h>

#include "ScopedMapping.hpp"

#include <QObject>
#include <QImage>
#include <mutex>
#include <memory>
#include <vector>
#include <map>

// ==============================================================
// Class: CameraNode
// Manages a single libcamera instance (Configuration, Buffers, Requests)
// ==============================================================
class CameraNode : public QObject {
    Q_OBJECT

public:
    enum class State {
        Idle,
        Previewing,
        Capturing
    };

    explicit CameraNode(std::shared_ptr<libcamera::Camera> cam, QObject *parent = nullptr);
    ~CameraNode();

    void freeResources();
    QImage getLatestImage();
    void stop();

public slots:
    bool startPreview();
    void captureAndSave(const QString &type);

signals:
    void frameReady();
    void captureComplete();

private:
    void requestComplete(libcamera::Request *request);
    void initDefaultControls();

    std::shared_ptr<libcamera::Camera> m_camera;
    std::unique_ptr<libcamera::CameraConfiguration> m_config;
    std::unique_ptr<libcamera::FrameBufferAllocator> m_allocator;
    std::vector<std::unique_ptr<libcamera::Request>> m_requests;

    // Map uses ScopedMapping (RAII)
    std::map<libcamera::FrameBuffer *, ScopedMapping> m_mappedBuffers;

    QImage m_currentImage;
    std::mutex m_mutex;
    std::atomic<State> m_currentState;
    libcamera::ControlList m_controls;
    QString m_capturePrefix;
};