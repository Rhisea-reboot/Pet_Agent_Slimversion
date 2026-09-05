#include "vpet/agent/tools/itool.h"
#include "vpet/agent/tools/tool_registry.h"
#include "vpet/agent/tools/tool_scheduler.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest>

namespace
{

/**
 * @brief Fake 工具行为矩阵。
 */
enum class FakeToolBehavior
{
    SucceedImmediately, ///< 同步成功
    FailImmediately,    ///< 同步失败
    TerminateHint,      ///< 成功并请求提前收束
    HangUntilCancelled, ///< 挂起直到被取消（不发结果）
    CompleteAfterDelay  ///< 延迟完成（可模拟超时后的迟到信号）
};

/**
 * @brief 受控 fake 工具，派生自真实 ITool（对齐项目测试风格）。
 */
class FakeTool : public vpet::ITool
{
    Q_OBJECT

public:
    FakeTool(const QString &name,
             vpet::ToolTrustTier tier,
             FakeToolBehavior behavior,
             int delayMs = 0,
             QObject *parent = nullptr)
        : vpet::ITool(parent)
        , m_behavior(behavior)
        , m_delayMs(delayMs)
        , m_spec()
        , m_busy(false)
        , m_executeCount(0)
        , m_cancelCount(0)
    {
        m_spec.name = name;
        m_spec.label = name;
        m_spec.description = QStringLiteral("Fake tool %1").arg(name);
        m_spec.trustTier = tier;

        vpet::_tagToolParameterSchema query;
        query.name = QStringLiteral("query");
        query.type = QStringLiteral("string");
        query.description = QStringLiteral("查询文本");
        query.required = true;
        m_spec.parameters.append(query);

        vpet::_tagToolParameterSchema limit;
        limit.name = QStringLiteral("limit");
        limit.type = QStringLiteral("integer");
        limit.description = QStringLiteral("数量上限");
        m_spec.parameters.append(limit);

        vpet::_tagToolParameterSchema engine;
        engine.name = QStringLiteral("engine");
        engine.type = QStringLiteral("string");
        engine.description = QStringLiteral("引擎");
        engine.enumValues << QStringLiteral("bing") << QStringLiteral("google");
        m_spec.parameters.append(engine);
    }

    vpet::_tagToolSpec Spec() const override
    {
        return m_spec;
    }

    bool ValidateArguments(const QJsonObject &arguments,
                           QString &errorMessage) const override
    {
        return vpet::ValidateToolArguments(m_spec, arguments, errorMessage);
    }

    void Execute(const vpet::_tagToolCall &call) override
    {
        m_busy = true;
        m_executeCount += 1;
        m_lastCall = call;

        switch (m_behavior)
        {
        case FakeToolBehavior::SucceedImmediately:
        {
            vpet::_tagToolExecutionResult result;
            result.ok = true;
            result.textOutput = QStringLiteral("fake ok for %1").arg(call.toolName);
            m_busy = false;
            result.executionId = call.executionId;
            emit Completed(result);
            break;
        }

        case FakeToolBehavior::FailImmediately:
        {
            vpet::_tagToolExecutionResult result;
            result.ok = false;
            result.textOutput = QStringLiteral("fake failure");
            m_busy = false;
            result.executionId = call.executionId;
            emit Completed(result);
            break;
        }

        case FakeToolBehavior::TerminateHint:
        {
            vpet::_tagToolExecutionResult result;
            result.ok = true;
            result.textOutput = QStringLiteral("done, stop now");
            result.terminateHint = true;
            m_busy = false;
            result.executionId = call.executionId;
            emit Completed(result);
            break;
        }

        case FakeToolBehavior::HangUntilCancelled:
            break;

        case FakeToolBehavior::CompleteAfterDelay:
            QTimer::singleShot(m_delayMs, this, [this, call]()
            {
                vpet::_tagToolExecutionResult result;
                result.ok = true;
                result.textOutput = QStringLiteral("delayed ok");
                result.executionId = call.executionId;
                emit Completed(result);
            });
            break;
        }
    }

    void Cancel() override
    {
        m_cancelCount += 1;
        m_busy = false;
    }

    bool IsBusy() const override
    {
        return m_busy;
    }

    int ExecuteCount() const
    {
        return m_executeCount;
    }

