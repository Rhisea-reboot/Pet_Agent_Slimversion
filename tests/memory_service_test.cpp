#include "vpet/memory/memory_service.h"
#include "vpet/memory/memory_consolidator.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

namespace
{

/**
 * @brief 等待 worker 完成队列处理（轮询 PendingCount）
 */
void WaitForDrain(vpet::MemoryService &service, int timeoutMs = 3000)
{
    QElapsedTimer timer;
    timer.start();

    while ((service.PendingCount() > 0) && (timer.elapsed() < timeoutMs))
    {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
}

/**
 * @brief 等待并消费指定请求 ID 的结果，跳过同一分区内较早的结果
 */
bool WaitForRequestResult(vpet::MemoryService &service,
                          const QString &petId,
                          const QString &triggerType,
                          quint64 requestId,
                          vpet::MemoryService::_tagRetrieveResult &result,
                          int timeoutMs = 3000)
{
    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs)
    {
        vpet::MemoryService::_tagRetrieveResult candidate;

        if (service.TakeLatestReadyResult(petId, triggerType, candidate)
            && (candidate.requestId == requestId))
        {
            result = candidate;
            return true;
        }

        QCoreApplication::processEvents();
        QThread::msleep(5);
    }

    return false;
}

/**
 * @brief 读取 graph.json 全部文本
 */
QByteArray ReadGraphFile(const QString &dataDir)
{
    QFile file(dataDir + QStringLiteral("/memory/graph.json"));

    if (!file.open(QIODevice::ReadOnly))
    {
        return QByteArray();
    }

    return file.readAll();
}

/**
 * @brief 确定性 fake 向量化器：按内容关键词映射到正交基向量
 *
 * EmbedDocument 对含 "爬山" 的文本返回 false（模拟该条目无向量），
 * 以便验证 BFS 图传播路径。
 */
class FakeEmbedder : public vpet::MemoryEmbedder
{
public:
    static constexpr int kDimension = 8;

    bool IsReady() const override
    {
        return true;
    }

    QString ModelId() const override
    {
        return QStringLiteral("fake-test-model");
    }

    int Dimension() const override
    {
        return kDimension;
    }

    bool EmbedDocument(const QString &text, QVector<float> &outVector) override
    {
        if (text.contains(QStringLiteral("爬山")))
        {
            return false;
        }

        outVector = VectorForKey(text);
        return true;
    }

    bool EmbedQuery(const QString &text, QVector<float> &outVector) override
    {
        outVector = VectorForKey(text);
        return true;
    }

private:
    static QVector<float> VectorForKey(const QString &text)
    {
        QVector<float> vector(kDimension, 0.0f);

        if (text.contains(QStringLiteral("咖啡")))
        {
            vector[0] = 1.0f;
        }
        else if (text.contains(QStringLiteral("茶")))
        {
            vector[1] = 1.0f;
        }
        else
        {
            vector[kDimension - 1] = 1.0f;
        }

        return vector;
    }
};

/**
 * @brief 永不就绪的 fake 向量化器（模拟模型缺失）
 */
class NeverReadyEmbedder : public vpet::MemoryEmbedder
{
public:
    bool IsReady() const override
    {
        return false;
    }

    QString ModelId() const override
    {
        return QStringLiteral("fake-test-model");
    }

    int Dimension() const override
    {
        return 8;
    }

    bool EmbedDocument(const QString &, QVector<float> &) override
    {
        return false;
    }

    bool EmbedQuery(const QString &, QVector<float> &) override
    {
        return false;
    }
};

} // anonymous namespace

class MemoryServiceTest : public QObject
{
    Q_OBJECT

private slots:
    void StartRejectsInvalidCapacity();
    void EnqueueFailsAfterShutdown();
    void RetrieveIsConsumedOnce();
    void NewerResultReplacesOlder();
    void MailboxIsPartitionedByPetAndTrigger();
    void ShutdownFlushesPendingStores();
    void PersistenceAcrossRestart();
    void StorePrivacyFilterRejects();
    void NonBlockingUnderLoad();
    void ForgetByKeywordRemovesMatches();
    void ListEntriesScopesAndPets();
    void CascadeRetrieveUsesVectorSeedsAndGraphExpansion();
    void CascadeFallsBackToKeywordWhenEmbedderUnavailable();
    void PromptSectionRespectsCharacterBudget();
    void ConsolidationParserRejectsUnsafeOrInvalidResponses();
    void ConsolidationStrengthensDuplicatesAndSupersedesConfirmedEntries();
    void ConsolidationKeepsConflictingEntries();
    void FeedbackIsAppliedAsynchronouslyAndPersisted();
    void ManagementOperationsHonorPetScope();
    void MissingVectorsAreBackfilledOnFirstRetrieve();
    void ExplicitSupersedesIsNotSwallowedByDuplicateStrengthening();
    void StoreForcesPetScopeAndTagsUsePrivacyFilter();
    void ServiceCanRestartSameInstance();
};

void MemoryServiceTest::StartRejectsInvalidCapacity()
{
    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(!service.Start(QStringLiteral(""), 0, errorMessage));
    QVERIFY(!service.IsRunning());
}

