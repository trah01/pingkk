#include "update_check.h"
#include "update_transfer.h"
#include "pingkk/version.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace {
const char* githubLatest = PINGKK_PROJECT_URL "/releases/latest";
const char* officialLatest = "https://he.sb/pingkk/latest/";

QString validVersion(const QString& tag) {
    static const QRegularExpression pattern("^v?([0-9]+\\.[0-9]+\\.[0-9]+)$");
    return pattern.match(tag).captured(1);
}
}

UpdateCheck::UpdateCheck(QObject* parent)
    : QObject(parent) {}

void UpdateCheck::start(const Completion& completion) {
    checkSource(true, completion);
}

void UpdateCheck::checkSource(bool github, const Completion& completion) {
    UpdateTransfer* transfer = new UpdateTransfer(
        QUrl(github ? QString(githubLatest) : QString(officialLatest) + "version.json"),
        QString(), github, github ? 1000 : 10000, this);
    connect(transfer, &QThread::finished, this, [=]() {
        QString version;
        if (transfer->error.isEmpty()) {
            if (github) {
                const QUrl release = transfer->finalUrl;
                const QString prefix = "/trah01/pingkk/releases/tag/";
                if (release.scheme() == "https" && release.host() == "github.com" &&
                    release.path().startsWith(prefix)) {
                    version = validVersion(release.path().mid(prefix.size()));
                }
            } else {
                const QJsonObject metadata = QJsonDocument::fromJson(transfer->data).object();
                version = validVersion(metadata.value("tag_name").toString());
            }
        }
        transfer->deleteLater();
        if (version.isEmpty() && github) {
            checkSource(false, completion);
        } else {
            completion(version, QUrl(github ? githubLatest : officialLatest));
        }
    });
    transfer->start();
}
