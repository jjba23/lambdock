// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The application: `src/main.c` in Rust, starting with the parts that are
//! decisions rather than wiring.
//!
//! `main.c` is the last C file in the project, and it is three kinds of code at
//! once: discovery (which settings files exist, which monitor they name), the
//! docks that follow from them, and the glue that starts GTK, Guile and the file
//! watchers. This module takes the discovery and its decisions first -- they are
//! values, so they are testable -- and the glue follows them file by file.
//!
//! The rule kept here: a `#[no_mangle]` entry point is thin and unsafe only where
//! it must be; the work happens in functions that take `&str`, `&[]` and return
//! values.

use gtk4::gdk;
use gtk4::glib;
use gtk4::glib::translate::{from_glib_none, IntoGlibPtr};
use gtk4::glib::variant::ToVariant;
use gtk4::glib::translate::IntoGlib;
use gtk4::prelude::*;
use std::cell::RefCell;
use std::os::raw::{c_char, c_double, c_int, c_uint, c_void};

use crate::cstr::{cstring, text_or_empty, translated};
use crate::ui::preferences::on_preferences_action;
use crate::dock_window::{
    on_about_action, on_quit_action,
    apply_dock_mode_classes, destroy_dock_instance, dock_apply_motion_preference,
    dock_window_set_tracker_connected, hide_dock, on_popover_closed, on_right_click,
    orientation_of, schedule_initial_autohide, setup_dock_motion_controller,
    setup_layer_shell_window, update_dock_dimensions, widget_of, window_of,
};
use crate::state::LambdockState;
use crate::ui::layer_shell::{self, EDGE_LEFT, EDGE_RIGHT};

/// `CONFIG_RELOAD_DEBOUNCE_MS` in `lambdock.h`: a write is several file events on
/// some backends, so the reload waits for them to stop.
const CONFIG_RELOAD_DEBOUNCE_MS: u32 = 150;

/// The names a settings file can have: `settings.scm`, and the per-dock
/// `settings-<name>.scm`.
const SETTINGS_STEM: &str = "settings";

/// The suffix every settings file carries.
const SETTINGS_EXTENSION: &str = "scm";

/// Whether `filename` is one of the dock's settings files.
///
/// `settings.scm` is the default dock; `settings-<name>.scm` is a named one.
/// Anything else -- a backup, an editor's swap file, a directory's other
/// content -- is not a dock.
fn is_settings_filename(filename: &str) -> bool {
    let Some(stem) = filename.strip_suffix(&format!(".{SETTINGS_EXTENSION}")) else {
        return false;
    };

    stem == SETTINGS_STEM || stem.starts_with(&format!("{SETTINGS_STEM}-"))
}

/// Whether `filename` is one of the dock's settings files.
///
/// # Safety
///
/// `filename` must be null or NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn is_settings_filename_c(filename: *const c_char) -> c_int {
    if filename.is_null() {
        return 0;
    }

    c_int::from(is_settings_filename(&text_or_empty(filename)))
}

/// The scroll offset a wheel event asks for.
///
/// One notch is thirty pixels, and the result is clamped to the scrollable band
/// so a wheel that keeps turning at the end of the row does not wind the
/// adjustment past its own bounds (GTK would clamp too, but the value is also
/// what a smooth-scroll animation would start from).
fn scrolled_value(value: f64, delta: f64, page_size: f64, upper: f64, lower: f64) -> f64 {
    let max_scroll = upper - page_size;

    (value + delta * 30.0).clamp(lower, max_scroll.max(lower))
}

/// Mouse wheel over the dock: scrolls the row along its main axis.
///
/// The wheel moves the dock's own viewport, not the window: a horizontal dock
/// scrolls on the dominant axis of the event, and a vertical one on `dy`, which
/// is what a wheel on a vertical dock sends.
///
/// # Safety
///
/// `user_data` must be null or a live dock with its scrolled window built.
#[no_mangle]
pub unsafe extern "C" fn on_dock_scroll(
    _controller: *mut gtk4::ffi::GtkEventControllerScroll,
    dx: c_double,
    dy: c_double,
    user_data: *mut std::os::raw::c_void,
) -> c_int {
    let Some(dock) = user_data.cast::<crate::state::LambdockState>().as_ref() else {
        return 0;
    };
    if dock.scrolled_window.is_null() {
        return 0;
    }

    let scrolled: gtk4::ScrolledWindow = gtk4::glib::translate::from_glib_none(dock.scrolled_window.cast());

    let horizontal = dock.orientation == gtk4::Orientation::Horizontal.into_glib();
    let adjustment = if horizontal {
        scrolled.hadjustment()
    } else {
        scrolled.vadjustment()
    };

    if dock.scrolled_window.is_null() {
        return 0;
    }

    // A horizontal dock is scrolled by whichever way the wheel went; a vertical
    // one only by the vertical component.
    let delta = if horizontal && dx.abs() > dy.abs() { dx } else { dy };

    let value = scrolled_value(
        adjustment.value(),
        delta,
        adjustment.page_size(),
        adjustment.upper(),
        adjustment.lower(),
    );
    adjustment.set_value(value);

    c_int::from(true)
}

/// The monitor whose connector is `connector`, if the display has one.
///
/// # Safety
///
/// `connector` must be null or NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn find_monitor_by_connector(
    connector: *const c_char,
) -> *mut gdk::ffi::GdkMonitor {
    if connector.is_null() {
        return std::ptr::null_mut();
    }

    let Some(connector) = crate::cstr::non_empty_text(connector) else {
        return std::ptr::null_mut();
    };

    let Some(display) = gdk::Display::default() else {
        return std::ptr::null_mut();
    };

    let monitors = display.monitors();
    for index in 0..monitors.n_items() {
        let Some(item) = monitors.item(index) else {
            continue;
        };
        let Some(monitor) = item.downcast_ref::<gdk::Monitor>() else {
            continue;
        };

        if monitor.connector().as_deref() == Some(connector.as_str()) {
            // `(transfer full)`: the caller owns the reference it asked for.
            return glib::gobject_ffi::g_object_ref(monitor.as_ptr().cast()).cast();
        }
    }

    std::ptr::null_mut()
}

// ===========================================================================
// DIRECTORY DISCOVERY
// ===========================================================================

/// The application's own directory under a GLib-provided base directory.
fn app_dir(base: std::path::PathBuf) -> std::path::PathBuf {
    base.join("lambdock")
}

/// `~/.config/lambdock`: the settings files.
fn config_dir() -> std::path::PathBuf {
    app_dir(glib::user_config_dir())
}

/// `~/.local/state/lambdock`: the runtime state the settings are layered with.
///
/// The preferences the Preferences window remembers and the layout drag and drop
/// writes live here, not in the config directory: they are runtime state, and
/// `(lambdock layout)` and `(lambdock prefs)` name the same place.
fn state_dir() -> std::path::PathBuf {
    // GLib grew `g_get_user_state_dir` in 2.72 and the vendored bindings do not
    // expose it, so the XDG rule it implements is written out here: an absolute
    // `XDG_STATE_HOME`, else `~/.local/state`.
    let base = std::env::var_os("XDG_STATE_HOME")
        .map(std::path::PathBuf::from)
        .filter(|path| path.is_absolute())
        .or_else(|| {
            std::env::var_os("HOME")
                .map(|home| std::path::PathBuf::from(home).join(".local").join("state"))
        })
        .unwrap_or_default();

    app_dir(base)
}

