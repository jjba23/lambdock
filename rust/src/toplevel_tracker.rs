// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The window tracker: `src/toplevel_tracker.c` in Rust.
//!
//! One tracker per session, shared by every dock: the compositor reports a
//! window and each dock then decides whether it wants an icon for it, which is
//! why every event here is a broadcast.
//!
//! The module holds both halves of that arrangement:
//!
//! 1. **the broadcast** -- a [`LambdockToplevel`] in, "every dock, now" out. This
//!    is what the backends call and the only part they share.
//! 2. **the Wayland half** -- one registry listener and one foreign-toplevel
//!    manager, with its proxy lifetimes.
//!
//! The X11/EWMH half is still `src/x11_tracker.c`: it fills the same records and
//! calls the same broadcasts. The *decisions* ("should this dock hide?", "is this
//! item stale?") are `core/tracker_rules.rs`, unit tested without a display.
//!
//! # The Wayland surface
//!
//! `wayland-client` is not vendored and does not need to be: libwayland is a C
//! library like GTK, so the calls the dock makes are declared here. The generated
//! protocol header's request functions are `static inline` -- they are one
//! `wl_proxy_marshal_flags` call each -- so the ones this module sends are
//! reimplemented with the opcodes from the protocol, and the interfaces are the
//! generated C objects.

use gtk4::gio;
use gtk4::glib;
use gtk4::glib::translate::{IntoGlib, ToGlibPtr};
use std::os::raw::{c_char, c_int, c_void};
use std::sync::{Mutex, OnceLock};

use crate::app_window::{
    app_window_matches, app_window_set_state, app_window_set_title, LambdockToplevel,
};
use crate::dock_item::{create_dock_item, free_dock_item, DockItem};
use crate::log_info;
use crate::log_warn;
use crate::state::LambdockState;
use crate::dock_window::{dock_window_set_tracker_connected, hide_dock, reveal_dock};
use crate::x11_tracker::{x11_tracker_activate, x11_tracker_close, x11_tracker_minimize, x11_tracker_replay, x11_tracker_start};
use crate::ui::dock_item_windows::{
    clear_tracked_windows, dock_item_add_window, dock_item_refresh_window_state,
    dock_item_remove_window, find_dock_item_by_app_id, update_dock_item_status,
};

// ============================================================================
// THE WAYLAND SURFACE
// ============================================================================

/// `struct wl_interface`, which the registry compares by name and binds by
/// pointer. The layout is libwayland's, and stable.
#[repr(C)]
struct WlInterface {
    name: *const c_char,
    version: c_int,
    method_count: c_int,
    methods: *const c_void,
    event_count: c_int,
    events: *const c_void,
}

/// `struct wl_array`: three words, as `core/tracker_rules`' reading of the state
/// event needs them.
#[repr(C)]
struct WlArray {
    size: usize,
    alloc: usize,
    data: *mut c_void,
}

type WlDisplay = c_void;
/// `GdkDisplay`, opaque: only the type test and the backend's own getter see it.
type GdkDisplay = c_void;
type WlRegistry = c_void;
type WlProxy = c_void;
type WlSeat = c_void;
type ToplevelManager = c_void;
type ToplevelHandle = c_void;

/// `WL_MARSHAL_FLAG_DESTROY`: the request destroys its proxy.
const MARSHAL_DESTROY: u32 = 1;

/// `wl_display.get_registry` and `wl_registry.bind`, from `wayland.xml`.
const DISPLAY_GET_REGISTRY: u32 = 1;
const REGISTRY_BIND: u32 = 0;

/// The opcodes of the requests this module sends, from the protocol.
const MANAGER_DESTROY: u32 = 1;
const HANDLE_SET_MINIMIZED: u32 = 2;
const HANDLE_UNSET_MINIMIZED: u32 = 3;
const HANDLE_ACTIVATE: u32 = 4;
const HANDLE_CLOSE: u32 = 5;
const HANDLE_DESTROY: u32 = 7;

/// `zwlr_foreign_toplevel_handle_v1_state`, the two flags a dock cares about.
const STATE_MINIMIZED: u32 = 1;
const STATE_ACTIVATED: u32 = 2;

extern "C" {
    fn wl_proxy_add_listener(
        proxy: *mut WlProxy,
        implementation: *const *const c_void,
        data: *mut c_void,
    ) -> c_int;
    fn wl_proxy_marshal_flags(
        proxy: *mut WlProxy,
        opcode: u32,
        interface: *const WlInterface,
        version: u32,
        flags: u32,
        ...
    ) -> *mut WlProxy;
    fn wl_proxy_get_version(proxy: *mut WlProxy) -> u32;
    fn wl_proxy_destroy(proxy: *mut WlProxy);

    static zwlr_foreign_toplevel_manager_v1_interface: WlInterface;
    static wl_registry_interface: WlInterface;
    static wl_seat_interface: WlInterface;

    /// GDK's Wayland backend: the Wayland display behind the GDK one, and the
    /// type test that says whether this session is that backend at all.
    fn gdk_wayland_display_get_type() -> glib::ffi::GType;
    fn gdk_wayland_display_get_wl_display(display: *mut GdkDisplay) -> *mut WlDisplay;

    /// GDK's X11 backend, for the type test only.
    fn gdk_x11_display_get_type() -> glib::ffi::GType;


}

// ============================================================================
// THE TRACKER
// ============================================================================

/// Which window list this session gave us.
#[derive(Clone, Copy, PartialEq, Eq, Default)]
enum Backend {
    #[default]
    None,
    Wayland,
    X11,
}

