以下是 OpenAI Chat API 的完整调用方式，包含官方 SDK 和通用 HTTP 两种写法，可直接嵌入你的节点化引擎。

---

## 一、基础端点

```
POST https://api.openai.com/v1/chat/completions
```

**请求头：**
```http
Authorization: Bearer <YOUR_API_KEY>
Content-Type: application/json
```

---

## 二、请求体结构

```json
{
  "model": "gpt-4o",
  "messages": [
    { "role": "system", "content": "你是一个有帮助的助手。" },
    { "role": "user", "content": "你好，请介绍一下自己。" }
  ],
  "temperature": 0.7,
  "max_tokens": 2048,
  "top_p": 1.0,
  "frequency_penalty": 0.0,
  "presence_penalty": 0.0,
  "stream": false
}
```

### 核心字段说明

| 字段 | 类型 | 说明 |
|------|------|------|
| `model` | string | 模型 ID，如 `gpt-4o`、`gpt-4o-mini`、`gpt-3.5-turbo` |
| `messages` | array | 对话历史，每条含 `role` 和 `content` |
| `temperature` | float | 采样温度，0~2，越低越确定，越高越随机 |
| `max_tokens` | integer | 最大生成 token 数（含输出） |
| `top_p` | float | 核采样，0~1，与 temperature 二选一调 |
| `frequency_penalty` | float | -2~2，惩罚重复词，越高越不重复 |
| `presence_penalty` | float | -2~2，惩罚已出现主题，越高越易引入新话题 |
| `stream` | boolean | 是否流式返回 SSE |

### Messages 角色

| role | 用途 |
|------|------|
| `system` | 系统指令，设定助手行为（非必须，但建议放首条） |
| `user` | 用户输入 |
| `assistant` | 模型之前的回复，用于多轮对话 |
| `tool` | 工具返回结果（Function Calling 时使用） |

---

## 三、响应结构（非流式）

```json
{
  "id": "chatcmpl-xxx",
  "object": "chat.completion",
  "created": 1677652288,
  "model": "gpt-4o",
  "choices": [
    {
      "index": 0,
      "message": {
        "role": "assistant",
        "content": "你好！我是由 OpenAI 训练的大型语言模型..."
      },
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 25,
    "completion_tokens": 50,
    "total_tokens": 75
  }
}
```

**提取回复：**
```python
reply = response["choices"][0]["message"]["content"]
```

---

## 四、调用示例

### 1. 官方 Python SDK（推荐）

```python
from openai import OpenAI

client = OpenAI(api_key="YOUR_API_KEY")

# 非流式调用
response = client.chat.completions.create(
    model="gpt-4o",
    messages=[
        {"role": "system", "content": "你是一个简洁的助手。"},
        {"role": "user", "content": "什么是节点化工作流？"}
    ],
    temperature=0.7,
    max_tokens=1024
)

print(response.choices[0].message.content)
```

### 2. 流式调用（SSE）

```python
stream = client.chat.completions.create(
    model="gpt-4o",
    messages=[{"role": "user", "content": "写一首短诗"}],
    stream=True  # 开启流式
)

for chunk in stream:
    if chunk.choices[0].delta.content is not None:
        print(chunk.choices[0].delta.content, end="")
```

### 3. 通用 HTTP 客户端（兼容多服务商）

```python
import httpx
import json

async def chat_with_openai(
    api_key: str,
    base_url: str = "https://api.openai.com/v1",
    model: str = "gpt-4o",
    messages: list = None,
    temperature: float = 0.7,
    max_tokens: int = 2048,
    stream: bool = False
):
    """通用 HTTP 调用，兼容所有 OpenAI 格式服务商"""
    
    url = f"{base_url}/chat/completions"
    
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json"
    }
    
    payload = {
        "model": model,
        "messages": messages or [],
        "temperature": temperature,
        "max_tokens": max_tokens,
        "stream": stream
    }
    
    async with httpx.AsyncClient() as client:
        if not stream:
            resp = await client.post(url, headers=headers, json=payload)
            resp.raise_for_status()
            return resp.json()
        
        # 流式处理
        async with client.stream("POST", url, headers=headers, json=payload) as resp:
            resp.raise_for_status()
            async for line in resp.aiter_lines():
                if line.startswith("data: "):
                    data = line[6:]
                    if data == "[DONE]":
                        break
                    chunk = json.loads(data)
                    content = chunk["choices"][0]["delta"].get("content", "")
                    if content:
                        yield content

# 使用
import asyncio

async def main():
    result = await chat_with_openai(
        api_key="sk-xxx",
        model="gpt-4o",
        messages=[{"role": "user", "content": "你好"}]
    )
    print(result["choices"][0]["message"]["content"])

asyncio.run(main())
```

### 4. 多轮对话示例

```python
messages = [
    {"role": "system", "content": "你是一个代码助手。"},
    {"role": "user", "content": "用 Python 写个快速排序"},
    {"role": "assistant", "content": "def quicksort(arr): ..."},  # 上一轮回复
    {"role": "user", "content": "能改成降序吗？"}  # 继续追问
]

response = client.chat.completions.create(
    model="gpt-4o",
    messages=messages
)
```

---

## 五、错误处理

| HTTP 状态码 | 含义 | 处理 |
|------------|------|------|
| 400 | 请求格式错误 | 检查 messages 结构、参数范围 |
| 401 | 认证失败 | API Key 无效或过期 |
| 429 | 速率限制 | 降低请求频率，加指数退避重试 |
| 500/503 | 服务端错误 | 重试，或降级到备用模型 |

```python
import httpx

try:
    response = await client.post(...)
    response.raise_for_status()
except httpx.HTTPStatusError as e:
    if e.response.status_code == 429:
        # 指数退避重试
        pass
    elif e.response.status_code == 401:
        raise ValueError("API Key 无效")
    else:
        raise
```

---

## 六、嵌入节点化系统的建议

在你的 JSON 配置中，LLM 节点只需声明：

```json
{
  "node_id": "chat",
  "type": "llm",
  "provider": "openai",
  "model": "gpt-4o",
  "temperature": 0.7,
  "max_tokens": 2048,
  "stream": false,
  "inputs": {
    "messages": "{{upstream.prompt}}"
  }
}
```

执行引擎统一调用上述 `chat_with_openai()`，将 `provider` 映射到对应的 `base_url` 和 `api_key` 即可。

---