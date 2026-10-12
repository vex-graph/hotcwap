#ifndef HOT_KERNEL_COMMANDLINE_H
#define HOT_KERNEL_COMMANDLINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

// kernel/commandline.h — R1 terminal-application host (the fourth process kind).
//
// The process taxonomy (the Vertical Integration Law) classifies by primary
// contract: pixel surfaces are Application, stdio sessions are Console,
// one-shot functions are Process, and a full-screen TERMINAL APPLICATION is
// CommandLine. CommandLine is the terminal analog of Window — it owns the TTY
// DEVICE (raw mode, alternate screen, colors, one cell grid, bounded input),
// never widget semantics.
//
// Determinant (R3) owns cells, widgets and the reloadable view/command module;
// TerminalApp (R3) composes it. CommandLine includes NO R3 header: it exposes a
// dependency-free seam (an injectable CommandLineIo backend plus the owned cell
// grid and normalized events) exactly as Console borrows ProcessSpawn through
// ConsoleIo. The R5 application wires the toolkit to this seam.
//
// ONE RESIZE SURFACE: CommandLine_revalidate re-reads the device size, grows the
// owned grid and presents once. Resize is ALWAYS honored and never deferred to
// a "settle" — the table stakes for a live terminal UI.
//
// View swappability: CommandLine_setApp replaces the borrowed application seam
// WITHOUT re-acquiring the terminal, so a hot-loaded view can be published in
// the same process — the restart is never required. CommandLine owns device
// lifetime; the host owns loaded-code lifetime (HotModule).

typedef struct CommandLine CommandLine;

#define COMMANDLINE_MAX_TITLE 128
#define COMMANDLINE_MIN_COLUMNS 20
#define COMMANDLINE_MIN_ROWS 4

// Host-normalized key vocabulary, independent of any ncurses numeric value.
typedef enum CommandLineKey {
    COMMANDLINE_KEY_NONE = 0,
    COMMANDLINE_KEY_UP,
    COMMANDLINE_KEY_DOWN,
    COMMANDLINE_KEY_LEFT,
    COMMANDLINE_KEY_RIGHT,
    COMMANDLINE_KEY_ENTER,
    COMMANDLINE_KEY_ESCAPE,
    COMMANDLINE_KEY_BACKSPACE,
    COMMANDLINE_KEY_TAB,
    COMMANDLINE_KEY_HOME,
    COMMANDLINE_KEY_END,
    COMMANDLINE_KEY_DELETE,
    COMMANDLINE_KEY_PAGE_UP,
    COMMANDLINE_KEY_PAGE_DOWN,
    COMMANDLINE_KEY_UNKNOWN
} CommandLineKey;

typedef enum CommandLineEventKind {
    COMMANDLINE_EVENT_NONE = 0,
    COMMANDLINE_EVENT_KEY,
    COMMANDLINE_EVENT_CLICK,
    COMMANDLINE_EVENT_WHEEL_UP,
    COMMANDLINE_EVENT_WHEEL_DOWN,
    COMMANDLINE_EVENT_RESIZE
} CommandLineEventKind;

// One normalized device event delivered on the owner thread. `key` holds a
// CommandLineKey for special keys or the literal character code for text/control.
typedef struct CommandLineEvent {
    CommandLineEventKind kind;
    int key;      // CommandLineKey or a printable/control code
    int x;        // cell column for pointer events (0 otherwise)
    int y;        // cell row for pointer events (0 otherwise)
} CommandLineEvent;

// Injectable device backend (the Conflict Triage Law managed exception). The
// default backend is ncurses (commandline_ncurses.c); tests inject a fake so the
// owner runs headless. Every fn-pointer is optional: NULL degrades safely.
// `poll` waits at most timeoutMs (the Bounded Wait Law) and returns false when
// no event arrived; it must never block unbounded.
typedef struct CommandLineIo {
    void *ctx;                                             // borrowed backend state
    bool (*open)(void *ctx);                               // raw mode + alt screen + colors
    void (*close)(void *ctx);                              // restore the terminal
    bool (*size)(void *ctx, size_t *outW, size_t *outH);   // current cell geometry
    bool (*present)(void *ctx, const char *cells, size_t width, size_t height); // one flush
    bool (*poll)(void *ctx, int timeoutMs, CommandLineEvent *out); // bounded wait
    bool (*setMouse)(void *ctx, bool enabled);             // pointer reporting toggle
} CommandLineIo;

// Borrowed application seam: the resident host's view/command code. CommandLine
// never owns it; the host keeps it live and excludes callers before replacement.
// `draw` fills the owned grid; `input` handles one normalized event; returning
// false requests a clean stop. `detach` releases host bookkeeping for this app.
typedef struct CommandLineApp {
    void *ctx;                                                             // borrowed host state
    bool (*draw)(void *ctx, CommandLine *line);                            // paint the grid
    bool (*input)(void *ctx, CommandLine *line, const CommandLineEvent *event); // handle one event
    void (*detach)(void *ctx);                                             // optional release
} CommandLineApp;