    int CancelCount() const
    {
        return m_cancelCount;
    }

private:
    FakeToolBehavior m_behavior;
    int m_delayMs;
    vpet::_tagToolSpec m_spec;
    bool m_busy;
    int m_executeCount;
    int m_cancelCount;
    vpet::_tagToolCall m_lastCall;
};

/**
 * @brief 构造一次工具调用请求。
 */
vpet::_tagToolCall MakeCall(const QString &toolName,
                            const QJsonObject &arguments = QJsonObject())
{
    vpet::_tagToolCall call;
    call.callId = QStringLiteral("call_1");
    call.toolName = toolName;
    call.arguments = arguments;
    return call;
}

/**
 * @brief 构造合法参数对象。
 */
QJsonObject MakeArguments(const QString &query,
                          int limit = -1,
                          const QString &engine = QString())
{
    QJsonObject arguments;
    arguments[QStringLiteral("query")] = query;

    if (limit >= 0)
    {
        arguments[QStringLiteral("limit")] = limit;
    }

    if (!engine.isEmpty())
    {
        arguments[QStringLiteral("engine")] = engine;
    }

    return arguments;
}

/**
 * @brief 构造带一个工具的注册表。
 */
std::shared_ptr<vpet::ToolRegistry> MakeRegistry(
    const std::shared_ptr<vpet::ITool> &tool,
    QString &errorMessage)
{
    auto registry = std::make_shared<vpet::ToolRegistry>();

    if (tool != nullptr)
    {
        registry->Register(tool, errorMessage);
    }

    return registry;
}

class ToolSchedulerTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<vpet::_tagToolCall>();
        qRegisterMetaType<vpet::_tagToolExecutionResult>();
    }

    // 权限矩阵
    void PermissionDeniesSideEffectTool();
    void PermissionAllowsReadOnlyTool();
    void AllowAllPolicyPermitsSideEffectTool();
    void IsGateBlockedReportsTrustTier();

    // 错误即结果
    void UnknownToolReturnsErrorResult();
    void ToolErrorFeedsBackFailureResult();
    void InvalidArgumentsReturnErrorResult();
    void TerminateHintIsPreserved();

    // 预算检查
    void ToolCallBudgetExhausted();
    void RoundBudgetExhausted();
    void TimeBudgetExhausted();

    // 超时 watchdog
    void ToolTimeoutCancelsAndFeedsBack();
    void LateCompletionAfterTimeoutIsDropped();

    // 取消传播
    void CancelActivePropagatesToTool();

    // Sequential 单飞
    void SequentialSingleFlightRejectsSecondCall();

    // 审计
    void SuccessfulCallWritesAuditEntry();
    void TimeoutAuditPreservesRound();
    void BuildAuditJsonShape();

    // 注册表
    void RegistryRejectsDuplicateName();
    void RegistryBuildsLlmToolsArray();

    // 参数校验子集
    void ValidateToolArgumentsSubset();
};

void ToolSchedulerTests::PermissionDeniesSideEffectTool()
{
    QString errorMessage;
    auto sideEffectTool = std::make_shared<FakeTool>(QStringLiteral("side_effect_tool"),
                                                     vpet::ToolTrustTier::SideEffect,
                                                     FakeToolBehavior::SucceedImmediately);
    auto registry = MakeRegistry(sideEffectTool, errorMessage);
    QVERIFY(registry != nullptr);

    vpet::ToolScheduler scheduler;
    scheduler.SetRegistry(registry);
    scheduler.SetPermissionPolicy(vpet::ToolPermissionPolicy::AutoReadonly);

    QSignalSpy spy(&scheduler, &vpet::ToolScheduler::ToolCallFinished);
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("side_effect_tool")), 1));
    QCOMPARE(spy.count(), 1);

    const vpet::_tagToolExecutionResult result =
        spy.at(0).at(1).value<vpet::_tagToolExecutionResult>();
    QVERIFY(!result.ok);
    QVERIFY(result.textOutput.contains(QStringLiteral("denied")));
    QCOMPARE(sideEffectTool->ExecuteCount(), 0);
    QCOMPARE(scheduler.AuditEntries().size(), 1);
    QCOMPARE(scheduler.AuditEntries().first().status, QStringLiteral("denied"));
}

void ToolSchedulerTests::PermissionAllowsReadOnlyTool()
{
    QString errorMessage;
    auto readOnlyTool = std::make_shared<FakeTool>(QStringLiteral("readonly_tool"),
                                                   vpet::ToolTrustTier::ReadOnly,
                                                   FakeToolBehavior::SucceedImmediately);
    auto registry = MakeRegistry(readOnlyTool, errorMessage);
    QVERIFY(registry != nullptr);

    vpet::ToolScheduler scheduler;
    scheduler.SetRegistry(registry);
    scheduler.SetPermissionPolicy(vpet::ToolPermissionPolicy::AutoReadonly);

    QSignalSpy spy(&scheduler, &vpet::ToolScheduler::ToolCallFinished);
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("readonly_tool"),
                                           MakeArguments(QStringLiteral("hello"))),
                                  1));
    QCOMPARE(spy.count(), 1);

    const vpet::_tagToolExecutionResult result =
        spy.at(0).at(1).value<vpet::_tagToolExecutionResult>();
    QVERIFY(result.ok);
    QCOMPARE(readOnlyTool->ExecuteCount(), 1);
}

