"""Close an X11 window the way a window manager's close button does.

The legs run on a bare Xvfb display with no window manager, so nothing there
sends WM_DELETE_WINDOW. The app only asks for that message when the atom
already exists as it starts, and Xvfb drops every atom when its last client
disconnects, so the atom needs a client holding the display open from before
the launch until the close is sent.

    x11_close.py hold               intern the atoms, print "ready", block
    x11_close.py close <window> [n]  send WM_DELETE_WINDOW n times (default 1)
"""

import ctypes
import signal
import sys

ClientMessage = 33


class XClientMessageEvent(ctypes.Structure):
    _fields_ = [
        ('type', ctypes.c_int),
        ('serial', ctypes.c_ulong),
        ('send_event', ctypes.c_int),
        ('display', ctypes.c_void_p),
        ('window', ctypes.c_ulong),
        ('message_type', ctypes.c_ulong),
        ('format', ctypes.c_int),
        ('data', ctypes.c_long * 5),
    ]


class XEvent(ctypes.Union):
    _fields_ = [('xclient', XClientMessageEvent), ('pad', ctypes.c_long * 24)]


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ('hold', 'close'):
        sys.exit(__doc__)
    x11 = ctypes.CDLL('libX11.so.6')
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XInternAtom.restype = ctypes.c_ulong
    x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x11.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int,
                               ctypes.c_long, ctypes.POINTER(XEvent)]
    x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]

    display = x11.XOpenDisplay(None)
    if not display:
        sys.exit('x11_close: cannot open the display')
    protocols = x11.XInternAtom(display, b'WM_PROTOCOLS', 0)
    delete = x11.XInternAtom(display, b'WM_DELETE_WINDOW', 0)
    x11.XSync(display, 0)

    if sys.argv[1] == 'hold':
        print('ready', flush=True)
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
        while True:
            signal.pause()

    if len(sys.argv) < 3:
        sys.exit(__doc__)
    window = int(sys.argv[2], 0)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    event = XEvent()
    event.xclient.type = ClientMessage
    event.xclient.window = window
    event.xclient.message_type = protocols
    event.xclient.format = 32
    event.xclient.data[0] = delete
    for _ in range(count):
        x11.XSendEvent(display, window, 0, 0, ctypes.byref(event))
    x11.XSync(display, 0)
    return 0


if __name__ == '__main__':
    sys.exit(main())