/// The settings files in `directory`, as absolute paths, sorted by name.
///
/// Sorted because a dock is spawned per file: a directory listing's order is the
/// filesystem's business, and the docks should appear in the same order on every
/// run. A directory that cannot be read has no settings files -- a fresh install
/// has not made one yet -- which is the same answer the C's `g_dir_open` gave.
fn settings_files_in(directory: &std::path::Path) -> Vec<std::path::PathBuf> {
    let Ok(entries) = std::fs::read_dir(directory) else {
        return Vec::new();
    };

    let mut files: Vec<std::path::PathBuf> = entries
        .filter_map(|entry| entry.ok())
        .filter(|entry| {
            entry
                .file_name()
                .to_str()
                .is_some_and(is_settings_filename)
        })
        .map(|entry| directory.join(entry.file_name()))
        .collect();

    files.sort();

    files
}

/// Every settings file in the dock's configuration directory.
fn settings_files() -> Vec<std::path::PathBuf> {
    settings_files_in(&config_dir())
}

/// A path as a `(transfer full)` C string.
fn dup_path(path: &std::path::Path) -> *mut c_char {
    unsafe { crate::glib::g_strdup(cstring(&path.to_string_lossy()).as_ptr()) }
}

/// `~/.config/lambdock`, for the C half. `(transfer full)`.
#[no_mangle]
pub extern "C" fn get_config_dir_c() -> *mut c_char {
    dup_path(&config_dir())
}

/// `~/.local/state/lambdock`, for the C half. `(transfer full)`.
#[no_mangle]
pub extern "C" fn get_state_dir_c() -> *mut c_char {
    dup_path(&state_dir())
}

/// The settings files, as a `GList` of `g_strdup`ed paths the caller frees with
/// `g_list_free_full(list, g_free)`.
#[no_mangle]
pub extern "C" fn scan_settings_files() -> *mut crate::app_window::GList {
    extern "C" {
        fn g_list_append(list: *mut crate::app_window::GList, data: *mut c_void) -> *mut crate::app_window::GList;
    }

    let mut list: *mut crate::app_window::GList = std::ptr::null_mut();

    for path in settings_files() {
        unsafe { list = g_list_append(list, dup_path(&path).cast()) };
    }

    list
}

// ===========================================================================
// SMALL ACTION HANDLERS
// ===========================================================================

/// Every dock re-reads the desktop's motion setting.
extern "C" fn apply_motion_visitor(state: *mut LambdockState, _data: *mut c_void) {
    unsafe { dock_apply_motion_preference(state) };
}

/// The desktop's "reduce motion" (GTK's `gtk-enable-animations`) can change while
/// the dock is running, so every dock re-reads it: the next slide or hover is
/// then immediate instead of animated.
///
/// The arguments are GLib's own -- the settings object, the property spec and the
/// data the handler was connected with -- and all three are ignored.
///
/// # Safety
///
/// Connected as a `notify::` handler, so it is called from GLib.
#[no_mangle]
pub unsafe extern "C" fn on_system_motion_changed(
    _settings: *mut c_void,
    _pspec: *mut c_void,
    _user_data: *mut c_void,
) {
    crate::dock_registry::dock_registry_foreach(Some(apply_motion_visitor), std::ptr::null_mut());
}

/// Writes the dock's resolved configuration as settings-file text and opens it in
/// the user's editor, so "what is my dock actually doing" is answerable without a
/// REPL.
///
/// # Safety
///
/// `parameter` must be null or a string `GVariant` holding the dock's settings
/// path, "" for the fallback dock.
#[no_mangle]
pub unsafe extern "C" fn on_export_settings_action(
    _action: *mut gtk4::gio::ffi::GSimpleAction,
    parameter: *mut glib::ffi::GVariant,
    _user_data: *mut c_void,
) {
    let config_path = if parameter.is_null() {
        std::ptr::null()
    } else {
        glib::ffi::g_variant_get_string(parameter, std::ptr::null_mut())
    };

    let written = crate::bridge::export_dock_settings_to_file(config_path);
    if written.is_null() {
        crate::log_warn!("Main", "Could not capture the dock's settings");
        return;
    }

    let path = crate::cstr::text_or_empty(written);
    crate::log_info!("Main", "Captured the running dock in {path}");

    if crate::ui::desktop_entry::lambdock_open_path(written) == 0 {
        crate::log_warn!("Main", "Could not open {path}");
    }

    crate::glib::g_free(written.cast());
}

// ===========================================================================
// BUILDING ONE DOCK
// ===========================================================================
//
// Creating a dock is a sequence of steps: read the settings, build the widget
// tree, build the menu, install the input, then show it. Each step is its own
// function, so a step can be read without holding the whole construction in your
// head.
//
// One ownership rule runs through all of them, and it is the reason this file is
// `unsafe` at all: the state holds *raw pointers* to its widgets, because the C
// header, the ABI and the tests all read them from there. So a widget is either
// parented inside the step that built it -- the tree holds the only reference
// that matters, and the wrapper may drop -- or it is handed over floating
// (`keep_floating`), so the step that adopts it later sinks that reference and
// nothing leaks. A `GObject` that is not a widget (the size group) is handed over
// owning its reference instead (`keep_owned`), which is what teardown unrefs.

/// Hands `widget` to the state as a floating reference, for a later parent to
/// sink.
fn keep_floating(widget: gtk4::Widget) -> *mut c_void {
    unsafe { glib::gobject_ffi::g_object_force_floating(widget.as_ptr().cast()) };

    let pointer: *mut gtk4::ffi::GtkWidget = widget.into_glib_ptr();

    pointer.cast()
}

/// Hands a `GObject` to the state, which owns the reference from here on.
fn keep_owned(object: glib::Object) -> *mut c_void {
    let pointer: *mut glib::gobject_ffi::GObject = object.into_glib_ptr();

    pointer.cast()
}

/// Called by GDK when a monitor is unplugged: unbinds layer shell from the dead
/// output immediately, before the next dispatch cycle fails.
unsafe fn on_monitor_invalidated(state: *mut LambdockState) {
    let Some(dock) = state.as_ref() else {
        return;
    };
    if dock.is_destroying != 0 {
        return;
    }

    let path = crate::cstr::text_or_empty(dock.config_path);
    crate::log_info!(
        "Main",
        "Monitor invalidated for dock instance [{}]. Unbinding layer shell output...",
        if path.is_empty() { "default" } else { path.as_str() }
    );

    if let Some(window) = window_of(dock) {
        if layer_shell::is_layer_window(&*window) {
            layer_shell::set_monitor(&window, std::ptr::null_mut());
        }
    }
}

/// The scrolling content viewport: the item box inside a scroller.
///
/// # Safety
///
/// `dock` must be a live dock under construction.
unsafe fn build_viewport(dock: &mut LambdockState) -> *mut c_void {
    let scrolled = gtk4::ScrolledWindow::new();
    scrolled.set_overflow(gtk4::Overflow::Visible);
    scrolled.set_propagate_natural_width(true);
    scrolled.set_propagate_natural_height(true);

    let box_ = gtk4::Box::new(orientation_of(dock), dock.item_spacing);
    box_.set_overflow(gtk4::Overflow::Visible);
    // Parented right here: the scroller owns it from now on.
    scrolled.set_child(Some(&box_));
    dock.box_ = box_.as_ptr().cast();

    // The viewport only ever scrolls along the dock's main axis.
    if orientation_of(dock) == gtk4::Orientation::Horizontal {
        scrolled.set_policy(gtk4::PolicyType::External, gtk4::PolicyType::Never);
    } else {
        scrolled.set_policy(gtk4::PolicyType::Never, gtk4::PolicyType::External);
    }

    let scroll = gtk4::EventControllerScroll::new(gtk4::EventControllerScrollFlags::BOTH_AXES);
    let pointer = dock as *mut LambdockState;
    scroll.connect_scroll(move |_, dx, dy| {
        if on_dock_scroll(std::ptr::null_mut(), dx, dy, pointer.cast()) != 0 {
            glib::Propagation::Stop
        } else {
            glib::Propagation::Proceed
        }
    });
    scrolled.add_controller(scroll);

    keep_floating(scrolled.upcast())
}

