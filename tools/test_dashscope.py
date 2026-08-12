import io
import base64
import json
import re
import wave
from pathlib import Path

import requests


config = (Path(__file__).parents[1] / "sdkconfig").read_text(encoding="utf-8")
match = re.search(r'^CONFIG_JULIA_AI_API_KEY="([^"]+)"', config, re.MULTILINE)
if not match:
    raise SystemExit("DashScope API key is not configured")

headers = {"Authorization": "Bearer " + match.group(1)}
models = requests.get(
    "https://dashscope.aliyuncs.com/compatible-mode/v1/models",
    headers=headers,
    timeout=30,
)
if models.ok:
    model_ids = [item.get("id", "") for item in models.json().get("data", [])]
    audio_models = [model for model in model_ids if any(
        token in model.lower() for token in ("audio", "tts", "cosy", "paraformer")
    )]
    print("MODELS", models.status_code, audio_models[:30])
else:
    print("MODELS", models.status_code, models.text[:240])

chat = requests.post(
    "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions",
    headers={**headers, "Content-Type": "application/json"},
    json={"model": "qwen-plus", "messages": [{"role": "user", "content": "只回答：连接成功"}]},
    timeout=30,
)
print("CHAT", chat.status_code, chat.text[:240])
wav_data = io.BytesIO()
with wave.open(wav_data, "wb") as wav_file:
    wav_file.setparams((1, 2, 16000, 0, "NONE", ""))
    wav_file.writeframes(bytes(32000))

native_url = "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"
audio_uri = "data:audio/wav;base64," + base64.b64encode(wav_data.getvalue()).decode("ascii")
asr = requests.post(
    native_url,
    headers={**headers, "Content-Type": "application/json"},
    data=json.dumps({
        "model": "qwen-audio-turbo-latest",
        "input": {"messages": [{"role": "user", "content": [
            {"audio": audio_uri}, {"text": "请只输出这段录音中的中文内容，不要解释。"}
        ]}]},
        "parameters": {"result_format": "message"},
    }, ensure_ascii=False).encode("utf-8"),
    timeout=30,
)
print("ASR", asr.status_code, asr.headers.get("content-type"), asr.text[:240])

tts = requests.post(
    native_url,
    headers={**headers, "Content-Type": "application/json"},
    data=json.dumps({
        "model": "qwen3-tts-flash",
        "input": {"text": "你好，我是Julia。", "voice": "Cherry", "language_type": "Chinese"},
    }, ensure_ascii=False).encode("utf-8"),
    timeout=30,
)
if tts.ok:
    audio_url = tts.json().get("output", {}).get("audio", {}).get("url")
    audio = requests.get(audio_url, timeout=30) if audio_url else None
    if audio is not None:
        print("TTS", tts.status_code, "download", audio.status_code,
              audio.headers.get("content-type"), len(audio.content), audio.content[:12])
    else:
        print("TTS", tts.status_code, "missing audio URL", tts.text[:240])
else:
    print("TTS", tts.status_code, tts.headers.get("content-type"), tts.text[:240])
