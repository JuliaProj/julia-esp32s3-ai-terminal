# Julia Cloud Configuration Template

## LLM

```text
API_URL=https://api.example.com/v1/chat/completions
API_KEY=sk-xxxxxxxx
MODEL=qwen-turbo
STREAM=true
TIMEOUT_MS=5000
```

The endpoint must accept the OpenAI chat-completions request schema. SSE streams must end with `data: [DONE]`.

## TTS

```text
API_URL=https://api.example.com/v1/audio/speech
API_KEY=sk-xxxxxxxx
VOICE_ID=zh-CN-XiaoxiaoNeural
FORMAT=wav
SAMPLE_RATE=16000
CHANNELS=1
BITS=16
STREAM=true
```

WAV and raw signed little-endian PCM are preferred. Disable `CONFIG_JULIA_LLM_MOCK_MODE` after both cloud endpoints are configured.

## ASR

```text
PROVIDER=Aliyun/Baidu/iFlytek/custom
APP_ID=xxxxxxxx
API_KEY=xxxxxxxx
API_SECRET=xxxxxxxx
SAMPLE_RATE=16000
CHANNELS=1
```

Disable `CONFIG_JULIA_MOCK_ASR_ONLY` when the microphone frontend and credentials are ready.

Secrets must be provisioned with flash encryption enabled. Do not commit production keys to `sdkconfig`.
