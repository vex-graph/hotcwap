// window/window_wayland.c — the lean Wayland backend for the Window API (draft).
//
// A 1:1 mirror of the AppKit backend (window/window_cocoa.m): the same
// exported C surface, the same state families (POLICY / CONTENT / ADAPTERS),
// the same reflection words, the same WindowEvent lifecycle bridge, the same
// id registry. The only differences are the OS handle that anchors each
// window (wl_surface + xdg_toplevel instead of NSWindow) and the window-manager
// dialect that serves it (xdg-shell events instead of AppKit notifications).
//
// This is a DRAFT translation unit: it is not wired into CMake and has not
// been compiled against a real libwayland. It mirrors the Cocoa backend
// function-for-function so that wiring it in later (UNIX with WAYLAND_DISPLAY
// -> window/window_wayland.c, with window_linux.c kept as the X11 fallback)
// is a build-script edit, not an API migration.
//
// Like the Cocoa backend it is deliberately LEAN: a window with bridges, and
// nothing else. The compositor does the presentation plumbing — the wl_pointer/
// wl_keyboard/wl_touch listeners mirror OS state into C-visible words exactly
// the way the AppKit delegate notifications do, and input routes into the
// vexspoke Key/Mouse/Touch rings. There is NO Vulkan, NO swapchain and NO
// present worker here — rendering is the render personal' job and reaches the
// screen through the software present seam (a lean wl_shm path mirroring the
// retired raster present) or, once Migrates, the render personal' own surfaces.
// A Window is a dumb surface + callback bridge per the Window Decoupling Law.
//
// The GPU-era composite surface (boards, panes, worker present) is retained
// as thin INERT stubs at the bottom of this file exactly as the Cocoa backend
// keeps them, so the still-unmigrated darling compositor links. They retire
// together with their window.h declarations.

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xdg-shell.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

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
 * DEFINITION: Window_wayland
 * ============================================================================
 * Wayland/XDG desktop window abstraction implementation mirroring Cocoa capabilities.
 * Manages wl_surface, xdg_surface, and xdg_toplevel primitives, routing Wayland
 * seat input into vexspoke device rings and dispatching surface configure events
 * through the embedded WindowEvent lifecycle handlers.
 *
 * Implements the Window Decoupling Law as a presentation surface and event
 * bridge decoupled from GPU drivers, featuring a lean wl_shm software presentation
 * seam alongside inert compatibility stubs.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Window (window/window_wayland.c)
 * ============================================================================
 * SUMMARY:
 *   DRAFT Wayland mirror of the AppKit window backend. One opaque C handle per
 *   wl_surface + xdg_toplevel; the engine loop constructs it, configures the
 *   chrome, commits it, then pumps Window_pollEvents once per frame while a
 *   render path draws through the software present seam (wl_shm) / event
 *   bridges. OS input is routed into the vexspoke device rings (tagged with
 *   this window's id); OS lifecycle (quit, resize, fullscreen, minimize,
 *   restore, press, focus) fires the embedded WindowEvent. Zero Vulkan, zero
 *   compositing — a Window is a dumb surface + callback bridge per the Window
 *   Decoupling Law.
 *
 * STRUCT FIELDS (Mirroring window/window.h incomplete tag — completed here):
 * ----------------------------------------------------------------------------
 *   struct wl_surface *surface;        // the surface anchor (we own it)
 *   struct xdg_surface *xdgSurface;    // xdg-shell wrapper (owned)
 *   struct xdg_toplevel *topLevel;     // the window chrome (owned)
 *   char *title;                       // strdup'd title (owned, xdg lifetime)
 *   WindowEvent lifecycle;             // OS lifecycle registry
 *   uint32_t id;                       // engine window id (1..N, 0 = broadcast)
 *   _Atomic bool shouldClose;          // true once close requested
 *   _Atomic uint64_t sizeGeneration;   // resize-reflection counter
 *   _Atomic int cachedWidth;           // surface width at last configure
 *   _Atomic int cachedHeight;          // surface height at last configure
 *   _Atomic bool liveResizing;         // set during interactive configure flurries
 *   _Atomic int presentMode;           // present pacing (FIFO/IMMEDIATE), stored
 *   _Atomic bool transparent;          // composite transparency request, stored
 *   _Atomic uint64_t renderGeneration; // policy-reflection counter
 *   _Atomic(void*) topLayer;           // content board handle
 *   _Atomic(void*) bottomLayer;        // scene board handle
 *   _Atomic bool enabled;              // false mutes ALL OS input
 *   bool lastFocused;                  // keyboard-enter flip detection
 *   _Atomic uint32_t monitorId;        // wl_output slot id (0 = unmapped)
 *   WindowCursorType cursorType;       // current cursor request
 *   bool decorated;                    // chrome mode (decoration is SSD-owned)
 *   bool fullscreen;                   // xdg toplevel fullscreen state
 *   bool minimized;                    // last set_minimized request
 *   int minWidth, minHeight;           // xdg_toplevel_set_min_size (0 = unset)
 *   int maxWidth, maxHeight;           // xdg_toplevel_set_max_size (0 = unbounded)
 *   bool movableByBackground;          // stored; no base protocol (;;INTENTION)
 *   bool fullscreenButton;             // stored chrome gate (SSD-owned)
 *   bool resizableEnabled;             // stored chrome gate (SSD-owned)
 *   bool closableEnabled;              // stored chrome gate (SSD-owned)
 *   bool minimizeEnabled;              // stored chrome gate (SSD-owned)
 *   bool clickThrough;                 // empty input region when true
 *   float opacity;                     // stored; no base protocol (;;INTENTION)
 *   bool shadow;                       // stored; SSD-owned (;;INTENTION)
 *   int frameW, frameH;                // retained shm staging size in px
 *   struct wl_buffer *shmBuffer;       // retained staging buffer (rebuilt on size)
 *   void *shmMap;                      // retained pool mapping (the staging target)
 *   size_t shmSize;                    // current pool capacity in Bytes
 *   WindowResizeRenderFn resizeRenderFn;   // resize-cadence render hook
 *   void *resizeRenderUserdata;        // hook userdata
 *
 * PRIVATE HELPERS (kept file-local, no external API):
 * ----------------------------------------------------------------------------
 *   WindowSlot                      — id-registry slot record
 *     struct wl_surface *surface;   // the surface carrying this id
 *     Window *handle;               // C handle owning that surface
 *   WaylandOutput                   — wl_output slot record
 *     struct wl_output *output;     // compositor output (may be nulled)
 *     uint32_t id;                  // stable engine id (index + 1)
 *   waylandConnect() / waylandDrain()
 *   windowRefreshSize, waylandOutputIdFor, keyFromKeycode, mirrorModifiers
 *   pointerSetCursorFor, routePointer, routeTouch
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - Window_0(void)                        : Window_new(nullptr)
 *   - Window_1(title)                       : Window_new(&{ .title })
 *   - Window_3(title, width, height)
 *   - Window_new(desc)                      : descResolve + waylandAlloc + show
 *   - Window_create(title, width, height)
 *
 * Private Constructors: (.c static)
 *   - waylandAlloc(desc)                    : shared internal constructor
 *   - descResolve(desc)                     : default parameter resolution
 *
 * Public Core Functions: (.h)
 *   - Window_destroy(window)                : detach, surface free, handle free
 *   - Window_destroyAll(void)
 *   - Window_shouldClose(window)
 *   - Window_pollEvents(void)               : non-blocking wl_display drain
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
 *   - Window_bringToFront(window)           : no base protocol (;;INTENTION)
 *   - Window_minimize(window)               : xdg_toplevel_set_minimized
 *   - Window_restore(window)
 *   - Window_toggleFullscreen(window)
 *   - Window_contentView(window)            : returns the wl_surface anchor
 *   - Window_nativeHandle(window)
 *   - Window_metalLayer(window)             : nullptr — no Metal here
 *   - Window_present(window, frame)         : lean wl_shm software path
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
 *   - Window_focus(window)                  : no base protocol (;;INTENTION)
 *   - Window_sizeGeneration(window)
 *
 * Private Core Functions: (.c static)
 *   - xdgSurfaceConfigure / toplevelConfigure / pointer* / keyboard* / touch*
 *
 * Public Setters: (.h)
 *   - Window_setShouldClose(window, shouldClose)
 *   - Window_setTitle(window, title)
 *   - Window_setSize(window, width, height)
 *   - Window_setLocation(window, x, y)      : xdg has no shell positioning
 *   - Window_setVisible(window, visible)
 *   - Window_setTopLayer(window, layer)
 *   - Window_setBottomLayer(window, layer)
 *   - Window_setPresentMode(window, mode)
 *   - Window_setTransparent(window, transparent)
 *   - Window_setEnabled(window, enabled)
 *   - Window_setKeyEnabled(window, enabled)
 *   - Window_setResizable(window, resizable) : SSD-owned
 *   - Window_setClosable(window, closable)   : SSD-owned
 *   - Window_setMiniaturizable(window, miniaturizable) : SSD-owned
 *   - Window_setFullscreenButton(window, enabled) : SSD-owned
 *   - Window_setUndecorated(window, mode)
 *   - Window_setDecorated(window, decorated)
 *   - Window_setNaked(window, naked)
 *   - Window_setBorderless(window, borderless)
 *   - Window_setViewportFlushToTop(window, flush) : macOS-only no-op
 *   - Window_setFloatingTrafficLights(window, floating) : macOS-only no-op
 *   - Window_macOS_setTrafficLightButtonVisible(window, light, visible) : macOS-only stub
 *   - Window_macOS_setTrafficLightHeaderPosition(window, x, y) : macOS-only stub
 *   - Window_setOpacity(window, opacity)    : stored (;;INTENTION)
 *   - Window_setTransparentBackground(window, transparent)
 *   - Window_setAlwaysOnTop(window, onTop)  : stored (;;INTENTION)
 *   - Window_setClickThrough(window, clickThrough) : empty input region
 *   - Window_setShadow(window, shadow)      : stored (;;INTENTION)
 *   - Window_setMovableByBackground(window, movable) : stored (;;INTENTION)
 *   - Window_setFullscreen(window, fullscreen)
 *   - Window_setDRM(window, enabled)        : no-op (;;INTENTION — macOS concept)
 *   - Window_setMinSize(window, width, height) : xdg min size
 *   - Window_setMaxSize(window, width, height) : xdg max size
 *   - Window_setCursorType(window, type)
 *   - Window_setCursorLocked(window, locked) : hidden cursor + delta warp note
 *   - Window_setResizeRenderHook(window, fn, userdata)
 *   - Window_setGravityTopLeft(window)      : no-op (;;INTENTION)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - Window_getLocation(window, outX, outY)     : last known offset (stored)
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
 *   - Window_isViewportFlushToTop(window) : macOS-only false
 *   - Window_macOS_isTrafficLightButtonVisible(window, light) : macOS-only stub
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
;;PLATFORM_EXCLUSIVE("Wayland")
;;DRAFT
;;INTENTION("Draft translation unit: wired opt-in (-DHOTCWAP_USE_WAYLAND=ON "
            "resolves wayland-client + xkbcommon via pkgconfig and selects "
            "window/window_wayland.c, auto-falling back to window_linux.c), but "
            "never compiled against a real libwayland — the macOS host excludes "
            "it. Mirrors the Cocoa backend 1:1; a Linux-host build is the "
            "promotion gate.")
