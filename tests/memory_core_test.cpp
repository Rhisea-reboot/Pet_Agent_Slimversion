#include "vpet/memory/memory_graph.h"
#include "vpet/memory/memory_maintenance.h"
#include "vpet/memory/memory_repository.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

namespace
{

/**
 * @brief 构造测试条目
 */
vpet::MemoryEntry MakeEntry(const QString &id,
                            const QString &content,
                            const QString &petId = QStringLiteral("pet_a"),
                            vpet::MemoryEntry::Scope scope = vpet::MemoryEntry::Scope::Pet)
{
    vpet::MemoryEntry entry;
    entry.id = id;
    entry.content = content;
    entry.petId = petId;
    entry.scope = scope;
    entry.createdAt = QDateTime::currentMSecsSinceEpoch();
    entry.updatedAt = entry.createdAt;
    return entry;
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

} // anonymous namespace

class MemoryGraphTest : public QObject
{
    Q_OBJECT

private slots:
    void AddAndGetEntry();
    void DuplicateIdRejected();
    void EmptyContentRejected();
    void SoftDeleteExcludesFromSearch();
    void TagMaintenanceKeepsIndexConsistent();
    void ScopeFiltering();
    void ChineseKeywordSearch();
    void SearchSortingIsStable();
    void UpdateEntryRefreshesIndex();
    void AddEdgeAndQuery();
    void DuplicateEdgeIdempotent();
    void EdgeRejectsMissingEndpoint();
    void EdgeRejectsInvalidWeight();
    void RemoveEdgeDeletesFromAdjacency();
    void TagSharedEdgesSyncWithTags();
    void TagSharedEdgesRemovedOnTagRemove();
    void ExpandByEdgesPropagatesWithDecay();
    void ExpandByEdgesHonorsScope();
    void ExpandByEdgesSkipsSeedsAndInactive();
    void LoadEdgesRejectsInvalid();
    void TriggerPatternsMatchNegativeAndProcedure();
};

class MemoryMaintenanceTest : public QObject
{
    Q_OBJECT

private slots:
    void ConfidenceDecayUsesHalfLife();
    void FeedbackAdjustsConfidenceAndStrength();
    void RetrievalDiscoversLinksAndInfersTags();
    void EmptyRetrievalStoresOnlyQueryHash();
    void DeepConsolidationMergesDuplicatesAndPrunesWeakEntries();
    void DeepConsolidationPreservesConflicts();
};

void MemoryGraphTest::AddAndGetEntry()
{
    vpet::MemoryGraph graph;
    QString errorMessage;
    const vpet::MemoryEntry entry = MakeEntry(QStringLiteral("m1"), QStringLiteral("用户喜欢喝咖啡"));

    QVERIFY(graph.AddEntry(entry, errorMessage));
    QCOMPARE(graph.EntryCount(), 1);
    QCOMPARE(graph.ActiveEntryCount(), 1);

    vpet::MemoryEntry loaded;
    QVERIFY(graph.GetEntry(QStringLiteral("m1"), loaded));
    QCOMPARE(loaded.content, QStringLiteral("用户喜欢喝咖啡"));
    QCOMPARE(loaded.petId, QStringLiteral("pet_a"));
}

void MemoryGraphTest::DuplicateIdRejected()
{
    vpet::MemoryGraph graph;
    QString errorMessage;
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("m1"), QStringLiteral("内容一")), errorMessage));
    QVERIFY(!graph.AddEntry(MakeEntry(QStringLiteral("m1"), QStringLiteral("内容二")), errorMessage));
    QCOMPARE(graph.EntryCount(), 1);
}

void MemoryGraphTest::EmptyContentRejected()
{
    vpet::MemoryGraph graph;
    QString errorMessage;
    QVERIFY(!graph.AddEntry(MakeEntry(QStringLiteral("m1"), QStringLiteral("   ")), errorMessage));
    QCOMPARE(graph.EntryCount(), 0);
}

void MemoryGraphTest::SoftDeleteExcludesFromSearch()
{
    vpet::MemoryGraph graph;
    QString errorMessage;
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("m1"), QStringLiteral("用户喜欢喝咖啡")), errorMessage));

    QVERIFY(graph.SoftDeleteEntry(QStringLiteral("m1")));
    QCOMPARE(graph.ActiveEntryCount(), 0);
    QCOMPARE(graph.EntryCount(), 1);

    const auto results = graph.SearchByKeywords(QStringLiteral("咖啡"),
                                                vpet::MemoryEntry::Scope::Pet,
                                                QStringLiteral("pet_a"),
                                                10);
    QVERIFY(results.isEmpty());
    QVERIFY(graph.ListEntries(vpet::MemoryEntry::Scope::Pet, QStringLiteral("pet_a")).isEmpty());
}

void MemoryGraphTest::TagMaintenanceKeepsIndexConsistent()
{
    vpet::MemoryGraph graph;
    QString errorMessage;
    const vpet::MemoryEntry entry = MakeEntry(QStringLiteral("m1"), QStringLiteral("用户喜欢喝咖啡"));
    QVERIFY(graph.AddEntry(entry, errorMessage));

    QVERIFY(graph.AddTag(QStringLiteral("m1"), QStringLiteral("咖啡"), errorMessage));

    auto tagged = graph.SearchByTags({ QStringLiteral("咖啡") },
                                     vpet::MemoryEntry::Scope::Pet,
                                     QStringLiteral("pet_a"),
                                     10);
    QCOMPARE(tagged.size(), 1);
    QCOMPARE(tagged.first().id, QStringLiteral("m1"));

    QVERIFY(graph.RemoveTag(QStringLiteral("m1"), QStringLiteral("咖啡"), errorMessage));
    tagged = graph.SearchByTags({ QStringLiteral("咖啡") },
                                vpet::MemoryEntry::Scope::Pet,
                                QStringLiteral("pet_a"),
                                10);
    QVERIFY(tagged.isEmpty());
    QVERIFY(graph.AllTags().isEmpty());
}

