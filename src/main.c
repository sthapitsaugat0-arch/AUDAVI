/*
 * Audavi — GTK4 + libmpv via mpv_render_context (GL)
 * Step 1: Video window + hover overlay with play/pause button
 *
 * Uses vo=libmpv to prevent separate mpv window, then renders
 * frames via mpv_render_context into a GtkGLArea.
 */

#define _POSIX_C_SOURCE 200809L
#include <gtk/gtk.h>
#include <gdk/wayland/gdkwayland.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <locale.h>
#include <string.h>
#include <stdlib.h>

/* ---- state ---- */
static mpv_handle          *mpv;
static mpv_render_context  *mpv_gl;
static GtkWidget           *gl_area;
static GtkWidget           *overlay_revealer;
static GtkWidget           *pause_btn;
static GtkWidget           *window;
static char                *pending_file;
static gboolean             is_paused = FALSE;
static struct wl_display   *wl_display = NULL;

/* ---- cleanup ---- */
static void on_window_destroy(void) {
  if (mpv_gl) { mpv_render_context_free(mpv_gl); mpv_gl = NULL; }
  if (mpv)    { mpv_destroy(mpv); mpv = NULL; }
}

/* ---- per-frame tick callback (synced to display refresh) ---- */
static gboolean on_tick(GtkWidget *w, GdkFrameClock *clock, gpointer d) {
  (void)w; (void)clock; (void)d;
  if (gl_area)
    gtk_gl_area_queue_render(GTK_GL_AREA(gl_area));
  return G_SOURCE_CONTINUE;
}

/* ---- mpv wants a redraw ---- */
static void gl_update(void *ctx) {
  (void)ctx;
  /* Tick callback handles render scheduling now */
}

/* ---- eglGetProcAddress wrapper ---- */
static void *gl_proc(void *ctx, const char *name) {
  (void)ctx;
  return (void*)(uintptr_t)eglGetProcAddress(name);
}

/* ---- render one frame ---- */
static gboolean on_render(GtkGLArea *area, GdkGLContext *ctx, gpointer d) {
  (void)area; (void)ctx; (void)d;
  if (!mpv_gl) return FALSE;

  /* Check if there's a new frame available */
  uint64_t flags = mpv_render_context_update(mpv_gl);
  if (!(flags & MPV_RENDER_UPDATE_FRAME))
    return FALSE;

  int w = gtk_widget_get_width(GTK_WIDGET(area));
  int h = gtk_widget_get_height(GTK_WIDGET(area));
  if (w < 1 || h < 1) return FALSE;

  /* Get the currently bound framebuffer (GTK may use an FBO on Wayland) */
  GLint fbo_id = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo_id);

  mpv_opengl_fbo fbo = { .fbo = (int)fbo_id, .w = w, .h = h, .internal_format = 0 };
  int flip_y = 1;
  mpv_render_param params[] = {
    {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
    {MPV_RENDER_PARAM_FLIP_Y,     &flip_y},
    {MPV_RENDER_PARAM_WL_DISPLAY, wl_display},
    {0, NULL}
  };
  mpv_render_context_render(mpv_gl, params);
  return FALSE;
}

/* ---- GL area realised -- create mpv renderer ---- */
static void on_realize(GtkGLArea *area, gpointer d) {
  (void)d;
  gtk_gl_area_make_current(area);
  if (gtk_gl_area_get_error(area)) {
    fprintf(stderr, "GL area error: %s\n", gtk_gl_area_get_error(area)->message);
    return;
  }

  /* Grab the Wayland display */
  GdkDisplay *gdisp = gdk_display_get_default();
  wl_display = gdk_wayland_display_get_wl_display(gdisp);
  if (!wl_display) {
    fprintf(stderr, "Failed to get Wayland display\n");
    return;
  }

  mpv_opengl_init_params gl_init = {
    .get_proc_address     = gl_proc,
    .get_proc_address_ctx = NULL,
  };
  mpv_render_param params[] = {
    {MPV_RENDER_PARAM_API_TYPE,           (void*)"opengl"},
    {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
    {MPV_RENDER_PARAM_WL_DISPLAY,         wl_display},
    {0, NULL}
  };
  int r = mpv_render_context_create(&mpv_gl, mpv, params);
  if (r < 0) {
    fprintf(stderr, "mpv_gl_create: %s\n", mpv_error_string(r));
    return;
  }
  mpv_render_context_set_update_callback(mpv_gl, gl_update, NULL);

  /* Request a render ASAP — mpv's gl_update callback will trigger subsequent ones */
  gtk_gl_area_queue_render(area);
}

/* ---- play a file ---- */
static void play_file(const char *path) {
  if (!path || !*path) return;
  const char *cmd[] = {"loadfile", path, NULL};
  mpv_command(mpv, cmd);
  is_paused = FALSE;
  if (pause_btn)
    gtk_button_set_label(GTK_BUTTON(pause_btn), "\xe2\x8f\xb8");
}

/* ---- toggle play/pause ---- */
static void on_pause_clicked(GtkButton *btn, gpointer d) {
  (void)d;
  is_paused = !is_paused;
  mpv_set_property_string(mpv, "pause", is_paused ? "yes" : "no");
  gtk_button_set_label(GTK_BUTTON(btn), is_paused ? "\xe2\x96\xb6" : "\xe2\x8f\xb8");
}

/* ---- mouse-motion tracking ---- */
static gboolean on_motion(GtkEventControllerMotion *ctrl,
                          gdouble x, gdouble y, gpointer d) {
  (void)ctrl; (void)x; (void)d;
  GtkWidget *win = GTK_WIDGET(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl)));
  int height = gtk_widget_get_height(win);
  gboolean show = (y >= height - 60);
  gtk_revealer_set_reveal_child(GTK_REVEALER(overlay_revealer), show);
  return FALSE;
}