;;INTENTION("Wayland has no client-side positioning: the compositor owns "
            "window placement, stacking, chrome, and focus. setLocation, "
            "center, bringToFront, setAlwaysOnTop, setShadow, setOpacity, "
            "setMovableByBackground, setFullscreenButton and the chrome "
            "capability toggles are therefore stored in the mirror fields only "
            "(the values a later xdg_layer/xdg_activation/ext-wm client could "
            "honor), never translated to protocol calls. Focus follows the "
            "wl_keyboard enter/leave/mods mirror, exactly like the Cocoa key "
            "window.")
;;INTENTION("The seat (pointer/keyboard/touch) is process-global — one "
            "connection, one wl_seat — so the listeners resolve their target "
            "window by reverse-looking-up the event's wl_surface in the id "
            "registry (the pointer's focused surface is tracked across enter/"
            "leave, since motion/button/axis carry no surface), the Wayland "
            "dialect of the Cocoa per-window delegate.")
;;INTENTION("Window_present is a genuinely lean Wayland path: a retained "
            "wl_shm pool + buffer rebuilt on size change, stamping the RGBA "
            "frame (Buffer layout, Bytes straight off the uint64 element "
            "array) into a mapped ARGB8888 pool and committing the surface. "
            "Straight alpha: the compositor blends against the desktop, "
            "mirroring the transparent swapchain ask.")
;;INTENTION("Cursor control uses a wl_cursor_theme loaded lazily; HIDDEN and "
            "cursor-lock hide the pointer (wl_pointer_set_cursor with a null "
            "surface) and route motion as move-deltas, the Wayland idiom for "
            "an FPS-style relative cursor — the compositor owns any warp.")
;;INTENTION("xdg-shell's toplevel.configure carries no minimize event, so "
            "isMinimized reads the last set_minimized request and restored "
            "derives from any configure that carries a size. Server-side "
            "decorations mean setUndecorated cannot be enforced from clients; "
            "it is stored like the other chrome gates.")
;;INTENTION("Window_setDRM has no Wayland analogue; stored-and-inert so the "
            "surface stays stable.")

// Multi-tap window for double-click style counting (legacy parity: 250ms).
static const uint64_t kTapThresholdNanos = 250000000ULL;

// --- Global compositor state (one connection, many windows) ------------------

static struct wl_display *gDisplay = nullptr;
static struct wl_registry *gRegistry = nullptr;
static struct wl_compositor *gCompositor = nullptr;
static struct xdg_wm_base *gWmBase = nullptr;
static struct wl_shm *gShm = nullptr;
static struct wl_seat *gSeat = nullptr;
static struct wl_pointer *gPointer = nullptr;
static struct wl_keyboard *gKeyboard = nullptr;
static struct wl_touch *gTouch = nullptr;

// The surface the pointer currently hovers (motion/button/axis carry none).
static struct wl_surface *sPointerSurface = nullptr;

// Keyboard mapping state (xkbcommon). One keymap/state per connection.
static struct xkb_context *gXkb = nullptr;
static struct xkb_keymap *gKeymap = nullptr;
static struct xkb_state *gXkbState = nullptr;

// Cursor theme (loaded lazily on first named cursor request).
static struct wl_cursor_theme *sCursorTheme = nullptr;
static struct wl_surface *sCursorSurface = nullptr;

// Window count drives connection teardown: the LAST Window_destroy disconnects.
static int gWindowCount = 0;

// The opaque handle handed back to C. Holds the native surface chain plus
// every C-visible reflection word — the mirror of the Cocoa struct Window.
// `id` is the engine's small window number (1..N; 0 is the broadcast reserved
// id) used to tag input events and route them to per-window listeners.
// `lifecycle` is the embedded WindowEvent — the app's OS-lifecycle bridge.
struct Window {
    struct wl_surface *surface;
    struct xdg_surface *xdgSurface;
    struct xdg_toplevel *topLevel;
    char *title;
    WindowEvent lifecycle;
    uint32_t id;
    _Atomic bool shouldClose;
    _Atomic uint64_t sizeGeneration;
    _Atomic int cachedWidth;
    _Atomic int cachedHeight;
    _Atomic bool liveResizing;

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

    // Chrome: stored gates (SSD owns the actual decoration).
    bool decorated;
    bool fullscreen;
    bool minimized;
    int minWidth;
    int minHeight;
    int maxWidth;
    int maxHeight;
    bool movableByBackground;
    bool fullscreenButton;
    bool resizableEnabled;
    bool closableEnabled;
    bool minimizeEnabled;
    bool clickThrough;
    float opacity;
    bool shadow;

    // Software present path (retained wl_shm staging, mirror of the retired
    // raster present).
    int frameW;
    int frameH;
    struct wl_buffer *shmBuffer;
    void *shmMap;
    size_t shmSize;

    WindowResizeRenderFn resizeRenderFn;
    void *resizeRenderUserdata;
};

// --- Id registry -------------------------------------------------------------
//
// Slot i holds the entries for engine id i (index = id, slot 0 reserved for
// FOCUS_BROADCAST). Grows by doubling (the Dynamic Scalability &
// Anti-Hardcoding Law — no fixed window ceiling).
typedef struct WindowSlot {
    struct wl_surface *surface;
    Window *handle;
} WindowSlot;

static WindowSlot *sRefs = nullptr;
static uint32_t sRefCap = 0;

// Grow the surface-to-window registry geometrically and clear the new slots.
static bool windowRefsGrow(void) {
    uint32_t nextCap = (sRefCap == 0) ? 8u : sRefCap * 2u;
    WindowSlot *grown = (WindowSlot*) realloc(sRefs, (size_t) nextCap * sizeof(WindowSlot));
    if (grown == nullptr)
        return false;
    for (uint32_t i = sRefCap; i < nextCap; i++) {
        grown[i].surface = nullptr;
        grown[i].handle = nullptr;
    }
    sRefs = grown;
    sRefCap = nextCap;
    return true;
}

// Assign a reusable non-broadcast id to a surface and its owning C handle.
static uint32_t windowIdAcquire(struct wl_surface *surface, Window *handle) {
    if (sRefCap == 0 && !windowRefsGrow())
        return FOCUS_BROADCAST;
    for (uint32_t i = 1; i < sRefCap; i++) {
        if (sRefs[i].surface == nullptr) {
            sRefs[i].surface = surface;
            sRefs[i].handle = handle;
            return i;
        }
    }
    if (!windowRefsGrow())
        return FOCUS_BROADCAST;
    for (uint32_t i = 1; i < sRefCap; i++) {
        if (sRefs[i].surface == nullptr) {
            sRefs[i].surface = surface;
            sRefs[i].handle = handle;
            return i;
        }
    }
    return FOCUS_BROADCAST;
}

// Clear a live registry slot when its window is destroyed.
static void windowIdRelease(uint32_t id) {
    if (id != FOCUS_BROADCAST && id < sRefCap) {
        sRefs[id].surface = nullptr;
        sRefs[id].handle = nullptr;
    }
}

// Reverse lookup: which engine id does this surface carry? 0 if unknown.
static uint32_t windowIdOf(struct wl_surface *surface) {
    if (surface == nullptr)
        return FOCUS_BROADCAST;
    for (uint32_t i = 1; i < sRefCap; i++)
        if (sRefs[i].surface == surface)
            return i;
    return FOCUS_BROADCAST;
}

static Window *windowHandleOf(struct wl_surface *surface) {
    if (surface == nullptr)
        return nullptr;
    for (uint32_t i = 1; i < sRefCap; i++)
        if (sRefs[i].surface == surface)
            return sRefs[i].handle;
    return nullptr;
}

// --- Output tracking (monitor identity mirror) -------------------------------

typedef struct WaylandOutput {
    struct wl_output *output;
    uint32_t id;
} WaylandOutput;

static WaylandOutput *sOutputs = nullptr;
static uint32_t sOutputCount = 0;
static uint32_t sOutputCap = 0;

// Resolve an output proxy to its stable engine-facing monitor id.
static uint32_t waylandOutputIdFor(struct wl_output *output) {
    for (uint32_t i = 0; i < sOutputCount; i++)
        if (sOutputs[i].output == output)
            return sOutputs[i].id;
    return 0;
}

// --- Composition helpers -----------------------------------------------------

