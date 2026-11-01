# xpad.py [command file] [log file]
# A virtual Xbox 360 controller for tests (this PC has no game controller): plugged into Meta's ViGEm bus
# (Oculus_ViGEmBus 1.5.8, installed with the Meta Quest software, with an interface GUID of its own, which the
# standard ViGEmClient does not find) through ViGEm 1.x's requests as OVRServer_x64.exe makes them. XInput, the game's
# DirectInput and the Steam release's SDL3 see it as an Xbox controller. Start it before the game (which lists its
# controllers as it starts) and stop it with quit. Driven by lines appended to the command file (default
# build\xpad_cmds.txt; xpadstep.ps1 writes them):
#   fg PID             presses only while that process's window is in front (the Steam release's fo2_zoom.dll turns
#                      the pad's buttons into keys with SendInput, which go to whichever window is in front)
#   press A|B|X|Y|START|BACK|UP|DOWN|LEFT|RIGHT|LB|RB|LS|RS [ms]   (down, wait, up; default 150 ms)
#   hold NAME / release NAME
#   stick lx ly        (left stick, -1..1)
#   wait ms
#   quit
import ctypes, os, struct, sys, time
from ctypes import wintypes

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
k32.DeviceIoControl.argtypes = (wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p)
k32.CreateEventW.restype = wintypes.HANDLE
k32.CreateEventW.argtypes = (ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR)
k32.GetOverlappedResult.argtypes = (wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD), wintypes.BOOL)
k32.CloseHandle.argtypes = (wintypes.HANDLE,)

u32 = ctypes.WinDLL('user32')
u32.GetForegroundWindow.restype = wintypes.HWND
u32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
required_pid = 0

PATH = r'\\?\ROOT#SYSTEM#0001#{51bd9b45-ef2c-49ba-a0ce-c163ebc352a2}'
PLUGIN, UNPLUG, CHECKVER, SUBMIT = 0x2AA004, 0x2AA008, 0x2AA00C, 0x2AA808
SERIAL = 1

cmdfile = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'xpad_cmds.txt')
logf = open(sys.argv[2], 'a') if len(sys.argv) > 2 else sys.stdout


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, file=logf, flush=True)


class OVERLAPPED(ctypes.Structure):
    _fields_ = [('Internal', ctypes.c_void_p), ('InternalHigh', ctypes.c_void_p), ('Offset', wintypes.DWORD),
                ('OffsetHigh', wintypes.DWORD), ('hEvent', wintypes.HANDLE)]


h = k32.CreateFileW(PATH, 0xC0000000, 3, None, 3, 0x80 | 0x20000000 | 0x80000000 | 0x40000000, None)
if h in (None, wintypes.HANDLE(-1).value):
    log('open failed: error', ctypes.get_last_error())
    sys.exit(1)
event = k32.CreateEventW(None, False, False, None)


def ioctl(code, data):
    buf = ctypes.create_string_buffer(data, len(data))
    ov = OVERLAPPED()
    ov.hEvent = event
    n = wintypes.DWORD()
    ok = k32.DeviceIoControl(h, code, buf, len(data), None, 0, ctypes.byref(n), ctypes.byref(ov))
    err = ctypes.get_last_error()
    if not ok and err == 997:  # ERROR_IO_PENDING
        ok = k32.GetOverlappedResult(h, ctypes.byref(ov), ctypes.byref(n), True)
        err = ctypes.get_last_error()
    return bool(ok), err


ok, err = ioctl(CHECKVER, struct.pack('<II', 8, 0x4001))
log('check version:', ok, err)
if not ok:
    sys.exit(1)
ok, err = ioctl(PLUGIN, struct.pack('<IIIHH', 16, SERIAL, 0, 0x045E, 0x028E))
log('plug in:', ok, err)
if not ok:
    sys.exit(1)

BUTTONS = {'UP': 0x1, 'DOWN': 0x2, 'LEFT': 0x4, 'RIGHT': 0x8, 'START': 0x10, 'BACK': 0x20, 'LS': 0x40, 'RS': 0x80,
           'LB': 0x100, 'RB': 0x200, 'A': 0x1000, 'B': 0x2000, 'X': 0x4000, 'Y': 0x8000}
state = {'buttons': 0, 'lx': 0, 'ly': 0}


def send():
    r = struct.pack('<HBBhhhh', state['buttons'], 0, 0, state['lx'], state['ly'], 0, 0)
    ok, err = ioctl(SUBMIT, struct.pack('<II', 20, SERIAL) + r)
    if not ok:
        log('submit failed:', err)


send()
pos = os.path.getsize(cmdfile) if os.path.exists(cmdfile) else 0
running = True
try:
    while running:
        time.sleep(0.02)
        if not os.path.exists(cmdfile):
            continue
        size = os.path.getsize(cmdfile)
        if size < pos:
            pos = 0
        if size == pos:
            continue
        with open(cmdfile) as f:
            f.seek(pos)
            text = f.read()
            pos = f.tell()
        for line in text.splitlines():
            p = line.split()
            if not p:
                continue
            log('cmd', line)
            c = p[0].lower()
            if c == 'quit':
                running = False
                break
            elif c == 'fg':
                required_pid = int(p[1])
                continue
            # fo2_zoom.dll turns the pad's buttons into keys with SendInput, which go to whichever window is in
            # front: no press unless the game's window is.
            if c in ('press', 'hold', 'stick') and required_pid:
                fpid = wintypes.DWORD()
                u32.GetWindowThreadProcessId(u32.GetForegroundWindow(), ctypes.byref(fpid))
                if fpid.value != required_pid:
                    log('skipped: the game is not in front (pid %d is)' % fpid.value)
                    continue
            if c == 'press':
                b = BUTTONS[p[1].upper()]
                state['buttons'] |= b
                send()
                time.sleep((int(p[2]) if len(p) > 2 else 150) / 1000)
                state['buttons'] &= ~b
                send()
            elif c == 'hold':
                state['buttons'] |= BUTTONS[p[1].upper()]
                send()
            elif c == 'release':
                state['buttons'] &= ~BUTTONS[p[1].upper()]
                send()
            elif c == 'stick':
                state['lx'] = int(max(-1, min(1, float(p[1]))) * 32767)
                state['ly'] = int(max(-1, min(1, float(p[2]))) * 32767)
                send()
            elif c == 'wait':
                time.sleep(int(p[1]) / 1000)
finally:
    ok, err = ioctl(UNPLUG, struct.pack('<II', 8, SERIAL))
    log('unplug:', ok, err)
    k32.CloseHandle(h)