/// The capsule (and its shelf frame) inside the window's overlay.
///
/// # Safety
///
/// `dock` must be a live dock with its window built.
unsafe fn build_capsule(dock: &mut LambdockState) -> *mut c_void {
    let Some(window) = window_of(dock) else {
        return std::ptr::null_mut();
    };

    let overlay = gtk4::Overlay::new();
    overlay.set_overflow(gtk4::Overflow::Visible);
    window.set_child(Some(&overlay));
    dock.overlay = overlay.as_ptr().cast();

    let capsule = gtk4::Box::new(orientation_of(dock), 0);
    capsule.add_css_class("lambdock-window");
    dock.bg_capsule = capsule.as_ptr().cast();

    // Wrap the styled capsule in the shelf frame, which parents the capsule.
    // With the default shelf parameters (no tilt) the frame passes through.
    let frame = crate::ui::shelf::shelf_frame_new(capsule.as_ptr().cast());
    apply_dock_mode_classes(dock as *mut LambdockState);

    if !frame.is_null() {
        let frame: gtk4::Widget = from_glib_none(frame);
        frame.set_overflow(gtk4::Overflow::Visible);
        dock.bg_shelf = frame.as_ptr().cast();
        keep_floating(frame)
    } else {
        std::ptr::null_mut()
    }
}

/// Window -> overlay -> [shelf frame( capsule ), scroller].
///
/// # Safety
///
/// `dock` must be a live dock with its window and settings read.
unsafe fn build_widgets(dock: &mut LambdockState) {
    let Some(window) = window_of(dock) else {
        return;
    };

    // The overlay and the shelf frame come from this step, as in the C: the frame
    // was handed over floating, so adopting it here is what sinks it.
    let frame_pointer = build_capsule(dock);

    let overlay_pointer = dock.overlay;
    if overlay_pointer.is_null() {
        return;
    }
    let overlay: gtk4::Overlay = from_glib_none(overlay_pointer.cast());

    if !frame_pointer.is_null() {
        let frame: gtk4::Widget = from_glib_none(frame_pointer.cast());
        overlay.set_child(Some(&frame));
    }

    let scrolled_pointer = build_viewport(dock);
    if scrolled_pointer.is_null() {
        return;
    }
    let scrolled: gtk4::ScrolledWindow = from_glib_none(scrolled_pointer.cast());
    dock.scrolled_window = scrolled_pointer;
    overlay.add_overlay(&scrolled);

    update_dock_dimensions(dock as *mut LambdockState);

    let mode = match orientation_of(dock) {
        gtk4::Orientation::Horizontal => gtk4::SizeGroupMode::Horizontal,
        _ => gtk4::SizeGroupMode::Vertical,
    };
    let size_group = gtk4::SizeGroup::new(mode);
    size_group.add_widget(&scrolled);
    if let Some(capsule) = widget_of(dock.bg_capsule) {
        size_group.add_widget(&*capsule);
    }
    dock.size_group = keep_owned(size_group.upcast());

    // The compositor restarting drops the foreign-toplevel proxies; this says so
    // while the tracker re-binds, and is hidden the rest of the time.
    let status = crate::core::tracker_rules::lambdock_tracker_status_text(0);
    let label = gtk4::Label::new(crate::cstr::text(status).as_deref());
    label.add_css_class("dock-reconnecting");
    label.set_halign(gtk4::Align::Center);
    label.set_valign(gtk4::Align::Start);
    label.set_can_target(false);
    label.set_visible(false);
    overlay.add_overlay(&label);
    dock.reconnect_label = label.as_ptr().cast();

    if let Some(box_) = widget_of(dock.box_) {
        box_.set_overflow(gtk4::Overflow::Visible);
    }
    window.set_overflow(gtk4::Overflow::Visible);
}

/// The window's own context menu: Preferences (for this dock), Save settings,
/// About and Quit.
///
/// # Safety
///
/// `dock` must be a live dock with its window built.
unsafe fn build_menu_popover(dock: &mut LambdockState) {
    let Some(window) = window_of(dock) else {
        return;
    };

    let path = crate::cstr::text_or_empty(dock.config_path);
    let target = path.to_variant();

    let menu = gtk4::gio::Menu::new();

    // Both of the first two carry the dock's settings path as their action
    // target: with several settings files in use, the menu must act on the dock
    // it was right-clicked on, not on an arbitrary one.
    let preferences = gtk4::gio::MenuItem::new(Some(&translated(c"Preferences…")), None);
    preferences.set_action_and_target_value(Some("app.preferences"), Some(&target));
    menu.append_item(&preferences);

    // Capture what this dock resolves to, GUI and drag-and-drop edits included,
    // as a settings file. A bug report can carry it; a user can read it.
    let export = gtk4::gio::MenuItem::new(Some(&translated(c"Save settings.scm…")), None);
    export.set_action_and_target_value(Some("app.export-settings"), Some(&target));
    menu.append_item(&export);

    menu.append(Some(&translated(c"About Lambdock")), Some("app.about"));
    menu.append(Some(&translated(c"Quit")), Some("app.quit"));

    let popover = gtk4::PopoverMenu::from_model(Some(&menu));
    popover.set_parent(&*window);
    popover.set_has_arrow(false);
    crate::ui::popover_menu::lambdock_popover_close_on_escape(popover.as_ptr().cast());

    let pointer = dock as *mut LambdockState;
    popover.connect_closed(move |popover| {
        on_popover_closed(popover.as_ptr().cast(), pointer.cast());
    });

    dock.popover = popover.as_ptr().cast();
}

/// Right click, drag and drop, motion tracking and keyboard navigation.
///
/// # Safety
///
/// `dock` must be a live dock with its window built.
unsafe fn build_input(dock: &mut LambdockState) {
    let Some(window) = window_of(dock) else {
        return;
    };

    let pointer = dock as *mut LambdockState;
    let right_click = gtk4::GestureClick::new();
    right_click.set_button(gtk4::gdk::BUTTON_SECONDARY);
    right_click.connect_pressed(move |gesture, n_press, x, y| {
        on_right_click(gesture.as_ptr().cast(), n_press, x, y, pointer.cast());
    });
    window.add_controller(right_click);

    setup_dock_motion_controller(dock as *mut LambdockState);
    crate::ui::dock_dnd::dock_dnd_setup(dock as *mut LambdockState);

    // Keyboard access: arrows walk the icons, Menu opens their context menu,
    // Escape closes whatever the dock has open, Ctrl+Arrow rearranges them.
    crate::dock_item::lambdock_dock_attach_key_navigation(
        window.as_ptr().cast(),
        dock as *mut LambdockState,
    );
}

/// Shows the dock and puts it under window tracking and auto-hide.
///
/// # Safety
///
/// `dock` must be a live dock with its widgets built.
unsafe fn present(dock: *mut LambdockState) {
    let Some(state) = dock.as_ref() else {
        return;
    };
    let Some(window) = window_of(state) else {
        return;
    };

    setup_layer_shell_window(dock);

    // A dock that starts with reduced motion must not run a first slide.
    dock_apply_motion_preference(dock);

    crate::dock_registry::dock_registry_add(dock);

    let pointer = dock;
    window.connect_destroy(move |_| destroy_dock_instance(pointer));

    window.present();

    // Window tracking is a bonus, not a requirement: say why it is unavailable
    // and carry on with the pinned items.
    let mut error: *mut glib::ffi::GError = std::ptr::null_mut();
    if crate::toplevel_tracker::init_toplevel_tracker(&mut error) != 0 {
        // A dock created during a compositor restart starts reconnecting,
        // without waiting for the next tracker event to tell it.
        dock_window_set_tracker_connected(
            dock,
            crate::toplevel_tracker::lambdock_tracker_connected() != 0,
        );
    } else {
        let message = crate::cstr::text((*error).message).unwrap_or_default();
        crate::log_warn!("Tracker", "window tracking unavailable: {message}");
    }

    crate::toplevel_tracker::sync_window_trackers(dock);

    let state = &*dock;
    if state.auto_hide != 0 && state.show_when_clear != 0 && !state.window_trackers.is_null() {
        hide_dock(dock);
    } else {
        schedule_initial_autohide(dock);
    }
}

