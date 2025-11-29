#pragma once

#include <libcamera/libcamera.h>

#include "ScopedMapping.hpp"

#include <QObject>
#include <QImage>
#include <QDebug>
#include <mutex>
#include <memory>
#include <vector>
#include <map>
#include <atomic>

// ==============================================================
// RAII Wrapper: Handles Camera Acquire/Release safely
// ==============================================================
class CameraLock {
public:
    explicit CameraLock(std::shared_ptr<libcamera::Camera> cam);
    ~CameraLock();
    
    bool isLocked() const;

    // Disable copy/move
    CameraLock(const CameraLock&) = delete;
    CameraLock& operator=(const CameraLock&) = delete;

private:
    std::shared_ptr<libcamera::Camera> m_cam;
    bool m_locked;
};

// ==============================================================
// RAII Wrapper: Handles a Running Camera Session
// (Config + Buffers + Requests + Start/Stop)
// ==============================================================
class CameraStreamSession {
public:
    CameraStreamSession(std::shared_ptr<libcamera::Camera> cam, 
                        libcamera::StreamRole role, 
                        int width, int height,
                        const libcamera::ControlList &controls);
    ~CameraStreamSession();

    void* getBufferData(libcamera::FrameBuffer* buffer);
    const libcamera::StreamConfiguration& getConfig() const;
    bool isValid() const;

private:
    std::shared_ptr<libcamera::Camera> m_camera;
    std::unique_ptr<libcamera::CameraConfiguration> m_config;
    std::unique_ptr<libcamera::FrameBufferAllocator> m_allocator;
    std::vector<std::unique_ptr<libcamera::Request>> m_requests;
    std::map<libcamera::FrameBuffer *, ScopedMapping> m_mappedBuffers;
    bool m_valid;
};

// ==============================================================
// Class: CameraNode
// ==============================================================
class CameraNode : public QObject {
    Q_OBJECT

public:
    enum class State { Idle, Previewing, Capturing };

    explicit CameraNode(std::shared_ptr<libcamera::Camera> cam, QObject *parent = nullptr);
    ~CameraNode();

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
    std::unique_ptr<CameraLock> m_cameraLock;
    std::unique_ptr<CameraStreamSession> m_session;

    // Persistent Controls (reused between sessions)
    libcamera::ControlList m_controls;

    QImage m_currentImage;
    std::mutex m_mutex;
    std::atomic<State> m_currentState;
    QString m_capturePrefix;
};