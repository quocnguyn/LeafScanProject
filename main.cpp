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
#include <QSizePolicy>
#include <QSize> // Added for QSize

using namespace libcamera;

// ==============================================================
// RAII Wrapper for Memory Mapped Buffers
// ==============================================================
class ScopedMapping {
public:
    ScopedMapping() : addr_(MAP_FAILED), len_(0) {}

    ScopedMapping(int fd, size_t length) : len_(length) {
        addr_ = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (addr_ == MAP_FAILED) {
            qWarning() << "mmap failed";
            len_ = 0;
        }
    }

    ~ScopedMapping() { reset(); }

    ScopedMapping(const ScopedMapping&) = delete;
    ScopedMapping& operator=(const ScopedMapping&) = delete;

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
// ==============================================================
class CameraNode : public QObject {
    Q_OBJECT

public:
    CameraNode(std::shared_ptr<Camera> cam, QObject *parent = nullptr)
        : QObject(parent), camera_(cam), capturing_(false) {
            
            if (camera_->acquire()) {
                qCritical() << "Failed to acquire camera:" << QString::fromStdString(camera_->id());
            }

            connect(this, &CameraNode::captureComplete, this, &CameraNode::startPreview, Qt::QueuedConnection);
        }

    ~CameraNode() {
        stop();
        if (camera_) camera_->release();
    }

