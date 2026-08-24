#include "vpet/dageditor/dag_document_store.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace vpet;

namespace
{

const char *kValidDagJson = R"({
    "nodes": [
        {"id": "a", "type": "user.input", "config": {"trigger": "user"}},
        {"id": "b", "type": "llm.chat", "config": {}}
    ],
    "edges": [{"from": "a", "to": "b"}]
})";

const QByteArray kValidDagBytes(kValidDagJson);

bool WriteConfig(const QString &path, const QByteArray &content)
{
    QFile file(path);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return false;
    }

    return file.write(content) == content.size();
}

QByteArray ReadConfig(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class DagDocumentStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void InitializeFromExistingFile();
    void InitializeWithMissingFileCreatesEmptyBaseline();
    void SaveWritesAtomicallyAndCreatesBackup();
    void SaveRejectsStaleRevision();
    void SaveRejectsInvalidGraphWithoutTouchingDisk();
    void AdoptExternalFileDetectsRealChanges();

private:
    QTemporaryDir m_temporaryDirectory;
};

void DagDocumentStoreTest::InitializeFromExistingFile()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("init.json"));
    QVERIFY(WriteConfig(path, kValidDagBytes));

    DagDocumentStore store;
    QString errorMessage;
    QVERIFY2(store.Initialize(path, nullptr, errorMessage), qPrintable(errorMessage));

    QCOMPARE(store.GetRevision(), quint64(1)); // 磁盘 revision 0 + 1
    QCOMPARE(store.GetDocument().GetNodes().size(), 2);
    QCOMPARE(store.GetConfigPath(), QFileInfo(path).absoluteFilePath());
}

void DagDocumentStoreTest::InitializeWithMissingFileCreatesEmptyBaseline()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("missing.json"));

    DagDocumentStore store;
    QString errorMessage;
    QVERIFY2(store.Initialize(path, nullptr, errorMessage), qPrintable(errorMessage));

    QVERIFY(store.GetDocument().GetNodes().isEmpty());
    QCOMPARE(store.GetRevision(), quint64(1));
}

void DagDocumentStoreTest::SaveWritesAtomicallyAndCreatesBackup()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("save.json"));
    QVERIFY(WriteConfig(path, kValidDagBytes));

    DagDocumentStore store;
    QString errorMessage;
    QVERIFY2(store.Initialize(path, nullptr, errorMessage), qPrintable(errorMessage));

    // 修改文档并保存。
    DagDocument document = store.GetDocument();
    _tagDagDocumentNode node;
    node.id = QStringLiteral("c");
    node.type = QStringLiteral("output.format");
    QVERIFY(document.UpsertNode(node));
    QVERIFY(document.AddEdge(QStringLiteral("b"), QStringLiteral("c")));

    QVector<DagValidationIssue> issues;
    quint64 baseRevision = store.GetRevision();

    const auto status = store.Save(document, baseRevision, issues, errorMessage);
    QCOMPARE(status, DagDocumentStore::SaveStatus::Saved);
    QCOMPARE(store.GetRevision(), baseRevision + 1);

    // 落盘内容包含新节点。
    const QByteArray savedBytes = ReadConfig(path);
    QVERIFY(savedBytes.contains(QByteArrayLiteral("\"c\"")));

    // .bak 备份是上一版内容（不含新节点）。
    QVERIFY(QFileInfo::exists(path + QStringLiteral(".bak")));
    QVERIFY(!ReadConfig(path + QStringLiteral(".bak")).contains(QByteArrayLiteral("\"c\"")));
}

void DagDocumentStoreTest::SaveRejectsStaleRevision()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("conflict.json"));
    QVERIFY(WriteConfig(path, kValidDagBytes));

    DagDocumentStore store;
    QString errorMessage;
    QVERIFY2(store.Initialize(path, nullptr, errorMessage), qPrintable(errorMessage));

    DagDocument stale = store.GetDocument();
    const quint64 staleBase = store.GetRevision() - 1;

    QVector<DagValidationIssue> issues;
    QCOMPARE(store.Save(stale, staleBase, issues, errorMessage),
             DagDocumentStore::SaveStatus::Conflict);
}

void DagDocumentStoreTest::SaveRejectsInvalidGraphWithoutTouchingDisk()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("invalid.json"));
    QVERIFY(WriteConfig(path, kValidDagBytes));
    const QByteArray originalBytes = ReadConfig(path);

    DagDocumentStore store;
    QString errorMessage;
    QVERIFY2(store.Initialize(path, nullptr, errorMessage), qPrintable(errorMessage));

    DagDocument broken = store.GetDocument();
    _tagDagDocumentNode loopNode;
    loopNode.id = QStringLiteral("loop");
    loopNode.type = QStringLiteral("llm.chat");
    QVERIFY(broken.UpsertNode(loopNode));

    // AddEdge 在文档层拒绝自环，这里直接注入边数据以验证校验器兜底。
    QVector<_tagDagDocumentEdge> edges = broken.GetEdges();
    _tagDagDocumentEdge selfLoop;
    selfLoop.from = QStringLiteral("loop");
    selfLoop.to = QStringLiteral("loop");
    edges.append(selfLoop);
    broken.SetEdges(edges);

    QVector<DagValidationIssue> issues;
    QCOMPARE(store.Save(broken, store.GetRevision(), issues, errorMessage),
             DagDocumentStore::SaveStatus::Invalid);
    QVERIFY(AgentDagValidator::HasErrors(issues));

    bool foundSelfLoop = false;

    for (const DagValidationIssue &issue : issues)
    {
        foundSelfLoop |= (issue.code == QStringLiteral("self_loop"));
    }

    QVERIFY(foundSelfLoop);
    QCOMPARE(ReadConfig(path), originalBytes); // 磁盘未被破坏
}

void DagDocumentStoreTest::AdoptExternalFileDetectsRealChanges()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("external.json"));
    QVERIFY(WriteConfig(path, kValidDagBytes));

    DagDocumentStore store;
    QString errorMessage;
    QVERIFY2(store.Initialize(path, nullptr, errorMessage), qPrintable(errorMessage));

    quint64 externalRevisions = 0;
    QObject::connect(&store, &DagDocumentStore::ExternalChanged,
                     [&externalRevisions](quint64) { ++externalRevisions; });

    const quint64 revisionBefore = store.GetRevision();

    // 无变化时不应采纳。
    QVERIFY(!store.AdoptExternalFile(errorMessage));
    QCOMPARE(store.GetRevision(), revisionBefore);

    // 外部写入新内容后采纳为新基线。
    QVERIFY(WriteConfig(path, QByteArrayLiteral(
        "{\"nodes\":[{\"id\":\"x\",\"type\":\"llm.chat\",\"config\":{}}],\"edges\":[]}")));

    QVERIFY2(store.AdoptExternalFile(errorMessage), qPrintable(errorMessage));
    QCOMPARE(store.GetRevision(), revisionBefore + 1);
    QCOMPARE(externalRevisions, 1);
    QCOMPARE(store.GetDocument().GetNodes().size(), 1);

    // 外部坏内容：拒绝采纳且保留基线。
    QVERIFY(WriteConfig(path, QByteArrayLiteral("{broken json")));
    QVERIFY(!store.AdoptExternalFile(errorMessage));
    QCOMPARE(store.GetDocument().GetNodes().size(), 1);
}

QTEST_MAIN(DagDocumentStoreTest)

#include "dag_document_store_test.moc"
