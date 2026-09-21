#include "main_window.h"
#include "pingkk/core.h"
#include "pingkk/version.h"

#include <QApplication>
#include <QAbstractSpinBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QImage>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTextCursor>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

class FixedPopupComboBox : public QComboBox {
public:
    explicit FixedPopupComboBox(QWidget* parent = 0) : QComboBox(parent) {}

protected:
    void paintEvent(QPaintEvent* event) {
        QComboBox::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(isEnabled() ? QColor("#4f5b67") : QColor("#9aa1aa"));
        pen.setWidthF(1.8);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        const int inset = objectName() == "languageCombo" ? 13 : 19;
        const qreal centerX = width() - inset;
        const qreal centerY = height() / 2.0;
        QPolygonF arrow;
        arrow << QPointF(centerX - 5, centerY - 2)
              << QPointF(centerX, centerY + 3)
              << QPointF(centerX + 5, centerY - 2);
        painter.drawPolyline(arrow);
    }

    void showPopup() {
        QComboBox::showPopup();
        QWidget* popup = view()->window();
        popup->setFixedWidth(width());
        popup->move(mapToGlobal(QPoint(0, height() + 4)));
    }
};

class UnitSpinBox : public QSpinBox {
public:
    explicit UnitSpinBox(const QString& unit, QWidget* parent = 0)
        : QSpinBox(parent), unitLabel_(new QLabel(unit, this)) {
        unitLabel_->setObjectName("inputUnit");
        unitLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        unitLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
        lineEdit()->setTextMargins(0, 0, 32, 0);
    }

protected:
    void resizeEvent(QResizeEvent* event) {
        QSpinBox::resizeEvent(event);
        unitLabel_->setGeometry(width() - 36, 0, 28, height());
        unitLabel_->raise();
    }

private:
    QLabel* unitLabel_;
};

void configureComboBox(QComboBox* comboBox) {
    QListView* view = new QListView(comboBox);
    view->setObjectName("comboPopup");
    view->setFrameShape(QFrame::NoFrame);
    view->setSpacing(2);
    view->setUniformItemSizes(true);
    view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    comboBox->setView(view);
}

bool normalizePorts(const QString& input, QString& normalized) {
    QString value = input.trimmed();
    value.replace(QString::fromUtf8("，"), ",");
    const QStringList items = value.split(',');
    QStringList ports;
    for (int index = 0; index < items.size(); ++index) {
        const QString item = items.at(index).trimmed();
        bool validNumber = false;
        const int port = item.toInt(&validNumber);
        if (!validNumber || port < 1 || port > 65535) return false;
        ports << QString::number(port);
    }
    if (ports.isEmpty()) return false;
    normalized = ports.join(",");
    return true;
}

#if !defined(Q_OS_MACOS) && !defined(Q_OS_MAC)
bool copyExecutable(const QString& source,
                    const QString& destination,
                    QString& error) {
    const QString temporary = destination + ".tmp";
    QFile::remove(temporary);
    if (!QFile::copy(source, temporary)) {
        error = QString::fromUtf8("无法复制命令行程序");
        return false;
    }
    QFile::setPermissions(temporary, QFile::permissions(temporary) |
                                         QFileDevice::ExeOwner |
                                         QFileDevice::ExeGroup |
                                         QFileDevice::ExeOther);
    if (QFile::exists(destination) && !QFile::remove(destination)) {
        QFile::remove(temporary);
        error = QString::fromUtf8("无法替换已有的命令行程序");
        return false;
    }
    if (!QFile::rename(temporary, destination)) {
        QFile::remove(temporary);
        error = QString::fromUtf8("无法完成命令行程序安装");
        return false;
    }
    return true;
}
#endif

#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
QString shellQuote(QString value) {
    value.replace("'", "'\\''");
    return "'" + value + "'";
}

