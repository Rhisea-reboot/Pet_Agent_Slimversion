#include "vpet/agent/agent_runtime.h"
#include "vpet/agent/proactive_topic_node.h"
#include "vpet/agent/agent_context_keys.h"
#include "vpet/agent/web_research_node.h"
#include "vpet/web/web_research_engine.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QMetaObject>
#include <QTemporaryDir>
#include <QVariant>
#include <QtTest>

namespace
{

class ControlledWebResearchEngine : public vpet::WebResearchEngine
{
public:
    ControlledWebResearchEngine()
        : vpet::WebResearchEngine(nullptr, nullptr)
        , startCount(0)
        , activeResearchId(41)
        , lastRequest()
    {
    }

    /** @brief Records a research request without using the network. @param[in] request Research request. @return Fixed research ID. */
    int Start(const vpet::_tagWebResearchRequest &request) override
    {
        if (request.question.trimmed().isEmpty())
        {
            return -1;
        }

        startCount += 1;
        lastRequest = request;
        return activeResearchId;
    }

    /** @brief Emits a controlled completion. @param[in] response Research response. */
    void Complete(const vpet::_tagWebResearchResponse &response)
    {
        emit Completed(response);
    }

    /** @brief Emits a controlled failure. @param[in] message Error message. */
    void Fail(const QString &message)
    {
        emit Failed(activeResearchId, message, 0);
    }

    int startCount;
    int activeResearchId;
    vpet::_tagWebResearchRequest lastRequest;
};

class SynchronousWebResearchEngine : public vpet::WebResearchEngine
{
public:
    enum Outcome
    {
        CompleteImmediately,
        FailImmediately
    };

    explicit SynchronousWebResearchEngine(Outcome outcome)
        : vpet::WebResearchEngine(nullptr, nullptr)
        , outcome(outcome)
        , activeResearchId(73)
    {
    }

    int Start(const vpet::_tagWebResearchRequest &request) override
    {
        if (outcome == CompleteImmediately)
        {
            vpet::_tagWebResearchResponse response;
            response.researchId = activeResearchId;
            response.question = request.question;
            response.status = QStringLiteral("completed");
            response.summary = QStringLiteral("Synchronous research result.");
            emit Completed(response);
        }
        else
        {
            emit Failed(activeResearchId, QStringLiteral("Synchronous research failure."), 0);
        }

        return activeResearchId;
    }

    Outcome outcome;
    int activeResearchId;
};

bool WriteDagConfig(const QTemporaryDir &temporaryDirectory,
                    const QByteArray &jsonData,
                    QString &configPath)
{
    if (!temporaryDirectory.isValid() || jsonData.isEmpty())
    {
        return false;
    }

    configPath = temporaryDirectory.filePath(QStringLiteral("scheduler_test_dag.json"));
    QFile configFile(configPath);

    if (!configFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return false;
    }

    if (configFile.write(jsonData) != jsonData.size())
    {
        return false;
    }

    return true;
}

QByteArray BuildDefaultSingleChainDagJson()
{
    return QByteArrayLiteral(
        R"({"nodes":[{"id":"vision_input","type":"vision.input","config":{}},{"id":"vision_llm","type":"vision.llm","config":{}},{"id":"proactive_topic","type":"proactive.topic","config":{}},{"id":"call_llm","type":"llm.chat","config":{}},{"id":"emotion_rewrite","type":"emotion.rewrite","config":{}},{"id":"format_output","type":"output.format","config":{}}],"edges":[{"from":"vision_input","to":"vision_llm"},{"from":"vision_llm","to":"proactive_topic"},{"from":"proactive_topic","to":"call_llm"},{"from":"call_llm","to":"emotion_rewrite"},{"from":"emotion_rewrite","to":"format_output"}]})");
}

bool InvokeLlmCompleted(vpet::AgentRuntime &runtime, int requestId, const QString &content)
{
    return QMetaObject::invokeMethod(&runtime,
                                     "OnLlmChatCompleted",
                                     Qt::DirectConnection,
                                     Q_ARG(int, requestId),
                                     Q_ARG(QString, content));
}

bool InvokeLlmFailed(vpet::AgentRuntime &runtime,
                     int requestId,
                     const QString &message,
                     int statusCode)
{
    return QMetaObject::invokeMethod(&runtime,
                                     "OnLlmChatFailed",
                                     Qt::DirectConnection,
                                     Q_ARG(int, requestId),
                                     Q_ARG(QString, message),
                                     Q_ARG(int, statusCode));
}

bool InvokeVisionCompleted(vpet::AgentRuntime &runtime,
                           int requestId,
                           const QString &content)
{
    return QMetaObject::invokeMethod(&runtime,
                                     "OnVisionAnalysisCompleted",
                                     Qt::DirectConnection,
                                     Q_ARG(int, requestId),
                                     Q_ARG(QString, content));
}

} // namespace

class AgentRuntimeSchedulerTest : public QObject
{
    Q_OBJECT

private slots:
    void ExecutesFanInAfterAllParents();
    void ExecutesReadyNodesInDeclarationOrder();
    void MergesDistinctParentKeysAtJoin();
    void KeepsEqualParentValuesAtJoin();
    void RejectsConflictingParentKeysAtJoin();
    void RejectsRemovedAndWrittenKeyAtJoin();
    void RejectsUnknownJoinStrategy();
    void AppliesPreferUserJoinStrategy();
    void AppliesConcatJoinStrategy();
    void DoesNotScheduleSuccessorAfterNodeFailure();
    void IsolatesSourceBranchLocalWritesBeforeJoin();
    void IsolatesFanOutBranchLocalWritesBeforeJoin();
    void CommitsConversationHistoryOnlyAfterSuccess();
    void DoesNotLeakFailedBranchOutputIntoNextInvocation();
    void QueuesUserInvocationsInFifoOrder();
    void PrunesSourcesByTrigger();
    void QueuesVisionAfterActiveInvocationWithoutDuplication();
    void StartsQueuedInvocationAfterFailure();
    void UsesLatestSessionHistoryForQueuedVisionInvocation();
    void ExecutesDefaultSingleChainWithUserInput();
    void EmitsOutputForSynchronousTerminalGraph();
    void ResumesAsyncNodeAndRunsSuccessors();
    void ResumesTwoAsyncBranchesOutOfOrder();
    void IsolatesSameRequestIdAcrossClients();
    void StopsInvocationWhenOneAsyncBranchFails();
    void StopsInvocationWhenAsyncRequestTimesOut();
    void IgnoresMismatchedAsyncCallbackWhilePending();
    void SuppressesDuplicateProactiveSummary();
    void SuppressesProactiveSpeechDuringCooldown();
    void DeduplicatesIdenticalPerceptionFrames();
    void DefersSynchronousWebResearchCompletionUntilPendingRegistration();
    void IsolatesWebRequestNamespaceAndSerializesResponse();
    void ContinuesAfterWebResearchFailure();
    void ContinuesAfterWebResearchFailureForVisionPrompt();
    void HandlesSynchronousWebResearchCompletion();
    void HandlesSynchronousWebResearchFailure();
    void DefaultsWebResearchEnginesToBing();
    void RejectsUntriggeredExecuteWhileInvocationIsActive();
    void SkipsWebResearchForVisionTrigger();
    void RejectsLlmJoinWithoutMergePolicy();
};

void AgentRuntimeSchedulerTest::ExecutesFanInAfterAllParents()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source_a","type":"source_a"},{"id":"source_b","type":"source_b"},{"id":"join","type":"join"},{"id":"output","type":"output"}],"edges":[{"from":"source_a","to":"join"},{"from":"source_b","to":"join"},{"from":"join","to":"output"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    QStringList executionTrace;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source_a"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &,
                                                          vpet::AgentContext &,
                                                          QString &)
    {
        executionTrace.append(QStringLiteral("source_a"));
        return true;
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source_b"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &,
                                                          vpet::AgentContext &,
                                                          QString &)
    {
        executionTrace.append(QStringLiteral("source_b"));
        return true;
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("join"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &,
                                                          vpet::AgentContext &,
                                                          QString &)
    {
        if (executionTrace.size() != 2)
        {
            return false;
        }

        executionTrace.append(QStringLiteral("join"));
        return true;
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("output"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &,
                                                          vpet::AgentContext &,
                                                          QString &)
    {
        if (executionTrace != QStringList({QStringLiteral("source_a"),
                                           QStringLiteral("source_b"),
                                           QStringLiteral("join")}))
        {
            return false;
        }

        executionTrace.append(QStringLiteral("output"));
        return true;
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));
    QCOMPARE(executionTrace,
             QStringList({QStringLiteral("source_a"),
                          QStringLiteral("source_b"),
                          QStringLiteral("join"),
                          QStringLiteral("output")}));
}

