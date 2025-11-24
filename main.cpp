// Move these includes to the VERY TOP
#include <iostream>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <map>
#include <array> 

#include <libcamera/libcamera.h>
#include <gpiod.hpp>

// Qt includes come AFTER
#include <QApplication>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QImage>
#include <QPixmap>
#include <QTimer>
#include <QThread>
#include <QDateTime>
#include <QDir>
#include <QDebug>

using namespace libcamera;

// ==============================================================
// RAII Wrapper for Memory Mapped Buffers
// Replaces manual munmap handling with automatic cleanup
// ==============================================================
class ScopedMapping {
public:
    // Default constructor (invalid state)
    ScopedMapping() : addr_(MAP_FAILED), len_(0) {}

    // RAII Constructor: Performs mmap immediately
    ScopedMapping(int fd, size_t length) : len_(length) {
        addr_ = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (addr_ == MAP_FAILED) {
            qWarning() << "mmap failed";
            len_ = 0;
        }
    }

    // Destructor: Automatically unmaps memory
    ~ScopedMapping() {
        reset();
    }

    // Disable Copying (to prevent double munmap)
    ScopedMapping(const ScopedMapping&) = delete;
    ScopedMapping& operator=(const ScopedMapping&) = delete;

    // Enable Moving
    ScopedMapping(ScopedMapping&& other) noexcept : addr_(other.addr_), len_(other.len_) {
        other.addr_ = MAP_FAILED;
        other.len_ = 0;
    }

    ScopedMapping& operator=(ScopedMapping&& other) noexcept {
        if (this != &other) {
            reset();
            addr_ = other.addr_;
            len_ = other.len_;
            other.addr_ = MAP_FAILED;
            other.len_ = 0;
        }
        return *this;
    }

    void* get() const { return addr_; }
    bool isValid() const { return addr_ != MAP_FAILED; }

private:
    void reset() {
        if (addr_ != MAP_FAILED && len_ > 0) {
            munmap(addr_, len_);
            addr_ = MAP_FAILED;
            len_ = 0;
        }
    }

    void* addr_;
    size_t len_;
};

// ==============================================================
// Class: CameraNode
// Manages a single libcamera instance (Configuration, Buffers, Requests)
// ==============================================================
class CameraNode : public QObject {
    Q_OBJECT

public:
    CameraNode(std::shared_ptr<Camera> cam, QObject *parent = nullptr)
        : QObject(parent), camera_(cam), capturing_(false) {
            
            // 1. Acquire the camera ONCE when the node is created.
            if (camera_->acquire()) {
                qCritical() << "Failed to acquire camera:" << QString::fromStdString(camera_->id());
            }

            connect(this, &CameraNode::captureComplete, this, &CameraNode::startPreview, Qt::QueuedConnection);
        }

    ~CameraNode() {
        stop();
        if (camera_) camera_->release();
    }

