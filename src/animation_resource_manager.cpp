#include "vpet/animation_resource_manager.h"

#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>

namespace vpet
{

namespace
{

const QString PNG_FILTER = QStringLiteral("*.png");

} // anonymous namespace

AnimationResourceManager::AnimationResourceManager(const QString &basePath)
    : m_basePath(basePath)
    , m_isLoaded(false)
    , m_candidates()
    , m_sayActionNames()
{
}

bool AnimationResourceManager::LoadAll()
{
    m_candidates.clear();
    m_isLoaded = false;

    LoadSingleSegmentAction(QStringLiteral("normal"),
                            m_basePath + QStringLiteral("/Nomal"),
                            PET_MOOD::NORMAL);

    LoadSegmentedAction(QStringLiteral("walk_left"),
                        m_basePath + QStringLiteral("/Walk/walk.left"));

    LoadSegmentedAction(QStringLiteral("walk_right"),
                        m_basePath + QStringLiteral("/Walk/walk.right"));

    LoadSegmentedAction(QStringLiteral("touch_head"),
                        m_basePath + QStringLiteral("/Touch_Head"));

    LoadSegmentedAction(QStringLiteral("touch_body"),
                        m_basePath + QStringLiteral("/Touch_Body"));

    LoadSegmentedAction(QStringLiteral("raised_static"),
                        m_basePath + QStringLiteral("/Raise/Raised_Static"));

    LoadSegmentedAction(QStringLiteral("raised_dynamic"),
                        m_basePath + QStringLiteral("/Raise/Raised_Dynamic"));

    if (!m_candidates.contains(QStringLiteral("raised_dynamic")))
    {
        LoadSingleSegmentAction(QStringLiteral("raised_dynamic"),
                                m_basePath + QStringLiteral("/Raise/Raised_Dynamic"),
                                PET_MOOD::NORMAL);
    }

    LoadSayActions(m_basePath + QStringLiteral("/Say"));

    m_isLoaded = (!m_candidates.isEmpty());
    return m_isLoaded;
}

AnimationClip AnimationResourceManager::GetClip(const QString &actionName, PET_MOOD mood) const
{
    AnimationClip clip(actionName, mood);

    auto actionIt = m_candidates.find(actionName);
    if (actionIt == m_candidates.end())
    {
        return clip;
    }

    const PET_MOOD resolvedMood = ResolveMood(actionName, mood);
    auto moodIt = actionIt->find(resolvedMood);
    if (moodIt == actionIt->end())
    {
        return clip;
    }

    const QMap<ANIMATION_TYPE, QList<QString>> &typeDirs = moodIt.value();
    for (auto typeIt = typeDirs.begin(); typeIt != typeDirs.end(); ++typeIt)
    {
        const ANIMATION_TYPE type = typeIt.key();
        const QList<QString> &dirs = typeIt.value();

        if (dirs.isEmpty())
        {
            continue;
        }

        const int index = QRandomGenerator::global()->bounded(dirs.size());
        AnimationSegment segment(type);

        if (segment.LoadFromDirectory(dirs[index]))
        {
            clip.AddSegment(segment);
        }
    }

    return clip;
}

bool AnimationResourceManager::HasAction(const QString &actionName) const
{
    return m_candidates.contains(actionName);
}

QList<QString> AnimationResourceManager::GetAllActionNames() const
{
    return m_candidates.keys();
}

bool AnimationResourceManager::IsLoaded() const
{
    return m_isLoaded;
}

QList<QString> AnimationResourceManager::GetSayActionNames() const
{
    return m_sayActionNames;
}

void AnimationResourceManager::LoadSegmentedAction(const QString &actionName,
                                                   const QString &actionDir)
{
    QList<_tagSegmentCandidate> candidates;
    CollectSegmentCandidates(actionDir, QString(), candidates);

    if (candidates.isEmpty())
    {
        return;
    }

    const QMap<PET_MOOD, QMap<ANIMATION_TYPE, QList<QString>>> grouped =
        GroupCandidatesByMoodAndType(candidates);

    for (auto moodIt = grouped.begin(); moodIt != grouped.end(); ++moodIt)
    {
        const PET_MOOD mood = moodIt.key();
        m_candidates[actionName][mood] = moodIt.value();
    }
}

void AnimationResourceManager::LoadSingleSegmentAction(const QString &actionName,
                                                       const QString &actionDir,
                                                       PET_MOOD mood)
{
    QDir dir(actionDir);
    if (!dir.exists())
    {
        return;
    }

    const QStringList subDirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QList<QString> candidateDirs;

    for (const QString &subDir : subDirs)
    {
        const QString fullPath = dir.absoluteFilePath(subDir);

        if (IsLeafFrameDirectory(fullPath))
        {
            candidateDirs.append(fullPath);
        }
    }

    if (candidateDirs.isEmpty())
    {
        return;
    }

    m_candidates[actionName][mood][ANIMATION_TYPE::SINGLE] = candidateDirs;
}

void AnimationResourceManager::LoadSayActions(const QString &sayDir)
{
    QDir dir(sayDir);

    if (!dir.exists())
    {
        return;
    }

    const QStringList subDirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString &subDir : subDirs)
    {
        const QString actionName = QStringLiteral("say_") + subDir.toLower();
        const QString actionPath = dir.absoluteFilePath(subDir);

        LoadSegmentedAction(actionName, actionPath);

        if (m_candidates.contains(actionName))
        {
            m_sayActionNames.append(actionName);
        }
    }
}

