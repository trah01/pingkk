#include "settings_dialog.h"
#include "update_check.h"
#include "update_installer.h"
#include "window_style.h"
#include "pingkk/version.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QSettings>
#include <QUrl>
#include <QVersionNumber>
#include <QVariant>

SettingsDialog::SettingsDialog(bool english, const std::function<void()>& install, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(english ? "Settings" : QString::fromUtf8("设置"));
    setMinimumWidth(400);
    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(14);
    // Hidden rows should not leave empty space, so the dialog follows its content height.
    root->setSizeConstraint(QLayout::SetFixedSize);

    QFormLayout* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(12);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto fieldLabel = [](const QString& text) {
        QLabel* label = new QLabel(text);
        label->setObjectName("fieldLabel");
        return label;
    };
    form->addRow(fieldLabel(english ? "Version" : QString::fromUtf8("当前版本")),
                 new QLabel(QString("v%1").arg(PINGKK_VERSION)));

    QComboBox* channel = new QComboBox;
    pingkk::gui::configureComboBox(channel);
    channel->setAccessibleName(english ? "Update channel" : QString::fromUtf8("更新渠道"));
    channel->addItem(english ? "Automatic (GitHub first, then domestic)"
                             : QString::fromUtf8("自动（先连海外，不通时改用国内）"), "auto");
    channel->addItem(english ? "Domestic" : QString::fromUtf8("国内"), "cn");
    channel->addItem(english ? "Overseas (GitHub)" : QString::fromUtf8("海外（GitHub）"), "global");
    const int saved = channel->findData(QSettings().value("update/channel", "auto").toString());
    channel->setCurrentIndex(saved < 0 ? 0 : saved);
    connect(channel, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, [=]() {
        QSettings().setValue("update/channel", channel->currentData().toString());
    });
    form->addRow(fieldLabel(english ? "Update channel" : QString::fromUtf8("更新渠道")), channel);
    root->addLayout(form);

    QPushButton* check = new QPushButton(english ? "Check for updates" : QString::fromUtf8("检查更新"));
    check->setObjectName("primaryButton");
    QPushButton* download = new QPushButton(english ? "Download update" : QString::fromUtf8("下载更新"));
    download->setObjectName("primaryButton");
    download->hide();
    QLabel* status = new QLabel;
    status->setObjectName("mutedLabel");
    status->setWordWrap(true);
    status->setAccessibleName(english ? "Update status" : QString::fromUtf8("更新状态"));
    status->hide();
    QHBoxLayout* actions = new QHBoxLayout;
    actions->setSpacing(12);
    actions->addWidget(check);
    actions->addWidget(download);
    actions->addWidget(status, 1);
    root->addLayout(actions);
    QProgressBar* progress = new QProgressBar;
    progress->setTextVisible(false);
    progress->setAccessibleName(english ? "Update progress" : QString::fromUtf8("更新进度"));
    progress->hide();
    root->addWidget(progress);

    QFrame* separator = new QFrame;
    separator->setObjectName("separator");
    separator->setFrameShape(QFrame::NoFrame);
    root->addWidget(separator);

    QPushButton* installButton = new QPushButton(english ? "Install CLI" : QString::fromUtf8("安装命令行工具"));
    QPushButton* project = new QPushButton(english ? "Project website" : QString::fromUtf8("项目地址"));
    QHBoxLayout* links = new QHBoxLayout;
    links->setSpacing(8);
    links->addWidget(installButton);
    links->addWidget(project);
    links->addStretch(1);
    root->addLayout(links);
    connect(installButton, &QPushButton::clicked, this, [install]() { install(); });
    connect(project, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl(PINGKK_PROJECT_URL));
    });

    UpdateCheck* updater = new UpdateCheck(this);
    UpdateInstaller* installer = new UpdateInstaller(english, this);
    connect(installer, &UpdateInstaller::statusChanged, status, &QLabel::setText);
    connect(installer, &UpdateInstaller::progress, this, [=](qint64 received, qint64 total) {
        if (total > 0) {
            progress->setRange(0, 1000);
            progress->setValue(static_cast<int>(received * 1000 / total));
        }
    });
    connect(installer, &UpdateInstaller::ready, this, [=]() {
        progress->hide();
        download->setProperty("prepared", true);
        download->setText(english ? "Restart and update" : QString::fromUtf8("重启并更新"));
        download->setEnabled(true);
        status->setText(english ? "The update is ready to install."
                                : QString::fromUtf8("更新已准备好，可以重启安装。"));
    });
    connect(installer, &UpdateInstaller::failed, this, [=](const QString& reason) {
        progress->hide();
        download->setProperty("prepared", false);
        download->setText(english ? "Retry update" : QString::fromUtf8("重试更新"));
        download->setEnabled(true);
        status->setText(reason);
    });
    connect(check, &QPushButton::clicked, this, [=]() {
        check->setEnabled(false);
        channel->setEnabled(false);
        status->setText(english ? "Checking..." : QString::fromUtf8("正在检查更新……"));
        status->show();
        const QString selected = channel->currentData().toString();
        updater->start(selected, [=](const QString& tag, const QUrl& downloadUrl) {
            check->setEnabled(true);
            channel->setEnabled(true);
            const QVersionNumber latest = QVersionNumber::fromString(tag);
            if (tag.isEmpty()) {
                status->setText(english ? "Unable to check for updates. Please try again later."
                                        : QString::fromUtf8("检查更新失败，请稍后重试。"));
            } else if (QVersionNumber::compare(latest, QVersionNumber::fromString(PINGKK_VERSION)) > 0) {
                status->setText(english ? QString("Version %1 is available.").arg(tag)
                                        : QString::fromUtf8("发现新版本 %1。").arg(tag));
                download->show();
                channel->setEnabled(false);
                connect(download, &QPushButton::clicked, this, [=]() {
                    download->setEnabled(false);
                    if (download->property("prepared").toBool()) {
                        installer->restart();
                    } else {
                        progress->setRange(0, 0);
                        progress->show();
                        installer->start(tag, downloadUrl, selected == "auto");
                    }
                });
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
