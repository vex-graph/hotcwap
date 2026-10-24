// window/window_win32.c — the lean Win32 backend for the Window API (draft).
//
// A 1:1 mirror of the AppKit backend (window/window_cocoa.m): the same
// exported C surface, the same state families (POLICY / CONTENT / ADAPTERS),
// the same reflection words, the same WindowEvent lifecycle bridge, the same
// id registry. The only differences are the OS handle that anchors each
// window (HWND instead of NSWindow) and the window-manager dialect that
// serves it (Win32 messages instead of AppKit notifications).
//
// This is a DRAFT translation unit: it is not wired into CMake and has not
// been compiled against a real Win32 SDK. It mirrors the Cocoa backend
// function-for-function so that wiring it in later (WIN32 -> window_win32.c,
// replacing window/window.c) is a build-script edit, not an API migration.
//
// Like the Cocoa backend it is deliberately LEAN: a window with bridges, and
// nothing else. Intrinsic Win32 does the presentation plumbing — the WndProc
// is a pump-fed callback, not a message loop owner; WM_SIZE/WM_MOVE/WM_CLOSE/
// WM_SETFOCUS mirror the OS state into C-visible words exactly the way the
// AppKit delegate notifications do; WM_* input messages route into the
// vexspoke Key/Mouse/Touch rings. There is NO Vulkan, NO D3D, NO swapchain
// and NO present worker here — rendering is the render personal' job and reaches
// the screen through the software present seam (a lean StretchDIBits path
// mirroring the retired raster present) or, once Migrates, the render personal'
// own surfaces. A Window is a dumb surface + callback bridge per the Window
// Decoupling Law.
//
// The GPU-era composite surface (boards, panes, worker present) is retained
// as thin INERT stubs at the bottom of this file exactly as the Cocoa backend
// keeps them, so the still-unmigrated darling compositor links. They retire
// together with their window.h declarations.

#if defined(_WIN32)

