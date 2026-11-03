/* The X client side of x11-drop-input.sh: an XDND drag source that can hold its
 * data back, and a pointer device added while the app runs.
 *
 *   x11_drop_input drag <window> <x> <y> <uri-list file> data-first|leave-before-data|never-answer
 *   x11_drop_input add-pointer <name>
 *   x11_drop_input click <name> <x> <y> <park x> <park y>
 *
 * drag offers text/plain;charset=utf-8 then text/uri-list, the order GTK lists
 * them in, at root coordinates x,y. leave-before-data holds every conversion
 * request until it has sent XdndDrop and then XdndLeave, the order Mutter's
 * relay can produce. never-answer sends the same messages and then never
 * answers at all. It prints FINISHED accepted=<0|1>, or NO-FINISHED when the
 * target never answers the drop. Its XdndPosition and XdndDrop carry a time
 * past 2^31 ms, which Xlib sign-extends in a ClientMessage and zero-extends in
 * the SelectionNotify that answers the conversion, as on a server up for more
 * than 25 days.
 *
 * add-pointer adds a master pointer, which brings its own XTEST slave. click
 * presses button 1 through that slave, so the events carry a device id the app
 * did not see when it started, and then moves the pointer away to the park
 * position. */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static Display* dpy;
static Window source, target;
static long dndTime;
static Atom aEnter, aPosition, aStatus, aLeave, aDrop, aFinished, aSelection, aCopy, aUriList, aText;
static char* payload;
static size_t payloadSize;

enum { maxHeld = 16 };
static XSelectionRequestEvent held[maxHeld];
static int numHeld, numRequests, statusSeen, statusAccept, finished = -1;

