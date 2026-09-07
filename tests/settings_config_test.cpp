#include "vpet/settings_config.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace vpet;

namespace
{

const QString LLM_FILE_NAME = QStringLiteral("llm_config.json");
const QString MEMORY_FILE_NAME = QStringLiteral("memory_config.json");

/**
 * @brief 写入文本文件（UTF-8）
 * @param[in] path 目标路径
 * @param[in] content 文本内容
 * @return 写入成功返回 true
 */
bool WriteTextFile(const QString &path, const QString &content)
{
    QFile file(path);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return false;
    }

    const bool ok = file.write(content.toUtf8()) == content.toUtf8().size();
    file.close();
    return ok;
}

/**
 * @brief 读取文件原始文本；文件不存在返回空字符串
 */
QString ReadTextFile(const QString &path)
{
    QFile file(path);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return QString();
    }

    const QString content = QString::fromUtf8(file.readAll());
    file.close();
    return content;
}

/**
 * @brief 构造合法的 LLM 字段
 */
LlmSettingsFields MakeValidLlmFields()
{
    LlmSettingsFields fields;
    fields.baseUrl = QStringLiteral("https://api.example.com");
    fields.apiKey = QStringLiteral("sk-test");
    fields.model = QStringLiteral("test-model");
    fields.timeoutMs = 30000;
    return fields;
}

/**
 * @brief 构造合法的记忆字段
 */
MemorySettingsFields MakeValidMemoryFields()
{
    MemorySettingsFields fields;
    fields.memoryEnabled = true;
    fields.embeddingEnabled = false;
    fields.embeddingModel = QStringLiteral("BAAI/bge-small-zh-v1.5");
    fields.embeddingModelDir = QString();
    return fields;
}

} // anonymous namespace

class SettingsConfigTest : public QObject
{
    Q_OBJECT

private slots:
    void ValidateLlmRejectsEmptyBaseUrl();
    void ValidateLlmRejectsInvalidUrl();
    void ValidateLlmRejectsEmptyKeyAndModel();
    void ValidateLlmRejectsTimeoutOutOfRange();
    void ValidateMemoryEmbeddingRequiresModel();
    void SaveCreatesMissingFileWithDefaults();
    void SaveLlmPreservesUnknownFields();
    void SaveMemoryPreservesUnknownFields();
    void SaveRefusesInvalidJsonAndKeepsFile();
    void RoundTripLlmFields();
    void RoundTripMemoryFields();
    void SaveFailsOnUnwritablePath();
};

void SettingsConfigTest::ValidateLlmRejectsEmptyBaseUrl()
{
    LlmSettingsFields fields = MakeValidLlmFields();
    fields.baseUrl = QStringLiteral("   ");

    QString errorMessage;
    QVERIFY(!ValidateLlmFields(fields, errorMessage));
    QVERIFY(!errorMessage.isEmpty());
}

void SettingsConfigTest::ValidateLlmRejectsInvalidUrl()
{
    QString errorMessage;

    LlmSettingsFields noScheme = MakeValidLlmFields();
    noScheme.baseUrl = QStringLiteral("not-a-url");
    QVERIFY(!ValidateLlmFields(noScheme, errorMessage));

    LlmSettingsFields noHost = MakeValidLlmFields();
    noHost.baseUrl = QStringLiteral("https://");
    QVERIFY(!ValidateLlmFields(noHost, errorMessage));
}

void SettingsConfigTest::ValidateLlmRejectsEmptyKeyAndModel()
{
    QString errorMessage;

    LlmSettingsFields emptyKey = MakeValidLlmFields();
    emptyKey.apiKey = QString();
    QVERIFY(!ValidateLlmFields(emptyKey, errorMessage));

    LlmSettingsFields emptyModel = MakeValidLlmFields();
    emptyModel.model = QString();
    QVERIFY(!ValidateLlmFields(emptyModel, errorMessage));

    // 合法字段必须通过，防止上面两条规则误伤正常路径。
    QVERIFY(ValidateLlmFields(MakeValidLlmFields(), errorMessage));
}

void SettingsConfigTest::ValidateLlmRejectsTimeoutOutOfRange()
{
    QString errorMessage;

    LlmSettingsFields tooSmall = MakeValidLlmFields();
    tooSmall.timeoutMs = 500;
    QVERIFY(!ValidateLlmFields(tooSmall, errorMessage));

    LlmSettingsFields tooLarge = MakeValidLlmFields();
    tooLarge.timeoutMs = 300000;
    QVERIFY(!ValidateLlmFields(tooLarge, errorMessage));

    LlmSettingsFields boundary = MakeValidLlmFields();
    boundary.timeoutMs = 1000;
    QVERIFY(ValidateLlmFields(boundary, errorMessage));
}

