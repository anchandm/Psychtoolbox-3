/*
  PsychToolbox3/Source/Linux/Screen/PsychWindowGlueWayland.c

  PLATFORMS:

  This is the Linux/Wayland version only.

  AUTHORS:

  Mario Kleiner      mk    mario.kleiner.de@gmail.com

  HISTORY:

  04-Jan-2015        mk    Created - Derived from Linux/Waffle version.

  DESCRIPTION:

  Functions in this file comprise an abstraction layer for probing and controlling window state, except for window content.  

  This is the Wayland specific backend.

  NOTES:

  Preformatted via: indent -linux -l240 -i4 PsychWindowGlueWayland.c

  TO DO: 

*/

/* Only enable the native Wayland backend at compile time if explicitely requested: */
#ifdef PTB_USE_WAYLAND

/* At the moment we still depend on Waffle even for "native" Wayland support.
 * Waffle is mostly used for initial Wayland connection setup, window creation,
 * and interacting with EGL to get the rendering api's setup and attached,
 * OpenGL contexts created and managed, GL extensions bound etc.
 *
 * Plan is to mix Wayland specific code with "minimal" Waffle here, and then incrementally
 * replace Waffle bits with Wayland bits as far as this is neccessary or useful.
 */
#ifndef PTB_USE_WAFFLE
#error PTB_USE_WAFFLE not defined for PTB_USE_WAYLAND build! We depend on Waffle at the moment, so this is a no-go!
#endif

#include "Screen.h"

#include <waffle.h>
#include <waffle_wayland.h>

// Number of currently open onscreen windows:
static int open_windowcount = 0;

// Tracking of currently bound OpenGL rendering context for master-thread:
static struct waffle_context *currentContext = NULL;

// Forward define prototype for glewContextInit(), which is normally not a public function:
GLenum glewContextInit(void);

#include <wayland-client.h>

// This header file and corresponding implementation file currently included in our
// source tree, as permitted by license. There's probably a better way to do this?
#include "presentation_timing-client-protocol.h"

#define ARRAY_LENGTH(a) (sizeof (a) / sizeof (a)[0])

// These are defined in PsychScreenGlueWayland.c:
extern struct waffle_display *waffle_display;
extern struct wl_display* wl_display;
extern struct wl_compositor* wl_compositor;
extern EGLDisplay egl_display;

extern uint32_t wayland_presentation_clock_id;
// wl_output* for all screens, from PsychScreenGlueWayland.c:
extern struct wl_output* displayWaylandOutputs[kPsychMaxPossibleDisplays];

// Read-only access this gem from PsychScreenGlue.c:
extern psych_bool displayOriginalCGSettingsValid[kPsychMaxPossibleDisplays];

// From PsychScreenGlueWayland.c:
struct presentation *get_wayland_presentation_extension(PsychWindowRecordType* windowRecord);

// Container with feedback about a completed swap - the equivalent of
// our good old INTEL_swap_event on X11/GLX, here for Wayland:
struct wayland_feedback {
    PsychWindowRecordType *windowRecord;
    unsigned int present_status; // 0 == Pending, 1 = Completed, 2 = Skipped.
    psych_uint64 sbc;
    psych_uint64 msc;
    psych_uint64 ust;
    uint32_t present_flags;
    struct presentation_feedback *feedback;
    struct timespec commit;
    struct timespec target;
    struct wl_list link;
    struct timespec present;
};

static void
destroy_wayland_feedback(struct wayland_feedback *wayland_feedback)
{
    if (wayland_feedback->feedback)
        presentation_feedback_destroy(wayland_feedback->feedback);

    wl_list_remove(&wayland_feedback->link);
    free(wayland_feedback);
}

static uint32_t
timespec_to_ms(const struct timespec *ts)
{
    return (uint32_t)ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
}

static void
timespec_from_proto(struct timespec *tm, uint32_t tv_sec_hi,
                    uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    tm->tv_sec = ((uint64_t)tv_sec_hi << 32) + tv_sec_lo;
    tm->tv_nsec = tv_nsec;
}

static uint64_t
timespec_to_us(const struct timespec *ts)
{
    return (uint64_t) ts->tv_sec * 1000000 + ts->tv_nsec / 1000;
}

static int
timespec_diff_to_usec(const struct timespec *a, const struct timespec *b)
{
    time_t secs = a->tv_sec - b->tv_sec;
    long nsec = a->tv_nsec - b->tv_nsec;

    return secs * 1000000 + nsec / 1000;
}

static char *
wayland_pflags_to_str(uint32_t flags, char *str, unsigned len)
{
    static const struct {
        uint32_t flag;
        char sym;
    } desc[] = {
        { PRESENTATION_FEEDBACK_KIND_VSYNC, 's' },
        { PRESENTATION_FEEDBACK_KIND_HW_CLOCK, 'c' },
        { PRESENTATION_FEEDBACK_KIND_HW_COMPLETION, 'e' },
        { PRESENTATION_FEEDBACK_KIND_ZERO_COPY, 'z' },
    };
    unsigned i;

    *str = '\0';
    if (len < ARRAY_LENGTH(desc) + 1)
        return str;

    for (i = 0; i < ARRAY_LENGTH(desc); i++)
        str[i] = flags & desc[i].flag ? desc[i].sym : '_';
    str[ARRAY_LENGTH(desc)] = '\0';

    return str;
}

static void
wayland_feedback_presented(void *data,
                            struct presentation_feedback *presentation_feedback,
                            uint32_t tv_sec_hi,
                            uint32_t tv_sec_lo,
                            uint32_t tv_nsec,
                            uint32_t refresh_nsec,
                            uint32_t seq_hi,
                            uint32_t seq_lo,
                            uint32_t flags)
{
    struct wayland_feedback *wayland_feedback = data;
    PsychWindowRecordType *windowRecord = wayland_feedback->windowRecord;
    uint64_t msc = ((uint64_t) seq_hi << 32) + seq_lo;
    uint32_t commit, present;
    uint32_t f2c, c2p, f2p;
    int p2p, t2p;
    char flagstr[10];

    // Translate protocol presentation timestamp to a timespec:
    timespec_from_proto(&wayland_feedback->present, tv_sec_hi, tv_sec_lo, tv_nsec);

    commit = timespec_to_ms(&wayland_feedback->commit);
    present = timespec_to_ms(&wayland_feedback->present);

    // Time delta in msecs between swap scheduled and completed:
    c2p = present - commit;

    // Compute delta in usecs between successive swaps:
    if (wayland_feedback->sbc > 1) {
        p2p = timespec_to_us(&wayland_feedback->present) - windowRecord->reference_ust;

        // Compute delta in usecs between target time and true time of swap:
        t2p = timespec_diff_to_usec(&wayland_feedback->present, &wayland_feedback->target);
    }
    else {
        p2p = 0;
        t2p = 0;
    }

    if (PsychPrefStateGet_Verbosity() > 4) {
        printf("PTB-DEBUG: SBC = %6lu: c2p %4u ms, p2p %5d us (refresh %6u us), t2p %6d us, [%s] MSC %" PRIu64 "\n",
               wayland_feedback->sbc, c2p, p2p, refresh_nsec / 1000, t2p, wayland_pflags_to_str(flags, flagstr, sizeof(flagstr)), msc);
    }

    // Record sbc, msc, ust, present flags of this completed swap:

    // Once for the feedback events used by our Screen('GetFlipInfo') logging functions:
    wayland_feedback->present_status = 1; // "Presented".
    wayland_feedback->present_flags = flags;
    wayland_feedback->msc = msc;
    wayland_feedback->ust = timespec_to_us(&wayland_feedback->present);

    // Once for the windowRecord for internal use by PsychOSGetSwapCompletionTimestamp() internal
    // timestamping and swap reliability asessment:
    windowRecord->reference_sbc = wayland_feedback->sbc;
    windowRecord->reference_msc = msc;
    windowRecord->reference_ust = wayland_feedback->ust;
    windowRecord->swapcompletiontype = flags;

    // Delete our own completion wayland_feedback container if swap completion logging
    // isn't enabled:
    if (windowRecord->swapevents_enabled == 0) destroy_wayland_feedback(wayland_feedback);

    return;
}

static void
wayland_feedback_discarded(void *data, struct presentation_feedback *presentation_feedback)
{
    struct wayland_feedback *wayland_feedback = data;
    PsychWindowRecordType *windowRecord = wayland_feedback->windowRecord;

    if (PsychPrefStateGet_Verbosity() > 1) printf("PTB-WARNING: Wayland presentation request discarded for SBC = %lu !\n", wayland_feedback->sbc);

    // Once for the feedback events used by our Screen('GetFlipInfo') logging functions:
    wayland_feedback->present_status = 2; // "Discarded/Skipped".
    wayland_feedback->present_flags = 0;
    wayland_feedback->msc = 0;
    wayland_feedback->ust = 0;

    // Once for the windowRecord for internal use by PsychOSGetSwapCompletionTimestamp() internal
    // timestamping and swap reliability asessment:
    windowRecord->reference_sbc = wayland_feedback->sbc;
    windowRecord->reference_msc = 0;
    windowRecord->reference_ust = 0;
    windowRecord->swapcompletiontype = -1; // "Presentation discarded/Skipped"

    // Delete our own completion wayland_feedback container if swap completion logging
    // isn't enabled:
    if (windowRecord->swapevents_enabled == 0) destroy_wayland_feedback(wayland_feedback);
}

static void
wayland_feedback_sync_output(void *data, struct presentation_feedback *presentation_feedback, struct wl_output *output)
{
    struct wayland_feedback *wayland_feedback = data;
    PsychWindowRecordType *windowRecord = wayland_feedback->windowRecord;

    if (PsychPrefStateGet_Verbosity() > 3) {
        printf("PTB-INFO: Window %i uses Wayland output %p with following properties as reference:\n", windowRecord->windowIndex, output);
    }
}

static const struct presentation_feedback_listener wayland_feedback_listener = {
    wayland_feedback_sync_output,
    wayland_feedback_presented,
    wayland_feedback_discarded
};

static void
wayland_window_create_feedback(PsychWindowRecordType* windowRecord)
{
    struct presentation *wayland_pres = get_wayland_presentation_extension(windowRecord);
    struct wayland_feedback *wayland_feedback;
    struct wl_surface *wl_surface;
    struct waffle_wayland_window *wayland_window;
    struct waffle_window* window = windowRecord->targetSpecific.windowHandle;

    // Retrieve underlying native Wayland window:
    union waffle_native_window *wafflewin = waffle_window_get_native(window);
    wayland_window = wafflewin->wayland;
    wl_surface = wayland_window->wl_surface;
    free(wafflewin);
    wafflewin = NULL;

    // Noop if presentation_feedback extension unavailable:
    if (!wayland_pres) return;

    // Create our own wayland_feedback container for swap completion events:
    wayland_feedback = calloc(1, sizeof(*wayland_feedback));
    if (!wayland_feedback) return;

    wayland_feedback->windowRecord = windowRecord;
    wayland_feedback->sbc = ++(windowRecord->submitted_sbc);

    // Create wayland present_feedback object, attach listener callbacks to it:
    wayland_feedback->feedback = presentation_feedback(wayland_pres, wl_surface);
    presentation_feedback_add_listener(wayland_feedback->feedback, &wayland_feedback_listener, wayland_feedback);

    // Record approximate time of swap submission according to the compositor clock:
    clock_gettime(wayland_presentation_clock_id, &wayland_feedback->commit);
    wayland_feedback->target = wayland_feedback->commit;

    // Add to our queue of pending swap completion events:
    wl_list_insert(&windowRecord->targetSpecific.presentation_feedback_list, &wayland_feedback->link);
}

