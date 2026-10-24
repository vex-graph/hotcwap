// window/window_cocoa.m — the AppKit window backend (the one ObjC file).
//
// vex's core is pure C23; this is the ONE file that talks to AppKit, because
// NSWindow/NSApplication are ObjC objects and there is no pure-C way to
// create them. Everything above this boundary stays C; everything here is
// "dip into the OS, hand back a handle, pump the OS event queue".
//
// This is the FRESH window backend, rebuilt from scratch after the _trash-era
// shim retired. It is deliberately LEAN: a window with bridges, and nothing
// else. It creates and owns an NSWindow, mirrors the OS's size/move/focus/
// monitor state into C-visible words, fires the per-window WindowEvent
// lifecycle (quit veto, resized, fullscreen, minimized, restored, pressed,
// focus, zoom), and routes OS input into the vexspoke Key/Mouse/Touch rings.
// There is NO Vulkan, NO Metal, NO CAMetalLayer, NO board/pane compositing,
// and NO present worker in this file — rendering is the render personal' job
// (graphvex/darling) and reaches the screen through the content view and the
// event bridges, exactly per the Window Decoupling Law (a Window is a dumb
// surface + callback bridge, never a renderer).
//
// The GPU-era composite surface (boards, panes, worker present, software
// frame present) is retained as thin INERT stubs at the bottom of this file
// so the still-unmigrated darling compositor links; each carries an
// ;;INTENTION marker. When darling migrates to the WindowEvent bridge, the
// stubs and their window.h declarations retire together.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <ImageIO/ImageIO.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#import <stdatomic.h>
#include <math.h>

#include "window/window.h"
#include "window/traffic_light.h"
#include "input/focus.h"
#include "input/key.h"
#include "input/mouse.h"
#include "input/touch.h"
#include "annotation/definition.h"
#include "annotation/overview.h"
#include "annotation/getter.h"
#include "annotation/setter.h"
#include "annotation/intention.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Window
 * ============================================================================
 * Platform abstraction encapsulating an OS-managed desktop display surface.
 * On macOS, implements the native AppKit backend via an NSWindow handle, owning
 * the window chrome, display scaling, frame geometry, and event dispatch seam.
 * Strictly decoupled from GPU rendering pipelines according to the Window
 * Decoupling Law, serving solely as a dumb presentation surface and callback bridge.
 *
 * Dispatches input events directly into vexspoke device rings (Key, Mouse, Touch)
 * and forwards window lifecycle state changes (geometry resize + move, minimize,
 * restore, fullscreen, key focus, and vetoable termination requests) through the
 * embedded WindowEvent registry on Thread 0. Geometry is reflected EVERY tracking
 * step — onResized(w,h) and onMoved(x,y) both fire the moment the window changes,
 * not on settle.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Window (window/window_cocoa.m)
 * ============================================================================
 * SUMMARY:
 *   The fresh, lean AppKit window backend. One opaque C handle per NSWindow;
 *   the engine loop constructs it, configures the chrome, shows it, then pumps
 *   Window_pollEvents once per frame while a render path draws through the
 *   content view / event bridges. OS input is routed into the vexspoke device
 *   rings (tagged with this window's id); OS lifecycle (quit, resize, move,
 *   fullscreen, minimize, restore, press, focus, zoom) fires the embedded
 *   WindowEvent. Zero Vulkan, zero Metal, zero compositing — a Window is a
 *   dumb surface + callback bridge per the Window Decoupling Law.
 *
 * STRUCT FIELDS (Mirroring window/window.h incomplete tag — completed here):
 * ----------------------------------------------------------------------------
 *   NSWindow *nsWindow;           // AppKit window (we own it; releasedWhenClosed NO)
 *   WindowDelegate *delegate;     // per-window close/resize/focus delegate
 *   WindowEvent lifecycle;        // OS lifecycle registry (window/window_event.h)
 *   uint32_t id;                  // engine window id (1..N, 0 = FOCUS_BROADCAST)
 *   _Atomic bool shouldClose;     // true once close requested (Thread 0 writes, loop reads)
 *   _Atomic uint64_t sizeGeneration; // resize-reflection counter (thread 0 bumps)
 *   _Atomic int cachedWidth;      // content width in physical pixels at last thread-0 event (any thread reads)
 *   _Atomic int cachedHeight;     // content height in physical pixels at last thread-0 event
 *   double cachedScale;           // active backing scale factor
 *   double cachedX;               // top-left screen X at last thread-0 event
 *   double cachedY;               // top-left screen Y at last thread-0 event
 *   double cachedContentX;        // CONTENT top-left X (below title bar)
 *   double cachedContentY;        // CONTENT top-left Y (below title bar)
 *   _Atomic bool liveResizing;    // thread 0 during NSViewLiveResize; renderer consumes
 *   _Atomic bool miniaturizing;   // thread 0 during genie minimize; suppress merge
 *   _Atomic int presentMode;      // present pacing (FIFO/IMMEDIATE), pure state
 *   _Atomic bool transparent;     // composite transparency request, pure state
 *   _Atomic uint64_t renderGeneration; // policy-reflection counter (rebuild ticket)
 *   _Atomic bool enabled;         // false mutes ALL OS input for this window
 *   bool lastFocused;             // focus-flip detection during the pump
 *   _Atomic uint32_t monitorId;   // CGDirectDisplayID mirror (0 = unmapped)
 *   WindowCursorType cursorType;  // active OS cursor style
 *   TrafficLight *trafficLights;  // macOS traffic-light chrome controller (window/traffic_light.h)
 *   WindowResizeRenderFn resizeRenderFn;  // resize-cadence render hook
 *   void *resizeRenderUserdata;   // hook userdata
 *
 * PRIVATE HELPERS (kept file-local, no external API):
 * ----------------------------------------------------------------------------
 *   WindowContentView : NSView      — flipped content view (top-left origin)
 *     BOOL active;                  // unused; class exists for flipped geometry
 *   WindowAppDelegate : NSObject
 *     (implements applicationShouldTerminateAfterLastWindowClosed)
 *   WindowDelegate : NSObject <NSWindowDelegate>
 *     atomic_bool *shouldClosePtr;  // weak assignment into the owning handle
 *     Window *handlePtr;            // weak assignment back to the owning handle
 *   WindowSlot                      — id-registry slot record
 *     NSWindow *window;             // OS window owning this id
 *     Window *handle;               // C handle owning that window
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - Window_0(void)
 *   - Window_1(title)
 *   - Window_3(title, width, height)
 *   - Window_new(desc)
 *   - Window_create(title, width, height)
 *
 * Private Constructors: (.c static)
 *   - windowAlloc(desc)               : shared constructor core
 *   - descResolve(desc)               : resolve configuration defaults
 *
 * Public Core Functions: (.h)
 *   - Window_destroy(window)          : detach delegate, close, free handle
 *   - Window_destroyAll(void)
 *   - Window_shouldClose(window)
 *   - Window_pollEvents(void)         : drain OS queue once per frame
 *   - Window_width(window)
 *   - Window_height(window)
 *   - Window_center(window)
 *   - Window_show(window)
 *   - Window_hide(window)
 *   - Window_attachPanes(window, panel, width, height) : inert (;;INTENTION)
 *   - Window_resizePanes(window, panel, width, height) : inert (;;INTENTION)
 *   - Window_compositePanes(window, contentPanel)      : inert (;;INTENTION)
 *   - Window_compositeBoards(window)  : inert (;;INTENTION)
 *   - Window_renderGeneration(window)
 *   - Window_bringToFront(window)
 *   - Window_minimize(window)
 *   - Window_restore(window)
 *   - Window_toggleFullscreen(window)
 *   - Window_contentView(window)
 *   - Window_metalLayer(window)       : nullptr — no Metal here (;;INTENTION)
 *   - Window_present(window, frame)   : inert (;;INTENTION)
 *   - Window_workerPresentBegin(window) : inert (;;INTENTION)
 *   - Window_workerPresentEnd(window)   : inert (;;INTENTION)
 *   - Window_orderLayers(window)      : inert (;;INTENTION)
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
 *   - Window_getScale(window)
 *   - Window_revalidate(window)
 *   - Window_getSizePoints(window, outWidth, outHeight)
 *   - Window_widthPoints(window)
 *   - Window_heightPoints(window)
 *
 * Private Core Functions: (.c static)
 *   - routeEvent(event)               : OS event dispatches to device rings + WindowEvent
 *   - windowRefreshSize(window)       : compute geometry and trigger resize callbacks
 *   - windowRefreshFocus(window, key) : track key window focus transitions
 *   - windowRefreshMonitor(window)    : update active monitor identity
 *   - windowBackingScale(window)      : query active backing scale factor
 *   - windowIdAcquire(win, handle)    : register handle in slot registry
 *   - windowIdRelease(id)             : release slot registry handle
 *   - windowIdOf(win)                 : resolve window ID from NSWindow pointer
 *   - windowHandleOf(win)             : resolve Window handle from NSWindow pointer
 *   - recenterIfLocked(void)          : cursor lock recenter helper
 *   - claimKeyAfterActivation(win)    : take key window status on app activation
 *
 * Public Setters: (.h)
 *   - Window_setShouldClose(window, shouldClose)
 *   - Window_setTitle(window, title)
 *   - Window_setSize(window, width, height)
 *   - Window_setSizePoints(window, width, height)
 *   - Window_setLocation(window, x, y)
 *   - Window_setVisible(window, visible)
 *   - Window_setPresentMode(window, mode)
 *   - Window_setTransparent(window, transparent)
 *   - Window_setEnabled(window, enabled)
 *   - Window_setKeyEnabled(window, enabled)
 *   - Window_setResizable(window, resizable)
 *   - Window_setClosable(window, closable)
 *   - Window_setMiniaturizable(window, miniaturizable)
 *   - Window_setFullscreenButton(window, enabled)
 *   - Window_setUndecorated(window, type)
 *   - Window_setDecorated(window, decorated)
 *   - Window_setNaked(window, naked)
 *   - Window_setBorderless(window, borderless)
 *   - Window_setViewportFlushToTop(window, flush)
 *   - Window_setFloatingTrafficLights(window, floating)
 *   - Window_macOS_setTrafficLightButtonVisible(window, light, visible)
 *   - Window_macOS_setTrafficLightHeaderPosition(window, x, y)
 *   - Window_macOS_hasLiquidGlass() / Window_macOS_setLiquidGlass(window, desc)
 *   - Window_setOpacity(window, opacity)
 *   - Window_setTransparentBackground(window, transparent)
 *   - Window_setAlwaysOnTop(window, onTop)
 *   - Window_setClickThrough(window, clickThrough)
 *   - Window_setShadow(window, shadow)
 *   - Window_setMovableByBackground(window, movable)
 *   - Window_setFullscreen(window, fullscreen)
 *   - Window_setDRM(window, enabled)
 *   - Window_setMinSize(window, width, height)
 *   - Window_setMaxSize(window, width, height)
 *   - Window_setCursorLocked(window, locked)
 *   - Window_setCursorType(window, type)
 *   - Window_setGravityTopLeft(window)  : no-op (;;INTENTION)
 *   - Window_setResizeRenderHook(window, fn, userdata)
 *   - Window_setTopLayer(window, layer)
 *   - Window_setBottomLayer(window, layer)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - Window_getLocation(window, outX, outY)
 *   - Window_getContentOrigin(window, outX, outY)
 *   - Window_getPresentMode(window)
 *   - Window_isTransparent(window)
 *   - Window_isEnabled(window)
 *   - Window_isKeyEnabled(window)
 *   - Window_isLiveResizing(window)
 *   - Window_isResizable(window)
 *   - Window_isClosable(window)
 *   - Window_isMiniaturizable(window)
 *   - Window_isDecorated(window)
 *   - Window_isNaked(window)
 *   - Window_isBorderless(window)
 *   - Window_isViewportFlushToTop(window)
 *   - Window_macOS_isTrafficLightButtonVisible(window, light)
 *   - Window_macOS_getTrafficLightHeaderPosition(window, outX, outY)
 *   - Window_isMinimized(window)
 *   - Window_isFullscreen(window)
 *   - Window_isFocused(window)
 *   - Window_getMonitorId(window)
 *   - Window_getCursorType(window)
 *   - Window_getLifecycle(window)
 *   - Window_getTopLayer(window)
 *   - Window_getBottomLayer(window)
 *   - Window_getResizeRenderHook(window)
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */
;;INTENTION("GPU-era composite surface (attachPanes/resizePanes/compositePanes/compositeBoards/orderLayers/metalLayer/setGravityTopLeft/workerPresentBegin/workerPresentEnd/present) is retained as inert stubs so the still-unmigrated darling compositor keeps linking; zero Vulkan/Metal code lives in this file. They retire together with their window.h declarations once darling migrates onto the WindowEvent bridge (the Window Decoupling Law).")
;;INTENTION("presentMode/transparent/renderGeneration are pure atomic policy state (swapchain-rebuild tickets for a future render path), not GPU calls; no render logic exists here.")

// Multi-tap window for double-click style counting (legacy parity: 250ms).
static const uint64_t kTapThresholdNanos = 250000000ULL;

