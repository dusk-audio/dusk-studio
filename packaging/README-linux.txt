Dusk Studio - Linux
===================

RUN IT (no install)
-------------------
    ./DuskStudio/DuskStudio

That's it - the program runs in place. To open a session, pass its path:
    ./DuskStudio/DuskStudio /path/to/session.json


INSTALL (menu entry + PATH + session file association)
------------------------------------------------------
    ./install.sh              install for the current user (~/.local, no root)
    sudo ./install.sh --system   install system-wide (/opt + /usr/local)

After installing you can launch "Dusk Studio" from your app menu, run
`DuskStudio` from a terminal, and double-click a session.json to open it.

Remove it again:
    ./install.sh --uninstall            (or: sudo ./install.sh --system --uninstall)


REQUIREMENTS
------------
A normal desktop Linux (X11 or Wayland). Audio runs over PipeWire/JACK
(preferred) or ALSA. On a minimal system, install the usual desktop libraries
(X11, freetype, fontconfig, alsa) if the binary reports a missing library.