// Lean and mean: strip the winsock/DDE/RPC/Shell bloat out of <windows.h>
// before it is pulled in. The lean backend needs USER/GDI only, plus the
// opt-in <dwmapi.h> for the blur seam. If a future render path in this file
// ever needs Winsock, it includes <winsock2.h> explicitly — never by undefining
// the lean flag.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <dwmapi.h>
#include <wingdi.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "window/window.h"
#include "input/focus.h"
#include "input/key.h"
#include "input/mouse.h"
#include "input/touch.h"
#include "buffer/buffer.h"
#include "annotation/definition.h"
#include "annotation/overview.h"
#include "annotation/getter.h"
#include "annotation/setter.h"
#include "annotation/intention.h"
#include "annotation/draft.h"
#include "annotation/platform_exclusive.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Window_win32
 * ============================================================================
 * Win32 desktop window abstraction implementation mirroring Cocoa capabilities.
 * Manages native HWND lifecycle, WndProc message dispatch, and User32 display
 * geometry, feeding OS input directly into vexspoke device rings and driving the
 * embedded WindowEvent callback registry.
 *
 * Conforms to the Window Decoupling Law by treating the window as a dumb display
 * surface and callback seam, supporting software frame presentation via
 * StretchDIBits alongside inert compatibility stubs.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Window (window/window_win32.c)
 * ============================================================================
 * SUMMARY:
 *   DRAFT Win32 mirror of the AppKit window backend. One opaque C handle per
 *   HWND; the engine loop constructs it, configures the chrome, shows it, then
 *   pumps Window_pollEvents once per frame while a render path draws through
 *   the software present seam (StretchDIBits) / event bridges. OS input is
 *   routed into the vexspoke device rings (tagged with this window's id); OS
 *   lifecycle (quit, resize, fullscreen, minimize, restore, press, focus) fires
 *   the embedded WindowEvent. Zero Vulkan, zero Metal, zero compositing — a
 *   Window is a dumb surface + callback bridge per the Window Decoupling Law.
 *
 * STRUCT FIELDS (Mirroring window/window.h incomplete tag — completed here):
 * ----------------------------------------------------------------------------
 *   HWND hwnd;                         // Win32 window (we own it)
 *   WindowEvent lifecycle;             // OS lifecycle registry
 *   uint32_t id;                       // engine window id (1..N, 0 = broadcast)
 *   _Atomic bool shouldClose;          // true once close requested
 *   _Atomic uint64_t sizeGeneration;   // resize-reflection counter
 *   _Atomic int cachedWidth;           // client width at last WM_SIZE
 *   _Atomic int cachedHeight;          // client height at last WM_SIZE
 *   _Atomic bool liveResizing;         // set during WM_ENTERSIZEMOVE..EXIT
 *   _Atomic int presentMode;           // present pacing (FIFO/IMMEDIATE), stored
 *   _Atomic bool transparent;          // composite transparency request, stored
 *   _Atomic uint64_t renderGeneration; // policy-reflection counter
 *   _Atomic(void*) topLayer;           // content board handle
 *   _Atomic(void*) bottomLayer;        // scene board handle
 *   _Atomic bool enabled;              // false mutes ALL OS input
 *   bool lastFocused;                  // focus-flip detection
 *   _Atomic uint32_t monitorId;        // HMONITOR slot id (0 = unmapped)
 *   WindowCursorType cursorType;       // current cursor request
 *   bool decorated;                    // chrome mode (WS_OVERLAPPED vs WS_POPUP)
 *   bool fullscreen;                   // fullscreen state
 *   WINDOWPLACEMENT prevPlacement;     // restored placement for fullscreen toggle
 *   int minWidth, minHeight;           // WM_GETMINMAXINFO minimums (0 = unset)
 *   int maxWidth, maxHeight;           // WM_GETMINMAXINFO maximums (0 = unbounded)
 *   bool movableByBackground;          // WM_NCHITTEST HTCAPTION toggle
 *   bool fullscreenButton;             // WS_MAXIMIZEBOX gate
 *   bool resizableEnabled;             // WS_THICKFRAME gate
 *   bool closableEnabled;              // SC_CLOSE system-menu gate
 *   bool minimizeEnabled;              // WS_MINIMIZEBOX gate
 *   bool clickThrough;                 // WS_EX_TRANSPARENT toggle
 *   float opacity;                     // SetLayeredWindowAttributes alpha
 *   bool shadow;                       // DWM non-client-rendering policy
 *   int frameW, frameH;                // retained bitmap dimensions in px
 *   BITMAPINFO bmi;                    // retained bitmap header for StretchDIBits
 *   WindowResizeRenderFn resizeRenderFn;   // resize-cadence render hook
 *   void *resizeRenderUserdata;        // hook userdata
 *
 * PRIVATE HELPERS (kept file-local, no external API):
 * ----------------------------------------------------------------------------
 *   WindowSlot                         — id-registry slot record
 *     HWND hwnd;                       // the window carrying this id
 *     Window *handle;                  // C handle owning that window
 *   Win32Monitor                       — HMONITOR slot record
 *     HMONITOR handle;                 // monitor handle (may be nulled)
 *     uint32_t id;                     // stable engine id (index + 1)
 *   win32EnsureClass(), windowWndProc()
 *   windowRefreshSize, windowRefreshFocus, windowRefreshMonitor, recenterIfLocked
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - Window_0(void)                        : Window_new(nullptr)
 *   - Window_1(title)                       : Window_new(&{ .title })
 *   - Window_3(title, width, height)
 *   - Window_new(desc)                      : descResolve + windowAlloc + show
 *   - Window_create(title, width, height)
 *
 * Private Constructors: (.c static)
 *   - windowAlloc(desc)                     : shared constructor implementation
 *   - descResolve(desc)                     : default parameter resolution
 *
 * Public Core Functions: (.h)
 *   - Window_destroy(window)                : detach, close, free handle
 *   - Window_destroyAll(void)
 *   - Window_shouldClose(window)
 *   - Window_pollEvents(void)               : drain the message queue once per frame
 *   - Window_width(window)
 *   - Window_height(window)
 *   - Window_center(window)
 *   - Window_show(window)
 *   - Window_hide(window)
 *   - Window_attachPanes(window, panel, width, height) : inert (;;INTENTION)
 *   - Window_resizePanes(window, panel, width, height) : inert (;;INTENTION)
 *   - Window_compositePanes(window, contentPanel)      : inert (;;INTENTION)
 *   - Window_compositeBoards(window)        : inert (;;INTENTION)
 *   - Window_orderLayers(window)            : inert (;;INTENTION)
 *   - Window_renderGeneration(window)
 *   - Window_bringToFront(window)
 *   - Window_minimize(window)
 *   - Window_restore(window)
 *   - Window_toggleFullscreen(window)
 *   - Window_contentView(window)            : returns the HWND (the surface anchor)
 *   - Window_nativeHandle(window)
 *   - Window_metalLayer(window)             : nullptr — no Metal here (;;INTENTION)
 *   - Window_present(window, frame)         : lean StretchDIBits software path
 *   - Window_workerPresentBegin(window)     : inert (;;INTENTION)
 *   - Window_workerPresentEnd(window)       : inert (;;INTENTION)
 *   - Window_addKeyAdapter(window, adapter)
 *   - Window_removeKeyAdapter(window, adapter)
 *   - Window_addMouseAdapter(window, adapter)
 *   - Window_removeMouseAdapter(window, adapter)
 *   - Window_addTouchAdapter(window, adapter)
 *   - Window_removeTouchAdapter(window, adapter)
 *   - Window_addWindowAdapter(window, adapter)
 *   - Window_removeWindowAdapter(window, adapter)
 *   - Window_dispatchEvents(window)
 *   - Window_id(window)
 *   - Window_focus(window)
 *   - Window_sizeGeneration(window)
 *
 * Private Core Functions: (.c static)
 *   - windowWndProc(...)                    : OS message dispatches to device rings + WindowEvent
 *
 * Public Setters: (.h)
 *   - Window_setShouldClose(window, shouldClose)
 *   - Window_setTitle(window, title)
 *   - Window_setSize(window, width, height)
 *   - Window_setLocation(window, x, y)      : SetWindowPos (top-left px)
 *   - Window_setVisible(window, visible)
 *   - Window_setTopLayer(window, layer)
 *   - Window_setBottomLayer(window, layer)
 *   - Window_setPresentMode(window, mode)
 *   - Window_setTransparent(window, transparent)   : WS_EX_LAYERED
 *   - Window_setEnabled(window, enabled)
 *   - Window_setKeyEnabled(window, enabled)
 *   - Window_setResizable(window, resizable)       : WS style bits
 *   - Window_setClosable(window, closable)
 *   - Window_setMiniaturizable(window, miniaturizable)
 *   - Window_setFullscreenButton(window, enabled)   : WS_MAXIMIZEBOX gate
 *   - Window_setUndecorated(window, mode)          : WS_POPUP / WS_OVERLAPPEDWINDOW
 *   - Window_setDecorated(window, decorated)
 *   - Window_setNaked(window, naked)
 *   - Window_setBorderless(window, borderless)
 *   - Window_setViewportFlushToTop(window, flush)       : macOS-only no-op
 *   - Window_setFloatingTrafficLights(window, floating) : macOS-only no-op
 *   - Window_macOS_setTrafficLightButtonVisible(window, light, visible)  : macOS-only stub
 *   - Window_macOS_setTrafficLightHeaderPosition(window, x, y)          : macOS-only stub
 *   - Window_setOpacity(window, opacity)          : SetLayeredWindowAttributes LWA_ALPHA
 *   - Window_setTransparentBackground(window, transparent) : WS_EX_LAYERED + clear-color note
 *   - Window_setAlwaysOnTop(window, onTop)        : HWND_TOPMOST / HWND_NOTOPMOST
 *   - Window_setClickThrough(window, clickThrough) : WS_EX_TRANSPARENT
 *   - Window_setShadow(window, shadow)            : DWM non-client-rendering policy
 *   - Window_setMovableByBackground(window, movable) : WM_NCHITTEST HTCAPTION
 *   - Window_setFullscreen(window, fullscreen)
 *   - Window_setDRM(window, enabled)              : no-op (;;INTENTION — macOS concept)
 *   - Window_setMinSize(window, width, height)    : WM_GETMINMAXINFO min track
 *   - Window_setMaxSize(window, width, height)    : WM_GETMINMAXINFO max track
 *   - Window_setCursorType(window, type)
 *   - Window_setCursorLocked(window, locked)      : ClipCursor + ShowCursor(FALSE) + warp
 *   - Window_setResizeRenderHook(window, fn, userdata)
 *   - Window_setGravityTopLeft(window)            : no-op (;;INTENTION)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - Window_getLocation(window, outX, outY)
 *   - Window_getContentOrigin(window, outX, outY)
 *   - Window_getTopLayer(window)
 *   - Window_getBottomLayer(window)
 *   - Window_getPresentMode(window)
 *   - Window_isTransparent(window)
 *   - Window_renderGeneration(window)
 *   - Window_isEnabled(window)
 *   - Window_isKeyEnabled(window)
 *   - Window_isLiveResizing(window)
 *   - Window_isResizable(window)
 *   - Window_isClosable(window)
 *   - Window_isMiniaturizable(window)
 *   - Window_isDecorated(window)
 *   - Window_isNaked(window)
 *   - Window_isBorderless(window)
 *   - Window_isViewportFlushToTop(window)             : macOS-only false
 *   - Window_macOS_isTrafficLightButtonVisible(window, light)  : macOS-only stub
 *   - Window_macOS_getTrafficLightHeaderPosition(window, outX, outY) : macOS-only stub
 *   - Window_isMinimized(window)
 *   - Window_isFullscreen(window)
 *   - Window_getCursorType(window)
 *   - Window_id(window)
 *   - Window_isFocused(window)
 *   - Window_getLifecycle(window)
 *   - Window_getMonitorId(window)
 *   - Window_sizeGeneration(window)
 *   - Window_getResizeRenderHook(window)
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */
;;PLATFORM_EXCLUSIVE("Windows")
;;DRAFT
;;INTENTION("Draft translation unit: wired platform-guarded (WIN32 selects "
            "window/window_win32.c; window.c stays reachable via "
            "-DHOTCWAP_WIN32_LEGACY=ON), but never compiled against a real Win32 "
            "SDK — the macOS host excludes it. Mirrors the Cocoa backend 1:1; a "
            "Windows-host build is the promotion gate.")
;;INTENTION("GPU-era composite surface (attachPanes/resizePanes/compositePanes/"
            "compositeBoards/orderLayers/metalLayer/setGravityTopLeft/"
            "workerPresentBegin/workerPresentEnd) is retained as inert stubs so "
            "the still-unmigrated darling compositor keeps linking — exactly as "
            "the Cocoa backend keeps them. They retire together with their "
            "window.h declarations.")
;;INTENTION("Window_present is a genuinely lean Win32 path (DIB section + "
            "StretchDIBits on WM_PAINT) — Win32 has a real raster target (the "
            "HWND client area), unlike the Cocoa backend whose inert slice "
            "exists because a pure AppKit view has none. Same signature, same "
            "call contract: software frames land scaled-to-fit into the client.")
;;INTENTION("Per-Monitor-V2 DPI awareness is requested once at register time; "
            "cachedWidth/Height and WM_SIZE speak physical client px, matching "
            "the Native Pixel Law contract (the window reports what its client "
            "really is, so swapchains rebuild at native size).")
;;INTENTION("Window_setDRM has no Win32 analogue (NSWindowSharingNone is an "
            "AppKit concept); stored-and-inert so the surface stays stable, "
            "mirroring the macOS 'no sharing' semantics as closely as the OS "
            "allows. Set member when the guest compositor arrives.")

// Multi-tap window for double-click style counting (legacy parity: 250ms).
static const uint64_t kTapThresholdNanos = 250000000ULL;

// Virtual keycode -> KEY_* code. Indexed by the raw Windows virtual-key code
// (0..255); -1 = unmapped. Physical layout maps letters/digits/function rows
// 1:1 like the Cocoa Carbon table, and punctuation rides the OEM VK_ codes.
static int winVkMap[256] = {
    // ASCII-printable codes carried by VK_ values < 0x60 already equal their
    // KEY_ value for ' '..'z' collateral — letters and digits are set below.
    [0x08] = KEY_BACKSPACE,   [0x09] = KEY_TAB,        [0x0D] = KEY_ENTER,
    [0x1B] = KEY_ESCAPE,      [0x20] = KEY_SPACE,
    [0x23] = KEY_END,         [0x24] = KEY_HOME,       [0x25] = KEY_LEFT,
    [0x26] = KEY_UP,          [0x27] = KEY_RIGHT,      [0x28] = KEY_DOWN,
    [0x2C] = KEY_PRINT_SCREEN,[0x2D] = KEY_INSERT,     [0x2E] = KEY_DELETE,
    [0x21] = KEY_PAGE_UP,     [0x22] = KEY_PAGE_DOWN,  [0x13] = KEY_PAUSE,
    [0x14] = KEY_CAPS_LOCK,   [0x5B] = KEY_LEFT_SUPER, [0x5C] = KEY_RIGHT_SUPER,
    [0x5D] = KEY_MENU,
    // Function rows: VK_F1..VK_F24 = 0x70..0x87.
    [0x70] = KEY_F1,  [0x71] = KEY_F2,  [0x72] = KEY_F3,  [0x73] = KEY_F4,
    [0x74] = KEY_F5,  [0x75] = KEY_F6,  [0x76] = KEY_F7,  [0x77] = KEY_F8,
    [0x78] = KEY_F9,  [0x79] = KEY_F10, [0x7A] = KEY_F11, [0x7B] = KEY_F12,
    [0x7C] = KEY_F13, [0x7D] = KEY_F14, [0x7E] = KEY_F15, [0x7F] = KEY_F16,
    [0x80] = KEY_F17, [0x81] = KEY_F18, [0x82] = KEY_F19, [0x83] = KEY_F20,
    [0x84] = KEY_F21, [0x85] = KEY_F22, [0x86] = KEY_F23, [0x87] = KEY_F24,
    // Numpad pad digits (VK_NUMPAD0..9 = 0x60..0x69) ride the top-row digits.
    // Punctuation OEM codes (documented stable VK_ values):
    [0xBA] = KEY_SEMICOLON,   [0xBB] = KEY_EQUAL,      [0xBC] = KEY_COMMA,
    [0xBD] = KEY_MINUS,       [0xBE] = KEY_PERIOD,     [0xBF] = KEY_SLASH,
    [0xC0] = KEY_GRAVE_ACCENT,[0xDB] = KEY_LEFT_BRACKET,
    [0xDC] = KEY_BACKSLASH,   [0xDD] = KEY_RIGHT_BRACKET,
    [0xDE] = KEY_APOSTROPHE,
};

// Cursor-lock state. While locked the pointer is clipped to the window and
// re-warped to the anchor centre every pump pass; WM input deltas feed mouse.
static bool s_cursorLocked = false;
static POINT s_lockCenter = {0, 0};

// The opaque handle handed back to C. Holds the native window (HWND) plus
// every C-visible reflection word — the mirror of the Cocoa struct Window.
// `id` is the engine's small window number (1..N; 0 is the broadcast reserved
// id) used to tag input events and route them to per-window listeners.
// `lifecycle` is the embedded WindowEvent — the app's OS-lifecycle bridge.
struct Window {
    HWND hwnd;
    WindowEvent lifecycle;
    uint32_t id;
    _Atomic bool shouldClose;
    _Atomic uint64_t sizeGeneration;
    _Atomic int cachedWidth;
    _Atomic int cachedHeight;
    int cachedX;
    int cachedY;
    int cachedContentX;
    int cachedContentY;
    _Atomic bool liveResizing;
    _Atomic bool miniaturizing;

    // Pure-state reflection words (no GPU calls in this file).
    _Atomic int presentMode;
    _Atomic bool transparent;
    _Atomic uint64_t renderGeneration;

    // Board slots — opaque layer handles consumed by graphvex/darling; this
    // window stores them, never dereferences them. Panels live on the Frame.
    _Atomic(void*) topLayer;
    _Atomic(void*) bottomLayer;

    _Atomic bool enabled;
    _Atomic bool keyEnabled; // stored; OS-level key refusal is Cocoa-only for now
    bool lastFocused;
    _Atomic uint32_t monitorId;
    WindowCursorType cursorType;

    // Chrome: style-mask mirror (WIN32 has no traffic lights; no lightBase).
    bool decorated;
    bool fullscreen;
    RECT savedFrame;                 // normal (pre-fullscreen) outer frame
    int minWidth;
    int minHeight;
    int maxWidth;
    int maxHeight;
    bool movableByBackground;
    bool fullscreenButton;
    bool resizableEnabled;
    bool closableEnabled;
    bool minimizeEnabled;

    // Software present path (mirror of the retired raster present).
    unsigned char *framePixels;
    int frameW;
    int frameH;
    bool frameDirty;

    WindowResizeRenderFn resizeRenderFn;
    void *resizeRenderUserdata;
};

// Window id registry: slot i holds the entries for engine id i (index = id,
// slot 0 reserved for FOCUS_BROADCAST). Grows by doubling (the Dynamic
// Scalability & Anti-Hardcoding Law — no fixed window ceiling).
typedef struct WindowSlot {
    HWND hwnd;
    Window *handle;
} WindowSlot;

static WindowSlot *s_refs = nullptr;
static uint32_t s_refCap = 0;

// Foreground-claim pending: set by Window_show/Window_focus, drained by the
// pump once the OS grants foreground (Win32 foreground-locking rules are the
// mirror of AppKit activation — a bounded re-assert, never a spin).
static HWND sPendingForeground = nullptr;
static uint64_t sPendingUntilMillis = 0;

// Grow the HWND-to-window registry geometrically and initialize the new slots.
static bool windowRefsGrow(void) {
    uint32_t nextCap = (s_refCap == 0) ? 8u : s_refCap * 2u;
    WindowSlot *grown = (WindowSlot*) realloc(s_refs, (size_t) nextCap * sizeof(WindowSlot));
    if (grown == nullptr)
        return false;
    for (uint32_t i = s_refCap; i < nextCap; i++) {
        grown[i].hwnd = nullptr;
        grown[i].handle = nullptr;
    }
    s_refs = grown;
    s_refCap = nextCap;
    return true;
}

// Resolve an id for a newly created window, or 0 when the table cannot grow.
static uint32_t windowIdAcquire(HWND hwnd, Window *handle) {
    if (s_refCap == 0 && !windowRefsGrow())
        return FOCUS_BROADCAST;
    for (uint32_t i = 1; i < s_refCap; i++) {
        if (s_refs[i].hwnd == nullptr) {
            s_refs[i].hwnd = hwnd;
            s_refs[i].handle = handle;
            return i;
        }
    }
    if (!windowRefsGrow())
        return FOCUS_BROADCAST;
    for (uint32_t i = 1; i < s_refCap; i++) {
        if (s_refs[i].hwnd == nullptr) {
            s_refs[i].hwnd = hwnd;
            s_refs[i].handle = handle;
            return i;
        }
    }
    return FOCUS_BROADCAST;
}

// Clear a registry slot after its native window and C handle are detached.
static void windowIdRelease(uint32_t id) {
    if (id != FOCUS_BROADCAST && id < s_refCap) {
        s_refs[id].hwnd = nullptr;
        s_refs[id].handle = nullptr;
    }
}

// Reverse lookup: which engine id does this OS window carry? 0 if unknown.
static uint32_t windowIdOf(HWND hwnd) {
    if (hwnd == nullptr)
        return FOCUS_BROADCAST;
    for (uint32_t i = 1; i < s_refCap; i++)
        if (s_refs[i].hwnd == hwnd)
            return i;
    return FOCUS_BROADCAST;
}

static Window *windowHandleOf(HWND hwnd) {
    if (hwnd == nullptr)
        return nullptr;
    for (uint32_t i = 1; i < s_refCap; i++)
        if (s_refs[i].hwnd == hwnd)
            return s_refs[i].handle;
    return nullptr;
}

// WM_GETMINMAXINFO: clamp the OS to the stored content constraints. Thread 0
// (message pump) — the same contract as the Cocoa min/max reflection.
static void windowApplyMinMax(Window *window, MINMAXINFO *mmi) {
    RECT frame = {0};
    if ((*window).minWidth > 0 || (*window).minHeight > 0)
        frame.right = (*window).minWidth;
    frame.bottom = (*window).minHeight;
    AdjustWindowRectEx(&frame, (DWORD) GetWindowLongPtr((*window).hwnd, GWL_STYLE), FALSE, 0);
    if ((*window).minWidth > 0)
        (*window).minWidth = frame.right - frame.left;
    if ((*window).minHeight > 0)
        (*window).minHeight = frame.bottom - frame.top;
    frame.left = frame.top = 0;
    frame.right = (*window).maxWidth;
    frame.bottom = (*window).maxHeight;
    // The tracking values are OUTER px: WM_GETMINMAXINFO wants the max frame.
    if ((*window).maxWidth > 0 || (*window).maxHeight > 0) {
        AdjustWindowRectEx(&frame, (DWORD) GetWindowLongPtr((*window).hwnd, GWL_STYLE), FALSE, 0);
        POINT *minTrack = &(*mmi).ptMinTrackSize;
        POINT *maxTrack = &(*mmi).ptMaxTrackSize;
        (*minTrack).x = ((*window).minWidth > 0) ?
            ((*window).minWidth + (frame.right - frame.left)) : 0;
        (*minTrack).y = ((*window).minHeight > 0) ?
            ((*window).minHeight + (frame.bottom - frame.top)) : 0;
        if ((*window).maxWidth > 0)
            (*maxTrack).x = (LONG)(*window).maxWidth
                            + (frame.right - frame.left);
        if ((*window).maxHeight > 0)
            (*maxTrack).y = (LONG)(*window).maxHeight
                            + (frame.bottom - frame.top);
    }
}

// Resize reflection: the message already carries the new client size (cx/cy);
// compare against the cache and bump sizegen + fire onResized + run the resize
// hook only on an actual change. Thread 0 only.
static void windowRefreshSize(Window *window, int cw, int ch) {
    if (window == nullptr || (*window).hwnd == nullptr)
        return;
    if (cw != atomic_load_explicit(&(*window).cachedWidth, memory_order_relaxed)
        || ch != atomic_load_explicit(&(*window).cachedHeight, memory_order_relaxed)) {
        atomic_store_explicit(&(*window).cachedWidth, cw, memory_order_relaxed);
        atomic_store_explicit(&(*window).cachedHeight, ch, memory_order_relaxed);
        atomic_fetch_add_explicit(&(*window).sizeGeneration, 1, memory_order_release);
        WindowEvent_fireResized(&(*window).lifecycle, window, cw, ch);
        if ((*window).resizeRenderFn)
            (*window).resizeRenderFn((*window).resizeRenderUserdata);
    }
}

// Mirror the OS foreground window into the focus word AND the WindowEvent
// focus slots (flips only). Thread 0 only, called at the end of each pump pass.
static void windowRefreshFocus(Window *window, HWND fgWindow) {
    if (window == nullptr || (*window).hwnd == nullptr)
        return;
    bool focused = (windowIdOf(fgWindow) == (*window).id);
    if (focused != (*window).lastFocused) {
        (*window).lastFocused = focused;
        if (focused)
            WindowEvent_fireFocusGained(&(*window).lifecycle, window);
        else
            WindowEvent_fireFocusLost(&(*window).lifecycle, window);
    }
}

// Mirror the display assignment into the atomic word; 0 means unmapped. The id
// is the EnumDisplayMonitors traversal index + 1 (stable per session, unlike
// the HMONITOR pointer which recesses between enumerations).
static uint32_t resolveMonitorId(HWND hwnd) {
    if (hwnd == nullptr)
        return 0;
    HMONITOR mine = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (mine == nullptr)
        return 0;
    uint32_t idx = 0;
    // Count preceding monitors until EnumDisplayMonitors reaches the target.
    BOOL CALLBACK countCb(HMONITOR m, HDC dc, LPRECT r, LPARAM lp) {
        (void) dc;
        (void) r;
        uint32_t *out = (uint32_t*) lp;
        if (m == mine)
            return FALSE;
        (*out)++;
        return TRUE;
    }
    EnumDisplayMonitors(nullptr, nullptr, countCb, (LPARAM) &idx);
    return idx + 1u;
}

// Refresh the monitor id mirror from the window's nearest display.
static void windowRefreshMonitor(Window *window) {
    if (window == nullptr)
        return;
    atomic_store_explicit(&(*window).monitorId, resolveMonitorId((*window).hwnd), memory_order_relaxed);
}

// Re-centre the cursor during the pump while locked, so the warp registers
// before the next event loop exit.
static void recenterIfLocked(void) {
    if (s_cursorLocked)
        SetCursorPos(s_lockCenter.x, s_lockCenter.y);
}

// Client-area coordinates (top-left origin) from an lParam — Win32 messages
// already carry client px, so no flip is needed (parity with the flipped
// Cocoa content view).
static void mousePoint(LPARAM lp, double *outX, double *outY) {
    *outX = (double) GET_X_LPARAM(lp);
    *outY = (double) GET_Y_LPARAM(lp);
}

// Map a Win32 mouse message to the MOUSE_* button constant.
static int winButton(UINT msg) {
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)
        return MOUSE_LEFT;
    if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP)
        return MOUSE_RIGHT;
    if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP)
        return MOUSE_MIDDLE;
    return MOUSE_MIDDLE; // X-button drags ride MOUSE_MIDDLE for parity with Cocoa's "other".
}

