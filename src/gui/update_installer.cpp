#include "update_installer.h"
#include "update_transfer.h"
#include "pingkk/version.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QVersionNumber>

UpdateInstaller::UpdateInstaller(bool english, QObject* parent)
    : QObject(parent), english_(english) {}

UpdateInstaller::~UpdateInstaller() {
    delete transfer_;
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(3000);
    }
}

QString UpdateInstaller::message(const char* english, const char* chinese) const {
    return QString::fromUtf8(english_ ? english : chinese);
}

void UpdateInstaller::fail(const QString& reason) {
    ready_ = false;
    emit failed(reason);
}

bool UpdateInstaller::selectPackage() {
    const QString directory = QCoreApplication::applicationDirPath();
    const QString architecture = QSysInfo::buildCpuArchitecture();
#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    QDir root(directory);
    root.cdUp();
    root.cdUp();
    target_ = root.absolutePath();
    if (!target_.endsWith(".app") || !QFileInfo::exists(root.filePath("Contents/Info.plist"))) return false;
    kind_ = "macos";
    asset_ = architecture == "arm64" ? "pingkk-macos-arm64-gui.zip" : "pingkk-macos-intel-gui.zip";
#elif defined(Q_OS_WIN)
    if (architecture != "x86_64" && architecture != "arm64") return false;
    const QString portable = qEnvironmentVariable("PINGKK_PORTABLE_EXECUTABLE");
    const QString prefix = architecture == "arm64" ? "pingkk-windows-arm64"
                          : QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
                              ? "pingkk-windows-x64-legacy" : "pingkk-windows-x64";
    if (!portable.isEmpty() && QFileInfo(portable).isFile()) {
        kind_ = "windows-file";
        target_ = QFileInfo(portable).absoluteFilePath();
        asset_ = prefix + "-portable.exe";
    } else {
        target_ = directory;
        kind_ = QFileInfo::exists(QDir(directory).filePath("unins000.exe"))
                    ? "windows-installer" : "windows-directory";
        if (kind_ == "windows-directory" &&
            (!QFileInfo::exists(QDir(directory).filePath("qt.conf")) ||
             !QFileInfo::exists(QDir(directory).filePath("platforms/qwindows.dll")))) return false;
        asset_ = prefix + (kind_ == "windows-installer" ? "-setup.exe" : "-portable.exe");
    }
#else
    QDir root(directory);
    root.cdUp();
    target_ = root.absolutePath();
    QString cpu;
    if (architecture == "x86_64") cpu = "x64";
    else if (architecture == "arm64") cpu = "arm64";
    else if (architecture == "i386" || architecture == "i686") cpu = "x86";
    else return false;
    if (target_ == "/usr/lib/pingkk" && QFileInfo::exists("/var/lib/dpkg/info/pingkk.list")) {
        kind_ = "linux-deb";
        asset_ = "pingkk-linux-" + cpu + "-installer.deb";
        if (QStandardPaths::findExecutable("pkexec").isEmpty() ||
            QStandardPaths::findExecutable("apt-get").isEmpty()) return false;
    } else if (QFileInfo::exists(root.filePath("run-pingkk.sh"))) {
        kind_ = "linux-directory";
        asset_ = "pingkk-linux-" + cpu + "-portable.tar.xz";
    } else return false;
#endif
    return true;
}

void UpdateInstaller::start(const QString& version, const QUrl& source) {
    if (transfer_ || process_.state() != QProcess::NotRunning) return;
    ready_ = false;
    static const QRegularExpression valid("^[0-9]+\\.[0-9]+\\.[0-9]+$");
    if (!valid.match(version).hasMatch() ||
        QVersionNumber::compare(QVersionNumber::fromString(version),
                               QVersionNumber::fromString(PINGKK_VERSION)) <= 0) {
        fail(message("No newer version is available.", "没有可安装的新版本。"));
        return;
    }
    version_ = version;
    if (!selectPackage()) {
        fail(message("This installation cannot update itself. Install an official release package first; Linux DEB updates also need pkexec and apt-get.",
                     "当前安装方式无法应用内更新。请先安装官方发行包；Linux DEB 更新还需要 pkexec 和 apt-get。"));
        return;
    }
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/updates";
    QDir().mkpath(cache);
    work_.reset(new QTemporaryDir(cache + "/pingkk-update-XXXXXX"));
    if (!work_->isValid()) {
        fail(message("Unable to create the update folder. Check disk space and permissions.",
                     "无法创建更新目录，请检查磁盘空间和目录权限。"));
        return;
    }
    package_ = work_->filePath(asset_);
    extracted_ = work_->filePath("new");
    download(source.host() == "github.com");
}

