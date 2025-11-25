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
// RAII Wrapper: Handles a Running Camera Session
// (Config + Buffers + Requests + Start/Stop)
// ==============================================================
class CameraStreamSession {
public:
    CameraStreamSession(std::shared_ptr<libcamera::Camera> cam, const libcamera::StreamRole &role,
                        const int &width, const int &height, const libcamera::ControlList &controls);
    
    ~CameraStreamSession();

    // Helper to access mapped memory for a specific buffer
    void* getBufferData(libcamera::FrameBuffer* buffer);
    const libcamera::StreamConfiguration& getConfig() const;

private:
    std::shared_ptr<libcamera::Camera> m_camera;
    std::unique_ptr<libcamera::CameraConfiguration> m_config;
    std::unique_ptr<libcamera::FrameBufferAllocator> m_allocator;
    std::vector<std::unique_ptr<libcamera::Request>> m_requests;
    std::map<libcamera::FrameBuffer *, ScopedMapping> m_mappedBuffers;
};

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
    // RAII Members
    std::unique_ptr<CameraStreamSession> m_session;

    QImage m_currentImage;
    std::mutex m_mutex;
    std::atomic<State> m_currentState;
    libcamera::ControlList m_controls;
    QString m_capturePrefix;
};