// Carbon virtual keycode -> KEY_* code. -1 = unmapped. Same table the legacy
// macOSWindow built (physical F1-F12, not Fn-doubled media keys).
static int macKeyMap[128] = {
    [0] = KEY_A,                   [1] = KEY_S,
    [2] = KEY_D,                   [3] = KEY_F,
    [4] = KEY_H,                   [5] = KEY_G,
    [6] = KEY_Z,                   [7] = KEY_X,
    [8] = KEY_C,                   [9] = KEY_V,
    [11] = KEY_B,                  [12] = KEY_Q,
    [13] = KEY_W,                  [14] = KEY_E,
    [15] = KEY_R,                  [16] = KEY_Y,
    [17] = KEY_T,                  [18] = KEY_NUM_1,
    [19] = KEY_NUM_2,              [20] = KEY_NUM_3,
    [21] = KEY_NUM_4,              [22] = KEY_NUM_6,
    [23] = KEY_NUM_5,              [24] = KEY_EQUAL,
    [25] = KEY_NUM_9,              [26] = KEY_NUM_7,
    [27] = KEY_MINUS,              [28] = KEY_NUM_8,
    [29] = KEY_NUM_0,              [30] = KEY_RIGHT_BRACKET,
    [31] = KEY_O,                  [32] = KEY_U,
    [33] = KEY_LEFT_BRACKET,       [34] = KEY_I,
    [35] = KEY_P,                  [36] = KEY_ENTER,
    [37] = KEY_L,                  [38] = KEY_J,
    [39] = KEY_APOSTROPHE,         [40] = KEY_K,
    [41] = KEY_SEMICOLON,          [42] = KEY_BACKSLASH,
    [43] = KEY_COMMA,              [44] = KEY_SLASH,
    [45] = KEY_N,                  [46] = KEY_M,
    [47] = KEY_PERIOD,             [48] = KEY_TAB,
    [49] = KEY_SPACE,              [50] = KEY_GRAVE_ACCENT,
    [51] = KEY_BACKSPACE,          [53] = KEY_ESCAPE,
    [54] = KEY_RIGHT_SUPER,        [55] = KEY_LEFT_SUPER,
    [56] = KEY_LEFT_SHIFT,         [57] = KEY_CAPS_LOCK,
    [58] = KEY_LEFT_ALT,           [59] = KEY_LEFT_CONTROL,
    [60] = KEY_RIGHT_SHIFT,        [61] = KEY_RIGHT_ALT,
    [62] = KEY_RIGHT_CONTROL,      [63] = KEY_FN,
    [96] = KEY_F5,                 [97] = KEY_F6,
    [98] = KEY_F7,                 [99] = KEY_F3,
    [100] = KEY_F8,                [101] = KEY_F9,
    [103] = KEY_F11,               [109] = KEY_F10,
    [111] = KEY_F12,               [118] = KEY_F4,
    [120] = KEY_F2,                [122] = KEY_F1,
    [123] = KEY_LEFT,              [124] = KEY_RIGHT,
    [125] = KEY_DOWN,              [126] = KEY_UP,
};

// Cursor-lock state. While locked the pointer is decoupled from motion and
// re-warped to the anchor centre every pump pass; NSEvent deltas feed mouse.
static bool s_cursorLocked = false;
static CGPoint s_lockCenter = {0, 0};

@class WindowDelegate;

// The opaque handle handed back to C. Holds the NS objects we must keep alive
// (window + delegate) plus every C-visible reflection word. `id` is the
// engine's small window number (1..N; 0 is the broadcast reserved id) used to
// tag input events and route them to per-window listeners. `lifecycle` is the
// embedded WindowEvent — the app's OS-lifecycle bridge (the Window Decoupling
// Law: the window publishes live state and callbacks; it never renders).
struct Window {
    NSWindow *nsWindow;
    WindowDelegate *delegate;
    WindowEvent lifecycle;
    uint32_t id;
    _Atomic bool shouldClose;
    _Atomic uint64_t sizeGeneration;
    _Atomic int cachedWidth;
    _Atomic int cachedHeight;
    double cachedScale;
    double cachedX;
    double cachedY;
    double cachedContentX;
    double cachedContentY;
    _Atomic bool liveResizing;
    _Atomic bool miniaturizing;

    // Pure-state reflection words (no GPU calls in this file).
    _Atomic int presentMode;
    _Atomic bool transparent;
    _Atomic uint64_t renderGeneration;

    _Atomic bool enabled;
    _Atomic bool keyEnabled; // false = canBecomeKeyWindow refuses (held by a modal dialog)
    bool lastFocused;
    _Atomic uint32_t monitorId;
    WindowCursorType cursorType;

    // Traffic-light chrome lives in the segregated TrafficLight class (window/
    // traffic_light.h); the window owns ONE instance and forwards the
    // Window_macOS_* / setFloatingTrafficLights surface to it (the Single
    // Class Per File Law). nullptr = never created (headless / BORDERLESS).
    TrafficLight *trafficLights;

    // The draw view sits ABOVE the content view, so a backdrop-blur effect view
    // can live behind it (behindWindow blur shows through the transparent parts).
    NSView *glView;        // where presents land (layer.contents)
    void *backdropView;    // NSVisualEffectView, or nullptr
    void *glassView;       // macOS 26 NSGlassEffectView, or nullptr
    WindowLiquidGlassDesc liquidGlass; // last-applied Liquid Glass descriptor

    WindowResizeRenderFn resizeRenderFn;
    void *resizeRenderUserdata;

    int undecoratedMode;     // WINDOW_DECORATED, WINDOW_UNDECORATED_NAKED, WINDOW_UNDECORATED_BORDERLESS
    bool viewportFlushToTop; // true when decorated window has viewport flushed into top bar
};

// Window id registry: slot i holds the entries for engine id i (index = id,
// slot 0 reserved for FOCUS_BROADCAST). Grows by doubling (the Dynamic
// Scalability & Anti-Hardcoding Law — no fixed window ceiling; the pump and
// the id scan are the only walkers, and live windows are few).
typedef struct WindowSlot {
    NSWindow *window;
    Window *handle;
} WindowSlot;

static WindowSlot *s_refs = nullptr;
static uint32_t s_refCap = 0;

// Grow the NSWindow-to-handle registry geometrically and initialize new slots.
static bool windowRefsGrow(void) {
    uint32_t nextCap = (s_refCap == 0) ? 8u : s_refCap * 2u;
    WindowSlot *grown = (WindowSlot*) realloc(s_refs, (size_t) nextCap * sizeof(WindowSlot));
    if (grown == nullptr)
        return false;
    for (uint32_t i = s_refCap; i < nextCap; i++) {
        grown[i].window = nil;
        grown[i].handle = nullptr;
    }
    s_refs = grown;
    s_refCap = nextCap;
    return true;
}

// Resolve an id for a newly created window, or 0 when the table cannot grow.
static uint32_t windowIdAcquire(NSWindow *window, Window *handle) {
    if (s_refCap == 0 && !windowRefsGrow())
        return FOCUS_BROADCAST;
    for (uint32_t i = 1; i < s_refCap; i++) {
        if (s_refs[i].window == nil) {
            s_refs[i].window = window;
            s_refs[i].handle = handle;
            return i;
        }
    }
    if (!windowRefsGrow())
        return FOCUS_BROADCAST;
    for (uint32_t i = 1; i < s_refCap; i++) {
        if (s_refs[i].window == nil) {
            s_refs[i].window = window;
            s_refs[i].handle = handle;
            return i;
        }
    }
    return FOCUS_BROADCAST;
}

// Clear the registry slot after native delegates and input routes are detached.
static void windowIdRelease(uint32_t id) {
    if (id != FOCUS_BROADCAST && id < s_refCap) {
        s_refs[id].window = nil;
        s_refs[id].handle = nullptr;
    }
}

// Reverse lookup: which engine id does this OS window carry? 0 if unknown.
static uint32_t windowIdOf(NSWindow *window) {
    if (window == nil)
        return FOCUS_BROADCAST;
    for (uint32_t i = 1; i < s_refCap; i++)
        if (s_refs[i].window == window)
            return i;
    return FOCUS_BROADCAST;
}

static Window *windowHandleOf(NSWindow *window) {
    if (window == nil)
        return nullptr;
    for (uint32_t i = 1; i < s_refCap; i++)
        if (s_refs[i].window == window)
            return s_refs[i].handle;
    return nullptr;
}

// Flipped content view so sublayers and Cocoa geometry natively speak
// TOP-LEFT coordinates (matching darling Container_resolve layout 1:1). Also
// tracks live-resize start/end onto the handle so Window_isLiveResizing works
// without any GPU state.
// setFrameSize: override: the SYNCHRONOUS per-step resize seam. AppKit calls
// setFrameSize: on the content view inside the live-resize tracking loop,
// immediately after the window frame changes and BEFORE the compositor draws
// the new bounds — the same beat the old VulkanView used. Firing the resize
// hook here closes the one-step gap: windowDidResize: (post-display) and the
// pump reflection remain as idempotent backups (windowRefreshSize no-ops when
// the rounded size is unchanged), but the drag-step chase now runs at geometry
// time, so layer frame + drawableSize + the forced present land in the same
// composite pass as the moved edge.
static void windowRefreshSize(Window *window);
static void windowRefreshOrigin(Window *window);
static CGFloat windowBackingScale(const Window *window);
@interface WindowContentView : NSView
@end

@implementation WindowContentView
/** Keep AppKit view coordinates in the engine's top-left origin convention. */
- (BOOL)isFlipped {
    return YES;
}

/** Publish each content-size step and invoke the resize render hook in the tracking turn. */
- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    Window *w = windowHandleOf([self window]);
    if (getenv("VEX_GEOMETRY_LOG") != nullptr)
        fprintf(stderr, "sf: %.0fx%.0f\n", newSize.width, newSize.height);
    if (w) {
        uint64_t previousSizeGeneration = Window_sizeGeneration(w);
        windowRefreshSize(w);
        // Repaint the WHOLE frame on EVERY geometry step, no exceptions. The
        // public resize event speaks rounded logical points, so a drag step
        // that crosses no rounding boundary still moves the seam's native
        // pixels and must repaint — and a step AppKit never marked as a live
        // resize must repaint too. The sizeGeneration compare fires the hook
        // exactly once per step: a rounded-point change already ran it inside
        // windowRefreshSize, every other step (fractional or non-live) runs it
        // here. The hook itself no-ops when the seam is already in sync, so a
        // redundant call is cheap; dropping a step is what leaves the trailing
        // edge stale (the Continuous Real-Time Live Resize Law).
        if (Window_sizeGeneration(w) == previousSizeGeneration &&
            !atomic_load_explicit(&(*w).miniaturizing, memory_order_relaxed) &&
            ![[self window] isMiniaturized] && (*w).resizeRenderFn)
            (*w).resizeRenderFn((*w).resizeRenderUserdata);
        // A drag off the top or left edge changes BOTH the content size and
        // the window origin in the same tracking tick; refresh the origin here
        // so onMoved fires per step alongside onResized (windowDidMove: is the
        // post-display backup, exactly as windowDidResize: is for size).
        windowRefreshOrigin(w);
    }
}

/** Mark the window as resizing before AppKit begins its live-resize callbacks. */
- (void)viewWillStartLiveResize {
    Window *w = windowHandleOf([self window]);
    if (w)
        atomic_store_explicit(&(*w).liveResizing, true, memory_order_relaxed);
    [super viewWillStartLiveResize];
}

/** Clear live-resize state and reconcile the final size and render hook. */
- (void)viewDidEndLiveResize {
    Window *w = windowHandleOf([self window]);
    if (w) {
        atomic_store_explicit(&(*w).liveResizing, false, memory_order_relaxed);
        windowRefreshSize(w);
        // Final geometry reconciliation only: each live step already publishes
        // its current extent. The Frame hook no-ops when this size is unchanged;
        // no frozen drawable, delayed rebuild or mandatory settle frame exists.
        if ((*w).resizeRenderFn)
            (*w).resizeRenderFn((*w).resizeRenderUserdata);
    }
    [super viewDidEndLiveResize];
}
@end

// App-level delegate: lets the process end when the last window closes.
@interface WindowAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation WindowAppDelegate
/** Allow the process to terminate after its final native window closes. */
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*) sender {
    (void) sender;
    return YES;
}
@end

// Window-level delegate: this is how we learn the user clicked the close
// button, resized, zoomed, or used the green orb. The pointers are (assign)
// because the delegate must not own our C struct.
@interface WindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) atomic_bool *shouldClosePtr;
@property(nonatomic, assign) Window *handlePtr;
@end

@implementation WindowDelegate
/** Ask lifecycle listeners whether AppKit should accept the close request. */
- (BOOL)windowShouldClose:(NSWindow*) sender {
    (void) sender;
    Window *w = self.handlePtr;
    if (w)
        return WindowEvent_fireQuitRequested(&(*w).lifecycle, w);
    return YES;
}

/** Set the C close latch when AppKit reports that the native window closed. */
- (void)windowWillClose:(NSNotification*) notification {
    (void) notification;
    if (self.shouldClosePtr)
        atomic_store_explicit(self.shouldClosePtr, true, memory_order_relaxed);
}