// Resize reflection: the configure carries the new surface size (px); compare
// against the cache and bump sizegen + fire onResized + run the resize hook
// only on an actual change. Thread 0 only.
static void windowRefreshSize(Window *window, int cw, int ch) {
    if (window == nullptr)
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

// Keyboard focus mirror, driven by wl_keyboard enter/leave (the key window in
// the Wayland dialect). Fires the focus slots only on flips.
static void windowRefreshFocus(Window *window, bool focused) {
    if (window == nullptr)
        return;
    if (focused != (*window).lastFocused) {
        (*window).lastFocused = focused;
        if (focused)
            WindowEvent_fireFocusGained(&(*window).lifecycle, window);
        else
            WindowEvent_fireFocusLost(&(*window).lifecycle, window);
    }
}

// --- Socket-level pump (the poll half of poll-then-tick) ---------------------
//
// The canonical non-blocking read: reserve the read slot, flush pending
// requests, drain events, then dispatch. If another thread already took the
// slot, just dispatch what is pending. Never blocks — exactly the same
// non-blocking contract as Cocoa's dispatchSource tap.
static void waylandDrain(void) {
    if (gDisplay == nullptr)
        return;
    if (wl_display_prepare_read(gDisplay) == -1) {
        wl_display_dispatch_pending(gDisplay);
        return;
    }
    if (wl_display_flush(gDisplay) == -1 && errno != EAGAIN) {
        wl_display_cancel_read(gDisplay);
        return;
    }
    wl_display_read_events(gDisplay);
    wl_display_dispatch_pending(gDisplay);
}

// --- xdg-shell listeners -----------------------------------------------------

// Acknowledge each compositor configure serial for this surface.
static void xdgSurfaceConfigure(void *data, struct xdg_surface *surface, uint32_t serial) {
    Window *window = (Window*) data;
    if (window == nullptr)
        return;
    xdg_surface_ack_configure(surface, serial);
}

static const struct xdg_surface_listener sXdgSurfaceListener = {
    .configure = xdgSurfaceConfigure,
};

// Reflect compositor state, resize and restore notifications into the C window.
static void toplevelConfigure(void *data, struct xdg_toplevel *topLevel, int32_t width,
                              int32_t height, struct wl_array *states) {
    Window *window = (Window*) data;
    if (window == nullptr)
        return;
    (void) topLevel;
    bool fs = false;
    uint32_t *state = nullptr;
    wl_array_for_each(state, states) {
        if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN)
            fs = true;
        else if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED)
            fs = false;
    }
    if (fs != (*window).fullscreen) {
        (*window).fullscreen = fs;
        if (fs)
            WindowEvent_fireFullscreen(&(*window).lifecycle, window);
        else
            WindowEvent_fireRestored(&(*window).lifecycle, window);
    }
    // A configured size means we are mapped/normal again: restored.
    if ((*window).minimized && width > 0 && height > 0) {
        (*window).minimized = false;
        WindowEvent_fireRestored(&(*window).lifecycle, window);
    }
    if (width > 0 && height > 0)
        windowRefreshSize(window, (int) width, (int) height);
}

// Honor the vetoable quit callback and latch close only when it permits.
static void toplevelClose(void *data, struct xdg_toplevel *topLevel) {
    Window *window = (Window*) data;
    if (window == nullptr)
        return;
    (void) topLevel;
    // Quit is the vetoable slot — the same contract as Cocoa's
    // windowShouldClose. A decline keeps the window alive (the compositor
    // stays out of it; we simply never tear down).
    if (WindowEvent_fireQuitRequested(&(*window).lifecycle, window))
        atomic_store_explicit(&(*window).shouldClose, true, memory_order_relaxed);
}

static const struct xdg_toplevel_listener sToplevelListener = {
    .configure = toplevelConfigure,
    .close = toplevelClose,
};

// Reply to the compositor's xdg-shell liveness ping.
static void wmBasePing(void *data, struct xdg_wm_base *wmBase, uint32_t serial) {
    (void) data;
    xdg_wm_base_pong(wmBase, serial);
}

static const struct xdg_wm_base_listener sWmBaseListener = {
    .ping = wmBasePing,
};

// --- Keyboard (xkbcommon) ----------------------------------------------------

// xkb keycode = evdev scan code + 8. Map evdev codes onto GLFW-style KEY_*
// values. The table answers the physical key; -1 = unmapped (rare keys fall
// back to a keysym lookup below).
static int sEvdevMap[256] = {
    [0 ... 255] = -1,
    [1] = KEY_ESCAPE,
    [2] = KEY_NUM_1, [3] = KEY_NUM_2, [4] = KEY_NUM_3, [5] = KEY_NUM_4,
    [6] = KEY_NUM_5, [7] = KEY_NUM_6, [8] = KEY_NUM_7, [9] = KEY_NUM_8,
    [10] = KEY_NUM_9, [11] = KEY_NUM_0,
    [14] = KEY_BACKSPACE, [15] = KEY_TAB, [28] = KEY_ENTER,
    [16] = KEY_Q, [17] = KEY_W, [18] = KEY_E, [19] = KEY_R, [20] = KEY_T,
    [21] = KEY_Y, [22] = KEY_U, [23] = KEY_I, [24] = KEY_O, [25] = KEY_P,
    [26] = KEY_LEFT_BRACKET, [27] = KEY_RIGHT_BRACKET, [43] = KEY_BACKSLASH,
    [30] = KEY_A, [31] = KEY_S, [32] = KEY_D, [33] = KEY_F, [34] = KEY_G,
    [35] = KEY_H, [36] = KEY_J, [37] = KEY_K, [38] = KEY_L,
    [39] = KEY_SEMICOLON, [40] = KEY_APOSTROPHE, [41] = KEY_GRAVE_ACCENT,
    [44] = KEY_Z, [45] = KEY_X, [46] = KEY_C, [47] = KEY_V, [48] = KEY_B,
    [49] = KEY_N, [50] = KEY_M,
    [51] = KEY_COMMA, [52] = KEY_PERIOD, [53] = KEY_SLASH, [57] = KEY_SPACE,
    [58] = KEY_CAPS_LOCK,
    [59] = KEY_F1, [60] = KEY_F2, [61] = KEY_F3, [62] = KEY_F4,
    [63] = KEY_F5, [64] = KEY_F6, [65] = KEY_F7, [66] = KEY_F8,
    [67] = KEY_F9, [68] = KEY_F10, [87] = KEY_F11, [88] = KEY_F12,
    [69] = KEY_NUM_LOCK, [70] = KEY_SCROLL_LOCK,
    [99] = KEY_PRINT_SCREEN, [119] = KEY_PAUSE,
    [103] = KEY_UP, [105] = KEY_LEFT, [106] = KEY_RIGHT, [108] = KEY_DOWN,
    [110] = KEY_INSERT, [111] = KEY_HOME, [112] = KEY_PAGE_UP,
    [113] = KEY_DELETE, [114] = KEY_END, [115] = KEY_PAGE_DOWN,
    [29] = KEY_LEFT_CONTROL, [42] = KEY_LEFT_SHIFT, [56] = KEY_LEFT_ALT,
    [125] = KEY_LEFT_SUPER,
    [54] = KEY_RIGHT_SHIFT, [97] = KEY_RIGHT_CONTROL, [100] = KEY_RIGHT_ALT,
    [126] = KEY_RIGHT_SUPER, [127] = KEY_MENU,
};

// Fallback path for keys the evdev table does not cover: fold an xkb keysym
// into the ASCII-aligned KEY_* slots (the a-z and punctuation rows all sit at
// their ASCII values by construction).
static int keyFromSym(xkb_keysym_t sym) {
    if (sym >= XKB_KEY_a && sym <= XKB_KEY_z)
        return (int) (KEY_A + (sym - XKB_KEY_a));
    if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z)
        return (int) (KEY_A + (sym - XKB_KEY_A));
    if (sym == XKB_KEY_0)
        return KEY_NUM_0;
    if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9)
        return (int) (KEY_NUM_1 + (sym - XKB_KEY_1));
    if (sym >= 0x20 && sym <= 0x7e) {
        int c = (int) sym;
        if (c == KEY_APOSTROPHE || c == KEY_COMMA || c == KEY_MINUS
            || c == KEY_PERIOD || c == KEY_SLASH || c == KEY_SEMICOLON
            || c == KEY_EQUAL || c == KEY_LEFT_BRACKET || c == KEY_BACKSLASH
            || c == KEY_RIGHT_BRACKET || c == KEY_GRAVE_ACCENT)
            return c;
    }
    return -1;
}