void MemoryGraphTest::ScopeFiltering()
{
    vpet::MemoryGraph graph;
    QString errorMessage;

    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("m1"), QStringLiteral("pet_a 的事实"), QStringLiteral("pet_a")), errorMessage));
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("m2"), QStringLiteral("pet_b 的事实"), QStringLiteral("pet_b")), errorMessage));
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("m3"),
                                     QStringLiteral("全局事实"),
                                     QString(),
                                     vpet::MemoryEntry::Scope::Global),
                           errorMessage));

    const auto petAList = graph.ListEntries(vpet::MemoryEntry::Scope::Pet, QStringLiteral("pet_a"));
    QCOMPARE(petAList.size(), 2);

    bool hasPetA = false;
    bool hasGlobal = false;

    for (const vpet::MemoryEntry &entry : petAList)
    {
        hasPetA = hasPetA || (entry.id == QStringLiteral("m1"));
        hasGlobal = hasGlobal || (entry.id == QStringLiteral("m3"));
    }

    QVERIFY(hasPetA);
    QVERIFY(hasGlobal);

    const auto petBList = graph.ListEntries(vpet::MemoryEntry::Scope::Pet, QStringLiteral("pet_b"));
    QCOMPARE(petBList.size(), 2);
}

void MemoryGraphTest::ChineseKeywordSearch()
{
    vpet::MemoryGraph graph;
    QString errorMessage;
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("m1"), QStringLiteral("用户喜欢喝咖啡")), errorMessage));
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("m2"), QStringLiteral("用户周末喜欢爬山")), errorMessage));

    const auto results = graph.SearchByKeywords(QStringLiteral("咖啡"),
                                                vpet::MemoryEntry::Scope::Pet,
                                                QStringLiteral("pet_a"),
                                                10);
    QCOMPARE(results.size(), 1);
    QCOMPARE(results.first().id, QStringLiteral("m1"));

    const auto multiResults = graph.SearchByKeywords(QStringLiteral("喜欢"),
                                                     vpet::MemoryEntry::Scope::Pet,
                                                     QStringLiteral("pet_a"),
                                                     10);
    QCOMPARE(multiResults.size(), 2);
}

void MemoryGraphTest::SearchSortingIsStable()
{
    vpet::MemoryGraph graph;
    QString errorMessage;

    vpet::MemoryEntry first = MakeEntry(QStringLiteral("a"), QStringLiteral("用户喜欢喝咖啡"));
    vpet::MemoryEntry second = MakeEntry(QStringLiteral("b"), QStringLiteral("用户喜欢喝咖啡"));

    QVERIFY(graph.AddEntry(first, errorMessage));
    QVERIFY(graph.AddEntry(second, errorMessage));

    const auto results = graph.SearchByKeywords(QStringLiteral("咖啡"),
                                                vpet::MemoryEntry::Scope::Pet,
                                                QStringLiteral("pet_a"),
                                                10);
    QCOMPARE(results.size(), 2);
    QCOMPARE(results.at(0).id, QStringLiteral("a"));
    QCOMPARE(results.at(1).id, QStringLiteral("b"));
}

void MemoryGraphTest::UpdateEntryRefreshesIndex()
{
    vpet::MemoryGraph graph;
    QString errorMessage;
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("m1"), QStringLiteral("用户喜欢喝咖啡")), errorMessage));

    vpet::MemoryEntry updated = MakeEntry(QStringLiteral("m1"), QStringLiteral("用户喜欢喝茶"));
    QVERIFY(graph.UpdateEntry(updated, errorMessage));

    const auto coffeeResults = graph.SearchByKeywords(QStringLiteral("咖啡"),
                                                      vpet::MemoryEntry::Scope::Pet,
                                                      QStringLiteral("pet_a"),
                                                      10);
    QVERIFY(coffeeResults.isEmpty());

    const auto teaResults = graph.SearchByKeywords(QStringLiteral("茶"),
                                                   vpet::MemoryEntry::Scope::Pet,
                                                   QStringLiteral("pet_a"),
                                                   10);
    QCOMPARE(teaResults.size(), 1);
}

void MemoryGraphTest::AddEdgeAndQuery()
{
    vpet::MemoryGraph graph;
    QString errorMessage;

    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("a"), QStringLiteral("记忆甲")), errorMessage));
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("b"), QStringLiteral("记忆乙")), errorMessage));
    QVERIFY(graph.AddEdge(QStringLiteral("a"),
                          QStringLiteral("b"),
                          vpet::MemoryEdge::Type::Explicit,
                          QString(),
                          1.0f,
                          errorMessage));

    QCOMPARE(graph.AllEdges().size(), 1);
    QCOMPARE(graph.AllEdges().first().sourceId, QStringLiteral("a"));
    QCOMPARE(graph.AllEdges().first().targetId, QStringLiteral("b"));
}

void MemoryGraphTest::DuplicateEdgeIdempotent()
{
    vpet::MemoryGraph graph;
    QString errorMessage;

    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("a"), QStringLiteral("记忆甲")), errorMessage));
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("b"), QStringLiteral("记忆乙")), errorMessage));

    QVERIFY(graph.AddEdge(QStringLiteral("a"),
                          QStringLiteral("b"),
                          vpet::MemoryEdge::Type::Explicit,
                          QString(),
                          1.0f,
                          errorMessage));
    QVERIFY(graph.AddEdge(QStringLiteral("b"),
                          QStringLiteral("a"),
                          vpet::MemoryEdge::Type::Explicit,
                          QString(),
                          1.0f,
                          errorMessage));
    QCOMPARE(graph.AllEdges().size(), 1);
}

void MemoryGraphTest::EdgeRejectsMissingEndpoint()
{
    vpet::MemoryGraph graph;
    QString errorMessage;

    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("a"), QStringLiteral("记忆甲")), errorMessage));
    QVERIFY(!graph.AddEdge(QStringLiteral("a"),
                           QStringLiteral("ghost"),
                           vpet::MemoryEdge::Type::Explicit,
                           QString(),
                           1.0f,
                           errorMessage));
    QVERIFY(!graph.AddEdge(QStringLiteral("ghost"),
                           QStringLiteral("a"),
                           vpet::MemoryEdge::Type::Explicit,
                           QString(),
                           1.0f,
                           errorMessage));
    QVERIFY(!graph.AddEdge(QStringLiteral("ghost"),
                           QStringLiteral("ghost2"),
                           vpet::MemoryEdge::Type::Explicit,
                           QString(),
                           1.0f,
                           errorMessage));
    QCOMPARE(graph.AllEdges().size(), 0);
}