/*
  PsychOSOpenOnscreenWindow()

  Creates the pixel format and the context objects and then instantiates the context onto the screen.

  -The pixel format and the context are stored in the target specific field of the window recored.  Close
  should clean up by destroying both the pixel format and the context.

  -We mantain the context because it must be be made the current context by drawing functions to draw into 
  the specified window.

  -We maintain the pixel format object because there seems to be now way to retrieve that from the context.

  -To tell the caller to clean up PsychOSOpenOnscreenWindow returns FALSE if we fail to open the window. It 
  would be better to just issue an PsychErrorExit() and have that clean up everything allocated outside of
  PsychOpenOnscreenWindow().
*/
psych_bool PsychOSOpenOnscreenWindow(PsychScreenSettingsType * screenSettings, PsychWindowRecordType * windowRecord, int numBuffers, int stereomode, int conserveVRAM)
{
    char windowTitle[32];
    PsychRectType screenrect;
    CGDirectDisplayID dpy;
    int scrnum;
    unsigned long mask;
    Window win = 0;
    int i, x, y, width, height, nrconfigs, buffdepth;
    GLenum glerr;
    int32_t attrib[41];
    int attribcount = 0;
    int stereoenableattrib = 0;
    int depth, bpc;
    int windowLevel;
    int32_t opengl_api;
    char backendname[16];
    char backendname2[16];
    struct waffle_config *config;
    struct waffle_window *window;
    union waffle_native_window *wafflewin;
    struct waffle_context *ctx;

    // Define default rendering backend:
    #ifdef PTB_USE_GLES1
    opengl_api = WAFFLE_CONTEXT_OPENGL_ES1;
    #else
    opengl_api = WAFFLE_CONTEXT_OPENGL;
    #endif

    // Map the logical screen number to the corresponding X11 display connection handle
    // for the corresponding X-Server connection.
    PsychGetCGDisplayIDFromScreenNumber(&dpy, screenSettings->screenNumber);

    // TODO FIXME: We currently don't have any way of selecting the target X-Screen 'scrnum' for
    // our window, as Waffle does not yet support selection of X-Screen. It always opens
    // windows on the display's default screen. Therefore this is mostly a dead placeholder for now:
    scrnum = PsychGetXScreenIdForScreen(screenSettings->screenNumber);

    // Include onscreen window index in title:
    sprintf(windowTitle, "PTB Onscreen Window [%i]:", windowRecord->windowIndex);

    // Set windowing system backend type for this window to Wayland:
    windowRecord->winsysType = (int) WAFFLE_PLATFORM_WAYLAND;

    // Translate spec to human readable name and spec string:
    sprintf(backendname, "Wayland/EGL");
    sprintf(backendname2, "wayland");

    // Announce actual choice of backend to runtime environment. This is a marker
    // to, e.g., moglcore, so it can adapt its context/gl setup:
    setenv("PSYCH_USE_DISPLAY_BACKEND", backendname2, 1);

    if (PsychPrefStateGet_Verbosity() > 2) {
        printf("PTB-INFO: Wayland display backend '%s' initialized [%s].\n", backendname, backendname2);
    }

    // Allow usercode to override our default backend choice via environment variable:
    if (getenv("PSYCH_USE_GFX_BACKEND")) {
        if (!strcmp(getenv("PSYCH_USE_GFX_BACKEND"), "gl")) opengl_api = WAFFLE_CONTEXT_OPENGL;
        if (!strcmp(getenv("PSYCH_USE_GFX_BACKEND"), "gles1")) opengl_api = WAFFLE_CONTEXT_OPENGL_ES1;
        if (!strcmp(getenv("PSYCH_USE_GFX_BACKEND"), "gles2")) opengl_api = WAFFLE_CONTEXT_OPENGL_ES2;
        if (!strcmp(getenv("PSYCH_USE_GFX_BACKEND"), "gles3")) opengl_api = WAFFLE_CONTEXT_OPENGL_ES3;
    }

    switch (opengl_api) {
        case WAFFLE_CONTEXT_OPENGL:
            sprintf(backendname, "OpenGL");
            break;

        case WAFFLE_CONTEXT_OPENGL_ES1:
            sprintf(backendname, "OpenGL-ES 1");
            break;

        case WAFFLE_CONTEXT_OPENGL_ES2:
            sprintf(backendname, "OpenGL-ES 2");
            break;

        case WAFFLE_CONTEXT_OPENGL_ES3:
            sprintf(backendname, "OpenGL-ES 3");
            break;

        default:
            sprintf(backendname, "UNKNOWN");
    }

    // Try the requested backend, try alternate backends on failure:
    if (!waffle_display_supports_context_api(waffle_display, opengl_api)) {
        if (PsychPrefStateGet_Verbosity() > 1) printf("PTB-WARNING: Selected Waffle display backend does not support requested OpenGL rendering API '%s': %s. Trying fallbacks...\n",
                                                      backendname, waffle_error_to_string(waffle_error_get_code()));
        // Try fallbacks: OpenGL > OpenGL-ES1 > OpenGL-ES2 > OpenGL-ES3
        if (waffle_display_supports_context_api(waffle_display, WAFFLE_CONTEXT_OPENGL)) {
            opengl_api = WAFFLE_CONTEXT_OPENGL;
        }
        else if (waffle_display_supports_context_api(waffle_display, WAFFLE_CONTEXT_OPENGL_ES1)) {
            opengl_api = WAFFLE_CONTEXT_OPENGL_ES1;
        }
        else if (waffle_display_supports_context_api(waffle_display, WAFFLE_CONTEXT_OPENGL_ES2)) {
            opengl_api = WAFFLE_CONTEXT_OPENGL_ES2;
        }
        else if (waffle_display_supports_context_api(waffle_display, WAFFLE_CONTEXT_OPENGL_ES3)) {
            opengl_api = WAFFLE_CONTEXT_OPENGL_ES3;
        }
        else {
            // Game over:
            if (PsychPrefStateGet_Verbosity() > 0) printf("PTB-ERROR: Waffle display backend does not support any OpenGL rendering API: %s. Game over!\n",
                                                          waffle_error_to_string(waffle_error_get_code()));
            return(FALSE);
        }
    }

    // Assign human readable name to selected OpenGL rendering api. Also assign
    // type flag for higher-level code:
    switch (opengl_api) {
        case WAFFLE_CONTEXT_OPENGL:
            sprintf(backendname, "OpenGL");
            sprintf(backendname2, "gl");
            windowRecord->glApiType = 0;
            break;

        case WAFFLE_CONTEXT_OPENGL_ES1:
            sprintf(backendname, "OpenGL-ES 1");
            sprintf(backendname2, "gles1");
            windowRecord->glApiType = 10;
            break;

        case WAFFLE_CONTEXT_OPENGL_ES2:
            sprintf(backendname, "OpenGL-ES 2");
            sprintf(backendname2, "gles2");
            windowRecord->glApiType = 20;
            break;

        case WAFFLE_CONTEXT_OPENGL_ES3:
            sprintf(backendname, "OpenGL-ES 3");
            sprintf(backendname2, "gles3");
            windowRecord->glApiType = 30;
            break;

        default:
            sprintf(backendname, "UNKNOWN");
            sprintf(backendname2, "X");
            windowRecord->glApiType = 0;
    }

    // Tell environment about our final rendering backend choice:
    setenv("PSYCH_USE_GFX_BACKEND", backendname2, 1);

    if (PsychPrefStateGet_Verbosity() > 2) printf("PTB-INFO: Waffle display backend connected to OpenGL rendering API '%s' [%s].\n", backendname, backendname2);

    // OpenGL embedded subset api in use?
    if (windowRecord->glApiType > 0) {
        // Yes: Try to disable all OpenGL error checking for now during initial OpenGL-ES support bringup.
        // We know there will be many errors due to incompatibilities but just try to keep going and rely
        // on external trace and debug tools to find and fix errors at this point:
        PsychPrefStateSet_ConserveVRAM(PsychPrefStateGet_ConserveVRAM() | kPsychAvoidCPUGPUSync);
        if (PsychPrefStateGet_Verbosity() > 2) printf("PTB-INFO: Will try to disable/suppress all internal OpenGL error reporting/handling for OpenGL-ES operation.\n");
    }

    // Mark window as EGL controlled:
    windowRecord->specialflags |= kPsychIsEGLWindow;

    // Retrieve windowLevel, an indicator of where non-fullscreen windows should
    // be located wrt. to other windows. 0 = Behind everything else, occluded by
    // everything else. 1 - 999 = At layer windowLevel -> Occludes stuff on layers "below" it.
    // 1000 - 1999 = At highest level, but partially translucent / alpha channel allows to make
    // regions transparent. 2000 or higher: Above everything, fully opaque, occludes everything.
    // 2000 is the default.
    windowLevel = PsychPrefStateGet_WindowShieldingLevel();

    // Init userspace GL context to safe default:
    windowRecord->targetSpecific.glusercontextObject = NULL;
    windowRecord->targetSpecific.glswapcontextObject = NULL;

    // Default to use of one shared x-display connection "dpy" for all onscreen windows
    // on a given x-display and x-screen:
    // windowRecord->targetSpecific.privDpy = dpy;
    // HACK TODO:
    windowRecord->targetSpecific.privDpy = NULL;

    // Check if this should be a fullscreen window:
    PsychGetScreenRect(screenSettings->screenNumber, screenrect);
    if (PsychMatchRect(screenrect, windowRecord->rect)) windowRecord->specialflags |= kPsychIsFullscreenWindow;

    PsychGetGlobalScreenRect(screenSettings->screenNumber, screenrect);
    if (PsychMatchRect(screenrect, windowRecord->rect)) windowRecord->specialflags |= kPsychIsFullscreenWindow;

    if (windowRecord->specialflags & kPsychIsFullscreenWindow) {
        // This is supposed to be a fullscreen window with the dimensions of
        // the current display/desktop:
        x = 0;
        y = 0;
        width = PsychGetWidthFromRect(screenrect);
        height = PsychGetHeightFromRect(screenrect);

        // Mark this window as fullscreen window:
        windowRecord->specialflags |= kPsychIsFullscreenWindow;

        // Copy absolute screen location and area of window to 'globalrect',
        // so functions like Screen('GlobalRect') can still query the real
        // bounding gox of a window onscreen:
        PsychGetGlobalScreenRect(screenSettings->screenNumber, windowRecord->globalrect);
    } else {
        // Window size different from current screen size:
        // A regular desktop window with borders and control icons is requested, e.g., for debugging:
        // Extract settings:
        x = windowRecord->rect[kPsychLeft];
        y = windowRecord->rect[kPsychTop];
        width = PsychGetWidthFromRect(windowRecord->rect);
        height = PsychGetHeightFromRect(windowRecord->rect);

        // Copy absolute screen location and area of window to 'globalrect',
        // so functions like Screen('GlobalRect') can still query the real
        // bounding gox of a window onscreen:
        PsychCopyRect(windowRecord->globalrect, windowRecord->rect);
    }

    // Select OpenGL context API for use with this window:
    attrib[attribcount++] = WAFFLE_CONTEXT_API;
    attrib[attribcount++] = opengl_api;

    // Which display depth is supported?
    depth = PsychGetValueFromDepthStruct(0, &(screenSettings->depth));

    // Select requested depth per color component 'bpc' for each channel:
    bpc = 8; // We default to 8 bpc == RGBA8
    if (windowRecord->depth == 30) {
        bpc = 10;
        if (PsychPrefStateGet_Verbosity() > 2) printf("PTB-INFO: Trying to enable at least 10 bpc fixed point framebuffer.\n");
    }
    if (windowRecord->depth == 33) {
        bpc = 11;
        if (PsychPrefStateGet_Verbosity() > 2) printf("PTB-INFO: Trying to enable at least 11 bpc fixed point framebuffer.\n");
    }
    if (windowRecord->depth == 48) {
        bpc = 16;
        if (PsychPrefStateGet_Verbosity() > 2) printf("PTB-INFO: Trying to enable at least 16 bpc fixed point framebuffer.\n");
    }
    if (windowRecord->depth == 64) {
        bpc = 16;
        if (PsychPrefStateGet_Verbosity() > 2) printf("PTB-INFO: Trying to enable 16 bpc fixed point framebuffer.\n");
    }
    if (windowRecord->depth == 128) {
        bpc = 32;
        if (PsychPrefStateGet_Verbosity() > 2) printf("PTB-INFO: Trying to enable 32 bpc fixed point framebuffer.\n");
    }

    attrib[attribcount++] = WAFFLE_RED_SIZE;   // Setup requested minimum depth of each color channel:
    attrib[attribcount++] = (depth > 16) ? bpc : 1;
    attrib[attribcount++] = WAFFLE_GREEN_SIZE;
    attrib[attribcount++] = (depth > 16) ? bpc : 1;
    attrib[attribcount++] = WAFFLE_BLUE_SIZE;
    attrib[attribcount++] = (depth > 16) ? bpc : 1;
    attrib[attribcount++] = WAFFLE_ALPHA_SIZE;
    // Alpha channel needs special treatment:
    if ((bpc != 10) && (bpc != 11)) {
        // Non 10/11 bpc drawable: Request a 'bpc' alpha channel if the underlying framebuffer
        // is in true-color mode ( >= 24 bpp format). If framebuffer is in 16 bpp mode, we
        // don't have/request an alpha channel at all:
        attrib[attribcount++] = (depth > 16) ? bpc : 0; // In 16 bit mode, we don't request an alpha-channel.
    } else if (bpc == 10) {
        // 10 bpc drawable: We have a 32 bpp pixel format with R10G10B10 10 bpc per color channel.
        // There are at most 2 bits left for the alpha channel, so we request an alpha channel with
        // minimum size 1 bit --> Will likely translate into a 2 bit alpha channel:
        attrib[attribcount++] = 1;
    }
    else {
        // 11 bpc drawable - or more likely a 32 bpp drawable with R11G11B10, ie., all 32 bpp
        // used up by RGB color info and no space for alpha bits. Therefore do not request an
        // alpha channel:
        attrib[attribcount++] = 0;
    }

    // Stereo display support: If stereo display output is requested with OpenGL native stereo,
    // we request a stereo-enabled rendering context. Or at least we would, if Waffle would support this.
    if (stereomode == kPsychOpenGLStereo) {
        if (PsychPrefStateGet_Verbosity() > 2) printf("PTB-INFO: OpenGL native quad-buffered stereomode requested, but this is not supported by the Wayland backend.\n");
    }

    // Multisampling support:
    if (windowRecord->multiSample > 0) {
        // Request a multisample buffer:
        attrib[attribcount++] = WAFFLE_SAMPLE_BUFFERS;
        attrib[attribcount++] = 1;

        // Request at least multiSample samples per pixel:
        attrib[attribcount++] = WAFFLE_SAMPLES;
        attrib[attribcount++] = windowRecord->multiSample;
    }

    // Support for OpenGL 3D rendering requested?
    if (PsychPrefStateGet_3DGfx()) {
        // Yes. Allocate and attach a 24 bit depth buffer and 8 bit stencil buffer:
        attrib[attribcount++] = WAFFLE_DEPTH_SIZE;
        attrib[attribcount++] = 24;
        attrib[attribcount++] = WAFFLE_STENCIL_SIZE;
        attrib[attribcount++] = 8;

        // Alloc an accumulation buffer as well?
        if (PsychPrefStateGet_3DGfx() & 2) {
            // Yes: Allocate an accumulation buffer if possible:
            attrib[attribcount++] = WAFFLE_ACCUM_BUFFER;
            attrib[attribcount++] = 1;
        }
    }

    // Double buffering requested?
    attrib[attribcount++] = WAFFLE_DOUBLE_BUFFERED;
    attrib[attribcount++] = (numBuffers >= 2) ? 1 : 0;

    // Finalize attrib array:
    attrib[attribcount++] = 0;

    PsychLockDisplay();

    // Choose waffle configuration for attrib's - the equivalent of
    // a pixelformat or framebuffer config in GLX speak:
    config = waffle_config_choose(waffle_display, attrib);

    if (!config) {
        // Failed to find matching visual: Could it be related to request for unsupported native 10/11/16 bpc framebuffer?
        if (((windowRecord->depth == 30) && (bpc == 10)) || ((windowRecord->depth == 33) && (bpc == 11)) || ((windowRecord->depth == 48) && (bpc == 16))) {
            // 10/11/16 bpc framebuffer requested: Let's see if we can get a visual by lowering our demand to 8 bpc:
            for (i = 0; i < attribcount && attrib[i] != WAFFLE_RED_SIZE; i++);
            attrib[i + 1] = 8;
            for (i = 0; i < attribcount && attrib[i] != WAFFLE_GREEN_SIZE; i++);
            attrib[i + 1] = 8;
            for (i = 0; i < attribcount && attrib[i] != WAFFLE_BLUE_SIZE; i++);
            attrib[i + 1] = 8;
            for (i = 0; i < attribcount && attrib[i] != WAFFLE_ALPHA_SIZE; i++);
            attrib[i + 1] = 1;

            // Retry:
            config = waffle_config_choose(waffle_display, attrib);
        }
    }

    if (!config) {
        // Failed to find matching visual: Could it be related to multisampling?
        if (windowRecord->multiSample > 0) {
            // Multisampling requested: Let's see if we can get a visual by
            // lowering our demand:
            for (i = 0; i < attribcount && attrib[i] != WAFFLE_SAMPLES; i++);
            while (!config && windowRecord->multiSample > 0) {
                attrib[i + 1]--;
                windowRecord->multiSample--;
                config = waffle_config_choose(waffle_display, attrib);
            }

            // Either we have a valid visual at this point or we still fail despite
            // requesting zero samples.
            if (!config) {
                // We still fail. Disable multisampling by requesting zero multisample buffers:
                for (i = 0; i < attribcount && attrib[i] != WAFFLE_SAMPLE_BUFFERS; i++);
                windowRecord->multiSample = 0;
                attrib[i + 1] = 0;
                config = waffle_config_choose(waffle_display, attrib);
            }
        }

        // Worked? If not, see if we can lower our 3D settings if in 3D mode:
        if (!config && PsychPrefStateGet_3DGfx()) {
            // Ok, retry with a 16 bit depth buffer...
            for (i = 0; i < attribcount && attrib[i] != WAFFLE_DEPTH_SIZE; i++);
            if (attrib[i] == WAFFLE_DEPTH_SIZE && i < attribcount) attrib[i + 1] = 16;
            if (PsychPrefStateGet_Verbosity() > 1) printf("PTB-WARNING: Have to use 16 bit depth buffer instead of 24 bit buffer due to limitations of your gfx-hardware or driver. Accuracy of 3D-Gfx may be limited...\n");
            config = waffle_config_choose(waffle_display, attrib);

            if (!config) {
                // Failed again. Retry with disabled stencil buffer:
                if (PsychPrefStateGet_Verbosity() > 1) printf("PTB-WARNING: Have to disable stencil buffer due to limitations of your gfx-hardware or driver. Some 3D Gfx algorithms may fail...\n");
                for (i = 0; i < attribcount && attrib[i] != WAFFLE_STENCIL_SIZE; i++);
                if (attrib[i] == WAFFLE_STENCIL_SIZE && i < attribcount) attrib[i + 1] = 0;
                config = waffle_config_choose(waffle_display, attrib);
            }
        }
    }

    if (!config) {
        // Ok, nothing worked. Require the absolute minimum configuration:
        if (PsychPrefStateGet_Verbosity() > 1) printf("PTB-WARNING: Could not get any display configuration matching your minimum requirements. Trying to get the bare minimum config of a RGB framebuffer of arbitrarily low resolution now.\n");
        attribcount = 0;
        attrib[attribcount++] = WAFFLE_CONTEXT_API;
        attrib[attribcount++] = opengl_api;
        attrib[attribcount++] = WAFFLE_RED_SIZE;
        attrib[attribcount++] = 1;
        attrib[attribcount++] = WAFFLE_GREEN_SIZE;
        attrib[attribcount++] = 1;
        attrib[attribcount++] = WAFFLE_BLUE_SIZE;
        attrib[attribcount++] = 1;
        attrib[attribcount++] = WAFFLE_ALPHA_SIZE;
        attrib[attribcount++] = WAFFLE_DONT_CARE;
        config = waffle_config_choose(waffle_display, attrib);
    }

    // Finally?
    if (!config) {
        // Game over:
        if (PsychPrefStateGet_Verbosity() > 0) printf("\nPTB-ERROR[waffle_config_choose() failed]: Couldn't get any suitable visual from display backend.\n\n");
        PsychUnlockDisplay();
        return (FALSE);
    }

    // Create our onscreen window:
    window = waffle_window_create(config, width, height);
    wafflewin = waffle_window_get_native(window);

    // Set hints for window sizing and positioning:
    // TODO FIXME Wayland...
    {
        XSizeHints sizehints;
        sizehints.x = x;
        sizehints.y = y;
        sizehints.width = width;
        sizehints.height = height;
        // Let window manager control window position if kPsychGUIWindowWMPositioned is set:
        sizehints.flags = USSize | (windowRecord->specialflags & kPsychGUIWindowWMPositioned) ? 0 : USPosition;
    }

    // Create associated OpenGL rendering context: We use ressource
    // sharing of textures, display lists, FBO's and shaders if 'slaveWindow'
    // is assigned for that purpose as master-window.
    ctx = waffle_context_create(config, ((windowRecord->slaveWindow) ? windowRecord->slaveWindow->targetSpecific.contextObject : NULL));
    if (!ctx) {
        if (PsychPrefStateGet_Verbosity() > 0) printf("\nPTB-ERROR:[waffle_context_create() failed] OpenGL context creation failed: %s\n\n", waffle_error_to_string(waffle_error_get_code()));
        PsychUnlockDisplay();
        return (FALSE);
    }

    // Store the handles:

    // windowHandle is a waffle_window*:
    windowRecord->targetSpecific.windowHandle = window;

    // xwindowHandle stores the underlying X-Window:
    // TODO FIXME wl_surface, or wl_shell_surface?
    windowRecord->targetSpecific.xwindowHandle = win;

    // waffle_display*
    windowRecord->targetSpecific.deviceContext = waffle_display;

    // waffle_context*
    windowRecord->targetSpecific.contextObject = ctx;

    // Create rendering context for async flips with identical config as main context, share all heavyweight ressources with it:
    windowRecord->targetSpecific.glswapcontextObject = waffle_context_create(config, windowRecord->targetSpecific.contextObject);
    if (windowRecord->targetSpecific.glswapcontextObject == NULL) {
        if (PsychPrefStateGet_Verbosity() > 0) printf("\nPTB-ERROR[SwapContextCreation failed]: Creating a private OpenGL context for async flips failed for unknown reasons.\n\n");
        PsychUnlockDisplay();
        return (FALSE);
    }

    // External 3D graphics support enabled?
    if (PsychPrefStateGet_3DGfx()) {
        // Yes. We need to create an extra OpenGL rendering context for the external
        // OpenGL code to provide optimal state-isolation. The context shares all
        // heavyweight ressources likes textures, FBOs, VBOs, PBOs, display lists and
        // starts off as an identical copy of PTB's context as of here.

        // Create rendering context with identical config as main context, share all heavyweight ressources with it:
        windowRecord->targetSpecific.glusercontextObject = waffle_context_create(config, windowRecord->targetSpecific.contextObject);
        if (windowRecord->targetSpecific.glusercontextObject == NULL) {
            if (PsychPrefStateGet_Verbosity() > 0) printf("\nPTB-ERROR[UserContextCreation failed]: Creating a private OpenGL context for MOGL failed for unknown reasons.\n\n");
            PsychUnlockDisplay();
            return (FALSE);
        }
    }

    // Release config info:
    waffle_config_destroy(config);

    // Setup window transparency:
    if ((windowLevel >= 1000) && (windowLevel < 2000)) {
        // For windowLevels between 1000 and 1999, make the window background transparent, so standard GUI
        // would be visible, wherever nothing is drawn, i.e., where alpha channel is zero:

        // Levels 1000 - 1499 and 1500 to 1999 map to a master opacity level of 0.0 - 1.0:        
        unsigned int opacity = (unsigned int)(0xffffffff * (((float)(windowLevel % 500)) / 499.0));

        // TODO FIXME Wayland...
    }

    // If this window is a GUI window then enable all window decorations and
    // manipulations, except for the window close button, which would wreak havoc:
    if (windowRecord->specialflags & kPsychGUIWindow) {
        // TODO Wayland
    }

    // For windowLevels of at least 500, tell window manager to try to keep
    // our window above most other windows, by setting the state to WM_STATE_ABOVE:
    if (windowLevel >= 500) {
        // TODO Wayland
    }

    // Sync with server:
    wl_display_roundtrip(wl_display);

    // Show our new window:
    if (windowLevel != -1) {
        struct waffle_wayland_window *wayland_window = wafflewin->wayland;
        struct wl_region *region = wl_compositor_create_region(wl_compositor);

        // Is this window supposed to be transparent to user input (mouse, keyboard etc.)?
        if ((windowLevel >= 1000) && (windowLevel < 1500)) {
            // Yes. Assign our currently empty 'region' as input region, so
            // the surface doesn't care about input but input is sent to
            // surfaces below our onscreen windows surface:
            wl_surface_set_input_region(wayland_window->wl_surface, region);

            // Note: The default input region is infinite, covering the whole
            // surface, ie., the whole surface accepts user input. Therefore
            // nothing to do in the regular case.
        }

        // Is this window supposed to be opaque / non-transparent?
        if (windowLevel < 1000 || windowLevel >= 2000) {
            // Yes. Define an opaque region the full size of the windows area:
            wl_region_add(region, 0, 0, width, height);
            wl_surface_set_opaque_region(wayland_window->wl_surface, region);
        }

        // Done with defining input and display regions, so destroy our region:
        wl_region_destroy(region);

        // Is this supposed to be a fullscreen window?
        if ((windowRecord->specialflags & kPsychIsFullscreenWindow) && (windowLevel < 1000 || windowLevel >= 2000)) {
            // Opaque fullscreen onscreen window setup:
            // WL_SHELL_SURFACE_FULLSCREEN_METHOD_DRIVER - Switch video output to optimal resolution to accomodate the
            // size of the surface, ie., the mode with the smallest resolution that can contain the surface. Add black
            // borders for padding if a perfect fit isn't possible.
            // TODO implement: 0 = Target refresh rate -- Zero == Don't change rate.
            if (PsychPrefStateGet_Verbosity() > 3) {
                printf("PTB-INFO: Opening fullscreen onscreen wl_shell_surface() window on screen %i - wl_output %p\n",
                       screenSettings->screenNumber, displayWaylandOutputs[screenSettings->screenNumber]);
            }

            // Video refresh rate switch requested?
            if (displayOriginalCGSettingsValid[screenSettings->screenNumber]) {
                // Video refresh rate switch requested:
                int milliHz = (int) (PsychGetNominalFramerate(screenSettings->screenNumber) * 1000.0 + 0.5);
                if (PsychPrefStateGet_Verbosity() > 3) {
                    printf("PTB-INFO: Requesting video mode switch for fullscreen windw to a target of %i x %i pixels at %i milliHz.\n", width, height, milliHz);
                }

                wl_shell_surface_set_fullscreen(wayland_window->wl_shell_surface, WL_SHELL_SURFACE_FULLSCREEN_METHOD_DRIVER, milliHz, displayWaylandOutputs[screenSettings->screenNumber]);
            }
            else {
                // No video refresh rate switch requested:
                wl_shell_surface_set_fullscreen(wayland_window->wl_shell_surface, WL_SHELL_SURFACE_FULLSCREEN_METHOD_DRIVER, 0, displayWaylandOutputs[screenSettings->screenNumber]);
            }
        }
        else {
            // A windowed window aka non-fullscreen, or a transparent fullscreen window.
            if (windowRecord->specialflags & kPsychGUIWindow) {
                // A GUI window. Nothing to do yet.
            }

            // Show it as toplevel window:
            wl_shell_surface_set_toplevel(wayland_window->wl_shell_surface);
        }

        // Give the window a title:
        wl_shell_surface_set_title(wayland_window->wl_shell_surface, windowTitle);

        // Make it so!
        wl_display_roundtrip(wl_display);
    }

    // If windowLevel is zero, lower it to the bottom of the stack of windows:
    if (windowLevel == 0) {
        // TODO Wayland
    }

    if (!(windowRecord->specialflags & kPsychGUIWindowWMPositioned)) {
        // TODO Wayland
        // XMoveWindow(dpy, win, x, y);
    }

    // Ok, the onscreen window is ready on the screen. Time for OpenGL setup...

    // Activate the associated rendering context:
    waffle_make_current(waffle_display, window, ctx);

    // Running on top of a FOSS Mesa graphics driver?
    if ((open_windowcount == 0) && strstr((const char*) glGetString(GL_VERSION), "Mesa") && !getenv("PSYCH_DONT_LOCK_MOGLCORE")) {
        // Yes. At least as of Mesa 10.1 as shipped in Ubuntu 14.04-LTS, Mesa
        // will become seriously crashy if our Screen() mex files is flushed
        // from memory due to a clear all/mex/Screen and afterwards reloaded.
        // This because Mesa maintains pointers back into our library image,
        // which will turn into dangling pointers if we get unloaded/reloaded
        // into a new location. To prevent Mesa crashes on clear Screen -> reload,
        // prevent this mex file against clearing from Octave/Matlab address space.
        // An ugly solution which renders "clear Screen" useless, but the best i can
        // come up with at the moment :(
        if (PsychRuntimeEvaluateString("moglcore('LockModule');") > 0) {
            if (PsychPrefStateGet_Verbosity() > 1) {
                printf("PTB-WARNING: Failed to enable moglcore locking workaround for Mesa OpenGL bug. Trying alternative workaround.\n");
                printf("PTB-WARNING: Calling 'clear all', 'clear mex', 'clear java', 'clear moglcore' is now unsafe and may crash if you try.\n");
                printf("PTB-WARNING: You may add setenv('PSYCH_DONT_LOCK_MOGLCORE','1'); to your Octave/Matlab startup script to work around this issue in future sessions.\n");
            }
            setenv("PSYCH_DONT_LOCK_MOGLCORE", "1", 0);
        }
        else {
            if (PsychPrefStateGet_Verbosity() > 2) printf("PTB-INFO: Workaround: Disabled ability to 'clear moglcore', as a workaround for a Mesa OpenGL bug. Sorry for the inconvenience.\n");
        }
    }

    // Ok, the OpenGL rendering context is up and running. Auto-detect and bind all
    // available OpenGL extensions via GLEW. Must be careful to only call GLX independent
    // init code as we are not using the X11/GLX backend, so call glewContextInit() instead
    // of the regular glewInit():
    glerr = glewContextInit();
    if (GLEW_OK != glerr) {
        /* Problem: glewContextInit failed, something is seriously wrong. */
        if (PsychPrefStateGet_Verbosity() > 0) printf("\nPTB-ERROR[GLEW init failed: %s]: Please report this to the forum. Will try to continue, but may crash soon!\n\n", glewGetErrorString(glerr));
    } else {
        if (PsychPrefStateGet_Verbosity() > 3) printf("PTB-INFO: Using GLEW version %s for automatic detection of OpenGL extensions...\n", glewGetString(GLEW_VERSION));
    }

    fflush(NULL);

    // Increase our own open window counter:
    open_windowcount++;

    // Disable screensavers:
    if (open_windowcount == 1) {
        // TODO Wayland
    }

    if (!egl_display && (PsychPrefStateGet_Verbosity() > 1)) {
        printf("PTB-WARNING: The Wayland display backend does not allow to control synchronization of bufferswap to vertical retrace.\n");
    }

    PsychUnlockDisplay();

    // Ok, we should be ready for OS independent setup...
    fflush(NULL);

    // First opened onscreen window? If so, we try to map GPU MMIO registers
    // to enable beamposition based timestamping and other special goodies:
    if (open_windowcount == 1) PsychScreenMapRadeonCntlMemory();

    PsychLockDisplay();

    // Retrieve modeline of current video mode on primary crtc for the screen to which
    // this onscreen window is assigned. Could also query useful info about crtc, but let's not
    // overdo it in the first iteration...
    XRRCrtcInfo *crtc_info = NULL;
    XRRModeInfo *mode = PsychOSGetModeLine(screenSettings->screenNumber, 0, &crtc_info);
    if (mode) {
        // Assign modes display height aka vactive or vdisplay as startline of vblank interval:
        windowRecord->VBL_Startline = mode->height;

        // Assign vbl endline as vtotal - 1:
        // TODO: Don't know how to get this on Wayland, but not critical, as it would
        // only be needed for the beamposition timestamping fallback, which itself
        // should usually not be needed due to present_feedback extension:
        // windowRecord->VBL_Endline = mode->vTotal - 1;

        // Check for output display rotation enabled. Will likely impair timing/timestamping
        // because it uses copy-swaps via an intermediate shadow framebuffer to do rotation
        // during copy-swap blit, instead of via rotated crtc scanout, as most crtc's don't
        // support this in hardware:
        // TODO: Wayland is more clever here, but need to investigate how this maps to
        // our standard method of solving such issues via imaging pipeline. Leave warning
        // of for the moment.
        //if ((crtc_info->rotation != RR_Rotate_0) && (PsychPrefStateGet_Verbosity() > 1)) {
        //    printf("PTB-WARNING: Your primary output display has hardware rotation enabled. It is not displaying in upright orientation.\n");
        //    printf("PTB-WARNING: On many graphics cards, this will cause unreliable stimulus presentation timing and timestamping.\n");
        //    printf("PTB-WARNING: If you want non-upright stimulus presentation, look at 'help PsychImaging' on how to achieve this in\n");
        //    printf("PTB-WARNING: a way that doesn't impair timing. The subfunctions 'FlipHorizontal' and 'FlipVertical' are what you probably need.\n");
        //}

        XRRFreeCrtcInfo(crtc_info);
    }

    PsychUnlockDisplay();

    // Display unlocked from here on...

    // OpenGL-ES bindings needed?
    if (PsychIsGLES(windowRecord)) {
        // Yes: Allow userspace to force rebinding:
        psych_bool forceRebind = (getenv("PSYCH_FORCE_REBINDGLES")) ? TRUE : FALSE;

        // Which version of OpenGL-ES? Set proper selector:
        int32_t apidl = 0;

        if (windowRecord->glApiType < 20) {
            apidl = WAFFLE_DL_OPENGL_ES1;
        }
        else if (windowRecord->glApiType < 30) {
            apidl = WAFFLE_DL_OPENGL_ES2;
        }
        else if (windowRecord->glApiType < 40) {
            apidl = WAFFLE_DL_OPENGL_ES3;
        }
        else printf("PTB-WARNING: Unsupported OpenGL-ES API version >= 4.0! Update the code, this may end badly!!\n");

        // Some diagnostics for forceRebind mode:
        if (forceRebind) printf("PTB-DEBUG: Forced rebinding of OpenGL-ES extensions requested.\n");
        if (PsychPrefStateGet_Verbosity() > 4) printf("PTB-DEBUG: Pre-GLES-Rebind: glBindFramebufferEXT = %p , glBindFramebuffer = %p \n",
                                                      (void*) glBindFramebufferEXT, (void*) waffle_dl_sym(WAFFLE_DL_OPENGL_ES2, "glBindFramebuffer"));

        // Try to rebind OpenGL-ES specific entry points to standard desktop OpenGL entry
        // points of compatible syntax and semantics, so we don't need to rewrite lots of
        // desktop GL support code just to account for syntactic sugar.

        // Needed in imaging pipeline: ES implementations don't expose the ARB suffixed version anymore,
        // as glActiveTexture() is part of core-spec:
        if (NULL == glActiveTextureARB || forceRebind) glActiveTextureARB = waffle_dl_sym(apidl, "glActiveTexture");

        // Fun with NVidia ES implementation, which defines glOrthof() but not glOrthofOES() on their
        // desktop drivers for GeForce and Quadro and their Tegra drivers for embedded.
        // It is crucial to force rebind here, especially on embedded, as the entry point might
        // be already bound to a dead zombie desktop GL implementation lib:
        glOrthofOES = waffle_dl_sym(WAFFLE_DL_OPENGL_ES1, "glOrthofOES");
        if (NULL == glOrthofOES) glOrthofOES = waffle_dl_sym(WAFFLE_DL_OPENGL_ES1, "glOrthof");
        if (NULL == glOrthofOES) {
            printf("PTB-ERROR: NO glOrthofOES() or glOrthof() available under OpenGL-ES api! This will not work with OpenGL-ES! Aborting.\n");
            return(FALSE);
        }

        // Bind glBlitFrameBuffer() if provided by extension or by core OpenGL-ES3:
        if (strstr(glGetString(GL_EXTENSIONS), "GL_NV_framebuffer_blit") || (windowRecord->glApiType >= 30)) {
            glBlitFramebufferEXT = waffle_dl_sym(apidl, "glBlitFramebufferNV");
            if (NULL == glBlitFramebufferEXT) glBlitFramebufferEXT = waffle_dl_sym(WAFFLE_DL_OPENGL_ES3, "glBlitFramebuffer");
            if (NULL == glBlitFramebufferEXT) printf("PTB-ERROR: Could not bind glBlitFramebuffer() function as expected! This may end in a crash!\n");
        }

        // Framebuffer objects supported? As extension on ES-1, core on ES-2+
        if (strstr(glGetString(GL_EXTENSIONS), "framebuffer_object") || (windowRecord->glApiType >= 20)) {
            // FBO extension or core code binding:
            // Some of this is a mess, as apparently each ES implementation vendor does its own non-standard thing. In theory it should be
            // enough to query for the "OES" suffixed entry points on the api as defined by apidl. In practice, three different
            // implementations (MESA, NVidia binary desktop GL drivers, NVidia binary Tegra drivers) required three different types
            // of hacks to get it working. We try the following strategy:

            // First try to bind ES-1 standard compliant: OES suffix and apidl as selected. Then retry without OES suffix with higher
            // apidl versions:
            if (apidl == WAFFLE_DL_OPENGL_ES1) {
                // OpenGL-ES 1: Framebuffer objects are only available as extension, so entry points should be OES-suffixed. Try with OES suffix:
                if (NULL == glBindFramebufferEXT || forceRebind) glBindFramebufferEXT = waffle_dl_sym(apidl, "glBindFramebufferOES");
                if (NULL == glDeleteFramebuffersEXT || forceRebind) glDeleteFramebuffersEXT = waffle_dl_sym(apidl, "glDeleteFramebuffersOES");
                if (NULL == glGenFramebuffersEXT || forceRebind) glGenFramebuffersEXT = waffle_dl_sym(apidl, "glGenFramebuffersOES");
                if (NULL == glIsFramebufferEXT || forceRebind) glIsFramebufferEXT = waffle_dl_sym(apidl, "glIsFramebufferOES");
                if (NULL == glCheckFramebufferStatusEXT || forceRebind) glCheckFramebufferStatusEXT = waffle_dl_sym(apidl, "glCheckFramebufferStatusOES");

                if (NULL == glFramebufferTexture2DEXT || forceRebind) glFramebufferTexture2DEXT = waffle_dl_sym(apidl, "glFramebufferTexture2DOES");
                if (NULL == glFramebufferRenderbufferEXT || forceRebind) glFramebufferRenderbufferEXT = waffle_dl_sym(apidl, "glFramebufferRenderbufferOES");
                if (NULL == glGetFramebufferAttachmentParameterivEXT || forceRebind) glGetFramebufferAttachmentParameterivEXT = waffle_dl_sym(apidl, "glGetFramebufferAttachmentParameterivOES");
                if (NULL == glGenerateMipmapEXT || forceRebind) glGenerateMipmapEXT = waffle_dl_sym(apidl, "glGenerateMipmapOES");

                if (NULL == glIsRenderbufferEXT || forceRebind) glIsRenderbufferEXT = waffle_dl_sym(apidl, "glIsRenderbufferOES");
                if (NULL == glBindRenderbufferEXT || forceRebind) glBindRenderbufferEXT = waffle_dl_sym(apidl, "glBindRenderbufferOES");
                if (NULL == glDeleteRenderbuffersEXT || forceRebind) glDeleteRenderbuffersEXT = waffle_dl_sym(apidl, "glDeleteRenderbuffersOES");
                if (NULL == glGenRenderbuffersEXT || forceRebind) glGenRenderbuffersEXT = waffle_dl_sym(apidl, "glGenRenderbuffersOES");
                if (NULL == glRenderbufferStorageEXT || forceRebind) glRenderbufferStorageEXT = waffle_dl_sym(apidl, "glRenderbufferStorageOES");
                if (NULL == glGetRenderbufferParameterivEXT || forceRebind) glGetRenderbufferParameterivEXT = waffle_dl_sym(apidl, "glGetRenderbufferParameterivOES");
            }

            // Try binding without OES suffix: We do this for OpenGL-ES2 and later, as FBO's are part of the core,
            // and on OpenGL-ES1 if the "OES" binding failed, e.g., on the non standard compliant NVidia Tegra binary drivers:
            if ((apidl != WAFFLE_DL_OPENGL_ES1) || (NULL == glBindFramebufferEXT)) {
                // NVidia weirdness?
                if (apidl == WAFFLE_DL_OPENGL_ES1) {
                    // Yes. Their Tegra OpenGL-ES1 library internally loads their OpenGL-ES2 library behind our back
                    // and then uses their ES-2 functionality to emulate ES-1 behaviour, ie., their ES-1 lib is a wrapper
                    // around ES-2 libs, using self-defined shaders etc. to replicate fixed-pipeline behaviour. Unfortunately
                    // their emulation is not perfectly transparent, as their ES-1 libs claim FBO support, but get FBO entry
                    // points from ES-2 libs, thereby their entry points don't have the ES-1 compliant OES suffixes and our
                    // binding procedure for ES-1 above fails miserably. What to do? We query their ES-2 implementation/libs
                    // directly and bind ES2 entry points and hope this works and doesn't die a miserable death:
                    apidl = WAFFLE_DL_OPENGL_ES2;

                    // Of course they may pull the same trick with ES-2 vs. ES-3 some time, so if their ES-2 libs don't
                    // expose the most basic FBO entry point, we try with ES3:
                    if (NULL == waffle_dl_sym(apidl, "glBindFramebuffer")) apidl = WAFFLE_DL_OPENGL_ES3;
                }

                if (NULL == glBindFramebufferEXT || forceRebind) glBindFramebufferEXT = waffle_dl_sym(apidl, "glBindFramebuffer");
                if (NULL == glDeleteFramebuffersEXT || forceRebind) glDeleteFramebuffersEXT = waffle_dl_sym(apidl, "glDeleteFramebuffers");
                if (NULL == glGenFramebuffersEXT || forceRebind) glGenFramebuffersEXT = waffle_dl_sym(apidl, "glGenFramebuffers");
                if (NULL == glIsFramebufferEXT || forceRebind) glIsFramebufferEXT = waffle_dl_sym(apidl, "glIsFramebuffer");
                if (NULL == glCheckFramebufferStatusEXT || forceRebind) glCheckFramebufferStatusEXT = waffle_dl_sym(apidl, "glCheckFramebufferStatus");

                if (NULL == glFramebufferTexture2DEXT || forceRebind) glFramebufferTexture2DEXT = waffle_dl_sym(apidl, "glFramebufferTexture2D");
                if (NULL == glFramebufferRenderbufferEXT || forceRebind) glFramebufferRenderbufferEXT = waffle_dl_sym(apidl, "glFramebufferRenderbuffer");
                if (NULL == glGetFramebufferAttachmentParameterivEXT || forceRebind) glGetFramebufferAttachmentParameterivEXT = waffle_dl_sym(apidl, "glGetFramebufferAttachmentParameteriv");
                if (NULL == glGenerateMipmapEXT || forceRebind) glGenerateMipmapEXT = waffle_dl_sym(apidl, "glGenerateMipmap");

                if (NULL == glIsRenderbufferEXT || forceRebind) glIsRenderbufferEXT = waffle_dl_sym(apidl, "glIsRenderbuffer");
                if (NULL == glBindRenderbufferEXT || forceRebind) glBindRenderbufferEXT = waffle_dl_sym(apidl, "glBindRenderbuffer");
                if (NULL == glDeleteRenderbuffersEXT || forceRebind) glDeleteRenderbuffersEXT = waffle_dl_sym(apidl, "glDeleteRenderbuffers");
                if (NULL == glGenRenderbuffersEXT || forceRebind) glGenRenderbuffersEXT = waffle_dl_sym(apidl, "glGenRenderbuffers");
                if (NULL == glRenderbufferStorageEXT || forceRebind) glRenderbufferStorageEXT = waffle_dl_sym(apidl, "glRenderbufferStorage");
                if (NULL == glGetRenderbufferParameterivEXT || forceRebind) glGetRenderbufferParameterivEXT = waffle_dl_sym(apidl, "glGetRenderbufferParameteriv");
            }
        } // End of FBO binding.
    } // End of OpenGL-ES binding.

    // Release the waffle_native_window:
    free(wafflewin);
    wafflewin = NULL;

    return(TRUE);
}