    // Helper to clean up resources
    void freeResources() {
        if (camera_) {
            camera_->stop();
            camera_->requestCompleted.disconnect(this, &CameraNode::requestComplete);
        }

        // SMART POINTER IMPROVEMENT:
        // No manual munmap loop needed anymore.
        // Clearing the map invokes ScopedMapping destructors, safely unmapping memory.
        mappedBuffers_.clear();
        
        allocator_.reset();
        requests_.clear();
    }

public slots:
    bool startPreview() {
        if (!camera_) return false;

        freeResources();

        // Configure for Preview (1280x1024 - 5:4 aspect ratio)
        // Matches 1280 width but extends height for 5:4
        config_ = camera_->generateConfiguration({ StreamRole::Viewfinder });
        StreamConfiguration &streamConfig = config_->at(0);
        streamConfig.size = {1280, 1024};
        streamConfig.pixelFormat = formats::RGB888; 
        
        if (config_->validate() == CameraConfiguration::Invalid) {
             qWarning() << "Configuration invalid";
             return false;
        }

        if (camera_->configure(config_.get())) {
            qWarning() << "Failed to configure camera";
            return false;
        }

        allocator_ = std::make_unique<FrameBufferAllocator>(camera_);
        Stream *stream = streamConfig.stream();
        
        if (allocator_->allocate(stream) < 0) {
            qWarning() << "Failed to allocate buffers";
            return false;
        }

        // Map buffers
        const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator_->buffers(stream);
        for (const auto &buffer : buffers) {
            const FrameBuffer::Plane &plane = buffer->planes()[0];
            
            // SMART POINTER IMPROVEMENT: Use ScopedMapping instead of raw mmap
            mappedBuffers_[buffer.get()] = ScopedMapping(plane.fd.get(), plane.length);

            std::unique_ptr<Request> request = camera_->createRequest();
            if (!request) return false;
            
            if (request->addBuffer(stream, buffer.get()) < 0) return false;
            requests_.push_back(std::move(request));
        }

        // Controls
        ControlList controls;
        if (camera_->controls().find(&controls::AfMode) != camera_->controls().end()) {
            controls.set(controls::AfMode, controls::AfModeContinuous);
        }
        if (camera_->controls().find(&controls::AwbEnable) != camera_->controls().end()) {
             controls.set(controls::AwbEnable, true); 
        }
        if (camera_->controls().find(&controls::AwbMode) != camera_->controls().end()) {
            controls.set(controls::AwbMode, controls::AwbAuto);
        }

        if (camera_->start(&controls)) {
             qWarning() << "Failed to start camera";
             return false;
        }

        camera_->requestCompleted.connect(this, &CameraNode::requestComplete);

        for (auto &req : requests_) {
            camera_->queueRequest(req.get());
        }

        return true;
    }

    void captureAndSave(const QString &type) {
        freeResources();
        
        capturing_ = true;
        capturePrefix_ = type;

        // Configure for Still
        config_ = camera_->generateConfiguration({ StreamRole::StillCapture });
        StreamConfiguration &cfg = config_->at(0);
        
        // UPDATED: 2560x2048 (5:4 Aspect Ratio, 2K Width)
        // Matches the aspect ratio of the UI labels (640x512)
        cfg.size = {2560, 2048}; 
        
        cfg.pixelFormat = formats::RGB888;
        
        config_->validate();
        camera_->configure(config_.get());

        allocator_ = std::make_unique<FrameBufferAllocator>(camera_);
        Stream *stream = cfg.stream();
        allocator_->allocate(stream);

        const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator_->buffers(stream);
        for (const auto &buffer : buffers) {
            const FrameBuffer::Plane &plane = buffer->planes()[0];
            
            // SMART POINTER IMPROVEMENT: Use ScopedMapping
            mappedBuffers_[buffer.get()] = ScopedMapping(plane.fd.get(), plane.length);
            
            std::unique_ptr<Request> req = camera_->createRequest();
            req->addBuffer(stream, buffer.get());
            requests_.push_back(std::move(req));
        }

        camera_->requestCompleted.connect(this, &CameraNode::requestComplete);
        camera_->start();
        camera_->queueRequest(requests_[0].get());
        
        qDebug() << "Requested Capture matching preview resolution (1280x1024) for" << type;
    }

    void stop() {
        freeResources();
    }
    
    QImage getLatestImage() {
        std::lock_guard<std::mutex> lock(mutex_);
        return currentImage_.copy();
    }

signals:
    void frameReady();
    void captureComplete();

private:
    void requestComplete(Request *request) {
        if (request->status() == Request::RequestCancelled) return;

        const std::map<const Stream *, FrameBuffer *> &buffers = request->buffers();
        
        for (auto [stream, buffer] : buffers) {
            if (mappedBuffers_.find(buffer) == mappedBuffers_.end()) continue;
            
            // Retrieve RAII managed pointer
            void *data = mappedBuffers_[buffer].get();
            if (!data) continue;

            StreamConfiguration &cfg = config_->at(0);

            QImage img((uchar*)data, cfg.size.width, cfg.size.height, cfg.stride, QImage::Format_RGB888);
            
            if (capturing_) {
                QString date = QDateTime::currentDateTime().toString("yyyy-MM-dd");
                QString time = QDateTime::currentDateTime().toString("HHmmss");
                QString dirPath = QString("captures/%1/%2").arg(capturePrefix_).arg(date);
                QDir().mkpath(dirPath);
                QString path = dirPath + "/" + time + ".jpg";
                
                img.save(path);
                qDebug() << "Saved Image:" << path << "Size:" << img.size();

                capturing_ = false;
                emit captureComplete(); 
                return; 
            } else {
                std::lock_guard<std::mutex> lock(mutex_);
                currentImage_ = img.copy(); 
            }
        }

        if (!capturing_) {
            request->reuse(Request::ReuseBuffers);
            camera_->queueRequest(request);
            emit frameReady();
        }
    }