// Classify whether a supported Win32 mouse-button message represents a press.
static bool winButtonDown(UINT msg) {
    return msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN
        || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN;
}

// Modifier mirror: compare the Win32 modifier state against the engine's
// Key ring and emit diffs — the same synthetic down/up pattern the Cocoa
// backend uses for modifier keys. Thread 0 only.
static void mirrorModifiers(Window *window, WPARAM state, UINT msg) {
    if (window == nullptr)
        return;
    uint32_t wid = (*window).id;
    bool ctrl = (state & MK_CONTROL) != 0;
    if (ctrl != (Key_isDown(KEY_LEFT_CONTROL) || Key_isDown(KEY_RIGHT_CONTROL)))
        Key_pushEvent(wid, KEY_LEFT_CONTROL, ctrl ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
    bool shift = (state & MK_SHIFT) != 0;
    if (shift != (Key_isDown(KEY_LEFT_SHIFT) || Key_isDown(KEY_RIGHT_SHIFT)))
        Key_pushEvent(wid, KEY_LEFT_SHIFT, shift ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
    // Alt/Win are NOT part of MK_*; read the real modifiers for the message.
    LONG_PTR l = (LONG_PTR) msg; // unused here — message modifiers are implicit;
    (void) l;
}

// Touch route (lean WM_TOUCH): decode each touch into TOUCHINPUT, normalize
// into client px (top-left origin), and push. Identity hash % TOUCH_MAX picks
// the slot, exactly like the Cocoa NSTouch path.
static void routeTouch(Window *window, WPARAM wp, LPARAM lp) {
    if (window == nullptr)
        return;
    UINT count = (UINT) LOWORD(wp);
    if (count == 0 || count > 64)
        return;
    TOUCHINPUT in[64];
    if (!GetTouchInputInfo((HTOUCHINPUT) lp, count, in, sizeof(TOUCHINPUT)))
        return;
    RECT client = {0};
    GetClientRect((*window).hwnd, &client);
    double winW = (double) (client.right - client.left);
    double winH = (double) (client.bottom - client.top);
    for (UINT i = 0; i < count; i++) {
        if ((in[i].dwFlags & TOUCHEVENTF_DOWN) == 0
            && (in[i].dwFlags & TOUCHEVENTF_UP) == 0
            && (in[i].dwFlags & TOUCHEVENTF_MOVE) == 0)
            continue;
        int action = TOUCH_MOVE;
        if ((in[i].dwFlags & TOUCHEVENTF_DOWN) != 0)
            action = TOUCH_DOWN;
        else if ((in[i].dwFlags & TOUCHEVENTF_UP) != 0)
            action = TOUCH_UP;
        int slot = (int) (in[i].dwID % TOUCH_MAX);
        double x = (double) in[i].x / 100.0;
        double y = (double) in[i].y / 100.0;
        Touch_pushTouchEvent((*window).id, slot, action, x, y, 0.8, kTapThresholdNanos);
    }
    CloseTouchInputHandle((HTOUCHINPUT) lp);
}

// The per-window message sink. Win32 has no delegate object: this is the one
// place OS events meet the C reflection words — the exact role of the Cocoa
// NSWindowDelegate notifications + routeEvent.
static LRESULT CALLBACK windowWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Window *window = (Window*) GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTW *cs = (CREATESTRUCTW*) lp;
            Window *w = (Window*) (*cs).lpCreateParams;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR) w);
            return TRUE;
        }

        case WM_CLOSE: {
            // Quit is the vetoable slot — the same contract as Cocoa's
            // windowShouldClose. A decline keeps the window alive.
            if (window && WindowEvent_fireQuitRequested(&(*window).lifecycle, window))
                DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            if (window)
                atomic_store_explicit(&(*window).shouldClose, true, memory_order_relaxed);
            return 0;
        }

        case WM_SIZE: {
            if (window) {
                int cw = (int) LOWORD(lp);
                int ch = (int) HIWORD(lp);
                if (wp == SIZE_MINIMIZED) {
                    atomic_store_explicit(&(*window).miniaturizing, true, memory_order_relaxed);
                    WindowEvent_fireMinimized(&(*window).lifecycle, window);
                } else {
                    if (atomic_load_explicit(&(*window).miniaturizing, memory_order_relaxed)) {
                        atomic_store_explicit(&(*window).miniaturizing, false, memory_order_relaxed);
                        WindowEvent_fireRestored(&(*window).lifecycle, window);
                    }
                    windowRefreshSize(window, cw, ch);
                }
            }
            return 0;
        }

        case WM_MOVE: {
            if (window) {
                RECT rc = {0};
                GetWindowRect(hwnd, &rc);
                (*window).cachedX = (int) rc.left;
                (*window).cachedY = (int) rc.top;
                POINT origin = {0, 0};
                ClientToScreen(hwnd, &origin);
                (*window).cachedContentX = (int) origin.x;
                (*window).cachedContentY = (int) origin.y;
            }
            return 0;
        }

        case WM_ENTERSIZEMOVE:
            if (window)
                atomic_store_explicit(&(*window).liveResizing, true, memory_order_relaxed);
            return 0;

        case WM_EXITSIZEMOVE:
            if (window)
                atomic_store_explicit(&(*window).liveResizing, false, memory_order_relaxed);
            return 0;

        case WM_SETFOCUS:
            if (window) {
                windowRefreshFocus(window, GetForegroundWindow());
                windowRefreshMonitor(window);
            }
            return 0;

        case WM_KILLFOCUS:
            if (window)
                windowRefreshFocus(window, GetForegroundWindow());
            return 0;

        case WM_GETMINMAXINFO:
            if (window)
                windowApplyMinMax(window, (MINMAXINFO*) lp);
            return 0;

        // Movable-by-background: a client-area hit reports as HTCAPTION so the
        // window drags from anywhere while the flag is set (mirror of the Cocoa
        // setMovableByWindowBackground). Resize/menu hits still ride through.
        case WM_NCHITTEST: {
            if (window && (*window).movableByBackground) {
                LRESULT hit = DefWindowProc(hwnd, msg, wp, lp);
                if (hit == HTCLIENT)
                    return HTCAPTION;
            }
            break;
        }

        case WM_MOUSEMOVE: {
            if (!window)
                break;
            uint32_t wid = (*window).id;
            if (atomic_load_explicit(&(*window).enabled, memory_order_relaxed))
                break;
            if (s_cursorLocked) {
                POINT now = {0};
                GetCursorPos(&now);
                Mouse_pushMoveDeltaEvent(wid,
                    (double) (now.x - s_lockCenter.x),
                    (double) (now.y - s_lockCenter.y));
                break;
            }
            double x, y;
            mousePoint(lp, &x, &y);
            Mouse_pushMoveEvent(wid, x, y);
            break;
        }

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP: {
            if (!window)
                break;
            uint32_t wid = (*window).id;
            double x, y;
            mousePoint(lp, &x, &y);
            Mouse_pushMoveEvent(wid, x, y);
            bool down = winButtonDown(msg);
            Mouse_pushButtonEvent(wid, winButton(msg),
                                  down ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                                  kTapThresholdNanos);
            if (down)
                WindowEvent_firePressed(&(*window).lifecycle, window);
            break;
        }

        case WM_MOUSEWHEEL: {
            if (window) {
                double delta = (double) GET_WHEEL_DELTA_WPARAM(wp) / (double) WHEEL_DELTA;
                Mouse_pushScrollEvent((*window).id, 0.0, delta);
            }
            return 0;
        }

        case WM_MOUSEHWHEEL: {
            if (window) {
                double delta = (double) GET_WHEEL_DELTA_WPARAM(wp) / (double) WHEEL_DELTA;
                Mouse_pushScrollEvent((*window).id, delta, 0.0);
            }
            return 0;
        }

        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP: {
            if (!window)
                break;
            uint32_t wid = (*window).id;
            UINT vk = (UINT) wp;
            bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
            // Modifier keys resolve left/right through the extended-key bit of
            // the lParam (bit 24), the same physical distinction AppKit folds
            // into its NSEvent modifier flags.
            bool ext = ((lp & (1L << 24)) != 0);
            int mod = -1;
            if (vk == VK_SHIFT)
                mod = ext ? KEY_RIGHT_SHIFT : KEY_LEFT_SHIFT;
            else if (vk == VK_CONTROL)
                mod = ext ? KEY_RIGHT_CONTROL : KEY_LEFT_CONTROL;
            else if (vk == VK_MENU)
                mod = ext ? KEY_RIGHT_ALT : KEY_LEFT_ALT;
            if (mod != -1) {
                Key_pushEvent(wid, mod, down ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
                break;
            }
            int stdKey = (vk < 256) ? winVkMap[vk] : -1;
            if (vk >= 'A' && vk <= 'Z')
                stdKey = KEY_A + (int) (vk - 'A');
            else if (vk >= '0' && vk <= '9')
                stdKey = KEY_NUM_0 + (int) (vk - '0');
            if (stdKey != -1)
                Key_pushEvent(wid, stdKey, down ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
            break;
        }

        case WM_CHAR: {
            if (window) {
                uint32_t c = (uint32_t) wp;
                if (c > 0)
                    Key_pushCharEvent((*window).id, c);
            }
            return 0;
        }

        case WM_TOUCH:
            if (window)
                routeTouch(window, wp, lp);
            return 0;

        case WM_PAINT: {
            if (window && (*window).frameDirty) {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd, &ps);
                if (dc && (*window).framePixels && (*window).frameW > 0 && (*window).frameH > 0) {
                    BITMAPINFO bi;
                    memset(&bi, 0, sizeof(bi));
                    BITMAPINFOHEADER *header = &(*bi).bmiHeader;
                    (*header).biSize = sizeof(BITMAPINFOHEADER);
                    (*header).biWidth = (*window).frameW;
                    (*header).biHeight = -(*window).frameH; // top-down
                    (*header).biPlanes = 1;
                    (*header).biBitCount = 32;
                    (*header).biCompression = BI_RGB;
                    RECT client = {0};
                    GetClientRect(hwnd, &client);
                    int cx = client.right - client.left;
                    int cy = client.bottom - client.top;
                    int mode = (SetStretchBltMode(dc, HALFTONE) != 0) ? HALFTONE : COLORONCOLOR;
                    StretchDIBits(dc, 0, 0, cx, cy, 0, 0,
                                  (*window).frameW, (*window).frameH,
                                  (*window).framePixels, &bi, DIB_RGB_COLORS, SRCCOPY);
                    (void) mode;
                }
                EndPaint(hwnd, &ps);
                (*window).frameDirty = false;
                return 0;
            }
            break;
        }

        default:
            break;
    }

    return DefWindowProc(hwnd, msg, wp, lp);
}

