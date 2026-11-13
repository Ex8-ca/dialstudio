"""Read-only spike: prove the D100H vendor channel works via BlueZ D-Bus on Linux.
Sends heartbeat + firmware-version query; prints decoded notifications for N seconds."""
import asyncio, struct, sys
from dbus_fast.aio import MessageBus
from dbus_fast import BusType, Variant

DEV = "/org/bluez/hci0/dev_CA_62_68_9B_1B_D3"
NOTIFY = DEV + "/service004b/char004c"
WRITE = DEV + "/service004b/char004f"
K = struct.unpack("<4I", bytes.fromhex("cabaa5ca6d8a2abcba9e5acaca8bb89b"))
D, M = 0x9E3779B9, 0xFFFFFFFF

def tea(block, enc):
    v0, v1 = struct.unpack("<2I", block); k0, k1, k2, k3 = K
    if enc:
        s = 0
        for _ in range(32):
            s = (s + D) & M
            v0 = (v0 + ((((v1 << 4) + k0) ^ (v1 + s) ^ ((v1 >> 5) + k1)))) & M
            v1 = (v1 + ((((v0 << 4) + k2) ^ (v0 + s) ^ ((v0 >> 5) + k3)))) & M
    else:
        s = 0xC6EF3720
        for _ in range(32):
            v1 = (v1 - ((((v0 << 4) + k2) ^ (v0 + s) ^ ((v0 >> 5) + k3)))) & M
            v0 = (v0 - ((((v1 << 4) + k0) ^ (v1 + s) ^ ((v1 >> 5) + k1)))) & M
            s = (s - D) & M
    return struct.pack("<2I", v0, v1)

def xf(b, enc):
    b = bytes(b); return tea(b[0:8], enc) + tea(b[8:16], enc) + b[16:]

def pkt(prefix_hex):
    p = bytes.fromhex(prefix_hex); return xf(p + bytes(20 - len(p)), True)

assert pkt("8b100101").hex() == "8e8f5e16ec67281e3890c499a360aaad00000000", "TEA vector mismatch"

async def main(secs):
    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    async def iface(path, name):
        intro = await bus.introspect("org.bluez", path)
        return bus.get_proxy_object("org.bluez", path, intro).get_interface(name)
    n = await iface(NOTIFY, "org.bluez.GattCharacteristic1")
    w = await iface(WRITE, "org.bluez.GattCharacteristic1")
    props = (await bus.introspect("org.bluez", NOTIFY))
    pobj = bus.get_proxy_object("org.bluez", NOTIFY, props).get_interface("org.freedesktop.DBus.Properties")
    def on_change(iface_name, changed, _inv):
        if "Value" in changed:
            pt = xf(changed["Value"].value, False)
            print("RX", pt.hex())
    pobj.on_properties_changed(on_change)
    await n.call_start_notify()
    opts = {"type": Variant("s", "command")}
    hb = pkt("06012300" + "01")
    await w.call_write_value(hb, opts)
    for q in ["01040401", "01064001", "01014201"]:  # fw version, haptics read, sleep read
        await w.call_write_value(pkt(q), opts)
    for _ in range(secs):
        await asyncio.sleep(1)
        await w.call_write_value(hb, opts)
    await n.call_stop_notify()
    print("done")

asyncio.run(main(int(sys.argv[1]) if len(sys.argv) > 1 else 6))
