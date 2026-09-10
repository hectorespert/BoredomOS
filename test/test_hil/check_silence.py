import serial, time, sys
p = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
p.dtr = True
p.rts = True
time.sleep(0.5)
p.reset_input_buffer()
t0 = time.time(); data = b''
while time.time() - t0 < 8:
    data += p.read(256)
print("bytes recibidos:", len(data))
if data:
    print("primeros 64 en hex:", data[:64].hex(' '))
    print("magic 0xFD:", data.count(b'\xfd'), " 0xFE:", data.count(b'\xfe'))
    printable = sum(1 for b in data if 32 <= b < 127 or b in (10,13,9))
    print("bytes imprimibles: %d/%d" % (printable, len(data)))
    print("como texto:", repr(data[:200]))