void MemoryServiceTest::EnqueueFailsAfterShutdown()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 4, errorMessage));

    quint64 requestId = 0;
    QVERIFY(service.TryEnqueueRetrieve(QStringLiteral("pet_a"),
                                       QStringLiteral("user"),
                                       QStringLiteral("咖啡"),
                                       requestId));

    service.Shutdown(2000);

    QVERIFY(!service.IsRunning());
    QVERIFY(!service.TryEnqueueRetrieve(QStringLiteral("pet_a"),
                                        QStringLiteral("user"),
                                        QStringLiteral("咖啡"),
                                        requestId));
}

void MemoryServiceTest::RetrieveIsConsumedOnce()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 4, errorMessage));

    quint64 requestId = 0;
    QVERIFY(service.TryEnqueueRetrieve(QStringLiteral("pet_a"),
                                       QStringLiteral("user"),
                                       QStringLiteral("咖啡"),
                                       requestId));
    WaitForDrain(service);

    QTRY_VERIFY_WITH_TIMEOUT(service.HasReadyResult(QStringLiteral("pet_a"), QStringLiteral("user")),
                             3000);

    vpet::MemoryService::_tagRetrieveResult result;
    QVERIFY(service.TakeLatestReadyResult(QStringLiteral("pet_a"), QStringLiteral("user"), result));
    QVERIFY(result.ok);
    QCOMPARE(result.query, QStringLiteral("咖啡"));
    QVERIFY(result.entries.isEmpty());

    QVERIFY(!service.TakeLatestReadyResult(QStringLiteral("pet_a"), QStringLiteral("user"), result));

    service.Shutdown(2000);
}

void MemoryServiceTest::NewerResultReplacesOlder()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 4, errorMessage));

    vpet::MemoryEntry entry = vpet::MemoryEntry();
    entry.content = QStringLiteral("用户喜欢喝咖啡");
    entry.petId = QStringLiteral("pet_a");

    quint64 storeRequestId = 0;
    QString rejectCategory;
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"),
                                    QStringLiteral("user"),
                                    entry,
                                    storeRequestId,
                                    rejectCategory));

    quint64 firstRequestId = 0;
    QVERIFY(service.TryEnqueueRetrieve(QStringLiteral("pet_a"),
                                       QStringLiteral("user"),
                                       QStringLiteral("咖啡"),
                                       firstRequestId));
    WaitForDrain(service);

    quint64 secondRequestId = 0;
    QVERIFY(service.TryEnqueueRetrieve(QStringLiteral("pet_a"),
                                       QStringLiteral("user"),
                                       QStringLiteral("咖啡"),
                                       secondRequestId));
    QVERIFY(secondRequestId > firstRequestId);
    WaitForDrain(service);

    vpet::MemoryService::_tagRetrieveResult result;
    QVERIFY(WaitForRequestResult(service,
                                 QStringLiteral("pet_a"),
                                 QStringLiteral("user"),
                                 secondRequestId,
                                 result));
    QCOMPARE(result.entries.size(), 1);
    QCOMPARE(result.entries.first().content, QStringLiteral("用户喜欢喝咖啡"));

    service.Shutdown(2000);
}

void MemoryServiceTest::MailboxIsPartitionedByPetAndTrigger()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 8, errorMessage));

    quint64 requestId = 0;
    QVERIFY(service.TryEnqueueRetrieve(QStringLiteral("pet_a"),
                                       QStringLiteral("user"),
                                       QStringLiteral("查询甲"),
                                       requestId));
    QVERIFY(service.TryEnqueueRetrieve(QStringLiteral("pet_a"),
                                       QStringLiteral("vision"),
                                       QStringLiteral("查询乙"),
                                       requestId));
    QVERIFY(service.TryEnqueueRetrieve(QStringLiteral("pet_b"),
                                       QStringLiteral("user"),
                                       QStringLiteral("查询丙"),
                                       requestId));
    WaitForDrain(service);

    QTRY_VERIFY_WITH_TIMEOUT(service.HasReadyResult(QStringLiteral("pet_a"), QStringLiteral("user"))
                                 && service.HasReadyResult(QStringLiteral("pet_a"), QStringLiteral("vision"))
                                 && service.HasReadyResult(QStringLiteral("pet_b"), QStringLiteral("user")),
                             3000);

    vpet::MemoryService::_tagRetrieveResult result;

    QVERIFY(service.TakeLatestReadyResult(QStringLiteral("pet_a"), QStringLiteral("user"), result));
    QCOMPARE(result.query, QStringLiteral("查询甲"));

    QVERIFY(service.TakeLatestReadyResult(QStringLiteral("pet_a"), QStringLiteral("vision"), result));
    QCOMPARE(result.query, QStringLiteral("查询乙"));

    QVERIFY(service.TakeLatestReadyResult(QStringLiteral("pet_b"), QStringLiteral("user"), result));
    QCOMPARE(result.query, QStringLiteral("查询丙"));

    service.Shutdown(2000);
}

void MemoryServiceTest::ShutdownFlushesPendingStores()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 4, errorMessage));

    vpet::MemoryEntry entry = vpet::MemoryEntry();
    entry.content = QStringLiteral("flush 前提交的写入");
    entry.petId = QStringLiteral("pet_a");

    quint64 requestId = 0;
    QString rejectCategory;
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"),
                                    QStringLiteral("user"),
                                    entry,
                                    requestId,
                                    rejectCategory));

    service.Shutdown(500);

    const QByteArray graphData = ReadGraphFile(directory.path());
    QVERIFY(graphData.contains("flush"));
}

