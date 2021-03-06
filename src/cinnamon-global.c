/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#include "config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#include <X11/extensions/Xfixes.h>
#include <canberra.h>
#include <clutter/glx/clutter-glx.h>
#include <clutter/x11/clutter-x11.h>
#include <gdk/gdkx.h>
#include <gio/gio.h>
#include <gjs/gjs-module.h>
#include <girepository.h>
#include <meta/display.h>
#include <meta/util.h>

/* Memory report bits */
#ifdef HAVE_MALLINFO
#include <malloc.h>
#endif

#include "cinnamon-enum-types.h"
#include "cinnamon-global-private.h"
#include "cinnamon-jsapi-compat-private.h"
#include "cinnamon-marshal.h"
#include "cinnamon-perf-log.h"
#include "cinnamon-window-tracker.h"
#include "cinnamon-wm.h"
#include "st.h"

static CinnamonGlobal *the_object = NULL;

static void grab_notify (GtkWidget *widget, gboolean is_grab, gpointer user_data);
static void cinnamon_global_on_gc (GjsContext   *context,
                                CinnamonGlobal  *global);

struct _CinnamonGlobal {
  GObject parent;

  ClutterStage *stage;
  Window stage_xwindow;
  GdkWindow *stage_gdk_window;

  MetaDisplay *meta_display;
  GdkDisplay *gdk_display;
  Display *xdisplay;
  MetaScreen *meta_screen;
  GdkScreen *gdk_screen;

  CinnamonSessionType session_type;

  /* We use this window to get a notification from GTK+ when
   * a widget in our process does a GTK+ grab.  See
   * http://bugzilla.gnome.org/show_bug.cgi?id=570641
   * 
   * This window is never mapped or shown.
   */
  GtkWindow *grab_notifier;
  gboolean gtk_grab_active;

  CinnamonStageInputMode input_mode;
  XserverRegion input_region;

  GjsContext *js_context;
  MetaPlugin *plugin;
  CinnamonWM *wm;
  GSettings *settings;
  const char *datadir;
  const char *imagedir;
  const char *userdatadir;
  StFocusManager *focus_manager;

  guint work_count;
  GSList *leisure_closures;
  guint leisure_function_id;

  /* For sound notifications */
  ca_context *sound_context;

  guint32 xdnd_timestamp;

  gint64 last_gc_end_time;
};

enum {
  PROP_0,

  PROP_SESSION_TYPE,
  PROP_OVERLAY_GROUP,
  PROP_SCREEN,
  PROP_GDK_SCREEN,
  PROP_DISPLAY,
  PROP_SCREEN_WIDTH,
  PROP_SCREEN_HEIGHT,
  PROP_STAGE,
  PROP_STAGE_INPUT_MODE,
  PROP_WINDOW_GROUP,
  PROP_TOP_WINDOW_GROUP,
  PROP_BACKGROUND_ACTOR,
  PROP_WINDOW_MANAGER,
  PROP_SETTINGS,
  PROP_DATADIR,
  PROP_IMAGEDIR,
  PROP_USERDATADIR,
  PROP_FOCUS_MANAGER,
};

/* Signals */
enum
{
 XDND_POSITION_CHANGED,
 XDND_LEAVE,
 XDND_ENTER,
 NOTIFY_ERROR,
 LAST_SIGNAL
};

G_DEFINE_TYPE(CinnamonGlobal, cinnamon_global, G_TYPE_OBJECT);

static guint cinnamon_global_signals [LAST_SIGNAL] = { 0 };

