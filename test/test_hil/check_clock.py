import time
from pymavlink import mavutil
m = mavutil.mavlink_connection('/dev/ttyACM0', baud=115200, source_system=255, source_component=190)
def read_time(label, secs=3):
    t0=time.time(); got=None
    while time.time()-t0 < secs:
        msg = m.recv_match(type='SYSTEM_TIME', blocking=True, timeout=1)
        if msg: got = msg.time_unix_usec // 1_000_000
    print("%-22s sat=%s  (%s)" % (label, got, time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(got)) if got else '-'))
    return got

before = read_time("antes")
host = int(time.time())
print("host                   = %d  (%s)" % (host, time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(host))))
print("delta antes            = %+d s" % ((before - host) if before else 0))

m.mav.system_time_send(host * 1_000_000, 0)
print("-> SYSTEM_TIME enviado")
time.sleep(2.5)
after = read_time("despues de fijar")
print("delta despues          = %+d s" % ((after - int(time.time())) if after else 0))

