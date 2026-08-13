#include "vpet/memory/embedding_client.h"
#include "vpet/memory/vector_store.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

namespace
{

/**
 * @brief 构造一个 L2 归一化的单位向量
 */
QVector<float> MakeUnitVector(int dimension, float firstComponent)
{
    QVector<float> vector(dimension, 0.0f);
    vector[0] = firstComponent;
    vector[dimension - 1] = static_cast<float>(std::sqrt(1.0 - firstComponent * firstComponent));
    return vector;
}

} // anonymous namespace

class MeanPoolTest : public QObject
{
    Q_OBJECT

private slots:
    void NormalizeUnitVector();
    void MeanPoolingWithMask();
    void AllMaskedRejected();
    void LengthMismatchRejected();
};

void MeanPoolTest::NormalizeUnitVector()
{
    QVector<float> result;
    const float hiddenState[] = { 3.0f, 4.0f };
    const int64_t mask[] = { 1, 1 };

    QVERIFY(vpet::EmbeddingClient::MeanPoolAndNormalize(hiddenState, mask, 2, 1, result));
    QCOMPARE(result.size(), 1);
    QVERIFY(qAbs(result.at(0) - 1.0f) < 1e-5f);
}

void MeanPoolTest::MeanPoolingWithMask()
{
    // hiddenDim = 2，序列长 3；第 3 个 token 被 mask 掉
    const float hiddenState[] = {
        1.0f, 0.0f,
        0.0f, 1.0f,
        9.0f, 9.0f
    };
    const int64_t mask[] = { 1, 1, 0 };

    QVector<float> result;
    QVERIFY(vpet::EmbeddingClient::MeanPoolAndNormalize(hiddenState, mask, 3, 2, result));
    QCOMPARE(result.size(), 2);

    const float length = static_cast<float>(std::sqrt(0.5 * 0.5 + 0.5 * 0.5));
    QVERIFY(qAbs(result.at(0) - 0.5f / length) < 1e-5f);
    QVERIFY(qAbs(result.at(1) - 0.5f / length) < 1e-5f);
}

void MeanPoolTest::AllMaskedRejected()
{
    QVector<float> result;
    const float hiddenState[] = { 1.0f, 2.0f };
    const int64_t mask[] = { 0, 0 };

    QVERIFY(!vpet::EmbeddingClient::MeanPoolAndNormalize(hiddenState, mask, 2, 1, result));
    QVERIFY(result.isEmpty());
}

void MeanPoolTest::LengthMismatchRejected()
{
    QVector<float> result;
    const float hiddenState[] = { 1.0f, 2.0f };
    const int64_t mask[] = { 1 };

    QVERIFY(!vpet::EmbeddingClient::MeanPoolAndNormalize(hiddenState, mask, 2, 1, result));
    QVERIFY(result.isEmpty());
}

class VectorStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void OpenCreatesDatabase();
    void UpsertAndGetRoundTrip();
    void QueryTopKOrdersByCosine();
    void DimensionMismatchRowIgnored();
    void RemoveDeletesVector();
    void ClearForModelIsScoped();
    void OpenRejectsEmptyPath();
    void UpsertRejectsDimensionMismatch();
};

void VectorStoreTest::OpenCreatesDatabase()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::VectorStore store;
    QString errorMessage;
    QVERIFY(store.Open(directory.path() + QStringLiteral("/vectors.sqlite3"), errorMessage));
    QVERIFY(store.IsOpen());
    QVERIFY(QFileInfo::exists(directory.path() + QStringLiteral("/vectors.sqlite3")));

    store.Close();
    QVERIFY(!store.IsOpen());
}

void VectorStoreTest::UpsertAndGetRoundTrip()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::VectorStore store;
    QString errorMessage;
    QVERIFY(store.Open(directory.path() + QStringLiteral("/vectors.sqlite3"), errorMessage));

    const QVector<float> embedding = MakeUnitVector(4, 0.6f);
    QVERIFY(store.Upsert(QStringLiteral("mem_1"),
                         QStringLiteral("test-model"),
                         4,
                         embedding,
                         errorMessage));

    QString modelId;
    int dimension = 0;
    QVector<float> loaded;
    QVERIFY(store.Get(QStringLiteral("mem_1"), modelId, dimension, loaded, errorMessage));
    QCOMPARE(modelId, QStringLiteral("test-model"));
    QCOMPARE(dimension, 4);
    QCOMPARE(loaded, embedding);

    store.Close();
}

void VectorStoreTest::QueryTopKOrdersByCosine()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::VectorStore store;
    QString errorMessage;
    QVERIFY(store.Open(directory.path() + QStringLiteral("/vectors.sqlite3"), errorMessage));

    // 查询向量指向 [0]=1 方向
    const QVector<float> query = MakeUnitVector(4, 1.0f);
    QVERIFY(store.Upsert(QStringLiteral("close"), QStringLiteral("test-model"), 4,
                         MakeUnitVector(4, 0.95f), errorMessage));
    QVERIFY(store.Upsert(QStringLiteral("far"), QStringLiteral("test-model"), 4,
                         MakeUnitVector(4, 0.1f), errorMessage));
    QVERIFY(store.Upsert(QStringLiteral("closer"), QStringLiteral("test-model"), 4,
                         MakeUnitVector(4, 0.99f), errorMessage));

    QVector<vpet::VectorStore::_tagVectorHit> hits;
    QVERIFY(store.QueryTopK(QStringLiteral("test-model"), query, 2, hits, errorMessage));
    QCOMPARE(hits.size(), 2);
    QCOMPARE(hits.at(0).entryId, QStringLiteral("closer"));
    QCOMPARE(hits.at(1).entryId, QStringLiteral("close"));
    QVERIFY(hits.at(0).score > hits.at(1).score);

    // 不同模型不可见
    QVector<vpet::VectorStore::_tagVectorHit> otherHits;
    QVERIFY(store.QueryTopK(QStringLiteral("other-model"), query, 10, otherHits, errorMessage));
    QVERIFY(otherHits.isEmpty());

    store.Close();
}

