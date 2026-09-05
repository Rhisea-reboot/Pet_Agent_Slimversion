#include "vpet/agent/tools/itool.h"

#include <QJsonDocument>
#include <QtGlobal>
#include <cmath>

namespace vpet
{

namespace
{

constexpr int DEFAULT_ARGS_DIGEST_CHARS = 200;

/**
 * @brief 判断 JSON 值是否符合声明的参数类型。
 * @param[in] value JSON 值。
 * @param[in] type 声明类型字符串。
 * @return 类型匹配返回 true。
 */
bool MatchesDeclaredType(const QJsonValue &value, const QString &type)
{
    if (type == QStringLiteral("string"))
    {
        return value.isString();
    }

    if (type == QStringLiteral("integer"))
    {
        if (!value.isDouble())
        {
            return false;
        }

        const double number = value.toDouble();
        return std::isfinite(number) && std::floor(number) == number
               && number >= -9007199254740991.0 && number <= 9007199254740991.0;
    }

    if (type == QStringLiteral("boolean"))
    {
        return value.isBool();
    }

    if (type == QStringLiteral("array"))
    {
        return value.isArray();
    }

    // 未知或未支持类型必须 fail-closed
    return false;
}

} // anonymous namespace

ITool::ITool(QObject *parent)
    : QObject(parent)
{
}

bool ValidateToolArguments(const _tagToolSpec &spec,
                           const QJsonObject &arguments,
                           QString &errorMessage)
{
    errorMessage.clear();

    if (spec.name.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Tool spec name is empty.");
        return false;
    }

    for (const _tagToolParameterSchema &parameter : spec.parameters)
    {
        if (!arguments.contains(parameter.name))
        {
            if (parameter.required)
            {
                errorMessage = QStringLiteral("Tool '%1' is missing required argument '%2'.")
                                   .arg(spec.name, parameter.name);
                return false;
            }

            continue;
        }

        const QJsonValue value = arguments.value(parameter.name);

        if (!MatchesDeclaredType(value, parameter.type))
        {
            errorMessage = QStringLiteral("Tool '%1' argument '%2' does not match declared type '%3'.")
                               .arg(spec.name, parameter.name, parameter.type);
            return false;
        }

        if (!parameter.enumValues.isEmpty())
        {
            if (!value.isString())
            {
                errorMessage = QStringLiteral("Tool '%1' argument '%2' must be a string for enum validation.")
                                   .arg(spec.name, parameter.name);
                return false;
            }

            const QString enumText = value.toString();

            if (!parameter.enumValues.contains(enumText))
            {
                errorMessage = QStringLiteral("Tool '%1' argument '%2' value is not one of the allowed enum values.")
                                   .arg(spec.name, parameter.name);
                return false;
            }
        }
    }

    return true;
}

QString SummarizeToolArguments(const QJsonObject &arguments, int maxChars)
{
    const QString compactJson = QString::fromUtf8(
        QJsonDocument(arguments).toJson(QJsonDocument::Compact));

    if (maxChars <= 0)
    {
        return compactJson;
    }

    if (compactJson.size() <= maxChars)
    {
        return compactJson;
    }

    return compactJson.left(maxChars) + QStringLiteral("...");
}

} // namespace vpet