void MemoryServiceTest::PersistenceAcrossRestart()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    {
        vpet::MemoryService service;
        QString errorMessage;
        QVERIFY(service.Start(directory.path(), 4, errorMessage));

        vpet::MemoryEntry entry = vpet::MemoryEntry();
        entry.content = QStringLiteral("重启后仍在的咖啡记忆");
        entry.petId = QStringLiteral("pet_a");

        quint64 requestId = 0;
        QString rejectCategory;
        QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"),
                                        QStringLiteral("user"),
                                        entry,
                                        requestId,
                                        rejectCategory));
        service.Shutdown(2000);
    }

    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 4, errorMessage));

    quint64 requestId = 0;
    QVERIFY(service.TryEnqueueRetrieve(QStringLiteral("pet_a"),
                                       QStringLiteral("user"),
                                       QStringLiteral("咖啡"),
                                       requestId));
    WaitForDrain(service);

    QTRY_VERIFY_WITH_TIMEOUT(service.HasReadyResult(QStringLiteral("pet_a"), QStringLiteral("user")),
                             3000);

    vpet::MemoryService::_tagRetrieveResult result;
    QVERIFY(service.TakeLatestReadyResult(QStringLiteral("pet_a"), QStringLiteral("user"), result));
    QCOMPARE(result.entries.size(), 1);
    QCOMPARE(result.entries.first().content, QStringLiteral("重启后仍在的咖啡记忆"));

    service.Shutdown(2000);
}

void MemoryServiceTest::StorePrivacyFilterRejects()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 4, errorMessage));

    vpet::MemoryEntry entry = vpet::MemoryEntry();
    entry.content = QStringLiteral("OPENAI_API_KEY=sk-proj-abcdefghijklmnopqrstuvwx");
    entry.petId = QStringLiteral("pet_a");

    quint64 requestId = 0;
    QString rejectCategory;
    QVERIFY(!service.TryEnqueueStore(QStringLiteral("pet_a"),
                                     QStringLiteral("user"),
                                     entry,
                                     requestId,
                                     rejectCategory));
    QCOMPARE(rejectCategory, QStringLiteral("env_file"));

    WaitForDrain(service);
    const QByteArray graphData = ReadGraphFile(directory.path());
    QVERIFY(!graphData.contains(QStringLiteral("sk-proj-abcdefghijklmnopqrstuvwx").toUtf8()));

    service.Shutdown(2000);
}

void MemoryServiceTest::NonBlockingUnderLoad()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 1, errorMessage));

    QElapsedTimer timer;
    timer.start();
    int succeeded = 0;
    int rejected = 0;

    for (int index = 0; index < 200; ++index)
    {
        quint64 requestId = 0;

        if (service.TryEnqueueRetrieve(QStringLiteral("pet_a"),
                                       QStringLiteral("user"),
                                       QStringLiteral("压力测试 %1").arg(index),
                                       requestId))
        {
            succeeded += 1;
        }
        else
        {
            rejected += 1;
        }
    }

    QVERIFY(timer.elapsed() < 1000);
    QCOMPARE(succeeded + rejected, 200);
    QVERIFY(succeeded > 0);

    WaitForDrain(service);
    QTRY_VERIFY_WITH_TIMEOUT(service.HasReadyResult(QStringLiteral("pet_a"), QStringLiteral("user")),
                             3000);

    vpet::MemoryService::_tagRetrieveResult result;
    QVERIFY(service.TakeLatestReadyResult(QStringLiteral("pet_a"), QStringLiteral("user"), result));
    QVERIFY(result.query.startsWith(QStringLiteral("压力测试")));

    service.Shutdown(2000);
}

void MemoryServiceTest::ForgetByKeywordRemovesMatches()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 4, errorMessage));

    vpet::MemoryEntry coffee = vpet::MemoryEntry();
    coffee.content = QStringLiteral("用户喜欢喝咖啡");
    coffee.petId = QStringLiteral("pet_a");

    vpet::MemoryEntry tea = vpet::MemoryEntry();
    tea.content = QStringLiteral("用户喜欢喝茶");
    tea.petId = QStringLiteral("pet_a");

    quint64 requestId = 0;
    QString rejectCategory;
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"), QStringLiteral("user"), coffee, requestId, rejectCategory));
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"), QStringLiteral("user"), tea, requestId, rejectCategory));
    WaitForDrain(service);

    QVERIFY(service.TryEnqueueForgetByKeyword(QStringLiteral("pet_a"),
                                              QStringLiteral("user"),
                                              QStringLiteral("咖啡"),
                                              requestId));
    WaitForDrain(service);

    quint64 retrieveId = 0;
    QVERIFY(service.TryEnqueueRetrieve(QStringLiteral("pet_a"),
                                       QStringLiteral("user"),
                                       QStringLiteral("咖啡"),
                                       retrieveId));
    WaitForDrain(service);

    QTRY_VERIFY_WITH_TIMEOUT(service.HasReadyResult(QStringLiteral("pet_a"), QStringLiteral("user")),
                             3000);

    vpet::MemoryService::_tagRetrieveResult result;
    QVERIFY(service.TakeLatestReadyResult(QStringLiteral("pet_a"), QStringLiteral("user"), result));
    QVERIFY(result.entries.isEmpty());

    QVERIFY(service.TryEnqueueRetrieve(QStringLiteral("pet_a"),
                                       QStringLiteral("user"),
                                       QStringLiteral("茶"),
                                       retrieveId));
    WaitForDrain(service);

    QTRY_VERIFY_WITH_TIMEOUT(service.HasReadyResult(QStringLiteral("pet_a"), QStringLiteral("user")),
                             3000);

    QVERIFY(service.TakeLatestReadyResult(QStringLiteral("pet_a"), QStringLiteral("user"), result));
    QCOMPARE(result.entries.size(), 1);
    QCOMPARE(result.entries.first().content, QStringLiteral("用户喜欢喝茶"));

    service.Shutdown(2000);
}

