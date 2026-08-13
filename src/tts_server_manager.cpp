#include "vpet/tts_server_manager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QTcpServer>
#include <QUrl>
#include <QUuid>

namespace vpet
{

namespace
{

constexpr int HTTP_TIMEOUT_MS = 3000; ///< 健康检查单次请求超时

/**
 * @brief 启动前释放被残留实例占用的 TTS 端口（Windows）
 *
 * 前一次运行强杀或手动启动的 Kokoro API 实例可能残留并占用端口，
 * 导致新实例绑定失败、健康检查命中错误实例。仅当端口被占用时，
 * 杀掉「命令行同时匹配 kokoro_server 与端口号」的进程，其余情况零开销返回。
 */
#ifdef Q_OS_WIN
void FreeConflictingInstance(const QString &serverUrl)
{
    const QUrl url(serverUrl);
    const int rawPort = url.port(9880);
    if (rawPort <= 0 || rawPort > 65535)
    {
        return;
    }

    const quint16 port = static_cast<quint16>(rawPort);

    QTcpServer probe;
    if (probe.listen(QHostAddress::LocalHost, port))
    {
        return; // 端口空闲，无需清理
    }

    qDebug() << "[TTS]   port" << port << "occupied, stopping stale Kokoro instance...";

    const QString script = QStringLiteral(
                               "Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'kokoro_server' -and $_.CommandLine -match '%1' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }")
                               .arg(port);
    QProcess killer;
    killer.start(QStringLiteral("powershell"),
                 { QStringLiteral("-NoProfile"),
                   QStringLiteral("-NonInteractive"),
                   QStringLiteral("-Command"),
                   script });

    if (!killer.waitForFinished(8000))
    {
        qDebug() << "[TTS]   WARNING: timed out waiting for stale instance cleanup";
    }
    else
    {
        qDebug() << "[TTS]   stale instance cleanup done, exit:" << killer.exitStatus();
    }
}
#endif

} // anonymous namespace

TtsServerManager::TtsServerManager(QObject *parent)
    : QObject(parent)
    , m_serverProcess(nullptr)
    , m_healthCheckTimer(nullptr)
    , m_startupTimeoutTimer(nullptr)
    , m_healthNetworkManager(new QNetworkAccessManager(this))
    , m_healthCheckReply(nullptr)
    , m_serverUrl()
    , m_pythonExePath()
    , m_apiScriptPath()
    , m_workingDirectory()
    , m_apiArguments()
    , m_configPath()
    , m_instanceId()
    , m_healthCheckCount(0)
    , m_restartAttempts(0)
    , m_isReady(false)
    , m_restartScheduled(false)
{
}

TtsServerManager::~TtsServerManager()
{
    Stop();
}

bool TtsServerManager::Start(const QString &configPath)
{
    qDebug() << "[TTS] TtsServerManager::Start";

    if (m_serverProcess != nullptr)
    {
        qDebug() << "[TTS]   already started";
        return false;
    }

    emit StatusChanged(QStringLiteral("正在查找配置文件..."));

    const QString resolvedPath = FindConfigFile(configPath);
    qDebug() << "[TTS]   FindConfigFile result:" << resolvedPath;

    if (resolvedPath.isEmpty())
    {
        qDebug() << "[TTS]   FAILED - config file not found";
        emit ServerStartFailed(QStringLiteral("未找到 tts_config.json 配置文件"));
        return false;
    }

    m_configPath = resolvedPath;

    if (!LoadServerConfig(resolvedPath))
    {
        qDebug() << "[TTS]   FAILED - LoadServerConfig failed";
        emit ServerStartFailed(QStringLiteral("TTS 配置文件解析失败"));
        return false;
    }

    qDebug() << "[TTS]   pythonExePath:" << m_pythonExePath;
    qDebug() << "[TTS]   apiScriptPath:" << m_apiScriptPath;
    qDebug() << "[TTS]   workingDir:" << m_workingDirectory;
    qDebug() << "[TTS]   api arguments:" << m_apiArguments;

    // 检查 Python 解释器是否存在
    if (!QFileInfo::exists(m_pythonExePath))
    {
        qDebug() << "[TTS]   FAILED - python.exe not found at:" << m_pythonExePath;
        emit ServerStartFailed(
            QStringLiteral("未找到 Python 解释器: %1").arg(m_pythonExePath));
        return false;
    }

    // 检查 API 脚本是否存在
    if (!QFileInfo::exists(m_apiScriptPath))
    {
        qDebug() << "[TTS]   FAILED - kokoro_server.py not found at:" << m_apiScriptPath;
        emit ServerStartFailed(
            QStringLiteral("未找到 API 脚本: %1").arg(m_apiScriptPath));
        return false;
    }

    emit StatusChanged(QStringLiteral("正在启动 TTS 服务..."));

#ifdef Q_OS_WIN
    // 先释放被残留实例占用的端口，避免新实例绑定失败
    FreeConflictingInstance(m_serverUrl);
#endif

    // 启动 Kokoro 进程
    m_serverProcess = new QProcess(this);

    connect(m_serverProcess, &QProcess::errorOccurred,
            this, &TtsServerManager::OnProcessError);

    QObject::connect(m_serverProcess,
                     QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     this, &TtsServerManager::OnProcessFinished);

    m_serverProcess->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    m_serverProcess->setWorkingDirectory(m_workingDirectory);

    // 设置 VPET_TTS_INSTANCE_ID，健康检查据此确认识别到本实例
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    m_instanceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    env.insert(QStringLiteral("VPET_TTS_INSTANCE_ID"), m_instanceId);
    m_serverProcess->setProcessEnvironment(env);

    qDebug() << "[TTS]   starting process:" << m_pythonExePath << m_apiArguments;
    m_serverProcess->start(m_pythonExePath, m_apiArguments);

    emit StatusChanged(QStringLiteral("TTS 服务正在加载模型，请稍候..."));

    // 启动健康检查定时器
    m_healthCheckTimer = new QTimer(this);
    m_healthCheckTimer->setInterval(HEALTH_CHECK_INTERVAL_MS);

    connect(m_healthCheckTimer, &QTimer::timeout,
            this, &TtsServerManager::OnHealthCheckTimer);

    m_healthCheckCount = 0;
    m_startupTimeoutTimer = new QTimer(this);
    m_startupTimeoutTimer->setSingleShot(true);
    connect(m_startupTimeoutTimer, &QTimer::timeout, this, [this]()
    {
        if (m_isReady || (m_serverProcess == nullptr))
        {
            return;
        }

        Stop();
        emit ServerStartFailed(QStringLiteral("TTS 服务启动超时（约 60 秒），请检查服务是否正常"));
    });
    m_startupTimeoutTimer->start(STARTUP_TIMEOUT_MS);

    // 延迟首次检查，给服务器一点初始化时间
    QTimer::singleShot(PROCESS_START_DELAY_MS, this, [this]()
    {
        if (m_healthCheckTimer != nullptr)
        {
            m_healthCheckTimer->start();
        }
    });

    qDebug() << "[TTS]   health check timer started, interval:" << HEALTH_CHECK_INTERVAL_MS << "ms";
    return true;
}

void TtsServerManager::Stop()
{
    m_isReady = false;
    m_restartScheduled = false;

    if (m_healthCheckReply != nullptr)
    {
        m_healthCheckReply->abort();
        m_healthCheckReply = nullptr;
    }

    if (m_healthCheckTimer != nullptr)
    {
        m_healthCheckTimer->stop();
        m_healthCheckTimer->deleteLater();
        m_healthCheckTimer = nullptr;
    }

    if (m_startupTimeoutTimer != nullptr)
    {
        m_startupTimeoutTimer->stop();
        m_startupTimeoutTimer->deleteLater();
        m_startupTimeoutTimer = nullptr;
    }

    if (m_serverProcess != nullptr)
    {
        // 主动停止期间不执行意外退出处理，只管理当前对象启动的进程。
        m_serverProcess->disconnect(this);

        if (m_serverProcess->state() != QProcess::NotRunning)
        {
            m_serverProcess->terminate();

            if (!m_serverProcess->waitForFinished(3000))
            {
                m_serverProcess->kill();
                m_serverProcess->waitForFinished(2000);
            }
        }

        delete m_serverProcess;
        m_serverProcess = nullptr;
    }
}

bool TtsServerManager::IsReady() const
{
    return m_isReady;
}

QString TtsServerManager::GetServerUrl() const
{
    return m_serverUrl;
}

QString TtsServerManager::GetWorkingDirectory() const
{
    return m_workingDirectory;
}

void TtsServerManager::OnHealthCheckTimer()
{
    if (m_isReady || (m_healthCheckReply != nullptr))
    {
        return;
    }

    m_healthCheckCount += 1;

    // 更新进度文本
    const int dots = (m_healthCheckCount % 4);
    QString progressText = QStringLiteral("TTS 服务正在加载模型，请稍候");

    for (int i = 0; i < dots; ++i)
    {
        progressText += QStringLiteral(".");
    }

    emit StatusChanged(progressText);

    PerformHealthCheck();
}

void TtsServerManager::OnProcessError(QProcess::ProcessError error)
{
    qDebug() << "[TTS] OnProcessError:" << error;

    const QString errorMsg = m_serverProcess != nullptr
                                 ? m_serverProcess->errorString()
                                 : QStringLiteral("未知进程错误");
    qDebug() << "[TTS]   process error string:" << errorMsg;

    // 崩溃等终止错误由 finished 统一收口，避免重复报告并保留 Ready 前状态。
    if (error != QProcess::FailedToStart)
    {
        return;
    }

    m_isReady = false;

    if (m_healthCheckTimer != nullptr)
    {
        m_healthCheckTimer->stop();
        m_healthCheckTimer->deleteLater();
        m_healthCheckTimer = nullptr;
    }

    if (m_startupTimeoutTimer != nullptr)
    {
        m_startupTimeoutTimer->stop();
        m_startupTimeoutTimer->deleteLater();
        m_startupTimeoutTimer = nullptr;
    }

    if (m_serverProcess != nullptr)
    {
        m_serverProcess->disconnect(this);
        m_serverProcess->deleteLater();
        m_serverProcess = nullptr;
    }

    emit ServerStartFailed(
        QStringLiteral("TTS 服务进程错误: %1").arg(errorMsg));
}

void TtsServerManager::OnProcessFinished(int exitCode,
                                          QProcess::ExitStatus exitStatus)
{
    qDebug() << "[TTS] OnProcessFinished, exitCode:" << exitCode << "exitStatus:" << exitStatus;

    const bool wasReady = m_isReady;
    m_isReady = false;

    if (m_healthCheckTimer != nullptr)
    {
        m_healthCheckTimer->stop();
        m_healthCheckTimer->deleteLater();
        m_healthCheckTimer = nullptr;
    }

    if (m_startupTimeoutTimer != nullptr)
    {
        m_startupTimeoutTimer->stop();
        m_startupTimeoutTimer->deleteLater();
        m_startupTimeoutTimer = nullptr;
    }

    if (m_serverProcess != nullptr)
    {
        const QByteArray stderrOutput = m_serverProcess->readAllStandardError();
        qDebug() << "[TTS]   stderr bytes:" << stderrOutput.size();

        m_serverProcess->deleteLater();
        m_serverProcess = nullptr;
    }

    const QString failureMessage = wasReady
                                       ? QStringLiteral("TTS 服务就绪后意外退出，退出码: %1").arg(exitCode)
                                       : QStringLiteral("TTS 服务进程意外退出，退出码: %1").arg(exitCode);

    emit StatusChanged(failureMessage);
    emit ServerStartFailed(failureMessage);

    if (wasReady)
    {
        ScheduleRestart();
    }
}

bool TtsServerManager::LoadServerConfig(const QString &configPath)
{
    qDebug() << "[TTS] LoadServerConfig, path:" << configPath;

    QFile file(configPath);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        qDebug() << "[TTS]   FAILED - cannot open file";
        return false;
    }