void AnimationResourceManager::CollectSegmentCandidates(const QString &currentPath,
                                                        const QString &parentName,
                                                        QList<_tagSegmentCandidate> &candidates) const
{
    QDir dir(currentPath);
    if (!dir.exists())
    {
        return;
    }

    if (IsLeafFrameDirectory(currentPath))
    {
        ANIMATION_TYPE type = ANIMATION_TYPE::SINGLE;
        PET_MOOD mood = PET_MOOD::NORMAL;

        if (ParseSegmentDirectory(dir.dirName(), parentName, type, mood))
        {
            candidates.append({mood, type, currentPath});
            return;
        }

        if (ParseSegmentDirectory(parentName, QString(), type, mood))
        {
            candidates.append({mood, type, currentPath});
        }

        return;
    }

    const QStringList subDirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString &subDir : subDirs)
    {
        const QString childPath = dir.absoluteFilePath(subDir);
        CollectSegmentCandidates(childPath, dir.dirName(), candidates);
    }
}

bool AnimationResourceManager::ParseSegmentDirectory(const QString &dirName,
                                                     const QString &parentName,
                                                     ANIMATION_TYPE &type,
                                                     PET_MOOD &mood)
{
    const QString upper = dirName.toUpper();

    if (upper.startsWith(QStringLiteral("A_")))
    {
        type = ANIMATION_TYPE::A_START;
        mood = StringToPetMood(dirName.mid(2));
        return true;
    }

    if (upper.startsWith(QStringLiteral("B_")))
    {
        type = ANIMATION_TYPE::B_LOOP;
        mood = StringToPetMood(dirName.mid(2));
        return true;
    }

    if (upper.startsWith(QStringLiteral("C_")))
    {
        type = ANIMATION_TYPE::C_END;
        mood = StringToPetMood(dirName.mid(2));
        return true;
    }

    if (upper == QStringLiteral("A"))
    {
        type = ANIMATION_TYPE::A_START;
        mood = StringToPetMood(parentName);
        return true;
    }

    if (upper == QStringLiteral("B"))
    {
        type = ANIMATION_TYPE::B_LOOP;
        mood = StringToPetMood(parentName);
        return true;
    }

    if (upper == QStringLiteral("C"))
    {
        type = ANIMATION_TYPE::C_END;
        mood = StringToPetMood(parentName);
        return true;
    }

    if (upper == QStringLiteral("SINGLE") || upper.startsWith(QStringLiteral("SINGLE_")))
    {
        type = ANIMATION_TYPE::SINGLE;
        mood = StringToPetMood(dirName.mid(7));

        if ((mood == PET_MOOD::NORMAL) && (!parentName.isEmpty()))
        {
            mood = StringToPetMood(parentName);
        }

        return true;
    }

    return false;
}

bool AnimationResourceManager::IsSegmentDirectoryName(const QString &dirName)
{
    const QString upper = dirName.toUpper();

    return (upper == QStringLiteral("A"))
           || (upper == QStringLiteral("B"))
           || (upper == QStringLiteral("C"))
           || (upper == QStringLiteral("SINGLE"))
           || upper.startsWith(QStringLiteral("A_"))
           || upper.startsWith(QStringLiteral("B_"))
           || upper.startsWith(QStringLiteral("C_"))
           || upper.startsWith(QStringLiteral("SINGLE_"));
}

bool AnimationResourceManager::IsLeafFrameDirectory(const QString &dirPath)
{
    QDir dir(dirPath);
    const QStringList filters = QStringList() << PNG_FILTER;

    return !dir.entryList(filters, QDir::Files).isEmpty();
}

QMap<PET_MOOD, QMap<ANIMATION_TYPE, QList<QString>>> AnimationResourceManager::GroupCandidatesByMoodAndType(
    const QList<_tagSegmentCandidate> &candidates)
{
    QMap<PET_MOOD, QMap<ANIMATION_TYPE, QList<QString>>> grouped;

    for (const _tagSegmentCandidate &candidate : candidates)
    {
        grouped[candidate.mood][candidate.type].append(candidate.directoryPath);
    }

    return grouped;
}

PET_MOOD AnimationResourceManager::ResolveMood(const QString &action, PET_MOOD preferredMood) const
{
    auto actionIt = m_candidates.find(action);
    if (actionIt == m_candidates.end())
    {
        return preferredMood;
    }

    const QMap<PET_MOOD, QMap<ANIMATION_TYPE, QList<QString>>> &moodMap = actionIt.value();
    if (moodMap.contains(preferredMood))
    {
        return preferredMood;
    }

    const QList<PET_MOOD> fallbacks =
    {
        PET_MOOD::NORMAL,
        PET_MOOD::HAPPY,
        PET_MOOD::ILL,
        PET_MOOD::POOR_CONDITION
    };

    for (PET_MOOD mood : fallbacks)
    {
        if (moodMap.contains(mood))
        {
            return mood;
        }
    }

    return preferredMood;
}

} // namespace vpet
