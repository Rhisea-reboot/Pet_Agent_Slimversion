#include "vpet/dageditor/dag_document.h"

#include <QtTest>

using namespace vpet;

namespace
{

/**
 * @brief 构造一个带布局信息的测试节点
 */
_tagDagDocumentNode MakeNode(const QString &id,
                             const QString &type,
                             bool hasLayout = false)
{
    _tagDagDocumentNode node;
    node.id = id;
    node.type = type;
    node.config.insert(QStringLiteral("temperature"), 0.7);

    if (hasLayout)
    {
        node.hasLayout = true;
        node.layoutX = 100.0;
        node.layoutY = 240.0;
        node.layoutWidth = 280.0;
        node.layoutHeight = 120.0;
        node.layoutTitle = QStringLiteral("对话 LLM");
        node.layoutCollapsed = true;
    }

    return node;
}

} // namespace

class DagDocumentTest : public QObject
{
    Q_OBJECT

private slots:
    void RoundTripPreservesAllFields();
    void LoadsLegacyFileWithoutVersionAndEditor();
    void DropsOrphanEditorLayout();
    void RejectsMalformedDocuments();
    void NodeMutationsCascadeEdges();
    void EdgeRulesRejectSelfLoopAndDuplicate();
    void ViewportAndRevisionRoundTrip();

private:
    DagDocument m_document;
};

void DagDocumentTest::RoundTripPreservesAllFields()
{
    DagDocument document;
    QVector<_tagDagDocumentNode> nodes;
    nodes.append(MakeNode(QStringLiteral("user_input"), QStringLiteral("user.input")));
    nodes.append(MakeNode(QStringLiteral("call_llm"), QStringLiteral("llm.chat"), true));
    document.SetNodes(nodes);
    document.AddEdge(QStringLiteral("user_input"), QStringLiteral("call_llm"));
    document.SetViewport(12.5, -3.25, 0.85);
    document.SetRevision(7);

    // 未识别的顶层字段往返保真：在序列化对象上注入扩展字段后重新加载。
    QJsonObject extendedRoot = document.ToJsonObject();
    extendedRoot.insert(QStringLiteral("custom_extension"),
                        QJsonObject{{QStringLiteral("a"), 1}});
    const QByteArray extended = QJsonDocument(extendedRoot).toJson(QJsonDocument::Indented);

    DagDocument restored;
    QString errorMessage;
    QVERIFY2(DagDocument::FromJsonData(extended, restored, errorMessage),
             qPrintable(errorMessage));

    QCOMPARE(restored.GetNodes().size(), 2);
    QCOMPARE(restored.GetEdges().size(), 1);
    QCOMPARE(restored.GetRevision(), quint64(7));
    QCOMPARE(restored.GetZoom(), 0.85);
    QCOMPARE(restored.GetScrollX(), 12.5);
    QCOMPARE(restored.GetScrollY(), -3.25);

    _tagDagDocumentNode layoutNode;

    QVERIFY(restored.FindNode(QStringLiteral("call_llm"), layoutNode));
    QVERIFY(layoutNode.hasLayout);
    QCOMPARE(layoutNode.layoutX, 100.0);
    QCOMPARE(layoutNode.layoutY, 240.0);
    QCOMPARE(layoutNode.layoutWidth, 280.0);
    QCOMPARE(layoutNode.layoutHeight, 120.0);
    QCOMPARE(layoutNode.layoutTitle, QStringLiteral("对话 LLM"));
    QVERIFY(layoutNode.layoutCollapsed);

    QVERIFY(restored.FindNode(QStringLiteral("user_input"), layoutNode));
    QVERIFY(!layoutNode.hasLayout);

    // 再次序列化保持稳定（幂等）
    DagDocument reparsed;
    QVERIFY2(DagDocument::FromJsonData(restored.ToJsonData(), reparsed, errorMessage),
             qPrintable(errorMessage));
    QCOMPARE(reparsed.ToJsonData(), restored.ToJsonData());

    QJsonObject rootObject = restored.ToJsonObject();
    QVERIFY(rootObject.contains(QStringLiteral("custom_extension")));
    QCOMPARE(rootObject.value(QStringLiteral("version")).toInt(), DagDocument::kFormatVersion);
}