/** Revalidate physical size and monitor state after the window changes screens. */
- (void)windowDidChangeScreen:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w)
        Window_revalidate(w);
}

/** Revalidate pixel geometry after AppKit changes backing scale properties. */
- (void)windowDidChangeBackingProperties:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w)
        Window_revalidate(w);
}

/** Refresh cached size and origin after AppKit completes a resize notification. */
- (void)windowDidResize:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (getenv("VEX_GEOMETRY_LOG") != nullptr)
        fprintf(stderr, "dr:\n");
    if (w) {
        windowRefreshSize(w);
        windowRefreshOrigin(w);
    }
}

/** Refresh the cached screen origin after AppKit moves the window. */
- (void)windowDidMove:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (getenv("VEX_GEOMETRY_LOG") != nullptr)
        fprintf(stderr, "dm:\n");
    if (w)
        windowRefreshOrigin(w);
}

/** Mark fullscreen transition work as live resizing before AppKit animates. */
- (void)windowWillEnterFullScreen:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w)
        atomic_store_explicit(&(*w).liveResizing, true, memory_order_relaxed);
}

/** Publish final fullscreen geometry and notify lifecycle listeners. */
- (void)windowDidEnterFullScreen:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w) {
        atomic_store_explicit(&(*w).liveResizing, false, memory_order_relaxed);
        windowRefreshSize(w);
        windowRefreshOrigin(w);
        WindowEvent_fireFullscreen(&(*w).lifecycle, w);
    }
}

/** Mark fullscreen exit as live resizing until AppKit completes the transition. */
- (void)windowWillExitFullScreen:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w)
        atomic_store_explicit(&(*w).liveResizing, true, memory_order_relaxed);
}

/** Reconcile restored geometry, render once after transition, and notify listeners. */
- (void)windowDidExitFullScreen:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w) {
        atomic_store_explicit(&(*w).liveResizing, false, memory_order_relaxed);
        windowRefreshSize(w);
        windowRefreshOrigin(w);
        // Same settle beat as viewDidEndLiveResize: geometry changed while
        // the live flag was set, so force one hook pass with it clear.
        if ((*w).resizeRenderFn)
            (*w).resizeRenderFn((*w).resizeRenderUserdata);
        WindowEvent_fireRestored(&(*w).lifecycle, w);
    }
}

/** Mark the native minimize animation so resize rendering can be suppressed. */
- (void)windowWillMiniaturize:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w)
        atomic_store_explicit(&(*w).miniaturizing, true, memory_order_relaxed);
}

/** Clear transition state and publish the minimized lifecycle event. */
- (void)windowDidMiniaturize:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w) {
        atomic_store_explicit(&(*w).miniaturizing, false, memory_order_relaxed);
        WindowEvent_fireMinimized(&(*w).lifecycle, w);
    }
}

/** Mark the restore animation so intermediate geometry is not rendered. */
- (void)windowWillDeminiaturize:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w)
        atomic_store_explicit(&(*w).miniaturizing, true, memory_order_relaxed);
}

/** Clear transition state, refresh size, and publish restoration. */
- (void)windowDidDeminiaturize:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w) {
        atomic_store_explicit(&(*w).miniaturizing, false, memory_order_relaxed);
        windowRefreshSize(w);
        WindowEvent_fireRestored(&(*w).lifecycle, w);
    }
}

// Zoom-to-fill (green button / double-click titlebar): publish the lifecycle
// event before AppKit performs the standard-frame change.
/** Publish the zoom-fill lifecycle event before AppKit applies the target frame. */
- (BOOL)windowShouldZoom:(NSWindow*) sender toFrame:(NSRect)newFrame {
    (void) sender;
    (void) newFrame;
    Window *w = self.handlePtr;
    if (w)
        WindowEvent_fireZoomFilled(&(*w).lifecycle, w);
    return YES;
}

// Occlusion flip (buried under windows / minimized / hidden Space / ordered
// out): publish the lifecycle event with current visibility. An ordered-out
// window reads zero state bits, so it reports not-visible and the renderer
// rests. The notification itself is the flip signal — no extra cache.
/** Publish whether the native window is currently visible to the user. */
- (void)windowDidChangeOcclusionState:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w) {
        bool visible = false;
        if ((*w).nsWindow != nil)
            visible = ([(*w).nsWindow occlusionState] & NSWindowOcclusionStateVisible) != 0;
        WindowEvent_fireOcclusionChanged(&(*w).lifecycle, w, visible);
    }
}
@end

static CGFloat windowBackingScale(const Window *window) {
    if (window == nullptr || (*window).nsWindow == nil) {
        NSScreen *main = [NSScreen mainScreen];
        return main ? [main backingScaleFactor] : 1.0;
    }
    CGFloat s = [(*window).nsWindow backingScaleFactor];
    return (s > 0.0) ? s : 1.0;
}

// Resize reflection (Native Pixel Law): content dimensions are cached and
// fired in native physical display pixels. On Retina/HiDPI screens, convertRectToBacking
// provides the exact hardware pixel size. Thread 0 only.
static void windowRefreshSize(Window *window) {
    if (window == nullptr || (*window).nsWindow == nil)
        return;
    @autoreleasepool {
        NSWindow *nsw = (*window).nsWindow;
        NSView *cv = [nsw contentView];
        NSRect content = cv != nil ? [cv bounds] : [nsw contentRectForFrameRect:[nsw frame]];
        CGFloat scale = windowBackingScale(window);
        (*window).cachedScale = (double) scale;

        int cw, ch;
        if (cv != nil) {
            NSRect backing = [cv convertRectToBacking:content];
            cw = (int) lround(backing.size.width);
            ch = (int) lround(backing.size.height);
        } else {
            cw = (int) lround(content.size.width * scale);
            ch = (int) lround(content.size.height * scale);
        }

        int lastW = atomic_load_explicit(&(*window).cachedWidth, memory_order_relaxed);
        int lastH = atomic_load_explicit(&(*window).cachedHeight, memory_order_relaxed);
        if (cw != lastW || ch != lastH) {
            atomic_store_explicit(&(*window).cachedWidth, cw, memory_order_relaxed);
            atomic_store_explicit(&(*window).cachedHeight, ch, memory_order_relaxed);
            atomic_fetch_add_explicit(&(*window).sizeGeneration, 1, memory_order_release);
            WindowEvent_fireResized(&(*window).lifecycle, window, cw, ch);
            // Genie gate: never render+present while the WindowServer is
            // warping the window into or out of the dock — the seam present
            // would target a moving/occluded surface. The deminiaturize path
            // re-enters here AFTER the warp, so restore still renders.
            if (!atomic_load_explicit(&(*window).miniaturizing, memory_order_relaxed)
                    && ![nsw isMiniaturized]
                    && (*window).resizeRenderFn)
                (*window).resizeRenderFn((*window).resizeRenderUserdata);
        }
    }
}

// Move reflection: top-left screen coords (the same space Window_setLocation
// speaks) plus the CONTENT top-left (below the title bar) cached for
// Window_getContentOrigin. Fires onMoved only when the top-left actually
// changed, so a renderer polling once per frame pays two compares. Called from
// the per-step setFrameSize: seam (a top/left-edge resize moves the origin in
// the same tracking tick), from windowDidMove: (post-display backup), and from
// the pump reflection pass. Thread 0 only.
static void windowRefreshOrigin(Window *window) {
    if (window == nullptr || (*window).nsWindow == nil)
        return;
    @autoreleasepool {
        NSWindow *nsw = (*window).nsWindow;
        NSRect frame = [nsw frame];
        NSRect content = [nsw contentRectForFrameRect:frame];
        CGFloat screenHeight = [[NSScreen mainScreen] frame].size.height;
        double tx = (double) frame.origin.x;
        double ty = (double) (screenHeight - frame.origin.y - frame.size.height);
        NSView *cv = [nsw contentView];
        if (cv != nil) {
            NSRect cvRectInWindow = [cv frame];
            NSRect cvRectInScreen = [nsw convertRectToScreen:cvRectInWindow];
            (*window).cachedContentX = (double) cvRectInScreen.origin.x;
            (*window).cachedContentY = (double) (screenHeight - cvRectInScreen.origin.y - cvRectInScreen.size.height);
        } else {
            (*window).cachedContentX = (double) content.origin.x;
            (*window).cachedContentY = (double) (screenHeight - content.origin.y - content.size.height);
        }
        if (tx != (*window).cachedX || ty != (*window).cachedY) {
            (*window).cachedX = tx;
            (*window).cachedY = ty;
            WindowEvent_fireMoved(&(*window).lifecycle, window, (int) tx, (int) ty);
        }
    }
}

// Mirror the OS key-window into the focus word AND the WindowEvent focus
// slots (flips only). Thread 0 only, called at the end of each pump pass.
static void windowRefreshFocus(Window *window, NSWindow *keyWindow) {
    if (window == nullptr || (*window).nsWindow == nil)
        return;
    bool focused = (windowIdOf(keyWindow) == (*window).id);
    if (focused != (*window).lastFocused) {
        (*window).lastFocused = focused;
        if (focused)
            WindowEvent_fireFocusGained(&(*window).lifecycle, window);
        else
            WindowEvent_fireFocusLost(&(*window).lifecycle, window);
    }
}

// Mirror the OS display assignment into the atomic word; 0 means unmapped.
static uint32_t resolveMonitorId(Window *window) {
    if (window == nullptr)
        return 0;
    @autoreleasepool {
        NSScreen *screen = [(*window).nsWindow screen];
        if (screen == nil)
            return 0;
        NSNumber *displayId = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
        return displayId ? (uint32_t) [displayId unsignedIntValue] : 0;
    }
}

// Resolve and publish the screen identity currently carrying this window.
static void windowRefreshMonitor(Window *window) {
    if (window == nullptr)
        return;
    uint32_t next = resolveMonitorId(window);
    atomic_store_explicit(&(*window).monitorId, next, memory_order_relaxed);
}

// Re-centre the cursor during the pump while locked, so the warp registers
// before the next event loop exit.
static void recenterIfLocked(void) {
    if (s_cursorLocked)
        CGWarpMouseCursorPosition(s_lockCenter);
}

// Content-area coordinates (top-left origin) for a mouse event in physical pixels (Native Pixel Law).
// Events that miss every window fall back to raw screen-space values.
static void mouseLocation(NSEvent *event, double *outX, double *outY) {
    NSPoint p = [event locationInWindow];
    NSWindow *eventWindow = [event window];
    CGFloat scale = 1.0;
    if (eventWindow) {
        scale = [eventWindow backingScaleFactor];
        if (scale <= 0.0)
            scale = 1.0;
        NSView *cv = [eventWindow contentView];
        if (cv != nil) {
            NSPoint pt = [cv convertPoint:p fromView:nil];
            *outX = (double) pt.x * (double) scale;
            *outY = (double) pt.y * (double) scale;
            return;
        }
        NSRect content = [eventWindow contentRectForFrameRect:[eventWindow frame]];
        p.y = content.size.height - p.y;
    }
    *outX = (double) p.x * (double) scale;
    *outY = (double) p.y * (double) scale;
}

// Map an NSTouch phase onto the Touch_* action codes.
static int touchAction(NSTouchPhase phase) {
    if (phase & NSTouchPhaseBegan)
        return TOUCH_DOWN;
    if (phase & (NSTouchPhaseMoved | NSTouchPhaseStationary))
        return TOUCH_MOVE;
    if (phase & NSTouchPhaseEnded)
        return TOUCH_UP;
    return TOUCH_CANCEL;
}

// Feed one gesture/touch carrier event into input.Touch: resolve each active
// touch into its slot (identity hash % TOUCH_MAX), normalize position into
// content coords, and estimate pressure from the resting flag.
static void dispatchTouches(NSEvent *event, uint32_t wid) {
    NSWindow *eventWindow = [event window];
    if (eventWindow == nil)
        return;
    NSView *contentView = [eventWindow contentView];
    if (contentView == nil)
        return;

    NSSet *touches = [event touchesMatchingPhase:NSTouchPhaseAny inView:contentView];
    if (touches == nil || touches.count == 0)
        return;

    NSRect frame = [contentView frame];
    double winW = frame.size.width;
    double winH = frame.size.height;

    for (NSTouch *touch in touches) {
        NSUInteger slot = [[touch identity] hash] % TOUCH_MAX;
        NSPoint norm = [touch normalizedPosition];
        double posX = norm.x * winW;
        double posY = (1.0 - norm.y) * winH;
        double pressure = [touch isResting] ? 0.2 : 0.8;
        Touch_pushTouchEvent(wid, (int) slot, touchAction([touch phase]),
                             posX, posY, pressure, kTapThresholdNanos);
    }
}