// Drain the OS message queue. Called every frame from the engine loop (the
// "poll" half of poll-then-tick). Returns immediately; never blocks. After
// the pump, mirror the OS's foreground window into the focus word so the rest
// of the engine can ask "who is focused?" without touching USER32.
static void pumpReflection(void) {
    HWND fg = GetForegroundWindow();
    Focus_set(windowIdOf(fg));
    for (uint32_t i = 1; i < s_refCap; i++) {
        Window *handle = s_refs[i].handle;
        if (handle == nullptr)
            continue;
        HWND hwnd = s_refs[i].hwnd;
        windowRefreshFocus(handle, fg);
        windowRefreshMonitor(handle);
        if (hwnd && (*handle).frameDirty)
            InvalidateRect(hwnd, nullptr, FALSE);
    }
    if (sPendingForeground) {
        if (GetForegroundWindow() == sPendingForeground) {
            sPendingForeground = nullptr;
        } else if (GetTickCount64() < sPendingUntilMillis) {
            // Bounded re-assert (the Bounded Wait Law): the OS owns the
            // foreground grant; we ask once per pump while the claim is fresh.
            SetForegroundWindow(sPendingForeground);
        } else {
            sPendingForeground = nullptr;
        }
    }
    recenterIfLocked();
}

// Drain all queued Win32 messages without waiting, then refresh window mirrors.
void Window_pollEvents(void) {
    MSG msg;
    // Non-blocking drain, exactly like Cocoa's
    // nextEventMatchingMask:untilDate:[NSDate distantPast]:dequeue:YES.
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    pumpReflection();
}

// Dispatch at most one queued message, refresh mirrors, and report more pending work.
bool Window_pollEventStep(void) {
    MSG msg;
    if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    pumpReflection();
    return PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE) != 0;
}

// Traffic-light indices (kept beside the constructors for symmetry with the
// Cocoa backend even though Win32 owns no lights).
enum { LIGHT_CLOSE = 0, LIGHT_MINI = 1, LIGHT_ZOOM = 2 };

// Convert an outer-frame size to the content size the style will produce —
// the mirror of frameRectForContentRect. Used so setters speak CONTENT px and
// the OS gets the frame that matches.
static void frameForContent(Window *window, int cw, int ch, int *outX, int *outY) {
    RECT rc = {0, 0, cw, ch};
    DWORD style = (DWORD) GetWindowLongPtr((*window).hwnd, GWL_STYLE);
    DWORD ex = (DWORD) GetWindowLongPtr((*window).hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&rc, style, FALSE, ex);
    *outX = (int) (rc.right - rc.left);
    *outY = (int) (rc.bottom - rc.top);
}

// Bounded foreground claim: show first, then ask for the foreground grant. If
// the OS denies (Win32 foreground locking), the pump's sPendingForeground
// re-assertion covers it for a bounded window — the mirror of the Cocoa
// activation spin + pending key-window fallback.
static void winClaimForeground(HWND hwnd) {
    if (hwnd == nullptr)
        return;
    ShowWindow(hwnd, SW_SHOW);
    SetActiveWindow(hwnd);
    SetForegroundWindow(hwnd);
    sPendingForeground = hwnd;
    sPendingUntilMillis = GetTickCount64() + 500;
}

