import argparse
import time

import serial


def collect(port, seconds, output):
    deadline = time.time() + seconds
    while time.time() < deadline:
        output.extend(port.read(4096))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--cycles", type=int, default=10)
    parser.add_argument("--off-seconds", type=float, default=10.0)
    parser.add_argument("--timeout-seconds", type=int, default=3)
    parser.add_argument("--clip-states", action="store_true")
    parser.add_argument("--remove-thinking", action="store_true")
    parser.add_argument("--brightness-chain", action="store_true")
    parser.add_argument("--video-demo", action="store_true")
    parser.add_argument("--remove-placeholders", action="store_true")
    parser.add_argument("--motion-log", action="store_true")
    args = parser.parse_args()

    output = bytearray()
    with serial.Serial(args.port, 115200, timeout=0.05) as port:
        port.dtr = False
        port.rts = False
        collect(port, 8, output)
        if args.motion_log:
            for command in ("phase 0\n", "phase 1\n", "phase 2\n", "phase 3\n", "state 0\n"):
                port.write(command.encode("ascii"))
                collect(port, 3.2, output)
            text = output.decode("utf-8", "replace")
            keys = ("anim tick", "Dialog phase", "State:", "Panic", "Guru")
            print("\n".join(line for line in text.splitlines() if any(key in line for key in keys)))
            return
        if args.remove_placeholders:
            for name in ("idle_loop", "listening_loop", "thinking_loop",
                         "speaking_base_loop", "sleep_loop"):
                port.write(f"clip-remove {name}\n".encode("ascii"))
                collect(port, 0.5, output)
            text = output.decode("utf-8", "replace")
            print("\n".join(line for line in text.splitlines() if "CLIP_REMOVE" in line))
            return
        if args.video_demo:
            for _ in range(3):
                for command in ("state 13\n", "state 6\n", "state 3\n"):
                    port.write(command.encode("ascii"))
                    collect(port, 5, output)
                port.write(b"screen-timeout 1\n")
                collect(port, 6, output)
                port.write(b"state 14\n")
                collect(port, 1, output)
            text = output.decode("utf-8", "replace")
            print("VIDEO_DEMO cycles=3 off=%d wake=%d wdt=%d panic=%d" % (
                text.count("display off panel"), text.count("wake first flush"),
                text.count("WDT"), text.count("Panic") + text.count("Guru")))
            return
        if args.brightness_chain:
            for command in ("state 13\n", "state 6\n", "state 3\n"):
                port.write(command.encode("ascii"))
                collect(port, 1.2, output)
            port.write(b"screen-timeout 3\n")
            collect(port, 5, output)
            text = output.decode("utf-8", "replace")
            print("\n".join(line for line in text.splitlines()
                            if "backlight fade" in line or "display off panel" in line))
            return
        if args.remove_thinking:
            port.write(b"clip-remove thinking_loop\n")
            collect(port, 1, output)
            port.write(b"phase 2\n")
            collect(port, 10, output)
            text = output.decode("utf-8", "replace")
            keys = ("CLIP_REMOVE", "thinking_loop", "fallback to Flash", "Panic", "Guru")
            print("\n".join(line for line in text.splitlines()
                            if any(key in line for key in keys)))
            return
        if args.clip_states:
            for command in ("phase 0\n", "phase 1\n", "phase 2\n", "phase 3\n", "state 0\n"):
                port.write(command.encode("ascii"))
                collect(port, 4, output)
            text = output.decode("utf-8", "replace")
            keys = ("loaded clip=", "switch complete clip=", "fallback to Flash",
                    "load failed", "Panic", "Guru")
            print("\n".join(line for line in text.splitlines()
                            if any(key in line for key in keys)))
            return
        port.write(f"screen-timeout {args.timeout_seconds}\n".encode("ascii"))
        for index in range(args.cycles):
            collect(port, args.timeout_seconds + 0.8 + args.off_seconds, output)
            # 交替使用两个对话子状态，避免状态队列合法合并连续重复请求。
            port.write(("state 13\n" if index % 2 == 0 else "state 14\n").encode("ascii"))
            collect(port, 1.0, output)
        collect(port, 2, output)

    text = output.decode("utf-8", "replace")
    keys = ("display off panel", "display off health", "backlight fade",
            "wake first flush", "display wake requested",
            "WDT", "Panic", "Guru", "rst:")
    print("\n".join(line for line in text.splitlines() if any(key in line for key in keys)))
    print("SUMMARY off=%d wake=%d wdt=%d panic=%d" % (
        text.count("display off panel"), text.count("wake first flush"),
        text.count("WDT"), text.count("Panic") + text.count("Guru")))


if __name__ == "__main__":
    main()
