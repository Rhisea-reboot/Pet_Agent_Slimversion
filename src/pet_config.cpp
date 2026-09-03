#include "vpet/pet_config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QtGlobal>

namespace vpet
{

void PetConfig::Clamp()
{
    volume = qBound(PET_VOLUME_MIN, volume, PET_VOLUME_MAX);
    displayScale = qBound(PET_DISPLAY_SCALE_MIN, displayScale, PET_DISPLAY_SCALE_MAX);
}

bool PetConfig::operator==(const PetConfig &other) const
{
    return qFuzzyCompare(volume, other.volume)
           && qFuzzyCompare(displayScale, other.displayScale);
}

bool PetConfig::operator!=(const PetConfig &other) const
{
    return !(*this == other);
}

PetConfig PetConfigFromJson(const QJsonObject &object)
{
    PetConfig config;
    config.volume = static_cast<float>(
        object.value(QStringLiteral("volume")).toDouble(PET_VOLUME_DEFAULT));
    config.displayScale = static_cast<float>(
        object.value(QStringLiteral("display_scale")).toDouble(PET_DISPLAY_SCALE_DEFAULT));
    config.Clamp();
    return config;
}

QJsonObject PetConfigToJson(const PetConfig &config)
{
    QJsonObject object;
    object.insert(QStringLiteral("volume"), static_cast<double>(config.volume));
    object.insert(QStringLiteral("display_scale"), static_cast<double>(config.displayScale));
    return object;
}

bool LoadPetConfig(const QString &configPath, PetConfig &config, QString &errorMessage)
{
    if (configPath.isEmpty())
    {
        errorMessage = QStringLiteral("config path is empty");
        return false;
    }

    QFile file(configPath);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        errorMessage = QStringLiteral("cannot open config file: %1").arg(configPath);
        return false;
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!document.isObject())
    {
        errorMessage = QStringLiteral("invalid JSON in: %1").arg(configPath);
        return false;
    }

    config = PetConfigFromJson(document.object());
    errorMessage.clear();
    return true;
}

bool SavePetConfig(const QString &configPath, const PetConfig &config, QString &errorMessage)
{
    if (configPath.isEmpty())
    {
        errorMessage = QStringLiteral("config path is empty");
        return false;
    }

    const QByteArray serialized =
        QJsonDocument(PetConfigToJson(config)).toJson(QJsonDocument::Indented);

    // 先备份旧文件（存在时），再做原子替换。
    if (QFileInfo::exists(configPath))
    {
        const QString backupPath = configPath + QStringLiteral(".bak");

        QFile::remove(backupPath);

        if (!QFile::copy(configPath, backupPath))
        {
            errorMessage = QStringLiteral("cannot create backup file: %1.bak")
                               .arg(configPath);
            return false;
        }
    }

    QSaveFile saveFile(configPath);

    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        errorMessage = QStringLiteral("cannot open config for writing: %1 (%2)")
                           .arg(configPath, saveFile.errorString());
        return false;
    }

    if (saveFile.write(serialized) != serialized.size())
    {
        errorMessage = QStringLiteral("cannot write config: %1")
                           .arg(saveFile.errorString());
        saveFile.cancelWriting();
        return false;
    }

    if (!saveFile.commit())
    {
        errorMessage = QStringLiteral("cannot commit config: %1")
                           .arg(saveFile.errorString());
        return false;
    }

    errorMessage.clear();
    return true;
}

QString FindPetConfigPath()
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QStringList candidatePaths =
    {
        QDir(exeDir).filePath(PET_CONFIG_FILE_NAME),
        QDir::currentPath() + QStringLiteral("/") + PET_CONFIG_FILE_NAME,
        QDir(exeDir).absoluteFilePath(QStringLiteral("../") + PET_CONFIG_FILE_NAME),
        QDir(exeDir).absoluteFilePath(QStringLiteral("../../") + PET_CONFIG_FILE_NAME)
    };

    for (const QString &candidatePath : candidatePaths)
    {
        if (QFileInfo::exists(candidatePath))
        {
            return QFileInfo(candidatePath).absoluteFilePath();
        }
    }

    return QString();
}

} // namespace vpet
