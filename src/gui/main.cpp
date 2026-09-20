#include "main_window.h"
#include "pingkk/version.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    application.setApplicationName("pingkk");
    application.setApplicationVersion(PINGKK_VERSION);
    application.setOrganizationName("pingkk");
    application.setWindowIcon(QIcon(":/icons/pingkk-icon.png"));

    MainWindow window;
    window.show();
    return application.exec();
}
