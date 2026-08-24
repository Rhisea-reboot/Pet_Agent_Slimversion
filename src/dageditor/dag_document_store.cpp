#include "vpet/dageditor/dag_document_store.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace vpet
{

DagDocumentStore::DagDocumentStore(QObject *parent)
    : QObject(parent)
    , m_configPath()
    , m_document()
    , m_revision(0)
    , m_fingerprint()
    , m_validator(nullptr)
{
}

bool DagDocumentStore::Initialize(const QString &configPath,
                                  const AgentNodeCatalog *catalog,
                                  QString &errorMessage)
{
    m_configPath = QFileInfo(configPath).absoluteFilePath();

    if (m_configPath.isEmpty())
    {
        errorMessage = QStringLiteral("DAG document store requires a config path.");
        return false;
    }

    // 重建校验器（目录可能由测试注入）。
    m_validator = AgentDagValidator(catalog);

    QFile configFile(m_configPath);

    if (!QFileInfo::exists(m_configPath))
    {
        // 首次使用：以空文档为基线，revision 从 1 开始。
        m_revision = 1;
        m_fingerprint.clear();
        errorMessage.clear();
        return true;
    }

    if (!configFile.open(QIODevice::ReadOnly))
    {
        errorMessage = QStringLiteral("Cannot open DAG config for reading: %1")
                           .arg(configFile.errorString());
        return false;
    }

    const QByteArray rawBytes = configFile.readAll();
    DagDocument loaded;

    if (!DagDocument::FromJsonData(rawBytes, loaded, errorMessage))
    {
        return false;
    }

    m_document = loaded;
    m_revision = loaded.GetRevision() + 1; // 进程内单调：至少比磁盘版本新一次
    m_fingerprint = Fingerprint(rawBytes);
    errorMessage.clear();
    return true;
}

const DagDocument &DagDocumentStore::GetDocument() const
{
    return m_document;
}

quint64 DagDocumentStore::GetRevision() const
{
    return m_revision;
}

QString DagDocumentStore::GetConfigPath() const
{
    return m_configPath;
}

DagDocumentStore::SaveStatus DagDocumentStore::Save(const DagDocument &document,
                                                    quint64 baseRevision,
                                                    QVector<DagValidationIssue> &issuesOut,
                                                    QString &errorMessage)
{
    issuesOut.clear();
    errorMessage.clear();

    if (baseRevision != m_revision)
    {
        return SaveStatus::Conflict;
    }

    issuesOut = m_validator.Validate(document);

    if (AgentDagValidator::HasErrors(issuesOut))
    {
        return SaveStatus::Invalid;
    }

    DagDocument committed = document;
    committed.SetRevision(m_revision + 1);
    const QByteArray serialized = committed.ToJsonData();

    // 先备份旧文件（存在时），再做原子替换。
    if (QFileInfo::exists(m_configPath))
    {
        const QString backupPath = m_configPath + QStringLiteral(".bak");

        QFile::remove(backupPath);

        if (!QFile::copy(m_configPath, backupPath))
        {
            errorMessage = QStringLiteral("Cannot create backup file: %1.bak")
                               .arg(m_configPath);
            return SaveStatus::Invalid;
        }
    }

    QSaveFile saveFile(m_configPath);

    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        errorMessage = QStringLiteral("Cannot open DAG config for writing: %1")
                           .arg(saveFile.errorString());
        return SaveStatus::Invalid;
    }

    if (saveFile.write(serialized) != serialized.size())
    {
        errorMessage = QStringLiteral("Cannot write DAG config: %1")
                           .arg(saveFile.errorString());
        saveFile.cancelWriting();
        return SaveStatus::Invalid;
    }

    if (!saveFile.commit())
    {
        errorMessage = QStringLiteral("Cannot commit DAG config: %1")
                           .arg(saveFile.errorString());
        return SaveStatus::Invalid;
    }

    m_document = committed;
    m_revision = committed.GetRevision();
    m_fingerprint = Fingerprint(serialized);
    emit DocumentSaved(m_revision);
    return SaveStatus::Saved;
}

bool DagDocumentStore::AdoptExternalFile(QString &errorMessage)
{
    errorMessage.clear();

    QFile configFile(m_configPath);

    if (!configFile.open(QIODevice::ReadOnly))
    {
        errorMessage = QStringLiteral("Cannot reopen DAG config after external change: %1")
                           .arg(configFile.errorString());
        return false;
    }

    const QByteArray rawBytes = configFile.readAll();

    if (rawBytes.isEmpty() || (Fingerprint(rawBytes) == m_fingerprint))
    {
        return false; // 内容未变化（或被清空），不采纳
    }

    DagDocument loaded;

    if (!DagDocument::FromJsonData(rawBytes, loaded, errorMessage))
    {
        // 外部写入了坏内容：保留内存基线，交由上层提示。
        return false;
    }

    m_document = loaded;
    m_revision += 1; // 以磁盘内容为新基线并单调递增
    m_document.SetRevision(m_revision);
    m_fingerprint = Fingerprint(rawBytes);
    emit ExternalChanged(m_revision);
    return true;
}

QVector<DagValidationIssue> DagDocumentStore::Validate(const DagDocument &document) const
{
    return m_validator.Validate(document);
}

QByteArray DagDocumentStore::Fingerprint(const QByteArray &rawBytes)
{
    return QCryptographicHash::hash(rawBytes, QCryptographicHash::Sha256);
}

} // namespace vpet
