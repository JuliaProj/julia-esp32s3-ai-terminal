import re
import sys
import time

import serial


duration = int(sys.argv[1]) if len(sys.argv) > 1 else 20
pattern = re.compile(
    r"VOICE|ASR|TTS|Speech|Capture|No speech|User:|Julia:|failure|Playing|session|Ready"
)
port = serial.Serial("COM5", 115200, timeout=0.2)
end = time.time() + duration
data = bytearray()
while time.time() < end:
    data.extend(port.read(4096))
port.close()
text = data.decode("utf-8", errors="replace")
print("\n".join(line for line in text.splitlines() if pattern.search(line)))