// Mirror the xkb modifier mask into the engine Key ring as left-hand diffs —
// Wayland's modifier state is side-neutral, so the LEFT constant represents
// both hands (the same synthetic down/up the Cocoa backend pushes).
static void mirrorModifiers(Window *window) {
    if (window == nullptr || gXkbState == nullptr)
        return;
    uint32_t wid = (*window).id;
    bool shift = xkb_state_mod_name_is_active(gXkbState, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) != 0;
    if (shift != Key_isDown(KEY_LEFT_SHIFT))
        Key_pushEvent(wid, KEY_LEFT_SHIFT, shift ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
    bool ctrl = xkb_state_mod_name_is_active(gXkbState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) != 0;
    if (ctrl != Key_isDown(KEY_LEFT_CONTROL))
        Key_pushEvent(wid, KEY_LEFT_CONTROL, ctrl ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
    bool alt = xkb_state_mod_name_is_active(gXkbState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) != 0;
    if (alt != Key_isDown(KEY_LEFT_ALT))
        Key_pushEvent(wid, KEY_LEFT_ALT, alt ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
    bool super = xkb_state_mod_name_is_active(gXkbState, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) != 0;
    if (super != Key_isDown(KEY_LEFT_SUPER))
        Key_pushEvent(wid, KEY_LEFT_SUPER, super ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
}

static void keyboardKeymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
                           int32_t fd, uint32_t size) {
    (void) data;
    (void) keyboard;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
        close(fd);
        return;
    }
    char *mapStr = (char*) mmap(nullptr, (size_t) size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapStr == MAP_FAILED) {
        close(fd);
        return;
    }
    struct xkb_keymap *keymap = xkb_keymap_new_from_string(
        gXkb, mapStr, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(mapStr, size);
    close(fd);
    if (keymap == nullptr)
        return;
    if (gKeymap)
        xkb_keymap_unref(gKeymap);
    gKeymap = keymap;
    if (gXkbState)
        xkb_state_unref(gXkbState);
    gXkbState = xkb_state_new(gKeymap);
}

static void keyboardEnter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                          struct wl_surface *surface, struct wl_array *keys) {
    (void) keyboard;
    (void) serial;
    (void) surface;
    (void) keys;
    Window *window = (Window*) data;
    if (window == nullptr)
        return;
    windowRefreshFocus(window, true);
    mirrorModifiers(window);
    Focus_set((*window).id);
}

static void keyboardLeave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                          struct wl_surface *surface) {
    (void) keyboard;
    (void) serial;
    (void) surface;
    Window *window = (Window*) data;
    if (window == nullptr)
        return;
    if ((*window).lastFocused)
        Focus_set(FOCUS_BROADCAST);
    windowRefreshFocus(window, false);
}

static void keyboardKey(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                        uint32_t time, uint32_t key, uint32_t state) {
    (void) keyboard;
    (void) serial;
    (void) time;
    Window *window = (Window*) data;
    if (window == nullptr || gXkbState == nullptr)
        return;
    uint32_t wid = (*window).id;
    int evdev = (int) key - 8;
    int code = (evdev >= 0 && evdev < 256) ? sEvdevMap[evdev] : -1;
    if (code == -1) {
        xkb_keysym_t sym = xkb_state_key_get_one_sym(gXkbState, key);
        code = keyFromSym(sym);
    }
    if (code == -1)
        return;
    bool down = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
    if (down && Key_isDown(code))
        Key_pushEvent(wid, code, KEY_ACTION_REPEAT, kTapThresholdNanos);
    else
        Key_pushEvent(wid, code, down ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
    if (down) {
        char utf[8] = {0};
        int n = xkb_state_key_get_utf8(gXkbState, key, utf, (size_t) sizeof(utf) - 1);
        if (n > 0)
            for (int i = 0; i < n; i++)
                Key_pushCharEvent(wid, (uint32_t) (unsigned char) utf[i]);
    }
    xkb_state_update_key(gXkbState, key, down ? XKB_KEY_DOWN : XKB_KEY_UP);
    mirrorModifiers(window);
}

static void keyboardModifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                              uint32_t modsDepressed, uint32_t modsLatched,
                              uint32_t modsLocked, uint32_t group) {
    (void) keyboard;
    (void) serial;
    Window *window = (Window*) data;
    if (window == nullptr || gXkbState == nullptr)
        return;
    xkb_state_update_mask(gXkbState, modsDepressed, modsLatched, modsLocked, 0, 0, group);
    mirrorModifiers(window);
}

static void keyboardRepeatInfo(void *data, struct wl_keyboard *keyboard,
                               int32_t rate, int32_t delay) {
    (void) data;
    (void) keyboard;
    (void) rate;
    (void) delay;
}

static const struct wl_keyboard_listener sKeyboardListener = {
    .keymap = keyboardKeymap,
    .enter = keyboardEnter,
    .leave = keyboardLeave,
    .key = keyboardKey,
    .modifiers = keyboardModifiers,
    .repeat_info = keyboardRepeatInfo,
};

// --- Pointer -----------------------------------------------------------------

// Cursor-theme names per WindowCursorType (the freedesktop cursor theme naming
// that every compositor ships). WINDOW_CURSOR_HIDDEN hides outright.
static const char *cursorName(WindowCursorType type) {
    switch (type) {
        case WINDOW_CURSOR_IBEAM:         return "text";
        case WINDOW_CURSOR_POINTING_HAND: return "pointer";
        case WINDOW_CURSOR_CROSSHAIR:     return "crosshair";
        case WINDOW_CURSOR_RESIZE_EW:     return "ew-resize";
        case WINDOW_CURSOR_RESIZE_NS:     return "ns-resize";
        case WINDOW_CURSOR_NOT_ALLOWED:   return "X_cursor";
        default:                          return "left_ptr";
    }
}

// Apply the requested cursor using the current pointer serial and shared theme.
static void pointerSetCursorFor(uint32_t serial, WindowCursorType type) {
    if (gPointer == nullptr)
        return;
    if (type == WINDOW_CURSOR_HIDDEN) {
        wl_pointer_set_cursor(gPointer, serial, nullptr, 0, 0);
        if (sCursorSurface) {
            wl_surface_attach(sCursorSurface, nullptr, 0, 0);
            wl_surface_commit(sCursorSurface);
        }
        return;
    }
    if (sCursorTheme == nullptr && gShm != nullptr)
        sCursorTheme = wl_cursor_theme_load(nullptr, gShm, 24);
    if (sCursorTheme == nullptr) {
        wl_pointer_set_cursor(gPointer, serial, nullptr, 0, 0);
        return;
    }
    struct wl_cursor *cur = wl_cursor_theme_get_cursor(sCursorTheme, cursorName(type));
    if (cur == nullptr)
        cur = wl_cursor_theme_get_cursor(sCursorTheme, "left_ptr");
    if (cur == nullptr || (*cur).image_count == 0)
        return;
    struct wl_cursor_image *img = (*cur).images[0];
    if (sCursorSurface == nullptr)
        sCursorSurface = wl_compositor_create_surface(gCompositor);
    if (sCursorSurface == nullptr)
        return;
    wl_surface_attach(sCursorSurface, (*img).buffer, 0, 0);
    wl_surface_damage(sCursorSurface, 0, 0, (int) (*img).width, (int) (*img).height);
    wl_surface_commit(sCursorSurface);
    wl_pointer_set_cursor(gPointer, serial, sCursorSurface, (int) (*img).hotspot_x, (int) (*img).hotspot_y);
}

static void pointerEnter(void *data, struct wl_pointer *pointer, uint32_t serial,
                         struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
    (void) data;
    (void) serial;
    (void) pointer;
    sPointerSurface = surface;
    Window *window = windowHandleOf(surface);
    if (window == nullptr || !atomic_load_explicit(&(*window).enabled, memory_order_relaxed))
        return;
    Mouse_pushMoveEvent((*window).id, wl_fixed_to_double(sx), wl_fixed_to_double(sy));
}

static void pointerLeave(void *data, struct wl_pointer *pointer, uint32_t serial,
                         struct wl_surface *surface) {
    (void) data;
    (void) pointer;
    (void) serial;
    (void) surface;
    sPointerSurface = nullptr;
}

static void pointerMotion(void *data, struct wl_pointer *pointer, uint32_t time,
                          wl_fixed_t sx, wl_fixed_t sy) {
    (void) data;
    (void) pointer;
    (void) time;
    Window *win = windowHandleOf(sPointerSurface);
    if (win == nullptr || !atomic_load_explicit(&(*win).enabled, memory_order_relaxed))
        return;
    double x = wl_fixed_to_double(sx);
    double y = wl_fixed_to_double(sy);
    if ((*win).cursorType == WINDOW_CURSOR_HIDDEN) {
        // Cursor-lock path: the compositor owns the pointer, we only emit
        // deltas from successive absolute positions (Wayland has no warp).
        static double sLastX = 0.0;
        static double sLastY = 0.0;
        static bool sHasLast = false;
        if (sHasLast)
            Mouse_pushMoveDeltaEvent((*win).id, x - sLastX, y - sLastY);
        sLastX = x;
        sLastY = y;
        sHasLast = true;
        return;
    }
    Mouse_pushMoveEvent((*win).id, x, y);
}

static void pointerButton(void *data, struct wl_pointer *pointer, uint32_t serial,
                          uint32_t time, uint32_t button, uint32_t state) {
    (void) data;
    (void) pointer;
    (void) time;
    Window *win = windowHandleOf(sPointerSurface);
    if (win == nullptr || !atomic_load_explicit(&(*win).enabled, memory_order_relaxed))
        return;
    int mbutton = -1;
    if (button == BTN_LEFT)
        mbutton = MOUSE_LEFT;
    else if (button == BTN_RIGHT)
        mbutton = MOUSE_RIGHT;
    else if (button == BTN_MIDDLE)
        mbutton = MOUSE_MIDDLE;
    if (mbutton < 0)
        return;
    bool down = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    Mouse_pushButtonEvent((*win).id, mbutton,
                          down ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
    if (down)
        WindowEvent_firePressed(&(*win).lifecycle, win);
    pointerSetCursorFor(serial, (*win).cursorType);
}

static void pointerAxis(void *data, struct wl_pointer *pointer, uint32_t time,
                        uint32_t axis, wl_fixed_t value) {
    (void) data;
    (void) pointer;
    (void) time;
    Window *win = windowHandleOf(sPointerSurface);
    if (win == nullptr || !atomic_load_explicit(&(*win).enabled, memory_order_relaxed))
        return;
    double v = wl_fixed_to_double(value);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        Mouse_pushScrollEvent((*win).id, 0.0, v);
    else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
        Mouse_pushScrollEvent((*win).id, v, 0.0);
}

// Complete a pointer event batch; no additional frame state is retained.
static void pointerFrame(void *data, struct wl_pointer *pointer) {
    (void) data;
    (void) pointer;
}

static const struct wl_pointer_listener sPointerListener = {
    .enter = pointerEnter,
    .leave = pointerLeave,
    .motion = pointerMotion,
    .button = pointerButton,
    .axis = pointerAxis,
    .frame = pointerFrame,
};

// --- Touch -------------------------------------------------------------------

static void touchDown(void *data, struct wl_touch *touch, uint32_t serial,
                      uint32_t time, struct wl_surface *surface, int32_t id,
                      wl_fixed_t sx, wl_fixed_t sy) {
    (void) data;
    (void) touch;
    (void) serial;
    (void) time;
    Window *win = windowHandleOf(surface);
    if (win == nullptr || !atomic_load_explicit(&(*win).enabled, memory_order_relaxed))
        return;
    int slot = (int) (id % TOUCH_MAX);
    Touch_pushTouchEvent((*win).id, slot, TOUCH_DOWN,
        wl_fixed_to_double(sx), wl_fixed_to_double(sy), 1.0, kTapThresholdNanos);
}

static void touchUp(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time,
                    int32_t id) {
    (void) data;
    (void) touch;
    (void) serial;
    (void) time;
    Window *win = windowHandleOf(wl_touch_get_surface(touch));
    if (win == nullptr)
        return;
    int slot = (int) (id % TOUCH_MAX);
    Touch_pushTouchEvent((*win).id, slot, TOUCH_UP, 0.0, 0.0, 0.0, kTapThresholdNanos);
}

static void touchMotion(void *data, struct wl_touch *touch, uint32_t time, int32_t id,
                        wl_fixed_t sx, wl_fixed_t sy) {
    (void) data;
    (void) touch;
    (void) time;
    Window *win = windowHandleOf(wl_touch_get_surface(touch));
    if (win == nullptr)
        return;
    int slot = (int) (id % TOUCH_MAX);
    Touch_pushTouchEvent((*win).id, slot, TOUCH_MOVE,
        wl_fixed_to_double(sx), wl_fixed_to_double(sy), 1.0, kTapThresholdNanos);
}

static void touchCancel(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time,
                        int32_t id) {
    (void) data;
    (void) touch;
    (void) serial;
    (void) time;
    Window *win = windowHandleOf(wl_touch_get_surface(touch));
    if (win == nullptr)
        return;
    int slot = (int) (id % TOUCH_MAX);
    Touch_pushTouchEvent((*win).id, slot, TOUCH_CANCEL, 0.0, 0.0, 0.0, kTapThresholdNanos);
}

// Complete a touch event batch; individual events were already forwarded.
static void touchFrame(void *data, struct wl_touch *touch) {
    (void) data;
    (void) touch;
}

static const struct wl_touch_listener sTouchListener = {
    .down = touchDown,
    .up = touchUp,
    .motion = touchMotion,
    .frame = touchFrame,
    .cancel = touchCancel,
};

// --- Seat --------------------------------------------------------------------

static void seatCapabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    (void) data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && gPointer == nullptr) {
        gPointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(gPointer, &sPointerListener, nullptr);
    } else if ((capabilities & WL_SEAT_CAPABILITY_POINTER) == 0 && gPointer != nullptr) {
        wl_pointer_destroy(gPointer);
        gPointer = nullptr;
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && gKeyboard == nullptr) {
        gKeyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(gKeyboard, &sKeyboardListener, nullptr);
    } else if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) == 0 && gKeyboard != nullptr) {
        wl_keyboard_destroy(gKeyboard);
        gKeyboard = nullptr;
    }
    if ((capabilities & WL_SEAT_CAPABILITY_TOUCH) != 0 && gTouch == nullptr) {
        gTouch = wl_seat_get_touch(seat);
        wl_touch_add_listener(gTouch, &sTouchListener, nullptr);
    } else if ((capabilities & WL_SEAT_CAPABILITY_TOUCH) == 0 && gTouch != nullptr) {
        wl_touch_destroy(gTouch);
        gTouch = nullptr;
    }
}

