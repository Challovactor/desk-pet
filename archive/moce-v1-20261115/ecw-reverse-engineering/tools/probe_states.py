import sys, time
sys.path.insert(0, r'C:\Users\LiuLinjun\AppData\Local\Programs\MOCEWorkbench-data\tools\pyserial-universal')
import serial

PORT = 'COM7'
BAUD = 115200
SYNC = b'cnys'

for dtr in (0, 1):
    for rts in (0, 1):
        s = serial.Serial()
        s.port = PORT
        s.baudrate = BAUD
        s.timeout = 0.002
        s.setDTR(dtr)
        s.setRTS(rts)
        try:
            s.open()
            time.sleep(0.3)
            s.reset_input_buffer()
            logs = s.read(256)
            resp = b''
            for _ in range(600):
                s.write(SYNC)
                b = s.read(1)
                if b:
                    resp = b + s.read(32)
                    break
            print(f'DTR={dtr} RTS={rts} logs={logs.hex()} resp={resp.hex() if resp else "none"}')
        except Exception as e:
            print(f'DTR={dtr} RTS={rts} ERROR {e!r}')
        finally:
            try: s.close()
            except Exception: pass
