#ifndef VPET_MEMORY_EMBEDDING_CLIENT_H
#define VPET_MEMORY_EMBEDDING_CLIENT_H

#include <QString>
#include <QVector>

#include <functional>
#include <cstdint>
#include <memory>

namespace vpet
{

/**
 * @brief 本地 BGE embedding 配置
 *
 * 仅支持 local_onnx 后端；embedding 阶段绝不将记忆文本发送到远端服务。
 */
struct EmbeddingConfig
{
    bool enabled = false;              ///< embedding 功能开关（与 automatic_extraction 无关）
    QString backend = QStringLiteral("local_onnx"); ///< 后端类型，目前仅 local_onnx
    QString model = QStringLiteral("BAAI/bge-small-zh-v1.5"); ///< 模型标识（用于向量库校验）
    QString modelDir;                  ///< 模型目录；为空时使用安装目录 models/embedding/<model>
    QString onnxModel = QStringLiteral("model.onnx"); ///< 模型文件名（相对 modelDir）
    QString tokenizerFile = QStringLiteral("tokenizer.json"); ///< 分词器文件（相对 modelDir）
    QString vectorStore = QStringLiteral("sqlite"); ///< 向量存储类型，目前仅 sqlite
    QString vectorDb;                  ///< 向量库路径；为空时使用 <dataDir>/memory/vectors.sqlite3
    int maxSequenceLength = 512;       ///< 输入序列最大长度
    QString device = QStringLiteral("cpu"); ///< 推理设备，目前仅 cpu
    QString queryInstruction = QStringLiteral("为这个句子生成表示以用于检索相关文章："); ///< 查询指令前缀
};

/**
 * @brief 记忆向量化抽象接口
 *
 * 生产实现为 EmbeddingClient（本地 ONNX）；测试可注入确定性 fake 实现，
 * 以便在无模型文件的环境下验证级联检索路径。
 */
class MemoryEmbedder
{
public:
    virtual ~MemoryEmbedder() = default;

    /**
     * @brief 模型是否已就绪可推理
     * @return 就绪返回 true
     */
    virtual bool IsReady() const = 0;

    /**
     * @brief 模型标识（用于向量库维度/模型校验）
     * @return 模型标识
     */
    virtual QString ModelId() const = 0;

    /**
     * @brief 输出向量维度；未就绪时返回 0
     * @return 向量维度
     */
    virtual int Dimension() const = 0;

    /**
     * @brief 对文档文本生成向量（无指令前缀）
     * @param[in] text 文本
     * @param[out] outVector 输出的 L2 归一化向量
     * @return 成功返回 true
     */
    virtual bool EmbedDocument(const QString &text, QVector<float> &outVector) = 0;

