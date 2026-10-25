// window.c — Win32 backend for the Window API.
//
// The annotations (@PlatformExclusive, @Draft, @Intention, @Incomplete) are
// real markers defined in src/annotation/*.h — C has no annotation feature,
// so they are macros that expand to static asserts embedding the annotation
// text in compiler diagnostics / debug info.
//
// Every function below is an @Incomplete stub returning a safe default. The
// real implementations will use User32: CreateWindowEx / DefWindowProc,
// ShowWindow / SetWindowTextA / SetWindowPos / SetWindowDisplayAffinity,
// and a GetMessage/PeekMessage-TranslateMessage-DispatchMessage event loop.

#include <stdio.h>
#include <stdlib.h>

#include "window/window.h"
#include "window/traffic_light.h"
#include "annotation/definition.h"
#include "annotation/overview.h"
#include "annotation/getter.h"
#include "annotation/setter.h"
#include "annotation/draft.h"
#include "annotation/incomplete.h"
#include "annotation/intention.h"
#include "annotation/platform_exclusive.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Window
 * ============================================================================
 * Win32 platform implementation stub of the high-level Window abstraction.
 * Provides fallback function symbols matching the window.h platform-agnostic API
 * seam on Windows environments, returning safe defaults until full Win32 User32
 * and DXGI integration is completed.
 *
 * Implements the Cold-Strict, Hot-Minimal Validation Law and preserves ABI
 * compatibility across targets by completing the Window constructor, core,
 * setter, and getter contracts.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Window (window/window.c)
 * ============================================================================
 * SUMMARY:
 *   Win32 stub backend for the platform-agnostic Window API.
 *   Provides complete fallback symbol definitions matching window.h for Windows builds.
 *
 * STRUCT FIELDS:
 * ----------------------------------------------------------------------------
 *   (none — stub backend; real fields live in window/window_cocoa.m)
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   (none)
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
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - Window_destroy(window)
 *   - Window_destroyAll(void)
 *   - Window_shouldClose(window)
 *   - Window_pollEvents(void)
 *   - Window_width(window)
 *   - Window_height(window)
 *   - Window_center(window)
 *   - Window_show(window)
 *   - Window_hide(window)
 *   - Window_attachPanes(window, panel, width, height)
 *   - Window_resizePanes(window, panel, width, height)
 *   - Window_compositePanes(window, contentPanel)
 *   - Window_compositeBoards(window)
 *   - Window_renderGeneration(window)
 *   - Window_bringToFront(window)
 *   - Window_minimize(window)
 *   - Window_restore(window)
 *   - Window_toggleFullscreen(window)
 *   - Window_contentView(window)
 *   - Window_nativeHandle(window)
 *   - Window_metalLayer(window)
 *   - Window_present(window, frame)
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
 *   - (none)
 *
 * Public Setters: (.h)
 *   - Window_setShouldClose(window, shouldClose)
 *   - Window_setTitle(window, title)
 *   - Window_setSize(window, width, height)
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
 *   - Window_macOS_setTrafficLightButtonVisible(window, light, visible)
 *   - Window_macOS_setTrafficLightHeaderPosition(window, x, y)
 *   - Window_setFloatingTrafficLights(window, floating)
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
 *   - Window_setGravityTopLeft(window)
 *   - Window_setResizeRenderHook(window, fn, userdata)
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
 *   - Window_getResizeRenderHook(window)
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */


#if defined(_WIN32) || defined(__linux__) || defined(__unix__)

;;PLATFORM_EXCLUSIVE("Windows")
;;INTENTION("Fills the Window API seam (window.h) on Windows so the engine can be built there; mirrors the legacy windowsWindow.java.")
;;DRAFT

;;;;INCOMPLETE // CreateWindowEx; returns nullptr until implemented.
/** Windows draft stub: returns nullptr until native window creation is implemented. */
Window *Window_create(const char *title, int width, int height) {
    (void) title;
    (void) width;
    (void) height;
    return nullptr;
}

;;INCOMPLETE // DestroyWindow; no-op until implemented.
/** Windows draft stub: does not destroy a native window yet. */
void Window_destroy(Window *window) {
    (void) window;
}

// Compatibility entry point; this stub backend has no registered windows to destroy.
/** Windows draft stub: has no windows to destroy. */
void Window_destroyAll(void) {
}

