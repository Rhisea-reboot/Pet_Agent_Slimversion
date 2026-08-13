#include "vpet/speech/voice_input_manager.h"

#include <QAudioDevice>
#include <QAudioInput>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QMediaFormat>
#include <QMediaRecorder>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariant>

namespace vpet
{

namespace
{

const QString TOOLS_DIRECTORY_NAME = QStringLiteral("tools");
const QString ASR_SCRIPT_PATH = QStringLiteral("tools/asr/sensevoice_transcribe.py");
const QString ASR_MODEL_DIR = QStringLiteral("models/sensevoice");
const QString VOICE_INPUT_DIRECTORY_NAME = QStringLiteral("vpet_voice_input");
const QString RECORD_FILE_NAME = QStringLiteral("voice_input.wav");
constexpr int RECORDER_STOP_TIMEOUT_MS = 5000;
constexpr int ASR_PROCESS_TIMEOUT_MS = 60000;

} // anonymous namespace

VoiceInputManager::VoiceInputManager(QObject *parent)
    : QObject(parent)
    , m_captureSession(new QMediaCaptureSession(this))
    , m_audioInput(nullptr)
    , m_mediaRecorder(new QMediaRecorder(this))
    , m_asrProcess(new QProcess(this))
    , m_recorderStopTimer(new QTimer(this))
    , m_asrTimeoutTimer(new QTimer(this))
    , m_recordSessionDirectory()
    , m_recordInputDirectory()
    , m_recordOutputDirectory()
    , m_recordAudioPath()
    , m_asrOutputFilePath()
    , m_isRecording(false)
    , m_awaitingRecorderStop(false)
    , m_asrTimedOut(false)
{
    m_audioInput = new QAudioInput(QMediaDevices::defaultAudioInput(), this);
    m_captureSession->setAudioInput(m_audioInput);
    m_captureSession->setRecorder(m_mediaRecorder);

    QMediaFormat mediaFormat;
    mediaFormat.setFileFormat(QMediaFormat::Wave);
    mediaFormat.setAudioCodec(QMediaFormat::AudioCodec::Wave);
    m_mediaRecorder->setMediaFormat(mediaFormat);
    m_mediaRecorder->setQuality(QMediaRecorder::HighQuality);

    connect(m_mediaRecorder, &QMediaRecorder::errorOccurred,
            this, &VoiceInputManager::OnRecorderError);
    connect(m_mediaRecorder, &QMediaRecorder::recorderStateChanged,
            this, &VoiceInputManager::OnRecorderStateChanged);
    connect(m_asrProcess,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this,
            &VoiceInputManager::OnAsrProcessFinished);
    connect(m_asrProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error)
    {
        if ((error != QProcess::FailedToStart) || m_asrTimedOut)
        {
            return;
        }

        m_asrTimeoutTimer->stop();
        CleanupRecordDirectory();
        emit TranscriptionFailed(QStringLiteral("Failed to start SenseVoice ASR process: %1")
                                 .arg(m_asrProcess->errorString()));
    });
    connect(m_asrProcess, &QProcess::readyReadStandardOutput, this, [this]()
    {
        m_asrProcess->readAllStandardOutput();
    });
    connect(m_asrProcess, &QProcess::readyReadStandardError, this, [this]()
    {
        m_asrProcess->readAllStandardError();
    });

    m_recorderStopTimer->setSingleShot(true);
    m_asrTimeoutTimer->setSingleShot(true);
    connect(m_recorderStopTimer, &QTimer::timeout,
            this, &VoiceInputManager::OnRecorderStopTimeout);
    connect(m_asrTimeoutTimer, &QTimer::timeout,
            this, &VoiceInputManager::OnAsrProcessTimeout);
}

VoiceInputManager::~VoiceInputManager()
{
    m_recorderStopTimer->stop();
    m_asrTimeoutTimer->stop();

    if (m_mediaRecorder->recorderState() == QMediaRecorder::RecordingState)
    {
        m_mediaRecorder->stop();
    }

    if (m_asrProcess->state() != QProcess::NotRunning)
    {
        m_asrProcess->kill();
        m_asrProcess->waitForFinished(3000);
    }

    CleanupRecordDirectory();
}

bool VoiceInputManager::StartRecording()
{
    if (m_isRecording || m_awaitingRecorderStop)
    {
        emit TranscriptionFailed(QStringLiteral("Voice input is already recording."));
        return false;
    }

    if (m_asrProcess->state() != QProcess::NotRunning)
    {
        emit TranscriptionFailed(QStringLiteral("Voice ASR process is still running."));
        return false;
    }

    QString errorMessage;

    if (!PrepareRecordDirectory(errorMessage))
    {
        emit TranscriptionFailed(errorMessage);
        return false;
    }

    m_mediaRecorder->setOutputLocation(QUrl::fromLocalFile(m_recordAudioPath));
    m_mediaRecorder->record();
    m_isRecording = true;
    m_awaitingRecorderStop = false;
    emit RecordingStarted();

    return true;
}