QString appleScriptQuote(QString value) {
    value.replace("\\", "\\\\");
    value.replace("\"", "\\\"");
    return "\"" + value + "\"";
}
#endif

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      english_(false),
      stopRequested_(false),
      process_(new QProcess(this)) {
    buildInterface();
    const QString localAddress = QString::fromStdString(pingkk::primaryLocalAddress());
    currentIpValue_->setText(localAddress == QString::fromUtf8("未知") ? "—" : localAddress);
    updateTexts();

    process_->setProcessChannelMode(QProcess::MergedChannels);
    connect(process_, SIGNAL(readyReadStandardOutput()), this, SLOT(readProcessOutput()));
    connect(process_, SIGNAL(finished(int,QProcess::ExitStatus)), this, SLOT(processFinished(int)));
}

void MainWindow::buildInterface() {
    setMinimumSize(700, 480);
    resize(800, 550);

    QWidget* central = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(central);
    root->setContentsMargins(22, 10, 22, 6);
    root->setSpacing(14);

    QHBoxLayout* topBar = new QHBoxLayout;
    currentIpTitle_ = new QLabel;
    currentIpTitle_->setObjectName("mutedLabel");
    currentIpValue_ = new QLabel("—");
    currentIpValue_->setObjectName("addressLabel");
    QFrame* currentIpPanel = new QFrame;
    currentIpPanel->setObjectName("currentIpPanel");
    currentIpPanel->setFixedHeight(34);
    QHBoxLayout* currentIpLayout = new QHBoxLayout(currentIpPanel);
    currentIpLayout->setContentsMargins(12, 0, 12, 0);
    currentIpLayout->setSpacing(10);
    currentIpLayout->addWidget(currentIpTitle_);
    currentIpLayout->addWidget(currentIpValue_);
    languageCombo_ = new FixedPopupComboBox;
    languageCombo_->setObjectName("languageCombo");
    languageCombo_->addItem(QString::fromUtf8("中文"));
    languageCombo_->addItem("English");
    languageCombo_->setFixedWidth(102);
    configureComboBox(languageCombo_);
    installButton_ = new QPushButton;
    installButton_->setObjectName("quietButton");
    topBar->addWidget(currentIpPanel);
    topBar->addStretch();
    topBar->addWidget(installButton_);
    topBar->addWidget(languageCombo_);

    QFrame* controlPanel = new QFrame;
    controlPanel->setObjectName("controlPanel");
    QGridLayout* controls = new QGridLayout(controlPanel);
    controls->setContentsMargins(18, 16, 18, 16);
    controls->setHorizontalSpacing(12);
    controls->setVerticalSpacing(12);

    targetTitle_ = new QLabel;
    targetEdit_ = new QLineEdit;
    targetEdit_->setClearButtonEnabled(true);
    portTitle_ = new QLabel;
    portEdit_ = new QLineEdit("80");
    portEdit_->setClearButtonEnabled(true);
    portEdit_->setFixedWidth(170);
    protocolTitle_ = new QLabel;
    protocolCombo_ = new FixedPopupComboBox;
    protocolCombo_->addItems(QStringList()
                             << "TCP" << "UDP" << "TCP + UDP" << "Ping"
                             << QString::fromUtf8("路由追踪"));
    protocolCombo_->setFixedWidth(160);
    configureComboBox(protocolCombo_);
    timeoutTitle_ = new QLabel;
    timeoutSpin_ = new UnitSpinBox("ms");
    timeoutSpin_->setRange(100, 60000);
    timeoutSpin_->setValue(1000);
    timeoutSpin_->setFixedWidth(125);
    timeoutSpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    singleButton_ = new QPushButton;
    singleButton_->setObjectName("primaryButton");
    continuousButton_ = new QPushButton;
    stopButton_ = new QPushButton;
    stopButton_->setObjectName("dangerButton");
    stopButton_->setVisible(false);

    controls->addWidget(targetTitle_, 0, 0);
    controls->addWidget(targetEdit_, 0, 1, 1, 3);
    controls->addWidget(portTitle_, 0, 4);
    controls->addWidget(portEdit_, 0, 5);
    controls->addWidget(protocolTitle_, 1, 0);
    controls->addWidget(protocolCombo_, 1, 1);
    controls->addWidget(timeoutTitle_, 1, 2);
    controls->addWidget(timeoutSpin_, 1, 3);
    controls->addWidget(singleButton_, 1, 4);
    controls->addWidget(continuousButton_, 1, 5);
    controls->addWidget(stopButton_, 1, 5);
    QVBoxLayout* inputArea = new QVBoxLayout;
    inputArea->setContentsMargins(0, 0, 0, 0);
    inputArea->setSpacing(8);
    inputArea->addLayout(topBar);
    inputArea->addWidget(controlPanel);
    root->addLayout(inputArea);

    QHBoxLayout* outputHeader = new QHBoxLayout;
    outputTitle_ = new QLabel;
    outputTitle_->setObjectName("sectionTitle");
    exportButton_ = new QPushButton;
    exportButton_->setObjectName("textButton");
    clearButton_ = new QPushButton;
    clearButton_->setObjectName("textButton");
    outputHeader->addWidget(outputTitle_);
    outputHeader->addStretch();
    outputHeader->addWidget(exportButton_);
    outputHeader->addWidget(clearButton_);
    root->addLayout(outputHeader);

    outputEdit_ = new QPlainTextEdit;
    outputEdit_->setReadOnly(true);
    outputEdit_->setObjectName("outputEdit");
    outputEdit_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    QLabel* projectLink = new QLabel(
        QString("v%2 · <a href=\"%1\">项目地址</a>")
            .arg(PINGKK_PROJECT_URL)
            .arg(PINGKK_VERSION));
    projectLink->setOpenExternalLinks(true);
    projectLink->setTextInteractionFlags(Qt::TextBrowserInteraction);
    projectLink->setStyleSheet("font-size: 13px; color: #68717d;");
    QHBoxLayout* footerBar = new QHBoxLayout;
    footerBar->setContentsMargins(0, 0, 0, 0);
    footerBar->addWidget(projectLink);
    footerBar->addStretch();
    QVBoxLayout* outputArea = new QVBoxLayout;
    outputArea->setContentsMargins(0, 0, 0, 0);
    outputArea->setSpacing(4);
    outputArea->addWidget(outputEdit_, 1);
    outputArea->addLayout(footerBar);
    root->addLayout(outputArea, 1);
    setCentralWidget(central);

    setStyleSheet(
        "QMainWindow, QWidget { background: #ffffff; color: #18202a; "
        "font-family: 'PingFang SC', 'Microsoft YaHei UI', sans-serif; "
        "font-size: 14px; }"
        "QLabel { background: transparent; }"
        "QFrame#controlPanel { background: #f8f9fb; border: 1px solid #dfe3e8; border-radius: 12px; }"
        "QFrame#currentIpPanel { background: #ffffff; border: 1px solid #cbd1d8; border-radius: 7px; }"
        "QLabel#mutedLabel { color: #68717d; }"
        "QLabel#inputUnit { color: #68717d; background: transparent; font-size: 14px; }"
        "QLabel#addressLabel, QLabel#sectionTitle { font-weight: 600; color: #18202a; }"
        "QLineEdit, QSpinBox, QComboBox { min-height: 36px; padding: 0 6px; "
        "font-size: 15px; background: #ffffff; border: 1px solid #cbd1d8; border-radius: 7px; }"
        "QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border: 2px solid #1769aa; }"
        "QComboBox { padding-right: 36px; }"
        "QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right; "
        "width: 32px; border: 0; border-left: 1px solid #e1e5e9; background: transparent; }"
        "QComboBox::down-arrow { image: none; }"
        "QComboBox#languageCombo { min-height: 32px; padding: 0 28px 0 8px; }"
        "QComboBox#languageCombo::drop-down { width: 26px; }"
        "QListView#comboPopup { background: #ffffff; border: 1px solid #cbd1d8; "
        "border-radius: 8px; padding: 5px; outline: 0; color: #18202a; }"
        "QListView#comboPopup::item { min-height: 38px; padding: 0 8px; border-radius: 5px; }"
        "QListView#comboPopup::item:hover { background: #f1f5f8; }"
        "QListView#comboPopup::item:selected { background: #e7f1f8; color: #124d78; }"
        "QPushButton { min-height: 36px; padding: 0 14px; border: 1px solid #cbd1d8; "
        "border-radius: 7px; background: #ffffff; }"
        "QPushButton:hover { background: #f1f3f5; }"
        "QPushButton:pressed { background: #e6e9ed; }"
        "QPushButton:disabled { color: #9aa1aa; background: #f1f2f4; }"
        "QPushButton#primaryButton { color: #ffffff; background: #1769aa; border-color: #1769aa; font-weight: 600; }"
        "QPushButton#primaryButton:hover { background: #13598f; }"
        "QPushButton#dangerButton { color: #a42b2b; border-color: #d8a7a7; }"
        "QPushButton#quietButton { min-height: 32px; color: #394451; background: transparent; }"
        "QPushButton#textButton { border: 0; color: #1769aa; padding: 0 8px; }"
        "QPlainTextEdit#outputEdit { background: #fbfcfd; border: 1px solid #dfe3e8; "
        "border-radius: 10px; padding: 12px; "
        "font-size: 14px; line-height: 1.5; selection-background-color: #cfe5f7; }"
        "QPlainTextEdit#outputEdit QScrollBar:vertical { border: 0; background: #f1f3f5; "
        "width: 10px; margin: 7px 2px 7px 0; border-radius: 5px; }"
        "QPlainTextEdit#outputEdit QScrollBar::handle:vertical { background: #bcc4cd; "
        "min-height: 36px; border-radius: 4px; }"
        "QPlainTextEdit#outputEdit QScrollBar::handle:vertical:hover { background: #929eaa; }"
        "QPlainTextEdit#outputEdit QScrollBar::add-line:vertical, "
        "QPlainTextEdit#outputEdit QScrollBar::sub-line:vertical { height: 0; border: 0; }"
        "QPlainTextEdit#outputEdit QScrollBar::add-page:vertical, "
        "QPlainTextEdit#outputEdit QScrollBar::sub-page:vertical { background: transparent; }"
        "QPlainTextEdit#outputEdit QScrollBar:horizontal { border: 0; background: #f1f3f5; "
        "height: 10px; margin: 0 7px 2px 7px; border-radius: 5px; }"
        "QPlainTextEdit#outputEdit QScrollBar::handle:horizontal { background: #bcc4cd; "
        "min-width: 36px; border-radius: 4px; }"
        "QPlainTextEdit#outputEdit QScrollBar::handle:horizontal:hover { background: #929eaa; }"
        "QPlainTextEdit#outputEdit QScrollBar::add-line:horizontal, "
        "QPlainTextEdit#outputEdit QScrollBar::sub-line:horizontal { width: 0; border: 0; }"
        "QPlainTextEdit#outputEdit QScrollBar::add-page:horizontal, "
        "QPlainTextEdit#outputEdit QScrollBar::sub-page:horizontal { background: transparent; }"
    );

    connect(singleButton_, SIGNAL(clicked()), this, SLOT(startSingleTest()));
    connect(continuousButton_, SIGNAL(clicked()), this, SLOT(startContinuousTest()));
    connect(stopButton_, SIGNAL(clicked()), this, SLOT(stopTest()));
    connect(languageCombo_, SIGNAL(currentIndexChanged(int)), this, SLOT(changeLanguage(int)));
    connect(protocolCombo_, SIGNAL(currentIndexChanged(int)), this, SLOT(updateProtocolFields(int)));
    connect(installButton_, SIGNAL(clicked()), this, SLOT(installCommandLineTool()));
    connect(exportButton_, SIGNAL(clicked()), this, SLOT(copyReportImage()));
    connect(clearButton_, SIGNAL(clicked()), this, SLOT(clearOutput()));
    connect(targetEdit_, SIGNAL(returnPressed()), this, SLOT(startSingleTest()));
    connect(portEdit_, SIGNAL(returnPressed()), this, SLOT(startSingleTest()));
}