void DagDocumentTest::LoadsLegacyFileWithoutVersionAndEditor()
{
    const QByteArray legacyJson = QByteArrayLiteral(
        "{\"nodes\":[{\"id\":\"a\",\"type\":\"llm.chat\",\"config\":{}},"
        "{\"id\":\"b\",\"type\":\"output.format\",\"config\":{}}],"
        "\"edges\":[{\"from\":\"a\",\"to\":\"b\"}]}");

    DagDocument document;
    QString errorMessage;
    QVERIFY2(DagDocument::FromJsonData(legacyJson, document, errorMessage),
             qPrintable(errorMessage));

    QCOMPARE(document.GetNodes().size(), 2);
    QCOMPARE(document.GetEdges().size(), 1);
    QCOMPARE(document.GetRevision(), quint64(0));

    // 首次保存后自动补齐 version 与 editor 字段。
    const QJsonObject savedRoot = document.ToJsonObject();
    QVERIFY(savedRoot.contains(QStringLiteral("version")));
    QVERIFY(savedRoot.contains(QStringLiteral("editor")));
}

void DagDocumentTest::DropsOrphanEditorLayout()
{
    const QByteArray jsonWithOrphan = QByteArrayLiteral(
        "{\"version\":2,"
        "\"nodes\":[{\"id\":\"a\",\"type\":\"llm.chat\",\"config\":{}}],"
        "\"edges\":[],"
        "\"editor\":{\"revision\":3,\"scroll\":[5,6],\"zoom\":1.2,"
        "\"nodes\":{\"a\":{\"pos\":[1,2]},\"ghost\":{\"pos\":[9,9]}}}}");

    DagDocument document;
    QString errorMessage;
    QVERIFY2(DagDocument::FromJsonData(jsonWithOrphan, document, errorMessage),
             qPrintable(errorMessage));

    QCOMPARE(document.GetNodes().size(), 1);
    QVERIFY(document.GetNodes().at(0).hasLayout);
    QCOMPARE(document.GetRevision(), quint64(3));
    QCOMPARE(document.GetZoom(), 1.2);

    // 孤儿坐标不应出现在再次序列化的结果里。
    const QJsonObject editorObject =
        document.ToJsonObject().value(QStringLiteral("editor")).toObject();
    const QJsonObject layoutNodes =
        editorObject.value(QStringLiteral("nodes")).toObject();
    QCOMPARE(layoutNodes.size(), 1);
    QVERIFY(layoutNodes.contains(QStringLiteral("a")));
    QVERIFY(!layoutNodes.contains(QStringLiteral("ghost")));

    // 非法缩放回退为 1.0
    QByteArray badZoom(jsonWithOrphan);
    DagDocument badZoomDocument;
    QVERIFY(DagDocument::FromJsonData(badZoom.replace(QByteArrayLiteral("\"zoom\":1.2"),
                                                      QByteArrayLiteral("\"zoom\":0")),
                                      badZoomDocument,
                                      errorMessage));
    QCOMPARE(badZoomDocument.GetZoom(), 1.0);
}

void DagDocumentTest::RejectsMalformedDocuments()
{
    DagDocument document;
    QString errorMessage;

    QVERIFY(!DagDocument::FromJsonData(QByteArrayLiteral("{not json"),
                                        document,
                                        errorMessage));

    QVERIFY(!DagDocument::FromJsonData(QByteArrayLiteral("{\"edges\":[]}"),
                                        document,
                                        errorMessage)); // 缺 nodes

    QVERIFY(!DagDocument::FromJsonData(QByteArrayLiteral("{\"nodes\":[]}"),
                                        document,
                                        errorMessage)); // 缺 edges

    QVERIFY(!DagDocument::FromJsonData(
        QByteArrayLiteral("{\"nodes\":[{\"type\":\"t\"}],\"edges\":[]}"),
        document,
        errorMessage)); // 节点缺 id

    QVERIFY(!DagDocument::FromJsonData(
        QByteArrayLiteral(
            "{\"nodes\":[{\"id\":\"a\",\"type\":\"t\"},{\"id\":\"a\",\"type\":\"u\"}],"
            "\"edges\":[]}"),
        document,
        errorMessage)); // 重复 id

    QVERIFY(!DagDocument::FromJsonData(
        QByteArrayLiteral("{\"nodes\":[],\"edges\":[{\"from\":\"x\"}]}"),
        document,
        errorMessage)); // 边缺 to

    QVERIFY(errorMessage.trimmed().isEmpty() == false);
}

