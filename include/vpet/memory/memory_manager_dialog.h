#ifndef VPET_MEMORY_MEMORY_MANAGER_DIALOG_H
#define VPET_MEMORY_MEMORY_MANAGER_DIALOG_H

#include "vpet/memory/memory_graph.h"

#include <QDialog>
#include <QStringList>

class QListWidget;
class QListWidgetItem;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QTimer;

namespace vpet
{

class AgentRuntime;

/**
 * @brief 长期记忆管理窗口
 *
 * 所有读写均通过 AgentRuntime 的非阻塞门面执行。窗口只保存当前列表
 * 的副本，不直接访问 MemoryService 的后台图或持久化文件。
 */
class MemoryManagerDialog : public QDialog
{
    Q_OBJECT

public:
    /**
     * @brief 构造记忆管理窗口
     * @param[in] runtime Agent 运行时；不得为空
     * @param[in] parent 父窗口
     */
    explicit MemoryManagerDialog(AgentRuntime *runtime, QWidget *parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~MemoryManagerDialog() override;

public slots:
    /**
     * @brief 显示窗口并请求最新记忆列表
     */
    void ShowAndRefresh();

private slots:
    /**
     * @brief 请求后台列表
     */
    void Refresh();

    /**
     * @brief 消费后台返回的列表
     */
    void PollReadyResult();

    /**
     * @brief 将选中条目载入编辑区
     * @param[in] item 列表项
     */
    void LoadSelectedEntry(QListWidgetItem *item);

    /**
     * @brief 保存编辑区中的条目
     */
    void SaveSelectedEntry();

    /**
     * @brief 逻辑删除当前条目
     */
    void ForgetSelectedEntry();

    /**
     * @brief 导出全部记忆
     */
    void ExportMemory();

    /**
     * @brief 导入记忆
     */
    void ImportMemory();

    /**
     * @brief 对当前条目提交有帮助反馈
     */
    void SubmitHelpfulFeedback();

    /**
     * @brief 对当前条目提交无帮助反馈
     */
    void SubmitUnhelpfulFeedback();

    /**
     * @brief 显示记忆服务管理操作的后台结果
     * @param[in] message 运行时日志消息
     */
    void OnRuntimeLogMessage(const QString &message);

private:
    /**
     * @brief 构造窗口控件与信号连接
     */
    void BuildUi();

    /**
     * @brief 读取当前选中条目
     * @param[out] entry 输出条目
     * @return 存在选中条目返回 true
     */
    bool GetSelectedEntry(MemoryEntry &entry) const;

    /**
     * @brief 将条目转换为列表显示文本
     * @param[in] entry 记忆条目
     * @return 显示文本
     */
    static QString EntryDisplayText(const MemoryEntry &entry);

    AgentRuntime *m_runtime;            ///< 不拥有的运行时指针
    QListWidget *m_entryList;            ///< 活跃记忆列表
    QPlainTextEdit *m_contentEdit;       ///< 条目正文编辑区
    QPlainTextEdit *m_tagsEdit;          ///< 逗号分隔标签编辑区
    QPushButton *m_saveButton;           ///< 保存按钮
    QPushButton *m_forgetButton;         ///< 删除按钮
    QPushButton *m_exportButton;         ///< 导出按钮
    QPushButton *m_importButton;         ///< 导入按钮
    QPushButton *m_helpfulButton;        ///< 有帮助反馈按钮
    QPushButton *m_unhelpfulButton;      ///< 无帮助反馈按钮
    QTimer *m_pollTimer;                 ///< 非阻塞结果轮询定时器
    QLabel *m_statusLabel;               ///< 后台管理操作状态
    QVector<MemoryEntry> m_entries;      ///< 当前列表快照
};

} // namespace vpet

#endif // VPET_MEMORY_MEMORY_MANAGER_DIALOG_H