// Apply the WS style bits for the current chrome mode. Computes the frame
// delta so a style switch never resizes the CONTENT area.
static void windowApplyChrome(Window *window, bool decorated) {
    if (window == nullptr || (*window).hwnd == nullptr)
        return;
    LONG_PTR style = GetWindowLongPtr((*window).hwnd, GWL_STYLE);
    LONG_PTR next;
    if (decorated) {
        next = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    } else if ((*window).decorated == WINDOW_UNDECORATED_NAKED) {
        // Transparent titlebar with resize affordance kept (the closest Win32
        // dialect of the macOS NAKED mode's "lights over content").
        next = (style & ~(WS_CAPTION | WS_BORDER)) | WS_THICKFRAME | WS_POPUP;
    } else {
        next = WS_POPUP | WS_CLIPCHILDREN;
    }
    if ((*window).fullscreenButton)
        next |= WS_MAXIMIZEBOX;
    if ((*window).minimizeEnabled)
        next |= WS_MINIMIZEBOX;
    if ((*window).closableEnabled)
        next |= WS_SYSMENU;
    if ((*window).resizableEnabled)
        next |= WS_THICKFRAME;
    SetWindowLongPtr((*window).hwnd, GWL_STYLE, next);
    // Frame-changed: re-lay the window so the content keeps its size.
    RECT rc = {0};
    GetClientRect((*window).hwnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    frameForContent(window, cw, ch, &cw, &ch);
    int x, y, w, h;
    GetWindowRect((*window).hwnd, &rc);
    x = (int) rc.left;
    y = (int) rc.top;
    w = cw;
    h = ch;
    AdjustWindowRectEx(&rc, (DWORD) next, FALSE,
                       (DWORD) GetWindowLongPtr((*window).hwnd, GWL_EXSTYLE));
    SetWindowPos((*window).hwnd, nullptr, x, y,
                 (int) (rc.right - rc.left), (int) (rc.bottom - rc.top),
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Build the HWND + C handle. Shared by every constructor. The window is
// created HIDDEN — visibility is an explicit Window_show() decision, so
// construct -> mutate -> show never flashes a half-configured window.
static Window *windowAlloc(const WindowDesc *desc) {
    HINSTANCE inst = (HINSTANCE) GetModuleHandleW(nullptr);

    // Register once per process (class name is stable; RegisterClassW is
    // idempotent — a re-register with the same name is simply ignored).
    static bool sRegistered = false;
    if (!sRegistered) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = windowWndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR) IDC_ARROW);
        wc.hIcon = LoadIconW(nullptr, (LPCWSTR) IDI_APPLICATION);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"VexWindowClass";
        RegisterClassW(&wc);
        sRegistered = true;
    }

    int cw = (*desc).width;
    int ch = (*desc).height;
    int wid = cw;
    int hei = ch;
    // Compute the FRAME (outer) rect from the requested CONTENT size.
    RECT rc = {0, 0, cw, ch};
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    wid = (int) (rc.right - rc.left);
    hei = (int) (rc.bottom - rc.top);

    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    if ((*desc).centered) {
        HMONITOR hm = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hm, &mi);
        RECT work = (*mi).rcWork;
        int wx = (int) (work.right - work.left - wid) / 2;
        int wy = (int) (work.bottom - work.top - hei) / 2;
        x = work.left + wx;
        y = work.top + wy;
    } else if ((*desc).x != 0 || (*desc).y != 0) {
        x = (*desc).x;
        y = (*desc).y;
    }

    Window *w = (Window*) calloc(1, sizeof(Window));
    if (w == nullptr)
        return nullptr;

    wchar_t titleBuf[256];
    const char *t = (*desc).title ? (*desc).title : "vex";
    size_t tn = strlen(t);
    if (tn >= 256)
        tn = 255;
    for (size_t i = 0; i < tn; i++)
        titleBuf[i] = (wchar_t) (unsigned char) t[i];
    titleBuf[tn] = 0;

    (*w).hwnd = CreateWindowExW(
        0, L"VexWindowClass", titleBuf,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, wid, hei,
        nullptr, nullptr, inst, w);
    if ((*w).hwnd == nullptr) {
        free(w);
        return nullptr;
    }

    // WM_NCCREATE already parked the handle into GWLP_USERDATA.
    WindowEvent_init(&(*w).lifecycle);
    atomic_store_explicit(&(*w).shouldClose, false, memory_order_relaxed);
    atomic_store_explicit(&(*w).sizeGeneration, 0, memory_order_relaxed);
    atomic_store_explicit(&(*w).cachedWidth, cw, memory_order_relaxed);
    atomic_store_explicit(&(*w).cachedHeight, ch, memory_order_relaxed);
    RECT fr = {0};
    GetWindowRect((*w).hwnd, &fr);
    (*w).cachedX = (int) fr.left;
    (*w).cachedY = (int) fr.top;
    POINT origin = {0, 0};
    ClientToScreen((*w).hwnd, &origin);
    (*w).cachedContentX = (int) origin.x;
    (*w).cachedContentY = (int) origin.y;
    atomic_store_explicit(&(*w).presentMode, WINDOW_PRESENT_FIFO, memory_order_relaxed);
    atomic_store_explicit(&(*w).transparent, false, memory_order_relaxed);
    atomic_store_explicit(&(*w).renderGeneration, 0, memory_order_relaxed);
    atomic_store_explicit(&(*w).topLayer, nullptr, memory_order_relaxed);
    atomic_store_explicit(&(*w).bottomLayer, nullptr, memory_order_relaxed);
    atomic_store_explicit(&(*w).enabled, true, memory_order_relaxed);
    atomic_store_explicit(&(*w).keyEnabled, true, memory_order_relaxed);
    (*w).lastFocused = false;
    atomic_store_explicit(&(*w).monitorId, 0, memory_order_relaxed);
    (*w).cursorType = WINDOW_CURSOR_DEFAULT;
    (*w).decorated = true;
    (*w).resizableEnabled = true;
    (*w).closableEnabled = true;
    (*w).minimizeEnabled = true;
    (*w).fullscreenButton = true;
    (*w).resizeRenderFn = nullptr;
    (*w).resizeRenderUserdata = nullptr;
    (*w).id = windowIdAcquire((*w).hwnd, w);
    windowRefreshMonitor(w);

    // Trackpad touches: opt in, mirroring the Cocoa content-view touch mask.
    if (!RegisterTouchWindow((*w).hwnd, TWF_WMFINETOUCH | TWF_WMTWOTOUCH))
        (void) 0;

    return w;
}

// Merge a caller's Desc over the defaults. Unset (zero) fields fall back —
// this is what makes partial designated initializers behave like overloads.
// Mirror of the Cocoa descResolve verbatim (platform-neutral).
static WindowDesc descResolve(const WindowDesc *desc) {
    WindowDesc d = { .title = "vex", .width = 800, .height = 600, .x = 0, .y = 0, .centered = true };
    if (desc == nullptr)
        return d;
    if ((*desc).title)
        d.title = (*desc).title;
    if ((*desc).width > 0)
        d.width = (*desc).width;
    if ((*desc).height > 0)
        d.height = (*desc).height;
    if ((*desc).x != 0 || (*desc).y != 0) {
        d.x = (*desc).x;
        d.y = (*desc).y;
        d.centered = false;
    }
    d.shown = (*desc).shown;
    return d;
}

// --- Constructors -----------------------------------------------------------

// Construct a default Win32 window through the common descriptor path.
Window *Window_0(void) {
    return Window_new(nullptr);
}

// Construct a window descriptor using the requested title and default geometry.
Window *Window_1(const char *title) {
    return Window_new(&(WindowDesc){ .title = title });
}

// Construct a window descriptor with explicit title and client dimensions.
Window *Window_3(const char *title, int width, int height) {
    return Window_new(&(WindowDesc){ .title = title, .width = width, .height = height });
}

// Resolve descriptor defaults, allocate the native window, and show if requested.
// Resolve descriptor defaults, allocate the native window, and show on request.
Window *Window_new(const WindowDesc *desc) {
    WindowDesc d = descResolve(desc);
    Window *w = windowAlloc(&d);
    if (w == nullptr)
        return nullptr;
    if (d.shown)
        Window_show(w);
    return w;
}

// Preserve the legacy constructor spelling by forwarding to Window_new.
Window *Window_create(const char *title, int width, int height) {
    return Window_new(&(WindowDesc){ .title = title, .width = width, .height = height });
}

// Tear down the window and free the handle. Safe to call whether the user
// already closed the window or not: if it's still open we close it. The
// WndProc is naturally detatched by DestroyWindow before the handle dies, so
// no callback can touch our freed memory (mirror of the delegate =nil step).
void Window_destroy(Window *window) {
    if (window == nullptr)
        return;
    if (sPendingForeground == (*window).hwnd)
        sPendingForeground = nullptr;
    if (!atomic_load_explicit(&(*window).shouldClose, memory_order_relaxed)
        && IsWindow((*window).hwnd))
        DestroyWindow((*window).hwnd);
    Key_detachWindowAll((*window).id);
    Mouse_detachWindowAll((*window).id);
    Touch_detachWindowAll((*window).id);
    windowIdRelease((*window).id);
    free((*window).framePixels);
    free(window);
}

// Read the close latch, treating a missing handle as already closed.
bool Window_shouldClose(Window *window) {
    return window ? atomic_load_explicit(&(*window).shouldClose, memory_order_relaxed) : true;
}

// Set the close latch observed by the host lifecycle loop.
void Window_setShouldClose(Window *window, bool shouldClose) {
    if (window != nullptr)
        atomic_store_explicit(&(*window).shouldClose, shouldClose, memory_order_relaxed);
}

// Destroy the native window and mark its C handle closed.
void Window_close(Window *window) {
    if (!window) return;
    if (IsWindow((*window).hwnd)) DestroyWindow((*window).hwnd);
    atomic_store_explicit(&(*window).shouldClose, true, memory_order_relaxed);
}

// --- Present policy (pure state; a future render path consumes it) ----------

void Window_setPresentMode(Window *window, int mode) {
    if (window == nullptr)
        return;
    int prev = atomic_exchange_explicit(&(*window).presentMode, mode, memory_order_relaxed);
    if (prev != mode)
        atomic_fetch_add_explicit(&(*window).renderGeneration, 1, memory_order_release);
}

// Read the stored present policy, defaulting to FIFO for a null handle.
int Window_getPresentMode(const Window *window) {
    if (window == nullptr)
        return WINDOW_PRESENT_FIFO;
    return atomic_load_explicit(&(*window).presentMode, memory_order_relaxed);
}

