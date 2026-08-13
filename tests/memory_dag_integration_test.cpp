#include "vpet/agent/agent_runtime.h"
#include "vpet/agent/agent_context_keys.h"
#include "vpet/agent/memory_store_node.h"
#include "vpet/memory/memory_service.h"

#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

namespace
{

const QString TEST_PET_ID = QStringLiteral("pet_a");

/**
 * @brief 写入测试 DAG 配置
 */
bool WriteDag(const QTemporaryDir &directory, QString &path)
{
    if (!directory.isValid())
    {
        return false;
    }

    path = directory.filePath(QStringLiteral("memory_integration_dag.json"));
    QFile file(path);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return false;
    }

    const QByteArray data = QByteArrayLiteral(
        R"({"nodes":[
              {"id":"user_source","type":"user.input","config":{"trigger":"user"}},
              {"id":"memory_retrieve","type":"memory.retrieve","config":{}},
              {"id":"fake_llm","type":"llm.chat","config":{}},
              {"id":"output","type":"output.format","config":{}},
              {"id":"memory_store","type":"memory.store","config":{}}
            ],
            "edges":[
              {"from":"user_source","to":"memory_retrieve"},
              {"from":"memory_retrieve","to":"fake_llm"},
              {"from":"fake_llm","to":"output"},
              {"from":"output","to":"memory_store"}
            ]})");
    return file.write(data) == data.size();
}

