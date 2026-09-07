#include "vpet/settings_config.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>

namespace vpet
{

namespace
{

constexpr int MIN_TIMEOUT_MS = 1000;
constexpr int MAX_TIMEOUT_MS = 120000;

/**
 * @brief 读取配置根对象
 * @param[in] configPath 配置文件路径
 * @param[out] object 输出根对象
 * @param[out] errorMessage 读取失败描述
 * @return 文件缺失（空对象）或读取成功返回 true；JSON 非法返回 false
 *
 * 文件存在但 JSON 非法时返回 false，由调用方拒绝保存，
 * 避免用默认结构悄悄覆盖用户的配置内容。
 */
bool LoadConfigObject(const QString &configPath,
                      QJsonObject &object,
                      QString &errorMessage)
{
    object = QJsonObject();

    QFile configFile(configPath);

    if (!configFile.exists())
    {
        errorMessage.clear();
        return true;
    }

    if (!configFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        errorMessage = QStringLiteral("无法读取配置文件：%1").arg(configPath);
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(configFile.readAll(), &parseError);
    configFile.close();

    if ((parseError.error != QJsonParseError::NoError) || !document.isObject())
    {
        errorMessage = QStringLiteral("配置文件不是有效的 JSON 对象，已拒绝读取：%1").arg(configPath);
        return false;
    }

    object = document.object();
    errorMessage.clear();
    return true;
}

/**
 * @brief 通过 QSaveFile 原子写入配置对象
 * @param[in] configPath 配置文件路径
 * @param[in] object 待写入根对象
 * @param[out] errorMessage 写入失败描述
 * @return 保存成功返回 true
 */
bool SaveConfigObject(const QString &configPath,
                      const QJsonObject &object,
                      QString &errorMessage)
{
    const QByteArray serialized =
        QJsonDocument(object).toJson(QJsonDocument::Indented);

    QSaveFile saveFile(configPath);

    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        errorMessage = QStringLiteral("无法打开配置文件用于写入：%1（%2）")
                           .arg(configPath, saveFile.errorString());
        return false;
    }

    if (saveFile.write(serialized) != serialized.size())
    {
        errorMessage = QStringLiteral("写入配置文件失败：%1（%2）")
                           .arg(configPath, saveFile.errorString());
        saveFile.cancelWriting();
        return false;
    }

    if (!saveFile.commit())
    {
        errorMessage = QStringLiteral("提交配置文件失败：%1（%2）")
                           .arg(configPath, saveFile.errorString());
        return false;
    }

    errorMessage.clear();
    return true;
}

} // anonymous namespace

bool ValidateLlmFields(const LlmSettingsFields &fields, QString &errorMessage)
{
    const QString trimmedBaseUrl = fields.baseUrl.trimmed();

    if (trimmedBaseUrl.isEmpty())
    {
        errorMessage = QStringLiteral("Base URL 不能为空。");
        return false;
    }

    const QUrl baseUrl(trimmedBaseUrl);

    if (!baseUrl.isValid() || baseUrl.scheme().isEmpty() || baseUrl.host().isEmpty())
    {
        errorMessage = QStringLiteral("Base URL 无效，需要包含协议和主机名，例如 https://api.example.com。");
        return false;
    }

    if (fields.apiKey.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("API Key 不能为空。");
        return false;
    }

    if (fields.model.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("模型名称不能为空。");
        return false;
    }

    if ((fields.timeoutMs < MIN_TIMEOUT_MS) || (fields.timeoutMs > MAX_TIMEOUT_MS))
    {
        errorMessage = QStringLiteral("超时时间必须在 %1 到 %2 毫秒之间。")
                           .arg(MIN_TIMEOUT_MS)
                           .arg(MAX_TIMEOUT_MS);
        return false;
    }

    errorMessage.clear();
    return true;
}

bool ValidateMemoryFields(const MemorySettingsFields &fields, QString &errorMessage)
{
    if (fields.embeddingEnabled && fields.embeddingModel.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("启用向量化时必须填写 embedding 模型名称。");
        return false;
    }

    errorMessage.clear();
    return true;
}

bool LoadLlmFields(const QString &configPath,
                   LlmSettingsFields &fields,
                   QString &errorMessage)
{
    fields = LlmSettingsFields();

    QJsonObject rootObject;

    if (!LoadConfigObject(configPath, rootObject, errorMessage))
    {
        return false;
    }

    fields.baseUrl = rootObject.value(QStringLiteral("base_url")).toString();
    fields.apiKey = rootObject.value(QStringLiteral("api_key")).toString();
    fields.model = rootObject.value(QStringLiteral("model")).toString();
    fields.timeoutMs = rootObject.value(QStringLiteral("timeout_ms")).toInt(fields.timeoutMs);

    errorMessage.clear();
    return true;
}

bool LoadMemoryFields(const QString &configPath,
                      MemorySettingsFields &fields,
                      QString &errorMessage)
{
    fields = MemorySettingsFields();

    QJsonObject rootObject;

    if (!LoadConfigObject(configPath, rootObject, errorMessage))
    {
        return false;
    }

    if (rootObject.contains(QStringLiteral("enabled")))
    {
        fields.memoryEnabled = rootObject.value(QStringLiteral("enabled")).toBool(true);
    }

    const QJsonValue embeddingValue = rootObject.value(QStringLiteral("embedding"));

    if (embeddingValue.isObject())
    {
        const QJsonObject embeddingObject = embeddingValue.toObject();

        fields.embeddingEnabled =
            embeddingObject.value(QStringLiteral("enabled")).toBool(false);
        fields.embeddingModel = embeddingObject.value(QStringLiteral("model"))
                                    .toString(fields.embeddingModel)
                                    .trimmed();
        fields.embeddingModelDir = embeddingObject.value(QStringLiteral("model_dir"))
                                       .toString()
                                       .trimmed();
    }

    errorMessage.clear();
    return true;
}

bool SaveLlmFields(const QString &configPath,
                   const LlmSettingsFields &fields,
                   QString &errorMessage)
{
    if (!ValidateLlmFields(fields, errorMessage))
    {
        return false;
    }

    QJsonObject rootObject;

    if (!LoadConfigObject(configPath, rootObject, errorMessage))
    {
        return false;
    }

    rootObject.insert(QStringLiteral("base_url"), fields.baseUrl.trimmed());
    rootObject.insert(QStringLiteral("api_key"), fields.apiKey.trimmed());
    rootObject.insert(QStringLiteral("model"), fields.model.trimmed());
    rootObject.insert(QStringLiteral("timeout_ms"), fields.timeoutMs);

    return SaveConfigObject(configPath, rootObject, errorMessage);
}

bool SaveMemoryFields(const QString &configPath,
                      const MemorySettingsFields &fields,
                      QString &errorMessage)
{
    if (!ValidateMemoryFields(fields, errorMessage))
    {
        return false;
    }

    QJsonObject rootObject;

    if (!LoadConfigObject(configPath, rootObject, errorMessage))
    {
        return false;
    }

    rootObject.insert(QStringLiteral("enabled"), fields.memoryEnabled);

    const QJsonValue existingEmbeddingValue = rootObject.value(QStringLiteral("embedding"));

    if (existingEmbeddingValue.isUndefined())
    {
        // 文件缺失或未配置 embedding 时创建最小结构，其余字段由
        // MemoryService::ParseConfig 按默认值补齐。
        QJsonObject embeddingObject;
        embeddingObject.insert(QStringLiteral("enabled"), fields.embeddingEnabled);
        embeddingObject.insert(QStringLiteral("backend"), QStringLiteral("local_onnx"));
        embeddingObject.insert(QStringLiteral("model"), fields.embeddingModel.trimmed());
        embeddingObject.insert(QStringLiteral("model_dir"), fields.embeddingModelDir.trimmed());
        rootObject.insert(QStringLiteral("embedding"), embeddingObject);
    }
    else if (existingEmbeddingValue.isObject())
    {
        // 只替换面板管理的键，embedding 内的其余字段原样保留。
        QJsonObject embeddingObject = existingEmbeddingValue.toObject();
        embeddingObject.insert(QStringLiteral("enabled"), fields.embeddingEnabled);
        embeddingObject.insert(QStringLiteral("model"), fields.embeddingModel.trimmed());
        embeddingObject.insert(QStringLiteral("model_dir"), fields.embeddingModelDir.trimmed());
        rootObject.insert(QStringLiteral("embedding"), embeddingObject);
    }
    else
    {
        errorMessage = QStringLiteral("配置文件中 embedding 字段必须是对象：%1").arg(configPath);
        return false;
    }

    return SaveConfigObject(configPath, rootObject, errorMessage);
}

} // namespace vpet
