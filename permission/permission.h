#ifndef PERMISSION_PERMISSION_H
#define PERMISSION_PERMISSION_H

#include <stdbool.h>

// permission/permission.h — OS capability permission requests (R1 host).
//
// Answers "may I do X?" for the capabilities a host app must ask the operating
// system for: screen capture, global key listening, event synthesis,
// accessibility control, camera, microphone, contacts, and kin. It is the R1
// prerequisite for the screen/input automation layer (screen capture + key
// listening + accessibility control) and for any app that needs a gated
// resource.
//
// CROSS-PLATFORM CONTRACT: a PermissionKind exists on every host; where the
// host has no such concept the status is PERMISSION_UNSUPPORTED. A kind is
// NEVER silently reported as granted.
//
// THE ASYNC + RESTART TRUTH (macOS): a request raises a prompt and the answer
// arrives LATER, so callers poll Permission_status in bounded slices (the
// Bounded Wait Law). Some grants (screen capture) only bind after the process
// RELAUNCHES — Permission_requiresRestart reports which. This class NEVER
// relaunches, exits, or re-execs the process: the user restarts on their own
// time, so the hotcwap hot-reload experience is never violated.
//
// THREAD CONTRACT: thread 0 only (the request calls touch OS UI).

typedef enum PermissionKind {
    PERMISSION_SCREEN_CAPTURE = 0, // see / record the screen
    PERMISSION_KEY_LISTEN,         // observe keystrokes globally (input monitoring)
    PERMISSION_POST_EVENT,         // synthesize input events for other apps
    PERMISSION_ACCESSIBILITY,      // read / control other apps' UI
    PERMISSION_CAMERA,
    PERMISSION_MICROPHONE,
    PERMISSION_CONTACTS,           // Apple-vexgraph-ecosystem-gitrepo only
    PERMISSION_CALENDAR,           // Apple-vexgraph-ecosystem-gitrepo only
    PERMISSION_PHOTOS,             // Apple-vexgraph-ecosystem-gitrepo only
    PERMISSION_LOCATION,
    PERMISSION_NOTIFICATIONS,
    PERMISSION_AUTOMATION,         // drive apps via AppleEvents / scripting
    PERMISSION_FULL_DISK,          // no programmatic prompt — settings only
    PERMISSION_COUNT
} PermissionKind;

typedef enum PermissionStatus {
    PERMISSION_UNSUPPORTED = 0,    // this host has no such concept
    PERMISSION_NOT_DETERMINED,     // never asked; a request would prompt
    PERMISSION_GRANTED,
    PERMISSION_DENIED,             // must be changed in OS settings
    PERMISSION_RESTRICTED          // policy / MDM blocks it
} PermissionStatus;

// Current status. NEVER prompts. An out-of-range kind reads UNSUPPORTED.
PermissionStatus Permission_status(PermissionKind kind);

// Raise the OS prompt for a kind. Returns true when the grant is already held
// OR a prompt was successfully raised; the ANSWER may arrive later, so poll
// Permission_status. False for unsupported kinds and when the host refuses to
// (re-)prompt. This never restarts the process.
bool Permission_request(PermissionKind kind);

// True when a granted status for this kind only binds after the process
// relaunches (macOS screen capture). This class does NOT relaunch; it reports.
bool Permission_requiresRestart(PermissionKind kind);

// Deep-link the OS settings pane where a denied permission is changed. False
// when the host has no pane for the kind.
bool Permission_openSettings(PermissionKind kind);

// Human-readable names. Never nullptr.
const char *Permission_kindName(PermissionKind kind);
const char *Permission_statusName(PermissionStatus status);

#endif