void VectorStoreTest::DimensionMismatchRowIgnored()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::VectorStore store;
    QString errorMessage;
    QVERIFY(store.Open(directory.path() + QStringLiteral("/vectors.sqlite3"), errorMessage));

    QVERIFY(store.Upsert(QStringLiteral("mem_1"), QStringLiteral("test-model"), 4,
                         MakeUnitVector(4, 0.9f), errorMessage));
    QVERIFY(store.Upsert(QStringLiteral("mem_2"), QStringLiteral("test-model"), 8,
                         MakeUnitVector(8, 0.9f), errorMessage));

    QVector<vpet::VectorStore::_tagVectorHit> hits;
    const QVector<float> query = MakeUnitVector(4, 1.0f);
    QVERIFY(store.QueryTopK(QStringLiteral("test-model"), query, 10, hits, errorMessage));
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.at(0).entryId, QStringLiteral("mem_1"));

    store.Close();
}

void VectorStoreTest::RemoveDeletesVector()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::VectorStore store;
    QString errorMessage;
    QVERIFY(store.Open(directory.path() + QStringLiteral("/vectors.sqlite3"), errorMessage));

    QVERIFY(store.Upsert(QStringLiteral("mem_1"), QStringLiteral("test-model"), 4,
                         MakeUnitVector(4, 0.9f), errorMessage));
    QVERIFY(store.Remove(QStringLiteral("mem_1"), errorMessage));
    QCOMPARE(store.Count(QStringLiteral("test-model")), 0);

    // 删除不存在的条目也视为成功
    QVERIFY(store.Remove(QStringLiteral("ghost"), errorMessage));

    store.Close();
}

void VectorStoreTest::ClearForModelIsScoped()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::VectorStore store;
    QString errorMessage;
    QVERIFY(store.Open(directory.path() + QStringLiteral("/vectors.sqlite3"), errorMessage));

    QVERIFY(store.Upsert(QStringLiteral("a"), QStringLiteral("model-a"), 4,
                         MakeUnitVector(4, 0.9f), errorMessage));
    QVERIFY(store.Upsert(QStringLiteral("b"), QStringLiteral("model-b"), 4,
                         MakeUnitVector(4, 0.9f), errorMessage));

    QVERIFY(store.ClearForModel(QStringLiteral("model-a"), errorMessage));
    QCOMPARE(store.Count(QStringLiteral("model-a")), 0);
    QCOMPARE(store.Count(QStringLiteral("model-b")), 1);

    store.Close();
}

void VectorStoreTest::OpenRejectsEmptyPath()
{
    vpet::VectorStore store;
    QString errorMessage;
    QVERIFY(!store.Open(QString(), errorMessage));
    QVERIFY(!store.IsOpen());
}

void VectorStoreTest::UpsertRejectsDimensionMismatch()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::VectorStore store;
    QString errorMessage;
    QVERIFY(store.Open(directory.path() + QStringLiteral("/vectors.sqlite3"), errorMessage));

    QVERIFY(!store.Upsert(QStringLiteral("mem_1"), QStringLiteral("test-model"), 4,
                          MakeUnitVector(3, 0.9f), errorMessage));

    store.Close();
}

class EmbeddingClientFallbackTest : public QObject
{
    Q_OBJECT

private slots:
    void MissingModelFileNotReady();
    void MissingTokenizerNotReady();
};

void EmbeddingClientFallbackTest::MissingModelFileNotReady()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::EmbeddingConfig config;
    config.enabled = true;
    config.backend = QStringLiteral("local_onnx");
    config.modelDir = directory.path();
    config.onnxModel = QStringLiteral("missing.onnx");
    config.tokenizerFile = QStringLiteral("missing_tokenizer.json");

    QStringList logs;
    vpet::EmbeddingClient client(config,
                                 [&logs](const QString &message) { logs.append(message); });
    QVERIFY(!client.IsReady());
    QCOMPARE(client.Dimension(), 0);
    QVERIFY(!client.Reload());
    QVERIFY(!client.LastError().isEmpty());
    QVERIFY(!logs.isEmpty());
}

void EmbeddingClientFallbackTest::MissingTokenizerNotReady()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::EmbeddingConfig config;
    config.enabled = true;
    config.backend = QStringLiteral("local_onnx");
    config.modelDir = directory.path();
    config.onnxModel = QStringLiteral("model.onnx");
    config.tokenizerFile = QStringLiteral("missing_tokenizer.json");

    vpet::EmbeddingClient client(config);
    QVERIFY(!client.IsReady());
    QVERIFY(!client.Reload());
    QVERIFY(!client.LastError().isEmpty());
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    int status = 0;

    {
        MeanPoolTest meanPoolTest;
        status |= QTest::qExec(&meanPoolTest, argc, argv);
    }

    {
        VectorStoreTest vectorStoreTest;
        status |= QTest::qExec(&vectorStoreTest, argc, argv);
    }

    {
        EmbeddingClientFallbackTest fallbackTest;
        status |= QTest::qExec(&fallbackTest, argc, argv);
    }

    return status;
}

#include "memory_embedding_test.moc"