// Intercept-and-forward: read everything the engine cares about off each OS
// event, push it into the input modules, then hand the event back to AppKit
// so the responder chain keeps working. This is the Thread-0 producer side of
// the input pipeline.
static void routeEvent(NSEvent *event) {
    NSEventType type = [event type];
    uint32_t wid = windowIdOf([event window]);

    // Disabled windows receive nothing: events never enter the device rings.
    Window *target = (wid != FOCUS_BROADCAST) ? windowHandleOf([event window]) : nullptr;
    if (target && !atomic_load_explicit(&(*target).enabled, memory_order_relaxed))
        return;

    switch (type) {
        case NSEventTypeFlagsChanged: {
            short macCode = [event keyCode];
            NSEventModifierFlags flags = [event modifierFlags];
            int stdKey = (macCode >= 0 && macCode < 128) ? macKeyMap[macCode] : -1;
            if (stdKey != -1) {
                bool isDown = false;
                if (macCode == 54 || macCode == 55) {
                    isDown = (flags & NSEventModifierFlagCommand) != 0;
                } else if (macCode == 56 || macCode == 60) {
                    isDown = (flags & NSEventModifierFlagShift) != 0;
                } else if (macCode == 58 || macCode == 61) {
                    isDown = (flags & NSEventModifierFlagOption) != 0;
                } else if (macCode == 59 || macCode == 62) {
                    isDown = (flags & NSEventModifierFlagControl) != 0;
                } else if (macCode == 57) {
                    isDown = (flags & NSEventModifierFlagCapsLock) != 0;
                }
                Key_pushEvent(wid, stdKey,
                              isDown ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                              kTapThresholdNanos);
            }
            break;
        }

        case NSEventTypeKeyDown:
        case NSEventTypeKeyUp: {
            NSEventModifierFlags flags = [event modifierFlags];
            bool cmd = (flags & NSEventModifierFlagCommand) != 0;
            if (cmd != (Key_isDown(KEY_LEFT_SUPER) || Key_isDown(KEY_RIGHT_SUPER))) {
                Key_pushEvent(wid, KEY_LEFT_SUPER, cmd ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
            }
            bool ctrl = (flags & NSEventModifierFlagControl) != 0;
            if (ctrl != (Key_isDown(KEY_LEFT_CONTROL) || Key_isDown(KEY_RIGHT_CONTROL))) {
                Key_pushEvent(wid, KEY_LEFT_CONTROL, ctrl ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
            }
            bool alt = (flags & NSEventModifierFlagOption) != 0;
            if (alt != (Key_isDown(KEY_LEFT_ALT) || Key_isDown(KEY_RIGHT_ALT))) {
                Key_pushEvent(wid, KEY_LEFT_ALT, alt ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
            }
            bool shift = (flags & NSEventModifierFlagShift) != 0;
            if (shift != (Key_isDown(KEY_LEFT_SHIFT) || Key_isDown(KEY_RIGHT_SHIFT))) {
                Key_pushEvent(wid, KEY_LEFT_SHIFT, shift ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
            }
            short macCode = [event keyCode];
            if (macCode >= 0 && macCode < 128) {
                int stdKey = macKeyMap[macCode];
                if (stdKey != -1)
                    Key_pushEvent(wid, stdKey,
                                  type == NSEventTypeKeyDown ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                                  kTapThresholdNanos);
                if (type == NSEventTypeKeyDown && stdKey != -1) {
                    NSString *chars = [event characters];
                    if (chars.length > 0) {
                        unsigned char c0 = (unsigned char) [chars characterAtIndex:0];
                        if (c0 > 0)
                            Key_pushCharEvent(wid, c0);
                    }
                }
            }
            break;
        }

        case NSEventTypeScrollWheel: {
            // A scroll can be the first event after entering the window. Feed
            // its own pointer location before the wheel delta so hit-testing
            // does not rely on an earlier MouseMoved event.
            double x, y;
            mouseLocation(event, &x, &y);
            Mouse_pushMoveEvent(wid, x, y);
            Mouse_pushScrollEvent(wid, [event scrollingDeltaX], [event scrollingDeltaY]);
            break;
        }

        case NSEventTypeMagnify:
            Mouse_pushZoomEvent(wid, [event magnification]);
            break;

        case NSEventTypeLeftMouseDown:
        case NSEventTypeRightMouseDown:
        case NSEventTypeOtherMouseDown: {
            int button = (type == NSEventTypeLeftMouseDown) ? MOUSE_LEFT
                       : ((type == NSEventTypeRightMouseDown) ? MOUSE_RIGHT
                                                              : (int) [event buttonNumber]);
            double x, y;
            mouseLocation(event, &x, &y);
            Mouse_pushMoveEvent(wid, x, y);
            Mouse_pushButtonEvent(wid, button, KEY_ACTION_DOWN, kTapThresholdNanos);
            if (target)
                WindowEvent_firePressed(&(*target).lifecycle, target);
            break;
        }

        case NSEventTypeLeftMouseUp:
        case NSEventTypeRightMouseUp:
        case NSEventTypeOtherMouseUp: {
            int button = (type == NSEventTypeLeftMouseUp) ? MOUSE_LEFT
                       : ((type == NSEventTypeRightMouseUp) ? MOUSE_RIGHT
                                                             : (int) [event buttonNumber]);
            double x, y;
            mouseLocation(event, &x, &y);
            Mouse_pushMoveEvent(wid, x, y);
            Mouse_pushButtonEvent(wid, button, KEY_ACTION_UP, kTapThresholdNanos);
            break;
        }

        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged: {
            if (s_cursorLocked) {
                CGFloat scale = [event window] ? [[event window] backingScaleFactor] : 1.0;
                if (scale <= 0.0)
                    scale = 1.0;
                Mouse_pushMoveDeltaEvent(wid, [event deltaX] * (double) scale, [event deltaY] * (double) scale);
                break;
            }
            double x, y;
            mouseLocation(event, &x, &y);
            if (type == NSEventTypeMouseMoved) {
                Mouse_pushMoveEvent(wid, x, y);
            } else {
                int button = (type == NSEventTypeLeftMouseDragged) ? MOUSE_LEFT
                           : ((type == NSEventTypeRightMouseDragged) ? MOUSE_RIGHT
                                                                      : (int) [event buttonNumber]);
                Mouse_pushDragEvent(wid, button, x, y);
            }
            break;
        }

        case NSEventTypeGesture:
        case NSEventTypeBeginGesture:
        case NSEventTypeEndGesture:
            dispatchTouches(event, wid);
            break;

        default:
            break;
    }
}

// Drain the OS event queue. Called every frame from the engine loop (the
// "poll" half of poll-then-tick). Returns immediately; never blocks. After
// the pump, mirror the OS's key window into the focus word so the rest of
// the engine can ask "who is focused?" without touching AppKit.
static NSWindow *sLastWindow = nil;
// Key-window claim pending: set by Window_show/Window_focus, drained by the
// pump once the app is active and the claim sticks.
static NSWindow *sPendingKeyWindow = nil;
static WindowAppDelegate *sAppDelegate = nil; // one app delegate for the whole process

// Drain AppKit events non-blockingly, optionally stopping after one event.
static bool windowPollEvents(bool singleStep) {
    bool more = false;
    @autoreleasepool {
        NSEvent *event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate distantPast]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES])) {
            routeEvent(event);
            [NSApp sendEvent:event];
            if (singleStep)
                break;
        }
        if (singleStep)
            more = [NSApp nextEventMatchingMask:NSEventMaskAny
                                      untilDate:[NSDate distantPast]
                                         inMode:NSDefaultRunLoopMode
                                        dequeue:NO] != nil;
        [NSApp updateWindows];
        recenterIfLocked();
        Focus_set(windowIdOf([NSApp keyWindow]));

        if (sPendingKeyWindow) {
            if ([NSApp keyWindow] == sPendingKeyWindow) {
                sPendingKeyWindow = nil;
            } else if ([NSApp isActive] && [sPendingKeyWindow canBecomeKeyWindow]) {
                [sPendingKeyWindow makeKeyWindow];
                if ([NSApp keyWindow] == sPendingKeyWindow)
                    sPendingKeyWindow = nil;
            }
        }

        // Reflection pass: size/move/focus/monitor mirroring per live window.
        NSWindow *keyWindow = [NSApp keyWindow];
        for (uint32_t i = 1; i < s_refCap; i++) {
            NSWindow *w = s_refs[i].window;
            if (w == nil)
                continue;
            Window *handle = s_refs[i].handle;
            if (handle == nullptr)
                continue;
            windowRefreshSize(handle);

            // Move reflection: top-left screen coords, same space
            // Window_setLocation speaks; fires onMoved on a real change.
            windowRefreshOrigin(handle);

            // Focus flip: mirror the OS spotlight into the WindowEvent slots.
            windowRefreshFocus(handle, keyWindow);
            // Monitor mirror: resolve the carrying display, flip the atomic.
            windowRefreshMonitor(handle);
        }
    }
    return more;
}

// Drain the process-wide AppKit queue and refresh registered window state.
void Window_pollEvents(void) {
    (void) windowPollEvents(false);
}

// Dispatch at most one AppKit event and report whether another remains queued.
bool Window_pollEventStep(void) {
    return windowPollEvents(true);
}

// Wait for one process-wide AppKit event for the requested bounded interval.
void Window_waitEvents(Window *window, int timeoutMs) {
    (void) window;   // the AppKit event pump is process-wide
    @autoreleasepool {
        NSDate *until = timeoutMs > 0
            ? [NSDate dateWithTimeIntervalSinceNow:(double) timeoutMs / 1000.0]
            : [NSDate distantFuture];
        NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:until
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
        if (event != nil) {
            routeEvent(event);
            [NSApp sendEvent:event];
        }
        [NSApp updateWindows];
        recenterIfLocked();
        Focus_set(windowIdOf([NSApp keyWindow]));
    }
}

// Key-gate window subclass: while a modal dialog holds its parent, the
// parent's C flag flips and AppKit itself refuses it key — no focus flash,
// no input gap, no yank-back. Render + ordering are untouched (only key).
@interface VexWindow : NSWindow
@property (assign, nonatomic) Window *vexHandle;
@end

@implementation VexWindow
/** Respect the C key-focus gate before allowing AppKit key-window eligibility. */
- (BOOL)canBecomeKeyWindow {
    Window *h = self.vexHandle;
    if (h != nullptr && !atomic_load_explicit(&(*h).keyEnabled, memory_order_relaxed))
        return NO;
    return [super canBecomeKeyWindow];
}

// The standard titlebar strip height (points). A FullSizeContentView window
// reports a zero titlebar inset, so fall back to the platform's ~28pt strip.
- (CGFloat)vexTitlebarHeight {
    NSRect frame = self.frame;
    NSRect content = [self contentRectForFrameRect:frame];
    NSSize frameSize = frame.size;
    NSSize contentSize = content.size;
    CGFloat h = frameSize.height - contentSize.height;
    return h > 0.0 ? h : 28.0;
}

// Titlebar double-click is the accessibility affordance Windows ships as the
// maximize (square) button, and macOS ships as the green zoom. On a NAKED /
// full-size-content window the title bar is transparent, so AppKit's own
// double-click target is nearly unhittable — catch it explicitly. A plain
// double-click zooms (fills the visible frame, the macOS maximize); an
// Option-double-click enters fullscreen. Both are no-ops if already there.
//
// The resize border keeps its own behavior: when the pointer is on an edge or
// corner (the up-down / diagonal resize cursor), the double-click stays a
// directional resize and we do not intercept — only the title-bar interior
// (normal cursor) maximizes.
static const CGFloat kWindowResizeBorderPoints = 6.0;

/** Route titlebar double-click gestures before ordinary AppKit event delivery. */
- (void)sendEvent:(NSEvent*) event {
    if (event.type == NSEventTypeLeftMouseDown && event.clickCount == 2) {
        NSPoint p = [event locationInWindow];
        NSSize size = self.frame.size;
        BOOL onResizeBorder = p.x <= kWindowResizeBorderPoints
                           || p.x >= size.width - kWindowResizeBorderPoints
                           || p.y <= kWindowResizeBorderPoints
                           || p.y >= size.height - kWindowResizeBorderPoints;
        CGFloat titlebar = [self vexTitlebarHeight];
        if (!onResizeBorder && p.y >= size.height - titlebar) {
            if ((event.modifierFlags & NSEventModifierFlagOption) != 0)
                [self toggleFullScreen:self];
            else
                [self performZoom:self];
            return;
        }
    }
    [super sendEvent:event];
}
@end