void MainWindow::updateTexts() {
    setWindowTitle(english_ ? "pingkk - Network Test" : QString::fromUtf8("ping看看 - 网络测试"));
    currentIpTitle_->setText(english_ ? "Local IP" : QString::fromUtf8("当前 IP"));
    targetTitle_->setText(english_ ? "Target" : QString::fromUtf8("目标地址"));
    targetEdit_->setPlaceholderText(english_ ? "IP, domain, or URL" : QString::fromUtf8("IP、域名或完整网址"));
    portTitle_->setText(english_ ? "Ports" : QString::fromUtf8("目标端口"));
    portEdit_->setPlaceholderText(english_ ? "80, 443, 8080"
                                           : QString::fromUtf8("多个端口用逗号分隔"));
    portEdit_->setToolTip(english_ ? "Separate multiple ports with commas."
                                   : QString::fromUtf8("多个端口使用逗号分隔，例如 80, 443, 8080。"));
    protocolTitle_->setText(english_ ? "Function" : QString::fromUtf8("功能选择"));
    timeoutTitle_->setText(english_ ? "Timeout" : QString::fromUtf8("超时时间"));
    protocolCombo_->setItemText(4, english_ ? "Route trace" : QString::fromUtf8("路由追踪"));
    singleButton_->setText(english_ ? "Test once" : QString::fromUtf8("单次测试"));
    continuousButton_->setText(english_ ? "Continuous" : QString::fromUtf8("持续测试"));
    stopButton_->setText(english_ ? "Stop" : QString::fromUtf8("停止测试"));
    installButton_->setText(english_ ? "Install CLI" : QString::fromUtf8("安装命令行工具"));
    outputTitle_->setText(english_ ? "Output" : QString::fromUtf8("日志输出"));
    exportButton_->setText(english_ ? "Copy report" : QString::fromUtf8("一键导出"));
    clearButton_->setText(english_ ? "Clear" : QString::fromUtf8("清空"));
}