;;INCOMPLETE // Poll WM_QUIT; false until implemented.
/** Windows draft stub: reports that no close request is pending. */
bool Window_shouldClose(Window *window) {
    (void) window;
    return false;
}

// Store the requested close state; the incomplete backend has no native window state.
/** Windows draft stub: ignores close-state updates. */
void Window_setShouldClose(Window *window, bool shouldClose) {
    (void) window;
    (void) shouldClose;
}

;;INCOMPLETE // PeekMessage/TranslateMessage/DispatchMessage loop; no-op until implemented.
/** Windows draft stub: does not pump native messages. */
void Window_pollEvents(void) {
}

// Perform the available event poll and report that no further event is pending.
/** Pumps the stub event function and reports no event. */
bool Window_pollEventStep(void) {
    Window_pollEvents();
    return false;
}

;;INCOMPLETE // DXGI swap interval; no-op until implemented.
/** Windows draft stub: ignores presentation mode changes. */
void Window_setPresentMode(Window *window, int mode) {
    (void) window;
    (void) mode;
}

// Return FIFO as the safe present-mode default for the unimplemented backend.
/** Returns FIFO because this backend does not implement presentation modes. */
int Window_getPresentMode(const Window *window) {
    (void) window;
    return WINDOW_PRESENT_FIFO;
}

;;INCOMPLETE // Layered window alpha; no-op until implemented.
/** Windows draft stub: ignores transparency changes. */
void Window_setTransparent(Window *window, bool transparent) {
    (void) window;
    (void) transparent;
}

// Report the stub backend's default opaque-window state.
/** Returns false because transparency state is not implemented. */
bool Window_isTransparent(const Window *window) {
    (void) window;
    return false;
}

// Return the unchanged render-policy generation for this stub backend.
/** Returns zero because this stub does not track render generations. */
uint64_t Window_renderGeneration(const Window *window) {
    (void) window;
    return 0;
}

;;INCOMPLETE // Input kill switch; no-op until implemented.
/** Windows draft stub: ignores enabled-state changes. */
void Window_setEnabled(Window *window, bool enabled) {
    (void) window;
    (void) enabled;
}

// Report the stub's permissive default input-enabled state.
/** Returns the stub's default enabled state. */
bool Window_isEnabled(const Window *window) {
    (void) window;
    return true;
}

;;INCOMPLETE // SetWindowTextA; no-op until implemented.
/** Windows draft stub: ignores title changes. */
void Window_setTitle(Window *window, const char *title) {
    (void) window;
    (void) title;
}

;;INCOMPLETE // SetWindowPos; no-op until implemented.
/** Windows draft stub: ignores pixel-size changes. */
void Window_setSize(Window *window, int width, int height) {
    (void) window;
    (void) width;
    (void) height;
}

// Report the unit scale used by this platform stub.
/** Returns the unit scale used by this unimplemented backend. */
float Window_getScale(const Window *window) {
    (void) window;
    return 1.0f;
}

// Provide the platform revalidation seam without native display state.
/** Windows draft stub: has no native geometry to revalidate. */
void Window_revalidate(Window *window) {
    (void) window;
}

// Accept the logical-size API seam; this backend does not retain geometry.
/** Windows draft stub: ignores point-size changes. */
void Window_setSizePoints(Window *window, float width, float height) {
    (void) window;
    (void) width;
    (void) height;
}

// Return zero logical dimensions because the stub creates no native window.
/** Writes zero dimensions to supplied outputs. */
void Window_getSizePoints(const Window *window, float *outWidth, float *outHeight) {
    (void) window;
    if (outWidth) *outWidth = 0.0f;
    if (outHeight) *outHeight = 0.0f;
}

// Return the stub window's logical width, which is always zero.
/** Returns zero because point width is unavailable in this stub. */
float Window_widthPoints(const Window *window) {
    (void) window;
    return 0.0f;
}

// Return the stub window's logical height, which is always zero.
/** Returns zero because point height is unavailable in this stub. */
float Window_heightPoints(const Window *window) {
    (void) window;
    return 0.0f;
}

// Return zero width because this stub backend has no physical display window.
/** Returns zero client width because geometry is not tracked. */
int Window_width(Window *window) {
    (void) window;
    return 0;
}

