import sys, time
sys.path.insert(0, r'C:\Users\LiuLinjun\AppData\Local\Programs\MOCEWorkbench-data\tools\pyserial-universal')
import serial

s = serial.Serial()
s.port = 'COM7'
s.baudrate = 115200
s.timeout = 0.1
s.setDTR(0)
s.setRTS(0)
s.open()
time.sleep(0.1)
s.setRTS(1)
time.sleep(0.05)
s.setRTS(0)
s.reset_input_buffer()
buf = b''
end = time.time() + 3.0
while time.time() < end:
    buf += s.read(4096)
s.close()
text = buf.decode('utf-8', 'replace')
lines = [ln for ln in text.splitlines() if ln.strip()]
# Print the first 40 non-empty lines, then any [ecw] init line.
for ln in lines[:40]:
    print(ln)
print('--- init lines ---')
for ln in lines:
    if '[ecw] init' in ln or 'state:' in ln or 'mic:' in ln or 'tts:' in ln:
        print(ln)
