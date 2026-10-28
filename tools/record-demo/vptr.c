/* lambdock -- hyper-hackable desktop dock for Wayland
 *
 * SPDX-License-Identifier: MIT
 * Copyright © Josep Bigorra <jjbigorra@gmail.com>
 */

/*
 * vptr -- a tiny deterministic pointer driver for the lambdock demo recorder.
 *
 * It speaks zwlr_virtual_pointer_unstable_v1 to the compositor in
 * $WAYLAND_DISPLAY and reads simple commands from stdin:
 *
 *   move X Y
 *   glide X Y MS
 *   click | rightclick | down | up | rdown | rup
 *   scroll N
 *   sleep MS
 *   quit
 *
 * Coordinates are logical compositor coordinates, with the origin at the
 * top-left of the output.  The output size is passed on the command line.
 *
 * This program is part of the demo-recording tooling, not of lambdock itself.
 */

#define _GNU_SOURCE

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <wayland-client.h>

#include "wlr-vptr.h"

static struct wl_display *g_dpy;
static struct zwlr_virtual_pointer_manager_v1 *manager;
static struct zwlr_virtual_pointer_v1 *pointer;
static struct wl_seat *seat;
static uint32_t extent_w = 1920;
static uint32_t extent_h = 1080;
static uint32_t start_ms;
static double cur_x, cur_y;

static uint32_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000 - start_ms);
}

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *iface,
                            uint32_t version) {
  (void)data;
  if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
    manager = wl_registry_bind(
        registry, name, &zwlr_virtual_pointer_manager_v1_interface,
        version < 2 ? version : 2);
  } else if (strcmp(iface, wl_seat_interface.name) == 0) {
    seat = wl_registry_bind(registry, name, &wl_seat_interface,
                            version < 5 ? version : 5);
  }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static double clamp(double v, double lo, double hi) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

static void motion_absolute(double x, double y) {
  x = clamp(x, 0, extent_w);
  y = clamp(y, 0, extent_h);
  zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(), (uint32_t)x,
                                          (uint32_t)y, extent_w, extent_h);
  zwlr_virtual_pointer_v1_frame(pointer);
  wl_display_flush(g_dpy);
}

static void button(uint32_t code, uint32_t state) {
  zwlr_virtual_pointer_v1_button(pointer, now_ms(), code, state);
  zwlr_virtual_pointer_v1_frame(pointer);
  wl_display_flush(g_dpy);
}

static void click_button(uint32_t code) {
  button(code, WL_POINTER_BUTTON_STATE_PRESSED);
  usleep(65000);
  button(code, WL_POINTER_BUTTON_STATE_RELEASED);
  usleep(45000);
}

static double ease_in_out_cubic(double t) {
  return t < 0.5 ? 4.0 * t * t * t : 1.0 - pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

static void glide(double x, double y, int duration_ms) {
  if (duration_ms <= 0)
    duration_ms = 1;
  int steps = duration_ms / 16;
  if (steps < 1)
    steps = 1;
  double sx = cur_x;
  double sy = cur_y;
  for (int i = 1; i <= steps; i++) {
    double t = ease_in_out_cubic((double)i / (double)steps);
    motion_absolute(sx + (x - sx) * t, sy + (y - sy) * t);
    usleep((useconds_t)(duration_ms * 1000 / steps));
  }
  cur_x = x;
  cur_y = y;
}

int main(int argc, char **argv) {
  if (argc >= 3) {
    extent_w = (uint32_t)atoi(argv[1]);
    extent_h = (uint32_t)atoi(argv[2]);
  }
  if (extent_w == 0 || extent_h == 0) {
    fprintf(stderr, "usage: vptr WIDTH HEIGHT < commands\n");
    return 2;
  }

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  start_ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

  g_dpy = wl_display_connect(NULL);
  if (!g_dpy) {
    fprintf(stderr, "vptr: cannot connect to Wayland (%s)\n",
            getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "?");
    return 1;
  }

  struct wl_registry *registry = wl_display_get_registry(g_dpy);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(g_dpy);

  if (!manager || !seat) {
    fprintf(stderr, "vptr: compositor lacks zwlr_virtual_pointer_v1/wl_seat\n");
    return 1;
  }

  pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, seat);
  if (!pointer) {
    fprintf(stderr, "vptr: could not create a virtual pointer\n");
    return 1;
  }
  wl_display_roundtrip(g_dpy);

  printf("READY %ux%u\n", extent_w, extent_h);
  fflush(stdout);

  char line[512];
  while (fgets(line, sizeof line, stdin)) {
    char cmd[32] = {0};
    double a = 0, b = 0;
    int n = 0;
    if (sscanf(line, "%31s %lf %lf %d", cmd, &a, &b, &n) < 1)
      continue;

    if (strcmp(cmd, "move") == 0) {
      cur_x = a;
      cur_y = b;
      motion_absolute(a, b);
    } else if (strcmp(cmd, "glide") == 0) {
      glide(a, b, n > 0 ? n : 500);
    } else if (strcmp(cmd, "click") == 0) {
      click_button(BTN_LEFT);
    } else if (strcmp(cmd, "rightclick") == 0) {
      click_button(BTN_RIGHT);
    } else if (strcmp(cmd, "down") == 0) {
      button(BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    } else if (strcmp(cmd, "up") == 0) {
      button(BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    } else if (strcmp(cmd, "rdown") == 0) {
      button(BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
    } else if (strcmp(cmd, "rup") == 0) {
      button(BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED);
    } else if (strcmp(cmd, "scroll") == 0) {
      int steps = (int)a;
      if (steps == 0)
        steps = 1;
      for (int i = 0; i < abs(steps); i++) {
        int32_t v = (steps > 0 ? 15 : -15);
        zwlr_virtual_pointer_v1_axis(pointer, now_ms(),
                                     WL_POINTER_AXIS_VERTICAL_SCROLL,
                                     wl_fixed_from_int(v));
        zwlr_virtual_pointer_v1_axis_discrete(
            pointer, now_ms(), WL_POINTER_AXIS_VERTICAL_SCROLL,
            wl_fixed_from_int(v), steps > 0 ? 1 : -1);
        zwlr_virtual_pointer_v1_frame(pointer);
        wl_display_flush(g_dpy);
        usleep(60000);
      }
    } else if (strcmp(cmd, "sleep") == 0) {
      usleep((useconds_t)(a * 1000));
    } else if (strcmp(cmd, "quit") == 0) {
      break;
    } else {
      fprintf(stderr, "vptr: unknown command '%s'\n", cmd);
    }
    fflush(stdout);
  }

  zwlr_virtual_pointer_v1_destroy(pointer);
  zwlr_virtual_pointer_manager_v1_destroy(manager);
  wl_display_disconnect(g_dpy);
  return 0;
}
