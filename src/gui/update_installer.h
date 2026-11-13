#ifndef PINGKK_UPDATE_INSTALLER_H
#define PINGKK_UPDATE_INSTALLER_H

#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QUrl>
#include <memory>

class UpdateTransfer;

class UpdateInstaller : public QObject {
    Q_OBJECT
public:
    explicit UpdateInstaller(bool english, QObject* parent);
    ~UpdateInstaller();
    // fallback: retry from the domestic mirror when GitHub fails (automatic channel).
    void start(const QString& version, const QUrl& source, bool fallback);
    bool restart();

signals:
    void statusChanged(const QString& message);
    void progress(qint64 received, qint64 total);
    void ready();
    void failed(const QString& message);

private:
    bool selectPackage();
    void download(bool github);
    void downloadPackage(const QUrl& base, bool github, const QByteArray& checksum);
    void prepare();
    void runPreparation(const QString& command, const QStringList& arguments, bool verify);
    void fail(const QString& message);
    QString message(const char* english, const char* chinese) const;

    bool english_;
    bool ready_ = false;
    bool fallback_ = false;
    QString version_;
    QString kind_;
    QString target_;
    QString asset_;
    QString script_;
    QString package_;
    QString extracted_;
    std::unique_ptr<QTemporaryDir> work_;
    UpdateTransfer* transfer_ = NULL;
    QProcess process_;
};

#endif
