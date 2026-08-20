Dusk Studio - Linux
===================

RUN IT (no install)
-------------------
    ./DuskStudio/DuskStudio

That's it - the program runs in place. To open a session, pass its path:
    ./DuskStudio/DuskStudio /path/to/session.json

App icon when running in place: it shows on X11, but is GENERIC on Wayland.
This is a Wayland rule, not a bug - a Wayland app can't set its own taskbar
icon; the dock only shows one after a desktop entry is registered. Run
./install.sh (below) to get the real Dusk Studio icon in your dock, app menu,
and alt-tab. (X11 shows the icon either way.)


INSTALL (menu entry + dock/taskbar icon + PATH + session file association)
--------------------------------------------------------------------------
    ./install.sh              install for the current user (~/.local, no root)
    sudo ./install.sh --system   install system-wide (/opt + /usr/local)

After installing you can launch "Dusk Studio" from your app menu, run
`DuskStudio` from a terminal, and double-click a session.json to open it.
The proper Dusk Studio icon then shows in the dock, app menu, and alt-tab
on both X11 and Wayland.

Remove it again:
    ./install.sh --uninstall            (or: sudo ./install.sh --system --uninstall)


REQUIREMENTS
------------
A normal desktop Linux (X11 or Wayland). Audio runs over PipeWire/JACK
(preferred) or ALSA.

The binary links these libraries and will not start without them. The PipeWire
client is linked in even if you only use ALSA, and libsuil and libmp3lame are
not on a stock desktop install, so check them first if the binary exits with
"error while loading shared libraries":

    libpipewire-0.3.so.0   libpipewire-0.3-0
    libsuil-0.so.0         libsuil-0-0
    libmp3lame.so.0        libmp3lame0
    libsndfile.so.1        libsndfile1
    libasound.so.2         libasound2
    libfreetype.so.6       libfreetype6
    libfontconfig.so.1     libfontconfig1
    libGL.so.1             libgl1
    libX11.so.6            libx11-6
    libXcursor.so.1        libxcursor1
    libXext.so.6           libxext6
    libXrandr.so.2         libxrandr2

The right-hand column is the Debian/Ubuntu package name. On those:

    sudo apt install libpipewire-0.3-0 libsuil-0-0 libmp3lame0 libsndfile1 \
        libasound2 libfreetype6 libfontconfig1 libgl1 libx11-6 libxcursor1 \
        libxext6 libxrandr2

Signed SFZ catalog authentication needs nothing from the host; libsodium is
compiled into the binary.


LICENSE
-------
Dusk Studio is GPL-3.0-or-later. The full text is in LICENSE beside this file;
LICENSES.txt inventories every third-party component compiled into the binary
and the license each one ships under. Both files travel with any copy you pass
on.
