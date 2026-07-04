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
#include <stdio.h>

/* ---- state ---- */
static mpv_handle          *mpv;
static mpv_render_context  *mpv_gl;
static GtkWidget           *gl_area;
static GtkWidget           *overlay_revealer;
static GtkWidget           *pause_btn;
static GtkWidget           *skip_fwd_btn;
static GtkWidget           *skip_bwd_btn;
static GtkWidget           *next_btn;
static GtkWidget           *prev_btn;
static GtkWidget           *volume_knob;
static double               volume_level = 100.0;
static gboolean             volume_dragging = FALSE;
static GtkWidget           *seek_bar;
static gboolean             seek_bar_dragging = FALSE;
static gboolean             seek_bar_range_set = FALSE;
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

  /* Update seek bar (unless user is dragging it) */
  if (!seek_bar_dragging && seek_bar && mpv) {
    /* Set duration range once it becomes available */
    if (!seek_bar_range_set) {
      double dur = 0;
      mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &dur);
      if (dur > 0) {
        gtk_range_set_range(GTK_RANGE(seek_bar), 0, dur);
        gtk_range_set_increments(GTK_RANGE(seek_bar), 1, dur / 20);
        seek_bar_range_set = TRUE;
      }
    }
    /* Update position — expand range if duration unknown */
    {
      double pos;
      mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
      if (pos >= 0) {
        gtk_range_set_value(GTK_RANGE(seek_bar), pos);
        if (!seek_bar_range_set) {
          GtkAdjustment *adj = gtk_range_get_adjustment(GTK_RANGE(seek_bar));
          double max = gtk_adjustment_get_upper(adj);
          if (pos > max - 10)
            gtk_range_set_range(GTK_RANGE(seek_bar), 0, pos + 60);
        }
      }
    }
  }
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
  seek_bar_range_set = FALSE;
}

/* ---- load a file and add all videos from the same directory to playlist ---- */
static void play_file_with_folder(const char *path) {
  if (!path || !*path) return;

  char *dir = g_path_get_dirname(path);
  if (!dir) { play_file(path); return; }

  /* Load the primary file */
  const char *load_cmd[] = {"loadfile", path, "replace", NULL};
  mpv_command(mpv, load_cmd);

  /* Find all video files in the same directory and add to playlist */
  char find_cmd[4096];
  snprintf(find_cmd, sizeof(find_cmd),
    "find '%s' -maxdepth 1 -type f \\( -name '*.mp4' -o -name '*.mkv' -o -name '*.webm' "
    "-o -name '*.avi' -o -name '*.mov' \\) 2>/dev/null | sort",
    dir);

  FILE *fp = popen(find_cmd, "r");
  if (fp) {
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
      size_t len = strlen(line);
      if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
      if (len == 0 || strcmp(line, path) == 0) continue;
      const char *add_cmd[] = {"loadfile", line, "append", NULL};
      mpv_command(mpv, add_cmd);
    }
    pclose(fp);
  }

  g_free(dir);
  is_paused = FALSE;
  if (pause_btn)
    gtk_button_set_label(GTK_BUTTON(pause_btn), "\xe2\x8f\xb8");
  seek_bar_range_set = FALSE;
}

/* ---- toggle play/pause ---- */
static void on_pause_clicked(GtkButton *btn, gpointer d) {
  (void)d;
  is_paused = !is_paused;
  mpv_set_property_string(mpv, "pause", is_paused ? "yes" : "no");
  gtk_button_set_label(GTK_BUTTON(btn), is_paused ? "\xe2\x96\xb6" : "\xe2\x8f\xb8");
}

/* ---- skip forward 10 seconds ---- */
static void on_skip_forward(GtkButton *btn, gpointer d) {
  (void)btn; (void)d;
  const char *cmd[] = {"seek", "10", "relative", NULL};
  mpv_command(mpv, cmd);
}

/* ---- skip backward 10 seconds ---- */
static void on_skip_backward(GtkButton *btn, gpointer d) {
  (void)btn; (void)d;
  const char *cmd[] = {"seek", "-10", "relative", NULL};
  mpv_command(mpv, cmd);
}

