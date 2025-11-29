#include "CameraNode.hpp"
#include "Config.hpp"

#include <QDebug>
#include <QDateTime>
#include <QDir>

using namespace libcamera;

CameraLock::CameraLock(std::shared_ptr<Camera> cam)
    : m_cam(cam), m_locked(false) 
{
    if(m_cam && m_cam->acquire() == 0) {
        m_locked = true;
    }
    else {
        qWarning() << "Failed to acquire camera lock for" << (m_cam ? QString::fromStdString(m_cam->id()) : "Unknown");
    }
}

CameraLock::~CameraLock() {
    if (m_cam && m_locked) {
        m_cam->release();
    }
}

bool CameraLock::isLocked() const { return m_locked; }

CameraStreamSession::CameraStreamSession(std::shared_ptr<Camera> cam, 
                                         StreamRole role, 
                                         int width, int height,
                                         const ControlList &controls)
    : m_camera(cam), m_valid(false)
{
    // 1. Generate Config
    m_config = m_camera->generateConfiguration({ role });
    if (!m_config) {
        qWarning() << "Failed to generate configuration";
        return;
    }

    StreamConfiguration &cfg = m_config->at(0);
    cfg.size = { static_cast<unsigned int>(width), static_cast<unsigned int>(height) };
    cfg.pixelFormat = formats::RGB888;
    
    if (m_config->validate() == CameraConfiguration::Invalid) {
        qWarning() << "Invalid Camera Configuration";
        return; 
    }
    
    if (m_camera->configure(m_config.get()) < 0) {
        qWarning() << "Configure Failed";
        return;
    }

    // 2. Allocate Buffers
    m_allocator = std::make_unique<FrameBufferAllocator>(m_camera);
    Stream *stream = cfg.stream();
    if (m_allocator->allocate(stream) < 0) {
        qWarning() << "Allocation Failed";
        return;
    }

    // 3. Map Buffers & Create Requests
    const auto &buffers = m_allocator->buffers(stream);
    for (const auto &buffer : buffers) {
        const FrameBuffer::Plane &plane = buffer->planes()[0];
        m_mappedBuffers[buffer.get()] = ScopedMapping(plane.fd.get(), plane.length);
        
        std::unique_ptr<Request> req = m_camera->createRequest();
        if (!req) {
            qWarning() << "Failed to create request";
            return;
        }
        
        if (req->addBuffer(stream, buffer.get()) < 0) {
            qWarning() << "Failed to add buffer to request";
            return;
        }
        m_requests.push_back(std::move(req));
    }

    // 4. Start Camera with persistent controls
    // Use the controls passed from CameraNode
    if (m_camera->start(&controls) < 0) {
        qWarning() << "Start Failed";
        return;
    }

    // Queue all requests
    for (auto &req : m_requests) {
        m_camera->queueRequest(req.get());
    }

    m_valid = true;
}

CameraStreamSession::~CameraStreamSession() {
    if (m_camera) { m_camera->stop(); }
}

void* CameraStreamSession::getBufferData(FrameBuffer* buffer) {
    if (m_mappedBuffers.find(buffer) == m_mappedBuffers.end()) return nullptr;
    if (!m_mappedBuffers[buffer].isValid()) return nullptr;
    return m_mappedBuffers[buffer].get();
}

const StreamConfiguration& CameraStreamSession::getConfig() const {
    return m_config->at(0);
}

bool CameraStreamSession::isValid() const { return m_valid; }

// ============================================================================
// CameraNode Implementation
// ============================================================================

CameraNode::CameraNode(std::shared_ptr<Camera> cam, QObject *parent)
    : QObject(parent), m_camera(cam), m_currentState(State::Idle) {
    
    // 1. Acquire Camera safely
    m_cameraLock = std::make_unique<CameraLock>(m_camera);
    
    // 2. Check if we actually got the lock
    if (not m_cameraLock->isLocked()) {
        qCritical() << "Critical: Could not acquire camera" << QString::fromStdString(m_camera->id());
        return; 
    }
    
    // 3. Initialize Controls (since we own the camera now)
    initDefaultControls();

    connect(this, &CameraNode::captureComplete, this, &CameraNode::startPreview, Qt::QueuedConnection);
    
    // Connect the libcamera signal
    // Note: We will manually disconnect this in destructor
    m_camera->requestCompleted.connect(this, &CameraNode::requestComplete);
}