void AgentRuntimeSchedulerTest::ExecutesReadyNodesInDeclarationOrder()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source","type":"source"},{"id":"later","type":"worker"},{"id":"earlier","type":"worker"},{"id":"join","type":"join"}],"edges":[{"from":"source","to":"later"},{"from":"source","to":"earlier"},{"from":"later","to":"join"},{"from":"earlier","to":"join"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    QStringList executionTrace;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &,
                                                          vpet::AgentContext &,
                                                          QString &)
    {
        executionTrace.append(QStringLiteral("source"));
        return true;
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("worker"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &,
                                                          QString &)
    {
        executionTrace.append(node.id);
        return true;
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("join"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &,
                                                          vpet::AgentContext &,
                                                          QString &)
    {
        if (executionTrace
            != QStringList({QStringLiteral("source"),
                            QStringLiteral("later"),
                            QStringLiteral("earlier")}))
        {
            return false;
        }

        executionTrace.append(QStringLiteral("join"));
        return true;
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));
    QCOMPARE(executionTrace,
             QStringList({QStringLiteral("source"),
                           QStringLiteral("later"),
                           QStringLiteral("earlier"),
                           QStringLiteral("join")}));
}

void AgentRuntimeSchedulerTest::MergesDistinctParentKeysAtJoin()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source_a","type":"source_a"},{"id":"source_b","type":"source_b"},{"id":"join","type":"join"}],"edges":[{"from":"source_a","to":"join"},{"from":"source_b","to":"join"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source_a"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(QStringLiteral("semantic.left"), QStringLiteral("left"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source_b"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(QStringLiteral("semantic.right"), QStringLiteral("right"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("join"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        QVariant leftValue;
        QVariant rightValue;

        return context.GetValue(QStringLiteral("semantic.left"), leftValue)
               && context.GetValue(QStringLiteral("semantic.right"), rightValue)
               && (leftValue.toString() == QStringLiteral("left"))
               && (rightValue.toString() == QStringLiteral("right"));
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));
}

void AgentRuntimeSchedulerTest::KeepsEqualParentValuesAtJoin()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source_a","type":"source"},{"id":"source_b","type":"source"},{"id":"join","type":"join"}],"edges":[{"from":"source_a","to":"join"},{"from":"source_b","to":"join"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(QStringLiteral("semantic.shared"), QStringLiteral("same"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("join"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        QVariant value;

        return context.GetValue(QStringLiteral("semantic.shared"), value)
               && (value.toString() == QStringLiteral("same"));
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));
}