// Return zero height because this stub backend has no physical display window.
/** Returns zero client height because geometry is not tracked. */
int Window_height(Window *window) {
    (void) window;
    return 0;
}

// Expose the window width through the viewport compatibility accessor.
/** Returns the window pixel width through the shared accessor. */
int Window_viewportWidth(const Window *window) {
    return Window_width((Window*) window);
}

// Expose the window height through the viewport compatibility accessor.
/** Returns the window pixel height through the shared accessor. */
int Window_viewportHeight(const Window *window) {
    return Window_height((Window*) window);
}

// Expose the logical width through the viewport compatibility accessor.
/** Returns the window point width through the shared accessor. */
float Window_viewportWidthPoints(const Window *window) {
    return Window_widthPoints(window);
}

// Expose the logical height through the viewport compatibility accessor.
/** Returns the window point height through the shared accessor. */
float Window_viewportHeightPoints(const Window *window) {
    return Window_heightPoints(window);
}

// Alias the client width as the full window width in this stub.
/** Returns the window pixel width through the shared accessor. */
int Window_windowWidth(const Window *window) {
    return Window_width((Window*) window);
}

// Alias the client height as the full window height in this stub.
/** Returns the window pixel height through the shared accessor. */
int Window_windowHeight(const Window *window) {
    return Window_height((Window*) window);
}

// Alias the logical client width as the full window width.
/** Returns the window point width through the shared accessor. */
float Window_windowWidthPoints(const Window *window) {
    return Window_widthPoints(window);
}

// Alias the logical client height as the full window height.
/** Returns the window point height through the shared accessor. */
float Window_windowHeightPoints(const Window *window) {
    return Window_heightPoints(window);
}

;;INCOMPLETE // SetWindowPos; no-op until implemented.
/** Windows draft stub: ignores location changes. */
void Window_setLocation(Window *window, int x, int y) {
    (void) window;
    (void) x;
    (void) y;
}

// Return zero content-origin coordinates for the unimplemented backend.
/** Writes zero content-origin coordinates to supplied outputs. */
void Window_getContentOrigin(const Window *window, int *outX, int *outY) {
    (void) window;
    if (outX) *outX = 0;
    if (outY) *outY = 0;
}

// Accept but do not retain the resize callback on this stub platform.
/** Windows draft stub: does not retain a resize-render callback. */
void Window_setResizeRenderHook(Window *window, WindowResizeRenderFn fn, void *userdata) {
    (void) window;
    (void) fn;
    (void) userdata;
}

;;INCOMPLETE // No hook slot on the stub platform; nullptr until implemented.
/** Returns nullptr because this stub has no resize-render hook storage. */
WindowResizeRenderFn Window_getResizeRenderHook(const Window *window) {
    (void) window;
    return nullptr;
}

// Return zero because this backend does not resolve monitor identities.
/** Returns zero because monitor identity is not implemented. */
uint32_t Window_getMonitorId(const Window *window) {
    (void) window;
    return 0;
}

;;INCOMPLETE // GetWindowRect top-left; zeros until implemented.
/** Writes zero location coordinates to supplied outputs. */
void Window_getLocation(const Window *window, int *outX, int *outY) {
    (void) window;
    if (outX) *outX = 0;
    if (outY) *outY = 0;
}

;;INCOMPLETE // CenterWindow; no-op until implemented.
/** Windows draft stub: does not reposition a native window. */
void Window_center(Window *window) {
    (void) window;
}

;;INCOMPLETE // ShowWindow(SW_SHOW)/ShowWindow(SW_HIDE); no-op until implemented.
/** Windows draft stub: ignores visibility changes. */
void Window_setVisible(Window *window, bool visible) {
    (void) window;
    (void) visible;
}

;;INCOMPLETE // GWL_STYLE WS_THICKFRAME; false until implemented.
/** Returns false because resizability is not implemented. */
bool Window_isResizable(Window *window) {
    (void) window;
    return false;
}

;;INCOMPLETE // SetWindowLong(GWL_STYLE, WS_THICKFRAME); no-op until implemented.
/** Windows draft stub: ignores resizability changes. */
void Window_setResizable(Window *window, bool resizable) {
    (void) window;
    (void) resizable;
}

;;INCOMPLETE // GWL_STYLE WS_SYSMENU; false until implemented.
/** Returns false because closability is not implemented. */
bool Window_isClosable(Window *window) {
    (void) window;
    return false;
}

