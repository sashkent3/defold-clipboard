#include "clipboard_private.h"
#include <dmsdk/sdk.h>

#if defined(DM_PLATFORM_LINUX)

// Inspired by https://github.com/exebook/x11clipboard

#include <X11/Xlib.h>
#include <dmsdk/dlib/thread.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

// How long the event thread sleeps waiting for X11 traffic before re-checking
// the queue and the exit flag. Bounds paste latency for other apps and shutdown time.
static const int EVENT_POLL_TIMEOUT_MS = 50;
// How long paste waits for the clipboard owner to reply before giving up (same as Qt).
static const int PASTE_TIMEOUT_MS = 5000;

Display *display;
Window window;
Atom UTF8, XA_STRING = 31, selection;
dmThread::Thread loop_thread;
// Guards all Xlib calls on `display` (shared between the main thread and the
// event thread) and `cur_text`.
dmMutex::HMutex mutex;
char *cur_text;
char *last_pasted;
volatile bool exit_flag;

static void SetCurText(char *text) {
    free(cur_text);
    cur_text = text;
}

static void HandleSelectionRequest(XEvent *event) {
    if (cur_text == 0) return;
    if (event->xselectionrequest.selection != selection) return;
    int size = strlen(cur_text);
    UTF8 = XInternAtom(display, "UTF8_STRING", 1);
    if (UTF8 == None) UTF8 = XA_STRING;
    Atom targets_atom = XInternAtom(display, "TARGETS", 0),
         text_atom = XInternAtom(display, "TEXT", 0),
         XA_ATOM = 4;
    XSelectionRequestEvent *xsr = &event->xselectionrequest;
    XSelectionEvent ev = {0};
    int R = 0;
    ev.type = SelectionNotify, ev.display = xsr->display, ev.requestor = xsr->requestor,
    ev.selection = xsr->selection, ev.time = xsr->time, ev.target = xsr->target, ev.property = xsr->property;
    if (ev.target == targets_atom)
        R = XChangeProperty(ev.display, ev.requestor, ev.property, XA_ATOM, 32,
                            PropModeReplace, (unsigned char *)&UTF8, 1);
    else if (ev.target == XA_STRING || ev.target == text_atom)
        R = XChangeProperty(ev.display, ev.requestor, ev.property, XA_STRING, 8, PropModeReplace, (unsigned char *)cur_text, size);
    else if (ev.target == UTF8)
        R = XChangeProperty(ev.display, ev.requestor, ev.property, UTF8, 8, PropModeReplace, (unsigned char *)cur_text, size);
    else
        ev.property = None;
    if ((R & 2) == 0)
        XSendEvent(display, ev.requestor, 0, 0, (XEvent *)&ev);
}

// Sleeps until the X server sends something or the timeout expires.
static void WaitForXInput(int timeout_ms) {
    struct pollfd pfd = { ConnectionNumber(display), POLLIN, 0 };
    poll(&pfd, 1, timeout_ms);
}

void EventLoop(void *user_data) {
    XEvent event;
    while (!exit_flag) {
        dmMutex::Lock(mutex);
        while (XCheckTypedEvent(display, SelectionClear, &event))
            SetCurText(0);
        while (XCheckTypedEvent(display, SelectionRequest, &event))
            HandleSelectionRequest(&event);
        XFlush(display);
        dmMutex::Unlock(mutex);
        // The timeout also covers events already read into Xlib's queue by the main thread.
        WaitForXInput(EVENT_POLL_TIMEOUT_MS);
    }
}

void InitializeLinuxClipboard() {
    display = XOpenDisplay(0);
    if (!display) {
        dmLogWarning("clipboard: unable to open X display, clipboard is disabled");
        return;
    }
    int N = DefaultScreen(display);
    window = XCreateSimpleWindow(display, RootWindow(display, N), 0, 0, 1, 1, 0,
                                 BlackPixel(display, N), WhitePixel(display, N));
    exit_flag = 0;
    mutex = dmMutex::New();
    loop_thread = dmThread::New(EventLoop, 0x80000, 0, "defclip");
}

void FinalizeLinuxClipboard() {
    if (!display) return;
    exit_flag = 1;
    dmThread::Join(loop_thread);
    dmMutex::Delete(mutex);
    SetCurText(0);
    free(last_pasted);
    last_pasted = 0;
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    display = 0;
}

void clipboard_to_clipboard(const char *text) {
    if (!display) return;
    DM_MUTEX_SCOPED_LOCK(mutex);
    selection = XInternAtom(display, "CLIPBOARD", 0);
    XSetSelectionOwner(display, selection, window, 0);
    if (XGetSelectionOwner(display, selection) == window)
        SetCurText(strdup(text));
}

static Bool IsSelectionNotify(Display *, XEvent *event, XPointer) {
    return event->type == SelectionNotify;
}

// Waits for the owner's SelectionNotify reply. Returns false if none arrives before `deadline_us`.
static bool WaitForSelectionNotify(XEvent *event, uint64_t deadline_us) {
    // Only take SelectionNotify, leave SelectionRequest/SelectionClear for the event thread.
    while (!XCheckIfEvent(display, event, IsSelectionNotify, 0)) {
        uint64_t now = dmTime::GetMonotonicTime();
        if (now >= deadline_us) {
            dmLogWarning("clipboard: the clipboard owner did not reply within %d ms", PASTE_TIMEOUT_MS);
            return false;
        }
        WaitForXInput((int)((deadline_us - now + 999) / 1000));
    }
    return true;
}

static char *XPasteType(Atom atom, uint64_t deadline_us) {
    XEvent event;
    int format;
    unsigned long N, size;
    char *data = 0, *s = 0;
    Atom target,
        CLIPBOARD = XInternAtom(display, "CLIPBOARD", 0),
        XSEL_DATA = XInternAtom(display, "XSEL_DATA", 0);
    // Best effort: drop late replies to earlier requests that timed out.
    while (XCheckIfEvent(display, &event, IsSelectionNotify, 0)) {}
    XConvertSelection(display, CLIPBOARD, atom, XSEL_DATA, window, CurrentTime);
    XFlush(display);
    if (!WaitForSelectionNotify(&event, deadline_us)) return 0;

    if (event.xselection.selection == CLIPBOARD && event.xselection.property) {
        XGetWindowProperty(event.xselection.display, event.xselection.requestor,
                           event.xselection.property, 0L, (~0L), 0, AnyPropertyType, &target,
                           &format, &size, &N, (unsigned char **)&data);
        if (data) {
            if (target == UTF8 || target == XA_STRING)
                s = strndup(data, size);
            XFree(data);
        }
        XDeleteProperty(event.xselection.display, event.xselection.requestor, event.xselection.property);
    }
    return s;
}

// The returned string stays valid until the next call.
const char *clipboard_from_clipboard() {
    if (!display) return 0;
    free(last_pasted);
    last_pasted = 0;
    DM_MUTEX_SCOPED_LOCK(mutex);
    if (cur_text != 0) {
        last_pasted = strdup(cur_text);
        return last_pasted;
    }
    // One deadline for the whole paste, including the STRING fallback.
    uint64_t deadline_us = dmTime::GetMonotonicTime() + PASTE_TIMEOUT_MS * 1000;
    UTF8 = XInternAtom(display, "UTF8_STRING", True);
    if (UTF8 != None) last_pasted = XPasteType(UTF8, deadline_us);
    if (!last_pasted && dmTime::GetMonotonicTime() < deadline_us)
        last_pasted = XPasteType(XA_STRING, deadline_us);
    return last_pasted;
}

#endif