void UpdateInstaller::download(bool github) {
    const QUrl base(github ? QString(PINGKK_PROJECT_URL "/releases/download/v%1/").arg(version_)
                           : QString("https://he.sb/pingkk/v%1/").arg(version_));
    emit statusChanged(message("Downloading checksums...", "正在获取更新校验信息……"));
    transfer_ = new UpdateTransfer(base.resolved(QUrl("SHA256SUMS.txt")), QString(), false, 15000, this);
    UpdateTransfer* transfer = transfer_;
    connect(transfer, &QThread::finished, this, [=]() {
        transfer_ = NULL;
        transfer->deleteLater();
        if (!transfer->error.isEmpty()) {
            if (github) { download(false); return; }
            fail(message("Unable to download update information: ", "无法获取更新信息：") + transfer->error);
            return;
        }
        QByteArray expected;
        for (const QByteArray& line : transfer->data.split('\n')) {
            const QList<QByteArray> fields = line.simplified().split(' ');
            if (fields.size() == 2 && fields[1] == asset_.toUtf8()) {
                if (!expected.isEmpty()) { expected.clear(); break; }
                expected = fields[0].toLower();
            }
        }
        static const QRegularExpression digest("^[0-9a-f]{64}$");
        if (!digest.match(QString::fromLatin1(expected)).hasMatch()) {
            fail(message("The update package has no valid checksum.", "更新包缺少有效的校验信息。"));
            return;
        }
        downloadPackage(base, github, expected);
    });
    transfer->start();
}

void UpdateInstaller::downloadPackage(const QUrl& base, bool github, const QByteArray& checksum) {
    emit statusChanged(message("Downloading update...", "正在下载更新……"));
    transfer_ = new UpdateTransfer(base.resolved(QUrl(asset_)), package_, false, 1800000, this);
    UpdateTransfer* transfer = transfer_;
    connect(transfer, &UpdateTransfer::progress, this, &UpdateInstaller::progress);
    connect(transfer, &QThread::finished, this, [=]() {
        transfer_ = NULL;
        transfer->deleteLater();
        if (!transfer->error.isEmpty()) {
            if (github) { download(false); return; }
            fail(message("Download failed: ", "下载失败：") + transfer->error);
            return;
        }
        QFile package(package_);
        QCryptographicHash digest(QCryptographicHash::Sha256);
        if (!package.open(QIODevice::ReadOnly) || !digest.addData(&package) ||
            digest.result().toHex() != checksum) {
            fail(message("Update verification failed. The current app has not been changed.",
                         "更新包校验失败，当前应用未改动。"));
            return;
        }
        prepare();
    });
    transfer->start();
}

void UpdateInstaller::prepare() {
    emit statusChanged(message("Preparing update...", "正在准备更新……"));
#ifdef Q_OS_WIN
    const QString name = "update_windows.ps1";
#else
    const QString name = "update_unix.sh";
#endif
    QFile resource(":/updater/" + name);
    script_ = work_->filePath(name);
    QFile script(script_);
    if (!resource.open(QIODevice::ReadOnly) || !script.open(QIODevice::WriteOnly)) {
        fail(message("Unable to prepare the update helper.", "无法准备更新程序。"));
        return;
    }
    QByteArray contents = resource.readAll();
#ifdef Q_OS_WIN
    contents.prepend("\xef\xbb\xbf");
#endif
    if (script.write(contents) != contents.size() || !script.flush()) {
        fail(message("Unable to write the update helper.", "无法写入更新程序。"));
        return;
    }
    script.close();
#ifdef Q_OS_WIN
    if (kind_ == "windows-installer") {
        runPreparation("powershell.exe", QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass"
                       << "-File" << script_ << "-Prepare" << "-Package" << package_
                       << "-ExpectedVersion" << version_, true);
    } else {
        runPreparation(package_, QStringList() << "--extract" << extracted_, false);
    }
#else
    QDir().mkpath(extracted_);
    if (kind_ == "macos") {
        runPreparation("/usr/bin/ditto", QStringList() << "-x" << "-k" << package_ << extracted_, false);
    } else if (kind_ == "linux-deb") {
        runPreparation("dpkg-deb", QStringList() << "--extract" << package_ << extracted_, false);
    } else {
        runPreparation("tar", QStringList() << "-xJf" << package_ << "-C" << extracted_, false);
    }
#endif
}

