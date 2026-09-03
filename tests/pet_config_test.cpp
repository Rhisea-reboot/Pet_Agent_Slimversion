#include "vpet/pet_config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace vpet;

namespace
{

QString ReadTextFile(const QString &path)
{
    QFile file(path);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return QString();
    }

    const QString content = QString::fromUtf8(file.readAll());
    file.close();
    return content;
}

} // anonymous namespace

class PetConfigTest : public QObject
{
    Q_OBJECT

private slots:
    void ParsesDefaultsWhenFieldsMissing();
    void ClampsOutOfRangeValues();
    void RoundTripsThroughJson();
    void LoadReportsMissingFile();
    void SaveAndLoadRoundTrip();
    void SaveCreatesBackupOfPreviousFile();
};

void PetConfigTest::ParsesDefaultsWhenFieldsMissing()
{
    const PetConfig config = PetConfigFromJson(QJsonObject());

    QCOMPARE(config.volume, PET_VOLUME_DEFAULT);
    QCOMPARE(config.displayScale, PET_DISPLAY_SCALE_DEFAULT);
}

void PetConfigTest::ClampsOutOfRangeValues()
{
    QJsonObject object;
    object.insert(QStringLiteral("volume"), 5.0);
    object.insert(QStringLiteral("display_scale"), 10.0);

    PetConfig config = PetConfigFromJson(object);
    QCOMPARE(config.volume, PET_VOLUME_MAX);
    QCOMPARE(config.displayScale, PET_DISPLAY_SCALE_MAX);

    object.insert(QStringLiteral("volume"), -1.0);
    object.insert(QStringLiteral("display_scale"), 0.1);

    config = PetConfigFromJson(object);
    QCOMPARE(config.volume, PET_VOLUME_MIN);
    QCOMPARE(config.displayScale, PET_DISPLAY_SCALE_MIN);
}

void PetConfigTest::RoundTripsThroughJson()
{
    PetConfig config;
    config.volume = 0.35f;
    config.displayScale = 1.75f;

    const PetConfig parsed = PetConfigFromJson(PetConfigToJson(config));

    QCOMPARE(parsed.volume, config.volume);
    QCOMPARE(parsed.displayScale, config.displayScale);
    QVERIFY(parsed == config);
}

void PetConfigTest::LoadReportsMissingFile()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QString missingPath = QDir(temporaryDirectory.path())
                                    .filePath(QStringLiteral("missing.json"));

    PetConfig config;
    config.volume = 0.5f;
    config.displayScale = 1.5f;

    const PetConfig original = config;
    QString errorMessage;

    QVERIFY(!QFileInfo::exists(missingPath));

    const bool loaded = LoadPetConfig(missingPath, config, errorMessage);

    QVERIFY(!loaded);
    QVERIFY(!errorMessage.isEmpty());
    QVERIFY(config == original);
}

void PetConfigTest::SaveAndLoadRoundTrip()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QString configPath = QDir(temporaryDirectory.path())
                                   .filePath(PET_CONFIG_FILE_NAME);

    PetConfig config;
    config.volume = 0.75f;
    config.displayScale = 1.5f;

    QString errorMessage;
    QVERIFY(SavePetConfig(configPath, config, errorMessage));
    QVERIFY(errorMessage.isEmpty());

    PetConfig loaded;
    QVERIFY(LoadPetConfig(configPath, loaded, errorMessage));
    QVERIFY(loaded == config);
}

void PetConfigTest::SaveCreatesBackupOfPreviousFile()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QString configPath = QDir(temporaryDirectory.path())
                                   .filePath(PET_CONFIG_FILE_NAME);

    PetConfig first;
    first.volume = 1.0f;
    first.displayScale = 1.0f;

    PetConfig second;
    second.volume = 0.25f;
    second.displayScale = 2.0f;

    QString errorMessage;
    QVERIFY(SavePetConfig(configPath, first, errorMessage));
    QVERIFY(SavePetConfig(configPath, second, errorMessage));

    const QJsonDocument backupDocument =
        QJsonDocument::fromJson(ReadTextFile(configPath + QStringLiteral(".bak")).toUtf8());

    QVERIFY(backupDocument.isObject());
    QVERIFY(PetConfigFromJson(backupDocument.object()) == first);

    PetConfig loaded;
    QVERIFY(LoadPetConfig(configPath, loaded, errorMessage));
    QVERIFY(loaded == second);
}

QTEST_MAIN(PetConfigTest)
#include "pet_config_test.moc"
