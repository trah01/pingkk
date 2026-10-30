#include "main_window.h"
#include "pingkk/version.h"

#include <QApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QDir>
#include <QProcess>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>

int main(int argc, char* argv[]) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Qt 5 leaves high-DPI scaling opt-in. Set these before QApplication so
    // Windows renders at the monitor's native resolution instead of bitmap
    // stretching the whole window.
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
#endif
    QApplication application(argc, argv);
    application.setApplicationName("pingkk");
    application.setApplicationVersion(PINGKK_VERSION);
    application.setOrganizationName("pingkk");
#if defined(Q_OS_MACOS)
    application.setWindowIcon(QIcon(":/icons/pingkk-icon-rounded.png"));
#else
    application.setWindowIcon(QIcon(":/icons/pingkk-icon.png"));
#endif

    MainWindow window;
    window.show();
    const int updateResultIndex = application.arguments().indexOf("--update-result");
    if (updateResultIndex >= 0 && updateResultIndex + 1 < application.arguments().size()) {
        const QString resultPath = application.arguments().at(updateResultIndex + 1);
        const QFileInfo resultInfo(resultPath);
        const QString updateRoot = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                                        + "/updates").canonicalPath();
        if (!updateRoot.isEmpty() && resultInfo.fileName() == "result.txt" &&
            resultInfo.canonicalFilePath().startsWith(updateRoot + "/pingkk-update-")) {
            QFile result(resultPath);
            if (result.open(QIODevice::ReadOnly) && result.size() <= 4096) {
                const QStringList lines = QString::fromUtf8(result.readAll()).split('\n');
                result.close();
                QFile::remove(resultPath);
                const bool succeeded = !lines.isEmpty() && lines[0] == "ok";
                const QString log = resultInfo.dir().filePath("apply.log");
                QTimer::singleShot(500, &window, [=, &window]() {
                    const bool english = lines.value(2) == "en";
                    const QString title = english ? "Application update" : QString::fromUtf8("应用更新");
                    if (succeeded) {
                        QMessageBox::information(&window, title,
                            english ? QString("Updated to version %1.").arg(PINGKK_VERSION)
                                    : QString::fromUtf8("已更新到版本 %1。").arg(PINGKK_VERSION));
                    } else {
                        QMessageBox::warning(&window, title,
                            (english ? "The update could not be completed.\n" : QString::fromUtf8("更新未完成。\n"))
                            + lines.value(1) + "\n" + (english ? "Log: " : QString::fromUtf8("日志：")) + log);
                    }
                });
            }
        }
    }
    // Exercise the deployed platform plugin, icon and bundled CLI in CI.
    if (application.arguments().contains("--smoke-test")) {
        if (application.windowIcon().pixmap(32, 32).isNull()) return 10;
#if defined(Q_OS_WIN)
        const QString cli = "pingkk.exe";
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
        const QString cli = "pingkk-cli";
#else
        const QString cli = "pingkk";
#endif
        QProcess process;
        process.start(QDir(application.applicationDirPath()).filePath(cli),
                      QStringList() << "--help");
        if (!process.waitForFinished(10000) ||
            process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            return 11;
        QTimer::singleShot(500, &application, [&]() {
            const int screenshot = application.arguments().indexOf("--screenshot");
            if (screenshot >= 0 && screenshot + 1 < application.arguments().size() &&
                !window.grab().save(application.arguments().at(screenshot + 1))) {
                application.exit(12);
                return;
            }
            application.quit();
        });
    }
    return application.exec();
}