void ToolSchedulerTests::AllowAllPolicyPermitsSideEffectTool()
{
    QString errorMessage;
    auto sideEffectTool = std::make_shared<FakeTool>(QStringLiteral("side_effect_tool"),
                                                     vpet::ToolTrustTier::SideEffect,
                                                     FakeToolBehavior::SucceedImmediately);
    auto registry = MakeRegistry(sideEffectTool, errorMessage);
    QVERIFY(registry != nullptr);

    vpet::ToolScheduler scheduler;
    scheduler.SetRegistry(registry);
    scheduler.SetPermissionPolicy(vpet::ToolPermissionPolicy::AllowAll);

    QSignalSpy spy(&scheduler, &vpet::ToolScheduler::ToolCallFinished);
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("side_effect_tool"),
                                           MakeArguments(QStringLiteral("hello"))),
                                  1));
    QCOMPARE(spy.count(), 1);

    const vpet::_tagToolExecutionResult result =
        spy.at(0).at(1).value<vpet::_tagToolExecutionResult>();
    QVERIFY(result.ok);
    QCOMPARE(sideEffectTool->ExecuteCount(), 1);
}

void ToolSchedulerTests::IsGateBlockedReportsTrustTier()
{
    QString errorMessage;
    auto readOnlyTool = std::make_shared<FakeTool>(QStringLiteral("readonly_tool"),
                                                   vpet::ToolTrustTier::ReadOnly,
                                                   FakeToolBehavior::SucceedImmediately);
    auto sideEffectTool = std::make_shared<FakeTool>(QStringLiteral("side_effect_tool"),
                                                     vpet::ToolTrustTier::SideEffect,
                                                     FakeToolBehavior::SucceedImmediately);
    auto registry = std::make_shared<vpet::ToolRegistry>();
    QVERIFY(registry->Register(readOnlyTool, errorMessage));
    QVERIFY(registry->Register(sideEffectTool, errorMessage));

    vpet::ToolScheduler scheduler;
    scheduler.SetRegistry(registry);
    scheduler.SetPermissionPolicy(vpet::ToolPermissionPolicy::AutoReadonly);

    QVERIFY(!scheduler.IsGateBlocked(QStringLiteral("readonly_tool")));
    QVERIFY(scheduler.IsGateBlocked(QStringLiteral("side_effect_tool")));
    QVERIFY(!scheduler.IsGateBlocked(QStringLiteral("missing_tool")));

    scheduler.SetPermissionPolicy(vpet::ToolPermissionPolicy::AllowAll);
    QVERIFY(!scheduler.IsGateBlocked(QStringLiteral("side_effect_tool")));
}

void ToolSchedulerTests::UnknownToolReturnsErrorResult()
{
    QString unusedError;
    auto registry = MakeRegistry(nullptr, unusedError);
    QVERIFY(registry != nullptr);

    vpet::ToolScheduler scheduler;
    scheduler.SetRegistry(registry);
    scheduler.SetPermissionPolicy(vpet::ToolPermissionPolicy::AllowAll);

    QSignalSpy spy(&scheduler, &vpet::ToolScheduler::ToolCallFinished);
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("no_such_tool")), 1));
    QCOMPARE(spy.count(), 1);

    const vpet::_tagToolExecutionResult result =
        spy.at(0).at(1).value<vpet::_tagToolExecutionResult>();
    QVERIFY(!result.ok);
    QVERIFY(result.textOutput.contains(QStringLiteral("Unknown tool")));
    QCOMPARE(scheduler.AuditEntries().first().status, QStringLiteral("unknown_tool"));
}

void ToolSchedulerTests::ToolErrorFeedsBackFailureResult()
{
    QString errorMessage;
    auto failingTool = std::make_shared<FakeTool>(QStringLiteral("failing_tool"),
                                                  vpet::ToolTrustTier::ReadOnly,
                                                  FakeToolBehavior::FailImmediately);
    auto registry = MakeRegistry(failingTool, errorMessage);
    QVERIFY(registry != nullptr);

    vpet::ToolScheduler scheduler;
    scheduler.SetRegistry(registry);
    scheduler.SetPermissionPolicy(vpet::ToolPermissionPolicy::AutoReadonly);

    QSignalSpy spy(&scheduler, &vpet::ToolScheduler::ToolCallFinished);
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("failing_tool"),
                                           MakeArguments(QStringLiteral("hello"))),
                                  1));
    QCOMPARE(spy.count(), 1);

    const vpet::_tagToolExecutionResult result =
        spy.at(0).at(1).value<vpet::_tagToolExecutionResult>();
    QVERIFY(!result.ok);
    QVERIFY(result.textOutput.contains(QStringLiteral("fake failure")));
    QCOMPARE(scheduler.AuditEntries().first().status, QStringLiteral("error"));
}