/// Constructs and displays one dock, on `monitor` and for `config_path`.
///
/// # Safety
///
/// `app` must be the live application; `monitor` must be null or a live
/// `GdkMonitor`; `config_path` must be null or NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn create_dock_instance(
    app: *mut gtk4::ffi::GtkApplication,
    monitor: *mut gdk::ffi::GdkMonitor,
    config_path: *const c_char,
) {
    if app.is_null() {
        return;
    }

    // Allocated with GLib, like the C did, because `destroy_dock_instance` frees
    // it with `g_free`.
    let memory = crate::glib::g_malloc0(std::mem::size_of::<LambdockState>());
    let state = memory.cast::<LambdockState>();
    let dock = &mut *state;

    if !monitor.is_null() {
        glib::gobject_ffi::g_object_ref(monitor.cast());
        dock.monitor = monitor.cast();

        // Listen for this monitor disappearing out from under the dock.
        let monitor: gdk::Monitor = from_glib_none(monitor);
        let pointer = state;
        monitor.connect_invalidate(move |_| on_monitor_invalidated(pointer));
    }

    dock.config_path = match crate::cstr::text(config_path) {
        Some(path) => cstring(&path).into_raw(),
        None => std::ptr::null_mut(),
    };

    dock.edge = crate::bridge::get_position_config_for(dock.config_path);
    dock.orientation = if dock.edge == EDGE_LEFT || dock.edge == EDGE_RIGHT {
        gtk4::Orientation::Vertical.into_glib()
    } else {
        gtk4::Orientation::Horizontal.into_glib()
    };

    // One reader for the whole settings file, shared with the reload path.
    crate::bridge::dock_read_settings(state);

    let app: gtk4::Application = from_glib_none(app);
    let window = gtk4::ApplicationWindow::new(&app);
    // A window without a title is announced as an unnamed frame; name the dock
    // so a screen reader says what surface the focus just landed on.
    window.set_title(Some(&translated(c"Lambdock dock")));
    dock.window = window.as_ptr().cast();

    // The theme stylesheet needs the window's CSS context, hence this order.
    crate::ui::theme_engine::load_custom_css(state);

    build_widgets(dock);
    crate::bridge::populate_dock_items_for(state);
    build_menu_popover(dock);
    build_input(dock);
    present(state);

    // The application owns the window from here on; the state keeps the pointer
    // the C header and the tests read.
    let _: *mut gtk4::ffi::GtkApplicationWindow = window.into_glib_ptr();
}

// ===========================================================================
// THE RECONCILE PASS
// ===========================================================================
//
// Reconciling is two steps, and the first one is a value: given the settings
// files on disk, the docks that are registered and the monitors that exist, work
// out which docks to destroy, which to reload and where to spawn new ones. That
// decision takes no widgets -- `plan_for_file` below is tested without a display
// -- and the second step applies it to the registry.

/// One dock, as the reconcile pass sees it: the settings path it was built for
/// (`None` for the fallback dock) and the monitor it was put on, as an address.
#[derive(Clone, Debug, PartialEq, Eq)]
struct DockFacts {
    config: Option<String>,
    monitor: Option<usize>,
}

/// A settings file's monitor policy, read from its `dock-monitor` string.
#[derive(Clone, Debug, PartialEq, Eq)]
enum MonitorPolicy {
    /// `all`: one dock per active monitor.
    All,
    /// `default`, empty or missing: a single dock that follows no monitor.
    Default,
    /// A named connector: exactly one dock, on that monitor.
    Connector(String),
}

impl MonitorPolicy {
    fn from_config(dock_monitor: Option<&str>) -> Self {
        match dock_monitor {
            Some("all") => Self::All,
            Some(name) if !name.is_empty() && name != "default" => Self::Connector(name.to_owned()),
            _ => Self::Default,
        }
    }
}

/// What one settings file needs done, as indices into the snapshot it was
/// computed from.
#[derive(Debug, Default, PartialEq, Eq)]
struct FilePlan {
    destroy: Vec<usize>,
    reload: Vec<usize>,
    /// Monitors to spawn on; `None` is "no monitor", the fallback dock.
    spawn: Vec<Option<usize>>,
}

/// What `file` needs, given the docks registered and the monitors that exist.
fn plan_for_file(
    file: &str,
    policy: &MonitorPolicy,
    target: Option<usize>,
    docks: &[DockFacts],
    monitors: &[usize],
) -> FilePlan {
    let mut plan = FilePlan::default();
    let is_for_file = |dock: &DockFacts| dock.config.as_deref() == Some(file);

    match policy {
        MonitorPolicy::All => {
            // A dock on a monitor that is gone is stranded; destroying it is what
            // makes room for its replacement below.
            for (index, dock) in docks.iter().enumerate().filter(|(_, dock)| is_for_file(dock)) {
                let live = dock.monitor.is_some_and(|monitor| monitors.contains(&monitor));
                if !live {
                    plan.destroy.push(index);
                }
            }

            for monitor in monitors {
                let existing = docks.iter().enumerate().find(|(index, dock)| {
                    is_for_file(dock) && dock.monitor == Some(*monitor) && !plan.destroy.contains(index)
                });

                match existing {
                    Some((index, _)) => plan.reload.push(index),
                    None => plan.spawn.push(Some(*monitor)),
                }
            }
        }
        MonitorPolicy::Connector(_) => {
            // A connector that does not exist is documented to fall back to the
            // default monitor, which is the `Default` policy's work.
            let Some(target) = target else {
                return plan_for_file(file, &MonitorPolicy::Default, None, docks, monitors);
            };

            for (index, dock) in docks.iter().enumerate().filter(|(_, dock)| is_for_file(dock)) {
                let stranded = dock.monitor.is_some_and(|monitor| !monitors.contains(&monitor));
                if dock.monitor != Some(target) || stranded {
                    plan.destroy.push(index);
                }
            }

            let existing = docks.iter().enumerate().find(|(index, dock)| {
                is_for_file(dock) && dock.monitor == Some(target) && !plan.destroy.contains(index)
            });

            match existing {
                Some((index, _)) => plan.reload.push(index),
                None => plan.spawn.push(Some(target)),
            }
        }
        MonitorPolicy::Default => {
            // A dock on any monitor is wrong for this file...
            for (index, dock) in docks.iter().enumerate().filter(|(_, dock)| is_for_file(dock)) {
                if dock.monitor.is_some() {
                    plan.destroy.push(index);
                }
            }

            // ...and exactly one dock without a monitor is wanted.
            let existing = docks.iter().enumerate().find(|(index, dock)| {
                is_for_file(dock) && dock.monitor.is_none() && !plan.destroy.contains(index)
            });

            match existing {
                Some((index, _)) => plan.reload.push(index),
                None => plan.spawn.push(None),
            }
        }
    }

    plan
}

/// The docks registered right now.
fn registry_snapshot() -> Vec<*mut LambdockState> {
    extern "C" fn collect(state: *mut LambdockState, data: *mut c_void) {
        unsafe { &mut *data.cast::<Vec<*mut LambdockState>>() }.push(state);
    }

    let mut docks: Vec<*mut LambdockState> = Vec::new();
    unsafe {
        crate::dock_registry::dock_registry_foreach(
            Some(collect),
            (&mut docks as *mut Vec<*mut LambdockState>).cast(),
        )
    };

    docks
}