void SettingsConfigTest::ValidateMemoryEmbeddingRequiresModel()
{
    QString errorMessage;

    MemorySettingsFields enabledNoModel = MakeValidMemoryFields();
    enabledNoModel.embeddingEnabled = true;
    enabledNoModel.embeddingModel = QString();
    QVERIFY(!ValidateMemoryFields(enabledNoModel, errorMessage));

    // 未启用向量化时允许空模型名（与 MemoryService::ParseConfig 一致）。
    MemorySettingsFields disabledNoModel = MakeValidMemoryFields();
    disabledNoModel.embeddingEnabled = false;
    disabledNoModel.embeddingModel = QString();
    QVERIFY(ValidateMemoryFields(disabledNoModel, errorMessage));
}

void SettingsConfigTest::SaveCreatesMissingFileWithDefaults()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString llmPath = tempDir.filePath(LLM_FILE_NAME);
    QString errorMessage;

    LlmSettingsFields fields = MakeValidLlmFields();
    QVERIFY(SaveLlmFields(llmPath, fields, errorMessage));
    QVERIFY(errorMessage.isEmpty());
    QVERIFY(QFile::exists(llmPath));

    // 重新读取应还原字段。
    LlmSettingsFields loaded;
    QVERIFY(LoadLlmFields(llmPath, loaded, errorMessage));
    QCOMPARE(loaded.baseUrl, fields.baseUrl);
    QCOMPARE(loaded.apiKey, fields.apiKey);
    QCOMPARE(loaded.model, fields.model);
    QCOMPARE(loaded.timeoutMs, fields.timeoutMs);

    // 缺失的记忆配置读取时返回默认值。
    const QString memoryPath = tempDir.filePath(MEMORY_FILE_NAME);
    MemorySettingsFields memoryFields;
    QVERIFY(LoadMemoryFields(memoryPath, memoryFields, errorMessage));
    QVERIFY(memoryFields.memoryEnabled);
    QVERIFY(!memoryFields.embeddingEnabled);
}

void SettingsConfigTest::SaveLlmPreservesUnknownFields()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString llmPath = tempDir.filePath(LLM_FILE_NAME);
    QVERIFY(WriteTextFile(llmPath,
                          QStringLiteral("{\n  \"base_url\": \"https://old.example.com\",\n"
                                         "  \"api_key\": \"sk-old\",\n"
                                         "  \"model\": \"old-model\",\n"
                                         "  \"custom_future_field\": {\"keep\": true},\n"
                                         "  \"retry_count\": 3\n}\n")));

    LlmSettingsFields fields = MakeValidLlmFields();
    QString errorMessage;
    QVERIFY(SaveLlmFields(llmPath, fields, errorMessage));

    const QJsonObject root = QJsonDocument::fromJson(ReadTextFile(llmPath).toUtf8()).object();
    QCOMPARE(root.value(QStringLiteral("base_url")).toString(), fields.baseUrl);
    QCOMPARE(root.value(QStringLiteral("api_key")).toString(), fields.apiKey);
    QCOMPARE(root.value(QStringLiteral("model")).toString(), fields.model);
    QCOMPARE(root.value(QStringLiteral("timeout_ms")).toInt(), fields.timeoutMs);

    // 未编辑的未知字段必须原样保留。
    QCOMPARE(root.value(QStringLiteral("retry_count")).toInt(), 3);
    const QJsonObject customField =
        root.value(QStringLiteral("custom_future_field")).toObject();
    QCOMPARE(customField.value(QStringLiteral("keep")).toBool(), true);
}

void SettingsConfigTest::SaveMemoryPreservesUnknownFields()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString memoryPath = tempDir.filePath(MEMORY_FILE_NAME);

    // 模拟真实 memory_config.json：含 maintenance 对象与 embedding 未知键。
    QVERIFY(WriteTextFile(memoryPath,
                          QStringLiteral("{\n  \"enabled\": true,\n"
                                         "  \"data_dir\": \"\",\n"
                                         "  \"maintenance\": {\"enabled\": true, "
                                         "\"custom_maintenance_key\": 7},\n"
                                         "  \"embedding\": {\"enabled\": false, "
                                         "\"backend\": \"local_onnx\", "
                                         "\"model\": \"BAAI/bge-small-zh-v1.5\", "
                                         "\"vector_store\": \"sqlite\", "
                                         "\"custom_embedding_key\": \"x\"}\n}\n")));

    MemorySettingsFields fields = MakeValidMemoryFields();
    fields.memoryEnabled = false;
    fields.embeddingEnabled = true;
    fields.embeddingModel = QStringLiteral("BAAI/bge-small-zh-v1.5");
    fields.embeddingModelDir = QStringLiteral("D:/models/bge");

    QString errorMessage;
    QVERIFY(SaveMemoryFields(memoryPath, fields, errorMessage));

    const QJsonObject root = QJsonDocument::fromJson(ReadTextFile(memoryPath).toUtf8()).object();
    QCOMPARE(root.value(QStringLiteral("enabled")).toBool(), false);
    QCOMPARE(root.value(QStringLiteral("data_dir")).toString(), QString());

    // maintenance 整块未被面板管理，必须原样保留。
    const QJsonObject maintenance = root.value(QStringLiteral("maintenance")).toObject();
    QCOMPARE(maintenance.value(QStringLiteral("enabled")).toBool(), true);
    QCOMPARE(maintenance.value(QStringLiteral("custom_maintenance_key")).toInt(), 7);

    // embedding 内被管理的键更新，未知键保留。
    const QJsonObject embedding = root.value(QStringLiteral("embedding")).toObject();
    QCOMPARE(embedding.value(QStringLiteral("enabled")).toBool(), true);
    QCOMPARE(embedding.value(QStringLiteral("model_dir")).toString(), fields.embeddingModelDir);
    QCOMPARE(embedding.value(QStringLiteral("backend")).toString(), QStringLiteral("local_onnx"));
    QCOMPARE(embedding.value(QStringLiteral("vector_store")).toString(), QStringLiteral("sqlite"));
    QCOMPARE(embedding.value(QStringLiteral("custom_embedding_key")).toString(),
             QStringLiteral("x"));
}