    /**
     * @brief 对查询文本生成向量（带指令前缀）
     * @param[in] text 文本
     * @param[out] outVector 输出的 L2 归一化向量
     * @return 成功返回 true
     */
    virtual bool EmbedQuery(const QString &text, QVector<float> &outVector) = 0;
};

/**
 * @brief 本地 BGE ONNX embedding 客户端
 *
 * 通过动态加载官方 onnxruntime.dll 的纯 C 入口 OrtGetApiBase 使用推理能力，
 * 不链接 MSVC 导入库，MinGW 工具链安全。DLL、模型或分词器缺失时 IsReady()
 * 返回 false，调用方应回退到阶段 1 关键词/标签检索，绝不调用远端服务。
 *
 * 分词采用内置 BERT WordPiece 实现（读取 tokenizer.json），行为与
 * HuggingFace tokenizers 对齐；句向量为 attention-masked mean pooling 后
 * 再做 L2 归一化。
 *
 * 线程约束：仅由 MemoryService 后台 worker 线程独占访问。
 */
class EmbeddingClient
    : public MemoryEmbedder
{
public:
    /**
     * @brief 构造函数
     * @param[in] config embedding 配置
     * @param[in] logCallback 诊断日志回调（可选，仅记录诊断类别，不含敏感内容）
     */
    explicit EmbeddingClient(const EmbeddingConfig &config,
                             std::function<void(const QString &)> logCallback = nullptr);

    /**
     * @brief 析构函数；释放 ONNX 会话、环境与动态库句柄
     */
    ~EmbeddingClient() override;

    EmbeddingClient(const EmbeddingClient &) = delete;
    EmbeddingClient &operator=(const EmbeddingClient &) = delete;

    /**
     * @brief 模型是否已就绪可推理
     * @return 就绪返回 true
     */
    bool IsReady() const override;

    /**
     * @brief 模型标识
     * @return 模型标识
     */
    QString ModelId() const override;

    /**
     * @brief 输出向量维度；未就绪时返回 0
     * @return 向量维度
     */
    int Dimension() const override;

    /**
     * @brief 对文档文本生成向量
     * @param[in] text 文档文本
     * @param[out] outVector 输出的 L2 归一化向量
     * @return 成功返回 true；未就绪、空文本或推理失败返回 false
     */
    bool EmbedDocument(const QString &text, QVector<float> &outVector) override;

    /**
     * @brief 对查询文本生成向量（自动附加指令前缀）
     * @param[in] text 查询文本
     * @param[out] outVector 输出的 L2 归一化向量
     * @return 成功返回 true；未就绪、空文本或推理失败返回 false
     */
    bool EmbedQuery(const QString &text, QVector<float> &outVector) override;

    /**
     * @brief 最近一次初始化/推理的诊断信息（不含敏感原文）
     * @return 诊断信息
     */
    QString LastError() const;

    /**
     * @brief 重新加载模型（此前失败后可重试，例如模型文件后到）
     * @return 加载成功返回 true
     */
    bool Reload();

    /**
     * @brief 均值池化 + L2 归一化（纯函数，便于单元测试）
     * @param[in] hiddenState last_hidden_state 数据（sequence * hiddenDim，行主序）
     * @param[in] attentionMask attention mask（0/1）
     * @param[in] sequenceLength 序列长度
     * @param[in] hiddenDim 隐藏维度
     * @param[out] outVector 输出的归一化向量
     * @return 成功返回 true；mask 全零或长度非法返回 false
     */
    static bool MeanPoolAndNormalize(const float *hiddenState,
                                     const int64_t *attentionMask,
                                     int sequenceLength,
                                     int hiddenDim,
                                     QVector<float> &outVector);

private:
    /**
     * @brief 动态加载 ONNX Runtime 并初始化 C++ API 表
     * @param[in] libraryDir 模型所在目录，用于优先查找配套运行时
     * @return 加载成功返回 true
     */
    bool LoadOnnxRuntime(const QString &libraryDir);

    /**
     * @brief 从 tokenizer.json 加载 BERT WordPiece 词表和特殊 token
     * @param[in] tokenizerPath tokenizer.json 完整路径
     * @return 加载成功返回 true
     */
    bool LoadTokenizer(const QString &tokenizerPath);

    /**
     * @brief 检查模型输入输出并解析张量名称和输出维度
     * @return 模型契约有效返回 true
     */
    bool ResolveTensors();

    /**
     * @brief 执行一次本地 ONNX 推理并生成归一化句向量
     * @param[in] text 待向量化文本
     * @param[in] isQuery 是否为查询文本
     * @param[out] outVector 输出的 L2 归一化向量
     * @return 成功返回 true
     */
    bool EmbedText(const QString &text, bool isQuery, QVector<float> &outVector);

    /**
     * @brief 将文本转为 BERT 输入张量
     * @param[in] text 待分词文本
     * @param[out] inputIds token ID 序列
     * @param[out] attentionMask attention mask 序列
     * @param[out] tokenTypeIds token type ID 序列
     * @return 分词成功返回 true
     */
    bool Tokenize(const QString &text,
                  QVector<int64_t> &inputIds,
                  QVector<int64_t> &attentionMask,
                  QVector<int64_t> &tokenTypeIds) const;

    /**
     * @brief 按标点切分单个预分词片段
     * @param[in] piece 预分词片段
     * @return 切分后的非空片段
     */
    QStringList SplitOnPunctuation(const QString &piece) const;

    /**
     * @brief 按 BERT WordPiece 词表编码 token
     * @param[in] token 待编码 token
     * @return token ID 序列；不存在可用切分时返回空
     */
    QVector<int64_t> WordPiece(const QString &token) const;

private:
    /**
     * @brief 内部实现（隔离 onnxruntime 头文件，避免污染公共头）
     */
    struct _tagImpl;

    std::unique_ptr<_tagImpl> m_impl; ///< 内部实现
};

} // namespace vpet

#endif // VPET_MEMORY_EMBEDDING_CLIENT_H