QString MainWindow::commandPath() const {
#ifdef Q_OS_WIN
    return QDir(QCoreApplication::applicationDirPath()).filePath("pingkk.exe");
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    return QDir(QCoreApplication::applicationDirPath()).filePath("pingkk-cli");
#else
    return QDir(QCoreApplication::applicationDirPath()).filePath("pingkk");
#endif
}

QString MainWindow::protocolArgument() const {
    if (protocolCombo_->currentIndex() == 1) return "udp";
    if (protocolCombo_->currentIndex() == 2) return "all";
    return "tcp";
}

void MainWindow::startSingleTest() {
    startTest(false);
}

void MainWindow::startContinuousTest() {
    startTest(true);
}

void MainWindow::startTest(bool continuous) {
    if (process_->state() != QProcess::NotRunning) return;
    const QString target = targetEdit_->text().trimmed();
    if (target.isEmpty()) {
        showInputError(english_ ? "Enter a target address." : QString::fromUtf8("请输入目标地址。"));
        return;
    }
    QString ports;
    if (protocolCombo_->currentIndex() < 3 && !normalizePorts(portEdit_->text(), ports)) {
        showInputError(english_ ? "Enter ports from 1 to 65535, separated with commas."
                                : QString::fromUtf8("请输入 1 到 65535 之间的端口，多个端口用逗号分隔。"));
        portEdit_->setFocus();
        portEdit_->selectAll();
        return;
    }
    const QString executable = commandPath();
    if (!QFileInfo(executable).isExecutable()) {
        showInputError(english_ ? "The pingkk command was not found beside the app."
                                : QString::fromUtf8("未在应用旁找到 pingkk 命令行程序。"));
        return;
    }

    QStringList arguments;
    if (protocolCombo_->currentIndex() == 4) {
        arguments << "-r" << target;
    } else {
        if (continuous) arguments << "-t";
        arguments << target;
    }
    if (protocolCombo_->currentIndex() < 3) {
        portEdit_->setText(ports);
        arguments << ports << protocolArgument();
    }
    arguments << "--timeout" << QString::number(timeoutSpin_->value());
    outputEdit_->appendPlainText(english_ ? "Starting test..." : QString::fromUtf8("开始测试……"));
    stopRequested_ = false;
    pendingOutputLine_.clear();
    setRunning(true);
    process_->start(executable, arguments);
    if (!process_->waitForStarted(3000)) {
        setRunning(false);
        outputEdit_->appendPlainText(
            english_ ? "The test process could not be started."
                     : QString::fromUtf8("无法启动测试进程，请重新打开应用后再试。"));
    }
}

