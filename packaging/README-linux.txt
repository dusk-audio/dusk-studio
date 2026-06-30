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
(preferred) or ALSA. On a minimal system, install the usual desktop libraries
(X11, freetype, fontconfig, alsa) if the binary reports a missing library.