// Build the NSWindow + C handle. Shared by every constructor. The window is
// created HIDDEN — visibility is an explicit Window_show() decision, so
// construct -> mutate -> show never flashes a half-configured window.
// Initial placement also happens here (not via a post-creation move):
// creating already-placed keeps AppKit's move tracker quiet — a
// setFrameOrigin: on a never-shown window logs "move completed
// without beginning".
static Window *windowAlloc(const WindowDesc *desc) {
    @autoreleasepool {
        if (NSApp == nil)
            [NSApplication sharedApplication];
        if (sAppDelegate == nil) {
            sAppDelegate = [[WindowAppDelegate alloc] init];
            [NSApp setDelegate:sAppDelegate];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        }

        NSWindowStyleMask style = NSWindowStyleMaskTitled
                                | NSWindowStyleMaskClosable
                                | NSWindowStyleMaskMiniaturizable
                                | NSWindowStyleMaskResizable;

        // Native Pixel Law: desc->width and desc->height are in native physical display pixels.
        // Convert to AppKit user points using the target screen's backingScaleFactor.
        CGFloat scale = [NSScreen mainScreen] ? [[NSScreen mainScreen] backingScaleFactor] : 1.0;
        if (scale <= 0.0)
            scale = 1.0;
        CGFloat cw = (CGFloat)(*desc).width / scale;
        CGFloat ch = (CGFloat)(*desc).height / scale;
        NSRect wantFrame = [NSWindow frameRectForContentRect:NSMakeRect(0, 0, cw, ch)
                                                  styleMask:style];
        if ((*desc).centered || ((*desc).x == 0 && (*desc).y == 0)) {
            NSRect avail = [[NSScreen mainScreen] visibleFrame];
            wantFrame.origin.x = avail.origin.x + (avail.size.width - wantFrame.size.width) * 0.5;
            wantFrame.origin.y = avail.origin.y + (avail.size.height - wantFrame.size.height) * 0.5;
        } else {
            NSRect screen = [[NSScreen mainScreen] frame];
            wantFrame.origin.x = (CGFloat)(*desc).x;
            wantFrame.origin.y = screen.size.height - (CGFloat)(*desc).y - wantFrame.size.height;
        }
        wantFrame.origin.x = (CGFloat) floor(wantFrame.origin.x + 0.5);
        wantFrame.origin.y = (CGFloat) floor(wantFrame.origin.y + 0.5);
        NSRect frame = [NSWindow contentRectForFrameRect:wantFrame
                                              styleMask:style];

        VexWindow *window = [[VexWindow alloc]
            initWithContentRect:frame
                       styleMask:style
                          backing:NSBackingStoreBuffered
                            defer:NO];
        [window setTitle:[NSString stringWithUTF8String:(*desc).title]];
        [window setReleasedWhenClosed:NO];

        WindowContentView *contentView = [[WindowContentView alloc] initWithFrame:frame];
        [contentView setWantsLayer:YES];
        [window setContentView:contentView];

        // A dedicated draw view on TOP of the content view. Presents land here;
        // a backdrop-blur effect view (Window_setBackdropBlur) sits behind it.
        NSView *glView = [[NSView alloc] initWithFrame:contentView.bounds];
        [glView setWantsLayer:YES];
        [glView setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
        [contentView addSubview:glView];

        // Green traffic light enters native fullscreen.
        [window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];

        // Live resize never stretches preserved content (the Continuous
        // Real-Time Live Resize Law).
        [window setPreservesContentDuringLiveResize:NO];

        // Hover delivery: without this, MouseMoved only fires mid-drag and
        // darling hover is dead on arrival. The Mouse ring gets every move.
        [window setAcceptsMouseMovedEvents:YES];

        // Trackpad touch delivery: the content view must opt in.
        [window.contentView setAllowedTouchTypes:NSTouchTypeMaskDirect | NSTouchTypeMaskIndirect];

        WindowDelegate *delegate = [[WindowDelegate alloc] init];
        [window setDelegate:delegate];

        Window *w = (Window*) calloc(1, sizeof(Window));
        if (w == nullptr)
            return nullptr;
        (*w).nsWindow = window;
        window.vexHandle = w;
        (*w).glView = glView;
        (*w).backdropView = nullptr;
        (*w).glassView = nullptr;
        (*w).delegate = delegate;
        WindowEvent_init(&(*w).lifecycle);
        atomic_store_explicit(&(*w).shouldClose, false, memory_order_relaxed);
        atomic_store_explicit(&(*w).sizeGeneration, 0, memory_order_relaxed);
        CGFloat winScale = [window backingScaleFactor];
        if (winScale <= 0.0)
            winScale = scale;
        (*w).cachedScale = (double) winScale;
        NSRect initialContent = [window contentRectForFrameRect:[window frame]];
        int initPxW = (int) lround(initialContent.size.width * winScale);
        int initPxH = (int) lround(initialContent.size.height * winScale);
        atomic_store_explicit(&(*w).cachedWidth, initPxW, memory_order_relaxed);
        atomic_store_explicit(&(*w).cachedHeight, initPxH, memory_order_relaxed);
        NSRect initialFrame = [window frame];
        CGFloat screenH = [[NSScreen mainScreen] frame].size.height;
        (*w).cachedX = (double) initialFrame.origin.x;
        (*w).cachedY = (double) (screenH - initialFrame.origin.y - initialFrame.size.height);
        (*w).cachedContentX = (double) initialFrame.origin.x
                              + (double) (initialContent.origin.x - initialFrame.origin.x);
        (*w).cachedContentY = (double) (screenH - initialContent.origin.y - initialContent.size.height);
        atomic_store_explicit(&(*w).presentMode, WINDOW_PRESENT_FIFO, memory_order_relaxed);
        atomic_store_explicit(&(*w).transparent, false, memory_order_relaxed);
        atomic_store_explicit(&(*w).renderGeneration, 0, memory_order_relaxed);
        atomic_store_explicit(&(*w).enabled, true, memory_order_relaxed);
        atomic_store_explicit(&(*w).keyEnabled, true, memory_order_relaxed);
        (*w).lastFocused = false;
        atomic_store_explicit(&(*w).monitorId, 0, memory_order_relaxed);
        (*w).cursorType = WINDOW_CURSOR_DEFAULT;
        // Traffic-light chrome controller (all three lights visible by default).
        // Created against the NSWindow; nullptr only if calloc fails.
        (*w).trafficLights = TrafficLight_create((__bridge void*) window);
        (*w).resizeRenderFn = nullptr;
        (*w).resizeRenderUserdata = nullptr;
        (*w).id = windowIdAcquire(window, w);
        delegate.shouldClosePtr = &(*w).shouldClose;
        delegate.handlePtr = w;

        sLastWindow = window;

        return w;
    }
}

// Merge a caller's Desc over the defaults. Unset (zero) fields fall back —
// this is what makes partial designated initializers behave like overloads.
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
        // Explicit placement defeats the centered default (a zeroed Desc
        // cannot express "top-left", so centered wins ties — documented).
        d.x = (*desc).x;
        d.y = (*desc).y;
        d.centered = false;
    }
    d.shown = (*desc).shown;
    return d;
}

// --- Constructors -----------------------------------------------------------

// Construct a default AppKit window through the common descriptor path.
Window *Window_0(void) {
    return Window_new(nullptr);
}

// Construct a window descriptor with the requested title and default dimensions.
Window *Window_1(const char *title) {
    return Window_new(&(WindowDesc){ .title = title });
}

// Construct a window descriptor with a title and explicit content dimensions.
Window *Window_3(const char *title, int width, int height) {
    return Window_new(&(WindowDesc){ .title = title, .width = width, .height = height });
}

// Resolve descriptor defaults, allocate the AppKit window, and show on request.
Window *Window_new(const WindowDesc *desc) {
    WindowDesc d = descResolve(desc);
    Window *w = windowAlloc(&d);
    if (w == nullptr)
        return nullptr;
    // Placement already happened inside windowAlloc (centered default or the
    // caller's x/y) — no post-creation move, so AppKit's move tracker stays
    // quiet. Window_center/Window_setLocation remain for later re-placement
    // of a live window.
    if (d.shown)
        Window_show(w);
    return w;
}

// Preserve the legacy constructor spelling by forwarding to Window_new.
Window *Window_create(const char *title, int width, int height) {
    return Window_new(&(WindowDesc){ .title = title, .width = width, .height = height });
}

// Tear down the window and free the handle. Safe to call whether the user
// already closed the window or not: if it's still open we close it, and we
// detach the delegate first so no callback can touch our freed memory.
void Window_destroy(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        if (sPendingKeyWindow == (*window).nsWindow)
            sPendingKeyWindow = nil;
        [(*window).nsWindow setDelegate:nil];
        [(*window).nsWindow close]; // flag-only close requests may still have a native window
        Key_detachWindowAll((*window).id);
        Mouse_detachWindowAll((*window).id);
        Touch_detachWindowAll((*window).id);
        TrafficLight_destroy((*window).trafficLights);
        windowIdRelease((*window).id);
    }
    free(window);
}

