#pragma once

#include <QWidget>
#include <QLabel>
#include <QImage>

// ==============================================================
// View: DualCameraView
// ==============================================================
class DualCameraView : public QWidget {
    Q_OBJECT
public:
    explicit DualCameraView(QWidget *parent = nullptr);

public slots:
    void updateRgb(const QImage &img);
    void updateNoir(const QImage &img);

signals:
    void captureRequested();

private:
    QLabel *m_rgbLabel;
    QLabel *m_noirLabel;
};