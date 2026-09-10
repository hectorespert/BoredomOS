import sys, time, collections
from pymavlink import mavutil
dev, baud, secs = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
m = mavutil.mavlink_connection(dev, baud=baud)
counts = collections.Counter()
ident = set()
t0 = time.time()
while time.time() - t0 < secs:
    msg = m.recv_match(blocking=True, timeout=1.0)
    if msg is None:
        continue
    t = msg.get_type()
    counts[t] += 1
    if t != 'BAD_DATA':
        ident.add((msg.get_srcSystem(), msg.get_srcComponent()))
    if t == 'HEARTBEAT':
        counts['_type%d' % msg.type] += 1
el = time.time() - t0
print("listened %.1fs on %s @ %d" % (el, dev, baud))
for k, v in sorted(counts.items()):
    print("  %-22s %4d   %.2f Hz" % (k, v, v/el))
print("identity (sysid, compid):", sorted(ident) or "none")
