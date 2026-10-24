#include "kernel/commandline.h"

#include <string.h>

#include <curses.h>

#include "annotation/definition.h"
#include "annotation/overview.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: CommandLine ncurses backend
 * ============================================================================
 * The default CommandLineIo device backend (R1). It is the terminal analog of
 * window/window_cocoa.m: the ONLY translation unit that includes a terminal
 * library, exposing the platform-agnostic CommandLineIo fn-table so CommandLine
 * and its callers never see ncurses. Owns the terminal screen for the process
 * (single `SCREEN`), enters cbreak/no-echo/keypad with hidden cursor and color
 * pairs, and restores the terminal on close. No widget, grid or app state lives
 * here — only device I/O.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * MODULE: CommandLine ncurses backend (kernel/commandline_ncurses.c)
 * ============================================================================
 * STATE (file-local):
 *   static bool g_open            : terminal screen acquired
 *   static bool g_mouse           : pointer reporting enabled
 * FUNCTION REGISTRY (all static; exposed only through CommandLine_defaultIo):
 *   ncOpen, ncClose, ncSize, ncPresent, ncPoll, ncSetMouse
 *   CommandLine_defaultIo(void)   : returns the populated fn-table by value
 * CONTRACT: single owner thread; no threads, sockets or allocation owned. poll
 * waits at most timeoutMs (the Bounded Wait Law). present is one refresh.
 * Unsupported terminals degrade (open returns false) rather than crashing.
 * ============================================================================
 */

static bool g_open = false;
static bool g_mouse = false;

static bool ncOpen(void *ctx) {
    (void) ctx;
    if (g_open)
        return true;
    if (initscr() == nullptr)
        return false;
    cbreak();
    noecho();
    keypad(stdscr, true);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
    }
    g_open = true;
    return true;
}

static void ncClose(void *ctx) {
    (void) ctx;
    if (!g_open)
        return;
    if (g_mouse) {
        mousemask(0, nullptr);
        g_mouse = false;
    }
    endwin();
    g_open = false;
}

static bool ncSize(void *ctx, size_t *outW, size_t *outH) {
    (void) ctx;
    if (!g_open)
        return false;
    int rows = 0, cols = 0;
    getmaxyx(stdscr, rows, cols);
    if (rows <= 0 || cols <= 0)
        return false;
    *outH = (size_t) rows;
    *outW = (size_t) cols;
    return true;
}

static bool ncPresent(void *ctx, const char *cells, size_t width, size_t height) {
    (void) ctx;
    if (!g_open || cells == nullptr || width == 0 || height == 0)
        return false;
    for (size_t y = 0; y < height; ++y) {
        if (move((int) y, 0) == ERR)
            return false;
        if (addnstr(cells + y * width, (int) width) == ERR)
            return false;
    }
    return refresh() != ERR;
}

static bool ncSetMouse(void *ctx, bool enabled) {
    (void) ctx;
    if (!g_open)
        return false;
    if (enabled) {
        mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
        g_mouse = true;
    } else {
        mousemask(0, nullptr);
        g_mouse = false;
    }
    return true;
}

static CommandLineKey ncMapKey(int ch) {
    switch (ch) {
        case KEY_UP: return COMMANDLINE_KEY_UP;
        case KEY_DOWN: return COMMANDLINE_KEY_DOWN;
        case KEY_LEFT: return COMMANDLINE_KEY_LEFT;
        case KEY_RIGHT: return COMMANDLINE_KEY_RIGHT;
        case KEY_HOME: return COMMANDLINE_KEY_HOME;
        case KEY_END: return COMMANDLINE_KEY_END;
        case KEY_DC: return COMMANDLINE_KEY_DELETE;
        case KEY_PPAGE: return COMMANDLINE_KEY_PAGE_UP;
        case KEY_NPAGE: return COMMANDLINE_KEY_PAGE_DOWN;
        case '\t': return COMMANDLINE_KEY_TAB;
        case 27: return COMMANDLINE_KEY_ESCAPE;
        case KEY_BACKSPACE: case 127: case '\b': return COMMANDLINE_KEY_BACKSPACE;
        default: return COMMANDLINE_KEY_NONE;
    }
}

static bool ncPoll(void *ctx, int timeoutMs, CommandLineEvent *out) {
    (void) ctx;
    if (!g_open || out == nullptr)
        return false;
    timeout(timeoutMs);
    int ch = getch();
    if (ch == ERR)
        return false;
    if (ch == KEY_RESIZE) {
        (*out).kind = COMMANDLINE_EVENT_RESIZE;
        return true;
    }
    if (ch == KEY_MOUSE) {
        MEVENT ev;
        if (getmouse(&ev) != OK)
            return false;
        (*out).x = ev.x;
        (*out).y = ev.y;
        if ((ev.bstate & BUTTON4_PRESSED) != 0)
            (*out).kind = COMMANDLINE_EVENT_WHEEL_UP;
#ifdef BUTTON5_PRESSED
        else if ((ev.bstate & BUTTON5_PRESSED) != 0)
            (*out).kind = COMMANDLINE_EVENT_WHEEL_DOWN;
#endif
        else if ((ev.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_RELEASED)) != 0 ||
                 (ev.bstate & (BUTTON2_CLICKED | BUTTON3_CLICKED)) != 0)
            (*out).kind = COMMANDLINE_EVENT_CLICK;
        else
            return false;
        return true;
    }
    CommandLineKey key = ncMapKey(ch);
    (*out).kind = COMMANDLINE_EVENT_KEY;
    if (key != COMMANDLINE_KEY_NONE) {
        (*out).key = (int) key;
    } else if (ch == '\n' || ch == KEY_ENTER) {
        (*out).key = (int) COMMANDLINE_KEY_ENTER;
    } else if (ch >= 32 && ch < 127) {
        (*out).key = ch;
    } else if (ch >= 1 && ch < 32) {
        (*out).key = ch;   // control code (e.g. Ctrl-Q = 17)
    } else {
        (*out).key = (int) COMMANDLINE_KEY_UNKNOWN;
    }
    return true;
}

CommandLineIo CommandLine_defaultIo(void) {
    CommandLineIo io;
    memset(&io, 0, sizeof(io));
    io.ctx = nullptr;
    io.open = ncOpen;
    io.close = ncClose;
    io.size = ncSize;
    io.present = ncPresent;
    io.poll = ncPoll;
    io.setMouse = ncSetMouse;
    return io;
}
