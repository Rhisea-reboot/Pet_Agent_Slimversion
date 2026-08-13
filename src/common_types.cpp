#include "vpet/common_types.h"

#include <QMap>

namespace vpet
{

ANIMATION_TYPE StringToAnimationType(const QString &segmentName)
{
    const QString upper = segmentName.toUpper();

    if (upper == "A" || upper == "START")
    {
        return ANIMATION_TYPE::A_START;
    }

    if (upper == "B" || upper == "LOOP")
    {
        return ANIMATION_TYPE::B_LOOP;
    }

    if (upper == "C" || upper == "END")
    {
        return ANIMATION_TYPE::C_END;
    }

    return ANIMATION_TYPE::SINGLE;
}

QString AnimationTypeToString(ANIMATION_TYPE type)
{
    switch (type)
    {
    case ANIMATION_TYPE::A_START:
        return QStringLiteral("A_START");

    case ANIMATION_TYPE::B_LOOP:
        return QStringLiteral("B_LOOP");

    case ANIMATION_TYPE::C_END:
        return QStringLiteral("C_END");

    case ANIMATION_TYPE::SINGLE:
    default:
        return QStringLiteral("SINGLE");
    }
}

PET_MOOD StringToPetMood(const QString &moodName)
{
    const QString lower = moodName.toLower();

    if (lower == "happy")
    {
        return PET_MOOD::HAPPY;
    }

    if (lower == "ill" || lower == "sick")
    {
        return PET_MOOD::ILL;
    }

    if (lower == "poorcondition" || lower == "poor_condition")
    {
        return PET_MOOD::POOR_CONDITION;
    }

    return PET_MOOD::NORMAL;
}

QString PetMoodToString(PET_MOOD mood)
{
    switch (mood)
    {
    case PET_MOOD::HAPPY:
        return QStringLiteral("happy");

    case PET_MOOD::ILL:
        return QStringLiteral("ill");

    case PET_MOOD::POOR_CONDITION:
        return QStringLiteral("poor_condition");

    case PET_MOOD::NORMAL:
    default:
        return QStringLiteral("normal");
    }
}

int GetPetStatePriority(PET_STATE state)
{
    switch (state)
    {
    case PET_STATE::DRAGGING:
        return 3;

    case PET_STATE::TOUCH_HEAD:
    case PET_STATE::TOUCH_BODY:
        return 2;

    case PET_STATE::IDLE:
    case PET_STATE::WALKING:
    case PET_STATE::SAYING:
        return 1;

    default:
        return 0;
    }
}

QString PetStateToActionName(PET_STATE state)
{
    switch (state)
    {
    case PET_STATE::IDLE:
        return QStringLiteral("normal");

    case PET_STATE::WALKING:
        return QStringLiteral("walk");

    case PET_STATE::SAYING:
        return QStringLiteral("say");

    case PET_STATE::TOUCH_HEAD:
        return QStringLiteral("touch_head");

    case PET_STATE::TOUCH_BODY:
        return QStringLiteral("touch_body");

    case PET_STATE::DRAGGING:
        return QStringLiteral("raised_static");

    default:
        return QStringLiteral("normal");
    }
}

} // namespace vpet
