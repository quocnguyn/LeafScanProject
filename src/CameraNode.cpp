#include "CameraNode.hpp"

#include <QDebug>
#include <QDateTime>
#include <QDir>

using namespace libcamera;

CameraNode::CameraNode(std::shared_ptr<Camera> cam, QObject *parent)
    : QObject(parent), m_camera(cam), m_capturing(false) {
        
    // 1. Acquire the camera ONCE when the node is created.
    if (m_camera->acquire()) {
        qCritical() << "Failed to acquire camera:" << QString::fromStdString(m_camera->id());
    }

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

bool CameraNode::startPreview() {
    if (!m_camera) return false;

    freeResources();

    // Configure for Preview (1280x1024 - 5:4 aspect ratio)
    m_config = m_camera->generateConfiguration({ StreamRole::Viewfinder });
    StreamConfiguration &streamConfig = m_config->at(0);
    streamConfig.size = {1280, 1024};
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
        if (!request) return false;
        
        if (request->addBuffer(stream, buffer.get()) < 0) return false;
        m_requests.push_back(std::move(request));
    }

    // Controls
    ControlList controls;
    auto isInControlList = [=](const auto &id){
        auto foundId = m_camera->controls().find(id);
        return foundId != m_camera->controls().end();
    };

    if (isInControlList(&controls::AfMode)) {
        controls.set(controls::AfMode, controls::AfModeContinuous);
    }
    
    if (isInControlList(&controls::AwbEnable)) {
        controls.set(controls::AwbEnable, true); 
    }

    bool isAwbEnable = controls.get(controls::AwbEnable).value();
    if (isInControlList(&controls::AwbMode)) {
        if (isAwbEnable) {
            controls.set(controls::AwbMode, controls::AwbAuto);
        }
        else {
            controls.set(controls::AwbMode, controls::AwbCustom);
        }
    }

    if (not isAwbEnable and isInControlList(&controls::ColourGains)) {
        std::array<float, 2u> manualGains = {2.1f, 2.1f};
        controls.set(controls::ColourGains, manualGains);
    }

    if (m_camera->start(&controls)) {
            qWarning() << "Failed to start camera";
            return false;
    }

    m_camera->requestCompleted.connect(this, &CameraNode::requestComplete);

    for (auto &req : m_requests) {
        m_camera->queueRequest(req.get());
    }

    return true;
}

void CameraNode::captureAndSave(const QString &type) {
    freeResources();
    
    m_capturing = true;
    m_capturePrefix = type;

    // Configure for Still
    m_config = m_camera->generateConfiguration({ StreamRole::StillCapture });
    StreamConfiguration &cfg = m_config->at(0);
    
    // 2560x2048 (5:4 Aspect Ratio, 2K Width)
    cfg.size = {2560, 2048}; 
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
    freeResources();
}

QImage CameraNode::getLatestImage() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_currentImage.copy();
}

void CameraNode::requestComplete(Request *request) {
    if (request->status() == Request::RequestCancelled) return;

    const std::map<const Stream *, FrameBuffer *> &buffers = request->buffers();
    
    for (auto [stream, buffer] : buffers) {
        if (m_mappedBuffers.find(buffer) == m_mappedBuffers.end()) continue;
        
        void *data = m_mappedBuffers[buffer].get();
        if (!data) continue;

        StreamConfiguration &cfg = m_config->at(0);

        QImage img((uchar*)data, cfg.size.width, cfg.size.height, cfg.stride, QImage::Format_BGR888);
        
        if (m_capturing) {
            QString date = QDateTime::currentDateTime().toString("yyyy-MM-dd");
            QString time = QDateTime::currentDateTime().toString("HHmmss");
            QString dirPath = QString("captures/%1/%2").arg(m_capturePrefix).arg(date);
            QDir().mkpath(dirPath);
            QString path = dirPath + "/" + time + ".jpg";
            
            img.save(path);
            qDebug() << "Saved Image:" << path << "Size:" << img.size();

            m_capturing = false;
            emit captureComplete(); 
            return; 
        } else {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_currentImage = img.copy(); 
        }
    }

    if (!m_capturing) {
        request->reuse(Request::ReuseBuffers);
        m_camera->queueRequest(request);
        emit frameReady();
    }
}