// Update transparency policy, mark render resources dirty, and toggle layered style.
void Window_setTransparent(Window *window, bool transparent) {
    if (window == nullptr)
        return;
    bool prev = atomic_exchange_explicit(&(*window).transparent, transparent, memory_order_relaxed);
    if (prev != transparent)
        atomic_fetch_add_explicit(&(*window).renderGeneration, 1, memory_order_release);
    // WS_EX_LAYERED is the Win32 dialect of per-pixel alpha: the swapchain
    // rebuild ticket lands first, then the OS surface follows.
    LONG_PTR ex = GetWindowLongPtr((*window).hwnd, GWL_EXSTYLE);
    if (transparent)
        ex |= WS_EX_LAYERED;
    else
        ex &= ~WS_EX_LAYERED;
    SetWindowLongPtr((*window).hwnd, GWL_EXSTYLE, ex);
    SetWindowPos((*window).hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Read the stored layered-window transparency policy.
bool Window_isTransparent(const Window *window) {
    return window ? atomic_load_explicit(&(*window).transparent, memory_order_relaxed) : false;
}

// Read the policy generation used by downstream render-resource rebuilds.
uint64_t Window_renderGeneration(const Window *window) {
    return window ? atomic_load_explicit(&(*window).renderGeneration, memory_order_acquire) : 0;
}

// --- Graphics board slots (stored + ordered; rendering lives elsewhere) -----

// ;;INTENTION("orderLayers on the lean window is a pure no-op — the window no
// longer parents render layers; the still-unmigrated darling compositor
// retains the call. It retires with the composite seam.")
void Window_orderLayers(Window *window) {
    (void) window;
}

// Store the opaque lower-layer handle; this backend does not inspect it.
void Window_setBottomLayer(Window *window, void *layer) {
    if (window == nullptr)
        return;
    atomic_store_explicit(&(*window).bottomLayer, layer, memory_order_release);
    Window_orderLayers(window);
}

// Return the stored lower-layer handle without interpreting it.
void *Window_getBottomLayer(const Window *window) {
    return window ? atomic_load_explicit(&(*window).bottomLayer, memory_order_acquire) : nullptr;
}

// Store the opaque upper-layer handle; this backend does not inspect it.
void Window_setTopLayer(Window *window, void *layer) {
    if (window == nullptr)
        return;
    atomic_store_explicit(&(*window).topLayer, layer, memory_order_release);
    Window_orderLayers(window);
}

// Return the stored upper-layer handle without interpreting it.
void *Window_getTopLayer(const Window *window) {
    return window ? atomic_load_explicit(&(*window).topLayer, memory_order_acquire) : nullptr;
}

// ;;INTENTION("Pane/board compositing is inert on the lean window (the same
// slice the Cocoa backend keeps inert); pane work migrates to the render
// personal' own pass. Retires together with the composite seam.")
bool Window_attachPanes(Window *window, Panel *panel, int width, int height) {
    (void) window;
    (void) panel;
    (void) width;
    (void) height;
    return false;
}

// Retain the retired pane API as an inert failure on the lean window backend.
bool Window_resizePanes(Window *window, Panel *panel, int width, int height) {
    (void) window;
    (void) panel;
    (void) width;
    (void) height;
    return false;
}

// Keep the retired pane-compositor entry point linkable without rendering.
void Window_compositePanes(Window *window, Panel *contentPanel) {
    (void) window;
    (void) contentPanel;
}

// Keep the retired board-compositor entry point linkable without rendering.
void Window_compositeBoards(Window *window) {
    (void) window;
}

// --- Runtime state ----------------------------------------------------------

void Window_setEnabled(Window *window, bool enabled) {
    if (window == nullptr)
        return;
    atomic_store_explicit(&(*window).enabled, enabled, memory_order_relaxed);
}

// Read whether native input forwarding is enabled for this window.
bool Window_isEnabled(const Window *window) {
    return window ? atomic_load_explicit(&(*window).enabled, memory_order_relaxed) : false;
}

// Read the flag set between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE.
bool Window_isLiveResizing(const Window *window) {
    return window ? atomic_load_explicit(&(*window).liveResizing, memory_order_relaxed) : false;
}

// --- Chrome / state API -----------------------------------------------------

void Window_setTitle(Window *window, const char *title) {
    if (window == nullptr || title == nullptr)
        return;
    wchar_t titleBuf[256];
    size_t tn = strlen(title);
    if (tn >= 256)
        tn = 255;
    for (size_t i = 0; i < tn; i++)
        titleBuf[i] = (wchar_t) (unsigned char) title[i];
    titleBuf[tn] = 0;
    SetWindowTextW((*window).hwnd, titleBuf);
}

// Read the cached client width in native pixels.
int Window_width(Window *window) {
    if (window == nullptr)
        return 0;
    return atomic_load_explicit(&(*window).cachedWidth, memory_order_relaxed);
}

// Read the cached client height in native pixels.
int Window_height(Window *window) {
    if (window == nullptr)
        return 0;
    return atomic_load_explicit(&(*window).cachedHeight, memory_order_relaxed);
}

// Return the unit scale of the Win32 physical-pixel window API.
float Window_getScale(const Window *window) {
    (void) window;
    return 1.0f;
}

// Provide the display-scale revalidation seam; Win32 reports physical pixels directly.
void Window_revalidate(Window *window) {
    (void) window;
}

// Convert compatibility point dimensions to the integer client-size request.
void Window_setSizePoints(Window *window, float width, float height) {
    Window_setSize(window, (int) width, (int) height);
}

// Return cached client dimensions through the points compatibility outputs.
void Window_getSizePoints(const Window *window, float *outWidth, float *outHeight) {
    if (outWidth) *outWidth = (float) Window_width((Window*) window);
    if (outHeight) *outHeight = (float) Window_height((Window*) window);
}

// Return cached client width as a floating-point compatibility value.
float Window_widthPoints(const Window *window) {
    return (float) Window_width((Window*) window);
}

// Return cached client height as a floating-point compatibility value.
float Window_heightPoints(const Window *window) {
    return (float) Window_height((Window*) window);
}

// Alias the client width as the rendering viewport width.
int Window_viewportWidth(const Window *window) {
    return Window_width((Window*) window);
}

// Alias the client height as the rendering viewport height.
int Window_viewportHeight(const Window *window) {
    return Window_height((Window*) window);
}

// Alias the client width through the viewport points compatibility accessor.
float Window_viewportWidthPoints(const Window *window) {
    return (float) Window_width((Window*) window);
}

// Alias the client height through the viewport points compatibility accessor.
float Window_viewportHeightPoints(const Window *window) {
    return (float) Window_height((Window*) window);
}

// Return the full outer-window width in physical pixels.
int Window_windowWidth(const Window *window) {
    return Window_width((Window*) window);
}

// Return the full outer-window height in physical pixels.
int Window_windowHeight(const Window *window) {
    return Window_height((Window*) window);
}

// Return the full outer-window width through the points compatibility API.
float Window_windowWidthPoints(const Window *window) {
    return (float) Window_width((Window*) window);
}

// Return the full outer-window height through the points compatibility API.
float Window_windowHeightPoints(const Window *window) {
    return (float) Window_height((Window*) window);
}

// Resize the HWND so its client area matches the requested physical dimensions.
void Window_setSize(Window *window, int width, int height) {
    if (window == nullptr)
        return;
    int wid, hei;
    frameForContent(window, width, height, &wid, &hei);
    RECT rc = {0};
    GetWindowRect((*window).hwnd, &rc);
    SetWindowPos((*window).hwnd, nullptr, 0, 0, wid, hei,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Move the native window to the requested top-left screen pixel coordinates.
void Window_setLocation(Window *window, int x, int y) {
    if (window == nullptr)
        return;
    RECT rc = {0};
    GetWindowRect((*window).hwnd, &rc);
    SetWindowPos((*window).hwnd, nullptr, x, y,
                 (int) (rc.right - rc.left), (int) (rc.bottom - rc.top),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

// Return the cached outer-window top-left coordinates through optional outputs.
void Window_getLocation(const Window *window, int *outX, int *outY) {
    if (outX)
        *outX = window ? (*window).cachedX : 0;
    if (outY)
        *outY = window ? (*window).cachedY : 0;
}

// Return the cached client-area screen origin through optional outputs.
void Window_getContentOrigin(const Window *window, int *outX, int *outY) {
    if (outX)
        *outX = window ? (*window).cachedContentX : 0;
    if (outY)
        *outY = window ? (*window).cachedContentY : 0;
}

// Center the outer window in the nearest monitor's usable work area.
void Window_center(Window *window) {
    if (window == nullptr)
        return;
    HMONITOR hm = MonitorFromWindow((*window).hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hm, &mi);
    RECT rc = {0};
    GetWindowRect((*window).hwnd, &rc);
    int wid = (int) (rc.right - rc.left);
    int hei = (int) (rc.bottom - rc.top);
    RECT work = (*mi).rcWork;
    int x = work.left + (work.right - work.left - wid) / 2;
    int y = work.top + (work.bottom - work.top - hei) / 2;
    SetWindowPos((*window).hwnd, nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Claim foreground synchronously: Win32 foreground rules are the mirror of
// AppKit activation — the grant is async and may be refused by the OS, so the
// pump's sPendingForeground re-assertion covers the tail (bounded 500ms).
static void claimForeground(HWND hwnd) {
    if (hwnd == nullptr)
        return;
    winClaimForeground(hwnd);
}

// Show the HWND, request foreground activation, and refresh its monitor mirror.
void Window_show(Window *window) {
    if (window == nullptr)
        return;
    claimForeground((*window).hwnd);
    windowRefreshMonitor(window);
}

// Hide the HWND and cancel any pending foreground claim for it.
void Window_hide(Window *window) {
    if (window == nullptr)
        return;
    if (sPendingForeground == (*window).hwnd)
        sPendingForeground = nullptr;
    ShowWindow((*window).hwnd, SW_HIDE);
}

// Select the native show or hide operation from the requested visibility.
void Window_setVisible(Window *window, bool visible) {
    if (visible)
        Window_show(window);
    else
        Window_hide(window);
}

// Query the current HWND style for the resizable-frame bit.
bool Window_isResizable(Window *window) {
    if (window == nullptr)
        return false;
    LONG_PTR s = GetWindowLongPtr((*window).hwnd, GWL_STYLE);
    return (s & WS_THICKFRAME) != 0;
}

// Update the stored resize capability and rebuild the window chrome style.
void Window_setResizable(Window *window, bool resizable) {
    if (window == nullptr)
        return;
    (*window).resizableEnabled = resizable;
    windowApplyChrome(window, (*window).decorated);
}

// Query the current HWND style for the system-menu close affordance.
bool Window_isClosable(Window *window) {
    if (window == nullptr)
        return false;
    LONG_PTR s = GetWindowLongPtr((*window).hwnd, GWL_STYLE);
    return (s & WS_SYSMENU) != 0;
}

// Toggle the system-menu close affordance while preserving normal close handling.
void Window_setClosable(Window *window, bool closable) {
    if (window == nullptr)
        return;
    // Closable on Win32 toggles the system-menu close affordance: remove
    // WS_SYSMENU (mirror of the Cocoa style-bit toggle), so the title-bar
    // closes but destruction still honors a WM_CLOSE properly when asked.
    (*window).closableEnabled = closable;
    windowApplyChrome(window, (*window).decorated);
}

// Query the current HWND style for the minimize-button affordance.
bool Window_isMiniaturizable(Window *window) {
    if (window == nullptr)
        return false;
    LONG_PTR s = GetWindowLongPtr((*window).hwnd, GWL_STYLE);
    return (s & WS_MINIMIZEBOX) != 0;
}

// Update minimize capability and rebuild the HWND chrome style.
void Window_setMiniaturizable(Window *window, bool miniaturizable) {
    if (window == nullptr)
        return;
    (*window).minimizeEnabled = miniaturizable;
    windowApplyChrome(window, (*window).decorated);
}

// Update maximize-button capability and rebuild the HWND chrome style.
void Window_setFullscreenButton(Window *window, bool enabled) {
    if (window == nullptr)
        return;
    (*window).fullscreenButton = enabled;
    windowApplyChrome(window, (*window).decorated);
}

// Switch window chrome at runtime: one of WINDOW_UNDECORATED_*. Mirrors the
// Cocoa mask swap — BORDERLESS = zero chrome, NAKED = transparent titlebar
// with the resize affordance kept, DECORATED = standard chrome. DECORATED
// strips any active blur so a frosted caption never renders (the blur ban).
void Window_setUndecorated(Window *window, int mode) {
    if (window == nullptr)
        return;
    if ((*window).fullscreen)
        return;
    bool decorated = (mode == WINDOW_DECORATED);
    (*window).decorated = mode;
    windowApplyChrome(window, decorated);
}

// Translate the boolean decoration request into the native chrome mode setter.
void Window_setDecorated(Window *window, bool decorated) {
    Window_setUndecorated(window, decorated ? WINDOW_DECORATED : WINDOW_UNDECORATED_BORDERLESS);
}

// Report whether the current chrome mode is the standard decorated mode.
bool Window_isDecorated(const Window *window) {
    if (window == nullptr)
        return false;
    return (*window).decorated == WINDOW_DECORATED;
}

// Translate the naked-titlebar request into the native chrome mode setter.
void Window_setNaked(Window *window, bool naked) {
    Window_setUndecorated(window, naked ? WINDOW_UNDECORATED_NAKED : WINDOW_DECORATED);
}

// Report whether the current chrome mode uses the naked titlebar style.
bool Window_isNaked(const Window *window) {
    if (window == nullptr)
        return false;
    return (*window).decorated == WINDOW_UNDECORATED_NAKED;
}

// Translate the borderless request into the native chrome mode setter.
void Window_setBorderless(Window *window, bool borderless) {
    Window_setUndecorated(window, borderless ? WINDOW_UNDECORATED_BORDERLESS : WINDOW_DECORATED);
}

// Report whether the current chrome mode removes native window decorations.
bool Window_isBorderless(const Window *window) {
    if (window == nullptr)
        return false;
    return (*window).decorated == WINDOW_UNDECORATED_BORDERLESS;
}

// Retain the AppKit-only titlebar flush seam; Win32 does not implement it.
void Window_setViewportFlushToTop(Window *window, bool flush) {
    if (window == nullptr)
        return;
    if ((*window).decorated != WINDOW_DECORATED)
        return;
    (void) flush;
}

// Report that viewport flush-to-top is unavailable on this backend.
bool Window_isViewportFlushToTop(const Window *window) {
    (void) window;
    return false;
}

// Forward the compatibility request to the inert viewport-flush setter.
void Window_setFloatingTrafficLights(Window *window, bool floating) {
    Window_setViewportFlushToTop(window, floating);
}

// Map a public traffic-light enum value to its native button slot, if any.
static int lightIndex(WindowTrafficLight light) {
    if (light == WINDOW_TRAFFIC_LIGHT_CLOSE)
        return LIGHT_CLOSE;
    if (light == WINDOW_TRAFFIC_LIGHT_MINIMIZE)
        return LIGHT_MINI;
    if (light == WINDOW_TRAFFIC_LIGHT_ZOOM)
        return LIGHT_ZOOM;
    return -1;
}

;;PLATFORM_EXCLUSIVE("macOS")
void Window_macOS_setTrafficLightButtonVisible(Window *window, WindowTrafficLight light, bool visible) {
    (void) window;
    (void) visible;
    (void) lightIndex(light);
    fprintf(stderr, "window: Window_macOS_setTrafficLightButtonVisible is macOS-only (needs a Mac machine)\n");
}

;;PLATFORM_EXCLUSIVE("macOS")
bool Window_macOS_isTrafficLightButtonVisible(const Window *window, WindowTrafficLight light) {
    (void) light;
    fprintf(stderr, "window: Window_macOS_isTrafficLightButtonVisible is macOS-only (needs a Mac machine)\n");
    (void) window;
    return false;
}

;;PLATFORM_EXCLUSIVE("macOS")
void Window_macOS_setTrafficLightHeaderPosition(Window *window, float x, float y) {
    (void) window;
    (void) x;
    (void) y;
    fprintf(stderr, "window: Window_macOS_setTrafficLightHeaderPosition is macOS-only (needs a Mac machine)\n");
}

;;PLATFORM_EXCLUSIVE("macOS")
void Window_macOS_getTrafficLightHeaderPosition(const Window *window, float *outX, float *outY) {
    (void) window;
    fprintf(stderr, "window: Window_macOS_getTrafficLightHeaderPosition is macOS-only (needs a Mac machine)\n");
    if (outX)
        *outX = 0.0f;
    if (outY)
        *outY = 0.0f;
}

// Apply the requested alpha to the layered HWND through User32.
void Window_setOpacity(Window *window, float opacity) {
    if (window == nullptr)
        return;
    LONG_PTR ex = GetWindowLongPtr((*window).hwnd, GWL_EXSTYLE);
    SetWindowLongPtr((*window).hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    BYTE alpha = (BYTE) (int) (opacity * 255.0f + 0.5f);
    SetLayeredWindowAttributes((*window).hwnd, 0, alpha, LWA_ALPHA);
}

// Configure layered per-pixel transparency through the shared policy setter.
void Window_setTransparentBackground(Window *window, bool transparent) {
    if (window == nullptr)
        return;
    // A clear backdrop is the Win32 dialect of the Cocoa clear background:
    // the layered window renders per-pixel, and a render path paints holes.
    Window_setTransparent(window, transparent);
}

// Toggle the HWND's topmost placement without moving or resizing it.
void Window_setAlwaysOnTop(Window *window, bool onTop) {
    if (window == nullptr)
        return;
    HWND insert = onTop ? HWND_TOPMOST : HWND_NOTOPMOST;
    SetWindowPos((*window).hwnd, insert, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Toggle the extended style that lets pointer input pass through the window.
void Window_setClickThrough(Window *window, bool clickThrough) {
    if (window == nullptr)
        return;
    LONG_PTR ex = GetWindowLongPtr((*window).hwnd, GWL_EXSTYLE);
    if (clickThrough) {
        // WS_EX_TRANSPARENT also requires WS_EX_LAYERED to take effect.
        ex |= WS_EX_LAYERED | WS_EX_TRANSPARENT;
    } else {
        ex &= ~WS_EX_TRANSPARENT;
    }
    SetWindowLongPtr((*window).hwnd, GWL_EXSTYLE, ex);
}

// Request the native DWM non-client rendering policy for the window.
void Window_setShadow(Window *window, bool shadow) {
    if (window == nullptr)
        return;
    // ;;INTENTION("DWM-driven drop shadow: DISABLED non-client rendering is
    // the documented low-level switch. Borderless (WS_POPUP) windows get no
    // native shadow regardless — the flag is best-effort for decorated chrome.")
    DWMNCRENDERINGPOLICY policy = shadow ? DWMNCRP_ENABLED : DWMNCRP_DISABLED;
    DwmSetWindowAttribute((*window).hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
}

// Store whether client-area hit tests should initiate a window drag.
void Window_setMovableByBackground(Window *window, bool movable) {
    if (window == nullptr)
        return;
    (*window).movableByBackground = movable;
}

// Raise the HWND in the native z-order without claiming foreground focus.
void Window_bringToFront(Window *window) {
    if (window == nullptr)
        return;
    BringWindowToTop((*window).hwnd);
}

// ;;INTENTION("WIN32 has no child-window ordering contract matching AppKit: attach/detach are link-parity no-ops; stacking stays caller-ordered")
void Window_attachChild(Window *parent, Window *child) {
    (void) parent;
    (void) child;
}

// Retain the child-window ordering seam as a no-op on this backend.
void Window_detachChild(Window *parent, Window *child) {
    (void) parent;
    (void) child;
}

// --- Minimize ---

void Window_minimize(Window *window) {
    if (window == nullptr)
        return;
    atomic_store_explicit(&(*window).miniaturizing, true, memory_order_relaxed);
    ShowWindow((*window).hwnd, SW_MINIMIZE);
}

// Restore an iconic HWND to its prior placement when it is minimized.
void Window_restore(Window *window) {
    if (window == nullptr)
        return;
    if (IsIconic((*window).hwnd))
        ShowWindow((*window).hwnd, SW_RESTORE);
}

// Report either an in-progress minimize transition or the OS iconic state.
bool Window_isMinimized(Window *window) {
    if (window == nullptr)
        return false;
    if (atomic_load_explicit(&(*window).miniaturizing, memory_order_relaxed))
        return true;
    return IsIconic((*window).hwnd) != FALSE;
}

// --- Fullscreen ---

bool Window_isFullscreen(Window *window) {
    if (window == nullptr)
        return false;
    return (*window).fullscreen;
}

// Switch between monitor-sized borderless fullscreen and the saved window frame.
void Window_setFullscreen(Window *window, bool fullscreen) {
    if (window == nullptr)
        return;
    if (fullscreen == (*window).fullscreen)
        return;
    HWND hwnd = (*window).hwnd;
    RECT rc = {0};
    if (fullscreen) {
        GetWindowRect(hwnd, &rc);
        (*window).savedFrame = rc;
        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        SetWindowLongPtr(hwnd, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex & ~WS_EX_DLGMODALFRAME);
        HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hm, &mi);
        (*window).fullscreen = true;
        RECT monitor = (*mi).rcMonitor;
        SetWindowPos(hwnd, HWND_TOPMOST,
                     monitor.left, monitor.top,
                     (int) (monitor.right - monitor.left),
                     (int) (monitor.bottom - monitor.top),
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
    } else {
        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        SetWindowLongPtr(hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        RECT saved = (*window).savedFrame;
        (*window).fullscreen = false;
        SetWindowPos(hwnd, HWND_NOTOPMOST,
                     (int) saved.left, (int) saved.top,
                     (int) (saved.right - saved.left),
                     (int) (saved.bottom - saved.top),
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
        WindowEvent_fireRestored(&(*window).lifecycle, window);
    }
}

// Toggle fullscreen using the current mirrored native fullscreen state.
void Window_toggleFullscreen(Window *window) {
    if (window == nullptr)
        return;
    Window_setFullscreen(window, !Window_isFullscreen(window));
}

// --- DRM / sharing ---

// ;;INTENTION("Window_setDRM has no Win32 analogue (NSWindowSharingNone is an
// AppKit concept). Stored-and-inert; the Win32 surface stays stable.")
void Window_setDRM(Window *window, bool enabled) {
    (void) window;
    (void) enabled;
}

// --- Size constraints ---

void Window_setMinSize(Window *window, int width, int height) {
    if (window == nullptr)
        return;
    (*window).minWidth = width;
    (*window).minHeight = height;
}

// Store the maximum client dimensions consumed by WM_GETMINMAXINFO.
void Window_setMaxSize(Window *window, int width, int height) {
    if (window == nullptr)
        return;
    (*window).maxWidth = width;
    (*window).maxHeight = height;
}

// --- Cursor control ---

// FPS-style relative cursor: hides the pointer, decouples it from movement,
// clips it to the window, and re-warps to the anchor centre each pump pass
// while deltas flow into the mouse stream as move-delta events.
void Window_setCursorLocked(Window *window, bool locked) {
    if (locked == s_cursorLocked)
        return;
    if (locked) {
        HWND anchor = window ? (*window).hwnd : nullptr;
        RECT rc = {0};
        if (anchor)
            GetWindowRect(anchor, &rc);
        s_lockCenter.x = (LONG) (rc.left + (rc.right - rc.left) / 2);
        s_lockCenter.y = (LONG) (rc.top + (rc.bottom - rc.top) / 2);
        if (anchor)
            ClipCursor(&rc);
        SetCursorPos(s_lockCenter.x, s_lockCenter.y);
        while (ShowCursor(FALSE) >= 0) {
        }
        s_cursorLocked = true;
    } else {
        s_cursorLocked = false;
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {
        }
    }
}

// Resolve the stored cursor enum to a Win32 cursor and apply its visibility.
static void windowApplyCursor(Window *window) {
    HCURSOR cursor = LoadCursorW(nullptr, (LPCWSTR) IDC_ARROW);
    switch ((*window).cursorType) {
        case WINDOW_CURSOR_IBEAM:
            cursor = LoadCursorW(nullptr, (LPCWSTR) IDC_IBEAM);
            break;
        case WINDOW_CURSOR_POINTING_HAND:
            cursor = LoadCursorW(nullptr, (LPCWSTR) IDC_HAND);
            break;
        case WINDOW_CURSOR_CROSSHAIR:
            cursor = LoadCursorW(nullptr, (LPCWSTR) IDC_CROSS);
            break;
        case WINDOW_CURSOR_RESIZE_EW:
            cursor = LoadCursorW(nullptr, (LPCWSTR) IDC_SIZEWE);
            break;
        case WINDOW_CURSOR_RESIZE_NS:
            cursor = LoadCursorW(nullptr, (LPCWSTR) IDC_SIZENS);
            break;
        case WINDOW_CURSOR_NOT_ALLOWED:
            cursor = LoadCursorW(nullptr, (LPCWSTR) IDC_NO);
            break;
        case WINDOW_CURSOR_HIDDEN:
            while (ShowCursor(FALSE) >= 0) {
            }
            break;
        case WINDOW_CURSOR_DEFAULT:
        default:
            cursor = LoadCursorW(nullptr, (LPCWSTR) IDC_ARROW);
            break;
    }
    if ((*window).cursorType != WINDOW_CURSOR_HIDDEN)
        SetClassLongPtr((*window).hwnd, GCLP_HCURSOR, (LONG_PTR) cursor);
    SetCursor(cursor);
}

// Store the cursor request and immediately apply the corresponding Win32 cursor.
void Window_setCursorType(Window *window, WindowCursorType type) {
    if (window == nullptr)
        return;
    (*window).cursorType = type;
    windowApplyCursor(window);
}

// Read the current cursor request, defaulting when no window is supplied.
WindowCursorType Window_getCursorType(const Window *window) {
    if (window == nullptr)
        return WINDOW_CURSOR_DEFAULT;
    return (*window).cursorType;
}

// --- Software frame presentation --------------------------------------------

// The lean Win32 software path: copy the caller's RGBA frame into a staging
// DIB and InvalidateRect so the next WM_PAINT StretchDIBits-s blits it scaled
// to fit the client area. Thread 0 only (BeginPaint/EndPaint are _main-thread).
bool Window_present(Window *window, const Buffer *frame) {
    if (window == nullptr || frame == nullptr)
        return false;
    if (Window_width(window) <= 0 || Window_height(window) <= 0)
        return false;
    uint32_t fw = (*frame).width;
    uint32_t fh = (*frame).height;
    if (fw == 0 || fh == 0)
        return false;
    uint32_t chans = (*frame).channels;
    if (chans != 4 && chans != 3 && chans != 1)
        return false;
    size_t Bytes = (size_t) fw * (size_t) fh * 4u;
    if (Bytes == 0)
        return false;
    // Reuse the staging slot when the frame dimensions are unchanged; grow it
    // otherwise (never in a hot path — present is demand-driven).
    if ((*window).frameW != (int) fw || (*window).frameH != (int) fh) {
        unsigned char *grown = (unsigned char*) realloc((*window).framePixels, Bytes);
        if (grown == nullptr)
            return false;
        (*window).framePixels = grown;
        (*window).frameW = (int) fw;
        (*window).frameH = (int) fh;
    }
    const uint64_t *data = (*frame).data;
    unsigned char *dst = (*window).framePixels;
    for (uint32_t y = 0; y < fh; y++) {
        for (uint32_t x = 0; x < fw; x++) {
            size_t i = (size_t) y * fw + x;
            uint64_t px = data[i];
            dst[4 * i + 0] = (unsigned char) ((px >> 16) & 0xFFu); // R (ColorBuffer order)
            dst[4 * i + 1] = (unsigned char) ((px >> 8) & 0xFFu);  // G
            dst[4 * i + 2] = (unsigned char) ((px >> 0) & 0xFFu);  // B
            dst[4 * i + 3] = (unsigned char) ((px >> 24) & 0xFFu); // A
        }
    }
    (*window).frameDirty = true;
    InvalidateRect((*window).hwnd, nullptr, FALSE);
    return true;
}

// --- Surface anchors (thread 0) ----------------------------------------------

void *Window_contentView(Window *window) {
    if (window == nullptr || (*window).hwnd == nullptr)
        return nullptr;
    return (void*) (*window).hwnd;
}

void *Window_nativeHandle(const Window *window) {
    if (window == nullptr || (*window).hwnd == nullptr)
        return nullptr;
    return (void*) (*window).hwnd;
}

// ;;INTENTION("Window_metalLayer is gone from this file by design (no Metal
// here). Returns nullptr so the VK_EXT_metal_surface path degrades cleanly
// until the render personal create their own surface. Retires with the composite
// seam.")
void *Window_metalLayer(Window *window) {
    (void) window;
    return nullptr;
}

// ;;INTENTION("Gravity ownership moved out of the window (no layer here).
// No-op; retires with the composite seam.")
void Window_setGravityTopLeft(Window *window) {
    (void) window;
}

// ;;INTENTION("Worker present transaction was GPU-era (presentsWithTransaction
// drawables). No layer means nothing to commit; no-op. Retires with the
// composite seam.")
void Window_workerPresentBegin(void) {
}

// Close the retired worker-present transaction hook; this backend owns none.
void Window_workerPresentEnd(void) {
}

// --- Event wiring (rings are engine-global; adapters attach by window id) ----

void Window_addKeyAdapter(Window *window, const KeyHandler *adapter) {
    if (window == nullptr)
        return;
    Key_attachWindow((*window).id, adapter);
}

// Detach a key callback from the window's engine input slot.
bool Window_removeKeyAdapter(Window *window, const KeyHandler *adapter) {
    if (window == nullptr)
        return false;
    return Key_detachWindow((*window).id, adapter);
}

// Attach a mouse callback to the window's engine input slot.
void Window_addMouseAdapter(Window *window, const MouseHandler *adapter) {
    if (window == nullptr)
        return;
    Mouse_attachWindow((*window).id, adapter);
}

// Detach a mouse callback from the window's engine input slot.
bool Window_removeMouseAdapter(Window *window, const MouseHandler *adapter) {
    if (window == nullptr)
        return false;
    return Mouse_detachWindow((*window).id, adapter);
}

// Attach a touch callback to the window's engine input slot.
void Window_addTouchAdapter(Window *window, const TouchHandler *adapter) {
    if (window == nullptr)
        return;
    Touch_attachWindow((*window).id, adapter);
}

// Detach a touch callback from the window's engine input slot.
bool Window_removeTouchAdapter(Window *window, const TouchHandler *adapter) {
    if (window == nullptr)
        return false;
    return Touch_detachWindow((*window).id, adapter);
}

// Drain key, mouse, and touch callback queues for the engine input modules.
void Window_dispatchEvents(Window *window) {
    (void) window;
    Key_dispatchEvents();
    Mouse_dispatchEvents();
    Touch_dispatchEvents();
}

// --- Focus (the spotlight: one focused window per machine) -------------------

uint32_t Window_id(Window *window) {
    return window ? (*window).id : FOCUS_BROADCAST;
}

// Request foreground activation and update the engine focus mirror.
void Window_focus(Window *window) {
    if (window == nullptr)
        return;
    claimForeground((*window).hwnd);
    Focus_set((*window).id);
}

// Compare this window's id with the engine-wide focused-window identity.
bool Window_isFocused(Window *window) {
    return window && Focus_isFocused((*window).id);
}

// ;;INTENTION("WIN32 stores the key gate without OS enforcement: no WS_CHILD key refusal matches AppKit; the darling redirect remains the enforcer here")
void Window_setKeyEnabled(Window *window, bool enabled) {
    if (window == nullptr)
        return;
    atomic_store_explicit(&(*window).keyEnabled, enabled, memory_order_relaxed);
}

// Read the key-input gate stored for this native window.
bool Window_isKeyEnabled(const Window *window) {
    if (window == nullptr)
        return false;
    return atomic_load_explicit(&(*window).keyEnabled, memory_order_relaxed);
}

// --- Lifecycle registry ------------------------------------------------------

WindowEvent *Window_getLifecycle(Window *window) {
    if (window == nullptr)
        return nullptr;
    return &(*window).lifecycle;
}

// --- Monitor identity --------------------------------------------------------

uint32_t Window_getMonitorId(const Window *window) {
    return window ? atomic_load_explicit(&(*window).monitorId, memory_order_acquire) : 0;
}

// --- Resize reflection -------------------------------------------------------

uint64_t Window_sizeGeneration(Window *window) {
    if (window == nullptr)
        return 0;
    return atomic_load_explicit(&(*window).sizeGeneration, memory_order_acquire);
}

// Register the callback invoked when native resize changes should redraw immediately.
void Window_setResizeRenderHook(Window *window, WindowResizeRenderFn fn, void *userdata) {
    if (window == nullptr)
        return;
    (*window).resizeRenderFn = fn;
    (*window).resizeRenderUserdata = userdata;
}

// Return the currently registered synchronous resize-render callback.
WindowResizeRenderFn Window_getResizeRenderHook(const Window *window) {
    return window ? (*window).resizeRenderFn : nullptr;
}

#endif // _WIN32