/// Everything the Wayland half owns, as one value.
///
/// There is exactly one instance because there is exactly one Wayland
/// connection, and the listeners receive it through Wayland's own `data` pointer
/// rather than reaching for a file-scope variable.
#[derive(Default)]
struct Tracker {
    manager: *mut ToplevelManager,
    /// Registry name of `manager`; 0 when unbound.
    manager_global: u32,
    seat: *mut WlSeat,
    /// Registry name of `seat`; 0 when unbound.
    seat_global: u32,
    /// The registry listener is installed once and stays installed: the registry
    /// is still alive after a teardown and re-announces the global if the
    /// compositor brings it back. Re-adding it would deliver every global twice.
    registry_listener_installed: bool,
    /// How many globals the registry has announced. Non-zero means it was
    /// dispatched at least once, which turns "the manager is missing" from a
    /// guess into a fact.
    globals_seen: u32,
    /// One diagnostic, not one per click.
    warned_no_seat: bool,
    /// Whether a manager is bound right now.
    connected: bool,
    /// The windows the compositor is reporting, owned here.
    toplevels: Vec<*mut LambdockToplevel>,
}

// SAFETY: every access goes through the main loop -- GTK dispatches Wayland's
// events there, and nothing spawns a thread that touches this -- so the mutex is
// never contended across threads. It exists so the tracker can live in a `static`
// at a stable address, which is what Wayland's `data` pointer needs.
unsafe impl Send for Tracker {}

/// The one tracker, at an address Wayland can hold on to.
fn tracker() -> &'static Mutex<Tracker> {
    static TRACKER: OnceLock<Mutex<Tracker>> = OnceLock::new();

    TRACKER.get_or_init(|| Mutex::new(Tracker::default()))
}

/// `wl_display_get_registry`: one `wl_proxy_marshal_flags`, in the header.
///
/// # Safety
///
/// `display` must be the Wayland display behind the GDK one.
unsafe fn display_get_registry(display: *mut WlProxy) -> *mut WlRegistry {
    wl_proxy_marshal_flags(
        display,
        DISPLAY_GET_REGISTRY,
        &wl_registry_interface,
        wl_proxy_get_version(display),
        0,
    )
    .cast()
}

/// `wl_registry_bind`: also one marshal, with the interface's own name and a null
/// implementation array, which is what the header does.
///
/// # Safety
///
/// `registry` must be a live registry proxy; `interface` a live interface.
unsafe fn registry_bind(
    registry: *mut WlRegistry,
    name: u32,
    interface: *const WlInterface,
    version: u32,
) -> *mut c_void {
    wl_proxy_marshal_flags(
        registry.cast(),
        REGISTRY_BIND,
        interface,
        version,
        0,
        name,
        (*interface).name,
        version,
        std::ptr::null_mut::<c_void>(),
    )
}

/// The tracker behind a listener's `data` pointer.
///
/// # Safety
///
/// `data` must be a pointer from [`tracker_data`].
unsafe fn tracker_of<'a>(data: *mut c_void) -> Option<&'a Mutex<Tracker>> {
    (data as *const Mutex<Tracker>).as_ref()
}