/// The live monitors, in the display's own order.
fn live_monitors() -> Vec<gdk::Monitor> {
    let Some(display) = gdk::Display::default() else {
        return Vec::new();
    };

    let monitors = display.monitors();
    (0..monitors.n_items())
        .filter_map(|index| monitors.item(index))
        .filter_map(|item| item.downcast::<gdk::Monitor>().ok())
        .collect()
}

/// The address that names `monitor` in a plan.
fn address_of(monitor: &gdk::Monitor) -> usize {
    monitor.as_ptr() as usize
}

/// Applies one file's plan: destroy, reload, then spawn -- in that order, because
/// a destroy is what makes room for a spawn on the same monitor.
///
/// # Safety
///
/// `docks` are the pointers `registry_snapshot` handed out, still live, and
/// `monitors` are the ones the plan was computed from.
unsafe fn apply_plan(
    app: &gtk4::Application,
    file: &str,
    docks: &[*mut LambdockState],
    monitors: &[gdk::Monitor],
    plan: &FilePlan,
) {
    for index in &plan.destroy {
        if let Some(dock) = docks.get(*index) {
            destroy_dock_instance(*dock);
        }
    }

    for index in &plan.reload {
        if let Some(dock) = docks.get(*index) {
            crate::dock_item::reload_dock_instance(*dock);
        }
    }

    for monitor in &plan.spawn {
        let monitor: *mut gdk::ffi::GdkMonitor = monitor
            .and_then(|address| {
                monitors
                    .iter()
                    .find(|candidate| address_of(candidate) == address)
            })
            .map_or(std::ptr::null_mut(), |monitor| {
                monitor.as_ptr().cast::<gdk::ffi::GdkMonitor>()
            });

        if monitor.is_null() {
            crate::log_info!("Main", "Spawning single dock instance ({file})...");
        } else {
            crate::log_info!("Main", "New monitor detected. Spawning dock instance ({file})...");
        }

        create_dock_instance(app.as_ptr().cast(), monitor, cstring(file).as_ptr());
    }
}

/// Reconciles one settings file with the docks and monitors that exist.
///
/// # Safety
///
/// `app` must be the live application.
unsafe fn reconcile_settings_file(app: &gtk4::Application, file: &str) {
    let file_c = cstring(file);
    crate::bridge::load_dock_config_scm(file_c.as_ptr());

    // The dock's own REPL, when the module provides one.
    // The dock's own REPL, when `(lambdock core)` provides one.
    let start_repl = crate::guile::scm_c_public_ref(c"lambdock core".as_ptr(), c"start-repl!".as_ptr());
    if !start_repl.is_null() {
        crate::guile::scm_call_1(start_repl, crate::guile::scm_from_utf8_string(file_c.as_ptr()));
    }

    let policy = MonitorPolicy::from_config(
        crate::cstr::text(crate::bridge::get_monitor_config_for(file_c.as_ptr())).as_deref(),
    );

    let monitors = live_monitors();
    let addresses: Vec<usize> = monitors.iter().map(address_of).collect();

    // A named connector resolves to a monitor; one that does not exist says so
    // once and falls back, which the policy below already knows how to do.
    let target = match &policy {
        MonitorPolicy::Connector(name) => {
            let found = monitors
                .iter()
                .find(|monitor| monitor.connector().as_deref() == Some(name.as_str()));

            if found.is_none() {
                crate::log_warn!(
                    "Main",
                    "Monitor connector '{name}' not found for [{file}]; falling back to the default monitor."
                );
            }

            found.map(address_of)
        }
        _ => None,
    };

    let docks = registry_snapshot();
    let facts: Vec<DockFacts> = docks.iter().map(|dock| unsafe { facts_of(*dock) }).collect();
    let plan = plan_for_file(file, &policy, target, &facts, &addresses);

    apply_plan(app, file, &docks, &monitors, &plan);
}

/// One dock's facts, read from its state.
///
/// # Safety
///
/// `dock` must be a live dock.
unsafe fn facts_of(dock: *mut LambdockState) -> DockFacts {
    let Some(dock) = dock.as_ref() else {
        return DockFacts {
            config: None,
            monitor: None,
        };
    };

    DockFacts {
        config: crate::cstr::non_empty_text(dock.config_path),
        monitor: Some(dock.monitor as usize).filter(|monitor| *monitor != 0),
    }
}

/// Reconciles the active docks with the settings files on disk: a dock per file
/// and monitor policy, the fallback dock when there is no file at all, and
/// nothing left over for a file that is gone.
///
/// # Safety
///
/// `app` must be the live application.
#[no_mangle]
pub unsafe extern "C" fn sync_docks_with_disk(app: *mut gtk4::ffi::GtkApplication) {
    if app.is_null() {
        return;
    }
    let app: gtk4::Application = from_glib_none(app);

    let files: Vec<String> = settings_files()
        .iter()
        .map(|path| path.to_string_lossy().into_owned())
        .collect();

    if files.is_empty() {
        // No settings file at all: one fallback dock, from the built-in settings.
        for dock in registry_snapshot() {
            if facts_of(dock).config.is_some() {
                destroy_dock_instance(dock);
            }
        }

        if registry_snapshot().is_empty() {
            crate::log_info!(
                "Main",
                "No settings files found. Spawning default fallback dock..."
            );
            crate::bridge::reload_guile_config();
            create_dock_instance(app.as_ptr().cast(), std::ptr::null_mut(), std::ptr::null());
        }

        return;
    }

    crate::log_info!("Main", "Settings file(s) detected. Destroying fallback dock.");

    // The fallback dock goes, and so does any dock whose settings file is gone.
    for dock in registry_snapshot() {
        let keep = facts_of(dock)
            .config
            .is_some_and(|config| files.contains(&config));

        if !keep {
            destroy_dock_instance(dock);
        }
    }

    for file in &files {
        reconcile_settings_file(&app, file);
    }
}

// ===========================================================================
// THE GUILES SUBRS
// ===========================================================================
//
// The REPL runs on its own thread, so every subr here marshals to the GTK thread
// with an idle and hands it a value it owns.

extern "C" {
    fn scm_c_resolve_module(name: *const c_char) -> *mut c_void;
    fn scm_set_current_module(module: *mut c_void) -> *mut c_void;
    fn scm_c_use_module(name: *const c_char);
    fn scm_c_define_gsubr(
        name: *const c_char,
        required: c_int,
        optional: c_int,
        rest: c_int,
        function: *mut c_void,
    ) -> *mut c_void;
    fn scm_c_export(name: *const c_char, ...);
    fn scm_boot_guile(
        argc: c_int,
        argv: *mut *mut c_char,
        main: extern "C" fn(*mut c_void, c_int, *mut *mut c_char),
        data: *mut c_void,
    );

    fn setlocale(category: c_int, locale: *const c_char) -> *mut c_char;
    fn bindtextdomain(domain: *const c_char, directory: *const c_char) -> *mut c_char;
    fn bind_textdomain_codeset(domain: *const c_char, codeset: *const c_char) -> *mut c_char;
    fn textdomain(domain: *const c_char) -> *mut c_char;
}

/// `LC_ALL`.
const LC_ALL: c_int = 6;

/// Guile's "nothing to return" value.
///
/// It is `SCM_UNSPECIFIED`, a word with its own flag bits, so it is asked of the
/// interpreter once rather than spelled as zero -- zero is a valid SCM and not
/// this one.
fn unspecified() -> *mut c_void {
    static UNSPECIFIED: std::sync::OnceLock<usize> = std::sync::OnceLock::new();

    let value = *UNSPECIFIED.get_or_init(|| unsafe {
        crate::guile::scm_c_eval_string(c"(if #f #f)".as_ptr()) as usize
    });

    value as *mut c_void
}

