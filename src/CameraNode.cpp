#include "CameraNode.hpp"
#include "Config.hpp"

#include <QDebug>
#include <QDateTime>
#include <QDir>

using namespace libcamera;

CameraStreamSession::CameraStreamSession(std::shared_ptr<libcamera::Camera> cam,
                                        const libcamera::StreamRole &role, const int &width,
                                        const int &height, const ControlList &controls)
        : m_camera(cam)
{
    // 1. Generate Config
    m_config = m_camera->generateConfiguration({ role });
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
        req->addBuffer(stream, buffer.get());
        m_requests.push_back(std::move(req));
    }

    // 4. Start Camera
    if (m_camera->start(&controls) < 0) {
        qWarning() << "Start Failed";
        return;
    }

    // Queue all requests
    for (auto &req : m_requests) {
        m_camera->queueRequest(req.get());
    }
}

CameraStreamSession::~CameraStreamSession() {
    // This runs automatically when m_session is reset
    if (m_camera) {
        m_camera->stop();
    }
}

void* CameraStreamSession::getBufferData(libcamera::FrameBuffer* buffer) {
    if (m_mappedBuffers.find(buffer) == m_mappedBuffers.end()) return nullptr;
    if (!m_mappedBuffers[buffer].isValid()) return nullptr;
    return m_mappedBuffers[buffer].get();
}
const StreamConfiguration& CameraStreamSession::getConfig() const {
    return m_config->at(0);
}

CameraNode::CameraNode(std::shared_ptr<Camera> cam, QObject *parent)
    : QObject(parent), m_camera(cam), m_currentState(State::Idle) {
        
    // 1. Acquire the camera ONCE when the node is created.
    if (m_camera->acquire()) {
        qCritical() << "Failed to acquire camera:" << QString::fromStdString(m_camera->id());
    }

    initDefaultControls();

    connect(this, &CameraNode::captureComplete, this, &CameraNode::startPreview, Qt::QueuedConnection);
    m_camera->requestCompleted.connect(this, &CameraNode::requestComplete);
}

CameraNode::~CameraNode() {
    // 1. Stop the session (stops the camera hardware)
    m_session.reset(); 

    // 2. CRITICAL: Manually disconnect the signal
    if (m_camera) {
        m_camera->requestCompleted.disconnect(this, &CameraNode::requestComplete);
        m_camera->release();
    }
}

void CameraNode::initDefaultControls() {
    m_controls = ControlList(m_camera->controls());

    auto isInControlList = [=](const auto &id){
        auto foundId = m_camera->controls().find(id);
        return foundId != m_camera->controls().end();
    };

    if (isInControlList(&controls::AfMode)) {
        m_controls.set(controls::AfMode, controls::AfModeContinuous);
    }
    
    if (isInControlList(&controls::AwbEnable)) {
        m_controls.set(controls::AwbEnable, false); 
    }

    bool isAwbEnable = m_controls.get(controls::AwbEnable).value();
    if (isInControlList(&controls::AwbMode)) {
        auto mode = isAwbEnable ? controls::AwbAuto : controls::AwbCustom;
        m_controls.set(controls::AwbMode, mode);
    }

    if (not isAwbEnable && isInControlList(&controls::ColourGains)) {
        m_controls.set(controls::ColourGains, Config::Camera::DefaultGains);
    }
}

bool CameraNode::startPreview() {
    if (not m_camera) return false;

    m_session = std::make_unique<CameraStreamSession>(
        m_camera, 
        StreamRole::Viewfinder, 
        Config::Camera::PreviewWidth, 
        Config::Camera::PreviewHeight,
        m_controls
    );

    m_currentState = State::Previewing;
    return true;
}

void CameraNode::captureAndSave(const QString &type) {
    // Do not interrupt an existing capture
    if(m_currentState == State::Capturing) {return;}

    m_currentState = State::Capturing;
    m_capturePrefix = type;

    m_session = std::make_unique<CameraStreamSession>(
        m_camera, 
        StreamRole::StillCapture, 
        Config::Camera::CaptureWidth, 
        Config::Camera::CaptureHeight,
        m_controls
    );
    
    qDebug() << "Requested Capture matching preview resolution (1280x1024) for" << type;
}

void CameraNode::stop() {
    m_currentState = State::Idle;
    m_session.reset();
}

QImage CameraNode::getLatestImage() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_currentImage.copy();
}

void CameraNode::requestComplete(Request *request) {
    if (request->status() == Request::RequestCancelled) return;
    if (m_currentState == State::Idle || !m_session) return;

    const std::map<const Stream *, FrameBuffer *> &buffers = request->buffers();

    auto saveFrame = [this](QImage &capturedFrame) -> void {
        QString date = QDateTime::currentDateTime().toString("yyyy-MM-dd");
        QString time = QDateTime::currentDateTime().toString("HHmmss");
        QString dirPath = QString("captures/%1/%2").arg(m_capturePrefix).arg(date);
        QDir().mkpath(dirPath);
        QString path = dirPath + "/" + time + ".jpg";
        
        capturedFrame.save(path);
        qDebug() << "Saved Image:" << path << "Size:" << capturedFrame.size();
    };
    
    for (auto [stream, buffer] : buffers) {
        void *data = m_session->getBufferData(buffer);
        if (!data) continue;

        const StreamConfiguration &cfg = m_session->getConfig();

        QImage img((uchar*)data, cfg.size.width, cfg.size.height, cfg.stride, QImage::Format_BGR888);

        switch(m_currentState.load()){
            case State::Capturing: {
                saveFrame(img);
                emit captureComplete();
                return;
            }
            case State::Previewing: {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_currentImage = img.copy(); 
            }
        }
    }

    if (m_currentState == State::Previewing) {
        request->reuse(Request::ReuseBuffers);
        m_camera->queueRequest(request);
        emit frameReady();
    }
}