void ToolSchedulerTests::InvalidArgumentsReturnErrorResult()
{
    QString errorMessage;
    auto readOnlyTool = std::make_shared<FakeTool>(QStringLiteral("readonly_tool"),
                                                   vpet::ToolTrustTier::ReadOnly,
                                                   FakeToolBehavior::SucceedImmediately);
    auto registry = MakeRegistry(readOnlyTool, errorMessage);
    QVERIFY(registry != nullptr);

    vpet::ToolScheduler scheduler;
    scheduler.SetRegistry(registry);
    scheduler.SetPermissionPolicy(vpet::ToolPermissionPolicy::AutoReadonly);

    QSignalSpy spy(&scheduler, &vpet::ToolScheduler::ToolCallFinished);

    // 缺失必填参数 query。
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("readonly_tool"),
                                           QJsonObject()), 1));
    QCOMPARE(spy.count(), 1);
    QVERIFY(!spy.at(0).at(1).value<vpet::_tagToolExecutionResult>().ok);
    QCOMPARE(readOnlyTool->ExecuteCount(), 0);

    // 类型错误：query 传了整数。
    QJsonObject badArguments;
    badArguments[QStringLiteral("query")] = 42;
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("readonly_tool"),
                                           badArguments), 1));
    QCOMPARE(spy.count(), 2);
    QVERIFY(!spy.at(1).at(1).value<vpet::_tagToolExecutionResult>().ok);
    QCOMPARE(readOnlyTool->ExecuteCount(), 0);

    // 枚举越界：engine 不在允许列表。
    QJsonObject badEngineArguments = MakeArguments(QStringLiteral("hello"));
    badEngineArguments[QStringLiteral("engine")] = QStringLiteral("duckduckgo");
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("readonly_tool"),
                                           badEngineArguments), 1));
    QCOMPARE(spy.count(), 3);
    QVERIFY(!spy.at(2).at(1).value<vpet::_tagToolExecutionResult>().ok);
    QCOMPARE(readOnlyTool->ExecuteCount(), 0);
}

void ToolSchedulerTests::TerminateHintIsPreserved()
{
    QString errorMessage;
    auto terminatingTool = std::make_shared<FakeTool>(QStringLiteral("terminating_tool"),
                                                      vpet::ToolTrustTier::ReadOnly,
                                                      FakeToolBehavior::TerminateHint);
    auto registry = MakeRegistry(terminatingTool, errorMessage);
    QVERIFY(registry != nullptr);

    vpet::ToolScheduler scheduler;
    scheduler.SetRegistry(registry);
    scheduler.SetPermissionPolicy(vpet::ToolPermissionPolicy::AutoReadonly);

    QSignalSpy spy(&scheduler, &vpet::ToolScheduler::ToolCallFinished);
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("terminating_tool"),
                                           MakeArguments(QStringLiteral("hello"))),
                                  1));
    QCOMPARE(spy.count(), 1);

    const vpet::_tagToolExecutionResult result =
        spy.at(0).at(1).value<vpet::_tagToolExecutionResult>();
    QVERIFY(result.ok);
    QVERIFY(result.terminateHint);
}

void ToolSchedulerTests::ToolCallBudgetExhausted()
{
    QString errorMessage;
    auto readOnlyTool = std::make_shared<FakeTool>(QStringLiteral("readonly_tool"),
                                                   vpet::ToolTrustTier::ReadOnly,
                                                   FakeToolBehavior::SucceedImmediately);
    auto registry = MakeRegistry(readOnlyTool, errorMessage);
    QVERIFY(registry != nullptr);

    vpet::ToolScheduler scheduler;
    scheduler.SetRegistry(registry);
    scheduler.SetPermissionPolicy(vpet::ToolPermissionPolicy::AutoReadonly);

    vpet::_tagToolBudgetConfig budget;
    budget.maxRounds = 10;
    budget.maxToolCalls = 1;
    scheduler.SetBudget(budget);
    scheduler.BeginInvocation();

    QSignalSpy spy(&scheduler, &vpet::ToolScheduler::ToolCallFinished);

    // 第一次调用在预算内。
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("readonly_tool"),
                                           MakeArguments(QStringLiteral("hello"))),
                                  1));
    QCOMPARE(spy.count(), 1);
    QVERIFY(spy.at(0).at(1).value<vpet::_tagToolExecutionResult>().ok);

    // 第二次调用被预算拒绝并回喂错误结果。
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("readonly_tool"),
                                           MakeArguments(QStringLiteral("hello"))),
                                  2));
    QCOMPARE(spy.count(), 2);
    const vpet::_tagToolExecutionResult result =
        spy.at(1).at(1).value<vpet::_tagToolExecutionResult>();
    QVERIFY(!result.ok);
    QVERIFY(result.textOutput.contains(QStringLiteral("budget")));
    QCOMPARE(scheduler.AuditEntries().last().status, QStringLiteral("budget_exhausted"));
}