/// `(reload-dock!)`: redraws the docks from the in-memory configuration.
extern "C" fn scm_reload_dock() -> *mut c_void {
    unsafe { glib::ffi::g_idle_add(Some(reload_idle), std::ptr::null_mut()) };

    unspecified()
}

/// `(show-launchpad!)`: shows the launchpad of the first active dock.
extern "C" fn scm_show_launchpad() -> *mut c_void {
    unsafe { glib::ffi::g_idle_add(Some(show_launchpad_idle), std::ptr::null_mut()) };

    unspecified()
}

/// `(hide-launchpad!)`.
extern "C" fn scm_hide_launchpad() -> *mut c_void {
    unsafe { glib::ffi::g_idle_add(Some(hide_launchpad_idle), std::ptr::null_mut()) };

    unspecified()
}

/// `(launchpad-search! query)`: types into the launchpad's search field.
extern "C" fn scm_launchpad_search(query: *mut c_void) -> *mut c_void {
    // The idle owns this copy of the string.
    let text = unsafe { crate::guile::scm_to_utf8_string(query.cast()) };
    if text.is_null() {
        return unspecified();
    }

    let text: *mut c_void = text.cast();
    unsafe { glib::ffi::g_idle_add(Some(launchpad_search_idle), text) };

    unspecified()
}

/// The idle that redraws every dock.
extern "C" fn reload_idle(_data: *mut c_void) -> c_int {
    crate::log_info!("Main", "Redrawing UI from in-memory state...");

    unsafe {
        crate::dock_registry::dock_registry_foreach(Some(redraw_visitor), std::ptr::null_mut())
    };

    glib::ffi::GFALSE
}

extern "C" fn redraw_visitor(state: *mut LambdockState, _data: *mut c_void) {
    unsafe { crate::dock_item::reload_dock_instance(state) };
}

extern "C" fn show_launchpad_idle(_data: *mut c_void) -> c_int {
    if let Some(first) = registry_snapshot().first() {
        unsafe { crate::ui::launchpad::lambdock_launchpad_show(*first) };
    }

    glib::ffi::GFALSE
}

extern "C" fn hide_launchpad_idle(_data: *mut c_void) -> c_int {
    crate::ui::launchpad::lambdock_launchpad_hide();

    glib::ffi::GFALSE
}

/// `data` is the interpreter's copy of the query, owned and freed here.
extern "C" fn launchpad_search_idle(data: *mut c_void) -> c_int {
    unsafe {
        let query = crate::cstr::text_or_empty(data.cast());
        crate::ui::launchpad::lambdock_launchpad_set_query(cstring(&query).as_ptr());
        crate::glib::g_free(data);
    }

    glib::ffi::GFALSE
}

/// Exposes the subrs to Scheme, inside `(lambdock core)`.
///
/// `scm_c_use_module` only imports that module into the current one, so defining
/// without switching first would put `reload-dock!` in `(guile-user)`, where the
/// setters -- which resolve it with `module-ref` on `(lambdock core)` -- would
/// never find it and the dock would not redraw after `(set-dock-...!)`.
fn setup_guile_primitives() {
    unsafe {
        let module = scm_c_resolve_module(c"lambdock core".as_ptr());
        let previous = scm_set_current_module(module);

        let define = |name: *const c_char, required: c_int, function: *const c_void| {
            scm_c_define_gsubr(name, required, 0, 0, function as *mut c_void);
        };

        define(
            c"reload-dock!".as_ptr(),
            0,
            scm_reload_dock as *const () as *const c_void,
        );
        define(
            c"show-launchpad!".as_ptr(),
            0,
            scm_show_launchpad as *const () as *const c_void,
        );
        define(
            c"hide-launchpad!".as_ptr(),
            0,
            scm_hide_launchpad as *const () as *const c_void,
        );
        define(
            c"launchpad-search!".as_ptr(),
            1,
            scm_launchpad_search as *const () as *const c_void,
        );

        scm_c_export(
            c"reload-dock!".as_ptr(),
            c"show-launchpad!".as_ptr(),
            c"hide-launchpad!".as_ptr(),
            c"launchpad-search!".as_ptr(),
            std::ptr::null::<c_char>(),
        );

        scm_set_current_module(previous);
    }
}

// ===========================================================================
// WATCHERS, MONITOR HOTPLUG AND STARTUP
// ===========================================================================

thread_local! {
    /// The two debounce timers and the pending monitor-sync idle: the
    /// process-wide source ids, in one place so their ownership is one thing to
    /// look at.
    static TIMERS: RefCell<Timers> = const { RefCell::new(Timers {
        config_reload: 0,
        state_reload: 0,
        monitor_sync: 0,
    }) };
}

struct Timers {
    config_reload: c_uint,
    state_reload: c_uint,
    monitor_sync: c_uint,
}

/// Restarts one timer: a write is several file events on some backends.
fn restart_timer(slot: impl Fn(&mut Timers) -> &mut c_uint, delay: u32, callback: extern "C" fn(*mut c_void) -> c_int, data: *mut c_void) {
    let previous = TIMERS.with_borrow_mut(|timers| std::mem::replace(slot(timers), 0));

    unsafe {
        if previous != 0 {
            glib::ffi::g_source_remove(previous);
        }

        let id = glib::ffi::g_timeout_add(delay, Some(callback), data);
        TIMERS.with_borrow_mut(|timers| *slot(timers) = id);
    }
}

/// Debounced: a settings directory that changed is reconciled once it settles.
extern "C" fn on_config_reload_timeout(app: *mut c_void) -> c_int {
    TIMERS.with_borrow_mut(|timers| timers.config_reload = 0);
    unsafe { sync_docks_with_disk(app.cast()) };

    glib::ffi::GFALSE
}

/// Debounced: a local-state file changed, so every dock re-reads its settings and
/// redraws. A local-state file is not a settings file -- it is replayed *on top
/// of* one by the Scheme loader -- so reading the settings file again is what
/// makes the new preferences or layout visible.
extern "C" fn on_state_reload_timeout(_data: *mut c_void) -> c_int {
    TIMERS.with_borrow_mut(|timers| timers.state_reload = 0);
    unsafe {
        crate::dock_registry::dock_registry_foreach(Some(state_reload_visitor), std::ptr::null_mut())
    };

    glib::ffi::GFALSE
}

extern "C" fn state_reload_visitor(state: *mut LambdockState, _data: *mut c_void) {
    let Some(path) = (unsafe { state.as_ref() })
        .and_then(|dock| unsafe { crate::cstr::non_empty_text(dock.config_path) })
    else {
        return;
    };

    unsafe {
        crate::bridge::load_dock_config_scm(cstring(&path).as_ptr());
        crate::dock_item::reload_dock_instance(state);
    }
}

extern "C" fn on_file_changed(_monitor: *mut c_void, _file: *mut c_void, _other: *mut c_void, _event: c_int, app: *mut c_void) {
    restart_timer(
        |timers| &mut timers.config_reload,
        CONFIG_RELOAD_DEBOUNCE_MS,
        on_config_reload_timeout,
        app,
    );
}

extern "C" fn on_state_changed(_monitor: *mut c_void, _file: *mut c_void, _other: *mut c_void, _event: c_int, _data: *mut c_void) {
    restart_timer(
        |timers| &mut timers.state_reload,
        CONFIG_RELOAD_DEBOUNCE_MS,
        on_state_reload_timeout,
        std::ptr::null_mut(),
    );
}