void AgentRuntimeSchedulerTest::RejectsConflictingParentKeysAtJoin()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source_a","type":"source_a"},{"id":"source_b","type":"source_b"},{"id":"join","type":"join"}],"edges":[{"from":"source_a","to":"join"},{"from":"source_b","to":"join"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source_a"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(QStringLiteral("semantic.shared"), QStringLiteral("left"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source_b"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(QStringLiteral("semantic.shared"), QStringLiteral("right"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("join"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &,
                                           QString &)
    {
        return true;
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY(!runtime.Execute(errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("join")));
    QVERIFY(errorMessage.contains(QStringLiteral("semantic.shared")));
    QVERIFY(errorMessage.contains(QStringLiteral("source_a")));
    QVERIFY(errorMessage.contains(QStringLiteral("source_b")));
}

void AgentRuntimeSchedulerTest::RejectsRemovedAndWrittenKeyAtJoin()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"remove_source","type":"remove_source"},{"id":"write_source","type":"write_source"},{"id":"join","type":"join"}],"edges":[{"from":"remove_source","to":"join"},{"from":"write_source","to":"join"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    vpet::AgentContext sessionContext;
    QString errorMessage;

    QVERIFY(sessionContext.SetValue(QStringLiteral("semantic.shared"), QStringLiteral("base")));
    runtime.SetContext(sessionContext);
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("remove_source"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.RemoveValue(QStringLiteral("semantic.shared"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("write_source"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(QStringLiteral("semantic.shared"), QStringLiteral("changed"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("join"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &,
                                           QString &)
    {
        return true;
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY(!runtime.Execute(errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("semantic.shared")));
    QVERIFY(errorMessage.contains(QStringLiteral("remove_source")));
    QVERIFY(errorMessage.contains(QStringLiteral("write_source")));
}

void AgentRuntimeSchedulerTest::RejectsUnknownJoinStrategy()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source_a","type":"source"},{"id":"source_b","type":"source"},{"id":"join","type":"join","config":{"merge":{"semantic.shared":"unknown"}}}],"edges":[{"from":"source_a","to":"join"},{"from":"source_b","to":"join"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(QStringLiteral("semantic.shared"), QStringLiteral("same"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("join"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &,
                                           QString &)
    {
        return true;
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY(!runtime.Execute(errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("unknown")));
    QVERIFY(errorMessage.contains(QStringLiteral("semantic.shared")));
}

void AgentRuntimeSchedulerTest::AppliesPreferUserJoinStrategy()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"user_source","type":"user_source","config":{"trigger":"user"}},{"id":"vision_source","type":"vision_source","config":{"trigger":"vision"}},{"id":"join","type":"join","config":{"merge":{"semantic.shared":"prefer_user"}}}],"edges":[{"from":"user_source","to":"join"},{"from":"vision_source","to":"join"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("user_source"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(QStringLiteral("semantic.shared"), QStringLiteral("user value"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("vision_source"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(QStringLiteral("semantic.shared"), QStringLiteral("vision value"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("join"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        QVariant value;
        QVariant triggerValue;

        return context.GetValue(QStringLiteral("semantic.shared"), value)
               && (value.toString() == QStringLiteral("user value"))
               && context.GetValue(vpet::AgentContextKeys::RUNTIME_TRIGGER_TYPE, triggerValue)
               && (triggerValue.toString() == QStringLiteral("user"));
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.ExecuteWithUserInput(QStringLiteral("hello"), errorMessage),
             qPrintable(errorMessage));
}

void AgentRuntimeSchedulerTest::AppliesConcatJoinStrategy()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source_a","type":"source_a"},{"id":"source_b","type":"source_b"},{"id":"join","type":"join","config":{"merge":{"semantic.text":"concat"}}}],"edges":[{"from":"source_a","to":"join"},{"from":"source_b","to":"join"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source_a"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(QStringLiteral("semantic.text"), QStringLiteral("first"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source_b"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(QStringLiteral("semantic.text"), QStringLiteral("second"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("join"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        QVariant value;

        return context.GetValue(QStringLiteral("semantic.text"), value)
               && (value.toString() == QStringLiteral("first\nsecond"));
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));
}

void AgentRuntimeSchedulerTest::DoesNotScheduleSuccessorAfterNodeFailure()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source","type":"source"},{"id":"successor","type":"successor"}],"edges":[{"from":"source","to":"successor"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    bool successorExecuted = false;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &,
                                           QString &handlerErrorMessage)
    {
        handlerErrorMessage = QStringLiteral("source failed");
        return false;
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("successor"),
                                        [&successorExecuted](const vpet::_tagAgentDagNode &,
                                                             vpet::AgentContext &,
                                                             QString &)
    {
        successorExecuted = true;
        return true;
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    errorMessage.clear();
    QVERIFY(!runtime.Execute(errorMessage));
    QCOMPARE(errorMessage, QStringLiteral("source failed"));
    QVERIFY(!successorExecuted);
}

void AgentRuntimeSchedulerTest::IsolatesSourceBranchLocalWritesBeforeJoin()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source_a","type":"source_a"},{"id":"source_b","type":"source_b"},{"id":"join","type":"join"}],"edges":[{"from":"source_a","to":"join"},{"from":"source_b","to":"join"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    bool sourceBObservedSourceAValue = false;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source_a"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(QStringLiteral("semantic.branch.value"),
                                QStringLiteral("from_a"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source_b"),
                                        [&sourceBObservedSourceAValue](const vpet::_tagAgentDagNode &,
                                                                       vpet::AgentContext &context,
                                                                       QString &)
    {
        QVariant value;

        sourceBObservedSourceAValue = context.GetValue(QStringLiteral("semantic.branch.value"),
                                                        value)
                                      && (value.toString() == QStringLiteral("from_a"));

        return context.SetValue(QStringLiteral("semantic.branch.value"),
                                QStringLiteral("from_b"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("join"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &,
                                           QString &)
    {
        return false;
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    errorMessage.clear();
    QVERIFY(!runtime.Execute(errorMessage));
    QVERIFY(!sourceBObservedSourceAValue);
    QVERIFY(errorMessage.contains(QStringLiteral("semantic.branch.value")));
}

void AgentRuntimeSchedulerTest::IsolatesFanOutBranchLocalWritesBeforeJoin()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source","type":"source"},{"id":"left","type":"left"},{"id":"right","type":"right"},{"id":"join","type":"join"}],"edges":[{"from":"source","to":"left"},{"from":"source","to":"right"},{"from":"left","to":"join"},{"from":"right","to":"join"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    bool rightObservedLeftValue = false;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(QStringLiteral("semantic.branch.value"),
                                QStringLiteral("from_source"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("left"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(QStringLiteral("semantic.branch.value"),
                                QStringLiteral("from_left"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("right"),
                                        [&rightObservedLeftValue](const vpet::_tagAgentDagNode &,
                                                                  vpet::AgentContext &context,
                                                                  QString &)
    {
        QVariant value;

        rightObservedLeftValue = context.GetValue(QStringLiteral("semantic.branch.value"), value)
                                 && (value.toString() == QStringLiteral("from_left"));

        return context.SetValue(QStringLiteral("semantic.branch.value"),
                                QStringLiteral("from_right"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("join"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &,
                                           QString &)
    {
        return false;
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    errorMessage.clear();
    QVERIFY(!runtime.Execute(errorMessage));
    QVERIFY(!rightObservedLeftValue);
    QVERIFY(errorMessage.contains(QStringLiteral("semantic.branch.value")));
}

void AgentRuntimeSchedulerTest::CommitsConversationHistoryOnlyAfterSuccess()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source","type":"source"},{"id":"terminal","type":"terminal"}],"edges":[{"from":"source","to":"terminal"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    int invocationCount = 0;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source"),
                                        [&invocationCount](const vpet::_tagAgentDagNode &,
                                                           vpet::AgentContext &context,
                                                           QString &)
    {
        ++invocationCount;

        if (invocationCount == 1)
        {
            return context.SetValue(vpet::AgentContextKeys::CONVERSATION_HISTORY,
                                    QStringList({QStringLiteral("assistant: first")}));
        }

        QVariant historyValue;

        return context.GetValue(vpet::AgentContextKeys::CONVERSATION_HISTORY, historyValue)
               && (historyValue.toStringList()
                   == QStringList({QStringLiteral("assistant: first")}));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("terminal"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &,
                                           QString &)
    {
        return true;
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));
    QCOMPARE(invocationCount, 2);
}

void AgentRuntimeSchedulerTest::DoesNotLeakFailedBranchOutputIntoNextInvocation()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source","type":"source"}],"edges":[]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    int invocationCount = 0;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source"),
                                        [&invocationCount](const vpet::_tagAgentDagNode &,
                                                           vpet::AgentContext &context,
                                                           QString &handlerErrorMessage)
    {
        ++invocationCount;

        if (invocationCount == 1)
        {
            if (!context.SetValue(QStringLiteral("semantic.failed.value"),
                                  QStringLiteral("must_not_leak")))
            {
                return false;
            }

            handlerErrorMessage = QStringLiteral("expected failure");
            return false;
        }

        return !context.Contains(QStringLiteral("semantic.failed.value"));
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY(!runtime.Execute(errorMessage));
    QCOMPARE(errorMessage, QStringLiteral("expected failure"));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));
    QCOMPARE(invocationCount, 2);
}

void AgentRuntimeSchedulerTest::QueuesUserInvocationsInFifoOrder()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source","type":"source"}],"edges":[]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    QStringList observedInputs;
    int requestId = 0;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source"),
                                        [&observedInputs, &requestId](const vpet::_tagAgentDagNode &,
                                                                      vpet::AgentContext &context,
                                                                      QString &)
    {
        observedInputs.append(context.GetUserInput());
        ++requestId;

        return context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING, true)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_ID,
                                    QStringLiteral("source"))
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_TYPE,
                                    QStringLiteral("source"))
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_REQUEST_ID,
                                   requestId);
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.ExecuteWithUserInput(QStringLiteral("first"), errorMessage),
             qPrintable(errorMessage));
    QVERIFY(runtime.HasPendingAsyncRequest());

    QVERIFY2(runtime.ExecuteWithUserInput(QStringLiteral("second"), errorMessage),
             qPrintable(errorMessage));
    QCOMPARE(observedInputs, QStringList({QStringLiteral("first")}));

    QVERIFY(InvokeLlmCompleted(runtime, 1, QStringLiteral("first reply")));
    QCoreApplication::processEvents();
    QCOMPARE(observedInputs,
             QStringList({QStringLiteral("first"), QStringLiteral("second")}));
    QVERIFY(runtime.HasPendingAsyncRequest());

    QVERIFY(InvokeLlmCompleted(runtime, 2, QStringLiteral("second reply")));
    QCoreApplication::processEvents();
    QVERIFY(!runtime.HasPendingAsyncRequest());
}

void AgentRuntimeSchedulerTest::PrunesSourcesByTrigger()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"user_source","type":"source","config":{"trigger":"user"}},{"id":"vision_source","type":"source","config":{"trigger":"vision"}},{"id":"user_output","type":"output"},{"id":"vision_output","type":"output"}],"edges":[{"from":"user_source","to":"user_output"},{"from":"vision_source","to":"vision_output"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    QStringList executionTrace;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &,
                                                          QString &)
    {
        executionTrace.append(node.id);
        return true;
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("output"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &,
                                                          QString &)
    {
        executionTrace.append(node.id);
        return true;
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.ExecuteWithUserInput(QStringLiteral("hello"), errorMessage),
             qPrintable(errorMessage));
    QCOMPARE(executionTrace,
             QStringList({QStringLiteral("user_source"), QStringLiteral("user_output")}));

    executionTrace.clear();
    QVERIFY2(runtime.UpdatePerceptionFrame(QByteArrayLiteral("image"),
                                           1,
                                           QSize(10, 10),
                                           QStringLiteral("image/png"),
                                           errorMessage),
             qPrintable(errorMessage));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));
    QCOMPARE(executionTrace,
             QStringList({QStringLiteral("vision_source"), QStringLiteral("vision_output")}));
}

void AgentRuntimeSchedulerTest::QueuesVisionAfterActiveInvocationWithoutDuplication()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"user_source","type":"source","config":{"trigger":"user"}},{"id":"vision_source","type":"source","config":{"trigger":"vision"}},{"id":"user_done","type":"async_worker"},{"id":"vision_done","type":"async_worker"}],"edges":[{"from":"user_source","to":"user_done"},{"from":"vision_source","to":"vision_done"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    QStringList executionTrace;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &,
                                                          QString &)
    {
        executionTrace.append(node.id);
        return true;
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("async_worker"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &context,
                                                          QString &)
    {
        executionTrace.append(node.id);
        const int requestId = (node.id == QStringLiteral("user_done")) ? 81 : 82;

        return context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING, true)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_ID, node.id)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_TYPE,
                                   node.type)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_REQUEST_ID,
                                   requestId);
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.ExecuteWithUserInput(QStringLiteral("hello"), errorMessage),
             qPrintable(errorMessage));

    QVERIFY(runtime.UpdatePerceptionFrame(QByteArrayLiteral("image"),
                                          1,
                                          QSize(10, 10),
                                          QStringLiteral("image/png"),
                                          errorMessage));
    QCOMPARE(runtime.GetContext().GetUserInput(), QStringLiteral("hello"));
    QVariant activeTrigger;
    QVERIFY(runtime.GetContext().GetValue(vpet::AgentContextKeys::RUNTIME_TRIGGER_TYPE,
                                          activeTrigger));
    QCOMPARE(activeTrigger.toString(), QStringLiteral("user"));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));
    QCOMPARE(executionTrace,
             QStringList({QStringLiteral("user_source"), QStringLiteral("user_done")}));

    QVERIFY(InvokeLlmCompleted(runtime, 81, QStringLiteral("user reply")));
    QCoreApplication::processEvents();
    QCOMPARE(executionTrace,
             QStringList({QStringLiteral("user_source"),
                          QStringLiteral("user_done"),
                          QStringLiteral("vision_source"),
                          QStringLiteral("vision_done")}));
}

void AgentRuntimeSchedulerTest::StartsQueuedInvocationAfterFailure()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source","type":"async_worker"}],"edges":[]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    int invocationCount = 0;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("async_worker"),
                                        [&invocationCount](const vpet::_tagAgentDagNode &node,
                                                           vpet::AgentContext &context,
                                                           QString &)
    {
        ++invocationCount;
        const int requestId = (invocationCount == 1) ? 91 : 92;

        return context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING, true)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_ID, node.id)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_TYPE,
                                   node.type)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_REQUEST_ID,
                                   requestId);
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.ExecuteWithUserInput(QStringLiteral("queued"), errorMessage),
             qPrintable(errorMessage));
    QVERIFY(InvokeLlmFailed(runtime, 91, QStringLiteral("failed"), 500));
    QCoreApplication::processEvents();

    QCOMPARE(invocationCount, 2);
    QVERIFY(runtime.HasPendingAsyncRequest());
    QVERIFY(InvokeLlmCompleted(runtime, 92, QStringLiteral("recovered")));
    QCoreApplication::processEvents();
    QVERIFY(!runtime.HasPendingAsyncRequest());
}

