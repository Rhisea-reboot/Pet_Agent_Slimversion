#include "vpet/memory/vector_store.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vpet
{

namespace
{

/**
 * @brief 将 SQL 错误格式化为诊断文本
 */
QString SqlErrorText(const QSqlError &error)
{
    return error.driverText() + QStringLiteral(" | ") + error.databaseText();
}

/**
 * @brief 验证向量元素为有限浮点数
 */
bool IsFiniteVector(const QVector<float> &embedding)
{
    for (const float value : embedding)
    {
        if (!std::isfinite(value))
        {
            return false;
        }
    }

    return true;
}

} // anonymous namespace

/**
 * @brief VectorStore 内部实现
 */
struct VectorStore::_tagImpl
{
    QString dbPath;                        ///< 数据库文件路径
    QString connectionName;                ///< 唯一连接名
    QSqlDatabase database;                 ///< 数据库连接
    bool open = false;                     ///< 是否已打开

    ~_tagImpl()
    {
        Close();
    }

    /**
     * @brief 关闭数据库连接并移除连接注册
     */
    void Close()
    {
        if (!open)
        {
            return;
        }

        database.close();
        const QString connectionNameToRemove = connectionName;
        database = QSqlDatabase();
        connectionName.clear();
        open = false;
        QSqlDatabase::removeDatabase(connectionNameToRemove);
    }
};

VectorStore::VectorStore() = default;

VectorStore::~VectorStore() = default;

bool VectorStore::Open(const QString &dbPath, QString &errorMessage)
{
    if (m_impl && m_impl->open)
    {
        return true;
    }

    const QString trimmedPath = dbPath.trimmed();

    if (trimmedPath.isEmpty())
    {
        errorMessage = QStringLiteral("Vector database path is empty.");
        return false;
    }

    const QFileInfo pathInfo(trimmedPath);

    if (!QDir().mkpath(pathInfo.absolutePath()))
    {
        errorMessage = QStringLiteral("Failed to create vector database directory: %1")
                            .arg(pathInfo.absolutePath());
        return false;
    }

    QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath()
                                     + QStringLiteral("/plugins"));

    m_impl = std::make_unique<_tagImpl>();
    m_impl->dbPath = pathInfo.absoluteFilePath();
    m_impl->connectionName = QStringLiteral("vpet_vectors_%1").arg(QUuid::createUuid().toString());

    m_impl->database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                 m_impl->connectionName);
    m_impl->database.setDatabaseName(m_impl->dbPath);

    if (!m_impl->database.open())
    {
        errorMessage = QStringLiteral("Failed to open vector database: %1")
                            .arg(SqlErrorText(m_impl->database.lastError()));
        m_impl->Close();
        m_impl.reset();
        return false;
    }

    QSqlQuery createTable(m_impl->database);

    if (!createTable.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS vectors ("
            "  entry_id TEXT PRIMARY KEY,"
            "  model_id TEXT NOT NULL,"
            "  dimension INTEGER NOT NULL,"
            "  embedding BLOB NOT NULL,"
            "  updated_at INTEGER NOT NULL"
            ")")))
    {
        errorMessage = QStringLiteral("Failed to create vectors table: %1")
                            .arg(SqlErrorText(createTable.lastError()));
        m_impl->Close();
        m_impl.reset();
        return false;
    }

    QSqlQuery createIndex(m_impl->database);

    if (!createIndex.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_vectors_model ON vectors(model_id)")))
    {
        errorMessage = QStringLiteral("Failed to create vectors index: %1")
                            .arg(SqlErrorText(createIndex.lastError()));
        m_impl->Close();
        m_impl.reset();
        return false;
    }

    m_impl->open = true;
    return true;
}

bool VectorStore::IsOpen() const
{
    return (m_impl != nullptr) && m_impl->open;
}

void VectorStore::Close()
{
    if (m_impl)
    {
        m_impl->Close();
    }
}