    std::shared_ptr<Camera> camera_;
    std::unique_ptr<CameraConfiguration> config_;
    std::unique_ptr<FrameBufferAllocator> allocator_;
    std::vector<std::unique_ptr<Request>> requests_;
    
    // IMPROVEMENT: Map uses ScopedMapping (RAII) instead of raw structs
    std::map<FrameBuffer *, ScopedMapping> mappedBuffers_;
    
    QImage currentImage_;
    std::mutex mutex_;
    
    bool capturing_;
    QString capturePrefix_;
};

// ==============================================================
// Class: GpioThread
// ==============================================================
class GpioThread : public QThread {
    Q_OBJECT
public:
    GpioThread(const std::string &chipName, int lineNum, QObject *parent = nullptr)
        : QThread(parent), chipName_(chipName), lineNum_(lineNum), running_(true) {}

    void stop() {
        running_ = false;
        requestInterruption();
        wait();
    }

signals:
    void buttonPressed();

protected:
    void run() override {
        try {
            gpiod::chip chip(chipName_);
            gpiod::line line = chip.get_line(lineNum_);
            
            gpiod::line_request config;
            config.consumer = "DualCamCpp";
            config.request_type = gpiod::line_request::EVENT_RISING_EDGE;
            
            line.request(config);
            
            while (running_ && !isInterruptionRequested()) {
                if (line.event_wait(std::chrono::seconds(1))) {
                    gpiod::line_event event = line.event_read();
                    if (event.event_type == gpiod::line_event::RISING_EDGE) {
                        emit buttonPressed();
                    }
                }
            }
        } catch (const std::exception &e) {
            qWarning() << "GPIO Error:" << e.what();
        }
    }

private:
    std::string chipName_;
    int lineNum_;
    bool running_;
};

// ==============================================================
// Model: DualCameraModel
// ==============================================================
class DualCameraModel : public QObject {
    Q_OBJECT
public:
    DualCameraModel(CameraManager *cm, QObject *parent = nullptr) 
        : QObject(parent), pendingCaptures_(0) {
        auto cameras = cm->cameras();
        if (cameras.size() < 2) {
            qWarning() << "Not enough cameras found!";
            return;
        }

        noirCam_ = std::make_unique<CameraNode>(cameras[0], nullptr);
        rgbCam_ = std::make_unique<CameraNode>(cameras[1], nullptr);

        connect(noirCam_.get(), &CameraNode::captureComplete, this, &DualCameraModel::onCaptureFinished);
        connect(rgbCam_.get(), &CameraNode::captureComplete, this, &DualCameraModel::onCaptureFinished);

        gpio_ = std::make_unique<GpioThread>("gpiochip0", 27, nullptr);
        connect(gpio_.get(), &GpioThread::buttonPressed, this, &DualCameraModel::buttonPressed);
        gpio_->start();
    }

    void start() {
        noirCam_->startPreview();
        rgbCam_->startPreview();
    }

    void stop() {
        gpio_->stop();
        noirCam_->stop();
        rgbCam_->stop();
    }

    void triggerCapture() {
        pendingCaptures_ = 2;
        rgbCam_->captureAndSave("rgb");
        noirCam_->captureAndSave("noir");
    }

    // Return raw pointers for observation (Standard C++ pattern for non-owning access)
    CameraNode* getRgbNode() { return rgbCam_.get(); }
    CameraNode* getNoirNode() { return noirCam_.get(); }

signals:
    void imageCaptured(QString msg);
    void buttonPressed();

private slots:
    void onCaptureFinished() {
        pendingCaptures_--;
        if (pendingCaptures_ <= 0) {
            emit imageCaptured("Captured RGB + NoIR");
        }
    }

private:
    std::unique_ptr<CameraNode> rgbCam_;
    std::unique_ptr<CameraNode> noirCam_;
    std::unique_ptr<GpioThread> gpio_;
    int pendingCaptures_;
};