    const QByteArray data = file.readAll();
    file.close();

    const QJsonDocument doc = QJsonDocument::fromJson(data);

    if (!doc.isObject())
    {
        qDebug() << "[TTS]   FAILED - not a JSON object";
        return false;
    }

    const QJsonObject obj = doc.object();

    // 服务器 URL
    m_serverUrl = obj.value(QStringLiteral("server_url")).toString(
                      QStringLiteral("http://127.0.0.1:9880")).trimmed();

    while (m_serverUrl.endsWith(QLatin1Char('/')))
    {
        m_serverUrl.chop(1);
    }

    const QUrl serverUrl(m_serverUrl);

    if (!serverUrl.isValid() || serverUrl.scheme().isEmpty() || serverUrl.host().isEmpty())
    {
        return false;
    }

    // Python 解释器与脚本路径 — 相对于项目根（配置文件同目录）
    const QString projectRoot = QFileInfo(configPath).absoluteDir().absolutePath();

    qDebug() << "[TTS]   projectRoot:" << projectRoot;

    m_workingDirectory = projectRoot;

    const QDir runtimeDir(QDir(projectRoot).absoluteFilePath(QStringLiteral("runtime")));
    const QString legacyPythonPath = runtimeDir.absoluteFilePath(QStringLiteral("python.exe"));
    const QString venvPythonPath = runtimeDir.absoluteFilePath(QStringLiteral("Scripts/python.exe"));
    if (QFileInfo::exists(venvPythonPath))
    {
        m_pythonExePath = venvPythonPath;
    }
    else if (QFileInfo::exists(legacyPythonPath))
    {
        m_pythonExePath = legacyPythonPath;
    }
    else
    {
        m_pythonExePath.clear();
    }