void UpdateInstaller::runPreparation(const QString& command, const QStringList& arguments, bool verify) {
    process_.disconnect(this);
    process_.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (verify && kind_ == "linux-directory") {
        environment.insert("LD_LIBRARY_PATH", extracted_ + "/lib:" + environment.value("LD_LIBRARY_PATH"));
    }
    process_.setProcessEnvironment(environment);
    QTimer* timer = new QTimer(&process_);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, &process_, &QProcess::kill);
    connect(&process_, &QProcess::errorOccurred, this, [=](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            timer->stop();
            timer->deleteLater();
            fail(message("Unable to start the update helper: ", "无法启动更新程序：") + process_.errorString());
        }
    });
    connect(&process_, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [=](int code, QProcess::ExitStatus exitStatus) {
        timer->stop();
        timer->deleteLater();
        const QString output = QString::fromUtf8(process_.readAll()).trimmed();
        if (code != 0 || exitStatus != QProcess::NormalExit) {
            fail(message("Unable to prepare the update. The current app has not been changed. ",
                         "更新准备失败，当前应用未改动。 ") + output.left(1000));
            return;
        }
        if (verify) {
            if (kind_ != "windows-installer" && output != version_) {
                fail(message("The package version does not match the update version.", "更新包内的程序版本不匹配。"));
                return;
            }
            ready_ = true;
            emit ready();
            return;
        }
        QString cli;
        QString gui;
        if (kind_ == "macos") {
            cli = extracted_ + "/pingkk.app/Contents/MacOS/pingkk-cli";
            gui = extracted_ + "/pingkk.app/Contents/MacOS/pingkk";
        } else if (kind_.startsWith("windows")) {
            cli = extracted_ + "/pingkk.exe";
            gui = extracted_ + "/pingkk-gui.exe";
        } else if (kind_ == "linux-deb") {
            cli = extracted_ + "/usr/bin/pingkk";
            gui = extracted_ + "/usr/lib/pingkk/bin/pingkk-gui";
        } else {
            cli = extracted_ + "/bin/pingkk";
            gui = extracted_ + "/bin/pingkk-gui";
        }
        if (!QFileInfo(cli).isExecutable() || !QFileInfo(gui).isExecutable()) {
            fail(message("The update package is incomplete.", "更新包不完整。"));
            return;
        }
        runPreparation(cli, QStringList() << "--version", true);
    });
    process_.start(command, arguments);
    timer->start(120000);
}

bool UpdateInstaller::restart() {
    if (!ready_) return false;
    const QString parent = QString::number(QCoreApplication::applicationPid());
    const QString language = english_ ? "en" : "zh";
#ifdef Q_OS_WIN
    const QString powershell = qEnvironmentVariable("SystemRoot") + "/System32/WindowsPowerShell/v1.0/powershell.exe";
    const QStringList args = QStringList() << "-NoProfile" << "-WindowStyle" << "Hidden" << "-ExecutionPolicy" << "Bypass"
        << "-File" << script_ << "-ParentPid" << parent << "-Kind" << kind_
        << "-Target" << target_ << "-Package" << package_ << "-Extracted" << extracted_
        << "-ExpectedVersion" << version_ << "-Language" << language
        << "-LauncherPid" << qEnvironmentVariable("PINGKK_PORTABLE_PID", "0");
    const bool started = QProcess::startDetached(powershell, args, work_->path());
#else
    const QStringList args = QStringList() << script_ << parent << kind_ << target_
        << package_ << extracted_ << version_ << language;
    const bool started = QProcess::startDetached("/bin/bash", args, work_->path());
#endif
    if (!started) {
        fail(message("Unable to start the update helper. The current app is still running.",
                     "无法启动更新程序，当前应用继续运行。"));
        return false;
    }
    work_->setAutoRemove(false);
    QCoreApplication::quit();
    return true;
}