    void freeResources() {
        if (camera_) {
            camera_->stop();
            camera_->requestCompleted.disconnect(this, &CameraNode::requestComplete);
        }

        mappedBuffers_.clear();
        allocator_.reset();
        requests_.clear();
    }

public slots:
    bool startPreview() {
        if (!camera_) return false;

        freeResources();

        // Configure for Preview (1280x1024 - 5:4 aspect ratio)
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

        const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator_->buffers(stream);
        for (const auto &buffer : buffers) {
            const FrameBuffer::Plane &plane = buffer->planes()[0];
            mappedBuffers_[buffer.get()] = ScopedMapping(plane.fd.get(), plane.length);

            std::unique_ptr<Request> request = camera_->createRequest();
            if (!request) return false;
            
            if (request->addBuffer(stream, buffer.get()) < 0) return false;
            requests_.push_back(std::move(request));
        }

        ControlList controls;
        if (camera_->controls().find(&controls::AfMode) != camera_->controls().end()) {
            controls.set(controls::AfMode, controls::AfModeContinuous);
        }
        if (camera_->controls().find(&controls::AwbEnable) != camera_->controls().end()) {
             controls.set(controls::AwbEnable, false); 
        }

        bool isAwbEnable = controls.get(controls::AwbEnable).value();
        if (isAwbEnable && camera_->controls().find(&controls::AfMode) != camera_->controls().end()) {
            auto mode = isAwbEnable ? controls::AwbAuto : controls::AwbCustom;
            controls.set(controls::AwbMode, mode);
        }

        if (not isAwbEnable && camera_->controls().find(&controls::ColourGains) != camera_->controls().end()) {
            std::array<float, 2> manualGain = {2.1f, 2.1f};
            controls.set(controls::ColourGains, manualGain);
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

    // UPDATED: Accepts specific size for capture
    void captureAndSave(const QString &type, const QSize &targetSize) {
        freeResources();
        
        capturing_ = true;
        capturePrefix_ = type;

        // Configure for Still with DYNAMIC SIZE
        config_ = camera_->generateConfiguration({ StreamRole::StillCapture });
        StreamConfiguration &cfg = config_->at(0);
        
        // Convert QSize to libcamera Size
        cfg.size = { static_cast<unsigned int>(targetSize.width()), static_cast<unsigned int>(targetSize.height()) };
        cfg.pixelFormat = formats::RGB888;
        
        config_->validate();
        camera_->configure(config_.get());

        allocator_ = std::make_unique<FrameBufferAllocator>(camera_);
        Stream *stream = cfg.stream();
        allocator_->allocate(stream);

        const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator_->buffers(stream);
        for (const auto &buffer : buffers) {
            const FrameBuffer::Plane &plane = buffer->planes()[0];
            mappedBuffers_[buffer.get()] = ScopedMapping(plane.fd.get(), plane.length);
            
            std::unique_ptr<Request> req = camera_->createRequest();
            req->addBuffer(stream, buffer.get());
            requests_.push_back(std::move(req));
        }

        camera_->requestCompleted.connect(this, &CameraNode::requestComplete);
        camera_->start();
        camera_->queueRequest(requests_[0].get());
        
        qDebug() << "Requested Capture for" << type << "at resolution" << targetSize;
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
            
            void *data = mappedBuffers_[buffer].get();
            if (!data) continue;

            StreamConfiguration &cfg = config_->at(0);

            QImage img((uchar*)data, cfg.size.width, cfg.size.height, cfg.stride, QImage::Format_BGR888);
            
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

        gpio_ = std::make_unique<GpioThread>("gpiochip0", 24, nullptr);
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

    // UPDATED: Accepts target resolution
    void triggerCapture(QSize targetSize) {
        pendingCaptures_ = 2;
        rgbCam_->captureAndSave("rgb", targetSize);
        noirCam_->captureAndSave("noir", targetSize);
    }

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
        
        rgbLabel_ = new QLabel("RGB Camera");
        rgbLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        rgbLabel_->setMinimumSize(1, 1); // Allow shrinking
        rgbLabel_->setAlignment(Qt::AlignCenter);
        rgbLabel_->setStyleSheet("background-color: #222; color: white;");

        noirLabel_ = new QLabel("NoIR Camera");
        noirLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        noirLabel_->setMinimumSize(1, 1);
        noirLabel_->setAlignment(Qt::AlignCenter);
        noirLabel_->setStyleSheet("background-color: #222; color: white;");

        QHBoxLayout *hbox = new QHBoxLayout();
        hbox->addWidget(rgbLabel_);
        hbox->addWidget(noirLabel_);
        hbox->setStretch(0, 1);
        hbox->setStretch(1, 1);

        QVBoxLayout *vbox = new QVBoxLayout();
        vbox->addLayout(hbox);
        
        QPushButton *btn = new QPushButton("Capture");
        connect(btn, &QPushButton::clicked, this, &DualCameraView::captureRequested);
        vbox->addWidget(btn);

        setLayout(vbox);
    }

    // Public accessor for Controller to read current label dimensions
    QSize getLabelSize() const {
        return rgbLabel_->size();
    }

public slots:
    void updateRgb(const QImage &img) {
        if (!img.isNull())
            rgbLabel_->setPixmap(QPixmap::fromImage(img).scaled(rgbLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    void updateNoir(const QImage &img) {
         if (!img.isNull())
            noirLabel_->setPixmap(QPixmap::fromImage(img).scaled(noirLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
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
    DualCameraController(DualCameraModel &model, DualCameraView &view) 
        : model_(model), view_(view) {
        
        connect(&model_, &DualCameraModel::buttonPressed, this, &DualCameraController::handleCapture);
        connect(&view_, &DualCameraView::captureRequested, this, &DualCameraController::handleCapture);
        connect(&model_, &DualCameraModel::imageCaptured, this, &DualCameraController::showInfo);

        timer_ = std::make_unique<QTimer>();
        connect(timer_.get(), &QTimer::timeout, this, &DualCameraController::updateFrames);
        timer_->start(50); 
    }

public slots:
    void handleCapture() {
        // Calculate resolution based on current UI Label size
        QSize labelSize = view_.getLabelSize();
        
        // Define target Width (2K)
        int targetWidth = 2560;
        
        // Calculate Height maintaining the label's Aspect Ratio
        // Height = (LabelHeight / LabelWidth) * TargetWidth
        int targetHeight = 0;
        if (labelSize.width() > 0) {
            double ratio = labelSize.height() * 1 / (double)labelSize.width();
            if(ratio > 0) { ratio = 1 / ratio; }
            targetHeight = static_cast<int>(targetWidth * ratio);
        } else {
            // Fallback if size invalid
            targetHeight = 2048;
        }

        // Ensure dimensions are even (often required by hardware)
        if (targetHeight % 2 != 0) targetHeight++;

        model_.triggerCapture(QSize(targetWidth, targetHeight));
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
        DualCameraController controller(model, view);

        model.start();
        view.showFullScreen();

        ret = app.exec();

        model.stop();
    } 

    cm->stop();

    return ret;
}

#include "main.moc"