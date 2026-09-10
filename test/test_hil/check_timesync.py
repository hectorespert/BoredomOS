import time
from pymavlink import mavutil
m = mavutil.mavlink_connection('/dev/ttyACM0', baud=115200, source_system=255, source_component=190)
m.recv_match(type='HEARTBEAT', blocking=True, timeout=5)
ts1 = int(time.time() * 1e9)
m.mav.timesync_send(0, ts1)
print("-> TIMESYNC(tc1=0, ts1=%d) enviado" % ts1)
t0 = time.time(); reply = None
while time.time() - t0 < 5:
    msg = m.recv_match(type='TIMESYNC', blocking=True, timeout=1)
    if msg and msg.tc1 != 0:
        reply = msg; break
if reply:
    print("respuesta: tc1=%d" % reply.tc1)
    print("  ts1 devuelto intacto:", reply.ts1 == ts1)
    print("  tc1 como fecha:", time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(reply.tc1/1e9)))
    print("  parte sub-segundo de tc1:", reply.tc1 % 1_000_000_000, "(0 = resolucion de 1 s, backlog conocido)")
else:
    print("respuesta: NINGUNA")