// ==============================================================
// View: DualCameraView
// ==============================================================
class DualCameraView : public QWidget {
    Q_OBJECT
public:
    DualCameraView(QWidget *parent = nullptr) : QWidget(parent) {
        setWindowTitle("Dual Camera (RGB + NoIR) - C++");
        
        // NOTE: We keep raw pointers here because Qt's Layout system
        // takes ownership of child widgets. Using smart pointers here
        // typically leads to double-free errors.
        rgbLabel_ = new QLabel("RGB Camera");
        // Updated to 5:4 aspect ratio (640x512 is exactly half of 1280x1024)
        rgbLabel_->setMinimumSize(640, 512);
        rgbLabel_->setAlignment(Qt::AlignCenter);
        rgbLabel_->setStyleSheet("background-color: #222; color: white;");

        noirLabel_ = new QLabel("NoIR Camera");
        // Updated to 5:4 aspect ratio (640x512 is exactly half of 1280x1024)
        noirLabel_->setMinimumSize(640, 512);
        noirLabel_->setAlignment(Qt::AlignCenter);
        noirLabel_->setStyleSheet("background-color: #222; color: white;");

        QHBoxLayout *hbox = new QHBoxLayout();
        hbox->addWidget(rgbLabel_);
        hbox->addWidget(noirLabel_);

        QVBoxLayout *vbox = new QVBoxLayout();
        vbox->addLayout(hbox);
        
        QPushButton *btn = new QPushButton("Capture");
        connect(btn, &QPushButton::clicked, this, &DualCameraView::captureRequested);
        vbox->addWidget(btn);

        setLayout(vbox);
    }

public slots:
    void updateRgb(const QImage &img) {
        if (!img.isNull())
            rgbLabel_->setPixmap(QPixmap::fromImage(img).scaled(rgbLabel_->size(), Qt::KeepAspectRatio));
    }

    void updateNoir(const QImage &img) {
         if (!img.isNull())
            noirLabel_->setPixmap(QPixmap::fromImage(img).scaled(noirLabel_->size(), Qt::KeepAspectRatio));
    }

signals:
    void captureRequested();

private:
    QLabel *rgbLabel_;
    QLabel *noirLabel_;
};

// ==============================================================
// Controller
// ==============================================================
class DualCameraController : public QObject {
    Q_OBJECT
public:
    // SMART POINTER IMPROVEMENT:
    // 1. Use References (&) instead of pointers (*) for Model/View to guarantee they are non-null.
    // 2. Use unique_ptr for QTimer to manage its lifecycle explicitly.
    DualCameraController(DualCameraModel &model, DualCameraView &view) 
        : model_(model), view_(view) {
        
        connect(&model_, &DualCameraModel::buttonPressed, this, &DualCameraController::handleCapture);
        connect(&view_, &DualCameraView::captureRequested, this, &DualCameraController::handleCapture);
        connect(&model_, &DualCameraModel::imageCaptured, this, &DualCameraController::showInfo);

        // Initialize unique_ptr (No parent needed, we own it)
        timer_ = std::make_unique<QTimer>();
        connect(timer_.get(), &QTimer::timeout, this, &DualCameraController::updateFrames);
        timer_->start(50); 
    }

public slots:
    void handleCapture() {
        model_.triggerCapture();
    }

    void updateFrames() {
        view_.updateRgb(model_.getRgbNode()->getLatestImage());
        view_.updateNoir(model_.getNoirNode()->getLatestImage());
    }

    void showInfo(QString msg) {
        QMessageBox::information(&view_, "Info", msg);
    }

private:
    DualCameraModel &model_;
    DualCameraView &view_;
    std::unique_ptr<QTimer> timer_;
};

// ==============================================================
// Main
// ==============================================================
int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    std::unique_ptr<CameraManager> cm = std::make_unique<CameraManager>();
    if (cm->start()) {
        qCritical() << "Failed to start camera manager";
        return -1;
    }

    int ret = 0;
    {
        DualCameraModel model(cm.get());
        DualCameraView view;
        
        // Pass by reference now
        DualCameraController controller(model, view);

        model.start();
        view.showMaximized();

        ret = app.exec();

        model.stop();
    } 

    cm->stop();

    return ret;
}

#include "main.moc"