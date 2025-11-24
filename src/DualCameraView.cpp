#include "DualCameraView.hpp"
#include "Config.hpp"

#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPixmap>

DualCameraView::DualCameraView(QWidget *parent) : QWidget(parent) {
    setWindowTitle("Dual Camera (RGB + NoIR) - C++");
    
    // Keep raw pointers here because Qt's Layout system
    // takes ownership of child widgets.
    m_rgbLabel = new QLabel("RGB Camera");
    // Updated to 5:4 aspect ratio
    m_rgbLabel->setMinimumSize(Config::Ui::ViewLabelWidth, Config::Ui::ViewLabelHeight);
    m_rgbLabel->setAlignment(Qt::AlignCenter);
    m_rgbLabel->setStyleSheet("background-color: #222; color: white;");

    m_noirLabel = new QLabel("NoIR Camera");
    m_noirLabel->setMinimumSize(Config::Ui::ViewLabelWidth, Config::Ui::ViewLabelHeight);
    m_noirLabel->setAlignment(Qt::AlignCenter);
    m_noirLabel->setStyleSheet("background-color: #222; color: white;");

    QHBoxLayout *hbox = new QHBoxLayout();
    hbox->addWidget(m_rgbLabel);
    hbox->addWidget(m_noirLabel);

    QVBoxLayout *vbox = new QVBoxLayout();
    vbox->addLayout(hbox);
    
    QPushButton *btn = new QPushButton("Capture");
    connect(btn, &QPushButton::clicked, this, &DualCameraView::captureRequested);
    vbox->addWidget(btn);

    setLayout(vbox);
}

void DualCameraView::updateRgb(const QImage &img) {
    if (img.isNull()) {
        return;
    }
    m_rgbLabel->setPixmap(QPixmap::fromImage(img).scaled(m_rgbLabel->size(), Qt::KeepAspectRatio));
}

void DualCameraView::updateNoir(const QImage &img) {
    if (img.isNull()) {
        return;
    }
    m_noirLabel->setPixmap(QPixmap::fromImage(img).scaled(m_noirLabel->size(), Qt::KeepAspectRatio));
}