static const struct wl_seat_listener sSeatListener = {
    .capabilities = seatCapabilities,
};

// --- Output ------------------------------------------------------------------

static void outputGeometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                           int32_t physicalWidth, int32_t physicalHeight, int32_t subpixel,
                           const char *make, const char *model, int32_t transform) {
    (void) data;
    (void) output;
    (void) x;
    (void) y;
    (void) physicalWidth;
    (void) physicalHeight;
    (void) subpixel;
    (void) make;
    (void) model;
    (void) transform;
}

static void outputMode(void *data, struct wl_output *output, uint32_t flags,
                       int32_t width, int32_t height, int32_t refresh) {
    (void) data;
    (void) output;
    (void) flags;
    (void) width;
    (void) height;
    (void) refresh;
}

// Consume the output batch terminator; output geometry is not cached here.
static void outputDone(void *data, struct wl_output *output) {
    (void) data;
    (void) output;
}

static const struct wl_output_listener sOutputListener = {
    .geometry = outputGeometry,
    .mode = outputMode,
    .done = outputDone,
};

// Register an output proxy and assign its session-stable monitor id.
static void outputAdd(struct wl_output *output) {
    if (sOutputCount == sOutputCap) {
        uint32_t nextCap = (sOutputCap == 0) ? 4u : sOutputCap * 2u;
        WaylandOutput *grown = (WaylandOutput*) realloc(sOutputs, (size_t) nextCap * sizeof(WaylandOutput));
        if (grown == nullptr)
            return;
        sOutputs = grown;
        sOutputCap = nextCap;
    }
    sOutputs[sOutputCount].output = output;
    sOutputs[sOutputCount].id = sOutputCount + 1u; // stable per session
    sOutputCount++;
}

// Mark a removed output slot inactive without renumbering other monitors.
static void outputRemove(struct wl_output *output) {
    for (uint32_t i = 0; i < sOutputCount; i++)
        if (sOutputs[i].output == output) {
            sOutputs[i].output = nullptr;
            return;
        }
}

// Surface enter/leave: the compositor tells us which output carries the
// window — the mirror of the Cocoa greatest-intersection resolver.
static void surfaceEnter(void *data, struct wl_surface *surface, struct wl_output *output) {
    Window *window = (Window*) data;
    if (window == nullptr)
        return;
    (void) surface;
    uint32_t id = waylandOutputIdFor(output);
    if (id != 0)
        atomic_store_explicit(&(*window).monitorId, id, memory_order_relaxed);
}

// Clear the monitor mirror when the surface leaves its currently assigned output.
static void surfaceLeave(void *data, struct wl_surface *surface, struct wl_output *output) {
    Window *window = (Window*) data;
    if (window == nullptr)
        return;
    (void) surface;
    if (waylandOutputIdFor(output) == atomic_load_explicit(&(*window).monitorId, memory_order_relaxed))
        atomic_store_explicit(&(*window).monitorId, 0, memory_order_relaxed);
}

static const struct wl_surface_listener sSurfaceListener = {
    .enter = surfaceEnter,
    .leave = surfaceLeave,
};

// --- Registry ----------------------------------------------------------------

static void registryGlobal(void *data, struct wl_registry *registry, uint32_t name,
                           const char *interface, uint32_t version) {
    (void) data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        if (gCompositor == nullptr)
            gCompositor = (struct wl_compositor*) wl_registry_bind(registry, name,
                &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        if (gWmBase == nullptr)
            gWmBase = (struct xdg_wm_base*) wl_registry_bind(registry, name,
                &xdg_wm_base_interface, 1);
        if (gWmBase)
            xdg_wm_base_add_listener(gWmBase, &sWmBaseListener, nullptr);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        if (gShm == nullptr)
            gShm = (struct wl_shm*) wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        if (gSeat == nullptr) {
            gSeat = (struct wl_seat*) wl_registry_bind(registry, name,
                &wl_seat_interface, version < 7 ? version : 7);
            wl_seat_add_listener(gSeat, &sSeatListener, nullptr);
        }
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct wl_output *output = (struct wl_output*) wl_registry_bind(registry, name,
            &wl_output_interface, 1);
        if (output) {
            wl_output_add_listener(output, &sOutputListener, nullptr);
            outputAdd(output);
        }
    }
}

// Ignore registry removals; bound globals remain owned until disconnect.
static void registryGlobalRemove(void *data, struct wl_registry *registry, uint32_t name) {
    (void) data;
    (void) registry;
    (void) name;
}

static const struct wl_registry_listener sRegistryListener = {
    .global = registryGlobal,
    .global_remove = registryGlobalRemove,
};

// Connect once per process; any later window rides the same socket. Refcounted
// by gWindowCount so the LAST Window_destroy tears the connection down.
static bool waylandConnect(void) {
    if (gDisplay != nullptr)
        return true;
    gDisplay = wl_display_connect(nullptr);
    if (gDisplay == nullptr)
        return false;
    gRegistry = wl_display_get_registry(gDisplay);
    wl_registry_add_listener(gRegistry, &sRegistryListener, nullptr);
    if (wl_display_roundtrip(gDisplay) == -1)
        return false;
    gXkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    return true;
}

// Release shared cursor, input, output, and protocol objects after the last window.
static void waylandDisconnect(void) {
    if (sCursorSurface) {
        wl_surface_destroy(sCursorSurface);
        sCursorSurface = nullptr;
    }
    if (sCursorTheme) {
        wl_cursor_theme_destroy(sCursorTheme);
        sCursorTheme = nullptr;
    }
    if (gXkbState) {
        xkb_state_unref(gXkbState);
        gXkbState = nullptr;
    }
    if (gKeymap) {
        xkb_keymap_unref(gKeymap);
        gKeymap = nullptr;
    }
    if (gXkb) {
        xkb_context_unref(gXkb);
        gXkb = nullptr;
    }
    if (gRegistry) {
        wl_registry_destroy(gRegistry);
        gRegistry = nullptr;
    }
    if (gDisplay) {
        wl_display_disconnect(gDisplay);
        gDisplay = nullptr;
    }
    gCompositor = nullptr;
    gWmBase = nullptr;
    gShm = nullptr;
    gSeat = nullptr;
    gPointer = nullptr;
    gKeyboard = nullptr;
    gTouch = nullptr;
    sPointerSurface = nullptr;
    free(sOutputs);
    sOutputs = nullptr;
    sOutputCount = 0;
    sOutputCap = 0;
    free(sRefs);
    sRefs = nullptr;
    sRefCap = 0;
}

// --- Software present (retained wl_shm pool + buffer) ------------------------

static uint32_t sShmNameSeq = 0;

// Rebuild the retained shm buffer + mapping when the frame size changes. A
// fresh pool is created when the size grows so the compositor can still be
// holding the old buffer; the pool object dies right after the buffer is
// created (the buffer keeps the pool's memory alive server-side).
static bool windowEnsureShm(Window *window, int w, int h) {
    size_t Bytes = (size_t) w * (size_t) h * 4u;
    if ((*window).shmBuffer && (*window).frameW == w && (*window).frameH == h)
        return (*window).shmMap != nullptr;
    if ((*window).shmBuffer) {
        wl_buffer_destroy((*window).shmBuffer);
        (*window).shmBuffer = nullptr;
    }
    if ((*window).shmMap) {
        munmap((*window).shmMap, (*window).shmSize);
        (*window).shmMap = nullptr;
    }
    (*window).frameW = w;
    (*window).frameH = h;
    (*window).shmSize = Bytes;

    char name[64];
    snprintf(name, sizeof(name), "/vexgraph-shm-%u", sShmNameSeq++);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd == -1)
        fd = shm_open(name, O_RDWR | O_CREAT, 0600);
    if (fd == -1)
        return false;
    shm_unlink(name); // the fd keeps the mapping alive; no name left behind
    if (ftruncate(fd, (off_t) Bytes) == -1) {
        close(fd);
        return false;
    }
    void *map = mmap(nullptr, Bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return false;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(gShm, fd, (int) Bytes);
    (*window).shmBuffer = wl_shm_pool_create_buffer(pool, 0,
        w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool); // keep only the buffer alive
    close(fd);
    (*window).shmMap = map;
    return true;
}

// --- Constructors ------------------------------------------------------------

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