// Destroy each registered window while its delegate and handle are still valid.
void Window_destroyAll(void) {
    if (s_refs == nullptr || s_refCap == 0)
        return;
    for (uint32_t i = 1; i < s_refCap; i++) {
        Window *handle = s_refs[i].handle;
        if (handle != nullptr) {
            Window_destroy(handle);
        }
    }
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

// Ask AppKit to close the native window and set the C-side close latch.
void Window_close(Window *window) {
    if (!window) return;
    @autoreleasepool { [(*window).nsWindow close]; }
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

// Update the stored transparency policy and advance the render generation on change.
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

// Read the policy generation used by downstream render-resource rebuilds.
uint64_t Window_renderGeneration(const Window *window) {
    return window ? atomic_load_explicit(&(*window).renderGeneration, memory_order_acquire) : 0;
}

// --- Graphics board slots (stored + ordered; rendering lives elsewhere) -----

// ;;INTENTION("Graphics board slots and layer ordering are GPU-era stubs retired per the Window Decoupling Law. A Window is a dumb presentation surface; graphics layers and surfaces are managed by graphvex.")
void Window_orderLayers(Window *window) {
    (void) window;
}

// Retain the retired lower-layer setter as an inert compatibility seam.
void Window_setBottomLayer(Window *window, void *layer) {
    (void) window;
    (void) layer;
}

// Return no layer because the lean window no longer owns graphics layers.
void *Window_getBottomLayer(const Window *window) {
    (void) window;
    return nullptr;
}

// Retain the retired upper-layer setter as an inert compatibility seam.
void Window_setTopLayer(Window *window, void *layer) {
    (void) window;
    (void) layer;
}

// Return no layer because the lean window no longer owns graphics layers.
void *Window_getTopLayer(const Window *window) {
    (void) window;
    return nullptr;
}

// ;;INTENTION("Pane/board compositing was the GPU-era shim's job; on this window it is inert (still-unmigrated darling compositor retains the call). Pane work migrates to the render personal' own pass; retires together with the composite seam.")
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

// Read whether OS input forwarding is enabled for this window.
bool Window_isEnabled(const Window *window) {
    return window ? atomic_load_explicit(&(*window).enabled, memory_order_relaxed) : false;
}

// Read live-resize state, including the file-local diagnostic override seam.
bool Window_isLiveResizing(const Window *window) {
    // LOCAL DIAGNOSTIC SEAM ONLY (never commit): /tmp/vex_live existing
    // forces live mode so the modal-drag path reproduces without a mouse.
    FILE *probe = fopen("/tmp/vex_live", "r");
    if (probe) {
        fclose(probe);
        return window != nullptr;
    }
    return window ? atomic_load_explicit(&(*window).liveResizing, memory_order_relaxed) : false;
}

// --- Chrome / state API -----------------------------------------------------

static NSWindowStyleMask styleMaskOf(Window *window) {
    return [(*window).nsWindow styleMask];
}

// Test whether the native window's current style mask contains the requested bit.
static bool hasStyleBit(Window *window, NSWindowStyleMask bit) {
    return (styleMaskOf(window) & bit) != 0;
}

// Single mask-rewrite path for all capability toggles. While native fullscreen
// AppKit owns the mask, so style mutations are skipped then.
static void updateStyleMask(Window *window, NSWindowStyleMask add, NSWindowStyleMask clear) {
    NSWindowStyleMask mask = styleMaskOf(window);
    if ((mask & NSWindowStyleMaskFullScreen) != 0)
        return;
    [(*window).nsWindow setStyleMask:(mask & ~clear) | add];
}

// Re-apply the traffic-light chrome through the segregated TrafficLight class
// (window/traffic_light.h): per-button visibility, then re-seat the cluster
// offset relative to the native layout (snapshotted on first touch; the
// snapshot is invalidated whenever the style mask changes). Pure public AppKit
// (standardWindowButton: + setHidden:/setFrameOrigin:) — no private API.
// BORDERLESS windows own no lights to re-seat. Null-safe.
static void refreshChrome(Window *w) {
    if (w == nullptr)
        return;
    TrafficLight_refresh((*w).trafficLights);
}

// Set the AppKit title and reapply hidden-title chrome when required.
void Window_setTitle(Window *window, const char *title) {
    if (window == nullptr || title == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setTitle:[NSString stringWithUTF8String:title]];

        // macOS 15+ re-reveals the native title view whenever the title string
        // changes, even when titleVisibility is hidden. Re-apply the hidden
        // state for FullSizeContentView (NAKED) windows, then re-seat the
        // traffic-light chrome so a title change can't resurrect hidden lights.
        if ((styleMaskOf(window) & NSWindowStyleMaskFullSizeContentView) != 0) {
            [(*window).nsWindow setTitlebarAppearsTransparent:YES];
            [(*window).nsWindow setTitleVisibility:NSWindowTitleHidden];
        }
        refreshChrome(window);
    }
}

// Read the cached content width in native physical pixels.
int Window_width(Window *window) {
    if (window == nullptr)
        return 0;
    return atomic_load_explicit(&(*window).cachedWidth, memory_order_relaxed);
}

// Read the cached content height in native physical pixels.
int Window_height(Window *window) {
    if (window == nullptr)
        return 0;
    return atomic_load_explicit(&(*window).cachedHeight, memory_order_relaxed);
}

// Convert requested physical dimensions to AppKit points and resize the content.
void Window_setSize(Window *window, int width, int height) {
    if (window == nullptr || width <= 0 || height <= 0)
        return;
    @autoreleasepool {
        CGFloat scale = windowBackingScale(window);
        CGFloat ptW = (CGFloat) width / scale;
        CGFloat ptH = (CGFloat) height / scale;
        [(*window).nsWindow setContentSize:NSMakeSize(ptW, ptH)];
        windowRefreshSize(window);
    }
}

// Return the current AppKit backing scale factor for the window or main screen.
float Window_getScale(const Window *window) {
    return (float) windowBackingScale(window);
}

// Reconcile scale, physical size, screen origin, and monitor after display changes.
void Window_revalidate(Window *window) {
    if (window == nullptr || (*window).nsWindow == nil)
        return;
    @autoreleasepool {
        CGFloat curScale = windowBackingScale(window);
        double prevScale = (*window).cachedScale;
        if (prevScale <= 0.0)
            prevScale = curScale;

        // If the scale factor changed (e.g. dragged from 2.0x Retina to 1.0x external monitor),
        // adjust the points content size so the target physical pixel size is preserved.
        int targetPxW = atomic_load_explicit(&(*window).cachedWidth, memory_order_relaxed);
        int targetPxH = atomic_load_explicit(&(*window).cachedHeight, memory_order_relaxed);
        if (curScale != prevScale && targetPxW > 0 && targetPxH > 0) {
            CGFloat newPtW = (CGFloat) targetPxW / curScale;
            CGFloat newPtH = (CGFloat) targetPxH / curScale;
            [(*window).nsWindow setContentSize:NSMakeSize(newPtW, newPtH)];
        }

        (*window).cachedScale = (double) curScale;
        windowRefreshSize(window);
        windowRefreshOrigin(window);
        windowRefreshMonitor(window);
    }
}

// Set content dimensions directly in AppKit points and refresh pixel geometry.
void Window_setSizePoints(Window *window, float width, float height) {
    if (window == nullptr || width <= 0.0f || height <= 0.0f)
        return;
    @autoreleasepool {
        [(*window).nsWindow setContentSize:NSMakeSize((CGFloat) width, (CGFloat) height)];
        windowRefreshSize(window);
    }
}

// Read content dimensions in points through optional output parameters.
void Window_getSizePoints(const Window *window, float *outWidth, float *outHeight) {
    if (window == nullptr || (*window).nsWindow == nil) {
        if (outWidth) *outWidth = 0.0f;
        if (outHeight) *outHeight = 0.0f;
        return;
    }
    @autoreleasepool {
        NSView *cv = [(*window).nsWindow contentView];
        NSRect content = cv != nil ? [cv bounds] : [(*window).nsWindow contentRectForFrameRect:[(*window).nsWindow frame]];
        if (outWidth)
            *outWidth = (float) content.size.width;
        if (outHeight)
            *outHeight = (float) content.size.height;
    }
}

// Return the content width in points through the shared size getter.
float Window_widthPoints(const Window *window) {
    float w = 0.0f;
    Window_getSizePoints(window, &w, nullptr);
    return w;
}

// Return the content height in points through the shared size getter.
float Window_heightPoints(const Window *window) {
    float h = 0.0f;
    Window_getSizePoints(window, nullptr, &h);
    return h;
}

// Alias the physical content width as the viewport width.
int Window_viewportWidth(const Window *window) {
    return Window_width((Window*) window);
}

// Alias the physical content height as the viewport height.
int Window_viewportHeight(const Window *window) {
    return Window_height((Window*) window);
}

// Alias the logical content width as the viewport width in points.
float Window_viewportWidthPoints(const Window *window) {
    return Window_widthPoints(window);
}

// Alias the logical content height as the viewport height in points.
float Window_viewportHeightPoints(const Window *window) {
    return Window_heightPoints(window);
}

// Return the outer NSWindow frame width in native pixels.
int Window_windowWidth(const Window *window) {
    if (window == nullptr || (*window).nsWindow == nil)
        return 0;
    @autoreleasepool {
        NSRect frame = [(*window).nsWindow frame];
        CGFloat scale = windowBackingScale(window);
        return (int) lround(frame.size.width * scale);
    }
}

// Return the outer NSWindow frame height in native pixels.
int Window_windowHeight(const Window *window) {
    if (window == nullptr || (*window).nsWindow == nil)
        return 0;
    @autoreleasepool {
        NSRect frame = [(*window).nsWindow frame];
        CGFloat scale = windowBackingScale(window);
        return (int) lround(frame.size.height * scale);
    }
}

// Return the outer NSWindow frame width in AppKit points.
float Window_windowWidthPoints(const Window *window) {
    if (window == nullptr || (*window).nsWindow == nil)
        return 0.0f;
    @autoreleasepool {
        return (float) [(*window).nsWindow frame].size.width;
    }
}

// Return the outer NSWindow frame height in AppKit points.
float Window_windowHeightPoints(const Window *window) {
    if (window == nullptr || (*window).nsWindow == nil)
        return 0.0f;
    @autoreleasepool {
        return (float) [(*window).nsWindow frame].size.height;
    }
}

// Place the window by its top-left screen coordinate in the public pixel space.
void Window_setLocation(Window *window, int x, int y) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        NSRect screen = [[NSScreen mainScreen] frame];
        [(*window).nsWindow setFrameTopLeftPoint:NSMakePoint((CGFloat) x, screen.size.height - (CGFloat) y)];
    }
}

// Return the cached top-left screen origin through optional output pointers.
void Window_getLocation(const Window *window, int *outX, int *outY) {
    if (outX)
        *outX = window ? (int) (*window).cachedX : 0;
    if (outY)
        *outY = window ? (int) (*window).cachedY : 0;
}

// Return the cached content-view screen origin through optional outputs.
void Window_getContentOrigin(const Window *window, int *outX, int *outY) {
    if (outX)
        *outX = window ? (int) (*window).cachedContentX : 0;
    if (outY)
        *outY = window ? (int) (*window).cachedContentY : 0;
}

// Center the window in the main display's usable visible frame.
void Window_center(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        // Explicit _main-screen centering (not [nsWindow center]'s
        // current-screen heuristic): the window lands in the usable area,
        // clear of the menu bar and Dock.
        NSRect avail = [[NSScreen mainScreen] visibleFrame];
        NSRect wf = [(*window).nsWindow frame];
        CGFloat x = avail.origin.x + (avail.size.width - wf.size.width) * 0.5;
        CGFloat y = avail.origin.y + (avail.size.height - wf.size.height) * 0.5;
        [(*window).nsWindow setFrameOrigin:NSMakePoint((CGFloat) floor(x + 0.5), (CGFloat) floor(y + 0.5))];
    }
}

// Claim key synchronously: activation is async, so spin the runloop briefly
// until [NSApp isActive] commits, then open animated + take key while the
// grant is fresh. Falls back to the pump's sPendingKeyWindow re-assertion if
// activation is denied outright.
static void claimKeyAfterActivation(NSWindow *nsw) {
    if (nsw == nil)
        return;
    // Foreground claim: a fresh open (e.g. launched from an IDE/debugger)
    // must take focus instead of landing behind the currently-active app.
    // (The old IgnoringOtherApps flag is deprecated with no effect on
    // macOS 14+; NSApp.activate is its sanctioned replacement.)
    if (@available(macOS 14.0, *))
        [NSApp activate];
    else
        [[NSRunningApplication currentApplication]
            activateWithOptions:NSApplicationActivateAllWindows];
    // Show FIRST so the window appears instantly instead of waiting behind
    // the activation spin below. makeKeyAndOrderFront (not a bare
    // orderFront:) so the open still plays the system animation; the key
    // half is re-asserted after the spin + by the pump fallback.
    [nsw makeKeyAndOrderFront:nil];
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:0.5];
    while (![NSApp isActive] &&
           [[NSDate date] compare:deadline] == NSOrderedAscending) {
        NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                        untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                                           inMode:NSDefaultRunLoopMode
                                          dequeue:YES];
        if (e)
            [NSApp sendEvent:e];
        [NSApp updateWindows];
    }
    // Animated open already issued above; now settle the key grant the
    // open requested. Ordering front was guaranteed by that call either
    // way; key follows when the grant allows.
    if (![NSApp isActive])
        NSLog(@"window: activation denied — opening behind the active app; click the Dock icon to take focus");
    if (![nsw isKeyWindow] && [nsw canBecomeKeyWindow])
        [nsw makeKeyWindow];
    sPendingKeyWindow = nsw;
}

// Activate and show the native window, then refresh its monitor mirror.
void Window_show(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        claimKeyAfterActivation((*window).nsWindow);
        windowRefreshMonitor(window);
    }
}

// Remove the native window from display and cancel its pending key-window claim.
void Window_hide(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        if (sPendingKeyWindow == (*window).nsWindow)
            sPendingKeyWindow = nil;
        [(*window).nsWindow orderOut:nil];
    }
}

// Select the native show or hide operation from the requested visibility.
void Window_setVisible(Window *window, bool visible) {
    if (visible)
        Window_show(window);
    else
        Window_hide(window);
}

// Query the AppKit style mask for the resizable capability.
bool Window_isResizable(Window *window) {
    if (window == nullptr)
        return false;
    return hasStyleBit(window, NSWindowStyleMaskResizable);
}

// Add or remove the resizable style bit unless AppKit owns fullscreen chrome.
void Window_setResizable(Window *window, bool resizable) {
    if (window == nullptr)
        return;
    updateStyleMask(window, resizable ? NSWindowStyleMaskResizable : 0, resizable ? 0 : NSWindowStyleMaskResizable);
}

// Query the AppKit style mask for the close-button capability.
bool Window_isClosable(Window *window) {
    if (window == nullptr)
        return false;
    return hasStyleBit(window, NSWindowStyleMaskClosable);
}

// Add or remove the native close-button style bit.
void Window_setClosable(Window *window, bool closable) {
    if (window == nullptr)
        return;
    updateStyleMask(window, closable ? NSWindowStyleMaskClosable : 0, closable ? 0 : NSWindowStyleMaskClosable);
}

// Query the AppKit style mask for the minimize-button capability.
bool Window_isMiniaturizable(Window *window) {
    if (window == nullptr)
        return false;
    return hasStyleBit(window, NSWindowStyleMaskMiniaturizable);
}

// Add or remove the native minimize-button style bit.
void Window_setMiniaturizable(Window *window, bool miniaturizable) {
    if (window == nullptr)
        return;
    updateStyleMask(window, miniaturizable ? NSWindowStyleMaskMiniaturizable : 0, miniaturizable ? 0 : NSWindowStyleMaskMiniaturizable);
}

// Enable or disable the native fullscreen collection behavior for the window.
void Window_setFullscreenButton(Window *window, bool enabled) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        NSWindowCollectionBehavior behavior = [(*window).nsWindow collectionBehavior];
        if (enabled)
            behavior |= NSWindowCollectionBehaviorFullScreenPrimary;
        else
            behavior &= ~NSWindowCollectionBehaviorFullScreenPrimary;
        [(*window).nsWindow setCollectionBehavior:behavior];
    }
}

// Apply the requested native chrome mode and refresh traffic-light geometry.
void Window_setUndecorated(Window *window, int mode) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        NSWindowStyleMask mask = styleMaskOf(window);
        if ((mask & NSWindowStyleMaskFullScreen) != 0)
            return;

        (*window).undecoratedMode = mode;
        (*window).viewportFlushToTop = false;

        NSWindowStyleMask next;
        if (mode == WINDOW_UNDECORATED_BORDERLESS)
            next = 0;
        else if (mode == WINDOW_UNDECORATED_NAKED)
            next = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                 | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                 | NSWindowStyleMaskFullSizeContentView;
        else
            next = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                 | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

        [(*window).nsWindow setStyleMask:next];

        // setStyleMask: resets the collectionBehavior, dropping FullScreenPrimary.
        [(*window).nsWindow setCollectionBehavior:([(*window).nsWindow collectionBehavior] | NSWindowCollectionBehaviorFullScreenPrimary)];

        bool transparent = (mode == WINDOW_UNDECORATED_NAKED);
        [(*window).nsWindow setTitlebarAppearsTransparent:transparent];
        [(*window).nsWindow setTitleVisibility:(transparent ? NSWindowTitleHidden : NSWindowTitleVisible)];

        // Mask (and therefore the native light layout) changed: re-snapshot and
        // re-apply per-button visibility + offset against the new layout.
        TrafficLight_resetBase((*window).trafficLights);
        refreshChrome(window);
        windowRefreshSize(window);
        windowRefreshOrigin(window);
    }
}

// Translate the boolean decoration preference into the native chrome mode setter.
void Window_setDecorated(Window *window, bool decorated) {
    Window_setUndecorated(window, decorated ? WINDOW_DECORATED : WINDOW_UNDECORATED_BORDERLESS);
}

