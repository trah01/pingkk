#ifndef PINGKK_UPDATE_TRANSFER_H
#define PINGKK_UPDATE_TRANSFER_H

#include <QByteArray>
#include <QThread>
#include <QUrl>

// libcurl 使用 Windows 系统证书，Qt 5 包无需另带 OpenSSL DLL。
class UpdateTransfer : public QThread {
    Q_OBJECT
public:
    UpdateTransfer(const QUrl& url, const QString& destination, bool head,
                   long timeoutMilliseconds, QObject* parent);
    ~UpdateTransfer();
    QByteArray data;
    QUrl finalUrl;
    QString error;

signals:
    void progress(qint64 received, qint64 total);

protected:
    void run() override;

private:
    QUrl url_;
    QString destination_;
    bool head_;
    long timeout_;
};

#endif