void ToolSchedulerTests::RoundBudgetExhausted()
{
    vpet::ToolScheduler scheduler;

    vpet::_tagToolBudgetConfig budget;
    budget.maxRounds = 2;
    scheduler.SetBudget(budget);
    scheduler.BeginInvocation();

    QString reason;
    QVERIFY(!scheduler.IsBudgetExhausted(reason));
    scheduler.NotifyRoundStarted();
    QVERIFY(!scheduler.IsBudgetExhausted(reason));
    scheduler.NotifyRoundStarted();
    QVERIFY(scheduler.IsBudgetExhausted(reason));
    QVERIFY(reason.contains(QStringLiteral("Round")));
}

void ToolSchedulerTests::TimeBudgetExhausted()
{
    vpet::ToolScheduler scheduler;

    vpet::_tagToolBudgetConfig budget;
    budget.maxRounds = 100;
    budget.timeBudgetMs = 100;
    scheduler.SetBudget(budget);
    scheduler.BeginInvocation();

    QString reason;
    QVERIFY(!scheduler.IsBudgetExhausted(reason));
    QTest::qWait(200);
    QVERIFY(scheduler.IsBudgetExhausted(reason));
    QVERIFY(reason.contains(QStringLiteral("Time")));
}

void ToolSchedulerTests::ToolTimeoutCancelsAndFeedsBack()
{
    QString errorMessage;
    auto hangingTool = std::make_shared<FakeTool>(QStringLiteral("hanging_tool"),
                                                  vpet::ToolTrustTier::ReadOnly,
                                                  FakeToolBehavior::HangUntilCancelled);
    auto registry = MakeRegistry(hangingTool, errorMessage);
    QVERIFY(registry != nullptr);

    vpet::ToolScheduler scheduler;
    scheduler.SetRegistry(registry);
    scheduler.SetPermissionPolicy(vpet::ToolPermissionPolicy::AutoReadonly);

    vpet::_tagToolBudgetConfig budget;
    budget.maxRounds = 10;
    budget.toolTimeoutMs = 100;
    scheduler.SetBudget(budget);

    QSignalSpy spy(&scheduler, &vpet::ToolScheduler::ToolCallFinished);
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("hanging_tool"),
                                           MakeArguments(QStringLiteral("hello"))),
                                  1));
    QVERIFY(spy.wait(3000));
    QCOMPARE(spy.count(), 1);

    const vpet::_tagToolExecutionResult result =
        spy.at(0).at(1).value<vpet::_tagToolExecutionResult>();
    QVERIFY(!result.ok);
    QVERIFY(result.textOutput.contains(QStringLiteral("timed out")));
    QCOMPARE(hangingTool->CancelCount(), 1);
    QCOMPARE(scheduler.AuditEntries().first().status, QStringLiteral("timeout"));
    QVERIFY(!scheduler.IsBusy());
}

void ToolSchedulerTests::LateCompletionAfterTimeoutIsDropped()
{
    QString errorMessage;
    auto delayedTool = std::make_shared<FakeTool>(QStringLiteral("delayed_tool"),
                                                  vpet::ToolTrustTier::ReadOnly,
                                                  FakeToolBehavior::CompleteAfterDelay,
                                                  400);
    auto registry = MakeRegistry(delayedTool, errorMessage);
    QVERIFY(registry != nullptr);

    vpet::ToolScheduler scheduler;
    scheduler.SetRegistry(registry);
    scheduler.SetPermissionPolicy(vpet::ToolPermissionPolicy::AutoReadonly);

    vpet::_tagToolBudgetConfig budget;
    budget.maxRounds = 10;
    budget.toolTimeoutMs = 100;
    scheduler.SetBudget(budget);

    QSignalSpy spy(&scheduler, &vpet::ToolScheduler::ToolCallFinished);
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("delayed_tool"),
                                           MakeArguments(QStringLiteral("hello"))),
                                  1));
    QVERIFY(spy.wait(3000));
    QCOMPARE(spy.count(), 1);
    QVERIFY(!spy.at(0).at(1).value<vpet::_tagToolExecutionResult>().ok);

    // 超时后迟到的完成信号被丢弃，不产生第二次收束。
    QTest::qWait(600);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(scheduler.AuditEntries().size(), 1);
}