bool VoiceInputManager::StopRecording()
{
    if (!m_isRecording)
    {
        emit TranscriptionFailed(QStringLiteral("Voice input is not recording."));
        return false;
    }

    if (m_awaitingRecorderStop)
    {
        return true;
    }

    // stop() 是异步的：必须等 recorderStateChanged → StoppedState 后再启动 ASR，
    // 否则 WAV 可能尚未写完，识别会失败或截断。
    m_awaitingRecorderStop = true;
    m_isRecording = false;
    m_mediaRecorder->stop();
    m_recorderStopTimer->start(RECORDER_STOP_TIMEOUT_MS);

    return true;
}

bool VoiceInputManager::IsRecording() const
{
    return m_isRecording;
}

void VoiceInputManager::OnAsrProcessFinished(int exitCode, int exitStatus)
{
    m_asrTimeoutTimer->stop();

    if (m_asrTimedOut)
    {
        m_asrTimedOut = false;
        CleanupRecordDirectory();
        return;
    }

    if ((exitStatus != QProcess::NormalExit) || (exitCode != 0))
    {
        const qsizetype standardOutputSize = m_asrProcess->readAllStandardOutput().size();
        const qsizetype standardErrorSize = m_asrProcess->readAllStandardError().size();
        const QString message = QStringLiteral(
                                    "Voice ASR process failed. exit code: %1, stdout bytes: %2, stderr bytes: %3")
                                .arg(exitCode)
                                .arg(standardOutputSize)
                                .arg(standardErrorSize);

        CleanupRecordDirectory();
        emit TranscriptionFailed(message);
        return;
    }

    QString text;
    QString errorMessage;

    if (!ReadTranscriptionText(text, errorMessage))
    {
        CleanupRecordDirectory();
        emit TranscriptionFailed(errorMessage);
        return;
    }

    CleanupRecordDirectory();
    emit TranscriptionCompleted(text);
}

void VoiceInputManager::OnRecorderError()
{
    const QString message = QStringLiteral("Voice recorder error: %1").arg(
                            m_mediaRecorder->errorString());

    m_isRecording = false;
    m_awaitingRecorderStop = false;
    m_recorderStopTimer->stop();

    CleanupRecordDirectory();
    emit TranscriptionFailed(message);
}

void VoiceInputManager::OnRecorderStateChanged(QMediaRecorder::RecorderState state)
{
    if (!m_awaitingRecorderStop)
    {
        return;
    }

    if (state != QMediaRecorder::StoppedState)
    {
        return;
    }

    m_awaitingRecorderStop = false;
    m_recorderStopTimer->stop();
    emit RecordingStopped(m_recordAudioPath);

    QString errorMessage;

    if (!StartAsrProcess(errorMessage))
    {
        CleanupRecordDirectory();
        emit TranscriptionFailed(errorMessage);
    }
}

void VoiceInputManager::OnRecorderStopTimeout()
{
    if (!m_awaitingRecorderStop)
    {
        return;
    }

    m_awaitingRecorderStop = false;
    m_isRecording = false;
    m_mediaRecorder->stop();
    CleanupRecordDirectory();
    emit TranscriptionFailed(QStringLiteral("Voice recorder did not stop before the timeout."));
}

void VoiceInputManager::OnAsrProcessTimeout()
{
    if ((m_asrProcess == nullptr) || (m_asrProcess->state() == QProcess::NotRunning))
    {
        return;
    }

    m_asrTimedOut = true;
    m_asrProcess->kill();
    emit TranscriptionFailed(QStringLiteral("Voice ASR process timed out."));
}

bool VoiceInputManager::PrepareRecordDirectory(QString &errorMessage)
{
    if (!CleanupRecordDirectory())
    {
        errorMessage = QStringLiteral("Failed to clean the previous voice input directory.");
        return false;
    }

    const QString baseDirectory = QStandardPaths::writableLocation(QStandardPaths::TempLocation);

    if (baseDirectory.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Voice input temp directory is empty.");
        return false;
    }

    const QString timestamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_hhmmss_zzz"));
    m_recordSessionDirectory = QDir(baseDirectory).filePath(
                                   VOICE_INPUT_DIRECTORY_NAME + QStringLiteral("/") + timestamp);
    QDir directory;

    if (!directory.mkpath(m_recordSessionDirectory))
    {
        m_recordSessionDirectory.clear();
        errorMessage = QStringLiteral("Failed to create voice input directory.");
        return false;
    }

    m_recordInputDirectory = QDir(m_recordSessionDirectory).filePath(QStringLiteral("input"));
    m_recordOutputDirectory = QDir(m_recordSessionDirectory).filePath(QStringLiteral("output"));

    if (!directory.mkpath(m_recordInputDirectory) || !directory.mkpath(m_recordOutputDirectory))
    {
        CleanupRecordDirectory();
        errorMessage = QStringLiteral("Failed to create voice ASR working directories.");
        return false;
    }

    m_recordAudioPath = QDir(m_recordInputDirectory).filePath(RECORD_FILE_NAME);
    m_asrOutputFilePath = QDir(m_recordOutputDirectory).filePath(QStringLiteral("result.txt"));

    return true;
}

