#include "main_window.h"
#include "pingkk/version.h"

#include <QApplication>
#include <QIcon>
#include <QDir>
#include <QProcess>
#include <QTimer>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    application.setApplicationName("pingkk");
    application.setApplicationVersion(PINGKK_VERSION);
    application.setOrganizationName("pingkk");
    application.setWindowIcon(QIcon(":/icons/pingkk-icon.png"));

    MainWindow window;
    window.show();
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
        QTimer::singleShot(500, &application, SLOT(quit()));
    }
    return application.exec();
}
