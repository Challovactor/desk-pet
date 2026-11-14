import sys, time
sys.path.insert(0, r'C:\Users\LiuLinjun\AppData\Local\Programs\MOCEWorkbench-data\tools\pyserial-universal')
import serial

PORT = 'COM7'
BAUD = 115200
SYNC = b'cnys'

def attempt(dtr, rts_idle, rts_pulse, attempts=400):
    s = serial.Serial()
    s.port = PORT
    s.baudrate = BAUD
    s.timeout = 0.002
    s.setDTR(dtr)
    s.setRTS(rts_idle)
    try:
        s.open()
        # mimic tool reset(): assert pulse level, brief wait, return to idle
        s.setRTS(rts_pulse)
        time.sleep(0.02)
        s.setRTS(rts_idle)
        time.sleep(0.02)
        s.reset_input_buffer()
        for i in range(attempts):
            s.write(SYNC)
            b = s.read(1)
            if b:
                tail = s.read(32)
                return b + tail
        return b''
    finally:
        try: s.close()
        except Exception: pass

for dtr in (0, 1):
    for rts_idle, rts_pulse in ((0, 1), (1, 0)):
        data = attempt(dtr, rts_idle, rts_pulse)
        print(f'DTR={dtr} RTS pulse {rts_pulse}->{rts_idle}: {data.hex() if data else "no response"}')