struct CommandLine {
    char title[COMMANDLINE_MAX_TITLE];  // device title / window label
    CommandLineIo io;                   // injected device backend
    bool explicitIo;                    // false => default (ncurses) backend
    CommandLineApp app;                 // borrowed swappable application seam
    bool hasApp;                        // app seam attached
    char *cells;                        // owned device framebuffer (width*height)
    size_t capacity;                    // admitted grid byte capacity
    size_t width;                       // columns
    size_t height;                      // rows
    uint64_t generation;                // successful app publications
    size_t refusalCount;                // epoch grid-capacity refusals
    _Atomic bool running;               // run loop active
    _Atomic bool cancel;                // stop request
    bool opened;                        // device acquired
    bool active;                        // callback exclusion guard
};

// --- Overloaded constructors (the Window chooser idiom) ---
//   CommandLine()          -> default title "vex"
//   CommandLine("title")   -> named
CommandLine *CommandLine_0(void);
CommandLine *CommandLine_1(const char *title);

#define COMMANDLINE_CHOOSER(_0, _1, NAME, ...) NAME
#define CommandLine(...) COMMANDLINE_CHOOSER( \
    dummy __VA_OPT__(,) __VA_ARGS__, CommandLine_1, CommandLine_0 \
)(__VA_ARGS__)

// Free the CommandLine. Closes the device if still open (the Teardown Order Law).
void CommandLine_free(CommandLine *self);

// --- Device backend injection (cold; before open) ---
// A NULL io clears any explicit backend and restores the default. Rejected
// while open or active.
bool CommandLine_setIo(CommandLine *self, const CommandLineIo *io);
const CommandLineIo *CommandLine_getIo(const CommandLine *self);

// --- Lifecycle ---
// Acquire the device and allocate the owned grid. False when already open, or
// when the backend has no open/size. Never re-acquires an open device.
bool CommandLine_open(CommandLine *self);
// Restore the device and release the grid. No-op when closed. Never blocks.
void CommandLine_close(CommandLine *self);

// THE one resize surface: re-read the device size, grow the grid, re-clamp the
// app and present once. Always honored; callers never defer it. False only when
// the device/grid cannot be satisfied (uncounted) — a resize refusal is counted.
bool CommandLine_revalidate(CommandLine *self);

// Present the owned grid in ONE flush. False when closed or the sink failed; a
// failed sink may have delivered a prefix and cannot be rolled back.
bool CommandLine_present(CommandLine *self);

// --- Application seam (swappable view/command, no terminal reset) ---
// Publish a borrowed app. Pauses+excludes first; rejects (false, preserves the
// old app) on null draw, busy, or exhausted generation. Never resets the device.
bool CommandLine_setApp(CommandLine *self, const CommandLineApp *app);
bool CommandLine_hasApp(const CommandLine *self);
// Detach the app (calls detach once). Safe when none attached.
bool CommandLine_detachApp(CommandLine *self);

// --- Owner-thread loop ---
// One non-blocking pass: bounded poll (0ms), deliver, draw, single present; a
// resize event always revalidates. False when not open/no app or a callback failed.
bool CommandLine_tick(CommandLine *self);
// Blocking terminal-application loop: tick until cancel/stop or a false result.
// Returns the exit code (0 on clean stop). Requires an attached app.
int CommandLine_run(CommandLine *self);
// Thread-safe, idempotent stop request (flips cancel; the loop observes it).
void CommandLine_stop(CommandLine *self);
void CommandLine_requestCancel(CommandLine *self);
bool CommandLine_isRunning(const CommandLine *self);

// --- Geometry / grid ---
size_t CommandLine_getWidth(const CommandLine *self);
size_t CommandLine_getHeight(const CommandLine *self);
size_t CommandLine_getCapacity(const CommandLine *self);
// Borrowed contiguous width*height cell grid; live until close/revalidate.
char *CommandLine_getCells(CommandLine *self);
const char *CommandLine_getCellsConst(const CommandLine *self);
// Fill the grid with blanks. Paints nothing to the device until present.
bool CommandLine_clear(CommandLine *self);

// --- Input ---
// Bounded device poll (waits at most timeoutMs). False when no event arrived or
// the backend is absent; *out is zeroed. Never blocks unbounded.
bool CommandLine_poll(CommandLine *self, int timeoutMs, CommandLineEvent *out);
bool CommandLine_setMouseReporting(CommandLine *self, bool enabled);

// --- Telemetry / identity: symmetric setters & getters ---
void        CommandLine_setTitle(CommandLine *self, const char *title);
const char *CommandLine_getTitle(const CommandLine *self);
uint64_t    CommandLine_getGeneration(const CommandLine *self);
size_t      CommandLine_exhaustionCount(const CommandLine *self);
bool        CommandLine_isOpen(const CommandLine *self);
bool        CommandLine_isActive(const CommandLine *self);
bool        CommandLine_isCancelled(const CommandLine *self);

// --- Projections (the toString Law) ---
bool CommandLine_toString(const CommandLine *self, char *dest, size_t cap, bool *outTruncated);
bool CommandLine_toStringStruct(const CommandLine *self, char *dest, size_t cap, bool *outTruncated);

// The default ncurses backend (commandline_ncurses.c). Returned by value; owns
// no state until opened. Exposed so an application can pre-set mouse policy.
CommandLineIo CommandLine_defaultIo(void);

#endif
