#ifndef VPET_SAY_DIALOG_H
#define VPET_SAY_DIALOG_H

#include <QString>

namespace vpet
{

/**
 * @brief Say 对话文本数据库
 *
 * 提供根据 Say 分组名随机获取台词的功能。
 * 所有方法均为静态方法，无需实例化。
 */
class SayDialog
{
public:
    /**
     * @brief 根据分组名获取随机台词
     * @param[in] groupName Say 分组名，如 "say_self"、"say_serious"
     * @return 随机选择的台词文本；分组不存在时返回空字符串
     */
    static QString GetRandomText(const QString &groupName);
};

} // namespace vpet

#endif // VPET_SAY_DIALOG_H