/**
 * @brief 等待队列排空
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

} // anonymous namespace

class MemoryDagIntegrationTest : public QObject
{
    Q_OBJECT

private slots:
    void StoreCommandPersistsMemory();
    void StoreIgnoresOrdinaryConversation();
    void RetrieveInjectsMemoryIntoPrompt();
    void PromptAliasesStaySynchronized();
    void DisabledServiceKeepsPromptUntouched();
    void StoreCommandParsing();
    void ProcedureCommandBuildsStructuredMemory();
    void TopicSwitchDropsDelayedMemory();
    void SingleCharacterTopicStillInjectsMemory();

private:
    QString m_capturedPrompt;
    bool m_storeAccepted = false;
};

void MemoryDagIntegrationTest::StoreCommandPersistsMemory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString configPath;
    QVERIFY(WriteDag(directory, configPath));

    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 4, errorMessage));

    vpet::AgentRuntime runtime(nullptr, &service, nullptr);
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("llm.chat"),
                                        [this](const vpet::_tagAgentDagNode &,
                                               vpet::AgentContext &context,
                                               QString &)
    {
        QVariant promptValue;
        context.GetValue(vpet::AgentContextKeys::NODE_INPUT_PROMPT, promptValue);
        m_capturedPrompt = promptValue.toString();
        return context.SetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE,
                                QStringLiteral("好的，我记住了。"));
    }));
    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.ExecuteWithUserInput(QStringLiteral("记住我喜欢喝咖啡"), errorMessage),
             qPrintable(errorMessage));
    WaitForDrain(service);

    QTRY_VERIFY_WITH_TIMEOUT(
        QFile::exists(directory.filePath(QStringLiteral("memory/graph.json"))), 3000);

    QFile graphFile(directory.filePath(QStringLiteral("memory/graph.json")));
    QVERIFY(graphFile.open(QIODevice::ReadOnly));
    const QByteArray graphData = graphFile.readAll();
    QVERIFY(graphData.contains(QStringLiteral("喜欢喝咖啡").toUtf8()));

    runtime.ShutdownMemory();
}

void MemoryDagIntegrationTest::StoreIgnoresOrdinaryConversation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString configPath;
    QVERIFY(WriteDag(directory, configPath));

    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 4, errorMessage));

    vpet::AgentRuntime runtime(nullptr, &service, nullptr);
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("llm.chat"),
                                        [this](const vpet::_tagAgentDagNode &,
                                               vpet::AgentContext &context,
                                               QString &)
    {
        return context.SetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE,
                                QStringLiteral("今天天气不错。"));
    }));
    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.ExecuteWithUserInput(QStringLiteral("今天天气不错"), errorMessage),
             qPrintable(errorMessage));
    WaitForDrain(service);
    QTest::qWait(100);

    QFile graphFile(directory.filePath(QStringLiteral("memory/graph.json")));

    if (graphFile.exists())
    {
        QVERIFY(graphFile.open(QIODevice::ReadOnly));
        const QByteArray graphData = graphFile.readAll();
        QVERIFY(!graphData.contains(QStringLiteral("今天天气不错").toUtf8()));
    }

    runtime.ShutdownMemory();
}

void MemoryDagIntegrationTest::RetrieveInjectsMemoryIntoPrompt()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString configPath;
    QVERIFY(WriteDag(directory, configPath));

    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 4, errorMessage));

    vpet::MemoryEntry entry = vpet::MemoryEntry();
    entry.content = QStringLiteral("用户喜欢喝咖啡");
    entry.petId = TEST_PET_ID;

    quint64 requestId = 0;
    QString rejectCategory;
    QVERIFY(service.TryEnqueueStore(TEST_PET_ID, QStringLiteral("user"), entry, requestId, rejectCategory));
    WaitForDrain(service);

    vpet::AgentRuntime runtime(nullptr, &service, nullptr);
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("llm.chat"),
                                        [this](const vpet::_tagAgentDagNode &,
                                               vpet::AgentContext &context,
                                               QString &)
    {
        QVariant promptValue;
        context.GetValue(vpet::AgentContextKeys::NODE_INPUT_PROMPT, promptValue);
        m_capturedPrompt = promptValue.toString();
        return context.SetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE,
                                QStringLiteral("好的。"));
    }));
    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));

    vpet::AgentContext petContext;
    petContext.SetValue(vpet::AgentContextKeys::PET_ID, TEST_PET_ID);
    runtime.SetContext(petContext);

    m_capturedPrompt.clear();
    QVERIFY2(runtime.ExecuteWithUserInput(QStringLiteral("咖啡怎么样"), errorMessage),
             qPrintable(errorMessage));

    QTRY_VERIFY_WITH_TIMEOUT(
        service.HasReadyResult(TEST_PET_ID, QStringLiteral("user")), 3000);

    m_capturedPrompt.clear();
    QVERIFY2(runtime.ExecuteWithUserInput(QStringLiteral("咖啡怎么样"), errorMessage),
             qPrintable(errorMessage));

    QVERIFY(m_capturedPrompt.contains(QStringLiteral("[长期记忆]")));
    QVERIFY(m_capturedPrompt.contains(QStringLiteral("喜欢喝咖啡")));
    QVERIFY(m_capturedPrompt.contains(QStringLiteral("咖啡怎么样")));

    QVariant entriesValue;
    QVERIFY(runtime.GetContext().GetValue(vpet::AgentContextKeys::SEMANTIC_MEMORY_ENTRIES,
                                          entriesValue));
    QCOMPARE(entriesValue.toList().size(), 1);

    QVariant pendingValue;

    if (runtime.GetContext().GetValue(vpet::AgentContextKeys::RUNTIME_PENDING, pendingValue))
    {
        QVERIFY(!pendingValue.toBool());
    }

    runtime.ShutdownMemory();
}

void MemoryDagIntegrationTest::PromptAliasesStaySynchronized()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString configPath;
    QVERIFY(WriteDag(directory, configPath));

    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 4, errorMessage));

    vpet::MemoryEntry entry = vpet::MemoryEntry();
    entry.content = QStringLiteral("用户喜欢喝茶");
    entry.petId = TEST_PET_ID;

    quint64 requestId = 0;
    QString rejectCategory;
    QVERIFY(service.TryEnqueueStore(TEST_PET_ID, QStringLiteral("user"), entry, requestId, rejectCategory));
    WaitForDrain(service);

    vpet::AgentRuntime runtime(nullptr, &service, nullptr);
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("llm.chat"),
                                        [this](const vpet::_tagAgentDagNode &,
                                               vpet::AgentContext &context,
                                               QString &)
    {
        QVariant promptValue;
        context.GetValue(vpet::AgentContextKeys::NODE_INPUT_PROMPT, promptValue);
        m_capturedPrompt = promptValue.toString();

        QVariant semanticValue;
        QVariant outputValue;
        QVariant promptTextValue;
        context.GetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_PROMPT, semanticValue);
        context.GetValue(vpet::AgentContextKeys::NODE_OUTPUT_PROMPT, outputValue);
        context.GetValue(vpet::AgentContextKeys::PROMPT_TEXT, promptTextValue);
        m_storeAccepted = (promptValue.toString() == semanticValue.toString())
                          && (promptValue.toString() == outputValue.toString())
                          && (promptValue.toString() == promptTextValue.toString());
        return context.SetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE,
                                QStringLiteral("好的。"));
    }));
    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));

    vpet::AgentContext petContext;
    petContext.SetValue(vpet::AgentContextKeys::PET_ID, TEST_PET_ID);
    runtime.SetContext(petContext);

    QVERIFY2(runtime.ExecuteWithUserInput(QStringLiteral("茶好喝吗"), errorMessage),
             qPrintable(errorMessage));

    QTRY_VERIFY_WITH_TIMEOUT(
        service.HasReadyResult(TEST_PET_ID, QStringLiteral("user")), 3000);

    m_storeAccepted = false;
    QVERIFY2(runtime.ExecuteWithUserInput(QStringLiteral("茶好喝吗"), errorMessage),
             qPrintable(errorMessage));

    QVERIFY(m_capturedPrompt.contains(QStringLiteral("[长期记忆]")));
    QVERIFY(m_storeAccepted);

    runtime.ShutdownMemory();
}

void MemoryDagIntegrationTest::DisabledServiceKeepsPromptUntouched()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString configPath;
    QString errorMessage;
    QVERIFY(WriteDag(directory, configPath));

    vpet::AgentRuntime runtime;
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("llm.chat"),
                                        [this](const vpet::_tagAgentDagNode &,
                                               vpet::AgentContext &context,
                                               QString &)
    {
        QVariant promptValue;
        context.GetValue(vpet::AgentContextKeys::NODE_INPUT_PROMPT, promptValue);
        m_capturedPrompt = promptValue.toString();
        return context.SetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE,
                                QStringLiteral("好的。"));
    }));
    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));

    QVERIFY(!runtime.IsMemoryEnabled());

    m_capturedPrompt.clear();
    QVERIFY2(runtime.ExecuteWithUserInput(QStringLiteral("普通对话内容"), errorMessage),
             qPrintable(errorMessage));

    QCOMPARE(m_capturedPrompt, QStringLiteral("普通对话内容"));
    QVERIFY(!runtime.GetContext().Contains(vpet::AgentContextKeys::SEMANTIC_MEMORY_ENTRIES));
}

void MemoryDagIntegrationTest::StoreCommandParsing()
{
    QVariantMap intent;

    QVERIFY(vpet::MemoryStoreNode::ParseCommand(QStringLiteral("记住我喜欢喝咖啡"), intent));
    QCOMPARE(intent.value(QStringLiteral("action")).toString(), QStringLiteral("remember"));
    QCOMPARE(intent.value(QStringLiteral("content")).toString(), QStringLiteral("我喜欢喝咖啡"));
    QCOMPARE(intent.value(QStringLiteral("type")).toString(), QStringLiteral("preference"));

    QVERIFY(vpet::MemoryStoreNode::ParseCommand(QStringLiteral("请记住不要在我开会时说话"), intent));
    QCOMPARE(intent.value(QStringLiteral("action")).toString(), QStringLiteral("remember"));
    QCOMPARE(intent.value(QStringLiteral("type")).toString(), QStringLiteral("negative"));

    QVERIFY(vpet::MemoryStoreNode::ParseCommand(QStringLiteral("以后不要再说这个了"), intent));
    QCOMPARE(intent.value(QStringLiteral("type")).toString(), QStringLiteral("negative"));

    QVERIFY(vpet::MemoryStoreNode::ParseCommand(QStringLiteral("忘了咖啡的事情"), intent));
    QCOMPARE(intent.value(QStringLiteral("action")).toString(), QStringLiteral("forget"));
    QCOMPARE(intent.value(QStringLiteral("content")).toString(), QStringLiteral("咖啡的事情"));

    QVERIFY(!vpet::MemoryStoreNode::ParseCommand(QStringLiteral("你记住这个了吗？"), intent));
    QVERIFY(!vpet::MemoryStoreNode::ParseCommand(QStringLiteral("记住"), intent));
    QVERIFY(!vpet::MemoryStoreNode::ParseCommand(QStringLiteral("今天天气不错"), intent));

    vpet::MemoryEntry entry;
    QString entryError;
    intent.clear();
    intent.insert(QStringLiteral("action"), QStringLiteral("remember"));
    intent.insert(QStringLiteral("content"), QStringLiteral("我是项目开发者"));
    intent.insert(QStringLiteral("type"), QStringLiteral("fact"));
    QVERIFY(vpet::MemoryStoreNode::BuildEntry(intent,
                                              TEST_PET_ID,
                                              QStringLiteral("pet"),
                                              entry,
                                              entryError));
    QCOMPARE(entry.content, QStringLiteral("我是项目开发者"));
    QCOMPARE(entry.type, vpet::MemoryEntry::Type::Fact);
    QCOMPARE(entry.scope, vpet::MemoryEntry::Scope::Pet);
    QCOMPARE(entry.petId, TEST_PET_ID);
}

void MemoryDagIntegrationTest::ProcedureCommandBuildsStructuredMemory()
{
    QVariantMap intent;
    QVERIFY(vpet::MemoryStoreNode::ParseCommand(
        QStringLiteral("记住流程；名称：发布；触发：准备发布；步骤：运行测试、生成包；警告：不要包含密钥"),
        intent));
    QCOMPARE(intent.value(QStringLiteral("type")).toString(), QStringLiteral("procedure"));

    vpet::MemoryEntry entry;
    QString errorMessage;
    QVERIFY(vpet::MemoryStoreNode::BuildEntry(intent,
                                              TEST_PET_ID,
                                              QStringLiteral("pet"),
                                              entry,
                                              errorMessage));
    QCOMPARE(entry.type, vpet::MemoryEntry::Type::Procedure);
    QCOMPARE(entry.procedure.name, QStringLiteral("发布"));
    QCOMPARE(entry.procedure.trigger, QStringLiteral("准备发布"));
    QCOMPARE(entry.procedure.steps, QStringList({ QStringLiteral("运行测试"),
                                                   QStringLiteral("生成包") }));
    QCOMPARE(entry.procedure.warnings, QStringList({ QStringLiteral("不要包含密钥") }));
    QVERIFY(entry.triggerPatterns.contains(QStringLiteral("准备发布")));

    QVariantMap negativeIntent;
    QVERIFY(vpet::MemoryStoreNode::ParseCommand(
        QStringLiteral("以后不要再在我开会时说话"), negativeIntent));
    vpet::MemoryEntry negativeEntry;
    QVERIFY(vpet::MemoryStoreNode::BuildEntry(negativeIntent,
                                              TEST_PET_ID,
                                              QStringLiteral("pet"),
                                              negativeEntry,
                                              errorMessage));
    QVERIFY(negativeEntry.triggerPatterns.contains(QStringLiteral("我开会")));
}

void MemoryDagIntegrationTest::TopicSwitchDropsDelayedMemory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString configPath;
    QVERIFY(WriteDag(directory, configPath));
    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 4, errorMessage));
    vpet::MemoryEntry entry;
    entry.content = QStringLiteral("用户喜欢喝咖啡");
    entry.petId = TEST_PET_ID;
    quint64 requestId = 0;
    QString rejectCategory;
    QVERIFY(service.TryEnqueueStore(TEST_PET_ID,
                                    QStringLiteral("user"),
                                    entry,
                                    requestId,
                                    rejectCategory));
    WaitForDrain(service);

    vpet::AgentRuntime runtime(nullptr, &service, nullptr);
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("llm.chat"),
                                        [this](const vpet::_tagAgentDagNode &,
                                               vpet::AgentContext &context,
                                               QString &)
    {
        QVariant promptValue;
        context.GetValue(vpet::AgentContextKeys::NODE_INPUT_PROMPT, promptValue);
        m_capturedPrompt = promptValue.toString();
        return context.SetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE,
                                QStringLiteral("好的。"));
    }));
    QVERIFY(runtime.Load(configPath, errorMessage));
    vpet::AgentContext context;
    context.SetValue(vpet::AgentContextKeys::PET_ID, TEST_PET_ID);
    runtime.SetContext(context);
    QVERIFY(runtime.ExecuteWithUserInput(QStringLiteral("请问我喜欢什么咖啡"), errorMessage));
    QTRY_VERIFY_WITH_TIMEOUT(service.HasReadyResult(TEST_PET_ID, QStringLiteral("user")), 3000);

    m_capturedPrompt.clear();
    QVERIFY(runtime.ExecuteWithUserInput(QStringLiteral("请问天气如何"), errorMessage));
    QVERIFY(!m_capturedPrompt.contains(QStringLiteral("[长期记忆]")));
    QVERIFY(!m_capturedPrompt.contains(QStringLiteral("喜欢喝咖啡")));
    runtime.ShutdownMemory();
}

void MemoryDagIntegrationTest::SingleCharacterTopicStillInjectsMemory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString configPath;
    QVERIFY(WriteDag(directory, configPath));
    vpet::MemoryService service;
    QString errorMessage;
    QVERIFY(service.Start(directory.path(), 4, errorMessage));
    vpet::MemoryEntry entry;
    entry.content = QStringLiteral("用户喜欢喝茶");
    entry.petId = TEST_PET_ID;
    quint64 requestId = 0;
    QString rejectCategory;
    QVERIFY(service.TryEnqueueStore(TEST_PET_ID,
                                    QStringLiteral("user"),
                                    entry,
                                    requestId,
                                    rejectCategory));
    WaitForDrain(service);
    vpet::AgentRuntime runtime(nullptr, &service, nullptr);
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("llm.chat"),
                                        [this](const vpet::_tagAgentDagNode &,
                                               vpet::AgentContext &context,
                                               QString &)
    {
        QVariant promptValue;
        context.GetValue(vpet::AgentContextKeys::NODE_INPUT_PROMPT, promptValue);
        m_capturedPrompt = promptValue.toString();
        return context.SetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE,
                                QStringLiteral("好的。"));
    }));
    QVERIFY(runtime.Load(configPath, errorMessage));
    vpet::AgentContext context;
    context.SetValue(vpet::AgentContextKeys::PET_ID, TEST_PET_ID);
    runtime.SetContext(context);
    QVERIFY(runtime.ExecuteWithUserInput(QStringLiteral("茶"), errorMessage));
    QTRY_VERIFY_WITH_TIMEOUT(service.HasReadyResult(TEST_PET_ID, QStringLiteral("user")), 3000);
    m_capturedPrompt.clear();
    QVERIFY(runtime.ExecuteWithUserInput(QStringLiteral("茶怎么样"), errorMessage));
    QVERIFY(m_capturedPrompt.contains(QStringLiteral("喜欢喝茶")));
    runtime.ShutdownMemory();
}

QTEST_MAIN(MemoryDagIntegrationTest)
#include "memory_dag_integration_test.moc"