/// Watches `~/.config/lambdock` for settings files and `~/.local/state/lambdock`
/// for the state they are layered with.
///
/// Both, for the same reason: what the dock draws is the settings file *plus* the
/// preferences and layout overrides, and a change to either half is something the
/// user expects to see. The state directory is created first, since a dock that
/// has never been edited has not made one yet.
///
/// The monitors are deliberately kept: they are process-lifetime objects whose
/// signal is the only thing that keeps them doing their job.
unsafe fn setup_config_file_monitors(app: &gtk4::Application) {
    let app_pointer: *mut c_void = app.as_ptr().cast();

    for (directory, label, callback) in [
        (config_dir(), "config", on_file_changed as extern "C" fn(_, _, _, _, _)),
        (state_dir(), "local state", on_state_changed as extern "C" fn(_, _, _, _, _)),
    ] {
        if label == "local state" {
            let _ = std::fs::create_dir_all(&directory);
        }

        let monitor = gtk4::gio::File::for_path(&directory)
            .monitor_directory(gtk4::gio::FileMonitorFlags::NONE, None::<&gtk4::gio::Cancellable>);

        match monitor {
            Ok(monitor) => {
                monitor.connect_changed(move |_, _, _, _| {
                    callback(
                        std::ptr::null_mut(),
                        std::ptr::null_mut(),
                        std::ptr::null_mut(),
                        0,
                        app_pointer,
                    )
                });
                std::mem::forget(monitor);
                crate::log_info!(
                    "Main",
                    "Watching directory for {label} changes: {}",
                    directory.display()
                );
            }
            Err(error) => {
                crate::log_warn!("Main", "Could not watch {}: {error}", directory.display());
            }
        }
    }
}

/// The pending monitor reconcile, deferred to an idle.
///
/// Creating and destroying layer surfaces (and walking the monitor model in
/// `is_monitor_valid`) while the `GListModel` is still emitting `items-changed`
/// is reentrancy-prone, so the work waits for the signal to finish.
extern "C" fn on_monitors_changed_idle(app: *mut c_void) -> c_int {
    TIMERS.with_borrow_mut(|timers| timers.monitor_sync = 0);

    crate::log_info!("Main", "MONITORS CHANGED: reconciling docks");
    unsafe { sync_docks_with_disk(app.cast()) };
    crate::log_info!("Main", "MONITORS CHANGED: reconciliation done");

    glib::ffi::GFALSE
}

/// Hooks the display's monitor model, so a hotplug reconciles the docks.
fn setup_monitor_listeners(app: &gtk4::Application) {
    let Some(display) = gdk::Display::default() else {
        return;
    };
    let monitors = display.monitors();

    let app_pointer: *mut c_void = app.as_ptr().cast();
    monitors.connect_items_changed(move |_, position, removed, added| {
        crate::log_info!(
            "Main",
            "MONITORS CHANGED: position={position} removed={removed} added={added}"
        );

        let pending = TIMERS.with_borrow(|timers| timers.monitor_sync);
        if pending == 0 {
            let id = unsafe { glib::ffi::g_idle_add(Some(on_monitors_changed_idle), app_pointer) };
            TIMERS.with_borrow_mut(|timers| timers.monitor_sync = id);
        }
    });

    crate::log_info!("Main", "Registered listener for display monitor hotplug events.");
}

/// The theme-independent overlay stylesheet: stack popovers, the launchpad and
/// drag feedback. Overlays are not part of any dock capsule, so they must not be
/// scoped to a theme class.
fn load_overlay_css() {
    let Some(display) = gdk::Display::default() else {
        return;
    };

    let provider = gtk4::CssProvider::new();
    provider.load_from_resource("/org/jointhefreeworld/lambdock/css/overlays.css");

    gtk4::style_context_add_provider_for_display(
        &display,
        &provider,
        gtk4::STYLE_PROVIDER_PRIORITY_APPLICATION,
    );
}

/// Adds one application action and connects it to `handler`.
fn add_action(
    app: &gtk4::Application,
    name: &str,
    parameter_type: Option<&glib::VariantTy>,
    handler: unsafe extern "C" fn(*mut gtk4::gio::ffi::GSimpleAction, *mut glib::ffi::GVariant, *mut c_void),
) {
    let action = gtk4::gio::SimpleAction::new(name, parameter_type);

    let app_pointer: *mut c_void = app.as_ptr().cast();
    action.connect_activate(move |action, parameter| unsafe {
        let parameter: *mut glib::ffi::GVariant = match parameter {
            Some(parameter) => parameter.as_ptr(),
            None => std::ptr::null_mut(),
        };
        let action_pointer: *mut gtk4::gio::ffi::GSimpleAction = action.as_ptr();

        handler(action_pointer, parameter, app_pointer);
    });

    app.add_action(&action);
}

/// The application's setup: Guile, the actions, the docks and the watchers.
fn activate(app: &gtk4::Application) {
    crate::log_info!("Main", "Initializing Guile engine & multi-monitor manager...");

    unsafe {
        crate::bridge::setup_guile_load_path();
        scm_c_use_module(c"lambdock core".as_ptr());
    }
    setup_guile_primitives();

    crate::ui::icon_resolver::setup_icon_theme();
    load_overlay_css();

    // Follow the desktop's reduced-motion setting while the dock runs.
    unsafe {
        crate::accessibility::lambdock_watch_system_motion(
            Some(on_system_motion_changed),
            std::ptr::null_mut(),
        )
    };

    // The global menu actions. Preferences and Save settings take the dock's
    // settings path as a string target; the action map holds its own reference.
    add_action(app, "about", None, on_about_action);
    add_action(app, "quit", None, on_quit_action);
    add_action(app, "preferences", Some(glib::VariantTy::STRING), on_preferences_action);
    add_action(app, "export-settings", Some(glib::VariantTy::STRING), on_export_settings_action);

    unsafe {
        sync_docks_with_disk(app.as_ptr().cast());
        setup_config_file_monitors(app);
    }
    setup_monitor_listeners(app);
}

/// Releases the preferences windows as the application tears down.
fn on_app_shutdown(_app: &gtk4::Application) {
    crate::ui::preferences::lambdock_preferences_shutdown();
}

// ===========================================================================
// THE PROGRAM ENTRY POINT
// ===========================================================================

/// Runs inside Guile's boot: the application is created and run here, so the
/// interpreter is ready before the first dock reads its settings.
extern "C" fn inner_main(_data: *mut c_void, argc: c_int, argv: *mut *mut c_char) {
    let app = gtk4::Application::new(
        Some("org.jointhefreeworld.lambdock"),
        gtk4::gio::ApplicationFlags::empty(),
    );

    app.connect_activate(activate);
    app.connect_shutdown(on_app_shutdown);

    // Blocks until the application exits.
    let arguments: Vec<String> = (0..argc)
        .map(|index| unsafe { crate::cstr::text_or_empty(*argv.add(index as usize)) })
        .collect();

    app.run_with_args(&arguments);
}

/// The binary's entry point: environment, i18n, Guile, then GTK.
///
/// # Safety
///
/// `argv` must be the process's own argument vector.
#[no_mangle]
pub unsafe extern "C" fn lambdock_main(argc: c_int, argv: *mut *mut c_char) -> c_int {
    // The renderer can be pinned at build time.
    if let Some(renderer) = option_env!("DEFAULT_GSK_RENDERER") {
        glib::ffi::g_setenv(c"GSK_RENDERER".as_ptr(), cstring(renderer).as_ptr(), 0);
    }

    setlocale(LC_ALL, c"".as_ptr());
    if let Some(locale_dir) = option_env!("LAMBDOCK_LOCALEDIR") {
        bindtextdomain(c"lambdock".as_ptr(), cstring(locale_dir).as_ptr());
    }
    bind_textdomain_codeset(c"lambdock".as_ptr(), c"UTF-8".as_ptr());
    textdomain(c"lambdock".as_ptr());

    // Boots the interpreter, runs the callback, and returns when GTK is done.
    scm_boot_guile(argc, argv, inner_main, std::ptr::null_mut());

    0
}

