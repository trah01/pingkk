#ifndef PINGKK_UPDATE_CHECK_H
#define PINGKK_UPDATE_CHECK_H

#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>

class UpdateCheck : public QObject {
public:
    typedef std::function<void(const QString&, const QUrl&)> Completion;
    explicit UpdateCheck(QObject* parent);
    // channel: auto (GitHub first, then the domestic mirror), cn or global.
    void start(const QString& channel, const Completion& completion);
    static QString mirrorBase();

private:
    void checkSource(bool github, bool fallback, long timeout, const Completion& completion);
};

#endif