void MemoryServiceTest::ListEntriesScopesAndPets()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 8, errorMessage));

    vpet::MemoryEntry petEntry = vpet::MemoryEntry();
    petEntry.content = QStringLiteral("pet_a 的事实");
    petEntry.petId = QStringLiteral("pet_a");

    vpet::MemoryEntry otherPetEntry = vpet::MemoryEntry();
    otherPetEntry.content = QStringLiteral("pet_b 的事实");
    otherPetEntry.petId = QStringLiteral("pet_b");

    vpet::MemoryEntry globalEntry = vpet::MemoryEntry();
    globalEntry.content = QStringLiteral("全局事实");
    globalEntry.petId = QString();
    globalEntry.scope = vpet::MemoryEntry::Scope::Global;

    quint64 requestId = 0;
    QString rejectCategory;
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"),
                                    QStringLiteral("user"),
                                    petEntry,
                                    requestId,
                                    rejectCategory));
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_b"),
                                    QStringLiteral("user"),
                                    otherPetEntry,
                                    requestId,
                                    rejectCategory));
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"),
                                    QStringLiteral("user"),
                                    globalEntry,
                                    requestId,
                                    rejectCategory));
    WaitForDrain(service);

    QVERIFY(service.TryEnqueueList(QStringLiteral("pet_a"),
                                   QStringLiteral("list"),
                                   vpet::MemoryEntry::Scope::Pet,
                                   requestId));
    WaitForDrain(service);

    QTRY_VERIFY_WITH_TIMEOUT(
        service.HasReadyResult(QStringLiteral("pet_a"), QStringLiteral("list")), 3000);

    vpet::MemoryService::_tagRetrieveResult result;
    QVERIFY(service.TakeLatestReadyResult(QStringLiteral("pet_a"), QStringLiteral("list"), result));
    QVERIFY(result.ok);
    QCOMPARE(result.entries.size(), 2);

    bool hasPetA = false;
    bool hasGlobal = false;

    for (const vpet::MemoryEntry &entry : result.entries)
    {
        hasPetA = hasPetA || (entry.content == QStringLiteral("pet_a 的事实"));
        hasGlobal = hasGlobal || (entry.content == QStringLiteral("全局事实"));
    }

    QVERIFY(hasPetA);
    QVERIFY(hasGlobal);

    QVERIFY(service.TryEnqueueList(QStringLiteral("pet_a"),
                                   QStringLiteral("list"),
                                   vpet::MemoryEntry::Scope::Global,
                                   requestId));
    WaitForDrain(service);

    QTRY_VERIFY_WITH_TIMEOUT(
        service.HasReadyResult(QStringLiteral("pet_a"), QStringLiteral("list")), 3000);

    QVERIFY(service.TakeLatestReadyResult(QStringLiteral("pet_a"), QStringLiteral("list"), result));
    QCOMPARE(result.entries.size(), 1);
    QCOMPARE(result.entries.first().content, QStringLiteral("全局事实"));

    service.Shutdown(2000);
}

