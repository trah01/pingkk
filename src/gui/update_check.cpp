#include "update_check.h"
#include "pingkk/version.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>

namespace {
const char* githubLatest = PINGKK_PROJECT_URL "/releases/latest";
const char* officialLatest = "https://he.sb/pingkk/latest/";

QString validVersion(const QString& tag) {
    static const QRegularExpression pattern("^v?([0-9]+\\.[0-9]+\\.[0-9]+)$");
    return pattern.match(tag).captured(1);
}
}

UpdateCheck::UpdateCheck(QObject* parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)) {}

void UpdateCheck::start(const Completion& completion) {
    checkSource(true, completion);
}

void UpdateCheck::checkSource(bool github, const Completion& completion) {
    QNetworkRequest request(QUrl(github ? QString(githubLatest)
                                        : QString(officialLatest) + "version.json"));
    request.setRawHeader("User-Agent", "pingkk/" PINGKK_VERSION);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    QNetworkReply* reply = github ? network_->head(request) : network_->get(request);
    QTimer* timer = new QTimer(reply);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    // The deadline includes redirects, matching the command-line updater.
    timer->start(github ? 1000 : 10000);
    connect(reply, &QNetworkReply::finished, this, [=]() {
        timer->stop();
        QString version;
        if (reply->error() == QNetworkReply::NoError &&
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
            if (github) {
                const QUrl release = reply->url();
                const QString prefix = "/trah01/pingkk/releases/tag/";
                if (release.scheme() == "https" && release.host() == "github.com" &&
                    release.path().startsWith(prefix)) {
                    version = validVersion(release.path().mid(prefix.size()));
                }
            } else {
                const QJsonObject metadata = QJsonDocument::fromJson(reply->readAll()).object();
                version = validVersion(metadata.value("tag_name").toString());
            }
        }
        reply->deleteLater();
        if (version.isEmpty() && github) {
            checkSource(false, completion);
        } else {
            completion(version, QUrl(github ? githubLatest : officialLatest));
        }
    });
}
