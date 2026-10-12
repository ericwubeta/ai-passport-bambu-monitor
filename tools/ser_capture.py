# tools/ser_capture.py —— 纯读取串口日志(不做 DTR/RTS 复位,避免把设备锁进 ROM 下载模式)。
# 用法: python ser_capture.py COM5 40 out.log
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
out  = sys.argv[3] if len(sys.argv) > 3 else "ser.log"

s = serial.Serial(port, 115200, timeout=0.2)
end = time.time() + secs
buf = bytearray()
with open(out, "wb") as f:
    while time.time() < end:
        d = s.read(4096)
        if d:
            f.write(d)
            f.flush()
            buf += d
s.close()
txt = buf.decode("utf-8", "replace")
print(txt[-6000:])
print("\n[BYTES]", len(buf))
