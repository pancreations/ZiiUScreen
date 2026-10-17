"""Motion load for offline capture tests: a borderless window covering one display
(found by monitor hardware ID, e.g. the HDMI dummy plug's YOUR_DISPLAY_ID) with scrolling
text on PowerShell blue, a moving box and a gradient. Optionally freezes after a
while so a test can see whether artifacts clear once motion stops.

  python tools/ziiu-drc/motion_load.py <monitor id substring> <seconds> [freeze after s]
"""
import ctypes
import sys
import time
import tkinter as tk
from ctypes import wintypes as W

ctypes.windll.shcore.SetProcessDpiAwareness(2)
u = ctypes.windll.user32


class DD(ctypes.Structure):
    _fields_ = [('cb', W.DWORD), ('Name', W.WCHAR * 32), ('Str', W.WCHAR * 128), ('Flags', W.DWORD),
                ('Id', W.WCHAR * 128), ('Key', W.WCHAR * 128)]


class MI(ctypes.Structure):
    _fields_ = [('cb', W.DWORD), ('rc', W.RECT), ('work', W.RECT), ('f', W.DWORD), ('dev', W.WCHAR * 32)]


def find_display(monitor_id):
    """GDI device name of the active display whose monitor ID contains monitor_id."""
    i = 0
    while True:
        a = DD()
        a.cb = ctypes.sizeof(DD)
        if not u.EnumDisplayDevicesW(None, i, ctypes.byref(a), 0):
            return None
        i += 1
        m = DD()
        m.cb = ctypes.sizeof(DD)
        if a.Flags & 1 and u.EnumDisplayDevicesW(a.Name, 0, ctypes.byref(m), 0) and monitor_id in m.Id:
            return a.Name


def display_rect(device):
    found = []
    proc = ctypes.WINFUNCTYPE(W.BOOL, W.HMONITOR, W.HDC, ctypes.POINTER(W.RECT), W.LPARAM)

    def cb(h, dc, r, lp):
        mi = MI()
        mi.cb = ctypes.sizeof(MI)
        u.GetMonitorInfoW(h, ctypes.byref(mi))
        if mi.dev == device:
            found.append((mi.rc.left, mi.rc.top, mi.rc.right - mi.rc.left, mi.rc.bottom - mi.rc.top))
        return True

    u.EnumDisplayMonitors(None, None, proc(cb), 0)
    return found[0] if found else None


dev = find_display(sys.argv[1])
rect = display_rect(dev) if dev else None
if not rect:
    sys.exit('no active display with monitor ID containing %s' % sys.argv[1])
x, y, w, h = rect
secs = float(sys.argv[2])
stop_at = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
print('motion load on %s at %d,%d %dx%d' % (dev, x, y, w, h), flush=True)

root = tk.Tk()
root.overrideredirect(True)
root.geometry(f'{w}x{h}+{x}+{y}')
c = tk.Canvas(root, width=w, height=h, highlightthickness=0, bg='#012456')   # PowerShell blue
c.pack()
for i in range(0, w, 8):
    c.create_line(i, 0, i, h // 3, fill=f'#{i * 255 // w:02x}60{255 - i * 255 // w:02x}')
font = max(10, h // 54)
step = int(font * 1.7)
lines = [c.create_text(40, h // 3 + 30 + k * step, anchor='w', fill='white', font=('Consolas', font),
                       text=f'line {k:02d}  The quick brown fox jumps over the lazy dog 0123456789') for k in range(20)]
box = c.create_rectangle(0, h - h // 5, w // 12, h - h // 18, fill='#e04040', outline='')
t0 = time.time()


def tick():
    t = time.time() - t0
    if t > secs:
        root.destroy()
        return
    tm = min(t, stop_at)
    bx = int((tm * w / 3.2) % (w - w // 12))
    c.coords(box, bx, h - h // 5, bx + w // 12, h - h // 18)
    for k, ln in enumerate(lines):
        c.coords(ln, 40, h // 3 + 30 + ((k * step - int(tm * h / 9)) % (20 * step)))
    root.after(8, tick)


tick()
root.mainloop()