/* ---- next/previous track ---- */
static void on_next(GtkButton *btn, gpointer d) {
  (void)btn; (void)d;
  if (seek_bar) {
    gtk_range_set_range(GTK_RANGE(seek_bar), 0, 100);
    gtk_range_set_value(GTK_RANGE(seek_bar), 0);
  }
  seek_bar_range_set = FALSE;
  const char *cmd[] = {"playlist-next", "weak", NULL};
  mpv_command(mpv, cmd);
}

static void on_prev(GtkButton *btn, gpointer d) {
  (void)btn; (void)d;
  if (seek_bar) {
    gtk_range_set_range(GTK_RANGE(seek_bar), 0, 100);
    gtk_range_set_value(GTK_RANGE(seek_bar), 0);
  }
  seek_bar_range_set = FALSE;
  const char *cmd[] = {"playlist-prev", "weak", NULL};
  mpv_command(mpv, cmd);
}

/* ---- mouse-motion tracking ---- */
static gboolean on_motion(GtkEventControllerMotion *ctrl,
                          gdouble x, gdouble y, gpointer d) {
  (void)ctrl; (void)x; (void)d;
  GtkWidget *win = GTK_WIDGET(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl)));
  int height = gtk_widget_get_height(win);
  gboolean show = (y >= height - 130);
  gtk_revealer_set_reveal_child(GTK_REVEALER(overlay_revealer), show);
  return FALSE;
}

/* ---- volume knob ---- */

/* Draw the volume knob using Cairo */
static void draw_volume_knob(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer d) {
  (void)area; (void)d;
  double size = w < h ? w : h;
  double cx = w / 2.0;
  double cy = h / 2.0;
  double radius = size / 2.0 - 4.0;

  /* Background circle */
  cairo_arc(cr, cx, cy, radius, 0, 2 * G_PI);
  cairo_set_source_rgba(cr, 0.3, 0.3, 0.35, 0.6);
  cairo_fill(cr);

  /* Volume arc — from -135° to +135° (bottom arc, like a real knob) */
  double start_angle = -0.75 * G_PI;   /* -135° */
  double end_angle   =  0.75 * G_PI;   /* +135° */
  double frac = volume_level / 100.0;
  double current_angle = start_angle + frac * (end_angle - start_angle);

  /* Active arc (from start to current) — coral color */
  cairo_set_line_width(cr, 3.0);
  cairo_arc(cr, cx, cy, radius - 2, start_angle, current_angle);
  cairo_set_source_rgb(cr, 1.0, 0.42, 0.42); /* #ff6b6b */
  cairo_stroke(cr);

  /* Inactive arc (from current to end) */
  cairo_arc(cr, cx, cy, radius - 2, current_angle, end_angle);
  cairo_set_source_rgba(cr, 0.6, 0.6, 0.65, 0.4);
  cairo_stroke(cr);

  /* Speaker icon */
  double icon_size = radius * 0.5;
  double s = icon_size;

  /* Speaker body (rectangle) */
  cairo_rectangle(cr, cx - s * 0.3, cy - s * 0.35, s * 0.45, s * 0.7);
  cairo_set_source_rgba(cr, 0.8, 0.8, 0.85, 0.7);
  cairo_fill(cr);

  /* Speaker cone (triangle on the right) */
  cairo_move_to(cr, cx + s * 0.15, cy - s * 0.3);
  cairo_line_to(cr, cx + s * 0.6, cy - s * 0.5);
  cairo_line_to(cr, cx + s * 0.6, cy + s * 0.5);
  cairo_line_to(cr, cx + s * 0.15, cy + s * 0.3);
  cairo_close_path(cr);
  cairo_fill(cr);

  /* If muted (volume 0), draw X across */
  if (volume_level < 0.5) {
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, cx, cy - s * 0.35);
    cairo_line_to(cr, cx + s * 0.5, cy + s * 0.35);
    cairo_move_to(cr, cx + s * 0.5, cy - s * 0.35);
    cairo_line_to(cr, cx, cy + s * 0.35);
    cairo_set_source_rgba(cr, 1.0, 0.42, 0.42, 0.8);
    cairo_stroke(cr);
  }

  /* Sound waves (showing there's audio) */
  if (volume_level > 0.5) {
    cairo_set_line_width(cr, 1.5);
    cairo_set_source_rgba(cr, 0.8, 0.8, 0.85, 0.5);
    /* Arc wave */
    cairo_arc(cr, cx + s * 0.5, cy, s * 0.15, -0.5, 0.5);
    cairo_stroke(cr);
  }
}