bool VectorStore::Upsert(const QString &entryId,
                         const QString &modelId,
                         int dimension,
                         const QVector<float> &embedding,
                         QString &errorMessage)
{
    if (!IsOpen())
    {
        errorMessage = QStringLiteral("Vector database is not open.");
        return false;
    }

    if (entryId.trimmed().isEmpty() || modelId.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Vector entry id or model id is empty.");
        return false;
    }

    if ((dimension <= 0) || (embedding.size() != dimension))
    {
        errorMessage = QStringLiteral("Vector dimension mismatch: expected %1, got %2.")
                            .arg(dimension)
                            .arg(embedding.size());
        return false;
    }

    if (!IsFiniteVector(embedding))
    {
        errorMessage = QStringLiteral("Vector contains a non-finite value.");
        return false;
    }

    const QByteArray blob(reinterpret_cast<const char *>(embedding.constData()),
                          static_cast<int>(embedding.size()) * static_cast<int>(sizeof(float)));

    QSqlQuery query(m_impl->database);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO vectors (entry_id, model_id, dimension, embedding, updated_at) "
        "VALUES (?, ?, ?, ?, ?)"));
    query.addBindValue(entryId.trimmed());
    query.addBindValue(modelId.trimmed());
    query.addBindValue(dimension);
    query.addBindValue(blob);
    query.addBindValue(static_cast<qint64>(QDateTime::currentMSecsSinceEpoch()));

    if (!query.exec())
    {
        errorMessage = QStringLiteral("Failed to upsert vector: %1")
                            .arg(SqlErrorText(query.lastError()));
        return false;
    }

    return true;
}

bool VectorStore::Remove(const QString &entryId, QString &errorMessage)
{
    if (!IsOpen())
    {
        errorMessage = QStringLiteral("Vector database is not open.");
        return false;
    }

    QSqlQuery query(m_impl->database);
    query.prepare(QStringLiteral("DELETE FROM vectors WHERE entry_id = ?"));
    query.addBindValue(entryId.trimmed());

    if (!query.exec())
    {
        errorMessage = QStringLiteral("Failed to remove vector: %1")
                            .arg(SqlErrorText(query.lastError()));
        return false;
    }

    return true;
}

bool VectorStore::Get(const QString &entryId,
                      QString &modelId,
                      int &dimension,
                      QVector<float> &embedding,
                      QString &errorMessage) const
{
    if (!IsOpen())
    {
        errorMessage = QStringLiteral("Vector database is not open.");
        return false;
    }

    QSqlQuery query(m_impl->database);
    query.prepare(QStringLiteral(
        "SELECT model_id, dimension, embedding FROM vectors WHERE entry_id = ?"));
    query.addBindValue(entryId.trimmed());

    if (!query.exec())
    {
        errorMessage = QStringLiteral("Failed to read vector: %1")
                            .arg(SqlErrorText(query.lastError()));
        return false;
    }

    if (!query.next())
    {
        return false;
    }

    modelId = query.value(0).toString();
    dimension = query.value(1).toInt();
    const QByteArray blob = query.value(2).toByteArray();
    const int floatSize = static_cast<int>(sizeof(float));

    if ((dimension <= 0) || ((blob.size() % floatSize) != 0))
    {
        errorMessage = QStringLiteral("Stored vector blob has an invalid size.");
        return false;
    }

    const int floatCount = static_cast<int>(blob.size() / floatSize);

    if (floatCount != dimension)
    {
        errorMessage = QStringLiteral("Stored vector dimension does not match blob size.");
        return false;
    }

    embedding.resize(floatCount);

    if (floatCount > 0)
    {
        std::memcpy(embedding.data(), blob.constData(),
                    static_cast<size_t>(floatCount) * sizeof(float));
    }

    return true;
}