void AgentRuntimeSchedulerTest::UsesLatestSessionHistoryForQueuedVisionInvocation()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"user_source","type":"user_source","config":{"trigger":"user"}},{"id":"vision_source","type":"vision_source","config":{"trigger":"vision"}},{"id":"user_terminal","type":"user_terminal"},{"id":"vision_terminal","type":"vision_terminal"}],"edges":[{"from":"user_source","to":"user_terminal"},{"from":"vision_source","to":"vision_terminal"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    bool visionReadLatestHistory = false;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("user_source"),
                                        [](const vpet::_tagAgentDagNode &node,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING, true)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_ID, node.id)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_TYPE,
                                   node.type)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_REQUEST_ID, 101);
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("user_terminal"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(vpet::AgentContextKeys::CONVERSATION_HISTORY,
                                QStringList({QStringLiteral("user: hello"),
                                             QStringLiteral("assistant: reply")}));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("vision_source"),
                                        [&visionReadLatestHistory](const vpet::_tagAgentDagNode &,
                                                                  vpet::AgentContext &context,
                                                                  QString &)
    {
        QVariant historyValue;

        visionReadLatestHistory = !context.Contains(vpet::AgentContextKeys::USER_INPUT)
                                  && context.GetValue(
                                      vpet::AgentContextKeys::CONVERSATION_HISTORY,
                                      historyValue)
                                  && (historyValue.toStringList()
                                      == QStringList({QStringLiteral("user: hello"),
                                                      QStringLiteral("assistant: reply")}));
        return visionReadLatestHistory;
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("vision_terminal"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &,
                                           QString &)
    {
        return true;
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.ExecuteWithUserInput(QStringLiteral("hello"), errorMessage),
             qPrintable(errorMessage));
    QVERIFY(runtime.UpdatePerceptionFrame(QByteArrayLiteral("image"),
                                          2,
                                          QSize(10, 10),
                                          QStringLiteral("image/png"),
                                          errorMessage));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));

    QVERIFY(InvokeLlmCompleted(runtime, 101, QStringLiteral("reply")));
    QCoreApplication::processEvents();
    QVERIFY(visionReadLatestHistory);
}

void AgentRuntimeSchedulerTest::ExecutesDefaultSingleChainWithUserInput()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, BuildDefaultSingleChainDagJson(), configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    QStringList executionTrace;
    QString outputContent;
    QString outputSource;
    int outputRequestId = -1;

    QObject::connect(&runtime,
                     &vpet::AgentRuntime::AgentOutputReady,
                     [&outputRequestId, &outputContent, &outputSource](int requestId,
                                                                       const QString &content,
                                                                       const QString &source)
    {
        outputRequestId = requestId;
        outputContent = content;
        outputSource = source;
    });

    QVERIFY(runtime.RegisterNodeHandler(vpet::AgentContextKeys::NODE_TYPE_VISION_INPUT,
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &,
                                                          QString &)
    {
        executionTrace.append(node.id);
        return true;
    }));
    QVERIFY(runtime.RegisterNodeHandler(vpet::AgentContextKeys::NODE_TYPE_VISION_LLM,
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &,
                                                          QString &)
    {
        executionTrace.append(node.id);
        return true;
    }));
    QVERIFY(runtime.RegisterNodeHandler(vpet::AgentContextKeys::NODE_TYPE_PROACTIVE_TOPIC,
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &,
                                                          QString &)
    {
        executionTrace.append(node.id);
        return true;
    }));
    QVERIFY(runtime.RegisterNodeHandler(vpet::AgentContextKeys::NODE_TYPE_LLM_CHAT,
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &context,
                                                          QString &)
    {
        executionTrace.append(node.id);

        return context.SetValue(vpet::AgentContextKeys::LLM_LAST_RESPONSE,
                                QStringLiteral("llm reply"))
               && context.SetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE,
                                   QStringLiteral("llm reply"))
               && context.SetValue(vpet::AgentContextKeys::NODE_OUTPUT_TEXT_RESPONSE,
                                   QStringLiteral("llm reply"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(vpet::AgentContextKeys::NODE_TYPE_EMOTION_REWRITE,
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &context,
                                                          QString &)
    {
        executionTrace.append(node.id);

        return context.SetValue(vpet::AgentContextKeys::EMOTION_OUTPUT_TEXT,
                                QStringLiteral("emotion reply"))
               && context.SetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE,
                                   QStringLiteral("emotion reply"))
               && context.SetValue(vpet::AgentContextKeys::NODE_OUTPUT_TEXT_RESPONSE,
                                   QStringLiteral("emotion reply"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(vpet::AgentContextKeys::NODE_TYPE_OUTPUT_FORMAT,
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &context,
                                                          QString &)
    {
        executionTrace.append(node.id);

        QVariant responseValue;

        if (!context.GetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE, responseValue))
        {
            return false;
        }

        const QString responseText = responseValue.toString();

        if (!context.SetValue(vpet::AgentContextKeys::OUTPUT_TEXT, responseText)
            || !context.SetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_FINAL, responseText)
            || !context.SetValue(vpet::AgentContextKeys::NODE_OUTPUT_TEXT_FINAL, responseText)
            || !context.SetValue(vpet::AgentContextKeys::SEMANTIC_OUTPUT_SOURCE,
                                 QStringLiteral("user_response")))
        {
            return false;
        }

        QVariant historyValue;
        QStringList history;

        if (context.GetValue(vpet::AgentContextKeys::CONVERSATION_HISTORY, historyValue))
        {
            history = historyValue.toStringList();
        }

        history.append(QStringLiteral("user: hello"));
        history.append(QStringLiteral("assistant: %1").arg(responseText));

        return context.SetValue(vpet::AgentContextKeys::CONVERSATION_HISTORY, history);
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QCOMPARE(runtime.GetExecutionOrder(),
             QVector<QString>({QStringLiteral("vision_input"),
                               QStringLiteral("vision_llm"),
                               QStringLiteral("proactive_topic"),
                               QStringLiteral("call_llm"),
                               QStringLiteral("emotion_rewrite"),
                               QStringLiteral("format_output")}));
    QVERIFY2(runtime.ExecuteWithUserInput(QStringLiteral("hello"), errorMessage),
             qPrintable(errorMessage));
    QCOMPARE(executionTrace,
             QStringList({QStringLiteral("vision_input"),
                          QStringLiteral("vision_llm"),
                          QStringLiteral("proactive_topic"),
                          QStringLiteral("call_llm"),
                          QStringLiteral("emotion_rewrite"),
                          QStringLiteral("format_output")}));

    QVariant finalValue;
    QVariant sourceValue;
    QVariant historyValue;

    QVERIFY(runtime.GetContext().GetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_FINAL, finalValue));
    QCOMPARE(finalValue.toString(), QStringLiteral("emotion reply"));
    QVERIFY(runtime.GetContext().GetValue(vpet::AgentContextKeys::SEMANTIC_OUTPUT_SOURCE, sourceValue));
    QCOMPARE(sourceValue.toString(), QStringLiteral("user_response"));
    QVERIFY(runtime.GetContext().GetValue(vpet::AgentContextKeys::CONVERSATION_HISTORY, historyValue));
    QCOMPARE(historyValue.toStringList(),
             QStringList({QStringLiteral("user: hello"),
                          QStringLiteral("assistant: emotion reply")}));
    QVERIFY(!runtime.HasPendingAsyncRequest());
    QVERIFY(!runtime.GetContext().Contains(vpet::AgentContextKeys::USER_INPUT));
    QVERIFY(!runtime.GetContext().Contains(vpet::AgentContextKeys::RUNTIME_TRIGGER_TYPE));
    QCOMPARE(outputRequestId, 1);
    QCOMPARE(outputContent, QStringLiteral("emotion reply"));
    QCOMPARE(outputSource, QStringLiteral("user_response"));
}

void AgentRuntimeSchedulerTest::EmitsOutputForSynchronousTerminalGraph()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"terminal","type":"sync_terminal"}],"edges":[]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    QString outputContent;
    QString outputSource;
    int outputRequestId = -1;

    QObject::connect(&runtime,
                     &vpet::AgentRuntime::AgentOutputReady,
                     [&outputRequestId, &outputContent, &outputSource](int requestId,
                                                                       const QString &content,
                                                                       const QString &source)
    {
        outputRequestId = requestId;
        outputContent = content;
        outputSource = source;
    });

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("sync_terminal"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(vpet::AgentContextKeys::OUTPUT_TEXT,
                                QStringLiteral("sync reply"))
               && context.SetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_FINAL,
                                   QStringLiteral("sync reply"));
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.UpdatePerceptionFrame(QByteArrayLiteral("image"),
                                           1,
                                           QSize(10, 10),
                                           QStringLiteral("image/png"),
                                           errorMessage),
             qPrintable(errorMessage));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));

    QCOMPARE(outputRequestId, 1);
    QCOMPARE(outputContent, QStringLiteral("sync reply"));
    QCOMPARE(outputSource, QStringLiteral("vision_proactive"));
}