void MemoryGraphTest::EdgeRejectsInvalidWeight()
{
    vpet::MemoryGraph graph;
    QString errorMessage;

    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("a"), QStringLiteral("记忆甲")), errorMessage));
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("b"), QStringLiteral("记忆乙")), errorMessage));

    QVERIFY(!graph.AddEdge(QStringLiteral("a"),
                           QStringLiteral("b"),
                           vpet::MemoryEdge::Type::Explicit,
                           QString(),
                           0.0f,
                           errorMessage));
    QCOMPARE(graph.AllEdges().size(), 0);
}

void MemoryGraphTest::RemoveEdgeDeletesFromAdjacency()
{
    vpet::MemoryGraph graph;
    QString errorMessage;

    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("a"), QStringLiteral("记忆甲")), errorMessage));
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("b"), QStringLiteral("记忆乙")), errorMessage));
    QVERIFY(graph.AddEdge(QStringLiteral("a"),
                          QStringLiteral("b"),
                          vpet::MemoryEdge::Type::Explicit,
                          QString(),
                          1.0f,
                          errorMessage));

    const QString edgeId = graph.AllEdges().first().id;
    QVERIFY(graph.RemoveEdge(edgeId, errorMessage));
    QCOMPARE(graph.AllEdges().size(), 0);

    QHash<QString, float> seeds;
    seeds.insert(QStringLiteral("a"), 1.0f);
    QVERIFY(graph.ExpandByEdges(seeds,
                                vpet::MemoryEntry::Scope::Pet,
                                QStringLiteral("pet_a"),
                                2,
                                0.5f).isEmpty());

    QVERIFY(!graph.RemoveEdge(edgeId, errorMessage));
}

void MemoryGraphTest::TagSharedEdgesSyncWithTags()
{
    vpet::MemoryGraph graph;
    QString errorMessage;

    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("a"), QStringLiteral("记忆甲")), errorMessage));
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("b"), QStringLiteral("记忆乙")), errorMessage));
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("c"), QStringLiteral("记忆丙")), errorMessage));

    QVERIFY(graph.AddTag(QStringLiteral("a"), QStringLiteral("咖啡"), errorMessage));
    QVERIFY(graph.AddTag(QStringLiteral("b"), QStringLiteral("咖啡"), errorMessage));
    QCOMPARE(graph.AllEdges().size(), 1);

    QVERIFY(graph.AddTag(QStringLiteral("c"), QStringLiteral("咖啡"), errorMessage));
    QCOMPARE(graph.AllEdges().size(), 3);

    for (const vpet::MemoryEdge &edge : graph.AllEdges())
    {
        QCOMPARE(edge.type, vpet::MemoryEdge::Type::TagShared);
        QCOMPARE(edge.tag, QStringLiteral("咖啡"));
    }
}

void MemoryGraphTest::TagSharedEdgesRemovedOnTagRemove()
{
    vpet::MemoryGraph graph;
    QString errorMessage;

    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("a"), QStringLiteral("记忆甲")), errorMessage));
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("b"), QStringLiteral("记忆乙")), errorMessage));

    QVERIFY(graph.AddTag(QStringLiteral("a"), QStringLiteral("咖啡"), errorMessage));
    QVERIFY(graph.AddTag(QStringLiteral("b"), QStringLiteral("咖啡"), errorMessage));
    QCOMPARE(graph.AllEdges().size(), 1);

    QVERIFY(graph.RemoveTag(QStringLiteral("a"), QStringLiteral("咖啡"), errorMessage));
    QCOMPARE(graph.AllEdges().size(), 0);
}

void MemoryGraphTest::ExpandByEdgesPropagatesWithDecay()
{
    vpet::MemoryGraph graph;
    QString errorMessage;

    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("a"), QStringLiteral("记忆甲")), errorMessage));
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("b"), QStringLiteral("记忆乙")), errorMessage));
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("c"), QStringLiteral("记忆丙")), errorMessage));

    QVERIFY(graph.AddEdge(QStringLiteral("a"),
                          QStringLiteral("b"),
                          vpet::MemoryEdge::Type::Explicit,
                          QString(),
                          0.5f,
                          errorMessage));
    QVERIFY(graph.AddEdge(QStringLiteral("b"),
                          QStringLiteral("c"),
                          vpet::MemoryEdge::Type::Explicit,
                          QString(),
                          1.0f,
                          errorMessage));

    QHash<QString, float> seeds;
    seeds.insert(QStringLiteral("a"), 1.0f);

    const auto hits = graph.ExpandByEdges(seeds,
                                          vpet::MemoryEntry::Scope::Pet,
                                          QStringLiteral("pet_a"),
                                          2,
                                          0.5f);
    QCOMPARE(hits.size(), 2);

    // b = 1.0 * 0.5 * 0.5 = 0.25；c = 0.25 * 1.0 * 0.5 = 0.125
    QVERIFY(hits.at(0).entry.id == QStringLiteral("b"));
    QCOMPARE(hits.at(0).score, 0.25f);
    QCOMPARE(hits.at(1).entry.id, QStringLiteral("c"));
    QCOMPARE(hits.at(1).score, 0.125f);
}

void MemoryGraphTest::ExpandByEdgesHonorsScope()
{
    vpet::MemoryGraph graph;
    QString errorMessage;

    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("a"), QStringLiteral("记忆甲"), QStringLiteral("pet_a")), errorMessage));
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("b"), QStringLiteral("记忆乙"), QStringLiteral("pet_b")), errorMessage));
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("g"),
                                     QStringLiteral("全局记忆"),
                                     QString(),
                                     vpet::MemoryEntry::Scope::Global),
                           errorMessage));

    QVERIFY(graph.AddEdge(QStringLiteral("a"),
                          QStringLiteral("b"),
                          vpet::MemoryEdge::Type::Explicit,
                          QString(),
                          1.0f,
                          errorMessage));
    QVERIFY(graph.AddEdge(QStringLiteral("a"),
                          QStringLiteral("g"),
                          vpet::MemoryEdge::Type::Explicit,
                          QString(),
                          1.0f,
                          errorMessage));

    QHash<QString, float> seeds;
    seeds.insert(QStringLiteral("a"), 1.0f);

    const auto hits = graph.ExpandByEdges(seeds,
                                          vpet::MemoryEntry::Scope::Pet,
                                          QStringLiteral("pet_a"),
                                          2,
                                          0.5f);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.at(0).entry.id, QStringLiteral("g"));
}

