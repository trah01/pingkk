#include "settings_dialog.h"
#include "update_check.h"
#include "pingkk/version.h"

#include <QDesktopServices>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QUrl>
#include <QVersionNumber>

SettingsDialog::SettingsDialog(bool english, const std::function<void()>& install, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(english ? "Settings" : QString::fromUtf8("设置"));
    setMinimumWidth(360);
    QFormLayout* layout = new QFormLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);
    layout->addRow(english ? "Version" : QString::fromUtf8("版本"),
                   new QLabel(QString("v%1").arg(PINGKK_VERSION)));

    QPushButton* check = new QPushButton(english ? "Check for updates" : QString::fromUtf8("检查更新"));
    QLabel* status = new QLabel(QString::fromUtf8("从github或者官方服务器获取"));
    status->setWordWrap(true);
    status->setAccessibleName(english ? "Update status" : QString::fromUtf8("更新状态"));
    layout->addRow(check);
    layout->addRow(status);
    QPushButton* installButton = new QPushButton(english ? "Install CLI" : QString::fromUtf8("安装命令行工具"));
    layout->addRow(installButton);
    connect(installButton, &QPushButton::clicked, this, [install]() { install(); });

    QPushButton* project = new QPushButton(english ? "Project website" : QString::fromUtf8("项目地址"));
    layout->addRow(project);
    connect(project, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl(PINGKK_PROJECT_URL));
    });

    UpdateCheck* updater = new UpdateCheck(this);
    connect(check, &QPushButton::clicked, this, [=]() {
        check->setEnabled(false);
        status->setText(english ? "Checking..." : QString::fromUtf8("正在检查更新……"));
        status->show();
        updater->start([=](const QString& tag, const QUrl& downloadUrl) {
            check->setEnabled(true);
            const QVersionNumber latest = QVersionNumber::fromString(tag);
            if (tag.isEmpty()) {
                status->setText(english ? "Unable to check for updates. Please try again later."
                                        : QString::fromUtf8("检查更新失败，请稍后重试。"));
            } else if (QVersionNumber::compare(latest, QVersionNumber::fromString(PINGKK_VERSION)) > 0) {
                status->setText(english ? QString("Version %1 is available.").arg(tag)
                                        : QString::fromUtf8("发现新版本 %1。").arg(tag));
                QPushButton* download = new QPushButton(english ? "Download update" : QString::fromUtf8("下载更新"));
                layout->insertRow(2, download);
                connect(download, &QPushButton::clicked, this, [=]() { QDesktopServices::openUrl(downloadUrl); });
                check->hide();
            } else if (QVersionNumber::compare(latest, QVersionNumber::fromString(PINGKK_VERSION)) < 0) {
                status->setText(english ? QString("Source version %1 is older; keeping the current version.").arg(tag)
                                        : QString::fromUtf8("下载源版本 %1 较旧，保留当前版本。").arg(tag));
            } else {
                status->setText(english ? "You are up to date." : QString::fromUtf8("当前已是最新版本。"));
            }
        });
    });
}