static void apply_volume(double val) {
  if (val < 0) val = 0;
  if (val > 100) val = 100;
  volume_level = val;
  char vol_str[16];
  snprintf(vol_str, sizeof(vol_str), "%.0f", val);
  mpv_set_property_string(mpv, "volume", vol_str);
  if (volume_knob)
    gtk_widget_queue_draw(volume_knob);
}

static gboolean on_volume_scroll(GtkEventControllerScroll *ctrl, gdouble dx, gdouble dy, gpointer d) {
  (void)ctrl; (void)dx; (void)d;
  apply_volume(volume_level - dy * 5);
  return GDK_EVENT_STOP;
}

static void on_volume_drag_begin(GtkGestureDrag *gesture, gdouble x, gdouble y, gpointer d) {
  (void)gesture; (void)x; (void)y; (void)d;
  volume_dragging = TRUE;
}

static void on_volume_drag_update(GtkGestureDrag *gesture, gdouble dx, gdouble dy, gpointer d) {
  (void)gesture; (void)dx;
  (void)d;
  if (volume_dragging)
    apply_volume(volume_level - dy * 0.5);
}

static void on_volume_drag_end(GtkGestureDrag *gesture, gdouble dx, gdouble dy, gpointer d) {
  (void)gesture; (void)dx; (void)dy; (void)d;
  volume_dragging = FALSE;
}

/* ---- seek bar ---- */
static void on_seek_value_changed(GtkRange *range, gpointer d) {
  (void)d;
  if (!seek_bar_dragging) return;
  double val = gtk_range_get_value(range);
  char val_str[64];
  snprintf(val_str, sizeof(val_str), "%.3f", val);
  const char *cmd[] = {"seek", val_str, "absolute", NULL};
  mpv_command(mpv, cmd);
}

static void on_seek_pressed(GtkGestureClick *gesture, gint n, gdouble x, gdouble y, gpointer d) {
  (void)gesture; (void)n; (void)x; (void)y; (void)d;
  seek_bar_dragging = TRUE;
}