void MemoryGraphTest::ExpandByEdgesSkipsSeedsAndInactive()
{
    vpet::MemoryGraph graph;
    QString errorMessage;

    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("a"), QStringLiteral("记忆甲")), errorMessage));
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("b"), QStringLiteral("记忆乙")), errorMessage));

    QVERIFY(graph.AddEdge(QStringLiteral("a"),
                          QStringLiteral("b"),
                          vpet::MemoryEdge::Type::Explicit,
                          QString(),
                          1.0f,
                          errorMessage));

    QHash<QString, float> seeds;
    seeds.insert(QStringLiteral("a"), 1.0f);
    QCOMPARE(graph.ExpandByEdges(seeds,
                                 vpet::MemoryEntry::Scope::Pet,
                                 QStringLiteral("pet_a"),
                                 2,
                                 0.5f).size(), 1);

    QVERIFY(graph.SoftDeleteEntry(QStringLiteral("b")));
    QVERIFY(graph.ExpandByEdges(seeds,
                                vpet::MemoryEntry::Scope::Pet,
                                QStringLiteral("pet_a"),
                                2,
                                0.5f).isEmpty());
}

void MemoryGraphTest::LoadEdgesRejectsInvalid()
{
    vpet::MemoryGraph graph;
    QString errorMessage;

    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("a"), QStringLiteral("记忆甲")), errorMessage));
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("b"), QStringLiteral("记忆乙")), errorMessage));

    vpet::MemoryEdge good;
    good.id = QStringLiteral("a|b|explicit|");
    good.sourceId = QStringLiteral("a");
    good.targetId = QStringLiteral("b");
    good.type = vpet::MemoryEdge::Type::Explicit;
    good.weight = 1.0f;
    good.active = true;

    vpet::MemoryEdge dangling;
    dangling.id = QStringLiteral("a|ghost|explicit|");
    dangling.sourceId = QStringLiteral("a");
    dangling.targetId = QStringLiteral("ghost");
    dangling.type = vpet::MemoryEdge::Type::Explicit;
    dangling.weight = 1.0f;
    dangling.active = true;

    vpet::MemoryEdge zeroWeight;
    zeroWeight.id = QStringLiteral("a|b|explicit|x");
    zeroWeight.sourceId = QStringLiteral("a");
    zeroWeight.targetId = QStringLiteral("b");
    zeroWeight.type = vpet::MemoryEdge::Type::Explicit;
    zeroWeight.weight = 0.0f;
    zeroWeight.active = true;

    QVERIFY(!graph.LoadEdges({ good, dangling, zeroWeight }, errorMessage));
    QVERIFY(!errorMessage.isEmpty());
}

void MemoryGraphTest::TriggerPatternsMatchNegativeAndProcedure()
{
    vpet::MemoryGraph graph;
    QString errorMessage;
    vpet::MemoryEntry negative = MakeEntry(QStringLiteral("negative"),
                                           QStringLiteral("不要在开会时打扰我"));
    negative.type = vpet::MemoryEntry::Type::Negative;
    negative.triggerPatterns = { QStringLiteral("开会"), QStringLiteral("忙") };
    vpet::MemoryEntry procedure = MakeEntry(QStringLiteral("procedure"),
                                             QStringLiteral("整理会议纪要"));
    procedure.type = vpet::MemoryEntry::Type::Procedure;
    procedure.procedure.name = QStringLiteral("会议纪要流程");
    procedure.procedure.trigger = QStringLiteral("会议结束");
    procedure.procedure.steps = { QStringLiteral("整理要点"), QStringLiteral("发送摘要") };

    QVERIFY(graph.AddEntry(negative, errorMessage));
    QVERIFY(graph.AddEntry(procedure, errorMessage));

    const QVector<vpet::MemoryEntry> meetingHits = graph.MatchTriggeredEntries(
        QStringLiteral("我正在开会"),
        vpet::MemoryEntry::Scope::Pet,
        QStringLiteral("pet_a"),
        4);
    QCOMPARE(meetingHits.size(), 1);
    QCOMPARE(meetingHits.first().id, QStringLiteral("negative"));

    const QVector<vpet::MemoryEntry> procedureHits = graph.MatchTriggeredEntries(
        QStringLiteral("会议结束了"),
        vpet::MemoryEntry::Scope::Pet,
        QStringLiteral("pet_a"),
        4);
    QCOMPARE(procedureHits.size(), 1);
    QCOMPARE(procedureHits.first().id, QStringLiteral("procedure"));
}

void MemoryMaintenanceTest::ConfidenceDecayUsesHalfLife()
{
    vpet::MemoryGraph graph;
    QString errorMessage;
    vpet::MemoryEntry entry = MakeEntry(QStringLiteral("m1"), QStringLiteral("事实"));
    entry.confidence = 1.0f;
    entry.confidenceUpdatedAt = 1000;
    QVERIFY(graph.AddEntry(entry, errorMessage));

    vpet::MemoryMaintenance maintenance;
    vpet::_tagMemoryMaintenanceConfig config;
    config.decayIntervalHours = 1;
    QVERIFY(maintenance.SetConfig(config, errorMessage));
    QVERIFY(maintenance.ApplyConfidenceDecay(graph, 1000 + (30LL * 24LL * 60LL * 60LL * 1000LL)));

    vpet::MemoryEntry decayed;
    QVERIFY(graph.GetEntry(QStringLiteral("m1"), decayed));
    QVERIFY(decayed.confidence < 0.51f);
    QVERIFY(decayed.confidence > 0.49f);
}