CameraNode::~CameraNode() {
    // 1. Stop active session first
    m_session.reset(); 

    // 2. Manually disconnect signal to prevent crashes after destruction
    if (m_camera) {
        m_camera->requestCompleted.disconnect(this);
    }
    
    // 3. Lock releases automatically
}

void CameraNode::initDefaultControls() {
    // Copy capabilities
    m_controls = ControlList(m_camera->controls());

    auto isInControlList = [=](const auto &id){
        return m_camera->controls().find(id) != m_camera->controls().end();
    };

    if (isInControlList(&controls::AfMode)) {
        m_controls.set(controls::AfMode, controls::AfModeContinuous);
    }
    
    if (isInControlList(&controls::AwbEnable)) {
        m_controls.set(controls::AwbEnable, false); 
    }

    if (isInControlList(&controls::ColourGains)) {
        m_controls.set(controls::ColourGains, Config::Camera::DefaultGains);
    }
}

void CameraNode::stop() {
    m_currentState = State::Idle;
    m_session.reset();
}

bool CameraNode::startPreview() {
    if (!m_cameraLock->isLocked()) return false;

    // Create new session
    m_session.reset();
    auto newSession = std::make_unique<CameraStreamSession>(
        m_camera, 
        StreamRole::Viewfinder, 
        Config::Camera::PreviewWidth, 
        Config::Camera::PreviewHeight,
        m_controls
    );

    if (!newSession->isValid()) { return false; }

    m_session = std::move(newSession);
    m_currentState = State::Previewing;
    return true;
}

void CameraNode::captureAndSave(const QString &type) {
    if (!m_cameraLock->isLocked()) return;
    if (m_currentState == State::Capturing) return; // Prevent double trigger
    
    m_capturePrefix = type;
    m_currentState = State::Capturing;

    // Create capture session
    m_session.reset();
    auto captureSession = std::make_unique<CameraStreamSession>(
        m_camera, 
        StreamRole::StillCapture, 
        Config::Camera::CaptureWidth, 
        Config::Camera::CaptureHeight,
        m_controls
    );

    if (!captureSession->isValid()) {
        qWarning() << "Failed to start capture session";
        // Revert to preview
        m_currentState = State::Previewing;
        emit captureComplete();
        return;
    }

    m_session = std::move(captureSession);
    qDebug() << "Requested Capture for" << type;
}

void CameraNode::requestComplete(Request *request) {
    if (request->status() == Request::RequestCancelled) return;
    
    // Safety check: ensure we have a session and are running
    if (m_currentState == State::Idle || not m_session) return;

    for (auto [stream, buffer] : request->buffers()) {
        void *data = m_session->getBufferData(buffer);
        if (!data) continue;

        const StreamConfiguration &cfg = m_session->getConfig();
        QImage img((uchar*)data, cfg.size.width, cfg.size.height, cfg.stride, QImage::Format_BGR888);
        
        switch (m_currentState.load()) {
            case State::Capturing: {
                QString date = QDateTime::currentDateTime().toString("yyyy-MM-dd");
                QString time = QDateTime::currentDateTime().toString("HHmmss");
                QString dirPath = QString("captures/%1/%2").arg(m_capturePrefix).arg(date);
                QDir().mkpath(dirPath);
                QString path = dirPath + "/" + time + ".jpg";
                
                img.save(path);
                qDebug() << "Saved Image:" << path;

                emit captureComplete(); 
                return; // Stop processing
            }
            case State::Previewing: {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_currentImage = img.copy(); 
                break;
            }
            default: break;
        }
    }

    // Reuse buffers only if we are still previewing
    if (m_currentState == State::Previewing) {
        request->reuse(Request::ReuseBuffers);
        m_camera->queueRequest(request);
        emit frameReady();
    }
}

QImage CameraNode::getLatestImage() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_currentImage.copy();
}