/// Which window list this session offers.
fn backend() -> Backend {
    *BACKEND.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// Records which backend answered `init_toplevel_tracker`.
fn set_backend(backend: Backend) {
    *BACKEND.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = backend;
}

static BACKEND: Mutex<Backend> = Mutex::new(Backend::None);

/// The GDK display, when there is one.
fn gdk_display() -> Option<*mut GdkDisplay> {
    let display = gtk4::gdk::Display::default()?;

    let display: *mut gtk4::gdk::ffi::GdkDisplay = display.to_glib_none().0;

    Some(display.cast())
}

/// Whether `display` is of the GTK type `type_`.
///
/// # Safety
///
/// `display` must be a live `GdkDisplay`.
unsafe fn display_is(display: *mut GdkDisplay, type_: glib::ffi::GType) -> bool {
    glib::gobject_ffi::g_type_check_instance_is_a(display.cast(), type_) != 0
}

// ============================================================================
// THE BROADCAST
// ============================================================================
//
// Every session event means "this, for each dock". Each visitor is a plain
// function over one dock, and `dock_registry_foreach` walks a snapshot, so a
// visitor may even destroy its own dock without disturbing the walk.

/// Payload of the two app-id visitors: the window, and the id it reports now.
struct AppIdEvent {
    window: *mut LambdockToplevel,
    app_id: *const c_char,
}

/// Creates the dock item for an application that is running but not pinned.
///
/// The session gives an app id, which is not a name people want to read
/// ("org.gnome.SystemMonitor") and not always an icon either. The desktop entry
/// cache turns it into the application's localized name, icon and command; when
/// there is no desktop file at all, the id is cleaned up into a name and used as
/// the icon lookup, since icon themes often name the icon after the app id.
///
/// # Safety
///
/// `state` must be a live `LambdockState`; `app_id` a C string.
unsafe fn create_item_for_app_id(
    state: *mut LambdockState,
    app_id: *const c_char,
) -> *mut DockItem {
    let entry = crate::ui::desktop_entry::lambdock_desktop_entry_lookup(app_id);
    let entry = entry.as_ref();

    let (name, icon, exec) = match entry {
        Some(entry) => {
            log_info!(
                "Tracker",
                "App id '{}' resolved to {} ({})",
                crate::cstr::text_or_empty(app_id),
                crate::cstr::text_or_empty(entry.id),
                crate::cstr::text_or_empty(entry.name)
            );

            (entry.name, entry.icon, entry.exec)
        }
        None => {
            let name = crate::ui::desktop_entry::lambdock_app_id_to_name(app_id);
            log_info!(
                "Tracker",
                "App id '{}' has no desktop entry; showing it as '{}'",
                crate::cstr::text_or_empty(app_id),
                crate::cstr::text_or_empty(name)
            );

            (name, app_id.cast_mut(), std::ptr::null_mut())
        }
    };

    create_dock_item(state, name, icon, exec, app_id, 0, 1)
}

/// Retitles one dock's copy of a window.
///
/// # Safety
///
/// `state` must be a live `LambdockState`; `data` a `LambdockToplevel`.
unsafe fn dock_apply_window_title(state: *mut LambdockState, data: *mut c_void) {
    let top = data.cast::<LambdockToplevel>();
    let Some(top_ref) = top.as_ref() else {
        return;
    };

    let item = find_dock_item_by_app_id(state, top_ref.app_id);
    let Some(item) = item.as_ref() else {
        return;
    };

    let mut link = item.windows;
    while let Some(entry) = link.as_ref() {
        link = entry.next;

        let window = entry.data.cast::<crate::app_window::AppWindow>();
        if app_window_matches(window, top) == 0 {
            continue;
        }

        // The session re-announces the title on every change, including changes
        // that do not touch the text shown. Nothing to do means nothing to
        // rebuild: no label, no accessible name, no title tooltip.
        if crate::glib::g_strcmp0((*window).title, top_ref.title) == 0 {
            return;
        }

        app_window_set_title(window, top_ref.title);

        // Refresh the running state, the accessible name and the window-title
        // label, which all read the title back.
        update_dock_item_status(item as *const _ as *mut _);

        return;
    }
}

/// Mirrors a window's minimized/focused state onto one dock.
///
/// # Safety
///
/// `state` must be a live `LambdockState`; `data` a `LambdockToplevel`.
unsafe fn dock_apply_window_state(state: *mut LambdockState, data: *mut c_void) {
    let top = data.cast::<LambdockToplevel>();
    let Some(top_ref) = top.as_ref() else {
        return;
    };

    let item = find_dock_item_by_app_id(state, top_ref.app_id);
    let Some(item) = item.as_ref() else {
        return;
    };

    let mut link = item.windows;
    while let Some(entry) = link.as_ref() {
        link = entry.next;

        let window = entry.data.cast::<crate::app_window::AppWindow>();
        if app_window_matches(window, top) == 0 {
            continue;
        }

        app_window_set_state(window, top_ref.minimized, top_ref.activated);

        // Minimize to dock is only half done if the icon does not show it: dim
        // the item when all of its windows are tucked away.
        dock_item_refresh_window_state(item as *const _ as *mut _);

        return;
    }
}

/// Detaches a window from the item tracking the id it used to have.
///
/// Applications that load a profile late (Electron apps, browsers) change app id
/// after their window exists; without this the old item keeps a stale window and
/// a permanent running indicator.
///
/// # Safety
///
/// `state` must be a live `LambdockState`; `data` an `AppIdEvent`.
unsafe fn dock_detach_previous_app_id(state: *mut LambdockState, data: *mut c_void) {
    let Some(event) = (data as *const AppIdEvent).as_ref() else {
        return;
    };

    let item = find_dock_item_by_app_id(state, event.window.as_ref().map_or(std::ptr::null(), |top| top.app_id));
    let Some(item) = item.as_ref() else {
        return;
    };

    dock_item_remove_window(item as *const _ as *mut _, event.window);

    // A pinned item stays; an item that only existed because a window did goes
    // away with it.
    let should_drop = crate::core::tracker_rules::lambdock_tracker_item_should_be_dropped(
        c_int::from(!item.windows.is_null()),
        item.is_favorite,
    );
    if should_drop != 0 {
        let item = item as *const DockItem as *mut DockItem;
        (*state).dock_items.retain(|each| *each != item);
        free_dock_item(item);
    }
}

/// Attaches a window to the item for its current app id.
///
/// # Safety
///
/// `state` must be a live `LambdockState`; `data` an `AppIdEvent`.
unsafe fn dock_attach_current_app_id(state: *mut LambdockState, data: *mut c_void) {
    let Some(event) = (data as *const AppIdEvent).as_ref() else {
        return;
    };
    let top = event.window;

    // A dock that does not track windows gets no running-app items here, exactly
    // as `sync_window_trackers` gives it none on reload. Without this gate the
    // two paths disagreed and a reload silently removed what a live event had
    // just added.
    if (*state).track_windows == 0 {
        return;
    }

    // An excluded app is the one case where the dock deliberately does nothing at
    // all.
    if crate::core::tracker_rules::lambdock_tracker_ignores_app_id(event.app_id) != 0
        || crate::bridge::is_app_excluded(event.app_id, (*state).config_path) != 0
    {
        log_info!(
            "Tracker",
            "Ignoring excluded app_id '{}' for [{}]",
            crate::cstr::text_or_empty(event.app_id),
            crate::cstr::text_or_empty((*state).config_path)
        );
        return;
    }

    let mut item = find_dock_item_by_app_id(state, event.app_id);
    if item.is_null() {
        item = create_item_for_app_id(state, event.app_id);
    }

    let top_ref = &*top;
    let window = dock_item_add_window(item, top);
    app_window_set_state(window, top_ref.minimized, top_ref.activated);
    app_window_set_title(window, top_ref.title);

    if !(*state).window_trackers.contains(&top) {
        (*state).window_trackers.push(top);
    }

    update_dock_item_status(item);

    let should_hide = crate::core::tracker_rules::lambdock_tracker_should_hide(
        (*state).auto_hide,
        (*state).show_when_clear,
        c_int::from(!(*state).window_trackers.is_empty()),
    );
    if should_hide != 0 {
        hide_dock(state);
    }
}

/// Forgets a window in one dock, because the session closed it.
///
/// # Safety
///
/// `state` must be a live `LambdockState`; `data` a `LambdockToplevel`.
unsafe fn dock_forget_closed_window(state: *mut LambdockState, data: *mut c_void) {
    let Some(top) = (data as *const LambdockToplevel).as_ref() else {
        return;
    };

    // Mirror of the gate above: a dock that never attached this window has no
    // item to detach it from, and no `window_trackers` entry either.
    if (*state).track_windows == 0 {
        return;
    }

    let item = find_dock_item_by_app_id(state, top.app_id);
    if let Some(item) = item.as_ref() {
        dock_item_remove_window(item as *const _ as *mut _, data.cast());

        let should_drop = crate::core::tracker_rules::lambdock_tracker_item_should_be_dropped(
            c_int::from(!item.windows.is_null()),
            item.is_favorite,
        );
        if should_drop != 0 {
            let item = item as *const DockItem as *mut DockItem;
            (*state).dock_items.retain(|each| *each != item);
            free_dock_item(item);
        }
    }

    let closed = data.cast::<crate::app_window::LambdockToplevel>();
    (*state).window_trackers.retain(|each| *each != closed);

    let should_reveal = crate::core::tracker_rules::lambdock_tracker_should_reveal(
        (*state).show_when_clear,
        c_int::from(!(*state).window_trackers.is_empty()),
    );
    if should_reveal != 0 {
        reveal_dock(state);
    }
}

/// Drops every tracked window of one dock, because the tracker went away.
///
/// # Safety
///
/// `state` must be a live `LambdockState`.
unsafe fn dock_forget_all_windows(state: *mut LambdockState, _data: *mut c_void) {
    clear_tracked_windows(state);
}

/// Tells one dock whether the compositor's window list is being tracked.
///
/// # Safety
///
/// `state` must be a live `LambdockState`.
unsafe fn dock_tracker_state(state: *mut LambdockState, data: *mut c_void) {
    dock_window_set_tracker_connected(state, !data.is_null());
}

/// The same answer for every dock: `connected` is a session fact, not a per-dock
/// one.
fn notify_docks_of_tracker(connected: bool) {
    unsafe {
        crate::dock_registry::dock_registry_foreach(
            Some(dock_tracker_state),
            c_int::from(connected) as *mut c_void,
        )
    };
}

/// Replays `visitor` into every dock.
///
/// # Safety
///
/// `visitor` must be valid for the call, and `data` whatever it expects.
unsafe fn foreach(
    visitor: unsafe fn(*mut LambdockState, *mut c_void),
    data: *mut c_void,
) {
    crate::dock_registry::dock_registry_foreach(Some(visitor), data);
}

// ============================================================================
// THE BROADCAST API -- what both backends call
// ============================================================================

/// A blank window record for a backend to fill in.
///
/// # Safety
///
/// The result is `(transfer full)`: free it with [`lambdock_toplevel_free`].
pub unsafe fn lambdock_toplevel_new() -> *mut LambdockToplevel {
    crate::glib::g_malloc0(std::mem::size_of::<LambdockToplevel>()).cast()
}

/// Frees the title, the id and the record. The backend destroys its own handle
/// first: only it knows what the handle means.
///
/// # Safety
///
/// `window` must be null or a record from [`lambdock_toplevel_new`].
pub unsafe fn lambdock_toplevel_free(window: *mut LambdockToplevel) {
    let Some(window) = window.as_mut() else {
        return;
    };

    crate::glib::g_free(window.title.cast());
    crate::glib::g_free(window.app_id.cast());
    crate::glib::g_free((window as *mut LambdockToplevel).cast());
}

/// Records the id and moves the window between the docks' items for it.
///
/// # Safety
///
/// `window` must be null or a live record; `app_id` a C string.
pub unsafe fn lambdock_toplevel_set_app_id(
    window: *mut LambdockToplevel,
    app_id: *const c_char,
) {
    let Some(window_ref) = window.as_mut() else {
        return;
    };
    if crate::core::tracker_rules::lambdock_tracker_ignores_app_id(app_id) != 0 {
        return;
    }

    // Detach from the previous id before recording the new one: the detach has to
    // look up the old item by the old app id, which is about to be overwritten.
    if !window_ref.app_id.is_null() && crate::glib::g_strcmp0(window_ref.app_id, app_id) != 0 {
        let event = AppIdEvent {
            window,
            app_id: window_ref.app_id,
        };
        foreach(dock_detach_previous_app_id, (&event as *const AppIdEvent).cast_mut().cast());
    }

    crate::glib::g_free(window_ref.app_id.cast());
    window_ref.app_id = crate::glib::strdup_string(&crate::cstr::text_or_empty(app_id));

    log_info!(
        "Tracker",
        "App ID resolved globally: '{}'",
        crate::cstr::text_or_empty(app_id)
    );

    let event = AppIdEvent {
        window,
        app_id: window_ref.app_id,
    };
    foreach(dock_attach_current_app_id, (&event as *const AppIdEvent).cast_mut().cast());
}

/// Records the title and renames the window in every dock, unless the text did
/// not change.
///
/// # Safety
///
/// `window` must be null or a live record; `title` a C string.
pub unsafe fn lambdock_toplevel_set_title(
    window: *mut LambdockToplevel,
    title: *const c_char,
) {
    let Some(window_ref) = window.as_mut() else {
        return;
    };
    if title.is_null() || crate::glib::g_strcmp0(window_ref.title, title) == 0 {
        return;
    }

    crate::glib::g_free(window_ref.title.cast());
    window_ref.title = crate::glib::strdup_string(&crate::cstr::text_or_empty(title));

    foreach(dock_apply_window_title, window.cast());
}

/// Records the flags and mirrors them onto every dock's copy.
///
/// # Safety
///
/// `window` must be null or a live record.
pub unsafe fn lambdock_toplevel_set_state(
    window: *mut LambdockToplevel,
    minimized: c_int,
    activated: c_int,
) {
    let Some(window_ref) = window.as_mut() else {
        return;
    };

    window_ref.minimized = minimized;
    window_ref.activated = activated;

    foreach(dock_apply_window_state, window.cast());
}

/// Replays one window into every dock, which is what makes
/// `sync_window_trackers` backend-independent.
///
/// # Safety
///
/// `window` must be null or a live record.
pub unsafe fn lambdock_toplevel_deliver(window: *mut LambdockToplevel) {
    let Some(window_ref) = window.as_ref() else {
        return;
    };
    if window_ref.app_id.is_null() {
        return;
    }

    let event = AppIdEvent {
        window,
        app_id: window_ref.app_id,
    };
    foreach(dock_attach_current_app_id, (&event as *const AppIdEvent).cast_mut().cast());
    foreach(dock_apply_window_title, window.cast());
}

/// Forgets the window in every dock. The record itself is left for the backend to
/// free.
///
/// # Safety
///
/// `window` must be null or a live record.
pub unsafe fn lambdock_toplevel_withdraw(window: *mut LambdockToplevel) {
    if window.is_null() {
        return;
    }

    foreach(dock_forget_closed_window, window.cast());
}

// ============================================================================
// THE WAYLAND HALF
// ============================================================================
//
// Registry binding, the manager, and the listeners. Everything above deals in
// window records; only this section touches `wl_proxy`.

/// The (data, handle, title) signature of the protocol's `title` event.
type TitleFn = unsafe extern "C" fn(*mut c_void, *mut ToplevelHandle, *const c_char);
/// `app_id`.
type AppIdFn = unsafe extern "C" fn(*mut c_void, *mut ToplevelHandle, *const c_char);
/// `output_enter` / `output_leave`.
type OutputFn = unsafe extern "C" fn(*mut c_void, *mut ToplevelHandle, *mut c_void);
/// `state`: the flags arrive as a `wl_array` of `uint32_t`.
type StateFn = unsafe extern "C" fn(*mut c_void, *mut ToplevelHandle, *mut WlArray);
/// `done`, `closed`.
type SimpleFn = unsafe extern "C" fn(*mut c_void, *mut ToplevelHandle);
/// `parent`: another handle, or null.
type ParentFn = unsafe extern "C" fn(*mut c_void, *mut ToplevelHandle, *mut ToplevelHandle);

/// `struct zwlr_foreign_toplevel_handle_v1_listener`, in the protocol's order.
#[repr(C)]
struct HandleListener {
    title: Option<TitleFn>,
    app_id: Option<AppIdFn>,
    output_enter: Option<OutputFn>,
    output_leave: Option<OutputFn>,
    state: Option<StateFn>,
    done: Option<SimpleFn>,
    closed: Option<SimpleFn>,
    parent: Option<ParentFn>,
}

/// `struct zwlr_foreign_toplevel_manager_v1_listener`.
#[repr(C)]
struct ManagerListener {
    toplevel: Option<unsafe extern "C" fn(*mut c_void, *mut ToplevelManager, *mut ToplevelHandle)>,
    finished: Option<unsafe extern "C" fn(*mut c_void, *mut ToplevelManager)>,
}

/// `struct wl_registry_listener`.
#[repr(C)]
struct RegistryListener {
    global: Option<
        unsafe extern "C" fn(*mut c_void, *mut WlRegistry, u32, *const c_char, u32),
    >,
    global_remove: Option<unsafe extern "C" fn(*mut c_void, *mut WlRegistry, u32)>,
}

/// The window behind a handle's `data` pointer.
///
/// # Safety
///
/// `data` must be a pointer from [`lambdock_toplevel_new`].
unsafe fn toplevel_of<'a>(data: *mut c_void) -> Option<&'a mut LambdockToplevel> {
    (data as *mut LambdockToplevel).as_mut()
}

unsafe extern "C" fn handle_title(data: *mut c_void, _handle: *mut ToplevelHandle, title: *const c_char) {
    lambdock_toplevel_set_title(data.cast(), title);
}

unsafe extern "C" fn handle_app_id(data: *mut c_void, _handle: *mut ToplevelHandle, app_id: *const c_char) {
    lambdock_toplevel_set_app_id(data.cast(), app_id);
}

unsafe extern "C" fn handle_output_enter(data: *mut c_void, _handle: *mut ToplevelHandle, _output: *mut c_void) {
    let _ = data;
}

unsafe extern "C" fn handle_output_leave(data: *mut c_void, _handle: *mut ToplevelHandle, _output: *mut c_void) {
    let _ = data;
}

/// Reads the two flags the dock cares about out of the compositor's array.
///
/// # Safety
///
/// `states` must be the array the protocol handed over.
unsafe fn read_state(states: &WlArray) -> (bool, bool) {
    let (mut minimized, mut activated) = (false, false);

    if states.data.is_null() {
        return (minimized, activated);
    }

    // `wl_array_for_each`: `size` counts bytes, and the entries are `uint32_t`.
    let entries = std::slice::from_raw_parts(states.data.cast::<u32>(), states.size / 4);
    for entry in entries {
        match *entry {
            STATE_MINIMIZED => minimized = true,
            STATE_ACTIVATED => activated = true,
            // Maximized and fullscreen do not change what a click should do.
            _ => {}
        }
    }

    (minimized, activated)
}

unsafe extern "C" fn handle_state(data: *mut c_void, _handle: *mut ToplevelHandle, states: *mut WlArray) {
    let Some(states) = states.as_ref() else {
        return;
    };
    if data.is_null() {
        return;
    }

    let (minimized, activated) = read_state(states);

    lambdock_toplevel_set_state(data.cast(), c_int::from(minimized), c_int::from(activated));
}

unsafe extern "C" fn handle_done(data: *mut c_void, _handle: *mut ToplevelHandle) {
    let _ = data;
}

/// The compositor said the window is gone: forget it everywhere, then free it.
unsafe extern "C" fn handle_closed(data: *mut c_void, _handle: *mut ToplevelHandle) {
    let Some(top) = toplevel_of(data) else {
        return;
    };

    log_info!(
        "Tracker",
        "Window closed globally: '{}'",
        crate::cstr::text_or_empty(top.app_id)
    );

    lambdock_toplevel_withdraw(data.cast());

    {
        let mut tracker = tracker().lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        tracker.toplevels.retain(|candidate| *candidate != data.cast());
    }

    if !top.handle.is_null() {
        handle_destroy(top.handle.cast());
        top.handle = std::ptr::null_mut();
    }

    lambdock_toplevel_free(data.cast());
}

/// Bound at version 1, so `parent` (since 3) is never delivered today. It is
/// filled in anyway: a future version bump must not hand libwayland a null handler
/// for an opcode a compositor may send, which aborts the process.
unsafe extern "C" fn handle_parent(
    data: *mut c_void,
    _handle: *mut ToplevelHandle,
    _parent: *mut ToplevelHandle,
) {
    let _ = data;
}

static HANDLE_LISTENER: HandleListener = HandleListener {
    title: Some(handle_title),
    app_id: Some(handle_app_id),
    output_enter: Some(handle_output_enter),
    output_leave: Some(handle_output_leave),
    state: Some(handle_state),
    done: Some(handle_done),
    closed: Some(handle_closed),
    parent: Some(handle_parent),
};

/// A new window the manager reports.
unsafe extern "C" fn manager_toplevel(
    data: *mut c_void,
    _manager: *mut ToplevelManager,
    toplevel: *mut ToplevelHandle,
) {
    let Some(tracker) = tracker_of(data) else {
        return;
    };
    if toplevel.is_null() {
        return;
    }

    let top = lambdock_toplevel_new();
    (*top).handle = toplevel.cast();

    {
        let mut tracker = tracker.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        tracker.toplevels.push(top);
    }

    wl_proxy_add_listener(
        toplevel,
        (&HANDLE_LISTENER as *const HandleListener).cast(),
        top.cast(),
    );
}

/// The manager is gone (a "finished" event): every handle we hold is invalid.
unsafe extern "C" fn manager_finished(data: *mut c_void, _manager: *mut ToplevelManager) {
    let Some(tracker) = tracker_of(data) else {
        return;
    };

    log_info!("Tracker", "Foreign toplevel manager finished; tearing down.");
    tracker_teardown(tracker);
}

static MANAGER_LISTENER: ManagerListener = ManagerListener {
    toplevel: Some(manager_toplevel),
    finished: Some(manager_finished),
};

unsafe extern "C" fn registry_global(
    data: *mut c_void,
    registry: *mut WlRegistry,
    name: u32,
    interface: *const c_char,
    _version: u32,
) {
    let Some(tracker) = tracker_of(data) else {
        return;
    };
    if interface.is_null() {
        return;
    }

    {
        let mut tracker = tracker.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        tracker.globals_seen += 1;
    }

    let interface = std::ffi::CStr::from_ptr(interface);
    let manager_name = std::ffi::CStr::from_ptr(zwlr_foreign_toplevel_manager_v1_interface.name);
    let seat_name = std::ffi::CStr::from_ptr(wl_seat_interface.name);

    if interface == manager_name {
        tracker_bind_manager(tracker, registry, name);
        return;
    }

    if interface == seat_name {
        tracker_bind_seat(tracker, registry, name);
    }
}

unsafe extern "C" fn registry_global_remove(data: *mut c_void, _registry: *mut WlRegistry, name: u32) {
    let Some(tracker) = tracker_of(data) else {
        return;
    };

    let (manager_global, seat_global) = {
        let tracker_ref = tracker.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        (tracker_ref.manager_global, tracker_ref.seat_global)
    };

    if manager_global != 0 && name == manager_global {
        log_info!("Tracker", "Foreign toplevel manager global removed; tearing down.");
        tracker_teardown(tracker);
        return;
    }

    if seat_global != 0 && name == seat_global {
        let mut tracker_ref = tracker.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        if !tracker_ref.seat.is_null() {
            wl_proxy_destroy(tracker_ref.seat.cast());
        }
        tracker_ref.seat = std::ptr::null_mut();
        tracker_ref.seat_global = 0;
    }
}

static REGISTRY_LISTENER: RegistryListener = RegistryListener {
    global: Some(registry_global),
    global_remove: Some(registry_global_remove),
};

/// Binds the manager global and starts listening for the windows it reports.
unsafe fn tracker_bind_manager(tracker: &Mutex<Tracker>, registry: *mut WlRegistry, name: u32) {
    let manager = {
        let tracker_ref = tracker.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        if !tracker_ref.manager.is_null() {
            return;
        }

        registry_bind(
            registry,
            name,
            &zwlr_foreign_toplevel_manager_v1_interface,
            1,
        )
    };

    // No error to report: the caller is a Wayland event, and a failed bind only
    // means this compositor will not be tracked.
    if manager.is_null() {
        return;
    }

    wl_proxy_add_listener(
        manager,
        (&MANAGER_LISTENER as *const ManagerListener).cast(),
        tracker as *const Mutex<Tracker> as *mut c_void,
    );

    let became_connected = {
        let mut tracker_ref = tracker.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        tracker_ref.manager = manager.cast();
        tracker_ref.manager_global = name;

        if !tracker_ref.connected {
            tracker_ref.connected = true;
            true
        } else {
            false
        }
    };

    log_info!("Tracker", "Bound foreign toplevel manager successfully.");

    // Re-binding after a compositor restart: the docks come out of their
    // reconnecting state and the new manager's events rebuild the window list.
    if became_connected {
        notify_docks_of_tracker(true);
    }
}

/// Binds the seat used to activate windows.
unsafe fn tracker_bind_seat(tracker: &Mutex<Tracker>, registry: *mut WlRegistry, name: u32) {
    let seat = {
        let tracker_ref = tracker.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        if !tracker_ref.seat.is_null() {
            return;
        }

        registry_bind(registry, name, &wl_seat_interface, 1)
    };
    if seat.is_null() {
        return;
    }

    let mut tracker_ref = tracker.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    tracker_ref.seat = seat.cast();
    tracker_ref.seat_global = name;

    log_info!("Tracker", "Bound default Wayland seat successfully.");
}

/// Takes the Wayland half down because the compositor withdrew the manager.
///
/// Every handle we hold is invalid from then on, so the bookkeeping goes, the
/// manager proxy is destroyed, and each dock forgets its tracked windows. The
/// registry listener deliberately stays installed -- see
/// [`Tracker::registry_listener_installed`].
unsafe fn tracker_teardown(tracker: &Mutex<Tracker>) {
    let (toplevels, manager) = {
        let mut tracker_ref = tracker.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let toplevels = std::mem::take(&mut tracker_ref.toplevels);
        let manager = tracker_ref.manager;
        tracker_ref.manager = std::ptr::null_mut();
        tracker_ref.manager_global = 0;

        (toplevels, manager)
    };

    // Free every window record, destroying its proxy first: only this side knows
    // what the handle means.
    for top in toplevels {
        if let Some(top_ref) = top.as_ref() {
            if !top_ref.handle.is_null() {
                handle_destroy(top_ref.handle.cast());
            }
        }
        lambdock_toplevel_free(top);
    }

    if !manager.is_null() {
        manager_destroy(manager.cast());
    }

    // Items are configuration and stay exactly where they are; only the window
    // proxies (and the running indicators built from them) go away.
    foreach(dock_forget_all_windows, std::ptr::null_mut());

    let was_connected = {
        let mut tracker_ref = tracker.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let was_connected = tracker_ref.connected;
        tracker_ref.connected = false;
        was_connected
    };

    if was_connected {
        notify_docks_of_tracker(false);
        log_info!(
            "Tracker",
            "Window tracking paused; docks show the reconnecting state"
        );
    }
}

/// `zwlr_foreign_toplevel_handle_v1.destroy`, with libwayland's destroy flag.
unsafe fn handle_destroy(handle: *mut WlProxy) {
    wl_proxy_marshal_flags(
        handle,
        HANDLE_DESTROY,
        std::ptr::null(),
        wl_proxy_get_version(handle),
        MARSHAL_DESTROY,
    );
}

/// `zwlr_foreign_toplevel_manager_v1.destroy`.
unsafe fn manager_destroy(manager: *mut WlProxy) {
    wl_proxy_marshal_flags(
        manager,
        MANAGER_DESTROY,
        std::ptr::null(),
        wl_proxy_get_version(manager),
        MARSHAL_DESTROY,
    );
}

/// Starts the Wayland half.
unsafe fn wayland_tracker_start(error: *mut *mut glib::ffi::GError) -> bool {
    let tracker = tracker();
    {
        let tracker_ref = tracker.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        if tracker_ref.registry_listener_installed {
            return true;
        }
    }

    let Some(display) = gdk_display() else {
        set_error(
            error,
            gio::IOErrorEnum::Failed,
            "The Wayland display is not available yet.",
        );
        return false;
    };

    let wayland_display = gdk_wayland_display_get_wl_display(display);
    if wayland_display.is_null() {
        set_error(
            error,
            gio::IOErrorEnum::Failed,
            "The Wayland display is not available yet.",
        );
        return false;
    }

    let registry = display_get_registry(wayland_display.cast());
    if registry.is_null() {
        set_error(
            error,
            gio::IOErrorEnum::Failed,
            "Could not get the Wayland registry.",
        );
        return false;
    }

    wl_proxy_add_listener(
        registry.cast(),
        (&REGISTRY_LISTENER as *const RegistryListener).cast(),
        tracker as *const Mutex<Tracker> as *mut c_void,
    );

    tracker
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
        .registry_listener_installed = true;

    log_info!("Tracker", "Initialized global toplevel tracker listener.");

    true
}

/// Sets `*error` to a fresh `GError`, when there is somewhere to put it.
fn set_error(error: *mut *mut glib::ffi::GError, code: gio::IOErrorEnum, message: &str) {
    if error.is_null() {
        return;
    }

    let message = crate::cstr::cstring(message);
    unsafe {
        glib::ffi::g_set_error_literal(
            error,
            gio::ffi::g_io_error_quark(),
            code.into_glib(),
            message.as_ptr(),
        );
    }
}

/// Says once, and only when it is a fact, that this compositor exposes no
/// foreign-toplevel manager.
///
/// "A fact" means the registry has announced at least one global: before that a
/// missing manager would only be a guess. GNOME/Mutter is the compositor people
/// hit this on -- the dock still runs there, it just cannot show running
/// applications.
fn warn_if_untrackable(tracker: &Tracker) {
    static WARNED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

    if !tracker.manager.is_null() || tracker.globals_seen == 0 {
        return;
    }
    if WARNED.swap(true, std::sync::atomic::Ordering::Relaxed) {
        return;
    }

    log_warn!(
        "Tracker",
        "This compositor does not implement wlr-foreign-toplevel: running \
         applications and their window menus will not appear."
    );
}

// ============================================================================
// THE PUBLIC API
// ============================================================================

/// Whether the compositor's window list is being tracked right now.
pub fn lambdock_tracker_connected() -> c_int {
    match backend() {
        Backend::Wayland => {
            let connected = tracker()
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner())
                .connected;
            c_int::from(connected)
        }
        // The EWMH window list is a connection of the dock's own, not a
        // compositor-announced global: it has no reconnecting state.
        Backend::X11 => 1,
        Backend::None => 0,
    }
}