void PsychOSCloseWindow(PsychWindowRecordType * windowRecord)
{
    Display *dpy = windowRecord->targetSpecific.privDpy;

    PsychLockDisplay();

    // Detach OpenGL rendering context again - just to be safe!
    waffle_make_current(windowRecord->targetSpecific.deviceContext, NULL, NULL);
    currentContext = NULL;

    // Delete rendering context:
    waffle_context_destroy(windowRecord->targetSpecific.contextObject);
    windowRecord->targetSpecific.contextObject = NULL;

    // Delete swap context:
    waffle_context_destroy(windowRecord->targetSpecific.glswapcontextObject);
    windowRecord->targetSpecific.glswapcontextObject = NULL;

    // Delete userspace context, if any:
    if (windowRecord->targetSpecific.glusercontextObject) {
        waffle_context_destroy(windowRecord->targetSpecific.glusercontextObject);
        windowRecord->targetSpecific.glusercontextObject = NULL;
    }

    // Close & Destroy the window:
    waffle_window_destroy(windowRecord->targetSpecific.windowHandle);
    windowRecord->targetSpecific.windowHandle = 0;
    windowRecord->targetSpecific.xwindowHandle = 0;

    PsychUnlockDisplay();

    // Make it so!
    wl_display_roundtrip(wl_display);

    while (!wl_list_empty(&windowRecord->targetSpecific.presentation_feedback_list)) {
        struct wayland_feedback *f;

        f = wl_container_of(windowRecord->targetSpecific.presentation_feedback_list.next, f, link);
        if (PsychPrefStateGet_Verbosity() > 3) printf("PTB-DEBUG: Cleaning up Wayland feedback event %llu.\n", f->sbc);
        destroy_wayland_feedback(f);
    }

    // Decrement global count of open onscreen windows:
    open_windowcount--;

    // Was this the last window?
    if (open_windowcount <= 0) {
        open_windowcount = 0;

        PsychLockDisplay();

        // (Re-)enable X-Windows screensavers if they were enabled before opening windows:
        // Set screensaver to previous settings, potentially enabling it:
        // TODO Wayland: XSetScreenSaver(dpy, -1, 0, DefaultBlanking, DefaultExposures);

        PsychUnlockDisplay();

        // Unmap/release possibly mapped device memory: Defined in PsychScreenGlue.c
        PsychScreenUnmapDeviceMemory();
    }

    windowRecord->targetSpecific.deviceContext = NULL;

    // Done.
    return;
}

