#include "vpet/agent/tools/tool_registry.h"

#include <QHash>
#include <QJsonObject>

namespace vpet
{

bool ToolRegistry::Register(const std::shared_ptr<ITool> &tool, QString &errorMessage)
{
    errorMessage.clear();

    if (tool == nullptr)
    {
        errorMessage = QStringLiteral("Tool registry input tool is null.");
        return false;
    }

    const _tagToolSpec spec = tool->Spec();
    const QString toolName = spec.name.trimmed();

    if (toolName.isEmpty())
    {
        errorMessage = QStringLiteral("Tool registry tool name is empty.");
        return false;
    }

    if (Contains(toolName))
    {
        errorMessage = QStringLiteral("Tool registry already contains tool '%1'.").arg(toolName);
        return false;
    }

    m_tools.append(tool);
    return true;
}

std::shared_ptr<ITool> ToolRegistry::Find(const QString &toolName) const
{
    const QString normalizedToolName = toolName.trimmed();

    for (const std::shared_ptr<ITool> &tool : m_tools)
    {
        if (tool == nullptr)
        {
            continue;
        }

        if (tool->Spec().name.trimmed() == normalizedToolName)
        {
            return tool;
        }
    }

    return nullptr;
}

bool ToolRegistry::Contains(const QString &toolName) const
{
    return Find(toolName) != nullptr;
}

QStringList ToolRegistry::ToolNames() const
{
    QStringList toolNames;

    for (const std::shared_ptr<ITool> &tool : m_tools)
    {
        if (tool == nullptr)
        {
            continue;
        }

        toolNames.append(tool->Spec().name.trimmed());
    }

    return toolNames;
}

QVector<_tagToolSpec> ToolRegistry::Specs() const
{
    QVector<_tagToolSpec> specs;

    for (const std::shared_ptr<ITool> &tool : m_tools)
    {
        if (tool == nullptr)
        {
            continue;
        }

        specs.append(tool->Spec());
    }

    return specs;
}

QJsonArray ToolRegistry::BuildLlmToolsArray() const
{
    return BuildLlmToolsArray(QStringList());
}

QJsonArray ToolRegistry::BuildLlmToolsArray(const QStringList &toolNames) const
{
    QJsonArray toolsArray;

    for (const _tagToolSpec &spec : Specs())
    {
        if (!toolNames.isEmpty() && !toolNames.contains(spec.name))
        {
            continue;
        }
        QJsonObject parametersObject;
        parametersObject[QStringLiteral("type")] = QStringLiteral("object");

        QJsonObject propertiesObject;
        QJsonArray requiredArray;

        for (const _tagToolParameterSchema &parameter : spec.parameters)
        {
            QJsonObject propertyObject;
            propertyObject[QStringLiteral("type")] = parameter.type;
            propertyObject[QStringLiteral("description")] = parameter.description;

            if (!parameter.enumValues.isEmpty())
            {
                QJsonArray enumArray;

                for (const QString &enumValue : parameter.enumValues)
                {
                    enumArray.append(enumValue);
                }

                propertyObject[QStringLiteral("enum")] = enumArray;
            }

            propertiesObject[parameter.name] = propertyObject;

            if (parameter.required)
            {
                requiredArray.append(parameter.name);
            }
        }

        parametersObject[QStringLiteral("properties")] = propertiesObject;

        if (!requiredArray.isEmpty())
        {
            parametersObject[QStringLiteral("required")] = requiredArray;
        }

        QJsonObject functionObject;
        functionObject[QStringLiteral("name")] = spec.name;
        functionObject[QStringLiteral("description")] = spec.description;
        functionObject[QStringLiteral("parameters")] = parametersObject;

        QJsonObject toolObject;
        toolObject[QStringLiteral("type")] = QStringLiteral("function");
        toolObject[QStringLiteral("function")] = functionObject;

        toolsArray.append(toolObject);
    }

    return toolsArray;
}

int ToolRegistry::Count() const
{
    return m_tools.size();
}

void ToolRegistry::Clear()
{
    m_tools.clear();
}

} // namespace vpet