void SettingsConfigTest::SaveRefusesInvalidJsonAndKeepsFile()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString llmPath = tempDir.filePath(LLM_FILE_NAME);
    const QString brokenContent = QStringLiteral("{ not valid json !!!");

    QVERIFY(WriteTextFile(llmPath, brokenContent));

    QString errorMessage;
    QVERIFY(!SaveLlmFields(llmPath, MakeValidLlmFields(), errorMessage));
    QVERIFY(!errorMessage.isEmpty());
    QCOMPARE(ReadTextFile(llmPath), brokenContent);

    LlmSettingsFields invalidLoadFields;
    QVERIFY(!LoadLlmFields(llmPath, invalidLoadFields, errorMessage));
    QVERIFY(!errorMessage.isEmpty());

    // 记忆配置同样拒绝覆盖非法 JSON。
    const QString memoryPath = tempDir.filePath(MEMORY_FILE_NAME);
    QVERIFY(WriteTextFile(memoryPath, brokenContent));
    QVERIFY(!SaveMemoryFields(memoryPath, MakeValidMemoryFields(), errorMessage));
    QVERIFY(!errorMessage.isEmpty());
    QCOMPARE(ReadTextFile(memoryPath), brokenContent);
}

void SettingsConfigTest::RoundTripLlmFields()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString llmPath = tempDir.filePath(LLM_FILE_NAME);
    QString errorMessage;

    LlmSettingsFields first = MakeValidLlmFields();
    first.timeoutMs = 45000;
    QVERIFY(SaveLlmFields(llmPath, first, errorMessage));

    LlmSettingsFields second = MakeValidLlmFields();
    second.baseUrl = QStringLiteral("http://127.0.0.1:8000/v1");
    second.model = QStringLiteral("another-model");
    QVERIFY(SaveLlmFields(llmPath, second, errorMessage));

    LlmSettingsFields loaded;
    QVERIFY(LoadLlmFields(llmPath, loaded, errorMessage));
    QCOMPARE(loaded.baseUrl, second.baseUrl);
    QCOMPARE(loaded.model, second.model);
    QCOMPARE(loaded.timeoutMs, second.timeoutMs);
    QVERIFY(errorMessage.isEmpty());
}

void SettingsConfigTest::RoundTripMemoryFields()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString memoryPath = tempDir.filePath(MEMORY_FILE_NAME);
    QString errorMessage;

    MemorySettingsFields fields;
    fields.memoryEnabled = false;
    fields.embeddingEnabled = true;
    fields.embeddingModel = QStringLiteral("custom/model");
    fields.embeddingModelDir = QStringLiteral("models/embedding/custom");
    QVERIFY(SaveMemoryFields(memoryPath, fields, errorMessage));

    MemorySettingsFields loaded;
    QVERIFY(LoadMemoryFields(memoryPath, loaded, errorMessage));
    QCOMPARE(loaded.memoryEnabled, fields.memoryEnabled);
    QCOMPARE(loaded.embeddingEnabled, fields.embeddingEnabled);
    QCOMPARE(loaded.embeddingModel, fields.embeddingModel);
    QCOMPARE(loaded.embeddingModelDir, fields.embeddingModelDir);
}

void SettingsConfigTest::SaveFailsOnUnwritablePath()
{
    // 指向一个目录而非文件，QSaveFile 打开必然失败，接口应如实报告。
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dirAsPath = tempDir.filePath(QStringLiteral("subdir"));
    QVERIFY(QDir().mkpath(dirAsPath));

    QString errorMessage;
    QVERIFY(!SaveLlmFields(dirAsPath, MakeValidLlmFields(), errorMessage));
    QVERIFY(!errorMessage.isEmpty());
}

QTEST_MAIN(SettingsConfigTest)
#include "settings_config_test.moc"