bool VoiceInputManager::StartAsrProcess(QString &errorMessage)
{
    if (m_recordAudioPath.trimmed().isEmpty() || !QFileInfo::exists(m_recordAudioPath))
    {
        errorMessage = QStringLiteral("Voice input audio file does not exist.");
        return false;
    }

    const QString projectRootPath = FindProjectRootPath();

    if (projectRootPath.isEmpty())
    {
        errorMessage = QStringLiteral("Project root directory is not found.");
        return false;
    }

    const QString scriptPath = QDir(projectRootPath).filePath(ASR_SCRIPT_PATH);

    if (!QFileInfo::exists(scriptPath))
    {
        errorMessage = QStringLiteral("SenseVoice ASR script is not found.");
        return false;
    }

    const QString modelPath = QDir(projectRootPath).filePath(ASR_MODEL_DIR);

    if (!QFileInfo::exists(QDir(modelPath).filePath(QStringLiteral("model.int8.onnx")))
        || !QFileInfo::exists(QDir(modelPath).filePath(QStringLiteral("tokens.txt"))))
    {
        errorMessage = QStringLiteral("SenseVoice model is not found in %1. Please run tools/asr/download_sensevoice.py first.")
                           .arg(modelPath);
        return false;
    }

    const QString pythonExecutable = FindPythonExecutable();

    if (pythonExecutable.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Python executable is not found for voice ASR.");
        return false;
    }

    QStringList arguments;
    arguments.append(scriptPath);
    arguments.append(QStringLiteral("-i"));
    arguments.append(m_recordInputDirectory);
    arguments.append(QStringLiteral("-o"));
    arguments.append(m_recordOutputDirectory);
    arguments.append(QStringLiteral("-m"));
    arguments.append(modelPath);

    m_asrProcess->setWorkingDirectory(projectRootPath);
    m_asrTimedOut = false;
    m_asrProcess->start(pythonExecutable, arguments);
    m_asrTimeoutTimer->start(ASR_PROCESS_TIMEOUT_MS);

    return true;
}

QString VoiceInputManager::FindProjectRootPath() const
{
    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    const QStringList candidatePaths =
    {
        // exe 同目录（打包发行布局）
        applicationDirectory,
        // 工作目录
        QDir::currentPath(),
        // exe 上级目录（exe 在 build/ 下，项目根在上级）
        QDir(applicationDirectory).absoluteFilePath(QStringLiteral("..")),
        // exe 上两级目录（Qt Creator Debug 布局 build/Debug/ 下）
        QDir(applicationDirectory).absoluteFilePath(QStringLiteral("../.."))
    };

    for (const QString &candidatePath : candidatePaths)
    {
        const QFileInfo fileInfo(candidatePath);
        const QDir directory(candidatePath);

        if (fileInfo.exists() && fileInfo.isDir()
            && QFileInfo::exists(directory.filePath(TOOLS_DIRECTORY_NAME)))
        {
            return fileInfo.absoluteFilePath();
        }
    }

    return QString();
}

QString VoiceInputManager::FindPythonExecutable() const
{
    const QString projectRootPath = FindProjectRootPath();
    const QStringList candidatePaths =
    {
        QDir(projectRootPath).filePath(QStringLiteral("runtime/Scripts/python.exe")),
        QDir(projectRootPath).filePath(QStringLiteral("runtime/python.exe")),
        QStringLiteral("python")
    };

    for (const QString &candidatePath : candidatePaths)
    {
        if (candidatePath == QStringLiteral("python"))
        {
            return candidatePath;
        }

        if (QFileInfo::exists(candidatePath))
        {
            return QFileInfo(candidatePath).absoluteFilePath();
        }
    }

    return QString();
}

bool VoiceInputManager::ReadTranscriptionText(QString &text, QString &errorMessage) const
{
    text.clear();

    if (m_asrOutputFilePath.trimmed().isEmpty() || !QFileInfo::exists(m_asrOutputFilePath))
    {
        errorMessage = QStringLiteral("Voice ASR output file is not found.");
        return false;
    }

    QFile file(m_asrOutputFilePath);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        errorMessage = QStringLiteral("Failed to open voice ASR output file.");
        return false;
    }

    const QString outputText = QString::fromUtf8(file.readLine()).trimmed();
    file.close();

    // SenseVoice treats a valid silent recording as a successful empty result.
    // Keep that distinction so MainWindow can ignore it without reporting a process failure.
    text = outputText;

    return true;
}

bool VoiceInputManager::CleanupRecordDirectory()
{
    if (!m_recordSessionDirectory.isEmpty())
    {
        QDir sessionDirectory(m_recordSessionDirectory);

        if (sessionDirectory.exists() && !sessionDirectory.removeRecursively())
        {
            qWarning() << "[VoiceInput] Failed to remove temporary session directory.";
            return false;
        }
    }

    m_recordSessionDirectory.clear();
    m_recordInputDirectory.clear();
    m_recordOutputDirectory.clear();
    m_recordAudioPath.clear();
    m_asrOutputFilePath.clear();

    return true;
}

} // namespace vpet