;;INCOMPLETE // SetWindowLong(GWL_STYLE, WS_SYSMENU); no-op until implemented.
/** Windows draft stub: ignores closability changes. */
void Window_setClosable(Window *window, bool closable) {
    (void) window;
    (void) closable;
}

;;INCOMPLETE // GWL_STYLE WS_MINIMIZEBOX; false until implemented.
/** Returns false because miniaturization is not implemented. */
bool Window_isMiniaturizable(Window *window) {
    (void) window;
    return false;
}

;;INCOMPLETE // SetWindowLong(GWL_STYLE, WS_MINIMIZEBOX); no-op until implemented.
/** Windows draft stub: ignores miniaturization changes. */
void Window_setMiniaturizable(Window *window, bool miniaturizable) {
    (void) window;
    (void) miniaturizable;
}

;;INCOMPLETE // WS_MAXIMIZEBOX semantics; no-op until implemented.
/** Windows draft stub: ignores the fullscreen-button setting. */
void Window_setFullscreenButton(Window *window, bool enabled) {
    (void) window;
    (void) enabled;
}

;;INCOMPLETE // Fullscreen/borderless chrome switch; no-op until implemented.
/** Windows draft stub: ignores decoration-mode changes. */
void Window_setUndecorated(Window *window, int mode) {
    (void) window;
    (void) mode;
}

// Map the boolean decoration request onto the undecorated-mode API.
/** Selects decorated or borderless mode through the draft mode setter. */
void Window_setDecorated(Window *window, bool decorated) {
    Window_setUndecorated(window, decorated ? WINDOW_DECORATED : WINDOW_UNDECORATED_BORDERLESS);
}

// Report the stub's default decorated state.
/** Returns the draft backend's decorated default. */
bool Window_isDecorated(const Window *window) {
    (void) window;
    return true;
}

// Map the naked-titlebar request onto the undecorated-mode API.
/** Selects naked or decorated mode through the draft mode setter. */
void Window_setNaked(Window *window, bool naked) {
    Window_setUndecorated(window, naked ? WINDOW_UNDECORATED_NAKED : WINDOW_DECORATED);
}

// Report that the stub does not provide naked chrome.
/** Returns false because naked-mode state is not retained. */
bool Window_isNaked(const Window *window) {
    (void) window;
    return false;
}

// Map the borderless request onto the undecorated-mode API.
/** Selects borderless or decorated mode through the draft mode setter. */
void Window_setBorderless(Window *window, bool borderless) {
    Window_setUndecorated(window, borderless ? WINDOW_UNDECORATED_BORDERLESS : WINDOW_DECORATED);
}

// Report that the stub does not provide borderless chrome.
/** Returns false because borderless state is not retained. */
bool Window_isBorderless(const Window *window) {
    (void) window;
    return false;
}

// Accept the titlebar-flush request; the incomplete backend stores no chrome state.
/** Windows draft stub: ignores viewport flush alignment. */
void Window_setViewportFlushToTop(Window *window, bool flush) {
    (void) window;
    (void) flush;
}

// Report that viewport flush-to-top is unavailable in the stub backend.
/** Returns false because viewport alignment state is not retained. */
bool Window_isViewportFlushToTop(const Window *window) {
    (void) window;
    return false;
}

// Forward the compatibility request to the viewport-flush setter.
/** Maps the compatibility floating setting to viewport flush behavior. */
void Window_setFloatingTrafficLights(Window *window, bool floating) {
    Window_setViewportFlushToTop(window, floating);
}

;;PLATFORM_EXCLUSIVE("macOS") // No traffic lights on Win32: needs a Mac to mean anything.
/** Reports the unsupported macOS-only operation to stderr. */
void Window_macOS_setTrafficLightButtonVisible(Window *window, WindowTrafficLight light, bool visible) {
    (void) window;
    (void) light;
    (void) visible;
    fprintf(stderr, "window: Window_macOS_setTrafficLightButtonVisible is macOS-only (needs a Mac machine)\n");
}

;;PLATFORM_EXCLUSIVE("macOS") // No traffic lights on Win32.
/** Reports the unsupported query and returns false. */
bool Window_macOS_isTrafficLightButtonVisible(const Window *window, WindowTrafficLight light) {
    (void) window;
    (void) light;
    fprintf(stderr, "window: Window_macOS_isTrafficLightButtonVisible is macOS-only (needs a Mac machine)\n");
    return false;
}