void DagDocumentTest::NodeMutationsCascadeEdges()
{
    DagDocument document;
    QVERIFY(document.UpsertNode(MakeNode(QStringLiteral("a"), QStringLiteral("user.input"))));
    QVERIFY(document.UpsertNode(MakeNode(QStringLiteral("b"), QStringLiteral("llm.chat"))));
    QVERIFY(document.UpsertNode(MakeNode(QStringLiteral("c"), QStringLiteral("output.format"))));
    QVERIFY(document.AddEdge(QStringLiteral("a"), QStringLiteral("b")));
    QVERIFY(document.AddEdge(QStringLiteral("b"), QStringLiteral("c")));
    QCOMPARE(document.GetEdges().size(), 2);

    // 更新已有节点不新增条目
    QVERIFY(document.UpsertNode(MakeNode(QStringLiteral("b"), QStringLiteral("web.research"))));
    QCOMPARE(document.GetNodes().size(), 3);
    QCOMPARE(document.IndexOfNode(QStringLiteral("b")), 1);

    // 删除节点连带删除关联边
    QVERIFY(document.RemoveNode(QStringLiteral("b")));
    QCOMPARE(document.GetNodes().size(), 2);
    QCOMPARE(document.GetEdges().size(), 0);

    QVERIFY(!document.RemoveNode(QStringLiteral("missing")));
    QVERIFY(!document.UpsertNode(_tagDagDocumentNode())); // 空 id 拒绝
}

void DagDocumentTest::EdgeRulesRejectSelfLoopAndDuplicate()
{
    DagDocument document;
    QVERIFY(document.UpsertNode(MakeNode(QStringLiteral("a"), QStringLiteral("user.input"))));

    QVERIFY(!document.AddEdge(QStringLiteral("a"), QStringLiteral("a"))); // 自环
    QVERIFY(document.AddEdge(QStringLiteral("a"), QStringLiteral("ghost"))); // 文档层不校验端点存在性
    QVERIFY(document.RemoveEdge(QStringLiteral("a"), QStringLiteral("ghost")));
    QVERIFY(document.AddEdge(QStringLiteral("a"), QStringLiteral("b")));
    QVERIFY(!document.AddEdge(QStringLiteral("a"), QStringLiteral("b")));     // 重复
    QVERIFY(!document.AddEdge(QString(), QStringLiteral("b")));               // 空端点

    QVERIFY(document.RemoveEdge(QStringLiteral("a"), QStringLiteral("b")));
    QVERIFY(!document.RemoveEdge(QStringLiteral("a"), QStringLiteral("b")));
}

void DagDocumentTest::ViewportAndRevisionRoundTrip()
{
    DagDocument document;
    document.SetViewport(0.0, 0.0, -5.0); // 非法缩放回退 1.0
    QCOMPARE(document.GetZoom(), 1.0);

    document.SetViewport(10.0, 20.0, 0.5);
    document.SetRevision(42);

    QString errorMessage;
    DagDocument restored;

    QVERIFY2(DagDocument::FromJsonObject(document.ToJsonObject(), restored, errorMessage),
             qPrintable(errorMessage));
    QCOMPARE(restored.GetScrollX(), 10.0);
    QCOMPARE(restored.GetScrollY(), 20.0);
    QCOMPARE(restored.GetZoom(), 0.5);
    QCOMPARE(restored.GetRevision(), quint64(42));
}

QTEST_MAIN(DagDocumentTest)

#include "dag_document_test.moc"