void AgentRuntimeSchedulerTest::ResumesAsyncNodeAndRunsSuccessors()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source","type":"async_source"},{"id":"successor","type":"successor"}],"edges":[{"from":"source","to":"successor"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    QStringList executionTrace;
    QString outputContent;
    QString outputSource;
    int outputRequestId = -1;

    QObject::connect(&runtime,
                     &vpet::AgentRuntime::AgentOutputReady,
                     [&outputRequestId, &outputContent, &outputSource](int requestId,
                                                                       const QString &content,
                                                                       const QString &source)
    {
        outputRequestId = requestId;
        outputContent = content;
        outputSource = source;
    });

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("async_source"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &context,
                                                          QString &)
    {
        executionTrace.append(node.id);

        return context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING, true)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_ID, node.id)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_TYPE,
                                   QStringLiteral("async_source"))
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_REQUEST_ID, 42)
               && context.SetValue(vpet::AgentContextKeys::LLM_PENDING, true);
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("successor"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &context,
                                                          QString &)
    {
        executionTrace.append(node.id);

        QVariant responseValue;

        if (!context.GetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE, responseValue))
        {
            return false;
        }

        const QString responseText = responseValue.toString();

        return context.SetValue(vpet::AgentContextKeys::OUTPUT_TEXT, responseText)
               && context.SetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_FINAL, responseText)
               && context.SetValue(vpet::AgentContextKeys::SEMANTIC_OUTPUT_SOURCE,
                                   QStringLiteral("user_response"));
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.ExecuteWithUserInput(QStringLiteral("hello"), errorMessage),
             qPrintable(errorMessage));
    QCOMPARE(executionTrace, QStringList({QStringLiteral("source")}));
    QVERIFY(runtime.HasPendingAsyncRequest());

    QVERIFY(InvokeLlmCompleted(runtime, 42, QStringLiteral("async reply")));
    QCOMPARE(executionTrace,
             QStringList({QStringLiteral("source"), QStringLiteral("successor")}));
    QVERIFY(!runtime.HasPendingAsyncRequest());

    QVariant finalValue;
    QVariant sourceValue;

    QVERIFY(runtime.GetContext().GetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_FINAL, finalValue));
    QCOMPARE(finalValue.toString(), QStringLiteral("async reply"));
    QVERIFY(runtime.GetContext().GetValue(vpet::AgentContextKeys::SEMANTIC_OUTPUT_SOURCE, sourceValue));
    QCOMPARE(sourceValue.toString(), QStringLiteral("user_response"));
    QCOMPARE(outputRequestId, 42);
    QCOMPARE(outputContent, QStringLiteral("async reply"));
    QCOMPARE(outputSource, QStringLiteral("user_response"));
}

void AgentRuntimeSchedulerTest::ResumesTwoAsyncBranchesOutOfOrder()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source","type":"source"},{"id":"left_async","type":"async_worker"},{"id":"right_async","type":"async_worker"},{"id":"left_done","type":"done_worker"},{"id":"right_done","type":"done_worker"},{"id":"join","type":"join"}],"edges":[{"from":"source","to":"left_async"},{"from":"source","to":"right_async"},{"from":"left_async","to":"left_done"},{"from":"right_async","to":"right_done"},{"from":"left_done","to":"join"},{"from":"right_done","to":"join"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    QStringList executionTrace;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &,
                                                          QString &)
    {
        executionTrace.append(node.id);
        return true;
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("async_worker"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &context,
                                                          QString &)
    {
        executionTrace.append(node.id);
        const int requestId = (node.id == QStringLiteral("left_async")) ? 41 : 42;

        return context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING, true)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_ID, node.id)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_TYPE, node.type)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_REQUEST_ID, requestId)
               && context.SetValue(vpet::AgentContextKeys::LLM_PENDING, true);
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("done_worker"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &context,
                                                          QString &)
    {
        QVariant responseValue;

        if (!context.GetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE, responseValue))
        {
            return false;
        }

        executionTrace.append(node.id);
        const QString outputKey = (node.id == QStringLiteral("left_done"))
                                      ? QStringLiteral("semantic.left")
                                      : QStringLiteral("semantic.right");

        context.RemoveValue(vpet::AgentContextKeys::LLM_LAST_REQUEST_ID);
        context.RemoveValue(vpet::AgentContextKeys::LLM_LAST_RESPONSE);
        context.RemoveValue(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE);
        context.RemoveValue(vpet::AgentContextKeys::NODE_OUTPUT_TEXT_RESPONSE);
        context.RemoveValue(vpet::AgentContextKeys::LLM_PENDING);
        return context.SetValue(outputKey, responseValue);
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("join"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &context,
                                                          QString &)
    {
        QVariant leftValue;
        QVariant rightValue;

        executionTrace.append(node.id);
        return context.GetValue(QStringLiteral("semantic.left"), leftValue)
               && context.GetValue(QStringLiteral("semantic.right"), rightValue)
               && (leftValue.toString() == QStringLiteral("left reply"))
               && (rightValue.toString() == QStringLiteral("right reply"));
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));
    QCOMPARE(executionTrace,
             QStringList({QStringLiteral("source"),
                          QStringLiteral("left_async"),
                          QStringLiteral("right_async")}));
    QVERIFY(runtime.HasPendingAsyncRequest());

    QVERIFY(InvokeLlmCompleted(runtime, 42, QStringLiteral("right reply")));
    QCOMPARE(executionTrace.last(), QStringLiteral("right_done"));
    QVERIFY(runtime.HasPendingAsyncRequest());

    QVERIFY(InvokeLlmCompleted(runtime, 41, QStringLiteral("left reply")));
    QCOMPARE(executionTrace,
             QStringList({QStringLiteral("source"),
                          QStringLiteral("left_async"),
                          QStringLiteral("right_async"),
                          QStringLiteral("right_done"),
                          QStringLiteral("left_done"),
                          QStringLiteral("join")}));
    QVERIFY(!runtime.HasPendingAsyncRequest());

    QVERIFY(InvokeLlmCompleted(runtime, 42, QStringLiteral("duplicate reply")));
    QCOMPARE(executionTrace.last(), QStringLiteral("join"));
}

void AgentRuntimeSchedulerTest::IsolatesSameRequestIdAcrossClients()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"text_async","type":"text_async"},{"id":"vision_async","type":"vision.llm"},{"id":"join","type":"join"}],"edges":[{"from":"text_async","to":"join"},{"from":"vision_async","to":"join"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    bool joinExecuted = false;

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("text_async"),
                                        [](const vpet::_tagAgentDagNode &node,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING, true)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_ID, node.id)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_TYPE, node.type)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_REQUEST_ID, 77);
    }));
    QVERIFY(runtime.RegisterNodeHandler(vpet::AgentContextKeys::NODE_TYPE_VISION_LLM,
                                        [](const vpet::_tagAgentDagNode &node,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING, true)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_ID, node.id)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_TYPE, node.type)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_REQUEST_ID, 77);
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("join"),
                                        [&joinExecuted](const vpet::_tagAgentDagNode &,
                                                        vpet::AgentContext &context,
                                                        QString &)
    {
        QVariant textValue;
        QVariant visionValue;

        joinExecuted = context.GetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_RESPONSE,
                                        textValue)
                       && context.GetValue(vpet::AgentContextKeys::SEMANTIC_VISION_SUMMARY,
                                           visionValue)
                       && (textValue.toString() == QStringLiteral("text reply"))
                       && (visionValue.toString() == QStringLiteral("vision reply"));
        return joinExecuted;
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));
    QVERIFY(runtime.HasPendingAsyncRequest());

    QVERIFY(InvokeVisionCompleted(runtime, 77, QStringLiteral("vision reply")));
    QVERIFY(runtime.HasPendingAsyncRequest());
    QVERIFY(!joinExecuted);

    QVERIFY(InvokeLlmCompleted(runtime, 77, QStringLiteral("text reply")));
    QVERIFY(!runtime.HasPendingAsyncRequest());
    QVERIFY(joinExecuted);
}