    m_apiScriptPath = QDir(projectRoot)
                      .absoluteFilePath(QStringLiteral("tools/kokoro/kokoro_server.py"));

    // API 启动参数
    const QString host = obj.value(QStringLiteral("server_host")).toString(
                             QStringLiteral("127.0.0.1")).trimmed();

    const int port = obj.value(QStringLiteral("server_port")).toInt(9880);

    if (host.isEmpty() || (port < 1) || (port > 65535))
    {
        return false;
    }

    m_apiArguments = QStringList{
        m_apiScriptPath,
        QStringLiteral("--host"), host,
        QStringLiteral("--port"), QString::number(port)};

    qDebug() << "[TTS]   serverUrl:" << m_serverUrl;
    qDebug() << "[TTS]   pythonExePath:" << m_pythonExePath;
    qDebug() << "[TTS]   apiScriptPath:" << m_apiScriptPath;
    qDebug() << "[TTS]   api arguments:" << m_apiArguments;

    return true;
}

QString TtsServerManager::FindConfigFile(const QString &configPath) const
{
    // 用户传入的路径（最高优先级）
    if (!configPath.isEmpty() && QFile::exists(configPath))
    {
        return QFileInfo(configPath).absoluteFilePath();
    }

    const QString exeDir = QCoreApplication::applicationDirPath();

    // 构建候选路径列表
    const QStringList candidatePaths =
    {
        // exe 同目录
        exeDir + QStringLiteral("/tts_config.json"),

        // 工作目录
        QDir::currentPath() + QStringLiteral("/tts_config.json"),

        // exe 上级目录（exe 在 build/ 下，配置在项目根目录）
        exeDir + QStringLiteral("/../tts_config.json"),

        // exe 上两级目录（Qt Creator Debug 模式 exe 在 build/Debug/ 下）
        exeDir + QStringLiteral("/../../tts_config.json"),
    };

    for (const QString &candidate : candidatePaths)
    {
        if (QFile::exists(candidate))
        {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }

    return QString();
}

void TtsServerManager::PerformHealthCheck()
{
    if (m_isReady || (m_healthNetworkManager == nullptr) || (m_healthCheckReply != nullptr))
    {
        return;
    }

    QNetworkRequest request(QUrl(m_serverUrl + QStringLiteral("/health")));
    request.setTransferTimeout(HTTP_TIMEOUT_MS);

    QNetworkReply *reply = m_healthNetworkManager->get(request);

    if (reply == nullptr)
    {
        return;
    }

    m_healthCheckReply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply]()
    {
        reply->deleteLater();

        if (m_healthCheckReply == reply)
        {
            m_healthCheckReply = nullptr;
        }

        const int statusCode = reply->attribute(
                                   QNetworkRequest::HttpStatusCodeAttribute).toInt();

        const QByteArray responseBody = reply->readAll();

        if (reply->error() == QNetworkReply::NoError)
        {
            if ((m_serverProcess == nullptr)
                || (m_serverProcess->state() == QProcess::NotRunning))
            {
                return;
            }

            const QJsonDocument responseDocument = QJsonDocument::fromJson(responseBody);
            const QString responseInstanceId = responseDocument.isObject()
                                               ? responseDocument.object().value(QStringLiteral("instance_id")).toString()
                                               : QString();

            if (statusCode != 200 || responseInstanceId != m_instanceId)
            {
                qDebug() << "[TTS]   health check#" << m_healthCheckCount
                         << "reached a different API instance";
                return;
            }

            qDebug() << "[TTS]   health check PASSED, HTTP" << statusCode;

            if (m_healthCheckTimer != nullptr)
            {
                m_healthCheckTimer->stop();
            }

            if (m_startupTimeoutTimer != nullptr)
            {
                m_startupTimeoutTimer->stop();
            }

            m_isReady = true;
            m_restartAttempts = 0;
            emit StatusChanged(QStringLiteral("TTS 服务就绪！"));
            emit ServerReady();
            return;
        }

        qDebug() << "[TTS]   health check#" << m_healthCheckCount
                  << "failed:" << reply->errorString();
    });
}

void TtsServerManager::ScheduleRestart()
{
    if (m_configPath.isEmpty() || m_restartScheduled
        || (m_restartAttempts >= MAX_RESTART_ATTEMPTS))
    {
        return;
    }

    m_restartAttempts += 1;
    m_restartScheduled = true;
    emit StatusChanged(QStringLiteral("TTS 服务异常退出，正在尝试恢复..."));

    QTimer::singleShot(RESTART_DELAY_MS, this, [this]()
    {
        if (!m_restartScheduled || (m_serverProcess != nullptr))
        {
            return;
        }

        m_restartScheduled = false;
        Start(m_configPath);
    });
}

} // namespace vpet