/*
    PsychOSGetVBLTimeAndCount()

    Returns absolute system time of last VBL and current total count of VBL interrupts since
    startup of gfx-system for the given screen. Returns a time of -1 and a count of 0 if this
    feature is unavailable on the given OS/Hardware configuration.
*/
double PsychOSGetVBLTimeAndCount(PsychWindowRecordType *windowRecord, psych_uint64* vblCount)
{
    // Unsupported:
    *vblCount = 0;
    return(-1);
}


/* PsychOSGetSwapCompletionTimestamp()
 *
 * Retrieve a very precise timestamp of doublebuffer swap completion by means
 * of OS specific facilities. This function is optional. If the underlying
 * OS/driver/GPU combo doesn't support a high-precision, high-reliability method
 * to query such timestamps, the function should return -1 as a signal that it
 * is unsupported or (temporarily) unavailable. Higher level timestamping code
 * should use/prefer timestamps returned by this function over other timestamps
 * provided by other mechanisms if possible. Calling code must be prepared to
 * use alternate timestamping methods if this method fails or returns a -1
 * unsupported error. Calling code must expect this function to block until
 * swap completion.
 *
 * Input argument targetSBC: Swapbuffers count for which to wait for. A value
 * of zero means to block until all pending bufferswaps for windowRecord have
 * completed, then return the timestamp of the most recently completed swap.
 *
 * A value of zero is recommended.
 *
 * Returns: Precise and reliable swap completion timestamp in seconds of
 * system time in variable referenced by 'tSwap', and msc value of completed swap,
 * or a negative value on error (-1 == unsupported, -2/-3 == Query failed).
 *
 */