void MemoryMaintenanceTest::FeedbackAdjustsConfidenceAndStrength()
{
    vpet::MemoryGraph graph;
    QString errorMessage;
    vpet::MemoryEntry entry = MakeEntry(QStringLiteral("m1"), QStringLiteral("偏好"));
    entry.confidence = 0.5f;
    QVERIFY(graph.AddEntry(entry, errorMessage));

    vpet::MemoryMaintenance maintenance;
    QVERIFY(maintenance.ApplyFeedback(graph,
                                      { QStringLiteral("m1") },
                                      QStringLiteral("pet_a"),
                                      true,
                                      1000));

    vpet::MemoryEntry updated;
    QVERIFY(graph.GetEntry(QStringLiteral("m1"), updated));
    QCOMPARE(updated.strength, 1u);
    QVERIFY(qFuzzyCompare(updated.confidence, 0.55f));

    QVERIFY(maintenance.ApplyFeedback(graph,
                                      { QStringLiteral("m1") },
                                      QStringLiteral("pet_a"),
                                      false,
                                      2000));
    QVERIFY(graph.GetEntry(QStringLiteral("m1"), updated));
    QVERIFY(qFuzzyCompare(updated.confidence, 0.45f));
}

void MemoryMaintenanceTest::RetrievalDiscoversLinksAndInfersTags()
{
    vpet::MemoryGraph graph;
    QString errorMessage;
    vpet::MemoryEntry first = MakeEntry(QStringLiteral("m1"), QStringLiteral("甲"));
    first.tags = { QStringLiteral("共同") };
    vpet::MemoryEntry second = MakeEntry(QStringLiteral("m2"), QStringLiteral("乙"));
    second.tags = { QStringLiteral("共同") };
    vpet::MemoryEntry target = MakeEntry(QStringLiteral("m3"), QStringLiteral("丙"));
    QVERIFY(graph.AddEntry(first, errorMessage));
    QVERIFY(graph.AddEntry(second, errorMessage));
    QVERIFY(graph.AddEntry(target, errorMessage));

    vpet::MemoryMaintenance maintenance;
    vpet::_tagMemoryMaintenanceConfig config;
    config.clusterUpdateRetrievals = 1;
    QVERIFY(maintenance.SetConfig(config, errorMessage));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(maintenance.SetMemoryDir(directory.path(), errorMessage));
    QStringList removedIds;
    QVERIFY(maintenance.OnRetrieved(graph,
                                   { first, second, target },
                                   QStringLiteral("查询"),
                                   QStringLiteral("pet_a"),
                                   QStringLiteral("user"),
                                   1000,
                                   removedIds,
                                   errorMessage));

    bool hasRelatedEdge = false;

    for (const vpet::MemoryEdge &edge : graph.AllEdges())
    {
        hasRelatedEdge = hasRelatedEdge
                         || (edge.type == vpet::MemoryEdge::Type::Related);
    }

    QVERIFY(hasRelatedEdge);
    QVERIFY(graph.GetEntry(QStringLiteral("m3"), target));
    QVERIFY(target.tags.contains(QStringLiteral("共同")));
}

void MemoryMaintenanceTest::EmptyRetrievalStoresOnlyQueryHash()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryMaintenance maintenance;
    QString errorMessage;
    QVERIFY(maintenance.SetMemoryDir(directory.path(), errorMessage));
    vpet::MemoryGraph graph;
    QStringList removedIds;
    QVERIFY(!maintenance.OnRetrieved(graph,
                                    {},
                                    QStringLiteral("用户的私密查询"),
                                    QStringLiteral("pet_a"),
                                    QStringLiteral("user"),
                                    1000,
                                    removedIds,
                                    errorMessage));

    QFile gapFile(QDir(directory.path()).filePath(QStringLiteral("gaps.json")));
    QVERIFY(gapFile.open(QIODevice::ReadOnly));
    const QByteArray data = gapFile.readAll();
    QVERIFY(data.contains("queryHash"));
    QVERIFY(!data.contains("用户的私密查询"));
}

void MemoryMaintenanceTest::DeepConsolidationMergesDuplicatesAndPrunesWeakEntries()
{
    vpet::MemoryGraph graph;
    QString errorMessage;
    vpet::MemoryEntry first = MakeEntry(QStringLiteral("first"),
                                        QStringLiteral("用户喜欢喝咖啡"));
    first.tags = { QStringLiteral("饮品") };
    first.strength = 2;
    vpet::MemoryEntry duplicate = MakeEntry(QStringLiteral("duplicate"),
                                            QStringLiteral("用户喜欢喝咖啡"));
    duplicate.tags = { QStringLiteral("偏好") };
    vpet::MemoryEntry weak = MakeEntry(QStringLiteral("weak"),
                                       QStringLiteral("低置信度推断"));
    weak.confidence = 0.01f;
    weak.strength = 1;
    QVERIFY(graph.AddEntry(first, errorMessage));
    QVERIFY(graph.AddEntry(duplicate, errorMessage));
    QVERIFY(graph.AddEntry(weak, errorMessage));

    vpet::MemoryMaintenance maintenance;
    QStringList removedIds;
    QVERIFY(maintenance.RunDeepConsolidation(graph, 5000, removedIds));
    QVERIFY(removedIds.contains(QStringLiteral("duplicate")));
    QVERIFY(removedIds.contains(QStringLiteral("weak")));

    vpet::MemoryEntry survivor;
    QVERIFY(graph.GetEntry(QStringLiteral("first"), survivor));
    QVERIFY(survivor.active);
    QVERIFY(survivor.tags.contains(QStringLiteral("饮品")));
    QVERIFY(survivor.tags.contains(QStringLiteral("偏好")));
    QVERIFY(survivor.strength >= 3u);

    vpet::MemoryEntry removed;
    QVERIFY(graph.GetEntry(QStringLiteral("duplicate"), removed));
    QVERIFY(!removed.active);
    QVERIFY(graph.GetEntry(QStringLiteral("weak"), removed));
    QVERIFY(!removed.active);

    bool hasSupersedes = false;

    for (const vpet::MemoryEdge &edge : graph.AllEdges())
    {
        hasSupersedes = hasSupersedes || (edge.type == vpet::MemoryEdge::Type::Supersedes);
    }

    QVERIFY(hasSupersedes);
}