void ToolSchedulerTests::CancelActivePropagatesToTool()
{
    QString errorMessage;
    auto hangingTool = std::make_shared<FakeTool>(QStringLiteral("hanging_tool"),
                                                  vpet::ToolTrustTier::ReadOnly,
                                                  FakeToolBehavior::HangUntilCancelled);
    auto registry = MakeRegistry(hangingTool, errorMessage);
    QVERIFY(registry != nullptr);

    vpet::ToolScheduler scheduler;
    scheduler.SetRegistry(registry);
    scheduler.SetPermissionPolicy(vpet::ToolPermissionPolicy::AutoReadonly);

    QSignalSpy spy(&scheduler, &vpet::ToolScheduler::ToolCallFinished);
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("hanging_tool"),
                                           MakeArguments(QStringLiteral("hello"))),
                                  2));
    QVERIFY(scheduler.IsBusy());

    scheduler.CancelActive();

    QCOMPARE(spy.count(), 1);
    const vpet::_tagToolExecutionResult result =
        spy.at(0).at(1).value<vpet::_tagToolExecutionResult>();
    QVERIFY(!result.ok);
    QVERIFY(result.textOutput.contains(QStringLiteral("cancelled")));
    QCOMPARE(hangingTool->CancelCount(), 1);
    QCOMPARE(scheduler.AuditEntries().first().status, QStringLiteral("cancelled"));
    QVERIFY(!scheduler.IsBusy());
}

void ToolSchedulerTests::SequentialSingleFlightRejectsSecondCall()
{
    QString errorMessage;
    auto delayedTool = std::make_shared<FakeTool>(QStringLiteral("delayed_tool"),
                                                  vpet::ToolTrustTier::ReadOnly,
                                                  FakeToolBehavior::CompleteAfterDelay,
                                                  200);
    auto registry = MakeRegistry(delayedTool, errorMessage);
    QVERIFY(registry != nullptr);

    vpet::ToolScheduler scheduler;
    scheduler.SetRegistry(registry);
    scheduler.SetPermissionPolicy(vpet::ToolPermissionPolicy::AutoReadonly);

    QSignalSpy spy(&scheduler, &vpet::ToolScheduler::ToolCallFinished);

    // 第一个调用受理，第二个调用在活动期间被单飞拒绝。
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("delayed_tool"),
                                           MakeArguments(QStringLiteral("hello"))),
                                  1));
    QVERIFY(scheduler.IsBusy());
    QVERIFY(!scheduler.ExecuteCall(MakeCall(QStringLiteral("delayed_tool"),
                                            MakeArguments(QStringLiteral("world"))),
                                   1));

    QVERIFY(spy.wait(3000));
    QCOMPARE(spy.count(), 1);
    QVERIFY(spy.at(0).at(1).value<vpet::_tagToolExecutionResult>().ok);
    QVERIFY(!scheduler.IsBusy());
}

void ToolSchedulerTests::SuccessfulCallWritesAuditEntry()
{
    QString errorMessage;
    auto readOnlyTool = std::make_shared<FakeTool>(QStringLiteral("readonly_tool"),
                                                   vpet::ToolTrustTier::ReadOnly,
                                                   FakeToolBehavior::SucceedImmediately);
    auto registry = MakeRegistry(readOnlyTool, errorMessage);
    QVERIFY(registry != nullptr);

    vpet::ToolScheduler scheduler;
    scheduler.SetRegistry(registry);
    scheduler.SetPermissionPolicy(vpet::ToolPermissionPolicy::AutoReadonly);
    scheduler.BeginInvocation();

    QSignalSpy spy(&scheduler, &vpet::ToolScheduler::ToolCallFinished);
    const QJsonObject arguments = MakeArguments(QStringLiteral("hello"), 5,
                                                QStringLiteral("bing"));
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("readonly_tool"), arguments), 3));
    QCOMPARE(spy.count(), 1);
    QVERIFY(spy.at(0).at(1).value<vpet::_tagToolExecutionResult>().ok);

    QCOMPARE(scheduler.AuditEntries().size(), 1);
    const vpet::_tagSchedulerAuditEntry entry = scheduler.AuditEntries().first();
    QCOMPARE(entry.round, 3);
    QCOMPARE(entry.tool, QStringLiteral("readonly_tool"));
    QCOMPARE(entry.status, QStringLiteral("ok"));
    QCOMPARE(entry.argsDigest.size(), 64);
    QVERIFY(!entry.argsDigest.contains(QStringLiteral("query")));
    QVERIFY(!entry.argsDigest.contains(QStringLiteral("bing")));
    QVERIFY(entry.detail.isEmpty());
}

void ToolSchedulerTests::TimeoutAuditPreservesRound()
{
    QString errorMessage;
    auto hangingTool = std::make_shared<FakeTool>(QStringLiteral("hanging_tool"),
                                                  vpet::ToolTrustTier::ReadOnly,
                                                  FakeToolBehavior::HangUntilCancelled);
    auto registry = MakeRegistry(hangingTool, errorMessage);
    QVERIFY(registry != nullptr);

    vpet::ToolScheduler scheduler;
    scheduler.SetRegistry(registry);
    scheduler.SetPermissionPolicy(vpet::ToolPermissionPolicy::AutoReadonly);

    vpet::_tagToolBudgetConfig budget;
    budget.maxRounds = 10;
    budget.toolTimeoutMs = 100;
    scheduler.SetBudget(budget);

    QSignalSpy spy(&scheduler, &vpet::ToolScheduler::ToolCallFinished);
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("hanging_tool"),
                                           MakeArguments(QStringLiteral("hello"))),
                                  4));
    QVERIFY(spy.wait(3000));
    QCOMPARE(spy.count(), 1);

    QCOMPARE(scheduler.AuditEntries().size(), 1);
    const vpet::_tagSchedulerAuditEntry entry = scheduler.AuditEntries().first();
    QCOMPARE(entry.round, 4);
    QCOMPARE(entry.status, QStringLiteral("timeout"));
    QCOMPARE(entry.tool, QStringLiteral("hanging_tool"));
}