psych_int64 PsychOSGetSwapCompletionTimestamp(PsychWindowRecordType *windowRecord, psych_int64 targetSBC, double* tSwap)
{
    psych_int64 ust = 0, sbc = 0, msc = -1;

    // Extension unsupported or known to be defective? Return -1 "unsupported" in that case:
    if (windowRecord->specialflags & kPsychOpenMLDefective) return(-1);

    // Translate targetSBC 0 aka sbc of last submitted swap into proper value:
    if (targetSBC == 0) {
        targetSBC = windowRecord->target_sbc;
        if (PsychPrefStateGet_Verbosity() > 11) printf("PTB-DEBUG:PsychOSGetSwapCompletionTimestamp: Supported. Calling with overriden targetSBC = %lld.\n", targetSBC);
    }
    else if (PsychPrefStateGet_Verbosity() > 11) printf("PTB-DEBUG:PsychOSGetSwapCompletionTimestamp: Supported. Calling with targetSBC = %lld.\n", targetSBC);

    if (!wl_display) return(-1);

    // Perform Wayland event dispatch cycles until targetSBC is reached:
    while (windowRecord->reference_sbc < targetSBC) {
        wl_display_dispatch(wl_display);
    }

    sbc = windowRecord->reference_sbc;

    // Was this a discarded swap?
    if (windowRecord->swapcompletiontype == -1) {
        // Yes! Can't do anything meaningful with this one atm. We can output a warning if this happened
        // on a vsynced window, and make up some values and fallback to standard code and hope for the
        // best:
        if ((windowRecord->vSynced) && (PsychPrefStateGet_Verbosity() > 1)) {
            // This should probably not happen
            printf("PTB-WARNING:PsychOSGetSwapCompletionTimestamp: Swaprequest with sbc = %lld was discarded by compositor for vsynced flip!\n", sbc);
        }
        else if (PsychPrefStateGet_Verbosity() > 4) {
            // How peculiar. This may happen, so just output some note at debug verbosity:
            printf("PTB-DEBUG:PsychOSGetSwapCompletionTimestamp: Swaprequest with sbc = %lld was discarded by compositor for non-vsynced flip.\n", sbc);
        }

        return(-1);
    }

    msc = windowRecord->reference_msc;
    ust = windowRecord->reference_ust;

    // Check for valid return values:
    if ((windowRecord->vSynced) && (ust == 0)) {
        if (PsychPrefStateGet_Verbosity() > 1) {
            printf("PTB-DEBUG:PsychOSGetSwapCompletionTimestamp: Invalid return values ust = %lld, msc = %lld from call with success return code (sbc = %lld)! Failing with rc = -2.\n", ust, msc, sbc);
            printf("PTB-DEBUG:PsychOSGetSwapCompletionTimestamp: This likely means a driver bug or malfunction, or that timestamping support has been disabled by the user in the driver!\n");
        }

        // Return with "failure" rc, so calling code can provide more error handling:
        return(-2);
    }

    // If no actual timestamp / msc was requested, then we return here. This was needed by the
    // workaround code for multi-threaded XLib access. It passes NULL to just (ab)use this
    // function to wait for swap completion, before it touches the framebuffer for real.
    // See function PsychLockedTouchFramebufferIfNeeded() in PsychWindowSupport.c
    if (tSwap == NULL) return(msc);

    // Success at least for timestamping. Translate ust into system time in seconds:
    *tSwap = PsychOSMonotonicToRefTime(((double) ust) / PsychGetKernelTimebaseFrequencyHz());

    if (PsychPrefStateGet_Verbosity() > 11) printf("PTB-DEBUG:PsychOSGetSwapCompletionTimestamp: Success! refust = %lld, refmsc = %lld, refsbc = %lld.\n", ust, msc, sbc);

    // This would drive internal swap completion logging if it would be enabled for internal use.
    // Atm. we don't enable swap completion logging for internal use, as there isn't any need due
    // to the different completion handling of Wayland as compared to X11/GLX. We leave this code
    // as reference for potential future use:
    if (PsychOSSwapCompletionLogging(windowRecord, 4, (int) sbc)) {
        // Internal logging worked and processed an event. Nothing to do atm.
    }

    // Perform validation of the quality, trustworthiness and precision of stimulus onset if
    // sync tests are not completely disabled and vsynced stimulus onset with precise timing
    // is requested, iow. if the usercode signals it cares about visual and timing correctness:
    if ((PsychPrefStateGet_SkipSyncTests() < 2) && (windowRecord->vSynced)) {
        // If usercode wants vsynced tear-free swap, we better got one, otherwise we consider this a warning condition:
        if (!(windowRecord->swapcompletiontype & PRESENTATION_FEEDBACK_KIND_VSYNC) && (PsychPrefStateGet_Verbosity() > 0)) {
            printf("PTB-ERROR: Flip for window %i was not vblank synchronized/tear-free as requested. Stimulus is likely visually corrupted and visual presentation timing is likely wrong!\n",
                   windowRecord->windowIndex);
        }

        // If we don't get trustworthy presentation completion events from kernel/hardware, then our timestamps, and ergo also
        // our presentation timing based on those timestamps as a baseline, will be untrustworthy and meaningless guesses - unuseable
        // for research grade timed visual stimulation:
        if (!(windowRecord->swapcompletiontype & PRESENTATION_FEEDBACK_KIND_HW_COMPLETION) && (PsychPrefStateGet_Verbosity() > 0)) {
            printf("PTB-ERROR: Flip for window %i didn't use reliable stimulus onset timestamping. Visual presentation timing and timestamps are likely wrong!\n",
                   windowRecord->windowIndex);
        }

        // If we got a vsynced presentation with hw completion, iow. reliable and robust, was then a high
        // precision/accuracy kernel/display driver/hardware clock used for stimulus onset timestamping?
        if (!(windowRecord->swapcompletiontype & PRESENTATION_FEEDBACK_KIND_HW_CLOCK) &&
            (windowRecord->swapcompletiontype & PRESENTATION_FEEDBACK_KIND_HW_COMPLETION) &&
            (windowRecord->swapcompletiontype & PRESENTATION_FEEDBACK_KIND_VSYNC)) {
            // No. Timestamps will be noisy. What to do about this? On most backends we can't do anything but
            // inform the user about the lower quality of timestamps. On some backends we can fallback to our
            // own homemade high precision beamposition timestamping, given that should work whenever at least vsync and
            // hardware completion events work. On such backends we can just trigger the fallback here, now that we are
            // certain the present completed:
            if (windowRecord->VBL_Endline > 0) {
                // Have beamposition timestamping fallback for this setup. Use it:
                if (PsychPrefStateGet_Verbosity() > 5)
                    printf("PTB-INFO: Display backend doesn't support precise stimulus onset timestamps. Falling back to our own high precision beamposition timestamping.\n");

                // Trigger silent fallback in calling code by returning the -1 = "unsupported" status code:
                return(-1);
            }
            else {
                // No higher precision fallback available than what we already got from Wayland. The returned timestamps
                // are not actually wrong or untrustworthy, they are just less accurate than what we would wish for demanding
                // neuroscience research paradigms, so this is not really a error or warning condition, but just something
                // the user should be made aware of, in case high precision is needed. Let's output a one-time notice at
                // normal levels of verbosity, and then ongoing notice at high debug levels for diagnostic purpose:
                if (!(windowRecord->specialflags & kPsychClockPrecisionOneTimeWarningDone) &&
                    (PsychPrefStateGet_Verbosity() > 1)) {
                    windowRecord->specialflags |= kPsychClockPrecisionOneTimeWarningDone;
                    printf("PTB-WARNING: This display backend doesn't support precise visual stimulus onset timestamps. Timestamps will be correct, but somewhat noisy and inaccurate!\n");
                }
                else if (PsychPrefStateGet_Verbosity() > 5) {
                    printf("PTB-DEBUG: The display backend doesn't support precise stimulus onset timestamps and no fallback available. Timestamps will be correct, but noisy.\n");
                }
            }
        }

        // Give additional warning and setup tips if our minimum requirement of vsync and hw completion isn't
        // fullfilled. Everything else is not catastrophic failure.
        if (!(windowRecord->swapcompletiontype & PRESENTATION_FEEDBACK_KIND_VSYNC) ||
            !(windowRecord->swapcompletiontype & PRESENTATION_FEEDBACK_KIND_HW_COMPLETION)) {
            if (PsychPrefStateGet_Verbosity() > 2) {
                printf("PTB-WARNING: Something is misconfigured or suboptimal on your system, otherwise Wayland would have provided tear-free and precise\n");
                printf("PTB-WARNING: visual stimulus onset timing and timestamping! Read 'help WaylandSetup' for troubleshooting tips for this problem.\n");
            }
        }
    }

    // If this is supposed to be a decorationless, unoccluded, fully opaque, topmost fullscreen window, then it should
    // be presented zero-copy without any interference by compositing. Important for stimuli which need pixel-perfect
    // display wrt. location, intensity and color for evey single pixel, and for driving special high precision neuroscience
    // display hardware. Make sure we get zero copy in this case, as everything else almost certainly means trouble.
    // Having zero-copy doesn't mean no trouble though, as we could end up in a hardware overlay with other hardware
    // overlay or primary plane visual content partially occluding us or at least partially occupying the display. Or
    // we could end up in a hardware overlay to the same effect with the primary plane or other overlays partially showing
    // or occluding us. TODO: Find a way to control for those things.
    if ((windowRecord->specialflags & kPsychIsFullscreenWindow) && (PsychPrefStateGet_WindowShieldingLevel() >= 2000) &&
        !(windowRecord->swapcompletiontype & PRESENTATION_FEEDBACK_KIND_ZERO_COPY) && (PsychPrefStateGet_Verbosity() > 1)) {
        printf("PTB-WARNING: Flip for window %i didn't use zero copy pageflips. Stimulus may not display onscreen pixel-perfect and exactly as specified by you!\n",
               windowRecord->windowIndex);
    }

    // Return swap completion msc:
    return(msc);
}