void MemoryServiceTest::CascadeRetrieveUsesVectorSeedsAndGraphExpansion()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryService service;
    QString errorMessage;
    service.InstallEmbedderForTest(std::make_unique<FakeEmbedder>());
    QVERIFY(service.Start(directory.path(), 8, errorMessage));

    vpet::MemoryEntry coffee = vpet::MemoryEntry();
    coffee.content = QStringLiteral("用户喜欢喝咖啡");
    coffee.petId = QStringLiteral("pet_a");

    vpet::MemoryEntry tea = vpet::MemoryEntry();
    tea.content = QStringLiteral("用户喜欢喝茶");
    tea.petId = QStringLiteral("pet_a");

    vpet::MemoryEntry hiking = vpet::MemoryEntry();
    hiking.content = QStringLiteral("用户周末喜欢爬山");
    hiking.petId = QStringLiteral("pet_a");

    vpet::MemoryEntry unrelated = vpet::MemoryEntry();
    unrelated.content = QStringLiteral("用户的电脑重置了");
    unrelated.petId = QStringLiteral("pet_a");

    quint64 requestId = 0;
    QString rejectCategory;
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"), QStringLiteral("user"),
                                    coffee, requestId, rejectCategory));
    const QString coffeeId = QStringLiteral("mem_%1").arg(requestId);
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"), QStringLiteral("user"),
                                    tea, requestId, rejectCategory));
    const QString teaId = QStringLiteral("mem_%1").arg(requestId);
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"), QStringLiteral("user"),
                                    hiking, requestId, rejectCategory));
    const QString hikingId = QStringLiteral("mem_%1").arg(requestId);
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"), QStringLiteral("user"),
                                    unrelated, requestId, rejectCategory));

    QVERIFY(service.TryEnqueueTag(QStringLiteral("pet_a"), QStringLiteral("user"),
                                  coffeeId,
                                  { QStringLiteral("喜好") }, requestId));
    QVERIFY(service.TryEnqueueTag(QStringLiteral("pet_a"), QStringLiteral("user"),
                                  teaId,
                                  { QStringLiteral("喜好") }, requestId));
    QVERIFY(service.TryEnqueueTag(QStringLiteral("pet_a"), QStringLiteral("user"),
                                  hikingId,
                                  { QStringLiteral("喜好") }, requestId));
    WaitForDrain(service);

    QVERIFY(service.TryEnqueueRetrieve(QStringLiteral("pet_a"),
                                       QStringLiteral("user"),
                                       QStringLiteral("咖啡"),
                                       requestId));
    WaitForDrain(service);

    QTRY_VERIFY_WITH_TIMEOUT(service.HasReadyResult(QStringLiteral("pet_a"), QStringLiteral("user")),
                             3000);

    vpet::MemoryService::_tagRetrieveResult result;
    QVERIFY(service.TakeLatestReadyResult(QStringLiteral("pet_a"), QStringLiteral("user"), result));
    QVERIFY(result.ok);
    QVERIFY(!result.entries.isEmpty());

    // 向量种子命中咖啡（score 1.0，应排第一）
    QCOMPARE(result.entries.first().content, QStringLiteral("用户喜欢喝咖啡"));

    // 爬山（hikingId）无向量，仅经 TagShared 边从咖啡 BFS 传播命中
    bool hasHiking = false;

    for (const vpet::MemoryEntry &entry : result.entries)
    {
        hasHiking = hasHiking || (entry.content == QStringLiteral("用户周末喜欢爬山"));
    }

    QVERIFY(hasHiking);

    // 未连接的无向量条目不得出现
    bool hasUnrelated = false;

    for (const vpet::MemoryEntry &entry : result.entries)
    {
        hasUnrelated = hasUnrelated || (entry.content == QStringLiteral("用户的电脑重置了"));
    }

    QVERIFY(!hasUnrelated);

    service.Shutdown(2000);
}

void MemoryServiceTest::CascadeFallsBackToKeywordWhenEmbedderUnavailable()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryService service;
    QString errorMessage;
    service.InstallEmbedderForTest(std::make_unique<NeverReadyEmbedder>());
    QVERIFY(service.Start(directory.path(), 4, errorMessage));

    vpet::MemoryEntry coffee = vpet::MemoryEntry();
    coffee.content = QStringLiteral("用户喜欢喝咖啡");
    coffee.petId = QStringLiteral("pet_a");

    quint64 requestId = 0;
    QString rejectCategory;
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"), QStringLiteral("user"),
                                    coffee, requestId, rejectCategory));
    WaitForDrain(service);

    QVERIFY(service.TryEnqueueRetrieve(QStringLiteral("pet_a"),
                                       QStringLiteral("user"),
                                       QStringLiteral("咖啡"),
                                       requestId));
    WaitForDrain(service);

    QTRY_VERIFY_WITH_TIMEOUT(service.HasReadyResult(QStringLiteral("pet_a"), QStringLiteral("user")),
                             3000);

    vpet::MemoryService::_tagRetrieveResult result;
    QVERIFY(service.TakeLatestReadyResult(QStringLiteral("pet_a"), QStringLiteral("user"), result));
    QVERIFY(result.ok);
    QCOMPARE(result.entries.size(), 1);
    QCOMPARE(result.entries.first().content, QStringLiteral("用户喜欢喝咖啡"));

    service.Shutdown(2000);
}

void MemoryServiceTest::PromptSectionRespectsCharacterBudget()
{
    vpet::MemoryEntry oversized;
    oversized.content = QString(200, QLatin1Char('x'));

    vpet::MemoryEntry fitting;
    fitting.content = QStringLiteral("保留这一条");

    const QString prompt = vpet::MemoryService::BuildPromptSection(
        { oversized, fitting }, 2, 32);

    QVERIFY(prompt.size() <= 32);
    QVERIFY(prompt.contains(fitting.content));
    QVERIFY(!prompt.contains(oversized.content));
}

void MemoryServiceTest::ConsolidationParserRejectsUnsafeOrInvalidResponses()
{
    const QVector<QString> knownIds = { QStringLiteral("old_preference") };
    QVector<vpet::_tagMemoryConsolidationCandidate> candidates;
    QString errorCategory;
    const QString validResponse = QString::fromUtf8(
        R"({"candidates":[{"content":"用户喜欢无糖咖啡","type":"preference","scope":"pet","tags":["饮食"],"confidence":0.92,"relation":"supersedes","related_memory_id":"old_preference"}]})");

    QVERIFY(vpet::MemoryConsolidator::ParseCandidates(validResponse,
                                                       QStringLiteral("pet_a"),
                                                       knownIds,
                                                       4,
                                                       candidates,
                                                       errorCategory));
    QCOMPARE(candidates.size(), 1);
    QCOMPARE(candidates.first().entry.provenance,
             vpet::MemoryEntry::Provenance::Extracted);
    QCOMPARE(candidates.first().relation,
             vpet::MEMORY_CONSOLIDATION_RELATION::SUPERSEDES);

    const QString unsafeResponse = QString::fromUtf8(
        R"({"candidates":[{"content":"API_KEY=secret-value-12345","type":"fact","scope":"pet","tags":[],"confidence":0.9,"relation":"none","related_memory_id":""}]})");

    QVERIFY(!vpet::MemoryConsolidator::ParseCandidates(unsafeResponse,
                                                        QStringLiteral("pet_a"),
                                                        knownIds,
                                                        4,
                                                        candidates,
                                                        errorCategory));
    QCOMPARE(errorCategory, QStringLiteral("credential"));

    const QString invalidRelationResponse = QString::fromUtf8(
        R"({"candidates":[{"content":"用户喜欢茶","type":"preference","scope":"pet","tags":[],"confidence":0.9,"relation":"supersedes","related_memory_id":"unknown"}]})");

    QVERIFY(!vpet::MemoryConsolidator::ParseCandidates(invalidRelationResponse,
                                                        QStringLiteral("pet_a"),
                                                        knownIds,
                                                        4,
                                                        candidates,
                                                        errorCategory));
    QCOMPARE(errorCategory, QStringLiteral("invalid_relation_target"));
}