void MemoryMaintenanceTest::DeepConsolidationPreservesConflicts()
{
    vpet::MemoryGraph graph;
    QString errorMessage;
    vpet::MemoryEntry first = MakeEntry(QStringLiteral("first"),
                                        QStringLiteral("用户在北京办公"));
    vpet::MemoryEntry second = MakeEntry(QStringLiteral("second"),
                                         QStringLiteral("用户在北京办公"));
    first.confidence = 0.01f;
    second.confidence = 0.01f;
    QVERIFY(graph.AddEntry(first, errorMessage));
    QVERIFY(graph.AddEntry(second, errorMessage));
    QVERIFY(graph.AddEdge(first.id,
                          second.id,
                          vpet::MemoryEdge::Type::Conflict,
                          QString(),
                          1.0f,
                          errorMessage));

    vpet::MemoryMaintenance maintenance;
    QStringList removedIds;
    QVERIFY(!maintenance.RunDeepConsolidation(graph, 5000, removedIds));
    QVERIFY(removedIds.isEmpty());

    QVERIFY(graph.GetEntry(first.id, first));
    QVERIFY(graph.GetEntry(second.id, second));
    QVERIFY(first.active);
    QVERIFY(second.active);
}

class MemoryRepositoryTest : public QObject
{
    Q_OBJECT

private slots:
    void RoundTrip();
    void UnknownFieldsTolerated();
    void MissingFileLoadsEmpty();
    void CorruptJsonRecoveredWithBackup();
    void UnsupportedSchemaVersionRecovered();
    void SaveFailureKeepsReturningFalse();
    void PrivacyFilterRejectsCredentials();
    void PrivacyFilterAllowsNormalContent();
    void RoundTripWithEdges();
    void V1GraphLoadsWithoutEdges();
    void ProcedureAndTriggerFieldsRoundTrip();
    void ExportAndImportRoundTrip();
    void InvalidContainersRecoverWithBackup();
    void StructuredFieldsUsePrivacyFilter();
    void EmptyTriggerRegexIsRejected();
};

void MemoryRepositoryTest::RoundTrip()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryRepository repository;
    QString errorMessage;
    QVERIFY(repository.SetDataDir(directory.path(), errorMessage));

    vpet::MemoryGraph graph;
    vpet::MemoryEntry entry;
    entry.id = QStringLiteral("mem_1");
    entry.content = QStringLiteral("用户喜欢喝咖啡");
    entry.petId = QStringLiteral("pet_a");
    entry.type = vpet::MemoryEntry::Type::Preference;
    entry.tags = { QStringLiteral("咖啡"), QStringLiteral("喜好") };
    entry.createdAt = 1234567890;
    entry.updatedAt = 1234567890;
    entry.lastAccessed = 1234567899;
    entry.confidenceUpdatedAt = 1234567900;
    entry.accessCount = 3;
    entry.strength = 2;
    entry.confidence = 0.9f;
    entry.trustScore = 1.0f;

    QVERIFY2(graph.AddEntry(entry, errorMessage), qPrintable(errorMessage));
    QVERIFY2(repository.Save(graph, errorMessage), qPrintable(errorMessage));

    vpet::MemoryGraph reloaded;
    const auto loadResult = repository.Load(reloaded);
    QVERIFY(loadResult.ok);
    QCOMPARE(reloaded.ActiveEntryCount(), 1);

    vpet::MemoryEntry loaded;
    QVERIFY(reloaded.GetEntry(QStringLiteral("mem_1"), loaded));
    QCOMPARE(loaded.content, QStringLiteral("用户喜欢喝咖啡"));
    QCOMPARE(loaded.petId, QStringLiteral("pet_a"));
    QCOMPARE(loaded.type, vpet::MemoryEntry::Type::Preference);
    QCOMPARE(loaded.tags, QStringList({ QStringLiteral("咖啡"), QStringLiteral("喜好") }));
    QCOMPARE(loaded.createdAt, static_cast<qint64>(1234567890));
    QCOMPARE(loaded.accessCount, 3u);
    QCOMPARE(loaded.confidenceUpdatedAt, static_cast<qint64>(1234567900));
    QCOMPARE(loaded.confidence, 0.9f);
}

void MemoryRepositoryTest::ProcedureAndTriggerFieldsRoundTrip()
{
    vpet::MemoryGraph graph;
    QString errorMessage;
    vpet::MemoryEntry entry = MakeEntry(QStringLiteral("procedure"),
                                        QStringLiteral("执行发布流程"));
    entry.type = vpet::MemoryEntry::Type::Procedure;
    entry.triggerPatterns = { QStringLiteral("发布") };
    entry.procedure.name = QStringLiteral("发布");
    entry.procedure.trigger = QStringLiteral("准备发布");
    entry.procedure.steps = { QStringLiteral("运行测试"), QStringLiteral("生成包") };
    entry.procedure.prerequisites = { QStringLiteral("工作区干净") };
    entry.procedure.warnings = { QStringLiteral("不得包含密钥") };
    QVERIFY(graph.AddEntry(entry, errorMessage));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    vpet::MemoryRepository repository;
    QVERIFY(repository.SetDataDir(directory.path(), errorMessage));
    QVERIFY(repository.Save(graph, errorMessage));

    vpet::MemoryGraph loadedGraph;
    QVERIFY(repository.Load(loadedGraph).ok);
    vpet::MemoryEntry loadedEntry;
    QVERIFY(loadedGraph.GetEntry(QStringLiteral("procedure"), loadedEntry));
    QCOMPARE(loadedEntry.type, vpet::MemoryEntry::Type::Procedure);
    QCOMPARE(loadedEntry.triggerPatterns, QStringList({ QStringLiteral("发布") }));
    QCOMPARE(loadedEntry.procedure.steps, QStringList({ QStringLiteral("运行测试"),
                                                         QStringLiteral("生成包") }));
    QCOMPARE(loadedEntry.procedure.warnings, QStringList({ QStringLiteral("不得包含密钥") }));
}

void MemoryRepositoryTest::ExportAndImportRoundTrip()
{
    vpet::MemoryGraph graph;
    QString errorMessage;
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("m1"), QStringLiteral("可导出的记忆")),
                           errorMessage));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    vpet::MemoryRepository repository;
    QVERIFY(repository.SetDataDir(directory.path(), errorMessage));
    const QString exportPath = QDir(directory.path()).filePath(QStringLiteral("export.json"));
    QVERIFY(repository.Export(graph, exportPath, errorMessage));

    vpet::MemoryGraph importedGraph;
    QVERIFY(repository.Import(exportPath, importedGraph, errorMessage));
    QCOMPARE(importedGraph.EntryCount(), 1);
    vpet::MemoryEntry importedEntry;
    QVERIFY(importedGraph.GetEntry(QStringLiteral("m1"), importedEntry));
    QCOMPARE(importedEntry.content, QStringLiteral("可导出的记忆"));
}