/* PsychOSInitializeOpenML() - Linux specific function.
 *
 * Performs basic initialization of the OpenML OML_sync_control extension.
 * Performs basic and extended correctness tests and disables extension if it
 * is unreliable, or enables workarounds for partially broken extensions.
 *
 * Called from PsychDetectAndAssignGfxCapabilities() as part of the PsychOpenOnscreenWindow()
 * procedure for a window with OpenML support.
 *
 */
void PsychOSInitializeOpenML(PsychWindowRecordType *windowRecord)
{
    // Disable clever swap scheduling for now:
    windowRecord->gfxcaps &= ~kPsychGfxCapSupportsOpenML;

    // Timestamping in PsychOSGetSwapCompletionTimestamp() and PsychOSGetVBLTimeAndCount() disabled:
    windowRecord->specialflags |= kPsychOpenMLDefective;

    // Always init the list for wayland present events:
    wl_list_init(&windowRecord->targetSpecific.presentation_feedback_list);

    // Try to get a handle to the Wayland presentation_interface: non-NULL == Success.
    if (!get_wayland_presentation_extension(windowRecord)) {
        if (PsychPrefStateGet_Verbosity() > 2) printf("PTB-INFO: No Wayland presentation_feedback extension. Using naive standard implementation.\n");

        // Timestamping in PsychOSGetSwapCompletionTimestamp() and PsychOSGetVBLTimeAndCount() disabled:
        windowRecord->specialflags |= kPsychOpenMLDefective;
        return;
    }

    // Enable use of Wayland presentation_feedback extension for swap completion timestamping:
    windowRecord->specialflags &= ~kPsychOpenMLDefective;

    // Ready to rock on Wayland:
    if (PsychPrefStateGet_Verbosity() > 2) printf("PTB-INFO: Enabling Wayland presentation_feedback extension for swap completion timestamping.\n");

    return;
}

