---

## 一、标准多模态格式（OpenAI 定义）

图片通过 `content` 数组传入，支持 **URL** 和 **Base64** 两种：

```json
{
  "model": "gpt-4o",
  "messages": [
    {
      "role": "user",
      "content": [
        {
          "type": "text",
          "text": "描述这张图片"
        },
        {
          "type": "image_url",
          "image_url": {
            "url": "https://example.com/image.jpg",
            "detail": "auto"
          }
        }
      ]
    }
  ]
}
```

### Base64 格式（本地图片）

```json
{
  "type": "image_url",
  "image_url": {
    "url": "data:image/jpeg;base64,/9j/4AAQSkZJRgABAQ...",
    "detail": "auto"
  }
}
```

**`detail` 参数：**
- `low`：低分辨率（512x512），省 token
- `high`：原图分块处理，细节保留
- `auto`：根据图片尺寸自动选择（默认）

---

## 二、各服务商兼容情况

| 服务商 | 是否兼容 | 支持方式 | 差异/限制 |
|--------|---------|---------|----------|
| **OpenAI** | ✅ 原生 | URL + Base64 | 标准定义者 |
| **Anthropic** | ✅ 兼容端点 | Base64 为主 | 原生格式不同，兼容端点可用 OpenAI 格式 |
| **Google Gemini** | ✅ 兼容端点 | URL + Base64 | 原生格式差异大，兼容端点支持 |
| **DeepSeek** | ✅ | Base64 | 仅支持 `data:image/jpeg;base64,...` 格式 |
| **Moonshot (Kimi)** | ✅ | URL + Base64 | 支持较好，与 OpenAI 基本一致 |
| **通义千问** | ✅ | URL + Base64 | 支持，但单图大小有限制（通常 10MB） |
| **字节豆包** | ✅ | URL + Base64 | 支持 |
| **Ollama** | ✅ | Base64 | 本地运行，需用多模态模型（如 llava） |
| **vLLM / llama.cpp** | ✅ | Base64 | 取决于部署的模型是否具备视觉能力 |

**结论**：所有主流服务商的兼容端点都支持上述 `image_url` 格式，但细节上有限制。

---

## 三、通用多模态客户端代码

```python
import base64
import httpx
from pathlib import Path


async def encode_image(image_path: str) -> str:
    """本地图片转 Base64 Data URL"""
    suffix = Path(image_path).suffix.lower().replace(".", "")
    if suffix == "jpg":
        suffix = "jpeg"
    
    with open(image_path, "rb") as f:
        encoded = base64.b64encode(f.read()).decode("utf-8")
    
    return f"data:image/{suffix};base64,{encoded}"


async def chat_with_vision(
    api_key: str,
    base_url: str = "https://api.openai.com/v1",
    model: str = "gpt-4o",
    text: str = "",
    image_url: str = None,      # 网络图片 URL
    image_path: str = None,     # 本地图片路径
    detail: str = "auto",       # low / high / auto
    temperature: float = 0.7
):
    """
    通用多模态调用
    image_url 和 image_path 二选一
    """
    content = [{"type": "text", "text": text}]
    
    if image_path:
        # 本地图片 → Base64
        url = await encode_image(image_path)
    elif image_url:
        # 网络图片 → 直接 URL
        url = image_url
    else:
        raise ValueError("需提供 image_url 或 image_path")
    
    content.append({
        "type": "image_url",
        "image_url": {
            "url": url,
            "detail": detail
        }
    })
    
    payload = {
        "model": model,
        "messages": [{"role": "user", "content": content}],
        "temperature": temperature
    }
    
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json"
    }
    
    async with httpx.AsyncClient() as client:
        resp = await client.post(
            f"{base_url}/chat/completions",
            headers=headers,
            json=payload
        )
        resp.raise_for_status()
        return resp.json()


# ========== 使用示例 ==========

import asyncio

async def demo():
    # 示例1：OpenAI + 本地图片
    result = await chat_with_vision(
        api_key="sk-xxx",
        model="gpt-4o",
        text="描述这张图片的内容",
        image_path="/path/to/photo.jpg",
        detail="high"
    )
    print(result["choices"][0]["message"]["content"])
    
    # 示例2：DeepSeek + 网络图片
    result2 = await chat_with_vision(
        api_key="sk-xxx",
        base_url="https://api.deepseek.com/v1",
        model="deepseek-chat",  # 注意：必须是支持视觉的模型
        text="提取图中的文字",
        image_url="https://example.com/chart.png"
    )

asyncio.run(demo())
```

---

## 四、多模态的兼容性差异（坑点）

### 1. 模型必须支持视觉
不是所有模型都能接图片。错误示例：
```json
{ "model": "gpt-3.5-turbo" }  // ❌ 不支持图片
{ "model": "gpt-4o" }          // ✅ 支持
{ "model": "deepseek-chat" }   // ✅ 支持（需确认版本）
{ "model": "llava" }           // ✅ 本地视觉模型
```

### 2. 图片格式限制

| 服务商 | 推荐格式 | 限制 |
|--------|---------|------|
| OpenAI | PNG, JPEG | 单图 20MB |
| DeepSeek | JPEG, PNG | 仅支持 Base64，URL 方式可能不支持 |
| Kimi | JPEG, PNG, WEBP, GIF | 单图 100MB（较宽松） |
| 通义千问 | JPEG, PNG | 单图 10MB |

### 3. `detail` 参数支持不一
- **OpenAI / Kimi**：完整支持 `low/high/auto`
- **部分服务商**：忽略 `detail` 字段，统一按原图处理
- **建议**：带上 `detail` 参数，不支持的会自动忽略，不影响调用

### 4. 多图输入
OpenAI 格式支持一次传多张图：

```json
"content": [
  { "type": "text", "text": "比较这两张图" },
  { "type": "image_url", "image_url": { "url": "data:image/jpeg;base64,..." } },
  { "type": "image_url", "image_url": { "url": "data:image/jpeg;base64,..." } }
]
```

**通用性**：Kimi、OpenAI 支持多图；部分服务商只支持单图，需分批调用。

---

## 五、在节点化系统中的配置

```json
{
  "node_id": "vision_understand",
  "type": "llm",
  "provider": "openai",
  "model": "gpt-4o",
  "inputs": {
    "messages": [
      {
        "role": "user",
        "content": [
          { "type": "text", "text": "{{user.query}}" },
          { "type": "image_url", "image_url": { "url": "{{upload.image_base64}}", "detail": "auto" } }
        ]
      }
    ]
  }
}
```

---

## 六、视频 / 音频呢？

| 模态 | 通用性 | 说明 |
|------|--------|------|
| **图片** | ✅ 高（85%） | 上述格式基本通行 |
| **音频** | ⚠️ 低 | OpenAI 有 `input_audio`，但多数服务商未兼容，各自为政 |
| **视频** | ❌ 很低 | 几乎无统一标准，通常需先抽帧为图片再输入 |

---

## 总结

> **图片输入在 OpenAI 兼容格式中是高度通用的**，所有主流服务商都支持 `type: "image_url"` 的 Base64/URL 格式。  
> 只需注意：**选对多模态模型、控制图片大小、多图支持需测试**。  
> 视频和音频目前尚无统一标准，不建议纳入通用节点。