void MainWindow::stopTest() {
    if (process_->state() == QProcess::NotRunning) return;
    stopRequested_ = true;
    process_->terminate();
    if (!process_->waitForFinished(800)) process_->kill();
}

void MainWindow::readProcessOutput() {
    const QString text = QString::fromUtf8(process_->readAllStandardOutput());
    if (text.isEmpty()) return;
    outputEdit_->moveCursor(QTextCursor::End);
    outputEdit_->insertPlainText(text);
    outputEdit_->moveCursor(QTextCursor::End);

    pendingOutputLine_.append(text);
    const QString marker = QString::fromUtf8("当前 IP：");
    int lineEnd = -1;
    while ((lineEnd = pendingOutputLine_.indexOf('\n')) >= 0) {
        const QString line = pendingOutputLine_.left(lineEnd).trimmed();
        pendingOutputLine_.remove(0, lineEnd + 1);
        if (!line.startsWith(marker)) continue;

        const QString address = line.mid(marker.size()).trimmed();
        if (!address.isEmpty() && address != QString::fromUtf8("未知")) {
            currentIpValue_->setText(address);
        }
    }
}

void MainWindow::processFinished(int exitCode) {
    setRunning(false);
    if (!stopRequested_ && exitCode >= 2) {
        outputEdit_->appendPlainText(english_ ? "Test ended with an error."
                                              : QString::fromUtf8("测试异常结束，请检查上方信息。"));
    }
    stopRequested_ = false;
}