/*
    PsychOSScheduleFlipWindowBuffers()

    Schedules a double buffer swap operation for given window at a given
    specific target time or target refresh count in a specified way.

    This uses OS specific API's and algorithms to schedule the asynchronous
    swap. This function is optional, target platforms are free to not implement
    it but simply return a "not supported" status code.

    Arguments:

    windowRecord - The window to be swapped.
    tWhen        - Requested target system time for swap. Swap shall happen at first
                    VSync >= tWhen.
    targetMSC    - If non-zero, specifies target msc count for swap. Overrides tWhen.
    divisor, remainder - If set to non-zero, msc at swap must satisfy (msc % divisor) == remainder.
    specialFlags - Additional options, a bit field consisting of single bits that can be or'ed together:
                    1 = Constrain swaps to even msc values, 2 = Constrain swaps to odd msc values. (Used for frame-seq. stereo field selection)

    Return value:

    Value greater than or equal to zero on success: The target msc for which swap is scheduled.
    Negative value: Error. Function failed. -1 == Function unsupported on current system configuration.
    -2 ... -x == Error condition.

*/
psych_int64 PsychOSScheduleFlipWindowBuffers(PsychWindowRecordType *windowRecord, double tWhen, psych_int64 targetMSC, psych_int64 divisor, psych_int64 remainder, unsigned int specialFlags)
{
    // Unsupported:
    return(-1);
}

/*
  PsychOSFlipWindowBuffers()

  Performs OS specific double buffer swap call.
*/
void PsychOSFlipWindowBuffers(PsychWindowRecordType *windowRecord)
{
    // If wayland present_feedback is supposed to be used for swap completion timestamping, then we
    // need to setup a proper present_feedback event for the upcoming swap:
    if (!(windowRecord->specialflags & kPsychOpenMLDefective)) {
        wayland_window_create_feedback(windowRecord);
    }

    // Execute OS neutral bufferswap code first:
    PsychExecuteBufferSwapPrefix(windowRecord);

    // Trigger the "Front <-> Back buffer swap (flip) (on next vertical retrace)":
    // This must be lock-protected for use with X11/XLib.
    PsychLockDisplay();
    waffle_window_swap_buffers(windowRecord->targetSpecific.windowHandle);
    windowRecord->target_sbc = windowRecord->submitted_sbc;
    PsychUnlockDisplay();

    return;
}

/* Enable/disable syncing of buffer-swaps to vertical retrace. */
void PsychOSSetVBLSyncLevel(PsychWindowRecordType *windowRecord, int swapInterval)
{
    // Enable rendering context of window (no-ops internally when not called from master-thread):
    // TODO: Check if needed on EGL?
    PsychSetGLContext(windowRecord);

    // Store new setting also in internal helper variable, e.g., to allow workarounds to work:
    windowRecord->vSynced = (swapInterval > 0) ? TRUE : FALSE;

    // EGL-based backend in use:
    if (egl_display) {
        #ifdef PTB_USE_EGL
        PsychLockDisplay();
        if (!eglSwapInterval(egl_display, (EGLint) swapInterval)) {
            if (PsychPrefStateGet_Verbosity() > 1) printf("\nPTB-WARNING: FAILED to %s synchronization to vertical retrace!\n\n", (swapInterval > 0) ? "enable" : "disable");
        }
        PsychUnlockDisplay();
        #endif
    }

    return;
}

/*
  PsychOSSetGLContext()

  Set the window to which GL drawing commands are sent.  
*/
void PsychOSSetGLContext(PsychWindowRecordType * windowRecord)
{
    // Only change context if not already proper context bound:
    // This is very important not only for performance, but also to avoid harmful
    // glBindFrameBufferEXT(0) calls without switching away from context -- something
    // that would completely mess up imaging pipeline state!
    if (currentContext != windowRecord->targetSpecific.contextObject) {
        if (currentContext) {
            // We need to glFlush the context before switching, otherwise race-conditions may occur:
            glFlush();

            // Need to unbind any FBO's in old context before switch, otherwise bad things can happen...
            if (glBindFramebufferEXT) glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
        }

        PsychLockDisplay();

        // Switch to new context: Skip surface bind if forbidden by higher-level code, e.g., for multi-threaded EGL use.
        if (!waffle_make_current(windowRecord->targetSpecific.deviceContext,
                                 (PsychGetParentWindow(windowRecord)->specialflags & kPsychSurfacelessContexts) ? EGL_NO_SURFACE : windowRecord->targetSpecific.windowHandle,
                                 windowRecord->targetSpecific.contextObject) && (PsychPrefStateGet_Verbosity() > 0)) {
            printf("PTB-ERROR: PsychOSSetGLContext(): Failed to bind master context of window %i: [%s] - Expect trouble!\n", windowRecord->windowIndex, waffle_error_to_string(waffle_error_get_code()));
        }
        else {
            // Update context tracking:
            currentContext = windowRecord->targetSpecific.contextObject;
        }

        PsychUnlockDisplay();
    }

    return;
}

/*
  PsychOSUnsetGLContext()

  Clear the drawing context.  
*/
void PsychOSUnsetGLContext(PsychWindowRecordType * windowRecord)
{
    if (PsychIsMasterThread()) {
        if (currentContext) {
            // We need to glFlush the context before switching, otherwise race-conditions may occur:
            glFlush();

            // Need to unbind any FBO's in old context before switch, otherwise bad things can happen...
            if (glBindFramebufferEXT) glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
        }

        // Reset tracking state:
        currentContext = NULL;
    }

    PsychLockDisplay();
    waffle_make_current(windowRecord->targetSpecific.deviceContext, NULL, NULL);
    PsychUnlockDisplay();

    return;
}

/* Same as PsychOSSetGLContext() but for selecting userspace rendering context,
 * optionally copying state from PTBs context.
 */
void PsychOSSetUserGLContext(PsychWindowRecordType * windowRecord, psych_bool copyfromPTBContext)
{
    // Child protection:
    if (windowRecord->targetSpecific.glusercontextObject == NULL)
        PsychErrorExitMsg(PsychError_user, "GL Userspace context unavailable! Call InitializeMatlabOpenGL *before* Screen('OpenWindow')!");

    // Only change context if not already proper context bound:
    // This is very important not only for performance, but also to avoid harmful
    // glBindFrameBufferEXT(0) calls without switching away from context -- something
    // that would completely mess up imaging pipeline state!
    if (currentContext != windowRecord->targetSpecific.glusercontextObject) {
        PsychLockDisplay();

        // Copy from context unsupported on Waffle backends:
        if (copyfromPTBContext && (PsychPrefStateGet_Verbosity() > 1)) {
            printf("PTB-WARNING: Tried to set the 'sharecontext' flag to 2 in Screen('BeginOpenGL'), but this is not supported with Waffle display backends. Ignored!\n");
        }

        // Bind it: Skip surface bind if forbidden by higher-level code, e.g., for multi-threaded EGL use.
        if (!waffle_make_current(windowRecord->targetSpecific.deviceContext,
                                 (PsychGetParentWindow(windowRecord)->specialflags & kPsychSurfacelessContexts) ? EGL_NO_SURFACE : windowRecord->targetSpecific.windowHandle,
                                 windowRecord->targetSpecific.glusercontextObject) && (PsychPrefStateGet_Verbosity() > 0)) {
            printf("PTB-ERROR: PsychOSSetUserGLContext(): Failed to bind userspace gl-context of window %i: [%s] - Expect trouble!\n", windowRecord->windowIndex, waffle_error_to_string(waffle_error_get_code()));
        }
        else {
            // Update context tracking:
            currentContext = windowRecord->targetSpecific.glusercontextObject;
        }

        PsychUnlockDisplay();
    }

    return;
}

/* PsychOSSetupFrameLock - Check if framelock / swaplock support is available on
 * the given graphics system implementation and try to enable it for the given
 * pair of onscreen windows.
 *
 * If possible, will try to add slaveWindow to the swap group and/or swap barrier
 * of which masterWindow is already a member, putting slaveWindow into a swap-lock
 * with the masterWindow. If masterWindow isn't yet part of a swap group, create a
 * new swap group and attach masterWindow to it, before joining slaveWindow into the
 * new group. If masterWindow is part of a swap group and slaveWindow is NULL, then
 * remove masterWindow from the swap group.
 *
 * The swap lock mechanism used is operating system and GPU dependent. Many systems
 * will not support framelock/swaplock at all.
 *
 * Returns TRUE on success, FALSE on failure.
 */
psych_bool PsychOSSetupFrameLock(PsychWindowRecordType *masterWindow, PsychWindowRecordType *slaveWindow)
{
    if (PsychPrefStateGet_Verbosity() > 5) printf("PTB-DEBUG: NV_swap_group and GLX_SGIX_swap_group unsupported.\n");
    return(FALSE);
}