void MemoryServiceTest::ConsolidationStrengthensDuplicatesAndSupersedesConfirmedEntries()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 8, errorMessage));

    vpet::MemoryEntry existingEntry;
    existingEntry.id = QStringLiteral("old_preference");
    existingEntry.content = QStringLiteral("用户喜欢喝咖啡");
    existingEntry.petId = QStringLiteral("pet_a");
    existingEntry.type = vpet::MemoryEntry::Type::Preference;

    quint64 requestId = 0;
    QString rejectCategory;
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"),
                                    QStringLiteral("user"),
                                    existingEntry,
                                    requestId,
                                    rejectCategory));
    WaitForDrain(service);

    vpet::_tagMemoryConsolidationCandidate duplicateCandidate;
    duplicateCandidate.entry.content = QStringLiteral("用户喜欢喝咖啡");
    duplicateCandidate.entry.petId = QStringLiteral("pet_a");
    duplicateCandidate.entry.type = vpet::MemoryEntry::Type::Preference;
    duplicateCandidate.entry.provenance = vpet::MemoryEntry::Provenance::Extracted;
    duplicateCandidate.entry.confidence = 0.8f;

    QVERIFY(service.TryEnqueueConsolidation(QStringLiteral("pet_a"),
                                            QStringLiteral("user"),
                                            { duplicateCandidate },
                                            requestId));
    WaitForDrain(service);

    QVERIFY(service.TryEnqueueList(QStringLiteral("pet_a"),
                                   QStringLiteral("list"),
                                   vpet::MemoryEntry::Scope::Pet,
                                   requestId));
    vpet::MemoryService::_tagRetrieveResult result;
    QVERIFY(WaitForRequestResult(service,
                                 QStringLiteral("pet_a"),
                                 QStringLiteral("list"),
                                 requestId,
                                 result));
    QCOMPARE(result.entries.size(), 1);
    QCOMPARE(result.entries.first().strength, static_cast<quint32>(1));

    vpet::_tagMemoryConsolidationCandidate replacementCandidate;
    replacementCandidate.entry.content = QStringLiteral("用户现在喜欢喝茶");
    replacementCandidate.entry.petId = QStringLiteral("pet_a");
    replacementCandidate.entry.type = vpet::MemoryEntry::Type::Preference;
    replacementCandidate.entry.provenance = vpet::MemoryEntry::Provenance::Extracted;
    replacementCandidate.relation = vpet::MEMORY_CONSOLIDATION_RELATION::SUPERSEDES;
    replacementCandidate.relatedMemoryId = QStringLiteral("old_preference");

    QVERIFY(service.TryEnqueueConsolidation(QStringLiteral("pet_a"),
                                            QStringLiteral("user"),
                                            { replacementCandidate },
                                            requestId));
    WaitForDrain(service);

    QVERIFY(service.TryEnqueueList(QStringLiteral("pet_a"),
                                   QStringLiteral("list"),
                                   vpet::MemoryEntry::Scope::Pet,
                                   requestId));
    QVERIFY(WaitForRequestResult(service,
                                 QStringLiteral("pet_a"),
                                 QStringLiteral("list"),
                                 requestId,
                                 result));
    QCOMPARE(result.entries.size(), 1);
    QCOMPARE(result.entries.first().content, QStringLiteral("用户现在喜欢喝茶"));
    QCOMPARE(result.entries.first().provenance,
             vpet::MemoryEntry::Provenance::Extracted);

    const QByteArray graphData = ReadGraphFile(directory.path());
    QVERIFY(graphData.contains("supersedes"));
    QVERIFY(graphData.contains("extracted"));

    service.Shutdown(2000);
}

