#include "vpet/tts_client.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>

namespace vpet
{

namespace
{

constexpr int HTTP_REQUEST_TIMEOUT_MS = 30000; ///< HTTP 请求超时（毫秒）

} // anonymous namespace

TtsClient::TtsClient(QObject *parent)
    : QObject(parent)
    , m_networkManager(nullptr)
    , m_config()
    , m_isConfigured(false)
{
    m_networkManager = new QNetworkAccessManager(this);

    connect(m_networkManager, &QNetworkAccessManager::finished,
            this, &TtsClient::OnReplyFinished);
}

TtsClient::~TtsClient()
{
    // m_networkManager 由 QObject 父子关系自动销毁
}

bool TtsClient::LoadConfig(const QString &configPath)
{
    qDebug() << "[TTS] TtsClient::LoadConfig";

    // 检查参数有效性
    if (configPath.isEmpty())
    {
        qDebug() << "[TTS]   FAILED - empty config path";
        return false;
    }

    QFile file(configPath);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        qDebug() << "[TTS]   FAILED - cannot open config file";
        return false;
    }

    const QByteArray data = file.readAll();
    file.close();

    const QJsonDocument doc = QJsonDocument::fromJson(data);

    if (!doc.isObject())
    {
        qDebug() << "[TTS]   FAILED - invalid JSON";
        return false;
    }

    const QJsonObject obj = doc.object();

    m_config.serverUrl = obj.value(QStringLiteral("server_url")).toString(
                             QStringLiteral("http://127.0.0.1:9880"));

    m_config.voice = obj.value(QStringLiteral("voice")).toString(
                         QStringLiteral("zf_xiaobei"));

    m_config.speed = obj.value(QStringLiteral("speed")).toDouble(1.0);
    m_config.speed = qBound(0.5, m_config.speed, 2.0);

    m_config.lang = obj.value(QStringLiteral("lang")).toString(
                        QStringLiteral("z"));

    qDebug() << "[TTS]   configuration parsed";

    m_isConfigured = true;
    qDebug() << "[TTS]   config loaded successfully";
    return true;
}

bool TtsClient::IsConfigured() const
{
    return m_isConfigured;
}

void TtsClient::Synthesize(const QString &text, const QString &outputPath)
{
    qDebug() << "[TTS] TtsClient::Synthesize";
    qDebug() << "[TTS]   text length:" << text.size();

    // 检查参数有效性
    if (!m_isConfigured)
    {
        qDebug() << "[TTS]   FAILED - not configured";
        emit SynthesisFinished(QString());
        return;
    }

    if (text.isEmpty() || outputPath.isEmpty())
    {
        qDebug() << "[TTS]   FAILED - empty text or outputPath";
        emit SynthesisFinished(QString());
        return;
    }

    // 确保输出目录存在
    const QFileInfo fileInfo(outputPath);
    const QDir dir = fileInfo.absoluteDir();

    if (!dir.exists())
    {
        dir.mkpath(QStringLiteral("."));
    }

    // 构建请求 JSON
    QJsonObject body;
    body[QStringLiteral("text")] = text;
    body[QStringLiteral("lang")] = m_config.lang;
    body[QStringLiteral("voice")] = m_config.voice;
    body[QStringLiteral("speed")] = m_config.speed;

    const QByteArray bodyData = QJsonDocument(body).toJson(QJsonDocument::Compact);

    qDebug() << "[TTS]   request body bytes:" << bodyData.size();

    QNetworkRequest request(QUrl(m_config.serverUrl + QStringLiteral("/tts")));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setTransferTimeout(HTTP_REQUEST_TIMEOUT_MS);

    QNetworkReply *reply = m_networkManager->post(request, bodyData);

    // 将输出路径绑定到此次请求的 reply 上，防止并发请求时路径错乱
    reply->setProperty("outputPath", outputPath);

    qDebug() << "[TTS]   request sent, waiting for reply...";
}

void TtsClient::OnReplyFinished(QNetworkReply *reply)
{
    qDebug() << "[TTS] TtsClient::OnReplyFinished";

    // 检查参数有效性
    if (reply == nullptr)
    {
        qDebug() << "[TTS]   FAILED - null reply";
        emit SynthesisFinished(QString());
        return;
    }

    const auto replyGuard = qScopeGuard([reply]()
    {
        reply->deleteLater();
    });

    // 从 reply 属性中获取本次请求的输出路径
    const QString outputPath = reply->property("outputPath").toString();

    if (outputPath.isEmpty())
    {
        qDebug() << "[TTS]   FAILED - no outputPath property on reply";
        emit SynthesisFinished(QString());
        return;
    }

    const int statusCode = reply->attribute(
                               QNetworkRequest::HttpStatusCodeAttribute).toInt();

    qDebug() << "[TTS]   HTTP status:" << statusCode;

    if (reply->error() != QNetworkReply::NoError)
    {
        const QByteArray errorBody = reply->readAll();
        qDebug() << "[TTS]   FAILED - network error:" << reply->errorString();
        qDebug() << "[TTS]   server response bytes:" << errorBody.size();
        emit SynthesisFinished(QString());
        return;
    }

    if (statusCode != 200)
    {
        const QByteArray errorBody = reply->readAll();
        qDebug() << "[TTS]   FAILED - HTTP" << statusCode
                   << "response bytes:" << errorBody.size();
        emit SynthesisFinished(QString());
        return;
    }

    const QByteArray audioData = reply->readAll();

    qDebug() << "[TTS]   received audio data size:" << audioData.size() << "bytes";

    if (audioData.isEmpty())
    {
        qDebug() << "[TTS]   FAILED - empty audio data";
        emit SynthesisFinished(QString());
        return;
    }

    // 写入音频文件
    QFile outputFile(outputPath);

    if (!outputFile.open(QIODevice::WriteOnly))
    {
        qDebug() << "[TTS]   FAILED - cannot write output file";
        emit SynthesisFinished(QString());
        return;
    }

    const qint64 bytesWritten = outputFile.write(audioData);
    outputFile.close();

    if (bytesWritten != audioData.size())
    {
        qDebug() << "[TTS]   FAILED - incomplete audio write:" << bytesWritten
                 << "expected:" << audioData.size();
        QFile::remove(outputPath);
        emit SynthesisFinished(QString());
        return;
    }

    qDebug() << "[TTS]   wrote audio bytes:" << bytesWritten;
    qDebug() << "[TTS]   synthesis SUCCESS";

    emit SynthesisFinished(outputPath);
}

} // namespace vpet