// Report whether the stored mode selects standard decorated chrome.
bool Window_isDecorated(const Window *window) {
    if (window == nullptr)
        return false;
    return (*window).undecoratedMode == WINDOW_DECORATED;
}

// Translate the naked-titlebar preference into the native chrome mode setter.
void Window_setNaked(Window *window, bool naked) {
    Window_setUndecorated(window, naked ? WINDOW_UNDECORATED_NAKED : WINDOW_DECORATED);
}

// Report whether the stored mode selects full-size-content naked chrome.
bool Window_isNaked(const Window *window) {
    if (window == nullptr)
        return false;
    return (*window).undecoratedMode == WINDOW_UNDECORATED_NAKED;
}

// Translate the borderless preference into the native chrome mode setter.
void Window_setBorderless(Window *window, bool borderless) {
    Window_setUndecorated(window, borderless ? WINDOW_UNDECORATED_BORDERLESS : WINDOW_DECORATED);
}

// Report whether the stored mode selects borderless chrome.
bool Window_isBorderless(const Window *window) {
    if (window == nullptr)
        return false;
    return (*window).undecoratedMode == WINDOW_UNDECORATED_BORDERLESS;
}

// Extend or restore decorated content into the titlebar and refresh geometry.
void Window_setViewportFlushToTop(Window *window, bool flush) {
    if (window == nullptr)
        return;
    // Viewport flush-to-top ONLY applies when the window is decorated (not naked, not borderless).
    // If the window is currently naked or undecorated, this function is disabled (no-op).
    if ((*window).undecoratedMode != WINDOW_DECORATED)
        return;
    if ((*window).viewportFlushToTop == flush)
        return;
    @autoreleasepool {
        NSWindowStyleMask mask = styleMaskOf(window);
        if ((mask & NSWindowStyleMaskFullScreen) != 0)
            return;

        (*window).viewportFlushToTop = flush;

        if (flush) {
            updateStyleMask(window, NSWindowStyleMaskFullSizeContentView, 0);
            [(*window).nsWindow setTitlebarAppearsTransparent:YES];
            [(*window).nsWindow setTitleVisibility:NSWindowTitleHidden];
        } else {
            updateStyleMask(window, 0, NSWindowStyleMaskFullSizeContentView);
            [(*window).nsWindow setTitlebarAppearsTransparent:NO];
            [(*window).nsWindow setTitleVisibility:NSWindowTitleVisible];
        }
        // Native light layout changed with the mask: re-snapshot, re-seat.
        TrafficLight_resetBase((*window).trafficLights);
        refreshChrome(window);
        windowRefreshSize(window);
        windowRefreshOrigin(window);
    }
}

// Read whether decorated chrome currently extends content into the titlebar.
bool Window_isViewportFlushToTop(const Window *window) {
    if (window == nullptr)
        return false;
    if ((*window).undecoratedMode != WINDOW_DECORATED)
        return false;
    return (*window).viewportFlushToTop;
}

// Route floating traffic-light behavior through the titlebar-flush setting.
void Window_setFloatingTrafficLights(Window *window, bool floating) {
    Window_setViewportFlushToTop(window, floating);
}

// Map the public WindowTrafficLight enum onto the TrafficLight class's button
// index (the values coincide: close=0, miniaturize=1, zoom=2). -1 = unknown.
static int lightIndex(WindowTrafficLight light) {
    if (light == WINDOW_TRAFFIC_LIGHT_CLOSE)
        return TRAFFIC_LIGHT_CLOSE;
    if (light == WINDOW_TRAFFIC_LIGHT_MINIMIZE)
        return TRAFFIC_LIGHT_MINIATURIZE;
    if (light == WINDOW_TRAFFIC_LIGHT_ZOOM)
        return TRAFFIC_LIGHT_ZOOM;
    return -1;
}

// Set visibility for a validated native traffic-light button enum.
void Window_macOS_setTrafficLightButtonVisible(Window *window, WindowTrafficLight light, bool visible) {
    if (window == nullptr)
        return;
    int i = lightIndex(light);
    if (i < 0)
        return;
    @autoreleasepool {
        TrafficLight_setButtonVisible((*window).trafficLights, (TrafficLightButton) i, visible);
    }
}

// Query configured visibility for a valid native traffic-light button.
bool Window_macOS_isTrafficLightButtonVisible(const Window *window, WindowTrafficLight light) {
    int i = lightIndex(light);
    if (window == nullptr || i < 0)
        return false;
    return TrafficLight_isButtonVisible((*window).trafficLights, (TrafficLightButton) i);
}

// Set the traffic-light cluster's header-relative position in AppKit points.
void Window_macOS_setTrafficLightHeaderPosition(Window *window, float x, float y) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        TrafficLight_setHeaderPosition((*window).trafficLights, x, y);
    }
}

// Return the configured header-relative button-cluster position through outputs.
void Window_macOS_getTrafficLightHeaderPosition(const Window *window, float *outX, float *outY) {
    float x = 0.0f;
    float y = 0.0f;
    if (window != nullptr)
        TrafficLight_getHeaderPosition((*window).trafficLights, &x, &y);
    if (outX)
        *outX = x;
    if (outY)
        *outY = y;
}

// Set the native NSWindow alpha value.
void Window_setOpacity(Window *window, float opacity) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setAlphaValue:(CGFloat) opacity];
    }
}

// Set the native opaque/background policy and publish the transparency state.
void Window_setTransparentBackground(Window *window, bool transparent) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setOpaque:!transparent];
        [(*window).nsWindow setBackgroundColor:(transparent ? [NSColor clearColor] : [NSColor windowBackgroundColor])];
        if ([(*window).nsWindow contentView].layer)
            [(*window).nsWindow contentView].layer.opaque = !transparent;
        Window_setTransparent(window, transparent);
    }
}

// Frosted backdrop: a behindWindow NSVisualEffectView under the draw view, so
// the transparent parts of a present show the blurred desktop through. radius
// is a hint (AppKit materials are fixed); 0 removes it.
void Window_setBackdropBlur(Window *window, float radius) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        NSView *content = [(*window).nsWindow contentView];
        if (content == nil)
            return;
        if (radius > 0.0f) {
            if ((*window).backdropView == nullptr) {
                NSVisualEffectView *ve = [[NSVisualEffectView alloc] initWithFrame:content.bounds];
                ve.blendingMode = NSVisualEffectBlendingModeBehindWindow;
                ve.material = NSVisualEffectMaterialHUDWindow;
                ve.state = NSVisualEffectStateActive;
                ve.autoresizingMask = (NSViewWidthSizable | NSViewHeightSizable);
                [content addSubview:ve
                        positioned:NSWindowBelow
                        relativeTo:(*window).glView];
                (*window).backdropView = (__bridge_retained void *) ve;
            }
        } else if ((*window).backdropView != nullptr) {
            NSView *ve = (__bridge NSView *)(*window).backdropView;
            [ve removeFromSuperview];
            CFRelease((*window).backdropView);
            (*window).backdropView = nullptr;
        }
    }
}

// Liquid Glass (macOS 26+): a real NSGlassEffectView behind the draw view. The
// class is resolved by name at runtime so this compiles against any SDK, and
// @available gates the OS; where it is unavailable the NSVisualEffectView
// backdrop (Window_setBackdropBlur) is the effective fallback.
bool Window_macOS_hasLiquidGlass(void) {
    if (@available(macOS 26.0, *))
        return NSClassFromString(@"NSGlassEffectView") != Nil;
    return false;
}

// Apply or remove the runtime-available Liquid Glass view from its descriptor.
void Window_macOS_setLiquidGlass(Window *window, const WindowLiquidGlassDesc *desc) {
    if (window == nullptr)
        return;
    (*window).liquidGlass = desc != nullptr ? *desc : (WindowLiquidGlassDesc){ 0 };
    WindowLiquidGlassDesc glass = (*window).liquidGlass;
    @autoreleasepool {
        if (!glass.enabled || !Window_macOS_hasLiquidGlass()) {
            if ((*window).glassView != nullptr) {
                NSView *g = (__bridge NSView *)(*window).glassView;
                [g removeFromSuperview];
                CFRelease((*window).glassView);
                (*window).glassView = nullptr;
            }
            return;
        }
        NSView *content = [(*window).nsWindow contentView];
        if (content == nil)
            return;
        if ((*window).glassView == nullptr) {
            Class glassClass = NSClassFromString(@"NSGlassEffectView");
            NSView *g = [[glassClass alloc] initWithFrame:content.bounds];
            g.autoresizingMask = (NSViewWidthSizable | NSViewHeightSizable);
            [content addSubview:g positioned:NSWindowBelow relativeTo:(*window).glView];
            (*window).glassView = (__bridge_retained void *) g;
        }
        NSView *g = (__bridge NSView *)(*window).glassView;
        WindowLiquidGlassDesc d = (*window).liquidGlass;
        if ([g respondsToSelector:NSSelectorFromString(@"setStyle:")])
            [g setValue:@(d.style) forKey:@"style"];
        if ([g respondsToSelector:NSSelectorFromString(@"setCornerRadius:")])
            [g setValue:@(d.cornerRadius) forKey:@"cornerRadius"];
        if ([g respondsToSelector:NSSelectorFromString(@"setTintColor:")]) {
            CGFloat r  = (CGFloat) ((d.tintColor >> 24) & 0xFFu) / 255.0;
            CGFloat gg = (CGFloat) ((d.tintColor >> 16) & 0xFFu) / 255.0;
            CGFloat b  = (CGFloat) ((d.tintColor >> 8) & 0xFFu) / 255.0;
            CGFloat a  = (CGFloat) (d.tintColor & 0xFFu) / 255.0;
            [g setValue:[NSColor colorWithSRGBRed:r green:gg blue:b alpha:a] forKey:@"tintColor"];
        }
    }
}

// Copy the currently stored Liquid Glass descriptor to the caller's output.
bool Window_macOS_getLiquidGlass(const Window *window, WindowLiquidGlassDesc *out) {
    if (window == nullptr || out == nullptr)
        return false;
    *out = (*window).liquidGlass;
    return true;
}

// Select the AppKit floating or normal window level.
void Window_setAlwaysOnTop(Window *window, bool onTop) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setLevel:(onTop ? NSFloatingWindowLevel : NSNormalWindowLevel)];
    }
}

// Publish caller-owned RGBA pixels as layer contents without copying the buffer.
void Window_presentRGBA(Window *window, const void *pixels, size_t stride, int width, int height) {
    if (window == nullptr || pixels == nullptr || width <= 0 || height <= 0)
        return;
    @autoreleasepool {
        NSView *view = (*window).glView ? (*window).glView : [(*window).nsWindow contentView];
        if (view == nil)
            return;
        // ZERO-COPY: the CGImage borrows the caller's buffer directly — no
        // bitmap-context allocation, no pixel copy. The buffer must stay valid
        // until the layer is done; live resize flushes the committed CA updates
        // but that submission is not a buffer-lifetime completion fence.
        CGDataProviderRef provider =
            CGDataProviderCreateWithData(nullptr, pixels, stride * (size_t) height, nullptr);
        if (provider == nullptr)
            return;
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGImageRef img = CGImageCreate((size_t) width, (size_t) height, 8, 32, stride, cs,
                                       kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big,
                                       provider, nullptr, false, kCGRenderingIntentDefault);
        CGColorSpaceRelease(cs);
        CGDataProviderRelease(provider);
        if (img == nullptr)
            return;
        view.wantsLayer = YES;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        view.layer.contents = (__bridge id) img;
        view.layer.contentsGravity = kCAGravityResize;  // stale-step fallback only
        [CATransaction commit];
        // Flush submitted changes, not an explicit transaction still open.
        // This submits CA work; it is not a GPU/WindowServer completion fence.
        if (Window_isLiveResizing(window))
            [CATransaction flush];
        CGImageRelease(img);
    }
}

// ── Zero-copy present surface (IOSurface + CALayer) ─────────────────────────
// The window's draw view already wantsLayer; we hand the layer an IOSurface as
// its contents. The render repo draws straight into the IOSurface on the GPU,
// so publishing is a pointer swap, not a pixel copy.
void *Window_createPresentSurface(Window *window, int widthPx, int heightPx) {
    if (window == nullptr || widthPx <= 0 || heightPx <= 0)
        return nullptr;
    @autoreleasepool {
        // Row Bytes must be aligned for Metal to build a texture from the
        // IOSurface (an unaligned width*4 aborts in _mtlValidateStrideTextureParameters).
        int rowBytes = ((widthPx * 4) + 63) & ~63;
        NSDictionary *props = @{
            (__bridge id) kIOSurfaceWidth: @(widthPx),
            (__bridge id) kIOSurfaceHeight: @(heightPx),
            (__bridge id) kIOSurfaceBytesPerElement: @(4),
            (__bridge id) kIOSurfaceBytesPerRow: @(rowBytes),
            (__bridge id) kIOSurfacePixelFormat: @(kCVPixelFormatType_32RGBA),
        };
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) props);
        return (void*) surface;
    }
}

// Release the retained IOSurface reference supplied by the caller.
void Window_destroyPresentSurface(Window *window, void *surface) {
    (void) window;
    if (surface != nullptr)
        CFRelease((CFTypeRef) surface);
}

