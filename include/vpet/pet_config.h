#ifndef VPET_PET_CONFIG_H
#define VPET_PET_CONFIG_H

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace vpet
{

constexpr float PET_VOLUME_MIN = 0.0f;           ///< 语音播放音量下限（静音）
constexpr float PET_VOLUME_MAX = 1.0f;           ///< 语音播放音量上限
constexpr float PET_VOLUME_DEFAULT = 1.0f;       ///< 语音播放音量默认值
constexpr float PET_DISPLAY_SCALE_MIN = 0.5f;    ///< 桌宠显示缩放下限
constexpr float PET_DISPLAY_SCALE_MAX = 2.0f;    ///< 桌宠显示缩放上限
constexpr float PET_DISPLAY_SCALE_DEFAULT = 1.0f; ///< 桌宠显示缩放默认值

/// pet_config.json 文件名
inline const QString PET_CONFIG_FILE_NAME = QStringLiteral("pet_config.json");

/**
 * @brief 桌宠本地配置（pet_config.json）
 *
 * 记录语音播放音量与显示缩放；越界值在读取时钳制到合法范围，不视为错误。
 */
struct PetConfig
{
    float volume = PET_VOLUME_DEFAULT;       ///< 语音播放音量，0.0（静音）~ 1.0
    float displayScale = PET_DISPLAY_SCALE_DEFAULT; ///< 桌宠显示缩放，0.5 ~ 2.0

    /**
     * @brief 将字段钳制到合法范围
     */
    void Clamp();

    bool operator==(const PetConfig &other) const;
    bool operator!=(const PetConfig &other) const;
};

/**
 * @brief 从 JSON 对象解析配置并钳制越界值
 *
 * 字段缺失时使用默认值；字段存在但类型不合法时 Qt 会返回默认值。
 *
 * @param[in] object JSON 对象
 * @return 解析后的配置
 */
PetConfig PetConfigFromJson(const QJsonObject &object);

/**
 * @brief 将配置序列化为 JSON 对象
 * @param[in] config 配置
 * @return JSON 对象
 */
QJsonObject PetConfigToJson(const PetConfig &config);

/**
 * @brief 从指定 JSON 文件加载配置
 *
 * 文件不存在、无法读取或 JSON 非法时返回 false，config 保持调用方传入值不变。
 *
 * @param[in] configPath 配置文件路径
 * @param[out] config 解析结果
 * @param[out] errorMessage 失败原因；成功时为空
 * @return 加载成功返回 true
 */
bool LoadPetConfig(const QString &configPath, PetConfig &config, QString &errorMessage);

/**
 * @brief 原子保存配置到指定文件
 *
 * 先将旧文件备份为 `<路径>.bak`（存在时），再通过 QSaveFile 写临时文件后原子替换。
 *
 * @param[in] configPath 目标文件路径
 * @param[in] config 待保存配置
 * @param[out] errorMessage 失败原因；成功时为空
 * @return 保存成功返回 true
 */
bool SavePetConfig(const QString &configPath, const PetConfig &config, QString &errorMessage);

/**
 * @brief 按项目配置查找约定搜索 pet_config.json
 *
 * 候选顺序：可执行文件目录 → 当前工作目录 → 上级目录 → 上两级目录。
 *
 * @return 找到的配置文件绝对路径；未找到返回空字符串
 */
QString FindPetConfigPath();

} // namespace vpet

#endif // VPET_PET_CONFIG_H
