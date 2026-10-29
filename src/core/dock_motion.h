// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef LAMBDOCK_CORE_DOCK_MOTION_H
#define LAMBDOCK_CORE_DOCK_MOTION_H

#include <glib.h>

/*
 * =========================================================================
 * DOCK MOTION (PURE) -- slide easing, auto-hide offsets, fisheye magnification
 * =========================================================================
 * The arithmetic behind the dock's movement, as functions of values: no
 * widgets, no timers, no display, no GTK. `dock_window.c` keeps the GTK half
 * (frame callbacks, widget mutation) and asks in here for every number it
 * needs, which is what makes the shapes below testable -- and arguable --
 * without a running compositor.
 */

/**
 * lambdock_slide_progress:
 * @start_us: frame time the slide started at.
 * @now_us: current frame time.
 * @duration_us: how long the whole slide should take.
 *
 * Frame clocks are not guaranteed to be monotonic across a compositor
 * reconfigure, and a zero duration means "already finished", so the result is
 * clamped rather than trusted.
 *
 * Returns: the fraction of the slide completed, clamped to [0, 1].
 */
double lambdock_slide_progress(gint64 start_us, gint64 now_us,
                              gint64 duration_us);

/**
 * lambdock_ease_out_cubic:
 * @t: progress in [0, 1].
 *
 * Returns: 1 - (1 - t)^3 -- quick off the mark, gentle into the stop.
 */
double lambdock_ease_out_cubic(double t);

/**
 * lambdock_slide_position:
 * @from_px: margin the slide starts at.
 * @to_px: margin the slide ends at.
 * @progress: fraction of the slide completed, in [0, 1].
 *
 * Returns: the margin the dock should sit at on this frame.
 */
int lambdock_slide_position(int from_px, int to_px, double progress);

/**
 * AUTOHIDE_TRIGGER_STRIP_PX:
 * How much of a tucked-away dock stays on screen, in pixels: the strip the
 * pointer has to reach to bring the dock back.
 *
 * Two pixels is less than one pixel at 2x scale and under two at the 1.2x the
 * dock usually runs at, so a pointer that stops a hair short of the screen edge
 * gets nothing at all -- which reads as a dock that simply never comes back.
 * Four is still under half a text line, so it costs almost nothing in clicks an
 * application would otherwise have received, and it is all of the surface that
 * is visible while the dock is away (the strip is the top of the shelf's
 * magnification headroom, so it shows nothing).
 */
#define AUTOHIDE_TRIGGER_STRIP_PX 4

/**
 * lambdock_hide_margin:
 * @measured_main_px: the surface size measured along the dock's main axis.
 * @cross_axis_px: the cross-axis size the dock asks for.
 * @tolerance_px: how far past the cross-axis request a measurement may be
 *     before it is treated as stale.
 * @trigger_strip_px: how much of the dock stays on screen, so that it can be
 *     revealed again by the pointer.
 *
 * Before the first layout, and right after an orientation change, the measured
 * main-axis size can be 0 or a leftover value from the *other* axis; both are
 * unusable, and the cross-axis request is the best estimate available.
 *
 * Returns: the (negative) layer-shell margin that hides the dock.
 */
int lambdock_hide_margin(int measured_main_px, int cross_axis_px,
                         int tolerance_px, int trigger_strip_px);

/**
 * lambdock_settling:
 * @sliding: TRUE while a slide is in flight.
 * @settled_at_us: monotonic time the last slide stopped at (0: never slid).
 * @now_us: current monotonic time.
 * @settle_ms: how long after stopping crossings still count as echoes.
 *
 * A dock that is sliding -- or has only just stopped -- moves under a pointer
 * that is not moving, and compositors report the crossings that follow as if
 * the pointer had moved. This is the window in which those reports belong to
 * the dock's own movement, not to the user.
 *
 * Returns: TRUE while crossings should be ignored.
 */
gboolean lambdock_settling(gboolean sliding, gint64 settled_at_us,
                           gint64 now_us, guint settle_ms);

/**
 * lambdock_fisheye_scale:
 * @distance_px: distance from the pointer to the item's centre along the dock.
 * @radius_px: the pointer's influence radius (0 or less: no magnification).
 * @max_scale: scale at the centre of the influence, 1.0 or more.
 *
 * Returns: a cosine bell falling from @max_scale at the centre to 1.0 -- a
 * resting icon -- at @radius_px.
 */
/**
 * lambdock_dock_needs_full_input:
 * @is_hidden: TRUE while the dock is parked off screen.
 * @current_margin: the dock's current slide offset in pixels (0 = fully out).
 *
 * Returns: TRUE when the whole surface has to take input.
 *
 * A tucked-away dock (or one mid-slide) is only on screen as the trigger strip at
 * the screen edge, and that strip is the surface's *headroom* -- the part the
 * dock-shaped input region otherwise cuts away. Cutting it there would leave the
 * pointer nothing to enter, and auto-hide could never bring the dock back.
 */
gboolean lambdock_dock_needs_full_input(gboolean is_hidden, int current_margin);

double lambdock_fisheye_scale(double distance_px, double radius_px,
                              double max_scale);

/**
 * lambdock_scaled_size:
 * @base_px: resting size, in pixels.
 * @scale: magnification factor.
 *
 * Returns: @base_px * @scale, rounded to the nearest whole pixel.
 */
int lambdock_scaled_size(int base_px, double scale);

/**
 * lambdock_points_differ:
 * @ax: x of the first point.
 * @ay: y of the first point.
 * @bx: x of the second point.
 * @by: y of the second point.
 * @epsilon_px: how close the two points have to be to count as the same.
 *
 * Returns: TRUE when either coordinate moved by at least @epsilon_px.
 */
gboolean lambdock_points_differ(double ax, double ay, double bx, double by,
                                double epsilon_px);

/**
 * lambdock_size_step:
 * @size_px: a size to lay out, in pixels.
 * @step_px: layout granularity (1 = every pixel, 2 = every other, ...).
 *
 * Returns: @size_px rounded /up/ to the nearest multiple of @step_px, or
 * @size_px unchanged for a step of 1 or less.
 *
 * The fisheye paints through a transform, so the icon follows the pointer
 * continuously; only the *layout* has to step. Rounding the size request to a
 * few pixels means the dock re-measures its row a fraction as often while the
 * pointer moves, and nothing visible changes: the artwork is scaled smoothly
 * inside the box either way.
 */
int lambdock_size_step(int size_px, int step_px);

#endif // LAMBDOCK_CORE_DOCK_MOTION_H