// Build the wl_surface + xdg_toplevel + C handle. Shared by every constructor.
// The surface is created MAPPED-HIDDEN: it only ever shows content once a
// first commit carries a buffer (the software present path), so
// construct -> mutate -> show never flashes a half-configured window.
static Window *waylandAlloc(const WindowDesc *desc) {
    if (!waylandConnect())
        return nullptr;
    WindowDesc d = descResolve(desc);
    Window *window = (Window*) calloc(1, sizeof(Window));
    if (window == nullptr)
        return nullptr;

    if (gCompositor == nullptr || gWmBase == nullptr) {
        free(window);
        return nullptr;
    }
    (*window).surface = wl_compositor_create_surface(gCompositor);
    if ((*window).surface == nullptr) {
        free(window);
        return nullptr;
    }
    wl_surface_add_listener((*window).surface, &sSurfaceListener, window);
    (*window).xdgSurface = xdg_wm_base_get_xdg_surface(gWmBase, (*window).surface);
    if ((*window).xdgSurface == nullptr) {
        wl_surface_destroy((*window).surface);
        free(window);
        return nullptr;
    }
    xdg_surface_add_listener((*window).xdgSurface, &sXdgSurfaceListener, window);
    (*window).topLevel = xdg_surface_get_toplevel((*window).xdgSurface);
    if ((*window).topLevel == nullptr) {
        xdg_surface_destroy((*window).xdgSurface);
        wl_surface_destroy((*window).surface);
        free(window);
        return nullptr;
    }
    xdg_toplevel_add_listener((*window).topLevel, &sToplevelListener, window);

    (*window).title = strdup(d.title ? d.title : "vex");
    xdg_toplevel_set_title((*window).topLevel, (*window).title ? (*window).title : "vex");
    xdg_toplevel_set_app_id((*window).topLevel, "vexgraph");

    WindowEvent_init(&(*window).lifecycle);
    atomic_store_explicit(&(*window).shouldClose, false, memory_order_relaxed);
    atomic_store_explicit(&(*window).sizeGeneration, 0, memory_order_relaxed);
    atomic_store_explicit(&(*window).cachedWidth, d.width, memory_order_relaxed);
    atomic_store_explicit(&(*window).cachedHeight, d.height, memory_order_relaxed);
    atomic_store_explicit(&(*window).presentMode, WINDOW_PRESENT_FIFO, memory_order_relaxed);
    atomic_store_explicit(&(*window).transparent, false, memory_order_relaxed);
    atomic_store_explicit(&(*window).renderGeneration, 0, memory_order_relaxed);
    atomic_store_explicit(&(*window).topLayer, nullptr, memory_order_relaxed);
    atomic_store_explicit(&(*window).bottomLayer, nullptr, memory_order_relaxed);
    atomic_store_explicit(&(*window).enabled, true, memory_order_relaxed);
    atomic_store_explicit(&(*window).keyEnabled, true, memory_order_relaxed);
    atomic_store_explicit(&(*window).monitorId, 0, memory_order_relaxed);
    (*window).lastFocused = false;
    (*window).cursorType = WINDOW_CURSOR_DEFAULT;
    (*window).decorated = true;
    (*window).resizableEnabled = true;
    (*window).closableEnabled = true;
    (*window).minimizeEnabled = true;
    (*window).fullscreenButton = true;
    (*window).resizeRenderFn = nullptr;
    (*window).resizeRenderUserdata = nullptr;
    (*window).id = windowIdAcquire((*window).surface, window);

    gWindowCount++;
    return window;
}

// Construct a default Wayland window through the common descriptor path.
Window *Window_0(void) {
    return Window_new(nullptr);
}

// Construct a Wayland window with the supplied title and default geometry.
Window *Window_1(const char *title) {
    return Window_new(&(WindowDesc){ .title = title });
}

// Construct a Wayland window with a title and explicit pixel dimensions.
Window *Window_3(const char *title, int width, int height) {
    return Window_new(&(WindowDesc){ .title = title, .width = width, .height = height });
}

// Resolve defaults, allocate the protocol surface chain, and show if requested.
Window *Window_new(const WindowDesc *desc) {
    WindowDesc d = descResolve(desc);
    Window *window = waylandAlloc(&d);
    if (window == nullptr)
        return nullptr;
    if (d.shown)
        Window_show(window);
    return window;
}

// Preserve the legacy constructor spelling by forwarding to Window_new.
Window *Window_create(const char *title, int width, int height) {
    return Window_new(&(WindowDesc){ .title = title, .width = width, .height = height });
}

// Tear down the surface + handle. Safe regardless of whether the compositor
// already closed it: object destruction is idempotent here, and every listener
// is detached by destroying the objects before the handle dies — no callback
// can touch our freed memory (mirror of the delegate = nil step).
void Window_destroy(Window *window) {
    if (window == nullptr)
        return;
    if ((*window).topLevel)
        xdg_toplevel_destroy((*window).topLevel);
    if ((*window).xdgSurface)
        xdg_surface_destroy((*window).xdgSurface);
    if ((*window).surface)
        wl_surface_destroy((*window).surface);
    if ((*window).shmBuffer)
        wl_buffer_destroy((*window).shmBuffer);
    if ((*window).shmMap)
        munmap((*window).shmMap, (*window).shmSize);
    Key_detachWindowAll((*window).id);
    Mouse_detachWindowAll((*window).id);
    Touch_detachWindowAll((*window).id);
    windowIdRelease((*window).id);
    free((*window).title);
    free(window);
    if (gWindowCount > 0)
        gWindowCount--;
    if (gWindowCount == 0)
        waylandDisconnect();
}

// Read the close latch, treating a missing window as already closed.
bool Window_shouldClose(Window *window) {
    return window ? atomic_load_explicit(&(*window).shouldClose, memory_order_relaxed) : true;
}

// Set the close latch observed by the host lifecycle loop.
void Window_setShouldClose(Window *window, bool shouldClose) {
    if (window != nullptr)
        atomic_store_explicit(&(*window).shouldClose, shouldClose, memory_order_relaxed);
}

// Destroy the native protocol surface chain and mark the handle closed.
void Window_close(Window *window) {
    if (!window) return;
    if ((*window).topLevel) { xdg_toplevel_destroy((*window).topLevel); (*window).topLevel = nullptr; }
    if ((*window).xdgSurface) { xdg_surface_destroy((*window).xdgSurface); (*window).xdgSurface = nullptr; }
    if ((*window).surface) { wl_surface_destroy((*window).surface); (*window).surface = nullptr; }
    atomic_store_explicit(&(*window).shouldClose, true, memory_order_relaxed);
}

// --- Poll loop ---------------------------------------------------------------

void Window_pollEvents(void) {
    if (gDisplay == nullptr)
        return;
    waylandDrain();
}

// Run the batch Wayland pump; this backend does not expose event-by-event stepping.
bool Window_pollEventStep(void) {
    Window_pollEvents();
    return false; // Wayland's existing batch pump owns its dispatch boundary.
}

// --- Present policy (pure state; a future render path consumes it) ----------

void Window_setPresentMode(Window *window, int mode) {
    if (window == nullptr)
        return;
    int prev = atomic_exchange_explicit(&(*window).presentMode, mode, memory_order_relaxed);
    if (prev != mode)
        atomic_fetch_add_explicit(&(*window).renderGeneration, 1, memory_order_release);
}

// Return the stored presentation policy, defaulting to FIFO for a null handle.
int Window_getPresentMode(const Window *window) {
    if (window == nullptr)
        return WINDOW_PRESENT_FIFO;
    return atomic_load_explicit(&(*window).presentMode, memory_order_relaxed);
}

// Update transparency policy and advance the render generation when it changes.
void Window_setTransparent(Window *window, bool transparent) {
    if (window == nullptr)
        return;
    bool prev = atomic_exchange_explicit(&(*window).transparent, transparent, memory_order_relaxed);
    if (prev != transparent)
        atomic_fetch_add_explicit(&(*window).renderGeneration, 1, memory_order_release);
}

// Read the stored transparency policy, returning false for a null handle.
bool Window_isTransparent(const Window *window) {
    return window ? atomic_load_explicit(&(*window).transparent, memory_order_relaxed) : false;
}

// Read the render-policy generation with acquire ordering.
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

// Store the opaque lower-layer handle; this backend does not render it.
void Window_setBottomLayer(Window *window, void *layer) {
    if (window == nullptr)
        return;
    atomic_store_explicit(&(*window).bottomLayer, layer, memory_order_release);
    Window_orderLayers(window);
}

// Return the opaque lower-layer handle without interpreting it.
void *Window_getBottomLayer(const Window *window) {
    return window ? atomic_load_explicit(&(*window).bottomLayer, memory_order_acquire) : nullptr;
}

// Store the opaque upper-layer handle; this backend does not render it.
void Window_setTopLayer(Window *window, void *layer) {
    if (window == nullptr)
        return;
    atomic_store_explicit(&(*window).topLayer, layer, memory_order_release);
    Window_orderLayers(window);
}

// Return the opaque upper-layer handle without interpreting it.
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

// Retain the legacy pane API as an inert failure on the lean window backend.
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

// --- Title / size / position -------------------------------------------------

void Window_setTitle(Window *window, const char *title) {
    if (window == nullptr || title == nullptr)
        return;
    free((*window).title);
    (*window).title = strdup(title);
    xdg_toplevel_set_title((*window).topLevel, (*window).title ? (*window).title : "");
}

// Read the most recently configured client width in physical pixels.
int Window_width(Window *window) {
    return window ? atomic_load_explicit(&(*window).cachedWidth, memory_order_relaxed) : 0;
}

// Read the most recently configured client height in physical pixels.
int Window_height(Window *window) {
    return window ? atomic_load_explicit(&(*window).cachedHeight, memory_order_relaxed) : 0;
}

// Report unit scale because this Wayland mirror has no fractional-scale support.
float Window_getScale(const Window *window) {
    (void) window;
    return 1.0f;
}

// Provide the display-revalidation seam; this draft backend stores no scale state.
void Window_revalidate(Window *window) {
    (void) window;
}

// Forward logical-size requests to the integer pixel sizing seam.
void Window_setSizePoints(Window *window, float width, float height) {
    Window_setSize(window, (int) width, (int) height);
}

// Project cached pixel dimensions into the compatibility points outputs.
void Window_getSizePoints(const Window *window, float *outWidth, float *outHeight) {
    if (outWidth) *outWidth = (float) Window_width(window);
    if (outHeight) *outHeight = (float) Window_height(window);
}

// Return the cached width through the points compatibility accessor.
float Window_widthPoints(const Window *window) {
    return (float) Window_width(window);
}