static double now (void)
{
    struct timespec ts;
    clock_gettime (CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static void answer (const XSelectionRequestEvent* req)
{
    XEvent reply;
    memset (&reply, 0, sizeof reply);
    reply.xselection.type = SelectionNotify;
    reply.xselection.requestor = req->requestor;
    reply.xselection.selection = req->selection;
    reply.xselection.target = req->target;
    reply.xselection.time = req->time;
    reply.xselection.property = None;

    if (req->target == aUriList || req->target == aText)
    {
        Atom property = req->property != None ? req->property : req->target;
        XChangeProperty (dpy, req->requestor, property, req->target, 8, PropModeReplace,
                         (const unsigned char*) payload, (int) payloadSize);
        reply.xselection.property = property;
    }

    XSendEvent (dpy, req->requestor, False, NoEventMask, &reply);
    XFlush (dpy);
}

/* Handles what arrives within the given seconds. A held request is kept for
 * later instead of being answered. */
static void pump (double seconds, int hold)
{
    double end = now() + seconds;

    for (;;)
    {
        while (XPending (dpy))
        {
            XEvent ev;
            XNextEvent (dpy, &ev);

            if (ev.type == SelectionRequest)
            {
                ++numRequests;
                if (hold && numHeld < maxHeld)
                    held[numHeld++] = ev.xselectionrequest;
                else
                    answer (&ev.xselectionrequest);
            }
            else if (ev.type == ClientMessage && ev.xclient.message_type == aStatus)
            {
                statusSeen = 1;
                statusAccept = (int) (ev.xclient.data.l[1] & 1);
            }
            else if (ev.type == ClientMessage && ev.xclient.message_type == aFinished)
            {
                finished = (int) (ev.xclient.data.l[1] & 1);
            }
        }

        double left = end - now();
        if (left <= 0)
            return;

        struct pollfd pfd = { ConnectionNumber (dpy), POLLIN, 0 };
        poll (&pfd, 1, (int) (left * 1000) + 1);
    }
}

static void pumpUntil (double seconds, int hold, const int* flag, int wanted)
{
    double end = now() + seconds;
    while (*flag != wanted && now() < end)
        pump (0.02, hold);
}

static void sendDnd (Atom type, long l1, long l2, long l3, long l4)
{
    XEvent ev;
    memset (&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = target;
    ev.xclient.message_type = type;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long) source;
    ev.xclient.data.l[1] = l1;
    ev.xclient.data.l[2] = l2;
    ev.xclient.data.l[3] = l3;
    ev.xclient.data.l[4] = l4;
    XSendEvent (dpy, target, False, NoEventMask, &ev);
    XFlush (dpy);
}

static int drag (int argc, char** argv)
{
    if (argc != 7)
        return 2;

    target = (Window) strtoul (argv[2], NULL, 0);
    int x = atoi (argv[3]), y = atoi (argv[4]);
    int neverAnswer = strcmp (argv[6], "never-answer") == 0;
    int leaveFirst = neverAnswer || strcmp (argv[6], "leave-before-data") == 0;

    FILE* f = fopen (argv[5], "rb");
    if (f == NULL)
        return 2;
    payload = malloc (1 << 20);
    payloadSize = fread (payload, 1, 1 << 20, f);
    fclose (f);

    aEnter = XInternAtom (dpy, "XdndEnter", False);
    aPosition = XInternAtom (dpy, "XdndPosition", False);
    aStatus = XInternAtom (dpy, "XdndStatus", False);
    aLeave = XInternAtom (dpy, "XdndLeave", False);
    aDrop = XInternAtom (dpy, "XdndDrop", False);
    aFinished = XInternAtom (dpy, "XdndFinished", False);
    aSelection = XInternAtom (dpy, "XdndSelection", False);
    aCopy = XInternAtom (dpy, "XdndActionCopy", False);
    aUriList = XInternAtom (dpy, "text/uri-list", False);
    aText = XInternAtom (dpy, "text/plain;charset=utf-8", False);

    dndTime = (long) (0x80000000UL | ((unsigned long) getpid() & 0xffffUL));
    source = XCreateSimpleWindow (dpy, DefaultRootWindow (dpy), -10, -10, 1, 1, 0, 0, 0);
    XSetSelectionOwner (dpy, aSelection, source, CurrentTime);
    if (XGetSelectionOwner (dpy, aSelection) != source)
    {
        puts ("ERROR could not own XdndSelection");
        return 1;
    }

    sendDnd (aEnter, 5L << 24, (long) aText, (long) aUriList, None);
    sendDnd (aPosition, 0, ((long) x << 16) | y, dndTime, (long) aCopy);

    if (leaveFirst)
    {
        pumpUntil (5.0, 1, &numRequests, 1);
        pumpUntil (5.0, 1, &statusSeen, 1);
        printf ("STATUS seen=%d accept=%d\n", statusSeen, statusAccept);
        if (numRequests == 0)
        {
            puts ("ERROR the target never asked for the data");
            return 1;
        }
        sendDnd (aDrop, 0, dndTime, 0, 0);
        pump (0.5, 1);
        sendDnd (aLeave, 0, 0, 0, 0);
        pump (0.5, 1);
        printf ("HELD %d\n", numHeld);
        if (! neverAnswer)
        {
            for (int i = 0; i < numHeld; ++i)
                answer (&held[i]);
            numHeld = 0;
        }
    }
    else
    {
        pumpUntil (5.0, 0, &statusSeen, 1);
        printf ("STATUS seen=%d accept=%d\n", statusSeen, statusAccept);
        pump (0.5, 0);
        sendDnd (aDrop, 0, dndTime, 0, 0);
    }

    double end = now() + 8.0;
    while (finished < 0 && now() < end)
        pump (0.02, neverAnswer);

    printf ("REQUESTS %d\n", numRequests);
    if (finished < 0)
        puts ("NO-FINISHED");
    else
        printf ("FINISHED accepted=%d\n", finished);
    fflush (stdout);
    return 0;
}

static int findDevice (const char* name, int use)
{
    int count = 0, id = -1;
    XIDeviceInfo* info = XIQueryDevice (dpy, XIAllDevices, &count);
    for (int i = 0; i < count; ++i)
        if (info[i].use == use && strcmp (info[i].name, name) == 0)
            id = info[i].deviceid;
    XIFreeDeviceInfo (info);
    return id;
}

static int addPointer (const char* name)
{
    XIAddMasterInfo add;
    memset (&add, 0, sizeof add);
    add.type = XIAddMaster;
    add.name = (char*) name;
    add.send_core = True;
    add.enable = True;

    if (XIChangeHierarchy (dpy, (XIAnyHierarchyChangeInfo*) &add, 1) != Success)
        return 1;
    XSync (dpy, False);

    char masterName[256], slaveName[256];
    snprintf (masterName, sizeof masterName, "%s pointer", name);
    snprintf (slaveName, sizeof slaveName, "%s XTEST pointer", name);
    int master = findDevice (masterName, XIMasterPointer);
    int slave = findDevice (slaveName, XISlavePointer);
    printf ("MASTER %d\nSLAVE %d\n", master, slave);
    return master < 0 || slave < 0;
}

static int click (const char* name, int x, int y, int parkX, int parkY)
{
    char masterName[256], slaveName[256];
    snprintf (masterName, sizeof masterName, "%s pointer", name);
    snprintf (slaveName, sizeof slaveName, "%s XTEST pointer", name);
    int master = findDevice (masterName, XIMasterPointer);
    int slave = findDevice (slaveName, XISlavePointer);
    if (master < 0 || slave < 0)
    {
        puts ("ERROR no such pointer");
        return 1;
    }

    /* XTest's core requests drive the XTEST slave of the client's own pointer,
     * so this client's pointer becomes the added master. */
    XISetClientPointer (dpy, None, master);
    XIWarpPointer (dpy, master, None, DefaultRootWindow (dpy), 0, 0, 0, 0, x, y);
    XSync (dpy, False);
    XTestFakeMotionEvent (dpy, -1, x, y, CurrentTime);
    XSync (dpy, False);
    struct timespec pause = { 0, 100 * 1000 * 1000 };
    nanosleep (&pause, NULL);
    XTestFakeButtonEvent (dpy, 1, True, CurrentTime);
    XSync (dpy, False);
    nanosleep (&pause, NULL);
    XTestFakeButtonEvent (dpy, 1, False, CurrentTime);
    XSync (dpy, False);
    nanosleep (&pause, NULL);
    XTestFakeMotionEvent (dpy, -1, parkX, parkY, CurrentTime);
    XSync (dpy, False);
    printf ("CLICKED master=%d slave=%d\n", master, slave);
    return 0;
}

int main (int argc, char** argv)
{
    if (argc < 2 || (dpy = XOpenDisplay (NULL)) == NULL)
        return 2;

    int rc = 2;
    if (strcmp (argv[1], "drag") == 0)
        rc = drag (argc, argv);
    else if (strcmp (argv[1], "add-pointer") == 0 && argc == 3)
        rc = addPointer (argv[2]);
    else if (strcmp (argv[1], "click") == 0 && argc == 7)
        rc = click (argv[2], atoi (argv[3]), atoi (argv[4]), atoi (argv[5]), atoi (argv[6]));

    XCloseDisplay (dpy);
    return rc;
}
