#include <libcamera/libcamera.h>

#include "DualCameraModel.hpp"
#include "DualCameraView.hpp"
#include "DualCameraController.hpp"

#include <QApplication>
#include <QDebug>
#include <memory>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    std::unique_ptr<libcamera::CameraManager> camManager = std::make_unique<libcamera::CameraManager>();
    if (camManager->start()) {
        qCritical() << "Failed to start camera manager";
        return -1;
    }

    int ret = 0;
    {
        DualCameraModel model(camManager.get());
        DualCameraView view;
        
        // Pass by reference
        DualCameraController controller(model, view);

        model.start();
        view.showMaximized();

        ret = app.exec();

        model.stop();
    } 

    camManager->stop();

    return ret;
}