// Return the cached height through the points compatibility accessor.
float Window_heightPoints(const Window *window) {
    return (float) Window_height(window);
}

// Alias the client width as the viewport width.
int Window_viewportWidth(const Window *window) {
    return Window_width(window);
}

// Alias the client height as the viewport height.
int Window_viewportHeight(const Window *window) {
    return Window_height(window);
}

// Alias the logical width as the viewport width in points.
float Window_viewportWidthPoints(const Window *window) {
    return (float) Window_width(window);
}

// Alias the logical height as the viewport height in points.
float Window_viewportHeightPoints(const Window *window) {
    return (float) Window_height(window);
}

// Alias the client width as the outer-window width in this backend.
int Window_windowWidth(const Window *window) {
    return Window_width(window);
}

// Alias the client height as the outer-window height in this backend.
int Window_windowHeight(const Window *window) {
    return Window_height(window);
}

// Alias the logical client width as the outer-window width in points.
float Window_windowWidthPoints(const Window *window) {
    return (float) Window_width(window);
}

// Alias the logical client height as the outer-window height in points.
float Window_windowHeightPoints(const Window *window) {
    return (float) Window_height(window);
}

// The compositor owns geometry on Wayland; setSize only nudges the configure
// cycle. There is no client-side set_size request on xdg-shell — the toplevel
// configure frames carry the truth and bump sizeGeneration via reflection.
void Window_setSize(Window *window, int width, int height) {
    if (window == nullptr)
        return;
    (void) width;
    (void) height;
    wl_surface_commit((*window).surface);
}

// Accept the placement request; xdg-shell leaves positioning to the compositor.
void Window_setLocation(Window *window, int x, int y) {
    (void) window;
    (void) x;
    (void) y;
}

// Return zero because Wayland does not expose client-readable toplevel position.
void Window_getLocation(const Window *window, int *outX, int *outY) {
    if (outX != nullptr)
        *outX = 0;
    if (outY != nullptr)
        *outY = 0;
    (void) window;
}

// Return zero content origin because compositor placement is not exposed here.
void Window_getContentOrigin(const Window *window, int *outX, int *outY) {
    if (outX != nullptr)
        *outX = 0;
    if (outY != nullptr)
        *outY = 0;
    (void) window;
}

// Retain the centering seam; Wayland compositors control window placement.
void Window_center(Window *window) {
    (void) window;
}

// Map the surface by committing it without a buffer to start configure exchange.
void Window_show(Window *window) {
    if (window == nullptr)
        return;
    // Commit with no attached buffer: the surface maps, the compositor runs
    // the configure sequence, and the first present attaches real pixels.
    wl_surface_commit((*window).surface);
}

// Unmap the surface by attaching a null buffer and committing it.
void Window_hide(Window *window) {
    if (window == nullptr)
        return;
    // Attach a null buffer: the compositor stops compositing the surface.
    wl_surface_attach((*window).surface, nullptr, 0, 0);
    wl_surface_commit((*window).surface);
}

// Select the Wayland show or hide operation from the requested visibility.
void Window_setVisible(Window *window, bool visible) {
    if (window == nullptr)
        return;
    if (visible)
        Window_show(window);
    else
        Window_hide(window);
}

// --- Runtime state -----------------------------------------------------------

void Window_setEnabled(Window *window, bool enabled) {
    if (window == nullptr)
        return;
    atomic_store_explicit(&(*window).enabled, enabled, memory_order_relaxed);
}

// Read whether input routing is enabled for this window.
bool Window_isEnabled(const Window *window) {
    return window ? atomic_load_explicit(&(*window).enabled, memory_order_relaxed) : false;
}

// Read the live-resize flag mirrored from compositor configure activity.
bool Window_isLiveResizing(const Window *window) {
    return window ? atomic_load_explicit(&(*window).liveResizing, memory_order_relaxed) : false;
}

// --- Chrome capability toggles (SSD-owned on Wayland; stored for clients) ----

// Read the stored resizable capability; server-side decorations enforce chrome.
bool Window_isResizable(Window *window) {
    return window ? (*window).resizableEnabled : false;
}

// Update the stored resizable capability for clients using server-side chrome.
void Window_setResizable(Window *window, bool resizable) {
    if (window == nullptr)
        return;
    (*window).resizableEnabled = resizable;
}

// Read the stored closable capability; the compositor owns the titlebar action.
bool Window_isClosable(Window *window) {
    return window ? (*window).closableEnabled : false;
}

// Update the stored closable capability for consumers of the window contract.
void Window_setClosable(Window *window, bool closable) {
    if (window == nullptr)
        return;
    (*window).closableEnabled = closable;
}

// Read the stored minimize capability maintained for server-side decorations.
bool Window_isMiniaturizable(Window *window) {
    return window ? (*window).minimizeEnabled : false;
}

// Update the stored minimize capability; no client-side chrome is controlled.
void Window_setMiniaturizable(Window *window, bool miniaturizable) {
    if (window == nullptr)
        return;
    (*window).minimizeEnabled = miniaturizable;
}

// Store whether clients should expose a fullscreen control in their own chrome.
void Window_setFullscreenButton(Window *window, bool enabled) {
    if (window == nullptr)
        return;
    (*window).fullscreenButton = enabled;
}

// Store the requested chrome mode; the compositor owns server-side decorations.
void Window_setUndecorated(Window *window, int type) {
    if (window == nullptr)
        return;
    (*window).decorated = (type == WINDOW_DECORATED);
}

// Translate the boolean chrome request to the undecorated-mode setter.
void Window_setDecorated(Window *window, bool decorated) {
    Window_setUndecorated(window, decorated ? WINDOW_DECORATED : WINDOW_UNDECORATED_BORDERLESS);
}

// Report whether the stored chrome mode is decorated.
bool Window_isDecorated(const Window *window) {
    if (window == nullptr)
        return false;
    return (*window).decorated;
}

// Translate the naked-titlebar request to the undecorated-mode setter.
void Window_setNaked(Window *window, bool naked) {
    Window_setUndecorated(window, naked ? WINDOW_UNDECORATED_NAKED : WINDOW_DECORATED);
}

// Report the stored undecorated state used as the naked-mode mirror.
bool Window_isNaked(const Window *window) {
    if (window == nullptr)
        return false;
    return !(*window).decorated;
}

// Translate the borderless request to the undecorated-mode setter.
void Window_setBorderless(Window *window, bool borderless) {
    Window_setUndecorated(window, borderless ? WINDOW_UNDECORATED_BORDERLESS : WINDOW_DECORATED);
}

// Report the stored undecorated state used as the borderless-mode mirror.
bool Window_isBorderless(const Window *window) {
    if (window == nullptr)
        return false;
    return !(*window).decorated;
}

// Retain the AppKit-only flush request as an inert Wayland compatibility seam.
void Window_setViewportFlushToTop(Window *window, bool flush) {
    (void) window;
    (void) flush;
}

// Report that viewport flush-to-top is unavailable on this backend.
bool Window_isViewportFlushToTop(const Window *window) {
    (void) window;
    return false;
}

// Retain the macOS floating-lights request without a Wayland implementation.
void Window_setFloatingTrafficLights(Window *window, bool floating) {
    (void) window;
    (void) floating;
}

// macOS-only traffic-light chrome (the Window_macOS_ infix IS the platform
// lock; these symbols exist only in the Cocoa backend — stubs make a Wayland
// build that calls them fail loudly instead of silently passing).
void Window_macOS_setTrafficLightButtonVisible(Window *window, WindowTrafficLight light, bool visible) {
    (void) window;
    (void) light;
    (void) visible;
    fprintf(stderr, "[window] macOS-only traffic-light API called on Wayland\n");
}

// Reject visibility queries for macOS chrome on this non-macOS backend.
bool Window_macOS_isTrafficLightButtonVisible(const Window *window, WindowTrafficLight light) {
    (void) window;
    (void) light;
    return false;
}

// Retain the macOS-only traffic-light placement API as an inert stub.
void Window_macOS_setTrafficLightHeaderPosition(Window *window, float x, float y) {
    (void) window;
    (void) x;
    (void) y;
}

// Return zero coordinates because this backend has no native traffic lights.
void Window_macOS_getTrafficLightHeaderPosition(const Window *window, float *outX, float *outY) {
    if (outX != nullptr)
        *outX = 0.0f;
    if (outY != nullptr)
        *outY = 0.0f;
    (void) window;
}

// Store opacity for consumers; Wayland core has no per-surface opacity request.
void Window_setOpacity(Window *window, float opacity) {
    if (window == nullptr)
        return;
    (*window).opacity = opacity;
}

// Forward the background transparency preference to the stored policy setter.
void Window_setTransparentBackground(Window *window, bool transparent) {
    if (window == nullptr)
        return;
    atomic_store_explicit(&(*window).transparent, transparent, memory_order_relaxed);
}

// Retain the stacking preference; Wayland core does not let clients raise windows.
void Window_setAlwaysOnTop(Window *window, bool onTop) {
    (void) window;
    (void) onTop;
}

// Click-through is expressible: an empty input region tells the compositor the
// window never wants input. Resetting hands the full surface back (a resized
// full-region rebuild happens lazily at next pointer/touch entry).
void Window_setClickThrough(Window *window, bool clickThrough) {
    if (window == nullptr)
        return;
    (*window).clickThrough = clickThrough;
    if (clickThrough) {
        wl_surface_set_input_region((*window).surface, nullptr);
    } else {
        struct wl_region *full = wl_compositor_create_region(gCompositor);
        if (full) {
            wl_region_add(full, 0, 0,
                atomic_load_explicit(&(*window).cachedWidth, memory_order_relaxed),
                atomic_load_explicit(&(*window).cachedHeight, memory_order_relaxed));
            wl_surface_set_input_region((*window).surface, full);
            wl_region_destroy(full);
        }
    }
}

// Store the requested shadow preference for client-side chrome consumers.
void Window_setShadow(Window *window, bool shadow) {
    if (window == nullptr)
        return;
    (*window).shadow = shadow;
}

// Store whether the backend's hit-test path should treat the surface as draggable.
void Window_setMovableByBackground(Window *window, bool movable) {
    if (window == nullptr)
        return;
    (*window).movableByBackground = movable;
}