static void
cinnamon_global_set_property(GObject         *object,
                          guint            prop_id,
                          const GValue    *value,
                          GParamSpec      *pspec)
{
  CinnamonGlobal *global = CINNAMON_GLOBAL (object);

  switch (prop_id)
    {
    case PROP_STAGE_INPUT_MODE:
      cinnamon_global_set_stage_input_mode (global, g_value_get_enum (value));
      break;
    case PROP_SESSION_TYPE:
      global->session_type = g_value_get_enum (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
cinnamon_global_get_property(GObject         *object,
                          guint            prop_id,
                          GValue          *value,
                          GParamSpec      *pspec)
{
  CinnamonGlobal *global = CINNAMON_GLOBAL (object);

  switch (prop_id)
    {
    case PROP_SESSION_TYPE:
      g_value_set_enum (value, cinnamon_global_get_session_type (global));
      break;
    case PROP_OVERLAY_GROUP:
      g_value_set_object (value, meta_get_overlay_group_for_screen (global->meta_screen));
      break;
    case PROP_SCREEN:
      g_value_set_object (value, global->meta_screen);
      break;
    case PROP_GDK_SCREEN:
      g_value_set_object (value, global->gdk_screen);
      break;
    case PROP_DISPLAY:
      g_value_set_object (value, global->meta_display);
      break;
    case PROP_SCREEN_WIDTH:
      {
        int width, height;

        meta_screen_get_size (global->meta_screen, &width, &height);
        g_value_set_int (value, width);
      }
      break;
    case PROP_SCREEN_HEIGHT:
      {
        int width, height;

        meta_screen_get_size (global->meta_screen, &width, &height);
        g_value_set_int (value, height);
      }
      break;
    case PROP_STAGE:
      g_value_set_object (value, global->stage);
      break;
    case PROP_STAGE_INPUT_MODE:
      g_value_set_enum (value, global->input_mode);
      break;
    case PROP_WINDOW_GROUP:
      g_value_set_object (value, meta_get_window_group_for_screen (global->meta_screen));
      break;
    case PROP_TOP_WINDOW_GROUP:
      g_value_set_object (value, meta_get_top_window_group_for_screen (global->meta_screen));
      break;
    case PROP_BACKGROUND_ACTOR:
      g_value_set_object (value, meta_get_background_actor_for_screen (global->meta_screen));
      break;
    case PROP_WINDOW_MANAGER:
      g_value_set_object (value, global->wm);
      break;
    case PROP_SETTINGS:
      g_value_set_object (value, global->settings);
      break;
    case PROP_DATADIR:
      g_value_set_string (value, global->datadir);
      break;
    case PROP_IMAGEDIR:
      g_value_set_string (value, global->imagedir);
      break;
    case PROP_USERDATADIR:
      g_value_set_string (value, global->userdatadir);
      break;
    case PROP_FOCUS_MANAGER:
      g_value_set_object (value, global->focus_manager);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
cinnamon_global_init (CinnamonGlobal *global)
{
  const char *datadir = g_getenv ("CINNAMON_DATADIR");
  const char *cinnamon_js = g_getenv("CINNAMON_JS");
  char *imagedir, **search_path;

  if (!datadir)
    datadir = CINNAMON_DATADIR;
  global->datadir = datadir;

  /* We make sure imagedir ends with a '/', since the JS won't have
   * access to g_build_filename() and so will end up just
   * concatenating global.imagedir to a filename.
   */
  imagedir = g_build_filename (datadir, "images/", NULL);
  if (g_file_test (imagedir, G_FILE_TEST_IS_DIR))
    global->imagedir = imagedir;
  else
    {
      g_free (imagedir);
      global->imagedir = g_strdup_printf ("%s/", datadir);
    }

  /* Ensure config dir exists for later use */
  global->userdatadir = g_build_filename (g_get_user_data_dir (), "cinnamon", NULL);
  g_mkdir_with_parents (global->userdatadir, 0700);

  global->settings = g_settings_new ("org.cinnamon");
  
  global->grab_notifier = GTK_WINDOW (gtk_window_new (GTK_WINDOW_TOPLEVEL));
  g_signal_connect (global->grab_notifier, "grab-notify", G_CALLBACK (grab_notify), global);
  global->gtk_grab_active = FALSE;

  global->input_mode = CINNAMON_STAGE_INPUT_MODE_NORMAL;

  ca_context_create (&global->sound_context);
  ca_context_change_props (global->sound_context, CA_PROP_APPLICATION_NAME, PACKAGE_NAME, CA_PROP_APPLICATION_ID, "org.Cinnamon", NULL);
  ca_context_open (global->sound_context);

  if (!cinnamon_js)
    cinnamon_js = JSDIR;
  search_path = g_strsplit (cinnamon_js, ":", -1);
  global->js_context = g_object_new (GJS_TYPE_CONTEXT,
                                     "search-path", search_path,
                                     "js-version", "1.8",
                                     "gc-notifications", TRUE,
                                     NULL);
  g_signal_connect (global->js_context, "gc", G_CALLBACK (cinnamon_global_on_gc), global);

  g_strfreev (search_path);
}

static void
cinnamon_global_finalize (GObject *object)
{
  CinnamonGlobal *global = CINNAMON_GLOBAL (object);

  g_object_unref (global->js_context);
  gtk_widget_destroy (GTK_WIDGET (global->grab_notifier));
  g_object_unref (global->settings);

  the_object = NULL;

  G_OBJECT_CLASS(cinnamon_global_parent_class)->finalize (object);
}

static void
cinnamon_global_class_init (CinnamonGlobalClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->get_property = cinnamon_global_get_property;
  gobject_class->set_property = cinnamon_global_set_property;
  gobject_class->finalize = cinnamon_global_finalize;

  /* Emitted from cinnamon-plugin.c during event handling */
  cinnamon_global_signals[XDND_POSITION_CHANGED] =
      g_signal_new ("xdnd-position-changed",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_LAST,
                    0,
                    NULL, NULL,
                    _cinnamon_marshal_VOID__INT_INT,
                    G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);

  /* Emitted from cinnamon-plugin.c during event handling */
  cinnamon_global_signals[XDND_LEAVE] =
      g_signal_new ("xdnd-leave",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_LAST,
                    0,
                    NULL, NULL,
                    g_cclosure_marshal_VOID__VOID,
                    G_TYPE_NONE, 0);

  /* Emitted from cinnamon-plugin.c during event handling */
  cinnamon_global_signals[XDND_ENTER] =
      g_signal_new ("xdnd-enter",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_LAST,
                    0,
                    NULL, NULL,
                    g_cclosure_marshal_VOID__VOID,
                    G_TYPE_NONE, 0);

  cinnamon_global_signals[NOTIFY_ERROR] =
      g_signal_new ("notify-error",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_LAST,
                    0,
                    NULL, NULL,
                    gi_cclosure_marshal_generic,
                    G_TYPE_NONE, 2,
                    G_TYPE_STRING,
                    G_TYPE_STRING);

  g_object_class_install_property (gobject_class,
                                   PROP_SESSION_TYPE,
                                   g_param_spec_enum ("session-type",
                                                      "Session Type",
                                                      "The type of session",
                                                      CINNAMON_TYPE_SESSION_TYPE,
                                                      CINNAMON_SESSION_USER,
                                                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (gobject_class,
                                   PROP_OVERLAY_GROUP,
                                   g_param_spec_object ("overlay-group",
                                                        "Overlay Group",
                                                        "Actor holding objects that appear above the desktop contents",
                                                        CLUTTER_TYPE_ACTOR,
                                                        G_PARAM_READABLE));
  g_object_class_install_property (gobject_class,
                                   PROP_SCREEN,
                                   g_param_spec_object ("screen",
                                                        "Screen",
                                                        "Metacity screen object for Cinnamon",
                                                        META_TYPE_SCREEN,
                                                        G_PARAM_READABLE));

  g_object_class_install_property (gobject_class,
                                   PROP_GDK_SCREEN,
                                   g_param_spec_object ("gdk-screen",
                                                        "GdkScreen",
                                                        "Gdk screen object for Cinnamon",
                                                        GDK_TYPE_SCREEN,
                                                        G_PARAM_READABLE));

  g_object_class_install_property (gobject_class,
                                   PROP_SCREEN_WIDTH,
                                   g_param_spec_int ("screen-width",
                                                     "Screen Width",
                                                     "Screen width, in pixels",
                                                     0, G_MAXINT, 1,
                                                     G_PARAM_READABLE));

  g_object_class_install_property (gobject_class,
                                   PROP_SCREEN_HEIGHT,
                                   g_param_spec_int ("screen-height",
                                                     "Screen Height",
                                                     "Screen height, in pixels",
                                                     0, G_MAXINT, 1,
                                                     G_PARAM_READABLE));
  g_object_class_install_property (gobject_class,
                                   PROP_DISPLAY,
                                   g_param_spec_object ("display",
                                                        "Display",
                                                        "Metacity display object for Cinnamon",
                                                        META_TYPE_DISPLAY,
                                                        G_PARAM_READABLE));

  g_object_class_install_property (gobject_class,
                                   PROP_STAGE,
                                   g_param_spec_object ("stage",
                                                        "Stage",
                                                        "Stage holding the desktop scene graph",
                                                        CLUTTER_TYPE_ACTOR,
                                                        G_PARAM_READABLE));
  g_object_class_install_property (gobject_class,
                                   PROP_STAGE_INPUT_MODE,
                                   g_param_spec_enum ("stage-input-mode",
                                                      "Stage input mode",
                                                      "The stage input mode",
                                                      CINNAMON_TYPE_STAGE_INPUT_MODE,
                                                      CINNAMON_STAGE_INPUT_MODE_NORMAL,
                                                      G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class,
                                   PROP_WINDOW_GROUP,
                                   g_param_spec_object ("window-group",
                                                        "Window Group",
                                                        "Actor holding window actors",
                                                        CLUTTER_TYPE_ACTOR,
                                                        G_PARAM_READABLE));
  g_object_class_install_property (gobject_class,
                                   PROP_TOP_WINDOW_GROUP,
                                   g_param_spec_object ("top-window-group",
                                                        "Top Window Group",
                                                        "Actor holding popup menus and other actors which must appear on top of the panels",
                                                        CLUTTER_TYPE_ACTOR,
                                                        G_PARAM_READABLE));
  g_object_class_install_property (gobject_class,
                                   PROP_BACKGROUND_ACTOR,
                                   g_param_spec_object ("background-actor",
                                                        "Background Actor",
                                                        "Actor drawing root window background",
                                                        CLUTTER_TYPE_ACTOR,
                                                        G_PARAM_READABLE));
  g_object_class_install_property (gobject_class,
                                   PROP_WINDOW_MANAGER,
                                   g_param_spec_object ("window-manager",
                                                        "Window Manager",
                                                        "Window management interface",
                                                        CINNAMON_TYPE_WM,
                                                        G_PARAM_READABLE));
  g_object_class_install_property (gobject_class,
                                   PROP_SETTINGS,
                                   g_param_spec_object ("settings",
                                                        "Settings",
                                                        "GSettings instance for cinnamon configuration",
                                                        G_TYPE_SETTINGS,
                                                        G_PARAM_READABLE));
  g_object_class_install_property (gobject_class,
                                   PROP_DATADIR,
                                   g_param_spec_string ("datadir",
                                                        "Data directory",
                                                        "Directory containing cinnamon data files",
                                                        NULL,
                                                        G_PARAM_READABLE));
  g_object_class_install_property (gobject_class,
                                   PROP_IMAGEDIR,
                                   g_param_spec_string ("imagedir",
                                                        "Image directory",
                                                        "Directory containing cinnamon image files",
                                                        NULL,
                                                        G_PARAM_READABLE));
  g_object_class_install_property (gobject_class,
                                   PROP_USERDATADIR,
                                   g_param_spec_string ("userdatadir",
                                                        "User data directory",
                                                        "Directory containing cinnamon user data",
                                                        NULL,
                                                        G_PARAM_READABLE));
  g_object_class_install_property (gobject_class,
                                   PROP_FOCUS_MANAGER,
                                   g_param_spec_object ("focus-manager",
                                                        "Focus manager",
                                                        "Cinnamon's StFocusManager",
                                                        ST_TYPE_FOCUS_MANAGER,
                                                        G_PARAM_READABLE));
}

/**•
 * _cinnamon_global_init: (skip)•
 * @first_property_name: the name of the first property
 * @...: the value of the first property, followed optionally by more
 *  name/value pairs, followed by %NULL
 *•
 * Initializes Cinnamon global singleton with the construction-time
 * properties.
 *
 * There are currently no such properties, so @first_property_name should
 * always be %NULL.
 *
 * This call must be called before cinnamon_global_get() and shouldn't be called
 * more than once.
 */
void
_cinnamon_global_init (const char *first_property_name,
                    ...)
{
  va_list argument_list;

  g_return_if_fail (the_object == NULL);

  va_start (argument_list, first_property_name);
  the_object = CINNAMON_GLOBAL (g_object_new_valist (CINNAMON_TYPE_GLOBAL,
                                                  first_property_name,
                                                  argument_list));
  va_end (argument_list);

}

/**
 * cinnamon_global_get:
 *
 * Gets the singleton global object that represents the desktop.
 *
 * Return value: (transfer none): the singleton global object
 */
CinnamonGlobal *
cinnamon_global_get (void)
{
  return the_object;
}

static void
focus_window_changed (MetaDisplay *display,
                      GParamSpec  *param,
                      gpointer     user_data)
{
  CinnamonGlobal *global = user_data;

  if (global->input_mode == CINNAMON_STAGE_INPUT_MODE_FOCUSED &&
      meta_display_get_focus_window (display) != NULL)
    cinnamon_global_set_stage_input_mode (global, CINNAMON_STAGE_INPUT_MODE_NORMAL);
}

static void
cinnamon_global_focus_stage (CinnamonGlobal *global)
{
  XSetInputFocus (global->xdisplay, global->stage_xwindow,
                  RevertToPointerRoot,
                  cinnamon_global_get_current_time (global));
}

/**
 * cinnamon_global_set_stage_input_mode:
 * @global: the #CinnamonGlobal
 * @mode: the stage input mode
 *
 * Sets the input mode of the stage; when @mode is
 * %CINNAMON_STAGE_INPUT_MODE_NONREACTIVE, then the stage does not absorb
 * any clicks, but just passes them through to underlying windows.
 * When it is %CINNAMON_STAGE_INPUT_MODE_NORMAL, then the stage accepts
 * clicks in the region defined by
 * cinnamon_global_set_stage_input_region() but passes through clicks
 * outside that region. When it is %CINNAMON_STAGE_INPUT_MODE_FULLSCREEN,
 * the stage absorbs all input.
 *
 * When the input mode is %CINNAMON_STAGE_INPUT_MODE_FOCUSED, the pointer
 * is handled as with %CINNAMON_STAGE_INPUT_MODE_NORMAL, but additionally
 * the stage window has the keyboard focus. If the stage loses the
 * focus (eg, because the user clicked into a window) the input mode
 * will revert to %CINNAMON_STAGE_INPUT_MODE_NORMAL.
 *
 * Note that whenever a muffin-internal Gtk widget has a pointer grab,
 * Cinnamon behaves as though it was in
 * %CINNAMON_STAGE_INPUT_MODE_NONREACTIVE, to ensure that the widget gets
 * any clicks it is expecting.
 */
void
cinnamon_global_set_stage_input_mode (CinnamonGlobal         *global,
                                   CinnamonStageInputMode  mode)
{
  g_return_if_fail (CINNAMON_IS_GLOBAL (global));

  if (mode == CINNAMON_STAGE_INPUT_MODE_NONREACTIVE || global->gtk_grab_active)
    meta_plugin_set_stage_reactive (global->plugin, FALSE);
  else if (mode == CINNAMON_STAGE_INPUT_MODE_FULLSCREEN || !global->input_region)
    meta_plugin_set_stage_reactive (global->plugin, TRUE);
  else
    meta_plugin_set_stage_input_region (global->plugin, global->input_region);

  if (mode == CINNAMON_STAGE_INPUT_MODE_FOCUSED)
    cinnamon_global_focus_stage (global);

  if (mode != global->input_mode)
    {
      global->input_mode = mode;
      g_object_notify (G_OBJECT (global), "stage-input-mode");
    }
}

/**
 * cinnamon_global_set_cursor:
 * @global: A #CinnamonGlobal
 * @type: the type of the cursor
 *
 * Set the cursor on the stage window.
 */
void
cinnamon_global_set_cursor (CinnamonGlobal *global,
                         CinnamonCursor type)
{
  const char *name;
  GdkCursor *cursor;

  switch (type)
    {
    case CINNAMON_CURSOR_DND_IN_DRAG:
      name = "dnd-none";
      break;
    case CINNAMON_CURSOR_DND_MOVE:
      name = "dnd-move";
      break;
    case CINNAMON_CURSOR_DND_COPY:
      name = "dnd-copy";
      break;
    case CINNAMON_CURSOR_DND_UNSUPPORTED_TARGET:
      name = "dnd-none";
      break;
    case CINNAMON_CURSOR_POINTING_HAND:
      name = "hand";
      break;
    default:
      g_return_if_reached ();
    }

  cursor = gdk_cursor_new_from_name (global->gdk_display, name);
  if (!cursor)
    {
      GdkCursorType cursor_type;
      switch (type)
        {
        case CINNAMON_CURSOR_DND_IN_DRAG:
          cursor_type = GDK_FLEUR;
          break;
        case CINNAMON_CURSOR_DND_MOVE:
          cursor_type = GDK_TARGET;
          break;
        case CINNAMON_CURSOR_DND_COPY:
          cursor_type = GDK_PLUS;
          break;
        case CINNAMON_CURSOR_POINTING_HAND:
          cursor_type = GDK_HAND2;
        case CINNAMON_CURSOR_DND_UNSUPPORTED_TARGET:
          cursor_type = GDK_X_CURSOR;
          break;
        default:
          g_return_if_reached ();
        }
      cursor = gdk_cursor_new (cursor_type);
    }

  gdk_window_set_cursor (global->stage_gdk_window, cursor);

  gdk_cursor_unref (cursor);
}

/**
 * cinnamon_global_unset_cursor:
 * @global: A #CinnamonGlobal
 *
 * Unset the cursor on the stage window.
 */
void
cinnamon_global_unset_cursor (CinnamonGlobal  *global)
{
  gdk_window_set_cursor (global->stage_gdk_window, NULL);
}

/**
 * cinnamon_global_set_stage_input_region:
 * @global: the #CinnamonGlobal
 * @rectangles: (element-type Meta.Rectangle): a list of #MetaRectangle
 * describing the input region.
 *
 * Sets the area of the stage that is responsive to mouse clicks when
 * the stage mode is %CINNAMON_STAGE_INPUT_MODE_NORMAL (but does not change the
 * current stage mode).
 */
void
cinnamon_global_set_stage_input_region (CinnamonGlobal *global,
                                     GSList      *rectangles)
{
  MetaRectangle *rect;
  XRectangle *rects;
  int nrects, i;
  GSList *r;

  g_return_if_fail (CINNAMON_IS_GLOBAL (global));

  nrects = g_slist_length (rectangles);
  rects = g_new (XRectangle, nrects);
  for (r = rectangles, i = 0; r; r = r->next, i++)
    {
      rect = (MetaRectangle *)r->data;
      rects[i].x = rect->x;
      rects[i].y = rect->y;
      rects[i].width = rect->width;
      rects[i].height = rect->height;
    }

  if (global->input_region)
    XFixesDestroyRegion (global->xdisplay, global->input_region);

  global->input_region = XFixesCreateRegion (global->xdisplay, rects, nrects);
  g_free (rects);

  /* set_stage_input_mode() will figure out whether or not we
   * should actually change the input region right now.
   */
  cinnamon_global_set_stage_input_mode (global, global->input_mode);
}

/**
 * cinnamon_global_get_screen:
 *
 * Return value: (transfer none): The default #MetaScreen
 */
MetaScreen *
cinnamon_global_get_screen (CinnamonGlobal  *global)
{
  return global->meta_screen;
}

/**
 * cinnamon_global_get_gdk_screen:
 *
 * Return value: (transfer none): Gdk screen object for Cinnamon
 */
GdkScreen *
cinnamon_global_get_gdk_screen (CinnamonGlobal *global)
{
  g_return_val_if_fail (CINNAMON_IS_GLOBAL (global), NULL);

  return global->gdk_screen;
}

/**
 * cinnamon_global_get_display:
 *
 * Return value: (transfer none): The default #MetaDisplay
 */
MetaDisplay *
cinnamon_global_get_display (CinnamonGlobal  *global)
{
  return global->meta_display;
}

/**
 * cinnamon_global_get_window_actors:
 *
 * Gets the list of #MetaWindowActor for the plugin's screen
 *
 * Return value: (element-type Meta.WindowActor) (transfer none): the list of windows
 */
GList *
cinnamon_global_get_window_actors (CinnamonGlobal *global)
{
  g_return_val_if_fail (CINNAMON_IS_GLOBAL (global), NULL);

  return meta_get_window_actors (global->meta_screen);
}

static void
global_stage_notify_width (GObject    *gobject,
                           GParamSpec *pspec,
                           gpointer    data)
{
  CinnamonGlobal *global = CINNAMON_GLOBAL (data);

  g_object_notify (G_OBJECT (global), "screen-width");
}

static void
global_stage_notify_height (GObject    *gobject,
                            GParamSpec *pspec,
                            gpointer    data)
{
  CinnamonGlobal *global = CINNAMON_GLOBAL (data);

  g_object_notify (G_OBJECT (global), "screen-height");
}

static void
global_stage_before_paint (ClutterStage *stage,
                           CinnamonGlobal  *global)
{
  cinnamon_perf_log_event (cinnamon_perf_log_get_default (),
                        "clutter.stagePaintStart");
}

static void
global_stage_after_paint (ClutterStage *stage,
                          CinnamonGlobal  *global)
{
  cinnamon_perf_log_event (cinnamon_perf_log_get_default (),
                        "clutter.stagePaintDone");
}

void
_cinnamon_global_set_plugin (CinnamonGlobal *global,
                          MetaPlugin  *plugin)
{
  g_return_if_fail (CINNAMON_IS_GLOBAL (global));
  g_return_if_fail (global->plugin == NULL);

  global->plugin = plugin;
  global->wm = cinnamon_wm_new (plugin);

  global->meta_screen = meta_plugin_get_screen (plugin);
  global->meta_display = meta_screen_get_display (global->meta_screen);
  global->xdisplay = meta_display_get_xdisplay (global->meta_display);

  global->gdk_display = gdk_x11_lookup_xdisplay (global->xdisplay);
  global->gdk_screen = gdk_display_get_screen (global->gdk_display,
                                               meta_screen_get_screen_number (global->meta_screen));

  global->stage = CLUTTER_STAGE (meta_plugin_get_stage (plugin));
  global->stage_xwindow = clutter_x11_get_stage_window (global->stage);
  global->stage_gdk_window = gdk_x11_window_foreign_new_for_display (global->gdk_display,
                                                                     global->stage_xwindow);

  g_signal_connect (global->stage, "notify::width",
                    G_CALLBACK (global_stage_notify_width), global);
  g_signal_connect (global->stage, "notify::height",
                    G_CALLBACK (global_stage_notify_height), global);

  g_signal_connect (global->stage, "paint",
                    G_CALLBACK (global_stage_before_paint), global);
  g_signal_connect_after (global->stage, "paint",
                          G_CALLBACK (global_stage_after_paint), global);

  cinnamon_perf_log_define_event (cinnamon_perf_log_get_default(),
                               "clutter.stagePaintStart",
                               "Start of stage page repaint",
                               "");
  cinnamon_perf_log_define_event (cinnamon_perf_log_get_default(),
                               "clutter.stagePaintDone",
                               "End of stage page repaint",
                               "");

  g_signal_connect (global->meta_display, "notify::focus-window",
                    G_CALLBACK (focus_window_changed), global);

  global->focus_manager = st_focus_manager_get_for_stage (global->stage);
}

GjsContext *
_cinnamon_global_get_gjs_context (CinnamonGlobal *global)
{
  return global->js_context;
}

/**
 * cinnamon_global_begin_modal:
 * @global: a #CinnamonGlobal
 *
 * Grabs the keyboard and mouse to the stage window. The stage will
 * receive all keyboard and mouse events until cinnamon_global_end_modal()
 * is called. This is used to implement "modes" for Cinnamon, such as the
 * overview mode or the "looking glass" debug overlay, that block
 * application and normal key shortcuts.
 *
 * Returns value: %TRUE if we succesfully entered the mode. %FALSE if we couldn't
 *  enter the mode. Failure may occur because an application has the pointer
 *  or keyboard grabbed, because Muffin is in a mode itself like moving a
 *  window or alt-Tab window selection, or because cinnamon_global_begin_modal()
 *  was previouly called.
 */
gboolean
cinnamon_global_begin_modal (CinnamonGlobal *global,
                          guint32      timestamp)
{
  return meta_plugin_begin_modal (global->plugin, global->stage_xwindow,
                                  None, 0, timestamp);
}

/**
 * cinnamon_global_end_modal:
 * @global: a #CinnamonGlobal
 *
 * Undoes the effect of cinnamon_global_begin_modal().
 */
void
cinnamon_global_end_modal (CinnamonGlobal *global,
                        guint32      timestamp)
{
  meta_plugin_end_modal (global->plugin, timestamp);
}

/**
 * cinnamon_global_create_pointer_barrier
 * @global: a #CinnamonGlobal
 * @x1: left X coordinate
 * @y1: top Y coordinate
 * @x2: right X coordinate
 * @y2: bottom Y coordinate
 * @directions: The directions we're allowed to pass through
 *
 * If supported by X creates a pointer barrier.
 *
 * Return value: value you can pass to cinnamon_global_destroy_pointer_barrier()
 */
guint32
cinnamon_global_create_pointer_barrier (CinnamonGlobal *global,
                                     int x1, int y1, int x2, int y2,
                                     int directions)
{
#if HAVE_XFIXESCREATEPOINTERBARRIER
  return (guint32)
    XFixesCreatePointerBarrier (global->xdisplay,
                                DefaultRootWindow (global->xdisplay),
                                x1, y1,
                                x2, y2,
                                directions,
                                0, NULL);
#else
  return 0;
#endif
}

/**
 * cinnamon_global_destroy_pointer_barrier
 * @global: a #CinnamonGlobal
 * @barrier: a pointer barrier
 *
 * Destroys the @barrier created by cinnamon_global_create_pointer_barrier().
 */
void
cinnamon_global_destroy_pointer_barrier (CinnamonGlobal *global, guint32 barrier)
{
#if HAVE_XFIXESCREATEPOINTERBARRIER
  g_return_if_fail (barrier > 0);

  XFixesDestroyPointerBarrier (global->xdisplay, (PointerBarrier)barrier);
#endif
}


/**
 * cinnamon_global_add_extension_importer:
 * @target_object_script: JavaScript code evaluating to a target object
 * @target_property: Name of property to use for importer
 * @directory: Source directory:
 * @error: A #GError
 *
 * This function sets a property named @target_property on the object
 * resulting from the evaluation of @target_object_script code, which
 * acts as a GJS importer for directory @directory.
 *
 * Returns: %TRUE on success
 */
gboolean
cinnamon_global_add_extension_importer (CinnamonGlobal *global,
                                     const char  *target_object_script,
                                     const char  *target_property,
                                     const char  *directory,
                                     GError     **error)
{
  jsval target_object;
  JSContext *context = gjs_context_get_native_context (global->js_context);
  char *search_path[2] = { 0, 0 };

  JS_BeginRequest (context);

  // This is a bit of a hack; ideally we'd be able to pass our target
  // object directly into this function, but introspection doesn't
  // support that at the moment.  Instead evaluate a string to get it.
  if (!JS_EvaluateScript(context,
                         JS_GetGlobalObject(context),
                         target_object_script,
                         strlen (target_object_script),
                         "<target_object_script>",
                         0,
                         &target_object))
    {
      char *message;
      gjs_log_exception(context,
                        &message);
      g_set_error(error,
                  G_IO_ERROR,
                  G_IO_ERROR_FAILED,
                  "%s", message ? message : "(unknown)");
      g_free(message);
      goto out_error;
    }

  if (!JSVAL_IS_OBJECT (target_object))
    {
      g_error ("cinnamon_global_add_extension_importer: invalid target object");
      goto out_error;
    }

  search_path[0] = (char*)directory;
  gjs_define_importer (context, JSVAL_TO_OBJECT (target_object), target_property, (const char **)search_path, FALSE);
  JS_EndRequest (context);
  return TRUE;
 out_error:
  JS_EndRequest (context);
  return FALSE;
}

/* Code to close all file descriptors before we exec; copied from gspawn.c in GLib.
 *
 * Authors: Padraig O'Briain, Matthias Clasen, Lennart Poettering
 *
 * http://bugzilla.gnome.org/show_bug.cgi?id=469231
 * http://bugzilla.gnome.org/show_bug.cgi?id=357585
 */

static int
set_cloexec (void *data, gint fd)
{
  if (fd >= GPOINTER_TO_INT (data))
    fcntl (fd, F_SETFD, FD_CLOEXEC);

  return 0;
}

#ifndef HAVE_FDWALK
static int
fdwalk (int (*cb)(void *data, int fd), void *data)
{
  gint open_max;
  gint fd;
  gint res = 0;

#ifdef HAVE_SYS_RESOURCE_H
  struct rlimit rl;
#endif

#ifdef __linux__
  DIR *d;

  if ((d = opendir("/proc/self/fd"))) {
      struct dirent *de;

      while ((de = readdir(d))) {
          glong l;
          gchar *e = NULL;

          if (de->d_name[0] == '.')
              continue;

          errno = 0;
          l = strtol(de->d_name, &e, 10);
          if (errno != 0 || !e || *e)
              continue;

          fd = (gint) l;

          if ((glong) fd != l)
              continue;

          if (fd == dirfd(d))
              continue;

          if ((res = cb (data, fd)) != 0)
              break;
        }

      closedir(d);
      return res;
  }

  /* If /proc is not mounted or not accessible we fall back to the old
   * rlimit trick */

#endif

#ifdef HAVE_SYS_RESOURCE_H
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_max != RLIM_INFINITY)
      open_max = rl.rlim_max;
  else
#endif
      open_max = sysconf (_SC_OPEN_MAX);

  for (fd = 0; fd < open_max; fd++)
      if ((res = cb (data, fd)) != 0)
          break;

  return res;
}
#endif

static void
pre_exec_close_fds(void)
{
  fdwalk (set_cloexec, GINT_TO_POINTER(3));
}

/**
 * cinnamon_global_reexec_self:
 * @global: A #CinnamonGlobal
 * 
 * Restart the current process.  Only intended for development purposes. 
 */
void 
cinnamon_global_reexec_self (CinnamonGlobal *global)
{
  GPtrArray *arr;
  gsize len;
  char *buf;
  char *buf_p;
  char *buf_end;
  GError *error = NULL;
  
  /* Linux specific (I think, anyways). */
  if (!g_file_get_contents ("/proc/self/cmdline", &buf, &len, &error))
    {
      g_warning ("failed to get /proc/self/cmdline: %s", error->message);
      return;
    }
      
  buf_end = buf+len;
  arr = g_ptr_array_new ();
  /* The cmdline file is NUL-separated */
  for (buf_p = buf; buf_p < buf_end; buf_p = buf_p + strlen (buf_p) + 1)
    g_ptr_array_add (arr, buf_p);
  
  g_ptr_array_add (arr, NULL);

  /* Close all file descriptors other than stdin/stdout/stderr, otherwise
   * they will leak and stay open after the exec. In particular, this is
   * important for file descriptors that represent mapped graphics buffer
   * objects.
   */
  pre_exec_close_fds ();

  meta_display_unmanage_screen (cinnamon_global_get_display (global),
                                cinnamon_global_get_screen (global),
                                cinnamon_global_get_current_time (global));

  execvp (arr->pdata[0], (char**)arr->pdata);
  g_warning ("failed to reexec: %s", g_strerror (errno));
  g_ptr_array_free (arr, TRUE);
}

/**
 * cinnamon_global_gc:
 * @global: A #CinnamonGlobal
 *
 * Start a garbage collection process.  For more information, see
 * https://developer.mozilla.org/En/JS_GC
 */
void
cinnamon_global_gc (CinnamonGlobal *global)
{
  JSContext *context = gjs_context_get_native_context (global->js_context);

  JS_GC (context);
}

/**
 * cinnamon_global_maybe_gc:
 * @global: A #CinnamonGlobal
 *
 * Start a garbage collection process when it would free up enough memory
 * to be worth the amount of time it would take
 * https://developer.mozilla.org/en/SpiderMonkey/JSAPI_Reference/JS_MaybeGC
 */
void
cinnamon_global_maybe_gc (CinnamonGlobal *global)
{
  gjs_context_maybe_gc (global->js_context);
}

static void
cinnamon_global_on_gc (GjsContext   *context,
                    CinnamonGlobal  *global)
{
  global->last_gc_end_time = g_get_monotonic_time ();
}

/**
 * cinnamon_global_get_memory_info:
 * @global:
 * @meminfo: (out caller-allocates): Output location for memory information
 *
 * Load process-global data about memory usage.
 */
void
cinnamon_global_get_memory_info (CinnamonGlobal        *global,
                              CinnamonMemoryInfo    *meminfo)
{
  JSContext *context;
  gint64 now;

  memset (meminfo, 0, sizeof (meminfo));
#ifdef HAVE_MALLINFO
  {
    struct mallinfo info = mallinfo ();
    meminfo->glibc_uordblks = info.uordblks;
  }
#endif

  context = gjs_context_get_native_context (global->js_context);

  meminfo->js_bytes = JS_GetGCParameter (JS_GetRuntime (context), JSGC_BYTES);

  meminfo->gjs_boxed = (unsigned int) gjs_counter_boxed.value;
  meminfo->gjs_gobject = (unsigned int) gjs_counter_object.value;
  meminfo->gjs_function = (unsigned int) gjs_counter_function.value;
  meminfo->gjs_closure = (unsigned int) gjs_counter_closure.value;

  now = g_get_monotonic_time ();

  meminfo->last_gc_seconds_ago = (now - global->last_gc_end_time) / G_TIME_SPAN_SECOND;
}


/**
 * cinnamon_global_notify_error:
 * @global: a #CinnamonGlobal
 * @msg: Error message
 * @details: Error details
 *
 * Show a system error notification.  Use this function
 * when a user-initiated action results in a non-fatal problem
 * from causes that may not be under system control.  For
 * example, an application crash.
 */
void
cinnamon_global_notify_error (CinnamonGlobal  *global,
                           const char   *msg,
                           const char   *details)
{
  g_signal_emit_by_name (global, "notify-error", msg, details);
}

static void
grab_notify (GtkWidget *widget, gboolean was_grabbed, gpointer user_data)
{
  CinnamonGlobal *global = CINNAMON_GLOBAL (user_data);
  
  global->gtk_grab_active = !was_grabbed;

  /* Update for the new setting of gtk_grab_active */
  cinnamon_global_set_stage_input_mode (global, global->input_mode);
}

/**
 * cinnamon_global_init_xdnd:
 * @global: the #CinnamonGlobal
 *
 * Enables tracking of Xdnd events
 */
void cinnamon_global_init_xdnd (CinnamonGlobal *global)
{
  Window output_window = meta_get_overlay_window (global->meta_screen);
  long xdnd_version = 5;

  XChangeProperty (global->xdisplay, global->stage_xwindow,
                   gdk_x11_get_xatom_by_name ("XdndAware"), XA_ATOM,
                   32, PropModeReplace, (const unsigned char *)&xdnd_version, 1);

  XChangeProperty (global->xdisplay, output_window,
                   gdk_x11_get_xatom_by_name ("XdndProxy"), XA_WINDOW,
                   32, PropModeReplace, (const unsigned char *)&global->stage_xwindow, 1);

  /*
   * XdndProxy is additionally set on the proxy window as verification that the
   * XdndProxy property on the target window isn't a left-over
   */
  XChangeProperty (global->xdisplay, global->stage_xwindow,
                   gdk_x11_get_xatom_by_name ("XdndProxy"), XA_WINDOW,
                   32, PropModeReplace, (const unsigned char *)&global->stage_xwindow, 1);
}

/**
 * cinnamon_global_get_pointer:
 * @global: the #CinnamonGlobal
 * @x: (out): the X coordinate of the pointer, in global coordinates
 * @y: (out): the Y coordinate of the pointer, in global coordinates
 * @mods: (out): the current set of modifier keys that are pressed down
 *
 * Gets the pointer coordinates and current modifier key state.
 * This is a wrapper around gdk_display_get_pointer() that strips
 * out any un-declared modifier flags, to make gjs happy; see
 * https://bugzilla.gnome.org/show_bug.cgi?id=597292.
 */
void
cinnamon_global_get_pointer (CinnamonGlobal         *global,
                          int                 *x,
                          int                 *y,
                          ClutterModifierType *mods)
{
  GdkModifierType raw_mods;

  gdk_display_get_pointer (global->gdk_display, NULL, x, y, &raw_mods);
  *mods = raw_mods & GDK_MODIFIER_MASK;
}

/**
 * cinnamon_global_sync_pointer:
 * @global: the #CinnamonGlobal
 *
 * Ensures that clutter is aware of the current pointer position,
 * causing enter and leave events to be emitted if the pointer moved
 * behind our back (ie, during a pointer grab).
 */
void
cinnamon_global_sync_pointer (CinnamonGlobal *global)
{
  int x, y;
  GdkModifierType mods;
  ClutterMotionEvent event;

  gdk_display_get_pointer (global->gdk_display, NULL, &x, &y, &mods);

  event.type = CLUTTER_MOTION;
  event.time = cinnamon_global_get_current_time (global);
  event.flags = 0;
  /* This is wrong: we should be setting event.stage to NULL if the
   * pointer is not inside the bounds of the stage given the current
   * stage_input_mode. For our current purposes however, this works.
   */
  event.stage = global->stage;
  event.x = x;
  event.y = y;
  event.modifier_state = mods;
  event.axes = NULL;
  event.device = clutter_device_manager_get_core_device (clutter_device_manager_get_default (),
                                                         CLUTTER_POINTER_DEVICE);

  /* Leaving event.source NULL will force clutter to look it up, which
   * will generate enter/leave events as a side effect, if they are
   * needed. We need a better way to do this though... see
   * http://bugzilla.clutter-project.org/show_bug.cgi?id=2615.
   */
  event.source = NULL;

  clutter_event_put ((ClutterEvent *)&event);
}

/**
 * cinnamon_global_get_settings:
 * @global: A #CinnamonGlobal
 *
 * Get the global GSettings instance.
 *
 * Return value: (transfer none): The GSettings object
 */
GSettings *
cinnamon_global_get_settings (CinnamonGlobal *global)
{
  return global->settings;
}

/**
 * cinnamon_global_get_current_time:
 * @global: A #CinnamonGlobal
 *
 * Returns: the current X server time from the current Clutter, Gdk, or X
 * event. If called from outside an event handler, this may return
 * %Clutter.CURRENT_TIME (aka 0), or it may return a slightly
 * out-of-date timestamp.
 */
guint32
cinnamon_global_get_current_time (CinnamonGlobal *global)
{
  guint32 time;
  const ClutterEvent *clutter_event;

  /* In case we have a xdnd timestamp use it */
  if (global->xdnd_timestamp != 0)
    return global->xdnd_timestamp;

  /* meta_display_get_current_time() will return the correct time
     when handling an X or Gdk event, but will return CurrentTime
     from some Clutter event callbacks.

     clutter_get_current_event_time() will return the correct time
     from a Clutter event callback, but may return an out-of-date
     timestamp if called at other times.

     So we try meta_display_get_current_time() first, since we
     can recognize a "wrong" answer from that, and then fall back
     to clutter_get_current_event_time().
   */

  time = meta_display_get_current_time (global->meta_display);
  if (time != CLUTTER_CURRENT_TIME)
      return time;
  /*
   * We don't use clutter_get_current_event_time as it can give us a
   * too old timestamp if there is no current event.
   */
  clutter_event = clutter_get_current_event ();

  if (clutter_event != NULL)
    return clutter_event_get_time (clutter_event);
  else
    return CLUTTER_CURRENT_TIME;
}

/**
 * cinnamon_global_create_app_launch_context:
 * @global: A #CinnamonGlobal
 *
 * Create a #GAppLaunchContext set up with the correct timestamp, and
 * targeted to activate on the current workspace.
 *
 * Return value: (transfer full): A new #GAppLaunchContext
 */
GAppLaunchContext *
cinnamon_global_create_app_launch_context (CinnamonGlobal *global)
{
  GdkAppLaunchContext *context;

  context = gdk_app_launch_context_new ();
  gdk_app_launch_context_set_timestamp (context, cinnamon_global_get_current_time (global));

  // Make sure that the app is opened on the current workspace even if
  // the user switches before it starts
  gdk_app_launch_context_set_desktop (context, meta_screen_get_active_workspace_index (global->meta_screen));

  return (GAppLaunchContext *)context;
}

typedef struct
{
  CinnamonLeisureFunction func;
  gpointer user_data;
  GDestroyNotify notify;
} LeisureClosure;

static gboolean
run_leisure_functions (gpointer data)
{
  CinnamonGlobal *global = data;
  GSList *closures;
  GSList *iter;

  global->leisure_function_id = 0;

  /* We started more work since we scheduled the idle */
  if (global->work_count > 0)
    return FALSE;

  /* Previously we called gjs_maybe_gc().  However, it simply doesn't
   * trigger often enough.  Garbage collection is very fast here, so
   * let's just aggressively GC.  This will help avoid both heap
   * fragmentation, and the GC kicking in when we don't want it to.
   */
  gjs_context_gc (global->js_context);

  /* No leisure closures, so we are done */
  if (global->leisure_closures == NULL)
    return FALSE;

  closures = global->leisure_closures;
  global->leisure_closures = NULL;

  for (iter = closures; iter; iter = iter->next)
    {
      LeisureClosure *closure = closures->data;
      closure->func (closure->user_data);

      if (closure->notify)
        closure->notify (closure->user_data);

      g_slice_free (LeisureClosure, closure);
    }

  g_slist_free (closures);

  return FALSE;
}

static void
schedule_leisure_functions (CinnamonGlobal *global)
{
  /* This is called when we think we are ready to run leisure functions
   * by our own accounting. We try to handle other types of business
   * (like ClutterAnimation) by adding a low priority idle function.
   *
   * This won't work properly if the mainloop goes idle waiting for
   * the vertical blanking interval or waiting for work being done
   * in another thread.
   */
  if (!global->leisure_function_id)
    global->leisure_function_id = g_idle_add_full (G_PRIORITY_LOW,
                                                   run_leisure_functions,
                                                   global, NULL);
}

/**
 * cinnamon_global_begin_work:
 * @global: the #CinnamonGlobal
 *
 * Marks that we are currently doing work. This is used to to track
 * whether we are busy for the purposes of cinnamon_global_run_at_leisure().
 * A count is kept and cinnamon_global_end_work() must be called exactly
 * as many times as cinnamon_global_begin_work().
 */
void
cinnamon_global_begin_work (CinnamonGlobal *global)
{
  global->work_count++;
}

/**
 * cinnamon_global_end_work:
 * @global: the #CinnamonGlobal
 *
 * Marks the end of work that we started with cinnamon_global_begin_work().
 * If no other work is ongoing and functions have been added with
 * cinnamon_global_run_at_leisure(), they will be run at the next
 * opportunity.
 */
void
cinnamon_global_end_work (CinnamonGlobal *global)
{
  g_return_if_fail (global->work_count > 0);

  global->work_count--;
  if (global->work_count == 0)
    schedule_leisure_functions (global);

}

/**
 * cinnamon_global_run_at_leisure:
 * @global: the #CinnamonGlobal
 * @func: function to call at leisure
 * @user_data: data to pass to @func
 * @notify: function to call to free @user_data
 *
 * Schedules a function to be called the next time Cinnamon is idle.
 * Idle means here no animations, no redrawing, and no ongoing background
 * work. Since there is currently no way to hook into the Clutter master
 * clock and know when is running, the implementation here is somewhat
 * approximation. Animations done through Cinnamon's Tweener module will
 * be handled properly, but other animations may be detected as terminating
 * early if they can be drawn fast enough so that the event loop goes idle
 * between frames.
 *
 * The intent of this function is for performance measurement runs
 * where a number of actions should be run serially and each action is
 * timed individually. Using this function for other purposes will
 * interfere with the ability to use it for performance measurement so
 * should be avoided.
 */
void
cinnamon_global_run_at_leisure (CinnamonGlobal         *global,
                             CinnamonLeisureFunction func,
                             gpointer             user_data,
                             GDestroyNotify       notify)
{
  LeisureClosure *closure = g_slice_new (LeisureClosure);
  closure->func = func;
  closure->user_data = user_data;
  closure->notify = notify;

  global->leisure_closures = g_slist_append (global->leisure_closures,
                                             closure);

  if (global->work_count == 0)
    schedule_leisure_functions (global);
}

/**
 * cinnamon_global_play_theme_sound:
 * @global: the #CinnamonGlobal
 * @id: an id, used to cancel later (0 if not needed)
 * @name: the sound name
 *
 * Plays a simple sound picked according to Freedesktop sound theme.
 * Really just a workaround for libcanberra not being introspected.
 */
void
cinnamon_global_play_theme_sound (CinnamonGlobal *global,
                               guint        id,
                               const char  *name)
{
  ca_context_play (global->sound_context, id, CA_PROP_EVENT_ID, name, NULL);
}

/**
 * cinnamon_global_cancel_theme_sound:
 * @global: the #CinnamonGlobal
 * @id: the id previously passed to cinnamon_global_play_theme_sound()
 *
 * Cancels a sound notification.
 */
void
cinnamon_global_cancel_theme_sound (CinnamonGlobal *global,
                                 guint id)
{
  ca_context_cancel (global->sound_context, id);
}

/*
 * Process Xdnd events
 *
 * We pass the position and leave events to JS via a signal
 * where the actual drag & drop handling happens.
 *
 * http://www.freedesktop.org/wiki/Specifications/XDND
 */
gboolean _cinnamon_global_check_xdnd_event (CinnamonGlobal  *global,
                                         XEvent       *xev)
{
  Window output_window = meta_get_overlay_window (global->meta_screen);

  if (xev->xany.window != output_window && xev->xany.window != global->stage_xwindow)
    return FALSE;

  if (xev->xany.type == ClientMessage && xev->xclient.message_type == gdk_x11_get_xatom_by_name ("XdndPosition"))
    {
      XEvent xevent;
      Window src = xev->xclient.data.l[0];

      memset (&xevent, 0, sizeof(xevent));
      xevent.xany.type = ClientMessage;
      xevent.xany.display = global->xdisplay;
      xevent.xclient.window = src;
      xevent.xclient.message_type = gdk_x11_get_xatom_by_name ("XdndStatus");
      xevent.xclient.format = 32;
      xevent.xclient.data.l[0] = output_window;
      /* flags: bit 0: will we accept the drop? bit 1: do we want more position messages */
      xevent.xclient.data.l[1] = 2;
      xevent.xclient.data.l[4] = None;

      XSendEvent (global->xdisplay, src, False, 0, &xevent);

      /* Store the timestamp of the xdnd position event */
      global->xdnd_timestamp = xev->xclient.data.l[3];
      g_signal_emit_by_name (G_OBJECT (global), "xdnd-position-changed",
                            (int)(xev->xclient.data.l[2] >> 16), (int)(xev->xclient.data.l[2] & 0xFFFF));
      global->xdnd_timestamp = 0;

      return TRUE;
    }
   else if (xev->xany.type == ClientMessage && xev->xclient.message_type == gdk_x11_get_xatom_by_name ("XdndLeave"))
    {
      g_signal_emit_by_name (G_OBJECT (global), "xdnd-leave");

      return TRUE;
    }
   else if (xev->xany.type == ClientMessage && xev->xclient.message_type == gdk_x11_get_xatom_by_name ("XdndEnter"))
    {
      g_signal_emit_by_name (G_OBJECT (global), "xdnd-enter");

      return TRUE;
    }

    return FALSE;
}

/**
 * cinnamon_global_launch_calendar_server:
 * @global: The #CinnamonGlobal.
 *
 * Launch the cinnamon-calendar-server helper.
 */
void
cinnamon_global_launch_calendar_server (CinnamonGlobal *global)
{
  const gchar *bin_dir;
  gchar *calendar_server_exe;
  GError *error;
  gchar *argv[2];
  gint child_standard_input;

  /* launch calendar-server */
  bin_dir = g_getenv ("CINNAMON_BINDIR");
  if (bin_dir != NULL)
    calendar_server_exe = g_strdup_printf ("%s/cinnamon-calendar-server", bin_dir);
  else
    calendar_server_exe = g_strdup_printf (CINNAMON_LIBEXECDIR "/cinnamon-calendar-server");

  argv[0] = calendar_server_exe;
  argv[1] = NULL;
  error = NULL;
  if (!g_spawn_async_with_pipes (NULL, /* working_directory */
                                 argv,
                                 NULL, /* envp */
                                 0, /* GSpawnFlags */
                                 NULL, /* child_setup */
                                 NULL, /* user_data */
                                 NULL, /* GPid *child_pid */
                                 &child_standard_input,
                                 NULL, /* gint *stdout */
                                 NULL, /* gint *stderr */
                                 &error))
    {
      g_warning ("Error launching `%s': %s (%s %d)",
                 calendar_server_exe,
                 error->message,
                 g_quark_to_string (error->domain),
                 error->code);
      g_error_free (error);
    }
  /* Note that cinnamon-calendar-server exits whenever its stdin
   * file descriptor is HUP'ed. This means that whenever the Cinnamon
   * process exits or is being replaced, the calendar server is also
   * exits...and if Cinnamon is being replaced, a new copy of the
   * calendar server is launched...
   */

  g_free (calendar_server_exe);
}

static void
grab_screenshot (ClutterActor *stage,
                 _screenshot_data *screenshot_data)
{
  MetaScreen *screen = cinnamon_global_get_screen (screenshot_data->global);
  cairo_status_t status;
  cairo_surface_t *image;
  guchar *data;
  int width, height;

  meta_plugin_query_screen_size (screenshot_data->global->plugin, &width, &height);
  image = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
  data = cairo_image_surface_get_data (image);

  cogl_flush();

  cogl_read_pixels (0, 0, width, height, COGL_READ_PIXELS_COLOR_BUFFER, CLUTTER_CAIRO_FORMAT_ARGB32, data);

  cairo_surface_mark_dirty (image);

  if (meta_screen_get_n_monitors (screen) > 1)
    {
      cairo_region_t *screen_region = cairo_region_create ();
      cairo_region_t *stage_region;
      MetaRectangle monitor_rect;
      cairo_rectangle_int_t stage_rect;
      int i;
      cairo_t *cr;

      for (i = meta_screen_get_n_monitors (screen) - 1; i >= 0; i--)
        {
          meta_screen_get_monitor_geometry (screen, i, &monitor_rect);
          cairo_region_union_rectangle (screen_region, (const cairo_rectangle_int_t *) &monitor_rect);
        }

      stage_rect.x = 0;
      stage_rect.y = 0;
      stage_rect.width = width;
      stage_rect.height = height;

      stage_region = cairo_region_create_rectangle ((const cairo_rectangle_int_t *) &stage_rect);
      cairo_region_xor (stage_region, screen_region);
      cairo_region_destroy (screen_region);

      cr = cairo_create (image);

      for (i = 0; i < cairo_region_num_rectangles (stage_region); i++)
        {
          cairo_rectangle_int_t rect;
          cairo_region_get_rectangle (stage_region, i, &rect);
          cairo_rectangle (cr, (double) rect.x, (double) rect.y, (double) rect.width, (double) rect.height);
          cairo_fill (cr);
        }

      cairo_destroy (cr);
      cairo_region_destroy (stage_region);
    }


  status = cairo_surface_write_to_png (image, screenshot_data->filename);
  cairo_surface_destroy (image);

  if (screenshot_data->callback)
    screenshot_data->callback (screenshot_data->global, status == CAIRO_STATUS_SUCCESS);

  g_signal_handlers_disconnect_by_func (stage, (void *)grab_screenshot, (gpointer)screenshot_data);
  g_free (screenshot_data->filename);
  g_free (screenshot_data);
}

static void
grab_area_screenshot (ClutterActor *stage,
                      _screenshot_data *screenshot_data)
{
  cairo_status_t status;
  cairo_surface_t *image = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, screenshot_data->width, screenshot_data->height);
  guchar *data = cairo_image_surface_get_data (image);

  cogl_flush();

  cogl_read_pixels (screenshot_data->x, screenshot_data->y, screenshot_data->width, screenshot_data->height,
                    COGL_READ_PIXELS_COLOR_BUFFER, CLUTTER_CAIRO_FORMAT_ARGB32, data);

  cairo_surface_mark_dirty (image);
  status = cairo_surface_write_to_png (image, screenshot_data->filename);
  cairo_surface_destroy (image);

  if (screenshot_data->callback)
    screenshot_data->callback (screenshot_data->global, status == CAIRO_STATUS_SUCCESS);

  g_signal_handlers_disconnect_by_func (stage, (void *)grab_area_screenshot, (gpointer)screenshot_data);
  g_free (screenshot_data->filename);
  g_free (screenshot_data);
}

/**
 * cinnamon_global_screenshot:
 * @global: the #CinnamonGlobal
 * @filename: The filename for the screenshot
 * @callback: (scope async): function to call returning success or failure
 * of the async grabbing
 *
 * Takes a screenshot of the whole screen
 * in @filename as png image.
 *
 */
void
cinnamon_global_screenshot (CinnamonGlobal  *global,
                        const char *filename,
                        CinnamonGlobalScreenshotCallback callback)
{
  ClutterActor *stage;
  _screenshot_data *data = g_new0 (_screenshot_data, 1);

  data->global = global;
  data->filename = g_strdup (filename);
  data->callback = callback;

  stage = CLUTTER_ACTOR (meta_plugin_get_stage (global->plugin));

  g_signal_connect_after (stage, "paint", G_CALLBACK (grab_screenshot), (gpointer)data);

  clutter_actor_queue_redraw (stage);
}

/**
 * cinnamon_global_screenshot_area:
 * @global: the #CinnamonGlobal
 * @x: The X coordinate of the area
 * @y: The Y coordinate of the area
 * @width: The width of the area
 * @height: The height of the area
 * @filename: The filename for the screenshot
 * @callback: (scope async): function to call returning success or failure
 * of the async grabbing
 *
 * Takes a screenshot of the passed in area and saves it
 * in @filename as png image.
 *
 */
void
cinnamon_global_screenshot_area (CinnamonGlobal  *global,
                              int x,
                              int y,
                              int width,
                              int height,
                              const char *filename,
                              CinnamonGlobalScreenshotCallback callback)
{
  ClutterActor *stage;
  _screenshot_data *data = g_new0 (_screenshot_data, 1);

  data->global = global;
  data->filename = g_strdup (filename);
  data->x = x;
  data->y = y;
  data->width = width;
  data->height = height;
  data->callback = callback;

  stage = CLUTTER_ACTOR (meta_plugin_get_stage (global->plugin));

  g_signal_connect_after (stage, "paint", G_CALLBACK (grab_area_screenshot), (gpointer)data);

  clutter_actor_queue_redraw (stage);
}

/**
 * cinnamon_global_screenshot_window:
 * @global: the #CinnamonGlobal
 * @include_frame: Whether to include the frame or not
 *
 * @filename: The filename for the screenshot
 *
 * Takes a screenshot of the focused window (optionally omitting the frame)
 * in @filename as png image.
 *
 * Return value: success or failure.
 */
gboolean
cinnamon_global_screenshot_window (CinnamonGlobal  *global,
                                gboolean include_frame,
                                const char *filename)
{
  CoglHandle texture;
  cairo_surface_t *image;
  guchar *data;

  MetaScreen *screen = meta_plugin_get_screen (global->plugin);
  MetaDisplay *display = meta_screen_get_display (screen);
  MetaWindow *window = meta_display_get_focus_window (display);
  ClutterActor *window_actor;

  cairo_status_t status;

  window_actor = CLUTTER_ACTOR (meta_window_get_compositor_private (window));
  texture = clutter_texture_get_cogl_texture (CLUTTER_TEXTURE (meta_window_actor_get_texture (META_WINDOW_ACTOR (window_actor))));

  if (!include_frame)
    {
      MetaRectangle *window_rect = meta_window_get_rect (window);
      texture = cogl_texture_new_from_sub_texture (texture,
                                                   window_rect->x,
                                                   window_rect->y,
                                                   window_rect->width,
                                                   window_rect->height);

      image = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                          window_rect->width,
                                          window_rect->height);
    }
  else
    image = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                        clutter_actor_get_width (window_actor),
                                        clutter_actor_get_height (window_actor));

  data = cairo_image_surface_get_data (image);

  cogl_flush();

  cogl_texture_get_data (texture, CLUTTER_CAIRO_FORMAT_ARGB32, 0, data);

  cairo_surface_mark_dirty (image);
  status = cairo_surface_write_to_png (image, filename);
  cairo_surface_destroy (image);

  return status == CAIRO_STATUS_SUCCESS;
}

/**
 * cinnamon_global_get_session_type:
 * @global: The #CinnamonGlobal.
 *
 * Gets the type of session cinnamon provides.
 *
 * The type determines what UI elements are displayed,
 * what keybindings work, and generally how Cinnamon
 * behaves.
 *
 * A session type of #CINNAMON_SESSION_USER means cinnamon
 * will enable the activities overview, status menu, run dialog,
 * etc. This is the default.
 *
 * A session type of #CINNAMON_SESSION_GDM means cinnamon
 * will enable a login dialog and run in a more confined
 * way. This type is suitable for the display manager.
 *
 * Returns the type of session cinnamon is providing.
 */
CinnamonSessionType
cinnamon_global_get_session_type (CinnamonGlobal  *global)
{
  g_return_val_if_fail (CINNAMON_IS_GLOBAL (global),
                        CINNAMON_SESSION_USER);

  return global->session_type;
}