static void on_seek_released(GtkGestureClick *gesture, gint n, gdouble x, gdouble y, gpointer d) {
  (void)gesture; (void)n; (void)x; (void)y; (void)d;
  /* Seek on release */
  if (seek_bar && mpv) {
    double val = gtk_range_get_value(GTK_RANGE(seek_bar));
    char val_str[64];
    snprintf(val_str, sizeof(val_str), "%.3f", val);
    const char *cmd[] = {"seek", val_str, "absolute", NULL};
    mpv_command(mpv, cmd);
  }
  seek_bar_dragging = FALSE;
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
    "  border-radius: 0px;\n"
    "  margin: 0;\n"
    "  padding: 8px;\n"
    "  min-height: 120px;\n"
    "}\n"
    ".overlay-bar button {\n"
    "  background: transparent;\n"
    "  border: none;\n"
    "  color: #ccc;\n"
    "  font-size: 18px;\n"
    "  min-width: 44px;\n"
    "  min-height: 36px;\n"
    "  border-radius: 6px;\n"
    "}\n"
    ".overlay-bar button:hover {\n"
    "  background: rgba(255,255,255,0.08);\n"
    "  color: white;\n"
    "}\n"
    ".skip-btn {\n"
    "  font-size: 16px;\n"
    "  font-weight: bold;\n"
    "}\n"
    ".volume-knob {\n"
    "  min-width: 40px;\n"
    "  min-height: 40px;\n"
    "  margin: 0 8px;\n"
    "}\n"
    "scale.seek-bar {\n"
    "  min-height: 6px;\n"
    "  margin: 0 8px;\n"
    "}\n"
    "scale.seek-bar trough {\n"
    "  min-height: 4px;\n"
    "  background: rgba(255,255,255,0.2);\n"
    "  border-radius: 2px;\n"
    "}\n"
    "scale.seek-bar highlight {\n"
    "  min-height: 4px;\n"
    "  background: #ff6b6b;\n"
    "  border-radius: 2px;\n"
    "}\n"
    "scale.seek-bar slider {\n"
    "  min-width: 12px;\n"
    "  min-height: 12px;\n"
    "  background: #ff6b6b;\n"
    "  border: none;\n"
    "  border-radius: 6px;\n"
    "  margin: -4px 0;\n"
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
  gtk_widget_set_margin_bottom(bar, 0);

  pause_btn = gtk_button_new_with_label("\xe2\x8f\xb8");
  g_signal_connect(pause_btn, "clicked", G_CALLBACK(on_pause_clicked), NULL);

  skip_fwd_btn = gtk_button_new_with_label("+10");
  gtk_widget_add_css_class(skip_fwd_btn, "skip-btn");
  g_signal_connect(skip_fwd_btn, "clicked", G_CALLBACK(on_skip_forward), NULL);

  skip_bwd_btn = gtk_button_new_with_label("-10");
  gtk_widget_add_css_class(skip_bwd_btn, "skip-btn");
  g_signal_connect(skip_bwd_btn, "clicked", G_CALLBACK(on_skip_backward), NULL);

  prev_btn = gtk_button_new_with_label("|<");
  gtk_widget_add_css_class(prev_btn, "skip-btn");
  g_signal_connect(prev_btn, "clicked", G_CALLBACK(on_prev), NULL);

  next_btn = gtk_button_new_with_label(">|");
  gtk_widget_add_css_class(next_btn, "skip-btn");
  g_signal_connect(next_btn, "clicked", G_CALLBACK(on_next), NULL);

  /* Volume knob */
  volume_knob = gtk_drawing_area_new();
  gtk_widget_add_css_class(volume_knob, "volume-knob");
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(volume_knob), draw_volume_knob, NULL, NULL);

  /* Scroll to change volume */
  GtkEventController *scroll = GTK_EVENT_CONTROLLER(gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES));
  g_signal_connect(scroll, "scroll", G_CALLBACK(on_volume_scroll), NULL);
  gtk_widget_add_controller(volume_knob, scroll);

  /* Drag to change volume */
  GtkGesture *drag = gtk_gesture_drag_new();
  g_signal_connect(drag, "drag-begin", G_CALLBACK(on_volume_drag_begin), NULL);
  g_signal_connect(drag, "drag-update", G_CALLBACK(on_volume_drag_update), NULL);
  g_signal_connect(drag, "drag-end", G_CALLBACK(on_volume_drag_end), NULL);
  gtk_widget_add_controller(volume_knob, GTK_EVENT_CONTROLLER(drag));

  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_halign(btn_box, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(btn_box), prev_btn);
  gtk_box_append(GTK_BOX(btn_box), skip_bwd_btn);
  gtk_box_append(GTK_BOX(btn_box), pause_btn);
  gtk_box_append(GTK_BOX(btn_box), skip_fwd_btn);
  gtk_box_append(GTK_BOX(btn_box), next_btn);
  gtk_box_append(GTK_BOX(bar), btn_box);
  /* Seek bar in the middle (replaces the spacer) */
  seek_bar = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 0.1);
  gtk_widget_add_css_class(seek_bar, "seek-bar");
  gtk_widget_set_hexpand(seek_bar, TRUE);
  gtk_range_set_slider_size_fixed(GTK_RANGE(seek_bar), FALSE);
  gtk_scale_set_draw_value(GTK_SCALE(seek_bar), FALSE);
  g_signal_connect(seek_bar, "value-changed", G_CALLBACK(on_seek_value_changed), NULL);

  /* Gesture for drag start/end */
  GtkGesture *seek_gesture = gtk_gesture_click_new();
  g_signal_connect(seek_gesture, "pressed", G_CALLBACK(on_seek_pressed), NULL);
  g_signal_connect(seek_gesture, "released", G_CALLBACK(on_seek_released), NULL);
  gtk_widget_add_controller(seek_bar, GTK_EVENT_CONTROLLER(seek_gesture));

  gtk_box_append(GTK_BOX(bar), seek_bar);
  gtk_box_append(GTK_BOX(bar), volume_knob);
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
    play_file_with_folder(pending_file);
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
      play_file_with_folder(pending_file);
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