// Retain the stacking seam; activation is compositor-controlled on Wayland.
void Window_bringToFront(Window *window) {
    (void) window;
}

// ;;INTENTION("Wayland has no client-side child-window ordering: attach/detach are link-parity no-ops; the compositor owns stacking")
void Window_attachChild(Window *parent, Window *child) {
    (void) parent;
    (void) child;
}

// Keep child-window ordering API linkable; the compositor owns Wayland stacking.
void Window_detachChild(Window *parent, Window *child) {
    (void) parent;
    (void) child;
}

// --- Minimize ----------------------------------------------------------------

void Window_minimize(Window *window) {
    if (window == nullptr)
        return;
    xdg_toplevel_set_minimized((*window).topLevel);
    (*window).minimized = true;
}

// Clear the remembered minimized state and recommit the surface for remapping.
void Window_restore(Window *window) {
    if (window == nullptr)
        return;
    (*window).minimized = false;
    wl_surface_commit((*window).surface);
}

// Report the last minimized request because xdg-shell has no minimize-state event.
bool Window_isMinimized(Window *window) {
    return window ? (*window).minimized : false;
}

// --- Fullscreen --------------------------------------------------------------

bool Window_isFullscreen(Window *window) {
    return window ? (*window).fullscreen : false;
}

// Request or release compositor-managed fullscreen for the toplevel.
void Window_setFullscreen(Window *window, bool fullscreen) {
    if (window == nullptr)
        return;
    if (fullscreen)
        xdg_toplevel_set_fullscreen((*window).topLevel, nullptr);
    else
        xdg_toplevel_unset_fullscreen((*window).topLevel);
}

// Toggle fullscreen based on the last state reflected from the compositor.
void Window_toggleFullscreen(Window *window) {
    if (window == nullptr)
        return;
    Window_setFullscreen(window, !(*window).fullscreen);
}

// --- DRM / sharing -----------------------------------------------------------

void Window_setDRM(Window *window, bool enabled) {
    (void) window;
    (void) enabled;
}

// --- Size constraints --------------------------------------------------------

void Window_setMinSize(Window *window, int width, int height) {
    if (window == nullptr)
        return;
    (*window).minWidth = width;
    (*window).minHeight = height;
    xdg_toplevel_set_min_size((*window).topLevel, width, height);
}

// Store and publish maximum toplevel dimensions to the compositor.
void Window_setMaxSize(Window *window, int width, int height) {
    if (window == nullptr)
        return;
    (*window).maxWidth = width;
    (*window).maxHeight = height;
    xdg_toplevel_set_max_size((*window).topLevel, width, height);
}

// --- Cursor control ----------------------------------------------------------

void Window_setCursorType(Window *window, WindowCursorType type) {
    if (window == nullptr)
        return;
    (*window).cursorType = type;
    // A named cursor is applied by the compositor on the next pointer enter or
    // button serial; there is no client-owned cursor surface on Wayland.
}

// Read the requested cursor shape, defaulting for a missing window.
WindowCursorType Window_getCursorType(const Window *window) {
    return window ? (*window).cursorType : WINDOW_CURSOR_DEFAULT;
}

// FPS-style relative cursor: on Wayland the pointer itself cannot be warped,
// so lock = hidden pointer + successive absolute positions become move-deltas
// (see pointerMotion). The mirror word is the cursor type itself.
void Window_setCursorLocked(Window *window, bool locked) {
    if (window == nullptr)
        return;
    if (locked)
        (*window).cursorType = WINDOW_CURSOR_HIDDEN;
    else
        (*window).cursorType = WINDOW_CURSOR_DEFAULT;
}

// --- Surface / Metal accessors -----------------------------------------------

void *Window_contentView(Window *window) {
    return window ? (void*) (*window).surface : nullptr;
}

void *Window_nativeHandle(const Window *window) {
    return window ? (void*) (*window).surface : nullptr;
}

// No Metal here: Wayland has no CAMetalLayer ancestor (the lean backend's
// metalLayer is nil, exactly like the Cocoa backend's on non-Apple).
void *Window_metalLayer(Window *window) {
    (void) window;
    return nullptr;
}

// Retain the retired gravity seam as an inert operation on the lean backend.
void Window_setGravityTopLeft(Window *window) {
    (void) window;
}

// Keep the retired worker-present transaction hook linkable as a no-op.
void Window_workerPresentBegin(void) {
}

// Close the retired worker-present transaction hook; no transaction is owned here.
void Window_workerPresentEnd(void) {
}

// --- Software frame presentation --------------------------------------------

bool Window_present(Window *window, const Buffer *frame) {
    if (window == nullptr || frame == nullptr)
        return false;
    const uint32_t fw = (*frame).width;
    const uint32_t fh = (*frame).height;
    const uint32_t ch = (*frame).channels;
    if (fw == 0 || fh == 0 || ch == 0 || ch > 4)
        return false;
    if (!windowEnsureShm(window, (int) fw, (int) fh))
        return false;

    // Stamp the Buffer payload (Bytes straight off the uint64 element array)
    // into the mapped ARGB8888 pool. Straight alpha: the compositor blends it
    // against the desktop — the transparent-swapchain ask, honored for free.
    const unsigned char *src = (const unsigned char*) (*frame).data;
    size_t srcStride = (size_t) fw * ch;
    uint32_t *dst = (uint32_t*) (*window).shmMap;
    for (uint32_t y = 0; y < fh; y++) {
        const unsigned char *row = src + (size_t) y * srcStride;
        for (uint32_t x = 0; x < fw; x++) {
            unsigned char b = row[x * ch + 0];
            unsigned char g = (ch > 1) ? row[x * ch + 1] : 0u;
            unsigned char r = (ch > 2) ? row[x * ch + 2] : 0u;
            unsigned char a = (ch > 3) ? row[x * ch + 3] : 255u;
            dst[(size_t) y * fw + x] = ((uint32_t) a << 24)
                | ((uint32_t) r << 16) | ((uint32_t) g << 8) | (uint32_t) b;
        }
    }

    wl_surface_attach((*window).surface, (*window).shmBuffer, 0, 0);
    wl_surface_damage_buffer((*window).surface, 0, 0, (int) fw, (int) fh);
    wl_surface_commit((*window).surface);
    return true;
}

// --- Event wiring (adapters) -------------------------------------------------

void Window_addKeyAdapter(Window *window, const KeyHandler *adapter) {
    if (window == nullptr || adapter == nullptr)
        return;
    Key_attachWindow((*window).id, adapter);
}

// Detach a key callback from the window's engine input slot.
bool Window_removeKeyAdapter(Window *window, const KeyHandler *adapter) {
    return (window != nullptr && adapter != nullptr)
        ? Key_detachWindow((*window).id, adapter) : false;
}

// Attach a mouse callback to the window's engine input slot.
void Window_addMouseAdapter(Window *window, const MouseHandler *adapter) {
    if (window == nullptr || adapter == nullptr)
        return;
    Mouse_attachWindow((*window).id, adapter);
}

// Detach a mouse callback from the window's engine input slot.
bool Window_removeMouseAdapter(Window *window, const MouseHandler *adapter) {
    return (window != nullptr && adapter != nullptr)
        ? Mouse_detachWindow((*window).id, adapter) : false;
}

// Attach a touch callback to the window's engine input slot.
void Window_addTouchAdapter(Window *window, const TouchHandler *adapter) {
    if (window == nullptr || adapter == nullptr)
        return;
    Touch_attachWindow((*window).id, adapter);
}

// Detach a touch callback from the window's engine input slot.
bool Window_removeTouchAdapter(Window *window, const TouchHandler *adapter) {
    return (window != nullptr && adapter != nullptr)
        ? Touch_detachWindow((*window).id, adapter) : false;
}

WindowEvent *Window_getLifecycle(Window *window) {
    return window ? &(*window).lifecycle : nullptr;
}

// Drain key, mouse, and touch callback queues for the engine input modules.
void Window_dispatchEvents(Window *window) {
    (void) window;
    Key_dispatchEvents();
    Mouse_dispatchEvents();
    Touch_dispatchEvents();
}

// --- Focus -------------------------------------------------------------------

uint32_t Window_id(Window *window) {
    return window ? (*window).id : 0;
}

// Focus on Wayland is entirely compositor-owned; there is no base xdg request
// to force it (that lives in the separate xdg-activation protocol). The
// mirror word updates when wl_keyboard enter/leave lands.
void Window_focus(Window *window) {
    (void) window;
}

// Read the focus mirror updated by wl_keyboard enter/leave notifications.
bool Window_isFocused(Window *window) {
    return window ? (*window).lastFocused : false;
}

// ;;INTENTION("Wayland stores the key gate without OS enforcement: the compositor owns activation; the darling redirect remains the enforcer here")
void Window_setKeyEnabled(Window *window, bool enabled) {
    if (window == nullptr)
        return;
    atomic_store_explicit(&(*window).keyEnabled, enabled, memory_order_relaxed);
}

// Read the stored key-input gate, returning false for a null handle.
bool Window_isKeyEnabled(const Window *window) {
    if (window == nullptr)
        return false;
    return atomic_load_explicit(&(*window).keyEnabled, memory_order_relaxed);
}

// --- Monitor identity --------------------------------------------------------

uint32_t Window_getMonitorId(const Window *window) {
    return window ? atomic_load_explicit(&(*window).monitorId, memory_order_relaxed) : 0;
}

// --- Resize reflection -------------------------------------------------------

uint64_t Window_sizeGeneration(Window *window) {
    return window ? atomic_load_explicit(&(*window).sizeGeneration, memory_order_acquire) : 0;
}

// --- Resize-cadence rendering (the c -> compositor -> c bridge) --------------
//
// xdg_toplevel.configure carries the compositor's committed size; the
// configure listener runs the same hook the Cocoa willResize/didResize path
// does — inline render+present on Thread 0 before the frame is composited.

void Window_setResizeRenderHook(Window *window, WindowResizeRenderFn fn, void *userdata) {
    if (window == nullptr)
        return;
    (*window).resizeRenderFn = fn;
    (*window).resizeRenderUserdata = userdata;
}

// Return the callback registered for synchronous configure-size rendering.
WindowResizeRenderFn Window_getResizeRenderHook(const Window *window) {
    return window ? (*window).resizeRenderFn : nullptr;
}

#endif // __linux__