/// Starts the tracker for this session: the foreign-toplevel manager on Wayland,
/// the EWMH window list on X11. Calling it again is a no-op rather than a second
/// set of listeners.
///
/// # Safety
///
/// `error` must be null or a `GError**`.
pub unsafe fn init_toplevel_tracker(error: *mut *mut glib::ffi::GError) -> c_int {
    if backend() != Backend::None {
        return 1;
    }

    let Some(display) = gdk_display() else {
        set_error(
            error,
            gio::IOErrorEnum::NotFound,
            "No display connection; window tracking disabled.",
        );
        return 0;
    };

    if display_is(display, gdk_wayland_display_get_type()) {
        if !wayland_tracker_start(error) {
            return 0;
        }

        set_backend(Backend::Wayland);
        return 1;
    }

    if display_is(display, gdk_x11_display_get_type()) {
        if x11_tracker_start(error) == 0 {
            return 0;
        }

        set_backend(Backend::X11);
        return 1;
    }

    set_error(
        error,
        gio::IOErrorEnum::NotSupported,
        "This session exposes no window list; window tracking disabled.",
    );
    0
}

/// Replays every window the tracker knows into one dock, and warns once when this
/// session cannot report any.
///
/// # Safety
///
/// `state` must be null or a live `LambdockState`.
pub unsafe fn sync_window_trackers(state: *mut LambdockState) {
    let Some(state_ref) = state.as_ref() else {
        return;
    };
    if state_ref.track_windows == 0 {
        return;
    }

    match backend() {
        Backend::Wayland => {
            let toplevels = {
                let tracker_ref = tracker().lock().unwrap_or_else(|poisoned| poisoned.into_inner());
                warn_if_untrackable(&tracker_ref);
                tracker_ref.toplevels.clone()
            };

            for top in toplevels {
                // Skip what the compositor has not finished describing: a window
                // with no app id yet has no icon to attach to.
                if let Some(top_ref) = top.as_ref() {
                    if !top_ref.app_id.is_null() {
                        lambdock_toplevel_deliver(top);
                    }
                }
            }
        }
        Backend::X11 => x11_tracker_replay(),
        Backend::None => {}
    }
}