void MemoryServiceTest::ConsolidationKeepsConflictingEntries()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 8, errorMessage));

    vpet::MemoryEntry existingEntry;
    existingEntry.id = QStringLiteral("work_location");
    existingEntry.content = QStringLiteral("用户在北京办公");
    existingEntry.petId = QStringLiteral("pet_a");

    quint64 requestId = 0;
    QString rejectCategory;
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"),
                                    QStringLiteral("user"),
                                    existingEntry,
                                    requestId,
                                    rejectCategory));
    WaitForDrain(service);

    vpet::_tagMemoryConsolidationCandidate conflictCandidate;
    conflictCandidate.entry.content = QStringLiteral("用户也许在上海办公");
    conflictCandidate.entry.petId = QStringLiteral("pet_a");
    conflictCandidate.relation = vpet::MEMORY_CONSOLIDATION_RELATION::CONFLICTS;
    conflictCandidate.relatedMemoryId = QStringLiteral("work_location");

    QVERIFY(service.TryEnqueueConsolidation(QStringLiteral("pet_a"),
                                            QStringLiteral("user"),
                                            { conflictCandidate },
                                            requestId));
    WaitForDrain(service);

    QVERIFY(service.TryEnqueueList(QStringLiteral("pet_a"),
                                   QStringLiteral("list"),
                                   vpet::MemoryEntry::Scope::Pet,
                                   requestId));
    vpet::MemoryService::_tagRetrieveResult result;
    QVERIFY(WaitForRequestResult(service,
                                 QStringLiteral("pet_a"),
                                 QStringLiteral("list"),
                                 requestId,
                                 result));
    QCOMPARE(result.entries.size(), 2);

    const QByteArray graphData = ReadGraphFile(directory.path());
    QVERIFY(graphData.contains("conflict"));

    service.Shutdown(2000);
}

void MemoryServiceTest::FeedbackIsAppliedAsynchronouslyAndPersisted()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 8, errorMessage));

    vpet::MemoryEntry entry;
    entry.id = QStringLiteral("feedback_entry");
    entry.content = QStringLiteral("反馈测试记忆");
    entry.petId = QStringLiteral("pet_a");
    entry.confidence = 0.5f;

    quint64 requestId = 0;
    QString rejectCategory;
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"),
                                    QStringLiteral("user"),
                                    entry,
                                    requestId,
                                    rejectCategory));
    WaitForDrain(service);

    QVERIFY(service.TryEnqueueFeedback(QStringLiteral("pet_a"),
                                       QStringLiteral("user"),
                                       { QStringLiteral("feedback_entry") },
                                       true,
                                       requestId));
    WaitForDrain(service);

    QVERIFY(service.TryEnqueueList(QStringLiteral("pet_a"),
                                   QStringLiteral("list"),
                                   vpet::MemoryEntry::Scope::Pet,
                                   requestId));
    vpet::MemoryService::_tagRetrieveResult result;
    QVERIFY(WaitForRequestResult(service,
                                 QStringLiteral("pet_a"),
                                 QStringLiteral("list"),
                                 requestId,
                                 result));
    QCOMPARE(result.entries.size(), 1);
    QCOMPARE(result.entries.first().strength, 1u);
    QVERIFY(qFuzzyCompare(result.entries.first().confidence, 0.55f));

    service.Shutdown(2000);

    vpet::MemoryService restartedService;
    QVERIFY(restartedService.Start(directory.path(), 8, errorMessage));
    QVERIFY(restartedService.TryEnqueueList(QStringLiteral("pet_a"),
                                            QStringLiteral("list"),
                                            vpet::MemoryEntry::Scope::Pet,
                                            requestId));
    QVERIFY(WaitForRequestResult(restartedService,
                                 QStringLiteral("pet_a"),
                                 QStringLiteral("list"),
                                 requestId,
                                 result));
    QCOMPARE(result.entries.first().strength, 1u);
    QVERIFY(qFuzzyCompare(result.entries.first().confidence, 0.55f));
    restartedService.Shutdown(2000);
}

void MemoryServiceTest::ManagementOperationsHonorPetScope()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 8, errorMessage));

    vpet::MemoryEntry entry;
    entry.id = QStringLiteral("pet_a_only");
    entry.content = QStringLiteral("只属于 pet_a 的记忆");
    entry.petId = QStringLiteral("pet_a");
    quint64 requestId = 0;
    QString rejectCategory;
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"),
                                    QStringLiteral("user"),
                                    entry,
                                    requestId,
                                    rejectCategory));
    WaitForDrain(service);

    QVERIFY(service.TryEnqueueTag(QStringLiteral("pet_b"),
                                  QStringLiteral("memory.management"),
                                  entry.id,
                                  { QStringLiteral("forbidden") },
                                  requestId));
    QVERIFY(service.TryEnqueueForget(QStringLiteral("pet_b"),
                                     QStringLiteral("memory.management"),
                                     entry.id,
                                     requestId));
    WaitForDrain(service);

    QVERIFY(service.TryEnqueueList(QStringLiteral("pet_a"),
                                   QStringLiteral("list"),
                                   vpet::MemoryEntry::Scope::Pet,
                                   requestId));
    vpet::MemoryService::_tagRetrieveResult result;
    QVERIFY(WaitForRequestResult(service,
                                 QStringLiteral("pet_a"),
                                 QStringLiteral("list"),
                                 requestId,
                                 result));
    QCOMPARE(result.entries.size(), 1);
    QVERIFY(!result.entries.first().tags.contains(QStringLiteral("forbidden")));
    service.Shutdown(2000);
}