void ToolSchedulerTests::BuildAuditJsonShape()
{
    QString errorMessage;
    auto sideEffectTool = std::make_shared<FakeTool>(QStringLiteral("side_effect_tool"),
                                                     vpet::ToolTrustTier::SideEffect,
                                                     FakeToolBehavior::SucceedImmediately);
    auto readOnlyTool = std::make_shared<FakeTool>(QStringLiteral("readonly_tool"),
                                                   vpet::ToolTrustTier::ReadOnly,
                                                   FakeToolBehavior::SucceedImmediately);
    auto registry = std::make_shared<vpet::ToolRegistry>();
    QVERIFY(registry->Register(sideEffectTool, errorMessage));
    QVERIFY(registry->Register(readOnlyTool, errorMessage));

    vpet::ToolScheduler scheduler;
    scheduler.SetRegistry(registry);
    scheduler.SetPermissionPolicy(vpet::ToolPermissionPolicy::AutoReadonly);

    QSignalSpy spy(&scheduler, &vpet::ToolScheduler::ToolCallFinished);
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("side_effect_tool")), 1));
    QVERIFY(scheduler.ExecuteCall(MakeCall(QStringLiteral("readonly_tool"),
                                           MakeArguments(QStringLiteral("hello"))),
                                  1));
    QCOMPARE(spy.count(), 2);

    const QJsonArray auditArray = scheduler.BuildAuditJson();
    QCOMPARE(auditArray.size(), 2);

    const QJsonObject deniedEntry = auditArray.at(0).toObject();
    QCOMPARE(deniedEntry.value(QStringLiteral("status")).toString(), QStringLiteral("denied"));
    QCOMPARE(deniedEntry.value(QStringLiteral("tool")).toString(),
             QStringLiteral("side_effect_tool"));
    QVERIFY(deniedEntry.contains(QStringLiteral("round")));
    QVERIFY(deniedEntry.contains(QStringLiteral("duration_ms")));
    QVERIFY(deniedEntry.contains(QStringLiteral("args_digest")));
    QVERIFY(deniedEntry.contains(QStringLiteral("error")));

    const QJsonObject okEntry = auditArray.at(1).toObject();
    QCOMPARE(okEntry.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    QVERIFY(!okEntry.contains(QStringLiteral("error")));
}

void ToolSchedulerTests::RegistryRejectsDuplicateName()
{
    QString errorMessage;
    auto firstTool = std::make_shared<FakeTool>(QStringLiteral("dup_tool"),
                                                vpet::ToolTrustTier::ReadOnly,
                                                FakeToolBehavior::SucceedImmediately);
    auto secondTool = std::make_shared<FakeTool>(QStringLiteral("dup_tool"),
                                                 vpet::ToolTrustTier::ReadOnly,
                                                 FakeToolBehavior::SucceedImmediately);

    vpet::ToolRegistry registry;
    QVERIFY(registry.Register(firstTool, errorMessage));
    QVERIFY(!registry.Register(secondTool, errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("already contains")));
    QCOMPARE(registry.Count(), 1);
}

void ToolSchedulerTests::RegistryBuildsLlmToolsArray()
{
    QString errorMessage;
    auto readOnlyTool = std::make_shared<FakeTool>(QStringLiteral("web.search"),
                                                   vpet::ToolTrustTier::ReadOnly,
                                                   FakeToolBehavior::SucceedImmediately);
    vpet::ToolRegistry registry;
    QVERIFY(registry.Register(readOnlyTool, errorMessage));
    QVERIFY(registry.Contains(QStringLiteral("web.search")));
    QCOMPARE(registry.ToolNames(), QStringList({QStringLiteral("web.search")}));

    const QJsonArray toolsArray = registry.BuildLlmToolsArray();
    QCOMPARE(toolsArray.size(), 1);

    const QJsonObject toolObject = toolsArray.at(0).toObject();
    QCOMPARE(toolObject.value(QStringLiteral("type")).toString(), QStringLiteral("function"));

    const QJsonObject functionObject =
        toolObject.value(QStringLiteral("function")).toObject();
    QCOMPARE(functionObject.value(QStringLiteral("name")).toString(),
             QStringLiteral("web.search"));
    QVERIFY(!functionObject.value(QStringLiteral("description")).toString().isEmpty());

    const QJsonObject parameters =
        functionObject.value(QStringLiteral("parameters")).toObject();
    QCOMPARE(parameters.value(QStringLiteral("type")).toString(), QStringLiteral("object"));
    QVERIFY(parameters.value(QStringLiteral("properties"))
                .toObject()
                .contains(QStringLiteral("query")));
    QVERIFY(parameters.value(QStringLiteral("required"))
                .toArray()
                .contains(QStringLiteral("query")));
    QVERIFY(parameters.value(QStringLiteral("properties"))
                .toObject()
                .value(QStringLiteral("engine"))
                .toObject()
                .value(QStringLiteral("enum"))
                .toArray()
                .contains(QStringLiteral("bing")));
}