// Publish the IOSurface as the view layer's contents at the current backing scale.
void Window_presentSurface(Window *window, void *surface) {
    if (window == nullptr || surface == nullptr)
        return;
    @autoreleasepool {
        NSView *view = (*window).glView ? (*window).glView : [(*window).nsWindow contentView];
        if (view == nil)
            return;
        view.wantsLayer = YES;
        CALayer *layer = view.layer;
        if (layer == nil)
            return;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.contents = (__bridge id)(IOSurfaceRef) surface;
        layer.contentsGravity = kCAGravityResize;
        double scale = (*window).cachedScale > 0.0 ? (*window).cachedScale : 1.0;
        layer.contentsScale = scale;
        [CATransaction commit];
        // This is a CALayer/IOSurface publication, not a CAMetalLayer drawable:
        // presentsWithTransaction is not available here. Submit the closed
        // transaction during tracking instead of waiting for the outer pump.
        // flush does not prove display completion or synchronize a GPU worker.
        if (Window_isLiveResizing(window))
            [CATransaction flush];
    }
}

// Return the current layer contents handle for the active draw view.
void *Window_presentSurfaceContents(const Window *window) {
    if (window == nullptr)
        return nullptr;
    NSView *view = (*window).glView ? (*window).glView : [(*window).nsWindow contentView];
    if (view == nil || view.layer == nil)
        return nullptr;
    return (__bridge void *) view.layer.contents;
}

// Copy IOSurface pixels into the caller's RGBA destination when dimensions permit.
bool Window_readPresentSurface(Window *window, void *surface, void *destRGBA, size_t destStride) {
    (void) window;
    if (surface == nullptr || destRGBA == nullptr || destStride == 0)
        return false;
    IOSurfaceRef s = (IOSurfaceRef) surface;
    if (IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr) != kIOReturnSuccess)
        return false;
    const uint8_t *base = (const uint8_t*) IOSurfaceGetBaseAddress(s);
    size_t srcStride = IOSurfaceGetBytesPerRow(s);
    uint32_t w = (uint32_t) IOSurfaceGetWidth(s);
    uint32_t h = (uint32_t) IOSurfaceGetHeight(s);
    for (uint32_t y = 0; y < h; y++)
        memcpy((uint8_t*) destRGBA + (size_t) y * destStride,
               base + (size_t) y * srcStride, (size_t) w * 4u);
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
    return true;
}

// Write a tightly packed RGBA8 buffer to a PNG file. The screenshot/CAPTURE
// path for tests and agents; ImageIO does the encoding.
bool Window_writePNG(const void *pixels, size_t stride, int width, int height, const char *path) {
    if (pixels == nullptr || width <= 0 || height <= 0 || path == nullptr)
        return false;
    bool ok = false;
    @autoreleasepool {
        CGDataProviderRef provider =
            CGDataProviderCreateWithData(nullptr, pixels, stride * (size_t) height, nullptr);
        if (provider == nullptr)
            return false;
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGImageRef img = CGImageCreate((size_t) width, (size_t) height, 8, 32, stride, cs,
                                       kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big,
                                       provider, nullptr, false, kCGRenderingIntentDefault);
        CGColorSpaceRelease(cs);
        CGDataProviderRelease(provider);
        if (img == nullptr)
            return false;
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(
            nullptr, (const UInt8*) path, (CFIndex) strlen(path), false);
        if (url != nullptr) {
            CGImageDestinationRef dest =
                CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
            if (dest != nullptr) {
                CGImageDestinationAddImage(dest, img, nullptr);
                ok = CGImageDestinationFinalize(dest);
                CFRelease(dest);
            }
            CFRelease(url);
        }
        CGImageRelease(img);
    }
    return ok;
}

// Enable or clear the native window's pointer-ignoring behavior.
void Window_setClickThrough(Window *window, bool clickThrough) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setIgnoresMouseEvents:clickThrough];
    }
}

// Request the native non-client shadow policy for decorated chrome.
void Window_setShadow(Window *window, bool shadow) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setHasShadow:shadow];
    }
}

// Configure whether background dragging is delegated to the native window.
void Window_setMovableByBackground(Window *window, bool movable) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setMovableByWindowBackground:movable];
    }
}

// Order the native window to the front of the AppKit window stack.
void Window_bringToFront(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow orderFrontRegardless];
    }
}

// Attach a child window using AppKit's parent-child ordering relationship.
void Window_attachChild(Window *parent, Window *child) {
    (void) parent;
    (void) child;
}

// Remove an existing child-window relationship from its parent.
void Window_detachChild(Window *parent, Window *child) {
    (void) parent;
    (void) child;
}

// --- Minimize ---

void Window_minimize(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow miniaturize:nil];
    }
}

// Restore a miniaturized window and let AppKit publish its new geometry.
void Window_restore(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow deminiaturize:nil];
    }
}

// Read the native miniaturization state for a valid window.
bool Window_isMinimized(Window *window) {
    if (window == nullptr)
        return false;
    if (atomic_load_explicit(&(*window).miniaturizing, memory_order_relaxed))
        return true;
    @autoreleasepool {
        return [(*window).nsWindow isMiniaturized];
    }
}

// --- Fullscreen ---

bool Window_isFullscreen(Window *window) {
    if (window == nullptr)
        return false;
    @autoreleasepool {
        return (styleMaskOf(window) & NSWindowStyleMaskFullScreen) != 0;
    }
}

// Ask AppKit to enter or exit its native fullscreen space.
void Window_setFullscreen(Window *window, bool fullscreen) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        if (fullscreen != Window_isFullscreen(window))
            [(*window).nsWindow toggleFullScreen:nil];
    }
}

// Toggle native fullscreen based on the window's current AppKit state.
void Window_toggleFullscreen(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow toggleFullScreen:nil];
    }
}

// --- DRM / sharing ---

void Window_setDRM(Window *window, bool enabled) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setSharingType:(enabled ? NSWindowSharingNone : NSWindowSharingReadOnly)];
    }
}

// --- Size constraints ---

void Window_setMinSize(Window *window, int width, int height) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        CGFloat scale = windowBackingScale(window);
        CGFloat ptW = width > 0 ? ((CGFloat) width / scale) : 0.0;
        CGFloat ptH = height > 0 ? ((CGFloat) height / scale) : 0.0;
        [(*window).nsWindow setContentMinSize:NSMakeSize(ptW, ptH)];
    }
}

// Convert maximum content dimensions from physical pixels and set AppKit limits.
void Window_setMaxSize(Window *window, int width, int height) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        CGFloat scale = windowBackingScale(window);
        CGFloat ptW = width > 0 ? ((CGFloat) width / scale) : (CGFloat) CGFLOAT_MAX;
        CGFloat ptH = height > 0 ? ((CGFloat) height / scale) : (CGFloat) CGFLOAT_MAX;
        [(*window).nsWindow setContentMaxSize:NSMakeSize(ptW, ptH)];
    }
}

// --- Cursor control ---

// FPS-style relative cursor: hides the pointer, decouples it from movement,
// and re-warps to the window centre each pump pass while deltas flow into the
// mouse stream as move-delta events.
void Window_setCursorLocked(Window *window, bool locked) {
    if (locked == s_cursorLocked)
        return;
    if (locked) {
        NSWindow *anchor = window ? (*window).nsWindow : sLastWindow;
        NSRect frame = anchor ? [anchor frame] : [[NSScreen mainScreen] frame];
        s_lockCenter = NSMakePoint(frame.origin.x + frame.size.width / 2.0,
                                   frame.origin.y + frame.size.height / 2.0);
        CGAssociateMouseAndMouseCursorPosition(NO);
        CGWarpMouseCursorPosition(s_lockCenter);
        CGDisplayHideCursor(kCGDirectMainDisplay);
        s_cursorLocked = true;
    } else {
        s_cursorLocked = false;
        CGDisplayShowCursor(kCGDirectMainDisplay);
        CGAssociateMouseAndMouseCursorPosition(YES);
    }
}

// Store the cursor shape that the event path applies to the active window.
void Window_setCursorType(Window *window, WindowCursorType type) {
    if (window == nullptr)
        return;
    WindowCursorType prev = (*window).cursorType;
    (*window).cursorType = type;
    @autoreleasepool {
        if (prev == WINDOW_CURSOR_HIDDEN && type != WINDOW_CURSOR_HIDDEN)
            [NSCursor unhide];
        switch (type) {
            case WINDOW_CURSOR_IBEAM:
                [[NSCursor IBeamCursor] set];
                break;
            case WINDOW_CURSOR_POINTING_HAND:
                [[NSCursor pointingHandCursor] set];
                break;
            case WINDOW_CURSOR_CROSSHAIR:
                [[NSCursor crosshairCursor] set];
                break;
            case WINDOW_CURSOR_RESIZE_EW:
                [[NSCursor resizeLeftRightCursor] set];
                break;
            case WINDOW_CURSOR_RESIZE_NS:
                [[NSCursor resizeUpDownCursor] set];
                break;
            case WINDOW_CURSOR_NOT_ALLOWED:
                [[NSCursor operationNotAllowedCursor] set];
                break;
            case WINDOW_CURSOR_HIDDEN:
                [NSCursor hide];
                break;
            case WINDOW_CURSOR_DEFAULT:
            default:
                [[NSCursor arrowCursor] set];
                break;
        }
    }
}

// Return the requested cursor shape, defaulting for a missing window.
WindowCursorType Window_getCursorType(const Window *window) {
    if (window == nullptr)
        return WINDOW_CURSOR_DEFAULT;
    return (*window).cursorType;
}

// --- Software frame presentation (inert without a render target) ------------

// ;;INTENTION("Window_present on the fresh window is inert (`return false`): a pure AppKit window has no raster target of its own. Software present was GPU-era shim logic; the render personal own pixel paths. Retires with the composite seam.")
bool Window_present(Window *window, const Buffer *frame) {
    (void) window;
    (void) frame;
    return false;
}

// --- Surface anchors (thread 0) ----------------------------------------------

void *Window_contentView(Window *window) {
    if (window == nullptr || (*window).nsWindow == nil)
        return nullptr;
    return (__bridge void*) [(*window).nsWindow contentView];
}

void *Window_nativeHandle(const Window *window) {
    if (window == nullptr || (*window).nsWindow == nil)
        return nullptr;
    return (__bridge void*) (*window).nsWindow;
}

// ;;INTENTION("Window_metalLayer is gone from this file by design (no Metal here). Returns nullptr so the VK_EXT_metal_surface path degrades cleanly until the render personal create their own CAMetalLayer on the content view. Retires with the composite seam.")
void *Window_metalLayer(Window *window) {
    (void) window;
    return nullptr;
}

// ;;INTENTION("Gravity ownership moved out of the window (no layer here). No-op; retires with the composite seam.")
void Window_setGravityTopLeft(Window *window) {
    (void) window;
}

// ;;INTENTION("Worker present transaction was GPU-era (presentsWithTransaction drawables). No layer means nothing to commit; no-op. Retires with the composite seam.")
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

// Request AppKit activation and schedule the window for key-window promotion.
void Window_focus(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        claimKeyAfterActivation((*window).nsWindow);
        Focus_set((*window).id);
    }
}

// Compare this window's id with the current process-wide focus identity.
bool Window_isFocused(Window *window) {
    return window && Focus_isFocused((*window).id);
}

// Enable or suppress AppKit key-window eligibility for this window.
void Window_setKeyEnabled(Window *window, bool enabled) {
    if (window == nullptr)
        return;
    atomic_store_explicit(&(*window).keyEnabled, enabled, memory_order_relaxed);
}

// Read whether this window currently accepts key focus.
bool Window_isKeyEnabled(const Window *window) {
    if (window == nullptr)
        return false;
    return atomic_load_explicit(&(*window).keyEnabled, memory_order_relaxed);
}

// --- Lifecycle registry ------------------------------------------------------
// The ONLY outside path to the embedded WindowEvent: fill slots with
// WindowEvent_setOn*(Window_getLifecycle(w), fn). Null-safe.

WindowEvent *Window_getLifecycle(Window *window) {
    if (window == nullptr)
        return nullptr;
    return &(*window).lifecycle;
}

// --- Monitor identity ---------------------------------------------------------

uint32_t Window_getMonitorId(const Window *window) {
    return window ? atomic_load_explicit(&(*window).monitorId, memory_order_acquire) : 0;
}

// --- Resize reflection ---------------------------------------------------------

uint64_t Window_sizeGeneration(Window *window) {
    if (window == nullptr)
        return 0;
    return atomic_load_explicit(&(*window).sizeGeneration, memory_order_acquire);
}

// Register the callback invoked synchronously as native resize geometry changes.
void Window_setResizeRenderHook(Window *window, WindowResizeRenderFn fn, void *userdata) {
    if (window == nullptr)
        return;
    (*window).resizeRenderFn = fn;
    (*window).resizeRenderUserdata = userdata;
}

// Return the callback registered for synchronous resize rendering.
WindowResizeRenderFn Window_getResizeRenderHook(const Window *window) {
    return window ? (*window).resizeRenderFn : nullptr;
}