psych_bool PsychOSSwapCompletionLogging(PsychWindowRecordType *windowRecord, int cmd, int aux1)
{
    const char *FieldNames[] = { "OnsetTime", "OnsetVBLCount", "SwapbuffersCount", "SwapType", "BackendFeedbackString" };
    const int  fieldCount = 5;
    PsychGenericScriptType  *s;
    char flagstr[10];
    int event_type;

    // Currently only have meaningful handling for Wayland with presentation_feedback extension:
    if (!(windowRecord->specialflags & kPsychOpenMLDefective)) {
        if (cmd == 0 || cmd == 1 || cmd == 2) {
            // Check if presentation_feedback extension is supported. Enable/Disable swap completion event delivery for our window, if so:
            PsychLockDisplay();
            // Logical enable state: Usercode has precedence. If it enables it goes to it. If it disabled,
            // it gets directed to us:
            // UPDATE: Actually no. Disable by usercode means disable for now, until we have an actual
            // use case for automatic redirection to our code on Wayland. Otherwise we'd just incur extra
            // overhead for nothing.
            // Old style with redirect: if (cmd == 0 || cmd == 1) windowRecord->swapevents_enabled = (cmd == 1) ? 1 : 2;
            // New style: Enable if usercode wants it, disable if usercode doesn't want it:
            if (cmd == 0 || cmd == 1) windowRecord->swapevents_enabled = cmd;

            // If we want the data and usercode doesn't have exclusive access to it already, then redirect to us:
            if (cmd == 2 && (windowRecord->swapevents_enabled != 1)) windowRecord->swapevents_enabled = 2;

            PsychUnlockDisplay();
            return(TRUE);
        }

        if (cmd == 3 || cmd == 4) {
            // Extension enabled? Process events if so:
            if (wl_display) {
                // Perform one dispatch cycle so event list is up to date:
                wl_display_dispatch_pending(wl_display);

                // Delivery to user-code?
                if (cmd == 3 && windowRecord->swapevents_enabled == 1) {
                    // Try to fetch oldest pending one for this window:
                    PsychLockDisplay();
                    struct wayland_feedback *sce, *tmp;
                    wl_list_for_each_reverse_safe(sce, tmp, &windowRecord->targetSpecific.presentation_feedback_list, link) {
                        // Completed or Discarded swap? That would be our one oldest completed candidate for procesing:
                        if (sce->present_status > 0) {
                            // Convert presentation status flags to something human readable:
                            wayland_pflags_to_str(sce->present_flags, flagstr, sizeof(flagstr));

                            if (PsychPrefStateGet_Verbosity() > 5) {
                                if (sce->present_status == 1) {
                                    printf("SWAPEVENT[%i]: ust = %lld, msc = %lld, sbc = %lld, flags [%s].\n", windowRecord->windowIndex,
                                           sce->ust, sce->msc, sce->sbc, flagstr);
                                }
                                else {
                                    printf("SWAPEVENT[%i]: sbc = %lld Skipped/Discarded.\n", windowRecord->windowIndex, sce->sbc);
                                }
                            }

                            PsychAllocOutStructArray(aux1, FALSE, 1, fieldCount, FieldNames, &s);

                            // Discarded / Skipped present?
                            if (sce->present_status == 2) {
                                // Yes. Not much todo - Assign dummy values and Discarded status:
                                PsychSetStructArrayDoubleElement("OnsetTime", 0, 0, s);
                                PsychSetStructArrayDoubleElement("OnsetVBLCount", 0, 0, s);
                                PsychSetStructArrayDoubleElement("SwapbuffersCount", 0, (double) sce->sbc, s);
                                PsychSetStructArrayStringElement("BackendFeedbackString", 0, "", s);
                                PsychSetStructArrayStringElement("SwapType", 0, "Discarded", s);

                                // Delete event:
                                destroy_wayland_feedback(sce);

                                PsychUnlockDisplay();
                                return(TRUE);
                            }

                            // Successfully presented onscreen, go ahead...
                            PsychSetStructArrayDoubleElement("OnsetTime", 0, PsychOSMonotonicToRefTime(((double) sce->ust) / PsychGetKernelTimebaseFrequencyHz()), s);
                            PsychSetStructArrayDoubleElement("OnsetVBLCount", 0, (double) sce->msc, s);
                            PsychSetStructArrayDoubleElement("SwapbuffersCount", 0, (double) sce->sbc, s);
                            PsychSetStructArrayStringElement("BackendFeedbackString", 0, flagstr, s);

                            if (sce->present_flags == (PRESENTATION_FEEDBACK_KIND_VSYNC | PRESENTATION_FEEDBACK_KIND_HW_COMPLETION |
                                                        PRESENTATION_FEEDBACK_KIND_HW_CLOCK | PRESENTATION_FEEDBACK_KIND_ZERO_COPY)) {
                                // ~ GLX_FLIP_COMPLETE_INTEL: A zero-copy pageflip of our buffer directly
                                // to the scanout, thereby pixel-perfect identity display, as required by
                                // special high precision neuro-science display hardware, precisely controlled
                                // "pixel-perfect" stimuli etc. Stimulus onset is perfectly tear-free and
                                // timestamped with robust and precise onset timestamps. For a non-fullscreen
                                // window, this could also have happened by atomic flipping into a hardware
                                // overlay instead of the primary scanout plane. This doesn't exclude the
                                // possible display of other content on the screen - via other active hardware
                                // overlays, or on the primary plane if our content went into a hw overlay, so
                                // this is not a guarantee that our stimulus was the only thing showing on the
                                // display:
                                PsychSetStructArrayStringElement("SwapType", 0, "IdentityPageflip", s);
                            }
                            else if (sce->present_flags == (PRESENTATION_FEEDBACK_KIND_VSYNC | PRESENTATION_FEEDBACK_KIND_HW_COMPLETION |
                                                            PRESENTATION_FEEDBACK_KIND_HW_CLOCK)) {
                                // ~ GLX_FLIP_COMPLETE_INTEL, likely implemented via pageflip, with precise
                                // and robust onset timestamps - same timing guarantees as a pageflip, but
                                // our image buffer was composited into a shared framebuffer before presentation.
                                // This is like a Copyswap on X11, but with reliable timing and timestamping.
                                // While executed with precise timing, pixel color and position mapping could
                                // be a non-identity due to compositor interference, so this might be
                                // problematic for high-precision stims and special neuro-science display
                                // equipment.
                                PsychSetStructArrayStringElement("SwapType", 0, "CompositedPageflip", s);
                            }

                            else if (sce->present_flags & (PRESENTATION_FEEDBACK_KIND_VSYNC | PRESENTATION_FEEDBACK_KIND_HW_COMPLETION) ==
                                        (PRESENTATION_FEEDBACK_KIND_VSYNC | PRESENTATION_FEEDBACK_KIND_HW_COMPLETION)) {
                                // A tear-free presentation due to reliable vsync (pageflipped or vblank synced copy) and hardware
                                // completion event, but no hw clock timestamp. This has well defined stimulus onset,
                                // and also defined stimulus onset timestamping, but the timestamps can be rather noisy:
                                if (sce->present_flags & PRESENTATION_FEEDBACK_KIND_ZERO_COPY) {
                                    // Map it to a "ImprecisePageflip" - not neccessary true, but the behaviour should come
                                    // close. This is the equivalent of what we get on proprietary X11/GLX drivers like the
                                    // NVidia binary blob or AMD Catalyst, or on Apple-OSX or MS-Windows in the best case
                                    // scenario - essentially a pageflip with noisy timestamps.
                                    PsychSetStructArrayStringElement("SwapType", 0, "ImprecisePageflip", s);
                                }
                                else {
                                    // This is a bit better than a CopySwap on X11, OSX or Windows (with/without compositing),
                                    // in that at least presentation completion gets reliably signalled, although with noisy
                                    // timestamps.
                                    PsychSetStructArrayStringElement("SwapType", 0, "ImpreciseCopy", s);
                                }
                            }

                            else if (sce->present_flags > 0) {
                                // Could be a pageflip or a copy, or any combination, but has undefined onset timing
                                // or undefined/tearing/non-vsynced onset, so this is close to unuseable for most of
                                // our purposes. It is what we'd get from X11, OSX, or Windows for any CopySwap or
                                // composited presentation, so just treat it as a regular shoddy CopySwap for now:
                                PsychSetStructArrayStringElement("SwapType", 0, "Copy", s);
                            } else {
                                // No info available from compositor at all:
                                PsychSetStructArrayStringElement("SwapType", 0, "Unknown", s);
                            }

                            // Delete event:
                            destroy_wayland_feedback(sce);

                            PsychUnlockDisplay();
                            return(TRUE);
                        }
                    }

                    PsychUnlockDisplay();
                    return(FALSE);
                }

                // Delivery to internal code "us"?
                // Note: This isn't used at the moment, just left here in case we need it in the future.
                // As opposed to the old X11/GLX backend which used the INTEL_swap_event extension, the
                // present flags are directly evaluated by the PsychOSGetSwapCompletionTimestamp()
                // function for asessment if the swap happened in a reliable/trustworthy way, and to
                // trigger warnings and error-handling if something bad happened. Therefore no need for
                // internal logging here. We may use this function in the future for other logging
                // purposes though, so leave the implementation, but don't enable its use by default:
                if (cmd == 4 && windowRecord->swapevents_enabled == 2) {
                    // Get the most recent event in the queue, old ones are not interesting to us atm.:
                    event_type = 0; // Init to "undefined"

                    // Fetch until exhausted:
                    PsychLockDisplay();
                    struct wayland_feedback *sce, *tmp;
                    wl_list_for_each_reverse_safe(sce, tmp, &windowRecord->targetSpecific.presentation_feedback_list, link) {
                        // Completed swap? That would be a candidate for procesing:
                        if (sce->present_status > 0) {
                            if (PsychPrefStateGet_Verbosity() > 10) {
                                // Convert presentation status flags to something human readable:
                                wayland_pflags_to_str(sce->present_flags, flagstr, sizeof(flagstr));
                                if (sce->present_status == 1) {
                                    printf("SWAPEVENT[%i]: ust = %lld, msc = %lld, sbc = %lld, flags [%s].\n", windowRecord->windowIndex,
                                            sce->ust, sce->msc, sce->sbc, flagstr);
                                }
                                else {
                                    printf("SWAPEVENT[%i]: sbc = %lld Skipped/Discarded.\n", windowRecord->windowIndex, sce->sbc);
                                }
                            }

                            // Assign the one that matches our last 'sbc' for swap completion on our windowRecord:
                            if ((int) sce->sbc == aux1) {
                                // Target event: Assign its flags, delete it, end our fetch:
                                event_type = (sce->present_status == 1) ? (int) sce->present_flags : -1;

                                // Delete event:
                                destroy_wayland_feedback(sce);

                                break;
                            }

                            // Delete non-target event:
                            destroy_wayland_feedback(sce);
                        }
                    }
                    PsychUnlockDisplay();

                    // TODO Doesn't make much sense atm., as this is done anyway by the
                    // present_feedback handler...
                    windowRecord->swapcompletiontype = event_type;

                    return(TRUE);
                }
            }
        }
    } else {
        // Failed to enable swap events, because they're unsupported without
        // Wayland present_feedback extension support enabled:
        windowRecord->swapevents_enabled = 0;
    }

    // Invalid cmd or failed cmd:
    return(FALSE);
}

/* PsychOSAdjustForCompositorDelay()
 *
 * Compute OS and desktop compositor specific delay that needs to be subtracted from the
 * target time for a OpenGL doublebuffer swap when conventional swap scheduling is used.
 * Subtract the delay, if any, from the given targetTime and return the corrected targetTime.
 *
 */
double PsychOSAdjustForCompositorDelay(PsychWindowRecordType *windowRecord, double targetTime)
{
    double nominalIFI = PsychGetNominalFramerate(windowRecord->screenNumber);
    if (nominalIFI > 0) nominalIFI = 1.0 / nominalIFI;

    // Need to compensate for Waylands (or only Westons?) 1 frame composition lag:
    if (!(windowRecord->specialflags & kPsychOpenMLDefective)) {
        targetTime -= nominalIFI;
        if (PsychPrefStateGet_Verbosity() > 4) printf("PTB-DEBUG: Compensating for Wayland/Weston 1 frame composition lag of %f msecs.\n", nominalIFI * 1000.0);
    }

    return(targetTime);
}

/* End of PTB_USE_WAYLAND */
#endif
