#include "CameraNode.hpp"
#include "Config.hpp"

#include <QDebug>
#include <QDateTime>
#include <QDir>

using namespace libcamera;

CameraNode::CameraNode(std::shared_ptr<Camera> cam, QObject *parent)
    : QObject(parent), m_camera(cam), m_currentState(State::Idle) {
        
    // 1. Acquire the camera ONCE when the node is created.
    if (m_camera->acquire()) {
        qCritical() << "Failed to acquire camera:" << QString::fromStdString(m_camera->id());
    }

    initDefaultControls();

    connect(this, &CameraNode::captureComplete, this, &CameraNode::startPreview, Qt::QueuedConnection);
}

CameraNode::~CameraNode() {
    stop();
    if (m_camera) m_camera->release();
}

void CameraNode::freeResources() {
    if (m_camera) {
        m_camera->stop();
        m_camera->requestCompleted.disconnect(this, &CameraNode::requestComplete);
    }

    // Clearing the map invokes ScopedMapping destructors, safely unmapping memory.
    m_mappedBuffers.clear();

    m_allocator.reset();
    m_requests.clear();
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

    freeResources();

    // Configure for Preview (1280x1024 - 5:4 aspect ratio)
    m_config = m_camera->generateConfiguration({ StreamRole::Viewfinder });
    StreamConfiguration &streamConfig = m_config->at(0);
    streamConfig.size = {Config::Camera::PreviewWidth, Config::Camera::PreviewHeight};
    streamConfig.pixelFormat = formats::RGB888; 
    
    if (m_config->validate() == CameraConfiguration::Invalid) {
        qWarning() << "Configuration invalid";
        return false;
    }

    if (m_camera->configure(m_config.get())) {
        qWarning() << "Failed to configure camera";
        return false;
    }

    m_allocator = std::make_unique<FrameBufferAllocator>(m_camera);
    Stream *stream = streamConfig.stream();
    
    if (m_allocator->allocate(stream) < 0) {
        qWarning() << "Failed to allocate buffers";
        return false;
    }

    // Map buffers
    const std::vector<std::unique_ptr<FrameBuffer>> &buffers = m_allocator->buffers(stream);
    for (const auto &buffer : buffers) {
        const FrameBuffer::Plane &plane = buffer->planes()[0];
        
        m_mappedBuffers[buffer.get()] = ScopedMapping(plane.fd.get(), plane.length);

        std::unique_ptr<Request> request = m_camera->createRequest();
        if (not request) return false;
        
        if (request->addBuffer(stream, buffer.get()) < 0) return false;
        m_requests.push_back(std::move(request));
    }

    if (m_camera->start(&m_controls)) {
        qWarning() << "Failed to start camera";
        return false;
    }

    m_camera->requestCompleted.connect(this, &CameraNode::requestComplete);

    for (auto &req : m_requests) {
        m_camera->queueRequest(req.get());
    }

    m_currentState = State::Previewing;
    return true;
}

void CameraNode::captureAndSave(const QString &type) {
    // Do not interrupt an existing capture
    if(m_currentState == State::Capturing) {return;}

    freeResources();
    m_currentState = State::Capturing;
    m_capturePrefix = type;

    // Configure for Still
    m_config = m_camera->generateConfiguration({ StreamRole::StillCapture });
    StreamConfiguration &cfg = m_config->at(0);
    
    // 2560x2048 (5:4 Aspect Ratio, 2K Width)
    cfg.size = {Config::Camera::CaptureWidth, Config::Camera::CaptureHeight}; 
    cfg.pixelFormat = formats::RGB888;
    
    m_config->validate();
    m_camera->configure(m_config.get());

    m_allocator = std::make_unique<FrameBufferAllocator>(m_camera);
    Stream *stream = cfg.stream();
    m_allocator->allocate(stream);

    const std::vector<std::unique_ptr<FrameBuffer>> &buffers = m_allocator->buffers(stream);
    for (const auto &buffer : buffers) {
        const FrameBuffer::Plane &plane = buffer->planes()[0];
        
        m_mappedBuffers[buffer.get()] = ScopedMapping(plane.fd.get(), plane.length);
        
        std::unique_ptr<Request> req = m_camera->createRequest();
        req->addBuffer(stream, buffer.get());
        m_requests.push_back(std::move(req));
    }

    m_camera->requestCompleted.connect(this, &CameraNode::requestComplete);
    m_camera->start();
    m_camera->queueRequest(m_requests[0].get());
    
    qDebug() << "Requested Capture matching preview resolution (1280x1024) for" << type;
}

void CameraNode::stop() {
    m_currentState = State::Idle;
    freeResources();
}

QImage CameraNode::getLatestImage() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_currentImage.copy();
}

void CameraNode::requestComplete(Request *request) {
    if (request->status() == Request::RequestCancelled) return;

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
        if (m_mappedBuffers.find(buffer) == m_mappedBuffers.end()) continue;
        if (not m_mappedBuffers[buffer].isValid()) continue;
        
        void *data = m_mappedBuffers[buffer].get();

        StreamConfiguration &cfg = m_config->at(0);

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