/// Whether this record came from the Wayland backend.
///
/// # Safety
///
/// `window` must be null or a live record.
unsafe fn is_wayland(window: *const LambdockToplevel) -> bool {
    window.as_ref().is_some_and(|window| !window.handle.is_null())
}

/// Requests window activation, un-minimizing it first, so this is also how a dock
/// click restores a window that was tucked away.
///
/// # Safety
///
/// `window` must be null or a live record.
pub unsafe fn activate_toplevel_window(window: *mut LambdockToplevel) {
    if window.is_null() {
        return;
    }

    if !is_wayland(window) {
        x11_tracker_activate(window.cast());
        return;
    }

    let handle = (*window).handle;

    // A minimized window has to be un-minimized as well as activated: the protocol
    // treats the two requests independently, so asking for focus alone would leave
    // a tucked-away window tucked away on some compositors.
    wl_proxy_marshal_flags(
        handle,
        HANDLE_UNSET_MINIMIZED,
        std::ptr::null(),
        wl_proxy_get_version(handle),
        0,
    );

    // `activate` needs the seat the request is made for. A compositor that
    // advertises no `wl_seat` can only be half-asked, so say so once instead of
    // pretending the click did something.
    let (seat, warned) = {
        let mut tracker_ref = tracker().lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let warned = tracker_ref.warned_no_seat;
        tracker_ref.warned_no_seat = true;

        (tracker_ref.seat.cast::<c_void>(), warned)
    };

    if seat.is_null() {
        if !warned {
            log_warn!(
                "Tracker",
                "No wl_seat on this session: the compositor cannot be asked to \
                 focus a window."
            );
        }
        return;
    }

    wl_proxy_marshal_flags(
        handle,
        HANDLE_ACTIVATE,
        std::ptr::null(),
        wl_proxy_get_version(handle),
        0,
        seat,
    );
}