void MemoryRepositoryTest::InvalidContainersRecoverWithBackup()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    vpet::MemoryRepository repository;
    QString errorMessage;
    QVERIFY(repository.SetDataDir(directory.path(), errorMessage));
    QFile graphFile(repository.GraphFilePath());
    QVERIFY(graphFile.open(QIODevice::WriteOnly));
    const QByteArray data = QByteArrayLiteral(
        R"({"schemaVersion":5,"entries":{},"edges":[],"updatedAt":1})");
    QCOMPARE(graphFile.write(data), static_cast<qint64>(data.size()));
    graphFile.close();

    vpet::MemoryGraph graph;
    const vpet::MemoryRepository::_tagLoadResult result = repository.Load(graph);
    QVERIFY(!result.ok);
    QVERIFY(result.recovered);
    QVERIFY(!result.backupPath.isEmpty());
    QVERIFY(QFileInfo::exists(result.backupPath));
    QCOMPARE(graph.EntryCount(), 0);
}

void MemoryRepositoryTest::StructuredFieldsUsePrivacyFilter()
{
    vpet::MemoryEntry entry;
    entry.id = QStringLiteral("procedure");
    entry.content = QStringLiteral("部署流程");
    entry.type = vpet::MemoryEntry::Type::Procedure;
    entry.procedure.steps = { QStringLiteral("使用 password=supersecret123") };
    QString errorCategory;
    QVERIFY(!vpet::MemoryRepository::ValidateEntry(entry, errorCategory));
    QCOMPARE(errorCategory, QStringLiteral("credential"));
}

void MemoryRepositoryTest::EmptyTriggerRegexIsRejected()
{
    vpet::MemoryGraph graph;
    vpet::MemoryEntry entry = MakeEntry(QStringLiteral("negative"),
                                        QStringLiteral("不要打扰"));
    entry.type = vpet::MemoryEntry::Type::Negative;
    entry.triggerPatterns = { QStringLiteral("re:") };
    QString errorMessage;
    QVERIFY(!graph.AddEntry(entry, errorMessage));
    QVERIFY(!errorMessage.isEmpty());
}

void MemoryRepositoryTest::UnknownFieldsTolerated()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryRepository repository;
    QString errorMessage;
    QVERIFY(repository.SetDataDir(directory.path(), errorMessage));

    QFile graphFile(repository.GraphFilePath());
    QVERIFY(graphFile.open(QIODevice::WriteOnly));
    const QByteArray data = QByteArrayLiteral(
        R"({
            "schemaVersion": 1,
            "futureField": {"anything": true},
            "entries": [
                {
                    "id": "mem_1",
                    "content": "未来字段兼容",
                    "type": "fact",
                    "scope": "pet",
                    "provenance": "user_stated",
                    "petId": "pet_a",
                    "tags": [],
                    "unknownEntryField": 42
                }
            ],
            "updatedAt": 1
        })");
    QVERIFY(graphFile.write(data) == data.size());
    graphFile.close();

    vpet::MemoryGraph graph;
    const auto loadResult = repository.Load(graph);
    QVERIFY(loadResult.ok);
    QCOMPARE(graph.ActiveEntryCount(), 1);

    vpet::MemoryEntry loaded;
    QVERIFY(graph.GetEntry(QStringLiteral("mem_1"), loaded));
    QCOMPARE(loaded.content, QStringLiteral("未来字段兼容"));
}

void MemoryRepositoryTest::MissingFileLoadsEmpty()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryRepository repository;
    QString errorMessage;
    QVERIFY(repository.SetDataDir(directory.path(), errorMessage));

    vpet::MemoryGraph graph;
    const auto loadResult = repository.Load(graph);
    QVERIFY(loadResult.ok);
    QCOMPARE(graph.EntryCount(), 0);
}

void MemoryRepositoryTest::CorruptJsonRecoveredWithBackup()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryRepository repository;
    QString errorMessage;
    QVERIFY(repository.SetDataDir(directory.path(), errorMessage));

    QFile graphFile(repository.GraphFilePath());
    QVERIFY(graphFile.open(QIODevice::WriteOnly));
    const QByteArray corruptData = QByteArrayLiteral("{ \"schemaVersion\": 1, \"entries\": [ broken");
    QVERIFY(graphFile.write(corruptData) == corruptData.size());
    graphFile.close();

    vpet::MemoryGraph graph;
    const auto loadResult = repository.Load(graph);
    QVERIFY(!loadResult.ok);
    QVERIFY(loadResult.recovered);
    QCOMPARE(graph.EntryCount(), 0);
    QVERIFY(!loadResult.backupPath.isEmpty());

    QFile backupFile(loadResult.backupPath);
    QVERIFY(backupFile.open(QIODevice::ReadOnly));
    QCOMPARE(backupFile.readAll(), corruptData);
}

void MemoryRepositoryTest::UnsupportedSchemaVersionRecovered()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryRepository repository;
    QString errorMessage;
    QVERIFY(repository.SetDataDir(directory.path(), errorMessage));

    QFile graphFile(repository.GraphFilePath());
    QVERIFY(graphFile.open(QIODevice::WriteOnly));
    const QByteArray data = QByteArrayLiteral(
        R"({"schemaVersion": 99, "entries": [], "updatedAt": 0})");
    QVERIFY(graphFile.write(data) == data.size());
    graphFile.close();

    vpet::MemoryGraph graph;
    const auto loadResult = repository.Load(graph);
    QVERIFY(!loadResult.ok);
    QVERIFY(loadResult.recovered);
    QVERIFY(!loadResult.backupPath.isEmpty());
    QVERIFY(QFileInfo::exists(loadResult.backupPath));
}

void MemoryRepositoryTest::SaveFailureKeepsReturningFalse()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryRepository repository;
    QString errorMessage;
    QVERIFY(repository.SetDataDir(directory.path(), errorMessage));

    vpet::MemoryGraph graph;
    QVERIFY(graph.AddEntry(MakeEntry(QStringLiteral("m1"), QStringLiteral("内容一")), errorMessage));
    QVERIFY(repository.Save(graph, errorMessage));

    QFile::remove(repository.GraphFilePath());
    QVERIFY(QDir().mkdir(repository.GraphFilePath()));

    QString saveError;
    QVERIFY(!repository.Save(graph, saveError));

    QFile::remove(repository.GraphFilePath());
}

