"""Minimal Kimi API helper. Reads credentials only from the process environment."""

import json
import os
import sys
import urllib.request
import urllib.error


BASE_URL = os.environ.get("KIMI_BASE_URL", "https://api.moonshot.cn/v1").rstrip("/")


def request(path: str, payload=None):
    key = os.environ.get("MOONSHOT_API_KEY")
    if not key:
        raise RuntimeError("MOONSHOT_API_KEY is not set")
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        BASE_URL + path,
        data=data,
        headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"},
        method="GET" if data is None else "POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=300) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Kimi API HTTP {error.code}: {detail}") from None


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "models":
        result = request("/models")
        print("\n".join(item["id"] for item in result.get("data", [])))
    elif len(sys.argv) == 2 and sys.argv[1] == "review-avatar":
        files = [
            "main/ui/julia_ui.c",
            "main/ui/avatar_micro_motion.c",
            "main/ui/generated/julia_rig_assets.c",
            "main/lvgl_port/lvgl_port.c",
        ]
        source = []
        for path in files:
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                source.append(f"\n===== {path} =====\n{handle.read()}")
        prompt = """你是 ESP32-S3、ESP-IDF 5.5、LVGL 8.4 显示系统专家。
审查下面的数字人分层实现。屏幕 360x360、RGB565、LV_COLOR_16_SWAP=1。
人物由 body/hair_back/face/hair_front/eyes/pupils/mouth 九个 RGBA 图层组成，
离线 composite_preview 完全正确，但设备曾出现花屏，修正 PNG 格式后又出现脸与头发错位、眼睛缺失。
现在资源已预转换为每像素 [RGB565高字节, RGB565低字节, Alpha]，描述符为
LV_IMG_CF_TRUE_COLOR_ALPHA。请找出当前代码仍会导致错位、缺层、抖动或刷新冲突的具体问题。
请按严重度给出：文件/函数、根因、可直接编译的最小修改。不要建议新增图片资源，
不要泛泛解释，也不要假设日志正常就代表视觉正常。重点检查 LVGL 图像像素字节序、
父对象内容坐标、Z顺序、对象尺寸变换是否会裁剪图片、直接 LCD 绘制与 LVGL 刷新的所有权。"""
        result = request("/chat/completions", {
            "model": "kimi-for-coding-highspeed",
            "max_tokens": 8000,
            "messages": [{"role": "user", "content": prompt + "".join(source)}],
        })
        choice = result["choices"][0]
        message = choice.get("message", {})
        output = message.get("content") or message.get("reasoning_content")
        if output:
            print(output)
        else:
            print(json.dumps({"finish_reason": choice.get("finish_reason"),
                              "message_fields": sorted(message.keys())}, ensure_ascii=False))
    elif len(sys.argv) == 2 and sys.argv[1] == "review-rig-tree":
        with open("main/ui/generated/julia_rig_assets.c", "r", encoding="utf-8") as handle:
            assets = handle.read()
        with open("main/ui/julia_ui.c", "r", encoding="utf-8", errors="replace") as handle:
            ui = handle.readlines()
        excerpt = "".join(ui[55:95] + ui[485:560])
        prompt = f"""你是 LVGL 8.4/ESP32-S3 专家。只输出最终结论，不展示推理，最多 8 条。
屏幕 360x360，LV_COLOR_DEPTH=16，LV_COLOR_16_SWAP=1。离线按相同坐标合成正常；
设备显示脸与头发错位且眼睛缺失。检查以下资源描述符和对象树，指出确定性 bug，
给出最小 C 修改。不要讨论无关网络语音，不新增图片。原生像素是 RGB565高字节、
低字节、Alpha，每像素3字节。\n===== assets =====\n{assets}\n===== object tree =====\n{excerpt}"""
        result = request("/chat/completions", {
            "model": "kimi-for-coding-highspeed",
            "max_tokens": 12000,
            "messages": [{"role": "user", "content": prompt}],
        })
        message = result["choices"][0].get("message", {})
        print(message.get("content") or message.get("reasoning_content") or "empty response")
    else:
        raise SystemExit("usage: kimi_api.py models | review-avatar | review-rig-tree")