void AgentRuntimeSchedulerTest::StopsInvocationWhenOneAsyncBranchFails()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"left","type":"async_worker"},{"id":"right","type":"async_worker"},{"id":"join","type":"join"}],"edges":[{"from":"left","to":"join"},{"from":"right","to":"join"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    bool joinExecuted = false;
    bool failureSeen = false;

    QObject::connect(&runtime,
                     &vpet::AgentRuntime::LlmRequestFailed,
                     [&failureSeen](int requestId, const QString &, int)
    {
        failureSeen = (requestId == 51);
    });
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("async_worker"),
                                        [](const vpet::_tagAgentDagNode &node,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        const int requestId = (node.id == QStringLiteral("left")) ? 51 : 52;

        return context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING, true)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_ID, node.id)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_TYPE, node.type)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_REQUEST_ID, requestId);
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("join"),
                                        [&joinExecuted](const vpet::_tagAgentDagNode &,
                                                        vpet::AgentContext &,
                                                        QString &)
    {
        joinExecuted = true;
        return true;
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));
    QVERIFY(runtime.HasPendingAsyncRequest());
    QVERIFY(InvokeLlmFailed(runtime, 51, QStringLiteral("expected failure"), 500));
    QVERIFY(failureSeen);
    QVERIFY(!runtime.HasPendingAsyncRequest());
    QVERIFY(!joinExecuted);

    QVERIFY(InvokeLlmCompleted(runtime, 52, QStringLiteral("late reply")));
    QVERIFY(!joinExecuted);
}

void AgentRuntimeSchedulerTest::StopsInvocationWhenAsyncRequestTimesOut()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source","type":"async_source","config":{"async_timeout_ms":20}},{"id":"successor","type":"successor"}],"edges":[{"from":"source","to":"successor"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    bool successorExecuted = false;
    bool timeoutSeen = false;

    QObject::connect(&runtime,
                     &vpet::AgentRuntime::LlmRequestFailed,
                     [&timeoutSeen](int requestId, const QString &message, int)
    {
        timeoutSeen = (requestId == 61)
                      && message.contains(QStringLiteral("timed out"));
    });
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("async_source"),
                                        [](const vpet::_tagAgentDagNode &node,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING, true)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_ID, node.id)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_TYPE, node.type)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_REQUEST_ID, 61);
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("successor"),
                                        [&successorExecuted](const vpet::_tagAgentDagNode &,
                                                             vpet::AgentContext &,
                                                             QString &)
    {
        successorExecuted = true;
        return true;
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));
    QVERIFY(runtime.HasPendingAsyncRequest());
    QTRY_VERIFY_WITH_TIMEOUT(timeoutSeen, 500);
    QVERIFY(!runtime.HasPendingAsyncRequest());
    QVERIFY(!successorExecuted);

    QVERIFY(InvokeLlmCompleted(runtime, 61, QStringLiteral("late reply")));
    QVERIFY(!successorExecuted);
}

void AgentRuntimeSchedulerTest::IgnoresMismatchedAsyncCallbackWhilePending()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source","type":"async_source"},{"id":"successor","type":"successor"}],"edges":[{"from":"source","to":"successor"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QString errorMessage;
    QStringList executionTrace;
    bool successorExecuted = false;
    bool failureSeen = false;

    QObject::connect(&runtime,
                     &vpet::AgentRuntime::LlmRequestFailed,
                     [&failureSeen](int, const QString &, int)
    {
        failureSeen = true;
    });

    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("async_source"),
                                        [&executionTrace](const vpet::_tagAgentDagNode &node,
                                                          vpet::AgentContext &context,
                                                          QString &)
    {
        executionTrace.append(node.id);

        return context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING, true)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_ID, node.id)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_TYPE,
                                   QStringLiteral("async_source"))
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_REQUEST_ID, 7)
               && context.SetValue(vpet::AgentContextKeys::LLM_PENDING, true);
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("successor"),
                                        [&successorExecuted, &executionTrace](
                                            const vpet::_tagAgentDagNode &node,
                                            vpet::AgentContext &,
                                            QString &)
    {
        successorExecuted = true;
        executionTrace.append(node.id);
        return true;
    }));

    QVERIFY2(runtime.Load(configPath, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.Execute(errorMessage), qPrintable(errorMessage));
    QVERIFY(runtime.HasPendingAsyncRequest());

    QVERIFY(InvokeLlmCompleted(runtime, 99, QStringLiteral("stale reply")));
    QVERIFY(InvokeLlmFailed(runtime, 99, QStringLiteral("stale failure"), 500));
    QVERIFY(InvokeLlmCompleted(runtime, -1, QStringLiteral("invalid reply")));

    QVERIFY(runtime.HasPendingAsyncRequest());
    QVERIFY(!successorExecuted);
    QVERIFY(!failureSeen);
    QCOMPARE(executionTrace, QStringList({QStringLiteral("source")}));

    QVERIFY(InvokeLlmCompleted(runtime, 7, QStringLiteral("matched reply")));
    QVERIFY(successorExecuted);
    QVERIFY(!runtime.HasPendingAsyncRequest());
    QCOMPARE(executionTrace,
             QStringList({QStringLiteral("source"), QStringLiteral("successor")}));
    QVERIFY(!failureSeen);
}

void AgentRuntimeSchedulerTest::SuppressesDuplicateProactiveSummary()
{
    vpet::_tagAgentDagNode node;
    node.id = QStringLiteral("proactive");
    node.type = QStringLiteral("proactive.topic");
    node.config.insert(QStringLiteral("dedup_window_ms"), 300000);
    node.config.insert(QStringLiteral("min_interval_ms"), 0);

    vpet::AgentContext context;
    const QString summary = QStringLiteral("正在编辑代码");
    const QString summaryHash = QString::fromLatin1(
        QCryptographicHash::hash(summary.toUtf8(), QCryptographicHash::Sha256).toHex());
    QVERIFY(context.SetValue(vpet::AgentContextKeys::SEMANTIC_VISION_SUMMARY, summary));
    QVERIFY(context.SetValue(vpet::AgentContextKeys::PROACTIVE_LAST_SUMMARY_HASH, summaryHash));
    QVERIFY(context.SetValue(vpet::AgentContextKeys::PROACTIVE_LAST_SPOKEN_AT,
                             QDateTime::currentDateTimeUtc().toMSecsSinceEpoch() - 1000));

    QString errorMessage;
    QVERIFY(vpet::ProactiveTopicNode::Execute(node, context, errorMessage));

    QVariant shouldSpeakValue;
    QVariant reasonValue;
    QVERIFY(context.GetValue(vpet::AgentContextKeys::SEMANTIC_PROACTIVE_SHOULD_SPEAK,
                             shouldSpeakValue));
    QVERIFY(context.GetValue(vpet::AgentContextKeys::SEMANTIC_PROACTIVE_REASON,
                             reasonValue));
    QVERIFY(!shouldSpeakValue.toBool());
    QCOMPARE(reasonValue.toString(), QStringLiteral("duplicate_summary"));
}