void MainWindow::changeLanguage(int index) {
    english_ = index == 1;
    updateTexts();
}

void MainWindow::updateProtocolFields(int index) {
    const bool needsPort = index < 3;
    const bool route = index == 4;
    const bool idle = singleButton_->isEnabled();
    portTitle_->setEnabled(needsPort);
    portEdit_->setEnabled(needsPort && idle);
    continuousButton_->setEnabled(!route && idle);
}

void MainWindow::setRunning(bool running) {
    targetEdit_->setEnabled(!running);
    portEdit_->setEnabled(!running);
    timeoutSpin_->setEnabled(!running);
    protocolCombo_->setEnabled(!running);
    singleButton_->setEnabled(!running);
    continuousButton_->setVisible(!running);
    stopButton_->setVisible(running);
    updateProtocolFields(protocolCombo_->currentIndex());
}

void MainWindow::showInputError(const QString& message) {
    QMessageBox::warning(this,
                         english_ ? "Cannot start" : QString::fromUtf8("无法开始测试"),
                         message);
}

void MainWindow::installCommandLineTool() {
    const QString source = commandPath();
    if (!QFileInfo(source).isExecutable()) {
        showInputError(english_ ? "The command-line program is missing."
                                : QString::fromUtf8("当前安装包中缺少命令行程序。"));
        return;
    }

#ifdef Q_OS_WIN
    const QString directory = QDir(
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                  .filePath("bin");
    if (!QDir().mkpath(directory)) {
        showInputError(english_ ? "The install folder could not be created."
                                : QString::fromUtf8("无法创建命令行工具安装目录。"));
        return;
    }
    const QString destination = QDir(directory).filePath("pingkk.exe");
    QString error;
    if (!copyExecutable(source, destination, error)) {
        showInputError(english_ ? "The command-line tool could not be installed." : error);
        return;
    }

    QSettings environment("HKEY_CURRENT_USER\\Environment", QSettings::NativeFormat);
    QString path = environment.value("Path").toString();
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    const QStringList entries = path.split(';', Qt::SkipEmptyParts);
#else
    const QStringList entries = path.split(';', QString::SkipEmptyParts);
#endif
    if (!entries.contains(directory, Qt::CaseInsensitive)) {
        if (!path.isEmpty() && !path.endsWith(';')) path += ';';
        path += directory;
        environment.setValue("Path", path);
        environment.sync();
        DWORD_PTR result = 0;
        SendMessageTimeoutW(HWND_BROADCAST,
                            WM_SETTINGCHANGE,
                            0,
                            reinterpret_cast<LPARAM>(L"Environment"),
                            SMTO_ABORTIFHUNG,
                            3000,
                            &result);
    }
    QMessageBox::information(
        this,
        english_ ? "Installed" : QString::fromUtf8("安装完成"),
        english_ ? "The pingkk command was installed. Open a new terminal to use it."
                 : QString::fromUtf8("pingkk 命令已安装，请打开新的终端窗口使用。"));
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    const QString destination = "/usr/local/bin/pingkk";
    const QString command = "/bin/mkdir -p /usr/local/bin && /usr/bin/install -m 755 " +
                            shellQuote(source) + " " + shellQuote(destination);
    const QString script = "do shell script " + appleScriptQuote(command) +
                           " with administrator privileges";
    QProcess installer;
    installer.start("/usr/bin/osascript", QStringList() << "-e" << script);
    if (!installer.waitForStarted(3000) || !installer.waitForFinished(-1) ||
        installer.exitStatus() != QProcess::NormalExit || installer.exitCode() != 0) {
        showInputError(english_ ? "Installation was cancelled or failed."
                                : QString::fromUtf8("安装已取消或失败。"));
        return;
    }
    QMessageBox::information(
        this,
        english_ ? "Installed" : QString::fromUtf8("安装完成"),
        english_ ? "The pingkk command was installed in /usr/local/bin."
                 : QString::fromUtf8("pingkk 命令已安装到 /usr/local/bin。"));
#else
    const QString directory = QDir(
        QStandardPaths::writableLocation(QStandardPaths::HomeLocation))
                                  .filePath(".local/bin");
    if (!QDir().mkpath(directory)) {
        showInputError(english_ ? "The install folder could not be created."
                                : QString::fromUtf8("无法创建命令行工具安装目录。"));
        return;
    }
    const QString destination = QDir(directory).filePath("pingkk");
    QString error;
    if (!copyExecutable(source, destination, error)) {
        showInputError(english_ ? "The command-line tool could not be installed." : error);
        return;
    }
    QMessageBox::information(
        this,
        english_ ? "Installed" : QString::fromUtf8("安装完成"),
        english_ ? "The pingkk command was installed in ~/.local/bin."
                 : QString::fromUtf8("pingkk 命令已安装到 ~/.local/bin。"));
#endif
}

void MainWindow::copyReportImage() {
    QString output = outputEdit_->toPlainText().trimmed();
    if (output.isEmpty()) {
        QMessageBox::information(
            this,
            english_ ? "Nothing to export" : QString::fromUtf8("暂无测试结果"),
            english_ ? "Run a test before exporting the report."
                     : QString::fromUtf8("请先执行测试，再导出报告图片。"));
        return;
    }

    QStringList lines = output.split('\n');
    if (lines.size() > 500) {
        lines = lines.mid(lines.size() - 500);
        lines.prepend(english_ ? "[The log was long; only the latest 500 lines are included.]"
                               : QString::fromUtf8("[日志较长，仅保留最近 500 行]"));
        output = lines.join('\n');
    }

    const int imageWidth = 1200;
    const int outerMargin = 32;
    const int contentLeft = 76;
    const int contentWidth = imageWidth - contentLeft * 2;
    const int logTop = 286;
    const int logPadding = 28;

    QFont bodyFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    bodyFont.setPixelSize(19);
    QFontMetrics bodyMetrics(bodyFont);
    const QRect bodyBounds = bodyMetrics.boundingRect(
        QRect(0, 0, contentWidth - logPadding * 2, 200000),
        Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
        output);
    const int logHeight = qMax(210, bodyBounds.height() + logPadding * 2);
    const int imageHeight = logTop + logHeight + 64;

    QImage report(imageWidth, imageHeight, QImage::Format_ARGB32_Premultiplied);
    report.fill(QColor("#ffffff"));
    QPainter painter(&report);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    painter.setPen(QPen(QColor("#dfe3e8"), 2));
    painter.setBrush(QColor("#ffffff"));
    painter.drawRoundedRect(
        QRect(outerMargin, outerMargin,
              imageWidth - outerMargin * 2, imageHeight - outerMargin * 2),
        18,
        18);

    QFont titleFont("PingFang SC");
    titleFont.setPixelSize(30);
    titleFont.setWeight(QFont::DemiBold);
    painter.setFont(titleFont);
    painter.setPen(QColor("#18202a"));
    painter.drawText(QRect(contentLeft, 68, 600, 42),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     english_ ? "pingkk Network Report"
                              : QString::fromUtf8("ping看看 网络测试报告"));

    QFont metaFont("PingFang SC");
    metaFont.setPixelSize(16);
    painter.setFont(metaFont);
    painter.setPen(QColor("#68717d"));
    painter.drawText(QRect(700, 68, imageWidth - contentLeft - 700, 42),
                     Qt::AlignRight | Qt::AlignVCenter,
                     QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));

    painter.setPen(QPen(QColor("#e5e8ec"), 1));
    painter.drawLine(contentLeft, 130, imageWidth - contentLeft, 130);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#f5f7f9"));
    painter.drawRoundedRect(QRect(contentLeft, 154, contentWidth, 66), 10, 10);
    painter.setFont(metaFont);
    painter.setPen(QColor("#68717d"));
    painter.drawText(QRect(contentLeft + 22, 154, 150, 66),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     english_ ? "Local IP" : QString::fromUtf8("当前 IP"));
    QFont ipFont("PingFang SC");
    ipFont.setPixelSize(21);
    ipFont.setWeight(QFont::DemiBold);
    painter.setFont(ipFont);
    painter.setPen(QColor("#18202a"));
    painter.drawText(QRect(contentLeft + 160, 154, contentWidth - 182, 66),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     currentIpValue_->text());

    QFont sectionFont("PingFang SC");
    sectionFont.setPixelSize(20);
    sectionFont.setWeight(QFont::DemiBold);
    painter.setFont(sectionFont);
    painter.setPen(QColor("#18202a"));
    painter.drawText(QRect(contentLeft, 238, contentWidth, 32),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     english_ ? "Test output" : QString::fromUtf8("测试结果"));

    painter.setPen(QPen(QColor("#dfe3e8"), 1));
    painter.setBrush(QColor("#fbfcfd"));
    painter.drawRoundedRect(QRect(contentLeft, logTop, contentWidth, logHeight), 12, 12);
    painter.setFont(bodyFont);
    painter.setPen(QColor("#18202a"));
    painter.drawText(QRect(contentLeft + logPadding,
                           logTop + logPadding,
                           contentWidth - logPadding * 2,
                           logHeight - logPadding * 2),
                     Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                     output);
    painter.end();

    QApplication::clipboard()->setImage(report);
    QMessageBox messageBox(QMessageBox::NoIcon,
                           english_ ? "Report copied" : QString::fromUtf8("导出完成"),
                           english_ ? "The report image is in the clipboard and ready to paste."
                                    : QString::fromUtf8("报告图片已复制到剪切板，可以直接粘贴发送。"),
                           QMessageBox::Ok,
                           this);
    messageBox.exec();
}

void MainWindow::clearOutput() {
    if (outputEdit_->toPlainText().isEmpty()) return;

    QMessageBox confirmation(QMessageBox::Question,
                             english_ ? "Clear output" : QString::fromUtf8("清空日志"),
                             english_ ? "Clear all test output?"
                                      : QString::fromUtf8("确定要清空全部测试日志吗？"),
                             QMessageBox::NoButton,
                             this);
    QPushButton* clear = confirmation.addButton(
        english_ ? "Clear" : QString::fromUtf8("清空"), QMessageBox::DestructiveRole);
    confirmation.addButton(english_ ? "Cancel" : QString::fromUtf8("取消"),
                           QMessageBox::RejectRole);
    confirmation.exec();
    if (confirmation.clickedButton() == clear) outputEdit_->clear();
}