void ToolSchedulerTests::ValidateToolArgumentsSubset()
{
    vpet::_tagToolSpec spec;
    spec.name = QStringLiteral("sample");

    vpet::_tagToolParameterSchema requiredString;
    requiredString.name = QStringLiteral("query");
    requiredString.type = QStringLiteral("string");
    requiredString.required = true;
    spec.parameters.append(requiredString);

    vpet::_tagToolParameterSchema optionalInteger;
    optionalInteger.name = QStringLiteral("limit");
    optionalInteger.type = QStringLiteral("integer");
    spec.parameters.append(optionalInteger);

    vpet::_tagToolParameterSchema enumString;
    enumString.name = QStringLiteral("engine");
    enumString.type = QStringLiteral("string");
    enumString.enumValues << QStringLiteral("bing") << QStringLiteral("google");
    spec.parameters.append(enumString);

    QString errorMessage;

    // 全合法。
    QJsonObject validArguments = MakeArguments(QStringLiteral("hello"), 5,
                                                QStringLiteral("bing"));
    QVERIFY(vpet::ValidateToolArguments(spec, validArguments, errorMessage));

    // 缺失必填参数。
    QVERIFY(!vpet::ValidateToolArguments(spec, QJsonObject(), errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("required")));

    // 类型错误。
    QJsonObject badTypeArguments = MakeArguments(QStringLiteral("hello"));
    badTypeArguments[QStringLiteral("limit")] = QStringLiteral("many");
    QVERIFY(!vpet::ValidateToolArguments(spec, badTypeArguments, errorMessage));

    // 枚举越界。
    QJsonObject badEnumArguments = MakeArguments(QStringLiteral("hello"));
    badEnumArguments[QStringLiteral("engine")] = QStringLiteral("yahoo");
    QVERIFY(!vpet::ValidateToolArguments(spec, badEnumArguments, errorMessage));

    // 未声明的参数忽略，不阻止调用。
    QJsonObject extraArguments = MakeArguments(QStringLiteral("hello"));
    extraArguments[QStringLiteral("mystery")] = 123;
    QVERIFY(vpet::ValidateToolArguments(spec, extraArguments, errorMessage));

    // 浮点数传入 integer 必须拒绝。
    QJsonObject floatArguments = MakeArguments(QStringLiteral("hello"));
    floatArguments[QStringLiteral("limit")] = 3.14;
    QVERIFY(!vpet::ValidateToolArguments(spec, floatArguments, errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("declared type")));

    // 含有未支持类型的参数声明必须 fail-closed 拒绝。
    vpet::_tagToolSpec unsupportedSpec;
    unsupportedSpec.name = QStringLiteral("bad_spec");
    vpet::_tagToolParameterSchema unknownTypeParam;
    unknownTypeParam.name = QStringLiteral("custom");
    unknownTypeParam.type = QStringLiteral("unknown_custom_type");
    unknownTypeParam.required = true;
    unsupportedSpec.parameters.append(unknownTypeParam);
    QJsonObject customArgs;
    customArgs[QStringLiteral("custom")] = QStringLiteral("value");
    QVERIFY(!vpet::ValidateToolArguments(unsupportedSpec, customArgs, errorMessage));

    // 非 string 类型尝试校验 enum 必须被拒绝而不是转空字符串放行。
    vpet::_tagToolSpec enumSpec;
    enumSpec.name = QStringLiteral("enum_spec");
    vpet::_tagToolParameterSchema enumParam;
    enumParam.name = QStringLiteral("tag");
    enumParam.type = QStringLiteral("integer");
    enumParam.enumValues = QStringList{QStringLiteral("1"), QStringLiteral("2")};
    enumSpec.parameters.append(enumParam);
    QJsonObject intEnumArgs;
    intEnumArgs[QStringLiteral("tag")] = 1;
    QVERIFY(!vpet::ValidateToolArguments(enumSpec, intEnumArgs, errorMessage));
}

} // anonymous namespace

QTEST_MAIN(ToolSchedulerTests)

#include "tool_scheduler_tests.moc"