void AgentRuntimeSchedulerTest::SuppressesProactiveSpeechDuringCooldown()
{
    vpet::_tagAgentDagNode node;
    node.id = QStringLiteral("proactive");
    node.type = QStringLiteral("proactive.topic");
    node.config.insert(QStringLiteral("dedup_window_ms"), 0);
    node.config.insert(QStringLiteral("min_interval_ms"), 300000);

    vpet::AgentContext context;
    QVERIFY(context.SetValue(vpet::AgentContextKeys::SEMANTIC_VISION_SUMMARY,
                             QStringLiteral("正在阅读文档")));
    QVERIFY(context.SetValue(vpet::AgentContextKeys::PROACTIVE_LAST_SPOKEN_AT,
                             QDateTime::currentDateTimeUtc().toMSecsSinceEpoch() - 1000));

    QString errorMessage;
    QVERIFY(vpet::ProactiveTopicNode::Execute(node, context, errorMessage));

    QVariant shouldSpeakValue;
    QVariant reasonValue;
    QVERIFY(context.GetValue(vpet::AgentContextKeys::SEMANTIC_PROACTIVE_SHOULD_SPEAK,
                             shouldSpeakValue));
    QVERIFY(context.GetValue(vpet::AgentContextKeys::SEMANTIC_PROACTIVE_REASON,
                             reasonValue));
    QVERIFY(!shouldSpeakValue.toBool());
    QCOMPARE(reasonValue.toString(), QStringLiteral("cooldown"));
}

void AgentRuntimeSchedulerTest::DeduplicatesIdenticalPerceptionFrames()
{
    vpet::AgentRuntime runtime;
    QString errorMessage;
    const QByteArray frameData = QByteArrayLiteral("same-frame");

    QVERIFY(runtime.UpdatePerceptionFrame(frameData,
                                          1,
                                          QSize(100, 100),
                                          QStringLiteral("vision/screenshot"),
                                          errorMessage));
    const vpet::AgentContext firstContext = runtime.GetContext().Snapshot();

    QVERIFY(runtime.UpdatePerceptionFrame(frameData,
                                          2,
                                          QSize(200, 200),
                                          QStringLiteral("vision/screenshot"),
                                          errorMessage));
    const vpet::AgentContext secondContext = runtime.GetContext().Snapshot();
    QVariant frameIdValue;
    QVERIFY(secondContext.GetValue(vpet::AgentContextKeys::VISION_LATEST_FRAME_ID,
                                   frameIdValue));
    QCOMPARE(frameIdValue.toInt(), 1);
    QCOMPARE(secondContext.GetUserInput(), firstContext.GetUserInput());
}

QTEST_MAIN(AgentRuntimeSchedulerTest)

void AgentRuntimeSchedulerTest::DefersSynchronousWebResearchCompletionUntilPendingRegistration()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"user","type":"user.input","config":{"trigger":"user"}},{"id":"research","type":"web.research","config":{"mode":"auto","async_timeout_ms":1000}},{"id":"sink","type":"sink"}],"edges":[{"from":"user","to":"research"},{"from":"research","to":"sink"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::WebResearchEngine researchEngine;
    vpet::AgentRuntime runtime(&researchEngine, nullptr);
    int sinkCount = 0;
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("sink"),
                                        [&sinkCount](const vpet::_tagAgentDagNode &node,
                                                     vpet::AgentContext &context,
                                                     QString &errorMessage)
    {
        (void)context;
        (void)errorMessage;

        if (node.id == QStringLiteral("sink"))
        {
            sinkCount += 1;
        }

        return true;
    }));
    QString errorMessage;
    QVERIFY(runtime.Load(configPath, errorMessage));
    QVERIFY(runtime.ExecuteWithUserInput(QStringLiteral("解释 C++ RAII"), errorMessage));
    QVERIFY(runtime.HasPendingAsyncRequest());
    QTRY_COMPARE(sinkCount, 1);
    QTRY_VERIFY(!runtime.HasPendingAsyncRequest());

    QVariant statusValue;
    QVERIFY(runtime.GetContext().GetValue(
        vpet::AgentContextKeys::SEMANTIC_WEB_RESEARCH_STATUS,
        statusValue));
    QCOMPARE(statusValue.toString(), QStringLiteral("skipped"));
}

void AgentRuntimeSchedulerTest::IsolatesWebRequestNamespaceAndSerializesResponse()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"user","type":"user.input","config":{"trigger":"user"}},{"id":"research","type":"web.research","config":{"mode":"explicit","failure_policy":"continue","async_timeout_ms":1000}},{"id":"sink","type":"sink"}],"edges":[{"from":"user","to":"research"},{"from":"research","to":"sink"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    ControlledWebResearchEngine researchEngine;
    vpet::AgentRuntime runtime(&researchEngine, nullptr);
    int sinkCount = 0;
    QString downstreamPrompt;
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("sink"),
                                        [&sinkCount, &downstreamPrompt](
                                            const vpet::_tagAgentDagNode &node,
                                            vpet::AgentContext &context,
                                            QString &errorMessage)
    {
        (void)errorMessage;
        QVariant promptValue;

        if ((node.id == QStringLiteral("sink"))
            && context.GetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_PROMPT, promptValue))
        {
            sinkCount += 1;
            downstreamPrompt = promptValue.toString();
        }

        return true;
    }));
    QString errorMessage;
    QVERIFY(runtime.Load(configPath, errorMessage));
    QVERIFY(runtime.ExecuteWithUserInput(QStringLiteral("/search Qt 6.9 release"), errorMessage));
    QCOMPARE(researchEngine.startCount, 1);
    QVERIFY(runtime.HasPendingAsyncRequest());

    QVERIFY(InvokeLlmCompleted(runtime,
                               researchEngine.activeResearchId,
                               QStringLiteral("wrong client response")));
    QCOMPARE(sinkCount, 0);
    QVERIFY(runtime.HasPendingAsyncRequest());

    vpet::_tagWebResearchResponse response;
    response.researchId = researchEngine.activeResearchId;
    response.question = QStringLiteral("Qt 6.9 release");
    response.needSearch = true;
    response.status = QStringLiteral("completed");
    response.reason = QStringLiteral("sufficient_evidence");
    response.summary = QStringLiteral("Qt 6.9 is documented at https://doc.qt.io/");
    response.plan.append(QStringLiteral("Qt 6.9 release"));
    response.queries.append(QStringLiteral("Qt 6.9 release"));
    response.citations.append(QStringLiteral("Qt Docs - https://doc.qt.io/"));
    response.roundCount = 1;
    vpet::_tagWebResearchEvidence evidence;
    evidence.claim = QStringLiteral("Qt 6.9 release");
    evidence.sourceTitle = QStringLiteral("Qt Docs");
    evidence.url = QStringLiteral("https://doc.qt.io/");
    evidence.publisher = QStringLiteral("doc.qt.io");
    evidence.snippet = QStringLiteral("Official documentation");
    evidence.sourceTier = QStringLiteral("official");
    evidence.freshness = QStringLiteral("current");
    evidence.confidence = QStringLiteral("high");
    response.evidence.append(evidence);
    researchEngine.Complete(response);

    QCOMPARE(sinkCount, 1);
    QVERIFY(!runtime.HasPendingAsyncRequest());
    QVERIFY(downstreamPrompt.contains(QStringLiteral("外部不可信数据")));
    QVERIFY(downstreamPrompt.contains(QStringLiteral("https://doc.qt.io/")));
    QVariant evidenceValue;
    QVERIFY(runtime.GetContext().GetValue(
        vpet::AgentContextKeys::SEMANTIC_WEB_RESEARCH_EVIDENCE,
        evidenceValue));
    QVERIFY(evidenceValue.toString().contains(QStringLiteral("source_title")));
}

void AgentRuntimeSchedulerTest::SkipsWebResearchForVisionTrigger()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"user","type":"user.input","config":{"trigger":"user"}},{"id":"research","type":"web.research","config":{"mode":"auto"}},{"id":"user_sink","type":"sink"},{"id":"vision","type":"vision.input","config":{"trigger":"vision"}},{"id":"vision_sink","type":"sink"}],"edges":[{"from":"user","to":"research"},{"from":"research","to":"user_sink"},{"from":"vision","to":"vision_sink"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    ControlledWebResearchEngine researchEngine;
    vpet::AgentRuntime runtime(&researchEngine, nullptr);
    QStringList sinkIds;
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("sink"),
                                        [&sinkIds](const vpet::_tagAgentDagNode &node,
                                                   vpet::AgentContext &context,
                                                   QString &errorMessage)
    {
        (void)context;
        (void)errorMessage;
        sinkIds.append(node.id);
        return true;
    }));
    QString errorMessage;
    QVERIFY(runtime.Load(configPath, errorMessage));
    QVERIFY(runtime.UpdatePerceptionFrame(QByteArrayLiteral("frame-data"),
                                          1,
                                          QSize(8, 8),
                                          QStringLiteral("image/png"),
                                          errorMessage));
    QVERIFY(runtime.Execute(errorMessage));
    QCOMPARE(researchEngine.startCount, 0);
    QCOMPARE(sinkIds, QStringList({QStringLiteral("vision_sink")}));
}