;;PLATFORM_EXCLUSIVE("macOS") // No traffic lights on Win32.
/** Reports the unsupported macOS-only operation to stderr. */
void Window_macOS_setTrafficLightHeaderPosition(Window *window, float x, float y) {
    (void) window;
    (void) x;
    (void) y;
    fprintf(stderr, "window: Window_macOS_setTrafficLightHeaderPosition is macOS-only (needs a Mac machine)\n");
}

;;PLATFORM_EXCLUSIVE("macOS") // No traffic lights on Win32.
/** Reports the unsupported query and writes zero to supplied outputs. */
void Window_macOS_getTrafficLightHeaderPosition(const Window *window, float *outX, float *outY) {
    (void) window;
    fprintf(stderr, "window: Window_macOS_getTrafficLightHeaderPosition is macOS-only (needs a Mac machine)\n");
    if (outX)
        *outX = 0.0f;
    if (outY)
        *outY = 0.0f;
}

;;INCOMPLETE // ShowWindow(SW_MINIMIZE); no-op until implemented.
/** Windows draft stub: does not minimize a native window. */
void Window_minimize(Window *window) {
    (void) window;
}

;;INCOMPLETE // ShowWindow(SW_RESTORE); no-op until implemented.
/** Windows draft stub: does not restore a native window. */
void Window_restore(Window *window) {
    (void) window;
}

;;INCOMPLETE // IsIconic; false until implemented.
/** Returns false because minimized state is not implemented. */
bool Window_isMinimized(Window *window) {
    (void) window;
    return false;
}

;;INCOMPLETE // GWL_STYLE WS_MAXIMIZE; false until implemented.
/** Returns false because fullscreen state is not implemented. */
bool Window_isFullscreen(Window *window) {
    (void) window;
    return false;
}

;;INCOMPLETE // ShowWindow(SW_MAXIMIZE) vs SW_RESTORE; no-op until implemented.
/** Windows draft stub: ignores fullscreen changes. */
void Window_setFullscreen(Window *window, bool fullscreen) {
    (void) window;
    (void) fullscreen;
}

;;INCOMPLETE // Window setFullscreen wrapper; no-op until implemented.
/** Windows draft stub: does not toggle fullscreen state. */
void Window_toggleFullscreen(Window *window) {
    (void) window;
}

;;INCOMPLETE // SetWindowDisplayAffinity(WDA_MONITOR/WDA_NONE); no-op until implemented.
/** Windows draft stub: ignores display-recording protection changes. */
void Window_setDRM(Window *window, bool enabled) {
    (void) window;
    (void) enabled;
}

;;INCOMPLETE // AdjustWindowRectEx + GetWindowRect; no-op until implemented.
/** Windows draft stub: ignores minimum-size constraints. */
void Window_setMinSize(Window *window, int width, int height) {
    (void) window;
    (void) width;
    (void) height;
}

;;INCOMPLETE // AdjustWindowRectEx + GetWindowRect; no-op until implemented.
/** Windows draft stub: ignores maximum-size constraints. */
void Window_setMaxSize(Window *window, int width, int height) {
    (void) window;
    (void) width;
    (void) height;
}

;;INCOMPLETE // SetCursor; no-op until implemented.
/** Windows draft stub: ignores cursor changes. */
void Window_setCursorType(Window *window, WindowCursorType type) {
    (void) window;
    (void) type;
}

;;INCOMPLETE // GetCursor; returns default until implemented.
/** Returns the default cursor because cursor state is not tracked. */
WindowCursorType Window_getCursorType(const Window *window) {
    (void) window;
    return WINDOW_CURSOR_DEFAULT;
}

;;INCOMPLETE // Board composite; no-op until implemented.
/** Windows draft stub: does not composite board content. */
void Window_compositeBoards(Window *window) {
    (void) window;
}

;;INCOMPLETE
// Return no content-view handle because this platform implementation is a stub.
/** Returns nullptr because the stub has no native content view. */
void *Window_contentView(Window *window) {
    (void) window;
    return nullptr;
}

;;INCOMPLETE
// Return no native handle because the stub never creates an operating-system window.
/** Returns nullptr because the stub has no native window handle. */
void *Window_nativeHandle(const Window *window) {
    (void) window;
    return nullptr;
}

