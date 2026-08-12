# ASR configuration template

Do not commit production secrets. Configure one provider at deployment time.

## Current firmware provider: Qwen/DashScope

```text
API_KEY: <provided out of band>
ASR_ENDPOINT: https://dashscope.aliyuncs.com/api/v1/services/audio/asr/transcription
SAMPLE_RATE: 16000
FORMAT: signed PCM LE16 mono
```

## Xunfei

```text
APPID: xxx
API_KEY: xxx
API_SECRET: xxx
ENDPOINT: wss://iat-api.xfyun.cn/v2/iat
```

## Alibaba NLS

```text
APP_KEY: xxx
ACCESS_KEY_ID: xxx
ACCESS_KEY_SECRET: xxx
ENDPOINT: wss://nls-gateway.cn-shanghai.aliyuncs.com/ws/v1
```

## Baidu

```text
APP_ID: xxx
API_KEY: xxx
SECRET_KEY: xxx
ENDPOINT: http://vop.baidu.com/server_api
```

## Local model

```text
MODEL_PATH: /sdcard/model/sensevoice_tiny.bin
DICT_PATH: /sdcard/model/dict.txt
```

The shipped WakeNet model is `wn9_nihaoxiaozhi_tts`, whose trained phrase is
`你好小智`. Changing the display name does not retrain a wake-word model. A
custom `你好 Julia` model must be exported and included in the ESP-SR model
partition before selecting it in `wake_word_config.h`.

The microphone and speaker use separate I2S controllers. The current AFE has
one microphone channel and no speaker reference channel, so AEC is disabled.
NS/AGC runtime commands report `ESP_ERR_NOT_SUPPORTED` until a compatible AFE
pipeline and reference feed are added.
