#ifndef PINGKK_MAIN_WINDOW_H
#define PINGKK_MAIN_WINDOW_H

#include <QMainWindow>
#include <QString>

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QSpinBox;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = 0);

private slots:
    void startSingleTest();
    void startContinuousTest();
    void stopTest();
    void readProcessOutput();
    void processFinished(int exitCode);
    void changeLanguage(int index);
    void updateProtocolFields(int index);
    void installCommandLineTool();
    void copyReportImage();
    void clearOutput();

private:
    void buildInterface();
    void updateTexts();
    void startTest(bool continuous);
    QString commandPath() const;
    QString protocolArgument() const;
    void setRunning(bool running);
    void showInputError(const QString& message);
    void appendOutputLine(const QString& line);
    void handleOutputLine(const QString& line);

    bool english_;
    bool stopRequested_;
    int reachableCount_;
    int unreachableCount_;
    int unknownCount_;
    QLabel* currentIpTitle_;
    QLabel* currentIpValue_;
    QLabel* targetTitle_;
    QLabel* portTitle_;
    QLabel* protocolTitle_;
    QLabel* timeoutTitle_;
    QLabel* outputTitle_;
    QLineEdit* targetEdit_;
    QLineEdit* portEdit_;
    QSpinBox* timeoutSpin_;
    QComboBox* protocolCombo_;
    QComboBox* languageCombo_;
    QCheckBox* advancedCheckBox_;
    QPushButton* singleButton_;
    QPushButton* continuousButton_;
    QPushButton* stopButton_;
    QPushButton* installButton_;
    QPushButton* exportButton_;
    QPushButton* clearButton_;
    QPlainTextEdit* outputEdit_;
    QProcess* process_;
    QString pendingOutputLine_;
    bool lastOutputWasBlank_;
};

#endif