Window *Window_0(void) {
    return nullptr;
}

Window *Window_1(const char *title) {
    (void) title;
    return nullptr;
}

Window *Window_3(const char *title, int width, int height) {
    (void) title;
    (void) width;
    (void) height;
    return nullptr;
}

Window *Window_new(const WindowDesc *desc) {
    (void) desc;
    return nullptr;
}

uint32_t Window_id(Window *window) {
    (void) window;
    return 0;
}

uint64_t Window_sizeGeneration(Window *window) {
    (void) window;
    return 0;
}

bool Window_isFocused(Window *window) {
    (void) window;
    return false;
}

void Window_focus(Window *window) {
    (void) window;
}

bool Window_isKeyEnabled(const Window *window) {
    (void) window;
    return true;
}

void Window_setKeyEnabled(Window *window, bool enabled) {
    (void) window;
    (void) enabled;
}

bool Window_isLiveResizing(const Window *window) {
    (void) window;
    return false;
}

void Window_show(Window *window) {
    (void) window;
}

void Window_hide(Window *window) {
    (void) window;
}

void Window_close(Window *window) {
    Window_setShouldClose(window, true);
}

void Window_waitEvents(Window *window, int timeoutMs) {
    (void) window;
    (void) timeoutMs;
}

void Window_bringToFront(Window *window) {
    (void) window;
}

void Window_attachChild(Window *parent, Window *child) {
    (void) parent;
    (void) child;
}

void Window_detachChild(Window *parent, Window *child) {
    (void) parent;
    (void) child;
}

void Window_setAlwaysOnTop(Window *window, bool onTop) {
    (void) window;
    (void) onTop;
}

void Window_setClickThrough(Window *window, bool clickThrough) {
    (void) window;
    (void) clickThrough;
}

void Window_setShadow(Window *window, bool shadow) {
    (void) window;
    (void) shadow;
}

void Window_setMovableByBackground(Window *window, bool movable) {
    (void) window;
    (void) movable;
}

void Window_setOpacity(Window *window, float opacity) {
    (void) window;
    (void) opacity;
}

void Window_setTransparentBackground(Window *window, bool transparent) {
    (void) window;
    (void) transparent;
}

void Window_setBackdropBlur(Window *window, float radius) {
    (void) window;
    (void) radius;
}

void Window_setGravityTopLeft(Window *window) {
    (void) window;
}

void Window_setCursorLocked(Window *window, bool locked) {
    (void) window;
    (void) locked;
}

void *Window_metalLayer(Window *window) {
    (void) window;
    return nullptr;
}

void Window_setBottomLayer(Window *window, void *layer) {
    (void) window;
    (void) layer;
}

void *Window_getBottomLayer(const Window *window) {
    (void) window;
    return nullptr;
}

void Window_setTopLayer(Window *window, void *layer) {
    (void) window;
    (void) layer;
}

void *Window_getTopLayer(const Window *window) {
    (void) window;
    return nullptr;
}

void Window_orderLayers(Window *window) {
    (void) window;
}

bool Window_attachPanes(Window *window, Panel *panel, int width, int height) {
    (void) window;
    (void) panel;
    (void) width;
    (void) height;
    return false;
}

bool Window_resizePanes(Window *window, Panel *panel, int width, int height) {
    (void) window;
    (void) panel;
    (void) width;
    (void) height;
    return false;
}

void Window_compositePanes(Window *window, Panel *contentPanel) {
    (void) window;
    (void) contentPanel;
}

void Window_addKeyAdapter(Window *window, const KeyHandler *adapter) {
    (void) window;
    (void) adapter;
}

bool Window_removeKeyAdapter(Window *window, const KeyHandler *adapter) {
    (void) window;
    (void) adapter;
    return false;
}

void Window_addMouseAdapter(Window *window, const MouseHandler *adapter) {
    (void) window;
    (void) adapter;
}

bool Window_removeMouseAdapter(Window *window, const MouseHandler *adapter) {
    (void) window;
    (void) adapter;
    return false;
}

void Window_addTouchAdapter(Window *window, const TouchHandler *adapter) {
    (void) window;
    (void) adapter;
}

bool Window_removeTouchAdapter(Window *window, const TouchHandler *adapter) {
    (void) window;
    (void) adapter;
    return false;
}