bool VectorStore::QueryTopK(const QString &modelId,
                            const QVector<float> &queryEmbedding,
                            int maxResults,
                            QVector<_tagVectorHit> &hits,
                            QString &errorMessage) const
{
    hits.clear();

    if (!IsOpen())
    {
        errorMessage = QStringLiteral("Vector database is not open.");
        return false;
    }

    if (modelId.trimmed().isEmpty() || queryEmbedding.isEmpty())
    {
        errorMessage = QStringLiteral("Vector query model or embedding is empty.");
        return false;
    }

    if (!IsFiniteVector(queryEmbedding))
    {
        errorMessage = QStringLiteral("Vector query contains a non-finite value.");
        return false;
    }

    const int queryDimension = queryEmbedding.size();
    const int expectedBytes = queryDimension * static_cast<int>(sizeof(float));

    QSqlQuery query(m_impl->database);
    query.prepare(QStringLiteral(
        "SELECT entry_id, dimension, embedding FROM vectors WHERE model_id = ?"));
    query.addBindValue(modelId.trimmed());

    if (!query.exec())
    {
        errorMessage = QStringLiteral("Failed to query vectors: %1")
                            .arg(SqlErrorText(query.lastError()));
        return false;
    }

    while (query.next())
    {
        const int storedDimension = query.value(1).toInt();
        const QByteArray blob = query.value(2).toByteArray();

        if ((storedDimension != queryDimension) || (blob.size() != expectedBytes))
        {
            continue;
        }

        QVector<float> storedEmbedding(queryDimension, 0.0f);
        std::memcpy(storedEmbedding.data(),
                    blob.constData(),
                    static_cast<size_t>(expectedBytes));

        if (!IsFiniteVector(storedEmbedding))
        {
            continue;
        }

        float score = 0.0f;

        for (int dim = 0; dim < queryDimension; ++dim)
        {
            score += storedEmbedding.at(dim) * queryEmbedding.at(dim);
        }

        _tagVectorHit hit;
        hit.entryId = query.value(0).toString();
        hit.score = score;
        hits.append(hit);
    }

    std::stable_sort(hits.begin(), hits.end(),
                     [](const _tagVectorHit &lhs, const _tagVectorHit &rhs)
    {
        if (lhs.score != rhs.score)
        {
            return lhs.score > rhs.score;
        }

        return lhs.entryId < rhs.entryId;
    });

    if ((maxResults > 0) && (hits.size() > maxResults))
    {
        hits.resize(maxResults);
    }

    return true;
}

bool VectorStore::ClearForModel(const QString &modelId, QString &errorMessage)
{
    if (!IsOpen())
    {
        errorMessage = QStringLiteral("Vector database is not open.");
        return false;
    }

    QSqlQuery query(m_impl->database);
    query.prepare(QStringLiteral("DELETE FROM vectors WHERE model_id = ?"));
    query.addBindValue(modelId.trimmed());

    if (!query.exec())
    {
        errorMessage = QStringLiteral("Failed to clear vectors: %1")
                            .arg(SqlErrorText(query.lastError()));
        return false;
    }

    return true;
}

bool VectorStore::ClearAll(QString &errorMessage)
{
    if (!IsOpen())
    {
        errorMessage = QStringLiteral("Vector database is not open.");
        return false;
    }

    QSqlQuery query(m_impl->database);

    if (!query.exec(QStringLiteral("DELETE FROM vectors")))
    {
        errorMessage = QStringLiteral("Failed to clear vector database: %1")
                           .arg(SqlErrorText(query.lastError()));
        return false;
    }

    return true;
}

int VectorStore::Count(const QString &modelId) const
{
    if (!IsOpen())
    {
        return 0;
    }

    QSqlQuery query(m_impl->database);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM vectors WHERE model_id = ?"));
    query.addBindValue(modelId.trimmed());

    if (!query.exec() || !query.next())
    {
        return 0;
    }

    return query.value(0).toInt();
}

} // namespace vpet
