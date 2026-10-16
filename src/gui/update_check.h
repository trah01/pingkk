#ifndef PINGKK_UPDATE_CHECK_H
#define PINGKK_UPDATE_CHECK_H

#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;

class UpdateCheck : public QObject {
public:
    typedef std::function<void(const QString&, const QUrl&)> Completion;
    explicit UpdateCheck(QObject* parent);
    void start(const Completion& completion);

private:
    void checkSource(bool github, const Completion& completion);
    QNetworkAccessManager* network_;
};

#endif