/// Asks the session to minimize the window, which is what "minimize to dock" is.
///
/// # Safety
///
/// `window` must be null or a live record.
pub unsafe fn minimize_toplevel_window(window: *mut LambdockToplevel) {
    if window.is_null() {
        return;
    }

    if !is_wayland(window) {
        x11_tracker_minimize(window.cast());
        return;
    }

    let handle = (*window).handle;
    wl_proxy_marshal_flags(
        handle,
        HANDLE_SET_MINIMIZED,
        std::ptr::null(),
        wl_proxy_get_version(handle),
        0,
    );
}

/// Requests a graceful close, exactly as the window's own close button does.
///
/// # Safety
///
/// `window` must be null or a live record.
pub unsafe fn close_toplevel_window(window: *mut LambdockToplevel) {
    if window.is_null() {
        return;
    }

    if !is_wayland(window) {
        x11_tracker_close(window.cast());
        return;
    }

    let handle = (*window).handle;
    wl_proxy_marshal_flags(
        handle,
        HANDLE_CLOSE,
        std::ptr::null(),
        wl_proxy_get_version(handle),
        0,
    );
}

/// A no-op on purpose: `state->window_trackers` holds *borrowed* records, which
/// the tracker owns and frees when a window closes or the manager goes away.
///
/// # Safety
///
/// `state` is ignored.
pub unsafe fn clear_window_tracker_dock_references(_state: *mut LambdockState) {}

/// A no-op on purpose, for the same reason as
/// [`clear_window_tracker_dock_references`].
///
/// # Safety
///
/// `tracker_ptr` is ignored.
pub unsafe fn free_window_tracker(_tracker_ptr: *mut c_void) {}
