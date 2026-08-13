#include "vpet/say_dialog.h"

#include <QRandomGenerator>
#include <QMap>
#include <QStringList>

namespace vpet
{

namespace
{

// Say 分组名 -> 台词列表
QMap<QString, QStringList> LoadSayDialogData()
{
    QMap<QString, QStringList> data;

    data[QStringLiteral("say_self")] = QStringList{
        QStringLiteral("这是我自己的小秘密~"),
        QStringLiteral("我觉得今天状态不错！"),
        QStringLiteral("来看看我的新发现吧~"),
        QStringLiteral("嘻嘻，你猜我在想什么？"),
        QStringLiteral("这样看起来还不错嘛~")
    };

    data[QStringLiteral("say_serious")] = QStringList{
        QStringLiteral("这个问题需要认真思考一下..."),
        QStringLiteral("让我分析分析，嗯..."),
        QStringLiteral("事情变得复杂起来了呢。"),
        QStringLiteral("不容小觑的问题啊。"),
        QStringLiteral("果然和我想的一样。")
    };

    data[QStringLiteral("say_shining")] = QStringList{
        QStringLiteral("看我闪闪发光！"),
        QStringLiteral("今天也要加油哦~"),
        QStringLiteral("我可是很厉害的！"),
        QStringLiteral("闪耀时刻到了！"),
        QStringLiteral("没什么能难倒我~")
    };

    data[QStringLiteral("say_shy")] = QStringList{
        QStringLiteral("唔...有点不好意思说"),
        QStringLiteral("那个...其实..."),
        QStringLiteral("呀，被发现了！"),
        QStringLiteral("好害羞啊……"),
        QStringLiteral("不要一直盯着我看啦~")
    };

    return data;
}

const QMap<QString, QStringList> s_sayDialogData = LoadSayDialogData();

} // anonymous namespace

QString SayDialog::GetRandomText(const QString &groupName)
{
    // 检查参数有效性
    if (groupName.isEmpty())
    {
        return QString();
    }

    auto it = s_sayDialogData.find(groupName);

    if (it == s_sayDialogData.end())
    {
        return QString();
    }

    const QStringList &lines = it.value();

    if (lines.isEmpty())
    {
        return QString();
    }

    const int index = QRandomGenerator::global()->bounded(lines.size());
    return lines.at(index);
}

} // namespace vpet