void MemoryRepositoryTest::PrivacyFilterRejectsCredentials()
{
    QString category;

    QVERIFY(!vpet::MemoryRepository::ValidateContent(
        QStringLiteral("我的 api_key 是 sk-1234567890abcdef"), category));
    QCOMPARE(category, QStringLiteral("credential"));

    QVERIFY(!vpet::MemoryRepository::ValidateContent(
        QStringLiteral("password = hunter2secret"), category));
    QCOMPARE(category, QStringLiteral("credential"));

    QVERIFY(!vpet::MemoryRepository::ValidateContent(
        QStringLiteral("-----BEGIN PRIVATE KEY-----\nMIIBVAIBADANBgkqhki\n-----END PRIVATE KEY-----"),
        category));
    QCOMPARE(category, QStringLiteral("private_key"));

    QVERIFY(!vpet::MemoryRepository::ValidateContent(
        QStringLiteral("token 是 eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.dozjgNryP4J3jVmNHl0w5N_XgL0n3I9PlFUP0THsR8U"),
        category));
    QCOMPARE(category, QStringLiteral("jwt"));

    QVERIFY(!vpet::MemoryRepository::ValidateContent(
        QStringLiteral("OPENAI_API_KEY=sk-proj-abcdefghijklmnopqrstuvwx"), category));
    QCOMPARE(category, QStringLiteral("env_file"));

    QVERIFY(!vpet::MemoryRepository::ValidateContent(
        QStringLiteral("我的身份证号是 11010519491231002X"), category));
    QCOMPARE(category, QStringLiteral("id_card"));

    QVERIFY(!vpet::MemoryRepository::ValidateContent(
        QStringLiteral("银行卡号 6222021234567890123"), category));
    QCOMPARE(category, QStringLiteral("bank_card"));

    QVERIFY(!vpet::MemoryRepository::ValidateContent(
        QStringLiteral("这是一个超长的内容") + QString(3000, QChar(0x957F)), category));
    QCOMPARE(category, QStringLiteral("too_long"));
}

void MemoryRepositoryTest::PrivacyFilterAllowsNormalContent()
{
    QString category;

    QVERIFY(vpet::MemoryRepository::ValidateContent(
        QStringLiteral("用户喜欢喝咖啡，下午三点工作"), category));
    QVERIFY(vpet::MemoryRepository::ValidateContent(
        QStringLiteral("不要再用户开会时搭话"), category));
    QVERIFY(vpet::MemoryRepository::ValidateContent(
        QStringLiteral("123456"), category));
}

void MemoryRepositoryTest::RoundTripWithEdges()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryRepository repository;
    QString errorMessage;
    QVERIFY(repository.SetDataDir(directory.path(), errorMessage));

    vpet::MemoryGraph graph;
    QVERIFY2(graph.AddEntry(MakeEntry(QStringLiteral("a"), QStringLiteral("记忆甲")), errorMessage),
             qPrintable(errorMessage));
    QVERIFY2(graph.AddEntry(MakeEntry(QStringLiteral("b"), QStringLiteral("记忆乙")), errorMessage),
             qPrintable(errorMessage));
    QVERIFY(graph.AddEdge(QStringLiteral("a"),
                          QStringLiteral("b"),
                          vpet::MemoryEdge::Type::Explicit,
                          QString(),
                          0.7f,
                          errorMessage));
    QVERIFY2(repository.Save(graph, errorMessage), qPrintable(errorMessage));

    vpet::MemoryGraph reloaded;
    const auto loadResult = repository.Load(reloaded);
    QVERIFY(loadResult.ok);
    QCOMPARE(reloaded.ActiveEntryCount(), 2);
    QCOMPARE(reloaded.AllEdges().size(), 1);
    QCOMPARE(reloaded.AllEdges().first().weight, 0.7f);
    QCOMPARE(reloaded.AllEdges().first().type, vpet::MemoryEdge::Type::Explicit);

    const QByteArray data = ReadGraphFile(directory.path());
    QVERIFY(data.contains(QStringLiteral("edges").toUtf8()));
}

void MemoryRepositoryTest::V1GraphLoadsWithoutEdges()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    vpet::MemoryRepository repository;
    QString errorMessage;
    QVERIFY(repository.SetDataDir(directory.path(), errorMessage));

    QFile graphFile(repository.GraphFilePath());
    QVERIFY(graphFile.open(QIODevice::WriteOnly));
    const QByteArray data = QByteArrayLiteral(
        R"({
            "schemaVersion": 1,
            "entries": [
                {
                    "id": "mem_1",
                    "content": "阶段一的旧记忆",
                    "type": "fact",
                    "scope": "pet",
                    "provenance": "user_stated",
                    "petId": "pet_a",
                    "tags": []
                }
            ],
            "updatedAt": 1
        })");
    QVERIFY(graphFile.write(data) == data.size());
    graphFile.close();

    vpet::MemoryGraph graph;
    const auto loadResult = repository.Load(graph);
    QVERIFY(loadResult.ok);
    QCOMPARE(graph.ActiveEntryCount(), 1);
    QCOMPARE(graph.AllEdges().size(), 0);

    // 加载后保存应升级到 v2 且保留条目
    QString saveError;
    QVERIFY(repository.Save(graph, saveError));
    const QByteArray savedData = ReadGraphFile(directory.path());
    QVERIFY(savedData.contains(QStringLiteral("edges").toUtf8()));
    QVERIFY(savedData.contains(QStringLiteral("阶段一的旧记忆").toUtf8()));
}

#include "memory_core_test.moc"

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    int status = 0;

    {
        MemoryGraphTest graphTest;
        status |= QTest::qExec(&graphTest, argc, argv);
    }

    {
        MemoryRepositoryTest repositoryTest;
        status |= QTest::qExec(&repositoryTest, argc, argv);
    }

    {
        MemoryMaintenanceTest maintenanceTest;
        status |= QTest::qExec(&maintenanceTest, argc, argv);
    }

    return status;
}