void Window_dispatchEvents(Window *window) {
    (void) window;
}

void Window_workerPresentBegin(void) {
}

void Window_workerPresentEnd(void) {
}

bool Window_present(Window *window, const Buffer *frame) {
    (void) window;
    (void) frame;
    return false;
}

void Window_presentRGBA(Window *window, const void *pixels, size_t stride, int width, int height) {
    (void) window;
    (void) pixels;
    (void) stride;
    (void) width;
    (void) height;
}

void *Window_createPresentSurface(Window *window, int widthPx, int heightPx) {
    (void) window;
    (void) widthPx;
    (void) heightPx;
    return nullptr;
}

void Window_destroyPresentSurface(Window *window, void *surface) {
    (void) window;
    (void) surface;
}

void Window_presentSurface(Window *window, void *surface) {
    (void) window;
    (void) surface;
}

void *Window_presentSurfaceContents(const Window *window) {
    (void) window;
    return nullptr;
}

bool Window_readPresentSurface(Window *window, void *surface, void *destRGBA, size_t destStride) {
    (void) window;
    (void) surface;
    (void) destRGBA;
    (void) destStride;
    return false;
}

bool Window_writePNG(const void *pixels, size_t stride, int width, int height, const char *path) {
    (void) pixels;
    (void) stride;
    (void) width;
    (void) height;
    (void) path;
    return false;
}

bool Window_macOS_hasLiquidGlass(void) {
    return false;
}

void Window_macOS_setLiquidGlass(Window *window, const WindowLiquidGlassDesc *desc) {
    (void) window;
    (void) desc;
}

bool Window_macOS_getLiquidGlass(const Window *window, WindowLiquidGlassDesc *out) {
    (void) window;
    (void) out;
    return false;
}

struct TrafficLight {
    void *window;
    bool lightVisible[TRAFFIC_LIGHT_COUNT];
    float lightOX;
    float lightOY;
};

/** Fallback stub: creates traffic-light adapter for non-macOS platforms. */
TrafficLight *TrafficLight_create(void *nsWindowHandle) {
    TrafficLight *self = (TrafficLight*) calloc(1, sizeof(TrafficLight));
    if (self == nullptr)
        return nullptr;
    (*self).window = nsWindowHandle;
    (*self).lightVisible[TRAFFIC_LIGHT_CLOSE] = true;
    (*self).lightVisible[TRAFFIC_LIGHT_MINIATURIZE] = true;
    (*self).lightVisible[TRAFFIC_LIGHT_ZOOM] = true;
    (*self).lightOX = 0.0f;
    (*self).lightOY = 0.0f;
    return self;
}

/** Fallback stub: destroys traffic-light adapter. */
void TrafficLight_destroy(TrafficLight *self) {
    if (self != nullptr)
        free(self);
}

/** Fallback stub: resetBase no-op. */
void TrafficLight_resetBase(TrafficLight *self) {
    (void) self;
}

/** Fallback stub: refresh no-op. */
void TrafficLight_refresh(TrafficLight *self) {
    (void) self;
}

/** Fallback stub: sets button visibility. */
void TrafficLight_setButtonVisible(TrafficLight *self, TrafficLightButton button, bool visible) {
    if (self == nullptr || (int) button < 0 || button >= TRAFFIC_LIGHT_COUNT)
        return;
    (*self).lightVisible[button] = visible;
}

/** Fallback stub: reports button visibility. */
bool TrafficLight_isButtonVisible(const TrafficLight *self, TrafficLightButton button) {
    if (self == nullptr || (int) button < 0 || button >= TRAFFIC_LIGHT_COUNT)
        return false;
    return (*self).lightVisible[button];
}

/** Fallback stub: setHeaderPosition no-op. */
void TrafficLight_setHeaderPosition(TrafficLight *self, float x, float y) {
    (void) self;
    (void) x;
    (void) y;
}

/** Fallback stub: getHeaderPosition writes zero coordinates. */
void TrafficLight_getHeaderPosition(const TrafficLight *self, float *outX, float *outY) {
    (void) self;
    if (outX)
        *outX = 0.0f;
    if (outY)
        *outY = 0.0f;
}

/** Fallback stub: setFloating no-op. */
void TrafficLight_setFloating(TrafficLight *self, bool floating) {
    (void) self;
    (void) floating;
}

#endif