/* ---- build the window ---- */
static void build_window(GApplication *a) {
  window = gtk_application_window_new(GTK_APPLICATION(a));
  gtk_window_set_title(GTK_WINDOW(window), "Audavi");
  gtk_window_set_default_size(GTK_WINDOW(window), 854, 480);
  gtk_window_maximize(GTK_WINDOW(window));

  /* CSS */
  GtkCssProvider *css = gtk_css_provider_new();
  gtk_css_provider_load_from_string(css,
    "window { background: black; }\n"
    ".overlay-bar {\n"
    "  background: rgba(20,20,25,0.88);\n"
    "  border-radius: 8px;\n"
    "  margin: 0 12px 12px 12px;\n"
    "  padding: 8px;\n"
    "  min-height: 40px;\n"
    "}\n"
    ".overlay-bar button {\n"
    "  background: transparent;\n"
    "  border: none;\n"
    "  color: #ccc;\n"
    "  font-size: 22px;\n"
    "  min-width: 44px;\n"
    "  min-height: 36px;\n"
    "  border-radius: 6px;\n"
    "}\n"
    ".overlay-bar button:hover {\n"
    "  background: rgba(255,255,255,0.08);\n"
    "  color: white;\n"
    "}"
  );
  gtk_style_context_add_provider_for_display(
    gdk_display_get_default(),
    GTK_STYLE_PROVIDER(css),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  /* layout: overlay */
  GtkWidget *stack = gtk_overlay_new();

  gl_area = gtk_gl_area_new();
  g_signal_connect(gl_area, "realize", G_CALLBACK(on_realize), NULL);
  g_signal_connect(gl_area, "render",  G_CALLBACK(on_render),  NULL);
  gtk_overlay_set_child(GTK_OVERLAY(stack), gl_area);

  /* hover bar */
  overlay_revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(overlay_revealer),
                                   GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
  gtk_revealer_set_transition_duration(GTK_REVEALER(overlay_revealer), 200);

  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(bar, "controls");
  gtk_widget_add_css_class(bar, "overlay-bar");
  gtk_widget_set_halign(bar, GTK_ALIGN_FILL);
  gtk_widget_set_valign(bar, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom(bar, 12);

  pause_btn = gtk_button_new_with_label("\xe2\x8f\xb8");
  g_signal_connect(pause_btn, "clicked", G_CALLBACK(on_pause_clicked), NULL);

  GtkWidget *btn_box = gtk_center_box_new();
  gtk_center_box_set_start_widget(GTK_CENTER_BOX(btn_box), pause_btn);
  gtk_box_append(GTK_BOX(bar), btn_box);
  gtk_revealer_set_child(GTK_REVEALER(overlay_revealer), bar);

  gtk_overlay_add_overlay(GTK_OVERLAY(stack), overlay_revealer);
  gtk_widget_set_halign(overlay_revealer, GTK_ALIGN_FILL);
  gtk_widget_set_valign(overlay_revealer, GTK_ALIGN_END);
  gtk_window_set_child(GTK_WINDOW(window), stack);

  /* mouse motion */
  GtkEventController *motion = GTK_EVENT_CONTROLLER(gtk_event_controller_motion_new());
  g_signal_connect(motion, "motion", G_CALLBACK(on_motion), NULL);
  gtk_widget_add_controller(window, motion);

  /* Register per-frame tick callback */
  gtk_widget_add_tick_callback(window, on_tick, NULL, NULL);

  g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);
  gtk_window_present(GTK_WINDOW(window));
}

/* ---- activate ---- */
static void on_activate(GApplication *a, gpointer d) {
  (void)d;
  if (window) return;
  build_window(a);
  if (pending_file) {
    play_file(pending_file);
    free(pending_file);
    pending_file = NULL;
  }
}

/* ---- open from file manager ---- */
static void on_open(GApplication *a, GFile **files, gint n,
                    const gchar *hint, gpointer d) {
  (void)a; (void)hint; (void)d;
  if (!window) build_window(a);
  if (n > 0 && files[0]) {
    char *path = g_file_get_path(files[0]);
    if (path) {
      if (pending_file) free(pending_file);
      pending_file = strdup(path);
      g_free(path);
      /* mpv is already initialized now, so play it */
      play_file(pending_file);
      free(pending_file);
      pending_file = NULL;
    }
  }
}

/* ---- entry point ---- */
int main(int argc, char *argv[]) {
  setlocale(LC_NUMERIC, "C");

  mpv = mpv_create();
  if (!mpv) { fprintf(stderr, "mpv_create failed\n"); return 1; }

  /* vo=libmpv prevents mpv from creating its own window */
  mpv_set_option_string(mpv, "vo", "libmpv");
  mpv_set_option_string(mpv, "input-default-bindings", "no");
  mpv_set_option_string(mpv, "osc",  "no");
  mpv_set_option_string(mpv, "osd-level", "0");
  mpv_set_option_string(mpv, "keep-open", "yes");
  mpv_set_option_string(mpv, "config", "no");
  mpv_set_option_string(mpv, "video-sync", "display-resample");

  if (mpv_initialize(mpv) < 0) { fprintf(stderr, "mpv init failed\n"); return 1; }

  if (argc > 1 && argv[1])
    pending_file = strdup(argv[1]);

  GtkApplication *app = gtk_application_new("com.audavi",
                             G_APPLICATION_HANDLES_OPEN);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  g_signal_connect(app, "open",     G_CALLBACK(on_open),     NULL);
  int st = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return st;
}
