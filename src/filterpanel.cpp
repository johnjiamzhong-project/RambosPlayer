#include "filterpanel.h"
#include "ui_filterpanel.h"
#include "playercontroller.h"
#include <QFileDialog>

// 构造函数：setupUi 完成所有控件创建，此处只做信号连接。
// 初始状态滤镜关闭，等待用户勾选启用。
FilterPanel::FilterPanel(PlayerController* player, QWidget* parent)
    : QWidget(parent), ui(new Ui::FilterPanel), player_(player) {
    ui->setupUi(this);

    ui->brightnessSlider->setEnabled(false);
    ui->contrastSlider->setEnabled(false);
    ui->saturationSlider->setEnabled(false);
    ui->watermarkEdit->setEnabled(false);
    ui->browseWatermarkBtn->setEnabled(false);

    connect(ui->enableCheck,        &QCheckBox::stateChanged,   this, &FilterPanel::onEnabledChanged);
    connect(ui->brightnessSlider,   &QSlider::valueChanged,    this, &FilterPanel::onBrightnessChanged);
    connect(ui->contrastSlider,     &QSlider::valueChanged,    this, &FilterPanel::onContrastChanged);
    connect(ui->saturationSlider,   &QSlider::valueChanged,    this, &FilterPanel::onSaturationChanged);
    connect(ui->browseWatermarkBtn, &QPushButton::clicked,     this, &FilterPanel::onBrowseWatermark);
    connect(ui->watermarkEdit,      &QLineEdit::editingFinished, this, [this]{
        player_->setWatermark(ui->watermarkEdit->text());
    });
}

FilterPanel::~FilterPanel() { delete ui; }

// 启用/禁用滤镜：切换所有滑块可用状态，转到播放器。
void FilterPanel::onEnabledChanged(int state) {
    bool on = (state == Qt::Checked);
    ui->brightnessSlider->setEnabled(on);
    ui->contrastSlider->setEnabled(on);
    ui->saturationSlider->setEnabled(on);
    ui->watermarkEdit->setEnabled(on);
    ui->browseWatermarkBtn->setEnabled(on);
    player_->setFilterEnabled(on);
}

// 滑块值 → float（-100..100 → -1.0..1.0），更新标签并转发
void FilterPanel::onBrightnessChanged(int value) {
    float f = value / 100.0f;
    ui->brightnessValue->setText(QString::number(f, 'f', 2));
    player_->setBrightness(f);
}

void FilterPanel::onContrastChanged(int value) {
    float f = value / 100.0f;
    ui->contrastValue->setText(QString::number(f, 'f', 2));
    player_->setContrast(f);
}

// 饱和度：0..300 → 0.0..3.0（默认 100 = 1.0）
void FilterPanel::onSaturationChanged(int value) {
    float f = value / 100.0f;
    ui->saturationValue->setText(QString::number(f, 'f', 2));
    player_->setSaturation(f);
}

// 浏览水印图片文件
void FilterPanel::onBrowseWatermark() {
    QString path = QFileDialog::getOpenFileName(
        this, "选择水印图片", QString(),
        "图片文件 (*.png *.jpg *.jpeg *.bmp *.gif);;所有文件 (*)");
    if (!path.isEmpty()) {
        ui->watermarkEdit->setText(path);
        player_->setWatermark(path);
    }
}