/// Runs the application with the process's own arguments, for the Rust binary.
///
/// The C entry point used to hand these over; now `main` does, and the argument
/// vector is built once here rather than read from a `char **`.
pub fn run() -> u8 {
    let arguments: Vec<std::ffi::CString> = std::env::args_os()
        .map(|argument| cstring(&argument.to_string_lossy()))
        .collect();

    let mut pointers: Vec<*mut c_char> = arguments
        .iter()
        .map(|argument| argument.as_ptr() as *mut c_char)
        .collect();

    let status = unsafe { lambdock_main(pointers.len() as c_int, pointers.as_mut_ptr()) };

    u8::try_from(status).unwrap_or(1)
}

#[cfg(test)]
mod plan_tests {
    use super::*;

    fn dock(config: Option<&str>, monitor: Option<usize>) -> DockFacts {
        DockFacts {
            config: config.map(str::to_owned),
            monitor,
        }
    }

    /// `dock-monitor: all` wants one dock per live monitor, and none on a monitor
    /// that is gone.
    #[test]
    fn all_monitors_get_a_dock_each() {
        let docks = [
            dock(Some("/a.scm"), Some(1)),
            dock(Some("/a.scm"), Some(2)),
            dock(Some("/a.scm"), None),
            dock(Some("/b.scm"), Some(1)),
        ];

        let plan = plan_for_file("/a.scm", &MonitorPolicy::All, None, &docks, &[1, 3]);

        assert_eq!(plan.destroy, vec![1, 2]);
        assert_eq!(plan.reload, vec![0]);
        assert_eq!(plan.spawn, vec![Some(3)]);
    }

    /// A named connector keeps one dock, on that monitor.
    #[test]
    fn a_connector_keeps_exactly_one_dock() {
        let docks = [dock(Some("/a.scm"), Some(1)), dock(Some("/a.scm"), Some(2))];

        let plan = plan_for_file(
            "/a.scm",
            &MonitorPolicy::Connector("DP-2".to_owned()),
            Some(2),
            &docks,
            &[1, 2],
        );

        assert_eq!(plan.destroy, vec![0]);
        assert_eq!(plan.reload, vec![1]);
        assert!(plan.spawn.is_empty());
    }

    /// A connector that does not exist falls back to the default monitor.
    #[test]
    fn a_missing_connector_falls_back_to_the_default_monitor() {
        let docks = [dock(Some("/a.scm"), Some(1))];

        let plan = plan_for_file(
            "/a.scm",
            &MonitorPolicy::Connector("nope".to_owned()),
            None,
            &docks,
            &[1],
        );

        assert_eq!(plan.destroy, vec![0]);
        assert_eq!(plan.spawn, vec![None]);
    }

    /// No `dock-monitor` means exactly one dock, with no monitor of its own.
    #[test]
    fn the_default_policy_wants_one_dock_without_a_monitor() {
        let docks = [dock(Some("/a.scm"), None), dock(Some("/b.scm"), None)];

        let plan = plan_for_file("/a.scm", &MonitorPolicy::Default, None, &docks, &[1]);
        assert!(plan.destroy.is_empty());
        assert_eq!(plan.reload, vec![0]);
        assert!(plan.spawn.is_empty());

        let only_other_file = [dock(Some("/b.scm"), None)];
        let plan = plan_for_file("/a.scm", &MonitorPolicy::Default, None, &only_other_file, &[1]);
        assert_eq!(plan.spawn, vec![None]);
    }

    /// A dock whose settings file is gone is not in any plan but its own file's
    /// absence: another file's docks are never touched.
    #[test]
    fn another_files_docks_are_left_alone() {
        let docks = [dock(Some("/b.scm"), Some(1)), dock(None, None)];

        let plan = plan_for_file("/a.scm", &MonitorPolicy::All, None, &docks, &[1]);

        assert!(plan.destroy.is_empty());
        assert!(plan.reload.is_empty());
        assert_eq!(plan.spawn, vec![Some(1)]);
    }

    /// The policy the settings file asks for, as a value.
    #[test]
    fn the_monitor_policy_is_read_from_the_settings() {
        assert_eq!(MonitorPolicy::from_config(Some("all")), MonitorPolicy::All);
        assert_eq!(MonitorPolicy::from_config(Some("default")), MonitorPolicy::Default);
        assert_eq!(MonitorPolicy::from_config(Some("")), MonitorPolicy::Default);
        assert_eq!(MonitorPolicy::from_config(None), MonitorPolicy::Default);
        assert_eq!(
            MonitorPolicy::from_config(Some("DP-1")),
            MonitorPolicy::Connector("DP-1".to_owned())
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Only the two documented shapes are settings files.
    #[test]
    fn a_settings_file_is_named_for_a_dock() {
        assert!(is_settings_filename("settings.scm"));
        assert!(is_settings_filename("settings-work.scm"));
        assert!(is_settings_filename("settings-home-2.scm"));

        // Not settings files: a backup, an editor's leftovers, a prefix, a
        // different extension, and the empty name.
        assert!(!is_settings_filename("settings.scm~"));
        assert!(!is_settings_filename("settings.scm.bak"));
        assert!(!is_settings_filename("settings-.scm.bak"));
        assert!(!is_settings_filename("mysettings.scm"));
        assert!(!is_settings_filename("settings"));
        assert!(!is_settings_filename(""));
        assert!(!is_settings_filename("settings-.scm."));
    }

    /// Only settings files are picked up, sorted, and an unreadable directory
    /// has none -- which is what a fresh install looks like.
    #[test]
    fn a_directory_is_scanned_for_settings_files() {
        let root = std::env::temp_dir().join(format!("lambdock-scan-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).expect("a temporary directory");

        for name in [
            "settings-work.scm",
            "settings.scm",
            "notes.txt",
            "settings.scm.bak",
            "settings-home.scm",
        ] {
            std::fs::write(root.join(name), "").expect("a file");
        }

        let found: Vec<String> = settings_files_in(&root)
            .iter()
            .map(|path| path.file_name().unwrap().to_string_lossy().into_owned())
            .collect();

        assert_eq!(
            found,
            ["settings-home.scm", "settings-work.scm", "settings.scm"],
            "sorted, and only the two documented shapes"
        );

        std::fs::remove_dir_all(&root).expect("cleanup");
        assert!(settings_files_in(&root).is_empty(), "a directory that is gone has none");
    }

    /// One notch is thirty pixels, and the offset never leaves the band.
    #[test]
    fn the_wheel_moves_the_row_and_stops_at_the_ends() {
        // A notch down, from the start: thirty pixels in.
        assert_eq!(scrolled_value(0.0, 1.0, 100.0, 400.0, 0.0), 30.0);
        assert_eq!(scrolled_value(300.0, -1.0, 100.0, 400.0, 0.0), 270.0);

        // A wheel that keeps turning at either end stays at the end.
        assert_eq!(scrolled_value(0.0, -5.0, 100.0, 400.0, 0.0), 0.0);
        assert_eq!(scrolled_value(300.0, 5.0, 100.0, 400.0, 0.0), 300.0);

        // A row that does not scroll at all (nothing above the page) is a
        // fixed zero rather than a negative offset.
        assert_eq!(scrolled_value(0.0, 1.0, 100.0, 100.0, 0.0), 0.0);
        assert_eq!(scrolled_value(0.0, 1.0, 100.0, 40.0, 10.0), 10.0);
    }
}
