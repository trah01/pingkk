#include "update_transfer.h"
#include "pingkk/version.h"

#include <curl/curl.h>
#include <QElapsedTimer>
#include <QFile>

namespace {
class CurlRuntime {
public:
    CurlRuntime() : status(curl_global_init(CURL_GLOBAL_DEFAULT)) {}
    ~CurlRuntime() { if (status == CURLE_OK) curl_global_cleanup(); }
    CURLcode status;
};

struct TransferContext {
    UpdateTransfer* transfer;
    QFile* file;
    qint64 received = 0;
    QElapsedTimer progressTimer;
};

size_t writeData(char* bytes, size_t size, size_t count, void* opaque) {
    TransferContext* context = static_cast<TransferContext*>(opaque);
    const qint64 length = static_cast<qint64>(size * count);
    const qint64 limit = context->file ? 512LL * 1024 * 1024 : 1024 * 1024;
    if (length > limit - context->received || context->transfer->isInterruptionRequested()) return 0;
    if (context->file) {
        if (context->file->write(bytes, length) != length) return 0;
    } else {
        context->transfer->data.append(bytes, static_cast<int>(length));
    }
    context->received += length;
    return static_cast<size_t>(length);
}

int transferProgress(void* opaque, curl_off_t total, curl_off_t received, curl_off_t, curl_off_t) {
    TransferContext* context = static_cast<TransferContext*>(opaque);
    if (context->transfer->isInterruptionRequested()) return 1;
    if (context->progressTimer.elapsed() >= 100) {
        emit context->transfer->progress(received, total);
        context->progressTimer.restart();
    }
    return 0;
}
}

UpdateTransfer::UpdateTransfer(const QUrl& url, const QString& destination,
                               bool head, long timeoutMilliseconds, QObject* parent)
    : QThread(parent), url_(url), destination_(destination), head_(head), timeout_(timeoutMilliseconds) {}

UpdateTransfer::~UpdateTransfer() {
    requestInterruption();
    wait();
}

void UpdateTransfer::run() {
    static CurlRuntime runtime;
    if (runtime.status != CURLE_OK) { error = "Network initialization failed."; return; }
    QFile output(destination_);
    if (!destination_.isEmpty() && !output.open(QIODevice::WriteOnly)) {
        error = output.errorString();
        return;
    }
    CURL* curl = curl_easy_init();
    if (!curl) { error = "Unable to create a download."; return; }
    TransferContext context;
    context.transfer = this;
    context.file = destination_.isEmpty() ? NULL : &output;
    context.progressTimer.start();
    const QByteArray url = url_.toEncoded();
    CURLcode status = CURLE_OK;
    // 所用选项均兼容 macOS 10.13 的系统 libcurl。
#define UPDATE_OPTION(option, value) \
    if (status == CURLE_OK) status = curl_easy_setopt(curl, option, value)
    UPDATE_OPTION(CURLOPT_URL, url.constData());
    UPDATE_OPTION(CURLOPT_USERAGENT, "pingkk/" PINGKK_VERSION);
    UPDATE_OPTION(CURLOPT_NOSIGNAL, 1L);
    UPDATE_OPTION(CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    UPDATE_OPTION(CURLOPT_TIMEOUT_MS, timeout_);
    UPDATE_OPTION(CURLOPT_LOW_SPEED_LIMIT, 1024L);
    UPDATE_OPTION(CURLOPT_LOW_SPEED_TIME, 30L);
    UPDATE_OPTION(CURLOPT_FOLLOWLOCATION, 1L);
    UPDATE_OPTION(CURLOPT_MAXREDIRS, 10L);
    UPDATE_OPTION(CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
    UPDATE_OPTION(CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
    UPDATE_OPTION(CURLOPT_SSL_VERIFYPEER, 1L);
    UPDATE_OPTION(CURLOPT_SSL_VERIFYHOST, 2L);
    UPDATE_OPTION(CURLOPT_FAILONERROR, 1L);
    UPDATE_OPTION(CURLOPT_NOBODY, head_ ? 1L : 0L);
    UPDATE_OPTION(CURLOPT_WRITEFUNCTION, &writeData);
    UPDATE_OPTION(CURLOPT_WRITEDATA, &context);
    UPDATE_OPTION(CURLOPT_NOPROGRESS, 0L);
    UPDATE_OPTION(CURLOPT_XFERINFOFUNCTION, &transferProgress);
    UPDATE_OPTION(CURLOPT_XFERINFODATA, &context);
#undef UPDATE_OPTION
    if (status == CURLE_OK) status = curl_easy_perform(curl);
    char* effective = NULL;
    long response = 0;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
    if (effective) finalUrl = QUrl(QString::fromUtf8(effective));
    if (status != CURLE_OK) error = QString::fromUtf8(curl_easy_strerror(status));
    else if (response != 200) error = QString("HTTP %1").arg(response);
    if (output.isOpen() && !output.flush()) error = output.errorString();
    curl_easy_cleanup(curl);
}