void AgentRuntimeSchedulerTest::ContinuesAfterWebResearchFailure()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"user","type":"user.input","config":{"trigger":"user"}},{"id":"research","type":"web.research","config":{"failure_policy":"continue","async_timeout_ms":1000}},{"id":"sink","type":"sink"}],"edges":[{"from":"user","to":"research"},{"from":"research","to":"sink"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    ControlledWebResearchEngine researchEngine;
    vpet::AgentRuntime runtime(&researchEngine, nullptr);
    int sinkCount = 0;
    QString fallbackPrompt;
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("sink"),
                                        [&sinkCount, &fallbackPrompt](
                                            const vpet::_tagAgentDagNode &node,
                                            vpet::AgentContext &context,
                                            QString &errorMessage)
    {
        (void)node;
        (void)errorMessage;
        QVariant promptValue;

        if (context.GetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_PROMPT, promptValue))
        {
            sinkCount += 1;
            fallbackPrompt = promptValue.toString();
        }

        return true;
    }));
    QString errorMessage;
    QVERIFY(runtime.Load(configPath, errorMessage));
    QVERIFY(runtime.ExecuteWithUserInput(QStringLiteral("/search unavailable fact"), errorMessage));
    QVERIFY(runtime.HasPendingAsyncRequest());
    researchEngine.Fail(QStringLiteral("Web search service unavailable."));
    QCOMPARE(sinkCount, 1);
    QVERIFY(!runtime.HasPendingAsyncRequest());
    QVERIFY(fallbackPrompt.contains(QStringLiteral("不得声称已经联网核实")));

    QVariant statusValue;
    QVERIFY(runtime.GetContext().GetValue(
        vpet::AgentContextKeys::SEMANTIC_WEB_RESEARCH_STATUS,
        statusValue));
    QCOMPARE(statusValue.toString(), QStringLiteral("error"));
}

void AgentRuntimeSchedulerTest::ContinuesAfterWebResearchFailureForVisionPrompt()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"vision","type":"vision_source","config":{"trigger":"vision"}},{"id":"research","type":"web.research","config":{"failure_policy":"continue","async_timeout_ms":1000}},{"id":"sink","type":"sink"}],"edges":[{"from":"vision","to":"research"},{"from":"research","to":"sink"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    ControlledWebResearchEngine researchEngine;
    vpet::AgentRuntime runtime(&researchEngine, nullptr);
    QString fallbackPrompt;
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("vision_source"),
                                        [](const vpet::_tagAgentDagNode &,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_PROMPT,
                                QStringLiteral("What is shown in this screen?"));
    }));
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("sink"),
                                        [&fallbackPrompt](const vpet::_tagAgentDagNode &,
                                                          vpet::AgentContext &context,
                                                          QString &)
    {
        QVariant promptValue;

        if (!context.GetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_PROMPT, promptValue))
        {
            return false;
        }

        fallbackPrompt = promptValue.toString();
        return true;
    }));
    QString errorMessage;
    QVERIFY(runtime.Load(configPath, errorMessage));
    QVERIFY(runtime.UpdatePerceptionFrame(QByteArrayLiteral("frame-data"),
                                          1,
                                          QSize(8, 8),
                                          QStringLiteral("image/png"),
                                          errorMessage));
    QVERIFY(runtime.Execute(errorMessage));
    QVERIFY(runtime.HasPendingAsyncRequest());

    researchEngine.Fail(QStringLiteral("Web search service unavailable."));

    QVERIFY(!runtime.HasPendingAsyncRequest());
    QVERIFY(fallbackPrompt.contains(QStringLiteral("What is shown in this screen?")));
    QVERIFY(fallbackPrompt.contains(QStringLiteral("不得声称已经联网核实")));
}

void AgentRuntimeSchedulerTest::HandlesSynchronousWebResearchCompletion()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"user","type":"user.input","config":{"trigger":"user"}},{"id":"research","type":"web.research","config":{"async_timeout_ms":1000}},{"id":"sink","type":"sink"}],"edges":[{"from":"user","to":"research"},{"from":"research","to":"sink"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    SynchronousWebResearchEngine researchEngine(
        SynchronousWebResearchEngine::CompleteImmediately);
    vpet::AgentRuntime runtime(&researchEngine, nullptr);
    int sinkCount = 0;
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("sink"),
                                        [&sinkCount](const vpet::_tagAgentDagNode &,
                                                     vpet::AgentContext &,
                                                     QString &)
    {
        ++sinkCount;
        return true;
    }));
    QString errorMessage;
    QVERIFY(runtime.Load(configPath, errorMessage));
    QVERIFY(runtime.ExecuteWithUserInput(QStringLiteral("Synchronous question"), errorMessage));

    QTRY_COMPARE(sinkCount, 1);
    QVERIFY(!runtime.HasPendingAsyncRequest());
}

void AgentRuntimeSchedulerTest::HandlesSynchronousWebResearchFailure()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"user","type":"user.input","config":{"trigger":"user"}},{"id":"research","type":"web.research","config":{"failure_policy":"continue","async_timeout_ms":1000}},{"id":"sink","type":"sink"}],"edges":[{"from":"user","to":"research"},{"from":"research","to":"sink"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    SynchronousWebResearchEngine researchEngine(SynchronousWebResearchEngine::FailImmediately);
    vpet::AgentRuntime runtime(&researchEngine, nullptr);
    int sinkCount = 0;
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("sink"),
                                        [&sinkCount](const vpet::_tagAgentDagNode &,
                                                     vpet::AgentContext &,
                                                     QString &)
    {
        ++sinkCount;
        return true;
    }));
    QString errorMessage;
    QVERIFY(runtime.Load(configPath, errorMessage));
    QVERIFY(runtime.ExecuteWithUserInput(QStringLiteral("Synchronous question"), errorMessage));

    QTRY_COMPARE(sinkCount, 1);
    QVERIFY(!runtime.HasPendingAsyncRequest());
}

void AgentRuntimeSchedulerTest::DefaultsWebResearchEnginesToBing()
{
    vpet::_tagAgentDagNode node;
    node.id = QStringLiteral("research");
    node.type = vpet::AgentContextKeys::NODE_TYPE_WEB_RESEARCH;
    vpet::AgentContext context;
    QVERIFY(context.SetValue(vpet::AgentContextKeys::SEMANTIC_TEXT_PROMPT,
                             QStringLiteral("Current Qt release")));
    vpet::_tagWebResearchRequest request;
    QString errorMessage;

    QVERIFY(vpet::WebResearchNode::BuildRequest(node, context, request, errorMessage));
    QCOMPARE(request.engines, QStringList({QStringLiteral("bing")}));
}

void AgentRuntimeSchedulerTest::RejectsUntriggeredExecuteWhileInvocationIsActive()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source","type":"async_source"}],"edges":[]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("async_source"),
                                        [](const vpet::_tagAgentDagNode &node,
                                           vpet::AgentContext &context,
                                           QString &)
    {
        return context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING, true)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_ID, node.id)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_NODE_TYPE, node.type)
               && context.SetValue(vpet::AgentContextKeys::RUNTIME_PENDING_REQUEST_ID, 88);
    }));
    QString errorMessage;
    QVERIFY(runtime.Load(configPath, errorMessage));
    QVERIFY(runtime.Execute(errorMessage));
    QVERIFY(runtime.HasPendingAsyncRequest());

    errorMessage.clear();
    QVERIFY(!runtime.Execute(errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("without a trigger")));
}

void AgentRuntimeSchedulerTest::RejectsLlmJoinWithoutMergePolicy()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QByteArray jsonData = QByteArrayLiteral(
        R"({"nodes":[{"id":"source_a","type":"source"},{"id":"source_b","type":"source"},{"id":"chat","type":"llm.chat"}],"edges":[{"from":"source_a","to":"chat"},{"from":"source_b","to":"chat"}]})");
    QString configPath;
    QVERIFY(WriteDagConfig(temporaryDirectory, jsonData, configPath));

    vpet::AgentRuntime runtime;
    QVERIFY(runtime.RegisterNodeHandler(QStringLiteral("source"),
                                        [](const vpet::_tagAgentDagNode &node,
                                           vpet::AgentContext &context,
                                           QString &errorMessage)
    {
        (void)errorMessage;
        return context.SetValue(QStringLiteral("source.%1").arg(node.id), true);
    }));
    QString errorMessage;
    QVERIFY(runtime.Load(configPath, errorMessage));
    QVERIFY(!runtime.Execute(errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("multiple active parents")));
    QVERIFY(errorMessage.contains(QStringLiteral("merge policy")));
}

#include "agent_runtime_scheduler_test.moc"