void MemoryServiceTest::MissingVectorsAreBackfilledOnFirstRetrieve()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    vpet::MemoryRepository repository;
    QString errorMessage;
    QVERIFY(repository.SetDataDir(directory.path(), errorMessage));
    vpet::MemoryGraph graph;
    vpet::MemoryEntry entry;
    entry.id = QStringLiteral("legacy_without_vector");
    entry.content = QStringLiteral("用户喜欢喝咖啡");
    entry.petId = QStringLiteral("pet_a");
    QVERIFY(graph.AddEntry(entry, errorMessage));
    QVERIFY(repository.Save(graph, errorMessage));

    vpet::MemoryService service;
    service.InstallEmbedderForTest(std::make_unique<FakeEmbedder>());
    QVERIFY(service.Start(directory.path(), 8, errorMessage));
    quint64 requestId = 0;
    QVERIFY(service.TryEnqueueRetrieve(QStringLiteral("pet_a"),
                                       QStringLiteral("user"),
                                       QStringLiteral("咖啡"),
                                       requestId));
    vpet::MemoryService::_tagRetrieveResult result;
    QVERIFY(WaitForRequestResult(service,
                                 QStringLiteral("pet_a"),
                                 QStringLiteral("user"),
                                 requestId,
                                 result));
    QCOMPARE(result.entries.size(), 1);
    service.Shutdown(2000);

    vpet::VectorStore vectorStore;
    QVERIFY(vectorStore.Open(directory.filePath(QStringLiteral("memory/vectors.sqlite3")),
                             errorMessage));
    QCOMPARE(vectorStore.Count(QStringLiteral("fake-test-model")), 1);
}

void MemoryServiceTest::ExplicitSupersedesIsNotSwallowedByDuplicateStrengthening()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 8, errorMessage));
    vpet::MemoryEntry existing;
    existing.id = QStringLiteral("old_preference");
    existing.content = QStringLiteral("用户喜欢喝咖啡");
    existing.type = vpet::MemoryEntry::Type::Preference;
    existing.petId = QStringLiteral("pet_a");
    quint64 requestId = 0;
    QString rejectCategory;
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"),
                                    QStringLiteral("user"),
                                    existing,
                                    requestId,
                                    rejectCategory));
    WaitForDrain(service);

    vpet::_tagMemoryConsolidationCandidate replacement;
    replacement.entry.content = QStringLiteral("用户喜欢喝咖啡");
    replacement.entry.type = vpet::MemoryEntry::Type::Preference;
    replacement.entry.petId = QStringLiteral("pet_a");
    replacement.relation = vpet::MEMORY_CONSOLIDATION_RELATION::SUPERSEDES;
    replacement.relatedMemoryId = existing.id;
    QVERIFY(service.TryEnqueueConsolidation(QStringLiteral("pet_a"),
                                            QStringLiteral("user"),
                                            { replacement },
                                            requestId));
    WaitForDrain(service);
    QVERIFY(service.TryEnqueueList(QStringLiteral("pet_a"),
                                   QStringLiteral("list"),
                                   vpet::MemoryEntry::Scope::Pet,
                                   requestId));
    vpet::MemoryService::_tagRetrieveResult result;
    QVERIFY(WaitForRequestResult(service,
                                 QStringLiteral("pet_a"),
                                 QStringLiteral("list"),
                                 requestId,
                                 result));
    QCOMPARE(result.entries.size(), 1);
    QVERIFY(result.entries.first().id != existing.id);
    QVERIFY(ReadGraphFile(directory.path()).contains("supersedes"));
    service.Shutdown(2000);
}

void MemoryServiceTest::StoreForcesPetScopeAndTagsUsePrivacyFilter()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 8, errorMessage));
    vpet::MemoryEntry entry;
    entry.id = QStringLiteral("scope_entry");
    entry.content = QStringLiteral("作用域测试");
    entry.petId = QStringLiteral("pet_b");
    quint64 requestId = 0;
    QString rejectCategory;
    QVERIFY(service.TryEnqueueStore(QStringLiteral("pet_a"),
                                    QStringLiteral("user"),
                                    entry,
                                    requestId,
                                    rejectCategory));
    WaitForDrain(service);
    QVERIFY(!service.TryEnqueueTag(QStringLiteral("pet_a"),
                                   QStringLiteral("memory.management"),
                                   entry.id,
                                   { QStringLiteral("password=supersecret123") },
                                   requestId));
    QVERIFY(service.TryEnqueueList(QStringLiteral("pet_a"),
                                   QStringLiteral("list"),
                                   vpet::MemoryEntry::Scope::Pet,
                                   requestId));
    vpet::MemoryService::_tagRetrieveResult result;
    QVERIFY(WaitForRequestResult(service,
                                 QStringLiteral("pet_a"),
                                 QStringLiteral("list"),
                                 requestId,
                                 result));
    QCOMPARE(result.entries.size(), 1);
    QCOMPARE(result.entries.first().petId, QStringLiteral("pet_a"));
    QVERIFY(result.entries.first().tags.isEmpty());
    service.Shutdown(2000);
}

void MemoryServiceTest::ServiceCanRestartSameInstance()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 4, errorMessage));
    service.Shutdown(2000);
    QVERIFY(service.Start(directory.path(), 4, errorMessage));
    quint64 requestId = 0;
    QVERIFY(service.TryEnqueueList(QStringLiteral("pet_a"),
                                   QStringLiteral("list"),
                                   vpet::MemoryEntry::Scope::Pet,
                                   requestId));
    vpet::MemoryService::_tagRetrieveResult result;
    QVERIFY(WaitForRequestResult(service,
                                 QStringLiteral("pet_a"),
                                 QStringLiteral("list"),
                                 requestId,
                                 result));
    service.Shutdown(2000);
}

QTEST_MAIN(MemoryServiceTest)
#include "memory_service_test.moc"
