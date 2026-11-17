// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The X11 (EWMH) backend: `src/x11_tracker.c` in Rust.
//!
//! On Wayland the compositor tells the dock about windows and the layer shell
//! lets it claim an edge. On X11 both are *conventions*: the extended window
//! manager hints, which every desktop there implements. This module speaks them,
//! and nothing else:
//!
//! 1. atoms -- the names EWMH is written in, interned once
//! 2. queries -- read a property off a window; no decisions
//! 3. the list -- which windows exist, tracked by XID
//! 4. events -- one Xlib connection of our own, drained on the main loop
//! 5. requests -- activate, minimize, close
//! 6. the dock -- its window's type, state, geometry and struts
//!
//! What the property values *mean* is `core/x11_rules.rs`, where it is tested
//! without an X server. A build without X11 support compiles the no-op half at the
//! bottom, which is `#[cfg(have_x11)]` against what meson tells cargo -- it is what
//! keeps `dock_window.c` free of `#ifdef`s.
//!
//! Why a private connection: GDK reads the events of its own X connection and does
//! not hand them out, and a dock that wants `PropertyNotify` on the root window
//! cannot ask GDK for it. One extra socket, watched like any other file
//! descriptor, keeps the tracker out of GDK's event path entirely.

// ============================================================================
// WHAT A NO-X11 BUILD LEAVES
// ============================================================================
//
// The build option is a `cfg`, not a runtime check: a GTK built for Wayland alone
// has no Xlib to link.

#[cfg(not(have_x11))]
mod backend {
    use crate::state::LambdockState;
    use gtk4::glib::translate::IntoGlib;
    use std::os::raw::{c_int, c_void};

    pub extern "C" fn lambdock_x11_session() -> c_int {
        0
    }

    /// # Safety
    ///
    /// `error` must be null or a `GError**`.
    pub unsafe extern "C" fn x11_tracker_start(error: *mut *mut gtk4::glib::ffi::GError) -> c_int {
        if !error.is_null() {
            let message = c"no-x11";
            gtk4::glib::ffi::g_set_error_literal(
                error,
                gtk4::gio::ffi::g_io_error_quark(),
                gtk4::gio::IOErrorEnum::NotSupported.into_glib(),
                message.as_ptr(),
            );
        }
        0
    }

    /// # Safety
    ///
    /// Nothing to keep safe: the backend is off.
    pub unsafe extern "C" fn x11_tracker_replay() {}

    /// # Safety
    ///
    /// `window` is ignored.
    pub unsafe extern "C" fn x11_tracker_activate(_window: *mut c_void) {}

    /// # Safety
    ///
    /// `window` is ignored.
    pub unsafe extern "C" fn x11_tracker_minimize(_window: *mut c_void) {}

    /// # Safety
    ///
    /// `window` is ignored.
    pub unsafe extern "C" fn x11_tracker_close(_window: *mut c_void) {}

    /// # Safety
    ///
    /// `state` is ignored.
    pub unsafe extern "C" fn lambdock_x11_dock_apply(_state: *mut LambdockState) {}

    /// # Safety
    ///
    /// `state` is ignored.
    pub unsafe extern "C" fn lambdock_x11_dock_forget(_state: *mut LambdockState) {}
}

#[cfg(not(have_x11))]
pub use backend::*;

// ============================================================================
// THE XLIB SURFACE
// ============================================================================

#[cfg(have_x11)]
mod backend {
    use crate::log_info;
    use crate::log_warn;
    use gtk4::gdk;
    use gtk4::glib;
    use std::os::raw::{c_char, c_int, c_long, c_uint, c_ulong, c_void};

    use crate::app_window::LambdockToplevel;
    use crate::dock_window::{dock_capsule_cross_dim, dock_cross_axis_dim};
    use crate::state::LambdockState;
    use crate::ui::layer_shell::is_monitor_valid;

    use gtk4::glib::translate::IntoGlib;
    use gtk4::prelude::*;
    use gtk4::glib::object::Cast;

    /// Xlib's own types: ids, atoms and timestamps are all machine words.
    type Display = c_void;
    type Window = c_ulong;
    type Atom = c_ulong;

    /// `None`, `CurrentTime`, `Success` and `False`: the same zero.
    const NONE: c_ulong = 0;

    /// ICCCM's predefined atoms, which have fixed numbers.
    const XA_ATOM: Atom = 4;
    const XA_CARDINAL: Atom = 6;
    const XA_STRING: Atom = 31;
    const XA_WINDOW: Atom = 33;
    const XA_WM_NAME: Atom = 39;

    /// Event types, from X.h.
    const DESTROY_NOTIFY: c_int = 17;
    const PROPERTY_NOTIFY: c_int = 28;
    const CLIENT_MESSAGE: c_int = 33;

    /// Event masks.
    const STRUCTURE_NOTIFY_MASK: c_long = 1 << 17;
    const SUBSTRUCTURE_NOTIFY_MASK: c_long = 1 << 19;
    const SUBSTRUCTURE_REDIRECT_MASK: c_long = 1 << 20;
    const PROPERTY_CHANGE_MASK: c_long = 1 << 22;

    /// `PropModeReplace`.
    const PROP_MODE_REPLACE: c_int = 0;

    /// `_NET_WM_STATE` request actions.
    const STATE_REMOVE: c_long = 0;
    const STATE_ADD: c_long = 1;

    /// EWMH's source indication: 2 means "a pager did this", which is what the
    /// dock is, and what lets a window manager apply its focus-stealing policy.
    const SOURCE_PAGER: c_long = 2;

    /// The X protocol errors a tracker reading a window list actually sees: a
    /// window that vanished between the list and the read.
    const BAD_VALUE: u8 = 2;
    const BAD_WINDOW: u8 = 3;
    const BAD_ATOM: u8 = 5;
    const BAD_MATCH: u8 = 8;
    const BAD_DRAWABLE: u8 = 9;

    /// `XEvent` is a union of 24 words; `pad` makes the size right and the
    /// variants give the fields names.
    #[repr(C)]
    union XEvent {
        pad: [u8; 192],
        any: XAnyEvent,
        property: XPropertyEvent,
        destroy: XDestroyWindowEvent,
        client: XClientMessageEvent,
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct XAnyEvent {
        type_: c_int,
        serial: c_ulong,
        send_event: c_int,
        display: *mut Display,
        window: Window,
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct XPropertyEvent {
        type_: c_int,
        serial: c_ulong,
        send_event: c_int,
        display: *mut Display,
        window: Window,
        atom: Atom,
        time: c_ulong,
        state: c_int,
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct XDestroyWindowEvent {
        type_: c_int,
        serial: c_ulong,
        send_event: c_int,
        display: *mut Display,
        event: Window,
        window: Window,
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct XClientMessageEvent {
        type_: c_int,
        serial: c_ulong,
        send_event: c_int,
        display: *mut Display,
        window: Window,
        message_type: Atom,
        format: c_int,
        data: [c_long; 5],
    }

    #[repr(C)]
    struct XErrorEvent {
        type_: c_int,
        display: *mut Display,
        resource_id: c_ulong,
        serial: c_ulong,
        error_code: u8,
        request_code: u8,
        minor_code: u8,
    }

    type ErrorHandler = unsafe extern "C" fn(*mut Display, *mut XErrorEvent) -> c_int;

    extern "C" {
        fn XOpenDisplay(name: *const c_char) -> *mut Display;
        fn XDefaultRootWindow(display: *mut Display) -> Window;
        fn XDefaultScreen(display: *mut Display) -> c_int;
        fn XDisplayWidth(display: *mut Display, screen: c_int) -> c_int;
        fn XDisplayHeight(display: *mut Display, screen: c_int) -> c_int;
        fn XConnectionNumber(display: *mut Display) -> c_int;
        fn XInternAtom(display: *mut Display, name: *const c_char, only_if_exists: c_int) -> Atom;
        fn XGetAtomName(display: *mut Display, atom: Atom) -> *mut c_char;
        fn XSelectInput(display: *mut Display, window: Window, mask: c_long) -> c_int;
        fn XPending(display: *mut Display) -> c_int;
        fn XNextEvent(display: *mut Display, event: *mut XEvent) -> c_int;
        fn XFlush(display: *mut Display) -> c_int;
        fn XFree(data: *mut c_void) -> c_int;
        fn XSetErrorHandler(handler: Option<ErrorHandler>) -> Option<ErrorHandler>;
        fn XGetWindowProperty(
            display: *mut Display,
            window: Window,
            property: Atom,
            long_offset: c_long,
            long_length: c_long,
            delete: c_int,
            req_type: Atom,
            actual_type: *mut Atom,
            actual_format: *mut c_int,
            n_items: *mut c_ulong,
            bytes_after: *mut c_ulong,
            prop: *mut *mut u8,
        ) -> c_int;
        fn XChangeProperty(
            display: *mut Display,
            window: Window,
            property: Atom,
            type_: Atom,
            format: c_int,
            mode: c_int,
            data: *const u8,
            n_elements: c_int,
        ) -> c_int;
        fn XMoveResizeWindow(
            display: *mut Display,
            window: Window,
            x: c_int,
            y: c_int,
            width: c_uint,
            height: c_uint,
        ) -> c_int;
        fn XIconifyWindow(display: *mut Display, window: Window, screen: c_int) -> c_int;
        fn XSendEvent(
            display: *mut Display,
            window: Window,
            propagate: c_int,
            mask: c_long,
            event: *mut XEvent,
        ) -> c_int;
        fn XTranslateCoordinates(
            display: *mut Display,
            src: Window,
            dest: Window,
            src_x: c_int,
            src_y: c_int,
            dest_x: *mut c_int,
            dest_y: *mut c_int,
            child: *mut Window,
        ) -> c_int;
        fn XGetGeometry(
            display: *mut Display,
            drawable: Window,
            root: *mut Window,
            x: *mut c_int,
            y: *mut c_int,
            width: *mut c_uint,
            height: *mut c_uint,
            border: *mut c_uint,
            depth: *mut c_uint,
        ) -> c_int;

        /// `g_unix_fd_add_full`, the watch on the connection's socket.
        fn g_unix_fd_add_full(
            priority: c_int,
            fd: c_int,
            condition: c_uint,
            function: Option<unsafe extern "C" fn(c_int, c_uint, *mut c_void) -> c_int>,
            user_data: *mut c_void,
            notify: Option<unsafe extern "C" fn(*mut c_void)>,
        ) -> c_uint;

        /// GDK's X11 backend, for the surface's window id.
        fn gdk_x11_display_get_type() -> gtk4::glib::ffi::GType;
        fn gdk_x11_surface_get_type() -> gtk4::glib::ffi::GType;
        fn gdk_x11_surface_get_xid(surface: *mut gdk::ffi::GdkSurface) -> Window;

}

    /// Every atom the backend touches, in one table: EWMH's names, plus the two
    /// GTK properties that say what application a window belongs to.
    const ATOM_NAMES: [&std::ffi::CStr; 20] = [
        c"_NET_CLIENT_LIST",
        c"_NET_ACTIVE_WINDOW",
        c"_NET_CLOSE_WINDOW",
        c"_NET_WM_NAME",
        c"_NET_WM_STATE",
        c"_NET_WM_STATE_HIDDEN",
        c"_NET_WM_STATE_SKIP_TASKBAR",
        c"_NET_WM_STATE_ABOVE",
        c"_NET_WM_STATE_STICKY",
        c"_NET_WM_STATE_SKIP_PAGER",
        c"_NET_WM_WINDOW_TYPE",
        c"_NET_WM_WINDOW_TYPE_DOCK",
        c"_NET_WM_PID",
        c"_NET_WM_DESKTOP",
        c"_NET_WM_STRUT",
        c"_NET_WM_STRUT_PARTIAL",
        c"_NET_SUPPORTING_WM_CHECK",
        c"_GTK_APPLICATION_ID",
        c"UTF8_STRING",
        c"WM_CLASS",
    ];

    const ATOM_NET_CLIENT_LIST: usize = 0;
    const ATOM_NET_ACTIVE_WINDOW: usize = 1;
    const ATOM_NET_CLOSE_WINDOW: usize = 2;
    const ATOM_NET_WM_NAME: usize = 3;
    const ATOM_NET_WM_STATE: usize = 4;
    const ATOM_NET_WM_STATE_HIDDEN: usize = 5;
    const ATOM_NET_WM_STATE_SKIP_TASKBAR: usize = 6;
    const ATOM_NET_WM_STATE_ABOVE: usize = 7;
    const ATOM_NET_WM_STATE_STICKY: usize = 8;
    const ATOM_NET_WM_STATE_SKIP_PAGER: usize = 9;
    const ATOM_NET_WM_WINDOW_TYPE: usize = 10;
    const ATOM_NET_WM_WINDOW_TYPE_DOCK: usize = 11;
    const ATOM_NET_WM_PID: usize = 12;
    const ATOM_NET_WM_DESKTOP: usize = 13;
    const ATOM_NET_WM_STRUT: usize = 14;
    const ATOM_NET_WM_STRUT_PARTIAL: usize = 15;
    const ATOM_GTK_APPLICATION_ID: usize = 17;
    const ATOM_UTF8_STRING: usize = 18;
    const ATOM_WM_CLASS: usize = 19;

    /// The tracker's whole state: one private connection, one window table.
    #[derive(Default)]
    struct X11Tracker {
        display: *mut Display,
        root: Window,
        fd_source: c_uint,
        own_pid: c_ulong,
        /// XID -> record, owned here.
        windows: Vec<(Window, *mut LambdockToplevel)>,
        active_window: c_ulong,
        warned_no_wm: bool,
        started: bool,
        atoms: [Atom; 20],
    }

    // SAFETY: every access is from the main loop -- the socket is watched with a
    // main-loop source and Xlib is only called from it -- so the mutex is never
    // contended across threads. It is what lets the tracker live in a `static`.
    unsafe impl Send for X11Tracker {}

    fn tracker() -> &'static std::sync::Mutex<X11Tracker> {
        static TRACKER: std::sync::OnceLock<std::sync::Mutex<X11Tracker>> =
            std::sync::OnceLock::new();

        TRACKER.get_or_init(|| std::sync::Mutex::new(X11Tracker::default()))
    }

    /// The tracker, borrowed.
    ///
    /// `try_lock` rather than `lock`: this tracker is touched from one thread only,
    /// so a second acquisition inside the same statement is a re-entrance, not
    /// contention -- and `std::sync::Mutex` is not reentrant, so `lock` would
    /// deadlock silently where this says which statement did it.
    fn lock() -> std::sync::MutexGuard<'static, X11Tracker> {
        tracker()
            .try_lock()
            .expect("the X11 tracker is re-entered: a statement takes two guards")
    }

    // ------------------------------------------------------------------
    // Queries: property reads only, each FALSE when the property is absent
    // ------------------------------------------------------------------

    /// Reads one property off `window`.
    ///
    /// Xlib only hands over a property whose type matches, which is why every
    /// caller names one.
    unsafe fn get_property(
        window: Window,
        property: Atom,
        type_: Atom,
    ) -> Option<(*mut u8, c_ulong)> {
        let mut actual_type: Atom = NONE;
        let mut actual_format: c_int = 0;
        let mut items: c_ulong = 0;
        let mut bytes_after: c_ulong = 0;
        let mut data: *mut u8 = std::ptr::null_mut();

        let display = lock().display;
        if XGetWindowProperty(
            display,
            window,
            property,
            0,
            4096,
            0,
            type_,
            &mut actual_type,
            &mut actual_format,
            &mut items,
            &mut bytes_after,
            &mut data,
        ) != 0
        {
            return None;
        }

        // Asking for a property a window does not have answers "no data", not an
        // error: the pointer comes back null and the type is None.
        if data.is_null() || actual_type != type_ {
            if !data.is_null() {
                XFree(data.cast());
            }
            return None;
        }

        if items == 0 {
            XFree(data.cast());
            return None;
        }

        Some((data, items))
    }

    /// A UTF-8 string property, as a newly allocated string.
    unsafe fn get_string_property(window: Window, property: Atom) -> Option<String> {
        let utf8_string = lock().atoms[ATOM_UTF8_STRING];

        let (data, items) = match get_property(window, property, utf8_string) {
            Some(found) => found,
            None => {
                // ICCCM's WM_NAME is Latin-1 in the strict reading and UTF-8 in
                // practice; it is a fallback, not the source of truth.
                get_property(window, property, XA_STRING)?
            }
        };

        // The property comes from another client: bound every read by the length
        // the server reported, never by the NULs it was supposed to contain.
        let bytes = std::slice::from_raw_parts(data, items as usize);
        let value = String::from_utf8_lossy(bytes).into_owned();
        XFree(data.cast());

        Some(value)
    }

    /// A property that is a single id: a window in `_NET_ACTIVE_WINDOW`, a process
    /// in `_NET_WM_PID`.
    unsafe fn get_id_property(window: Window, property: Atom, type_: Atom) -> Option<c_ulong> {
        let (data, _) = get_property(window, property, type_)?;
        let value = *(data as *const c_ulong);
        XFree(data.cast());

        Some(value)
    }

    /// A list property, as the server handed it over.
    unsafe fn get_list_property(
        window: Window,
        property: Atom,
        type_: Atom,
    ) -> Option<(*mut c_ulong, c_ulong)> {
        let (data, items) = get_property(window, property, type_)?;

        Some((data as *mut c_ulong, items))
    }

    /// Whether the atom list in `property` holds `atom`.
    unsafe fn atom_list_has(window: Window, property: Atom, atom: Atom) -> bool {
        let Some((atoms, count)) = get_list_property(window, property, XA_ATOM) else {
            return false;
        };

        let entries = std::slice::from_raw_parts(atoms, count as usize);
        let found = entries.contains(&atom);

        XFree(atoms.cast());

        found
    }

    /// Whether any of the window's declared types is one the desktop owns.
    ///
    /// The names are read back from the server because the decision table lives in
    /// `core/x11_rules.rs`, where it is tested; a second copy here would be the copy
    /// nobody updates.
    unsafe fn window_type_is_desktop_chrome(window: Window) -> bool {
        let window_type = lock().atoms[ATOM_NET_WM_WINDOW_TYPE];

        let Some((atoms, count)) = get_list_property(window, window_type, XA_ATOM) else {
            return false;
        };

        let entries = std::slice::from_raw_parts(atoms, count as usize);
        let display = lock().display;
        let mut chrome = false;

        for atom in entries {
            let name = XGetAtomName(display, *atom);
            if !name.is_null() {
                chrome = crate::core::x11_rules::lambdock_x11_type_is_desktop_chrome(name) != 0;
                XFree(name.cast());
            }
            if chrome {
                break;
            }
        }

        XFree(atoms.cast());

        chrome
    }

    /// The application a window belongs to, in the order that identifies it best:
    /// the GTK application id when the toolkit publishes one, then `WM_CLASS`'s
    /// class, then its instance name.
    unsafe fn window_app_id(window: Window) -> Option<String> {
        let (gtk_app_id, wm_class) = {
            let tracker = lock();
            (
                tracker.atoms[ATOM_GTK_APPLICATION_ID],
                tracker.atoms[ATOM_WM_CLASS],
            )
        };

        if let Some(id) = get_string_property(window, gtk_app_id) {
            if !id.is_empty() {
                return Some(id);
            }
        }

        let (data, items) = get_property(window, wm_class, XA_STRING)?;

        // WM_CLASS is two NUL-terminated strings: instance, then class. The
        // property comes from another client, so every read is bounded by the
        // length the server reported rather than by the NULs it should contain.
        let bytes = std::slice::from_raw_parts(data, items as usize);
        let instance_len = bytes.iter().position(|byte| *byte == 0).unwrap_or(bytes.len());
        let instance = String::from_utf8_lossy(&bytes[..instance_len]).into_owned();

        let rest = &bytes[(instance_len + 1).min(bytes.len())..];
        let class_len = rest.iter().position(|byte| *byte == 0).unwrap_or(rest.len());
        let class = String::from_utf8_lossy(&rest[..class_len]).into_owned();

        XFree(data.cast());

        // A window with no WM_CLASS at all has nothing to attach to.
        if class.is_empty() && instance.is_empty() {
            return None;
        }

        Some(if class.is_empty() { instance } else { class })
    }

    /// The window's title, or nothing while it has not set one.
    unsafe fn window_title(window: Window) -> Option<String> {
        // WM_NAME is the ICCCM fallback for toolkits that set nothing else.
        get_string_property(window, lock().atoms[ATOM_NET_WM_NAME])
            .or_else(|| get_string_property(window, XA_WM_NAME))
            .filter(|title| !title.is_empty())
    }

    unsafe fn window_state(window: Window) -> crate::core::x11_rules::WindowState {
        let (atoms, active_window) = {
            let tracker = lock();
            (tracker.atoms, tracker.active_window)
        };

        let hidden = atom_list_has(window, atoms[ATOM_NET_WM_STATE], atoms[ATOM_NET_WM_STATE_HIDDEN]);

        crate::core::x11_rules::lambdock_x11_window_state(
            c_int::from(hidden),
            window,
            active_window,
        )
    }

    // ------------------------------------------------------------------
    // The window list: a diff, not a rebuild
    // ------------------------------------------------------------------

    /// Reads everything the dock wants out of `window` into a new record.
    unsafe fn tracked_window_new(window: Window) -> *mut LambdockToplevel {
        let top = crate::toplevel_tracker::lambdock_toplevel_new();
        (*top).x11_window = window;
        (*top).title = match window_title(window) {
            Some(title) => crate::glib::strdup_string(&title),
            None => std::ptr::null_mut(),
        };

        let state = window_state(window);
        (*top).minimized = state.minimized;
        (*top).activated = state.activated;

        top
    }

    /// Drops one window: the docks forget it, then the record goes.
    unsafe fn tracked_window_drop(xid: Window) {
        let top = {
            let mut tracker = lock();
            match tracker.windows.iter().position(|(id, _)| *id == xid) {
                Some(index) => tracker.windows.remove(index).1,
                None => return,
            }
        };

        log_info!(
            "Tracker",
            "Window closed globally: '{}'",
            crate::cstr::text_or_empty((*top).app_id)
        );

        crate::toplevel_tracker::lambdock_toplevel_withdraw(top);
        crate::toplevel_tracker::lambdock_toplevel_free(top);
    }

    /// Starts tracking `window` when the dock should list it.
    ///
    /// The three properties EWMH spells "do not list me" are read here, in the
    /// order that is cheapest to bail out of: the type list, the taskbar flag and
    /// the pid that says the window is ours.
    unsafe fn tracked_window_consider(window: Window) {
        {
            let tracker = lock();
            if tracker.windows.iter().any(|(id, _)| *id == window) {
                return;
            }
        }

        let desktop_chrome = window_type_is_desktop_chrome(window);

        let (atoms, own_pid) = {
            let tracker = lock();
            (tracker.atoms, tracker.own_pid)
        };

        let skip_taskbar = atom_list_has(
            window,
            atoms[ATOM_NET_WM_STATE],
            atoms[ATOM_NET_WM_STATE_SKIP_TASKBAR],
        );

        let own_window =
            get_id_property(window, atoms[ATOM_NET_WM_PID], XA_CARDINAL).is_some_and(|pid| pid == own_pid);

        let app_id = window_app_id(window);

        if crate::core::x11_rules::lambdock_x11_shows_window(
            c_int::from(desktop_chrome),
            c_int::from(skip_taskbar),
            c_int::from(own_window),
            c_int::from(app_id.is_some()),
        ) == 0
        {
            return;
        }

        let top = tracked_window_new(window);
        (*top).app_id = match app_id {
            Some(app_id) => crate::glib::strdup_string(&app_id),
            None => std::ptr::null_mut(),
        };

        lock().windows.push((window, top));

        // Title and state changes on this window are what keep the icon current.
        let display = lock().display;
        XSelectInput(
            display,
            window,
            PROPERTY_CHANGE_MASK | STRUCTURE_NOTIFY_MASK,
        );

        crate::toplevel_tracker::lambdock_toplevel_deliver(top);
    }

    /// The whole window list, read at once: adds, drops, leaves the rest alone.
    unsafe fn sync_window_list() {
        let root = lock().root;
        let client_list = lock().atoms[ATOM_NET_CLIENT_LIST];

        let listed = get_list_property(root, client_list, XA_WINDOW);
        let windows: Vec<Window> = match listed {
            Some((windows, count)) => {
                let entries = std::slice::from_raw_parts(windows, count as usize).to_vec();
                XFree(windows.cast());
                entries
            }
            None => {
                // No `_NET_CLIENT_LIST` means no EWMH window manager: an X server
                // alone lists nothing. Say so once, and keep showing the pinned
                // items.
                let mut tracker = lock();
                if !tracker.warned_no_wm {
                    tracker.warned_no_wm = true;
                    log_warn!(
                        "Tracker",
                        "This X11 session has no EWMH window manager: running \
                         applications and their window menus will not appear."
                    );
                }
                Vec::new()
            }
        };

        // A window that left the list is gone, whichever way it went.
        let known: Vec<Window> = lock().windows.iter().map(|(id, _)| *id).collect();
        for xid in known {
            if !windows.contains(&xid) {
                tracked_window_drop(xid);
            }
        }

        for window in windows {
            tracked_window_consider(window);
        }
    }

    /// `_NET_ACTIVE_WINDOW` changed: one window gained the focus, one lost it.
    unsafe fn sync_active_window() {
        let (root, active_window_atom) = {
            let tracker = lock();
            (tracker.root, tracker.atoms[ATOM_NET_ACTIVE_WINDOW])
        };

        let active = get_id_property(root, active_window_atom, XA_WINDOW).unwrap_or(0);

        let known: Vec<*mut LambdockToplevel> = {
            let mut tracker = lock();
            tracker.active_window = active;
            tracker.windows.iter().map(|(_, top)| *top).collect()
        };

        for top in known {
            let state = window_state((*top).x11_window);
            crate::toplevel_tracker::lambdock_toplevel_set_state(
                top,
                state.minimized,
                state.activated,
            );
        }
    }

    /// Re-reads the properties that change while a window is open.
    unsafe fn sync_window_properties(window: Window, property: Atom) {
        let top = {
            let tracker = lock();
            match tracker.windows.iter().find(|(id, _)| *id == window) {
                Some((_, top)) => *top,
                None => return,
            }
        };

        let atoms = lock().atoms;

        if property == atoms[ATOM_NET_WM_NAME] || property == XA_WM_NAME {
            let title = window_title(window);
            crate::toplevel_tracker::lambdock_toplevel_set_title(
                top,
                title
                    .map_or(std::ptr::null_mut(), |title| crate::glib::strdup_string(&title)),
            );
            return;
        }

        if property == atoms[ATOM_NET_WM_STATE] {
            let state = window_state(window);
            crate::toplevel_tracker::lambdock_toplevel_set_state(
                top,
                state.minimized,
                state.activated,
            );
            return;
        }

        // A browser that loads its profile late changes the application it
        // belongs to after its window exists: the id has to be re-read, not
        // remembered.
        if property == atoms[ATOM_WM_CLASS] || property == atoms[ATOM_GTK_APPLICATION_ID] {
            let app_id = window_app_id(window);
            crate::toplevel_tracker::lambdock_toplevel_set_app_id(
                top,
                app_id
                    .map_or(std::ptr::null_mut(), |app_id| crate::glib::strdup_string(&app_id)),
            );
        }
    }

    // ------------------------------------------------------------------
    // Events: one file descriptor, drained whenever the server speaks
    // ------------------------------------------------------------------

    /// Counts a window that vanished between the list and the read as "gone".
    ///
    /// Reading a property of a window that has just been destroyed is a race the
    /// server reports as `BadWindow`, and Xlib's default handler prints it and keeps
    /// going -- a line of noise per closed window, which is exactly what a tracker
    /// reading the window list produces. Every other error goes to whichever
    /// handler was installed before this one (GDK's, in practice).
    unsafe extern "C" fn x11_error_handler(
        display: *mut Display,
        event: *mut XErrorEvent,
    ) -> c_int {
        let Some(event) = event.as_ref() else {
            return 0;
        };

        match event.error_code {
            BAD_WINDOW | BAD_DRAWABLE | BAD_MATCH | BAD_ATOM | BAD_VALUE => return 0,
            _ => {}
        }

        match previous_error_handler() {
            Some(previous) => previous(display, event as *const XErrorEvent as *mut XErrorEvent),
            None => 0,
        }
    }

    fn previous_error_handler() -> Option<ErrorHandler> {
        static PREVIOUS: std::sync::Mutex<Option<ErrorHandler>> = std::sync::Mutex::new(None);

        *PREVIOUS.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    fn set_previous_error_handler(handler: Option<ErrorHandler>) {
        static PREVIOUS: std::sync::Mutex<Option<ErrorHandler>> = std::sync::Mutex::new(None);

        *PREVIOUS.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = handler;
    }

    /// Drains the connection whenever the server has something to say.
    ///
    /// Xlib queues what it has read, not what has arrived: drain the socket and the
    /// queue together, so a burst of window events is one pass.
    /// The `GUnixFDSourceFunc` the watch calls: drain, then keep watching.
    unsafe extern "C" fn on_x11_fd_ready(_fd: c_int, _condition: c_uint, _data: *mut c_void) -> c_int {
        on_x11_events();

        // `G_SOURCE_CONTINUE`.
        1
    }

    unsafe fn on_x11_events() {
        let display = lock().display;

        while XPending(display) > 0 {
            let mut event = XEvent { pad: [0; 192] };
            XNextEvent(display, &mut event);

            let kind = event.any.type_;
            match kind {
                PROPERTY_NOTIFY => {
                    let property = event.property;
                    if property.window == lock().root {
                        if property.atom == lock().atoms[ATOM_NET_CLIENT_LIST] {
                            sync_window_list();
                        } else if property.atom == lock().atoms[ATOM_NET_ACTIVE_WINDOW] {
                            sync_active_window();
                        }
                        continue;
                    }

                    sync_window_properties(property.window, property.atom);
                }
                DESTROY_NOTIFY => {
                    let window = event.destroy.window;
                    tracked_window_drop(window);
                }
                _ => {}
            }
        }

    }

    // ------------------------------------------------------------------
    // Requests: the three things the popover and the click policy ask for
    // ------------------------------------------------------------------

    /// Sends one root-window client message, the way EWMH asks for a request.
    unsafe fn send_root_message(target: Window, message: Atom, args: [c_long; 4]) {
        let (display, root) = {
            let tracker = lock();
            (tracker.display, tracker.root)
        };

        let mut event = XEvent {
            client: XClientMessageEvent {
                type_: CLIENT_MESSAGE,
                serial: 0,
                send_event: 0,
                display,
                window: target,
                message_type: message,
                format: 32,
                data: [args[0], args[1], args[2], args[3], 0],
            },
        };

        XSendEvent(
            display,
            root,
            0,
            SUBSTRUCTURE_REDIRECT_MASK | SUBSTRUCTURE_NOTIFY_MASK,
            &mut event,
        );
        XFlush(display);
    }

    /// Adds or removes one `_NET_WM_STATE` flag on a window.
    unsafe fn request_state_change(window: Window, action: c_long, state: Atom, state2: Atom) {
        let atoms = lock().atoms;

        send_root_message(
            window,
            atoms[ATOM_NET_WM_STATE],
            [action, state as c_long, state2 as c_long, SOURCE_PAGER],
        );
    }

    /// # Safety
    ///
    /// `window` must be null or a live record.
    pub unsafe extern "C" fn x11_tracker_activate(window: *mut LambdockToplevel) {
        let Some(window) = window.as_ref() else {
            return;
        };
        if window.x11_window == 0 || lock().display.is_null() {
            return;
        }

        // A minimized window has to be restored as well as raised: the two
        // requests are independent in EWMH, so asking for focus alone would leave
        // a tucked-away window tucked away.
        request_state_change(
            window.x11_window,
            STATE_REMOVE,
            lock().atoms[ATOM_NET_WM_STATE_HIDDEN],
            NONE,
        );

        send_root_message(
            window.x11_window,
            lock().atoms[ATOM_NET_ACTIVE_WINDOW],
            [SOURCE_PAGER, 0, 0, 0],
        );
    }

    /// # Safety
    ///
    /// `window` must be null or a live record.
    pub unsafe extern "C" fn x11_tracker_minimize(window: *mut LambdockToplevel) {
        let Some(window) = window.as_ref() else {
            return;
        };
        if window.x11_window == 0 || lock().display.is_null() {
            return;
        }

        // WM_CHANGE_STATE is the ICCCM request every window manager implements,
        // reparenting or not; EWMH's hidden flag is advisory on top of it.
        let display = lock().display;
        XIconifyWindow(display, window.x11_window, XDefaultScreen(display));
        XFlush(display);
    }

    /// # Safety
    ///
    /// `window` must be null or a live record.
    pub unsafe extern "C" fn x11_tracker_close(window: *mut LambdockToplevel) {
        let Some(window) = window.as_ref() else {
            return;
        };
        if window.x11_window == 0 || lock().display.is_null() {
            return;
        }

        // The same request the window's own close button makes: the application
        // gets to save its work, and can even cancel.
        send_root_message(
            window.x11_window,
            lock().atoms[ATOM_NET_CLOSE_WINDOW],
            [0, SOURCE_PAGER, 0, 0],
        );
    }

    // ------------------------------------------------------------------
    // The dock's own surface
    // ------------------------------------------------------------------

    /// The dock's window id, or 0 when it is not an X11 surface.
    ///
    /// # Safety
    ///
    /// `state` must be null or a live `LambdockState`.
    unsafe fn dock_xid(state: &LambdockState) -> Window {
        let Some(window) = state.window.as_ref() else {
            return 0;
        };
        let Some(window) = window.downcast_ref::<gtk4::Window>() else {
            return 0;
        };

        let Some(surface) = window.surface() else {
            return 0;
        };
        if glib::gobject_ffi::g_type_check_instance_is_a(
            surface.as_ptr().cast(),
            gdk_x11_surface_get_type(),
        ) == 0
        {
            return 0;
        }

        // Deprecated in GTK 4.18 without a replacement: an X11 window id is the one
        // handle a strut, a move and a client message are all addressed to, and GDK
        // offers no other way to name the window it created.
        #[allow(deprecated)]
        gdk_x11_surface_get_xid(surface.as_ptr())
    }

    /// Where the dock's window really is, in root coordinates.
    ///
    /// `XGetGeometry` answers in the window's parent's coordinates, and a window
    /// manager reparents top-level windows into a frame, so the position is
    /// translated to the root rather than read off the geometry call.
    unsafe fn dock_actual_geometry(xid: Window) -> Option<(c_int, c_int, c_int, c_int)> {
        let (display, root) = {
            let tracker = lock();
            (tracker.display, tracker.root)
        };

        let (mut width, mut height) = (0 as c_uint, 0 as c_uint);
        let (mut border, mut depth) = (0 as c_uint, 0 as c_uint);
        let (mut window_x, mut window_y) = (0, 0);
        let mut scratch: Window = NONE;

        if XGetGeometry(
            display,
            xid,
            &mut scratch,
            &mut window_x,
            &mut window_y,
            &mut width,
            &mut height,
            &mut border,
            &mut depth,
        ) == 0
        {
            return None;
        }

        let (mut root_x, mut root_y) = (0, 0);
        if XTranslateCoordinates(
            display,
            xid,
            root,
            0,
            0,
            &mut root_x,
            &mut root_y,
            &mut scratch,
        ) == 0
        {
            return None;
        }

        Some((root_x, root_y, width as c_int, height as c_int))
    }

    /// Whether the strut last published is the one `placement` describes.
    fn strut_published(placement: &crate::core::x11_rules::Placement) -> bool {
        static PUBLISHED: std::sync::Mutex<Option<crate::core::x11_rules::Placement>> =
            std::sync::Mutex::new(None);

        let mut published = PUBLISHED.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        if published.as_ref() == Some(placement) {
            return true;
        }

        *published = Some(*placement);

        false
    }

    /// Writes an array of longs as a 32-bit property.
    unsafe fn set_cardinal_list(window: Window, property: Atom, values: &[c_long]) {
        let display = lock().display;
        XChangeProperty(
            display,
            window,
            property,
            XA_CARDINAL,
            32,
            PROP_MODE_REPLACE,
            values.as_ptr().cast(),
            values.len() as c_int,
        );
    }

    unsafe fn set_atom_value(window: Window, property: Atom, value: Atom) {
        let display = lock().display;
        XChangeProperty(
            display,
            window,
            property,
            XA_ATOM,
            32,
            PROP_MODE_REPLACE,
            (&value as *const Atom).cast(),
            1,
        );
    }

    /// Declares the dock's window as what it is: a dock, above the applications,
    /// pinned to every desktop, and absent from taskbars and pagers.
    ///
    /// The flags are published twice on purpose. As a property, this is what the
    /// window manager reads when it manages the window -- and GTK writes its own
    /// `_NET_WM_STATE` when it maps a toplevel, so the property is set again once
    /// the window is mapped, or GTK's empty list would be the last word. As a
    /// client message, it is what EWMH specifies for a window that is already
    /// mapped, which is also the case for a dock a reload re-presented.
    unsafe fn dock_set_window_state(xid: Window, mapped: bool) {
        let atoms = lock().atoms;

        set_atom_value(
            xid,
            atoms[ATOM_NET_WM_WINDOW_TYPE],
            atoms[ATOM_NET_WM_WINDOW_TYPE_DOCK],
        );

        let states: [Atom; 4] = [
            atoms[ATOM_NET_WM_STATE_STICKY],
            atoms[ATOM_NET_WM_STATE_ABOVE],
            atoms[ATOM_NET_WM_STATE_SKIP_TASKBAR],
            atoms[ATOM_NET_WM_STATE_SKIP_PAGER],
        ];
        XChangeProperty(
            lock().display,
            xid,
            atoms[ATOM_NET_WM_STATE],
            XA_ATOM,
            32,
            PROP_MODE_REPLACE,
            states.as_ptr().cast(),
            states.len() as c_int,
        );

        // 0xFFFFFFFF is "on all desktops", the same as the sticky flag above for
        // window managers that read one and not the other.
        set_cardinal_list(xid, atoms[ATOM_NET_WM_DESKTOP], &[0xFFFF_FFFF]);

        if mapped {
            // A managed window changes its state through the window manager, two
            // flags per message, which is also how a running dock re-asserts itself
            // after a reload re-presented it.
            request_state_change(
                xid,
                STATE_ADD,
                atoms[ATOM_NET_WM_STATE_STICKY],
                atoms[ATOM_NET_WM_STATE_ABOVE],
            );
            request_state_change(
                xid,
                STATE_ADD,
                atoms[ATOM_NET_WM_STATE_SKIP_TASKBAR],
                atoms[ATOM_NET_WM_STATE_SKIP_PAGER],
            );
        }

        XFlush(lock().display);
    }

    /// Publishes the reservation so the work area stops at the dock.
    unsafe fn dock_set_struts(xid: Window, placement: &crate::core::x11_rules::Placement) {
        let atoms = lock().atoms;

        let partial: [c_long; 12] = [
            c_long::from(placement.strut_left),
            c_long::from(placement.strut_right),
            c_long::from(placement.strut_top),
            c_long::from(placement.strut_bottom),
            c_long::from(placement.strut_left_start_y),
            c_long::from(placement.strut_left_end_y),
            c_long::from(placement.strut_right_start_y),
            c_long::from(placement.strut_right_end_y),
            c_long::from(placement.strut_top_start_x),
            c_long::from(placement.strut_top_end_x),
            c_long::from(placement.strut_bottom_start_x),
            c_long::from(placement.strut_bottom_end_x),
        ];
        set_cardinal_list(xid, atoms[ATOM_NET_WM_STRUT_PARTIAL], &partial);

        // The four-value version is the older hint, for window managers that never
        // learned the partial one; the ranges above are simply not expressible.
        let struts: [c_long; 4] = [
            c_long::from(placement.strut_left),
            c_long::from(placement.strut_right),
            c_long::from(placement.strut_top),
            c_long::from(placement.strut_bottom),
        ];
        set_cardinal_list(xid, atoms[ATOM_NET_WM_STRUT], &struts);

        XFlush(lock().display);
    }

    /// The monitor the dock belongs to, in root coordinates.
    ///
    /// GTK reports monitor geometry in application pixels and X in device pixels;
    /// the scale factor is the bridge between them.
    ///
    /// shortcut: fractional scaling is rounded to the integer scale factor here, so
    /// a dock on a 1.5x monitor can be off by a couple of pixels. Upgrade when GTK
    /// exposes a monitor geometry in device pixels.
    unsafe fn dock_monitor_rect(
        state: &LambdockState,
    ) -> Option<crate::core::x11_rules::Rect> {
        let display = gdk::Display::default()?;

        let mut monitor: Option<gdk::Monitor> = state
            .monitor
            .clone()
            .filter(|monitor| is_monitor_valid(monitor.as_ptr()) != 0);

        if monitor.is_none() {
            let surface = state
                .window
                .as_ref()
                .and_then(|window| window.downcast_ref::<gtk4::Window>().cloned())
                .and_then(|window| window.surface());
            monitor = surface.and_then(|surface| display.monitor_at_surface(&surface));
        }

        let owned;
        let monitor = match monitor {
            Some(monitor) => Some(monitor),
            None => {
                let monitors = display.monitors();
                owned = monitors
                    .item(0)
                    .and_then(|monitor| monitor.downcast::<gdk::Monitor>().ok());
                owned.clone()
            }
        };

        let monitor = monitor.as_ref()?;
        let geometry = monitor.geometry();
        let scale = monitor.scale_factor();

        Some(crate::core::x11_rules::Rect {
            x: geometry.x() * scale,
            y: geometry.y() * scale,
            width: geometry.width() * scale,
            height: geometry.height() * scale,
        })
    }

    /// Types, moves and reserves space for the dock's own window.
    ///
    /// # Safety
    ///
    /// `state` must be null or a live `LambdockState`.
    pub unsafe extern "C" fn lambdock_x11_dock_apply(state: *mut LambdockState) {
        let Some(state_ref) = state.as_mut() else {
            return;
        };
        if state_ref.overlay.is_none() || !x11_connect() {
            return;
        }

        let xid = dock_xid(state_ref);
        if xid == 0 {
            return;
        }

        // Typed before the window manager sees the window, and typed again once it
        // is mapped, because mapping is when GTK writes its own window state.
        let mapped = state_ref.window.as_ref()
            .is_some_and(|window| window.is_mapped());

        if state_ref.x11_window != xid || mapped {
            state_ref.x11_window = xid;
            state_ref.x11_placed = 0;
            dock_set_window_state(xid, mapped);
        }

        let (display, screen) = {
            let tracker = lock();
            (tracker.display, XDefaultScreen(tracker.display))
        };
        let root = crate::core::x11_rules::Rect {
            x: 0,
            y: 0,
            width: XDisplayWidth(display, screen),
            height: XDisplayHeight(display, screen),
        };
        let monitor = dock_monitor_rect(state_ref).unwrap_or(root);

        // The overlay's size requests already say how big the dock wants to be: the
        // row plus the room it magnifies into, or -1 for a taskbar, which spans its
        // monitor. Reading them back keeps one source of truth for the dock's size.
        let horizontal = state_ref.orientation == gtk4::Orientation::Horizontal.into_glib();
        let (mut requested_width, mut requested_height) = (-1, -1);
        if let Some(overlay) = state_ref.overlay.as_ref() {
            let (width, height) = (overlay.width(), overlay.height());
            if horizontal {
                requested_width = overlay.width_request();
                requested_height = height;
            } else {
                requested_width = width;
                requested_height = overlay.height_request();
            }
        }

        let cross_px = if horizontal {
            let requested = requested_height_request(state_ref);
            if requested > 0 {
                requested
            } else {
                dock_cross_axis_dim(state_ref)
            }
        } else {
            let requested = requested_width_request(state_ref);
            if requested > 0 {
                requested
            } else {
                dock_cross_axis_dim(state_ref)
            }
        };

        let placement = crate::core::x11_rules::lambdock_x11_placement_compute(
            state_ref.edge,
            root,
            monitor,
            if horizontal { requested_width } else { requested_height },
            cross_px,
            dock_capsule_cross_dim(state_ref),
        );

        // Compare against where the window actually *is*, not against what was
        // asked for last time: GTK sizes and moves its toplevels itself, so a dock
        // that only remembered its own last request could be quietly left wherever
        // GTK put it. Reading the window back makes each call self-correcting.
        let actual = dock_actual_geometry(xid);
        let moved = actual.is_none_or(|(x, y, width, height)| {
            x != placement.x
                || y != placement.y
                || width != placement.width
                || height != placement.height
        });

        if moved {
            let display = lock().display;
            XMoveResizeWindow(
                display,
                xid,
                placement.x,
                placement.y,
                placement.width as c_uint,
                placement.height as c_uint,
            );
            XFlush(display);
        }

        // The strut follows the *capsule*, not the window: the room the icons
        // magnify into is the dock's, not the desktop's.
        if moved || !strut_published(&placement) {
            dock_set_struts(xid, &placement);
        }

        state_ref.x11_placed = 1;
        state_ref.x11_rect_x = placement.x;
        state_ref.x11_rect_y = placement.y;
        state_ref.x11_rect_width = placement.width;
        state_ref.x11_rect_height = placement.height;
    }

    /// The overlay's height request, as a number.
    unsafe fn requested_height_request(state: &LambdockState) -> c_int {
        state.overlay.as_ref().map_or(-1, |overlay| overlay.height_request())
    }

    /// The overlay's width request, as a number.
    unsafe fn requested_width_request(state: &LambdockState) -> c_int {
        state.overlay.as_ref().map_or(-1, |overlay| overlay.width_request())
    }

    /// # Safety
    ///
    /// `state` must be null or a live `LambdockState`.
    pub unsafe extern "C" fn lambdock_x11_dock_forget(state: *mut LambdockState) {
        let Some(state) = state.as_mut() else {
            return;
        };

        state.x11_window = 0;
        state.x11_placed = 0;
    }

    // ------------------------------------------------------------------
    // Public API
    // ------------------------------------------------------------------

    /// Whether this session is an X11 one.
    pub extern "C" fn lambdock_x11_session() -> c_int {
        let Some(display) = gdk::Display::default() else {
            return 0;
        };

        unsafe {
            glib::gobject_ffi::g_type_check_instance_is_a(
                display.as_ptr().cast(),
                gdk_x11_display_get_type(),
            )
        }
    }

    unsafe fn intern_atoms() {
        let display = lock().display;
        let mut atoms = [0 as Atom; 20];
        for (index, name) in ATOM_NAMES.iter().enumerate() {
            atoms[index] = XInternAtom(display, name.as_ptr(), 0);
        }

        lock().atoms = atoms;
    }

    /// Opens the private connection and interns the atoms, once.
    ///
    /// Deliberately separate from starting the tracker: the dock's own window has
    /// to be typed and placed from the moment it is mapped, which is before the
    /// tracker is asked for the window list -- and a session whose window list
    /// cannot be read still gets a dock that reserves its edge.
    unsafe fn x11_connect() -> bool {
        if !lock().display.is_null() {
            return true;
        }

        let Some(gdk_display) = gdk::Display::default() else {
            return false;
        };
        if glib::gobject_ffi::g_type_check_instance_is_a(
            gdk_display.as_ptr().cast(),
            gdk_x11_display_get_type(),
        ) == 0
        {
            return false;
        }

        // A connection of our own: GDK reads the events of its own and hands none
        // of them out, and this backend is built on `PropertyNotify` on the root
        // window. The server allows as many connections as needed, and window ids
        // are server-wide, so the dock's own surface can still be moved from here.
        let name = crate::cstr::cstring(&gdk_display.name());
        let display = XOpenDisplay(name.as_ptr());
        if display.is_null() {
            return false;
        }

        {
            let mut tracker = lock();
            tracker.display = display;
            tracker.root = XDefaultRootWindow(display);
            tracker.own_pid = std::process::id() as c_ulong;
        }

        set_previous_error_handler(XSetErrorHandler(Some(x11_error_handler)));

        intern_atoms();

        true
    }

    /// # Safety
    ///
    /// `error` must be null or a `GError**`.
    pub unsafe extern "C" fn x11_tracker_start(
        error: *mut *mut glib::ffi::GError,
    ) -> c_int {
        if lock().started {
            return 1;
        }

        if !x11_connect() {
            if !error.is_null() {
                let message = c"Could not open the X display.";
                gtk4::glib::ffi::g_set_error_literal(
                    error,
                    gtk4::gio::ffi::g_io_error_quark(),
                    gtk4::gio::IOErrorEnum::Failed.into_glib(),
                    message.as_ptr(),
                );
            }
            return 0;
        }

        let display = lock().display;
        XSelectInput(display, lock().root, PROPERTY_CHANGE_MASK);
        XFlush(display);

        sync_active_window();
        sync_window_list();

        // `G_PRIORITY_DEFAULT` and `G_IO_IN`: the socket is watched the way any
        // other file descriptor is, which is the whole point of the private
        // connection.
        let source = g_unix_fd_add_full(
            0,
            XConnectionNumber(display),
            1,
            Some(on_x11_fd_ready),
            std::ptr::null_mut(),
            None,
        );
        lock().fd_source = source;
        lock().started = true;

        log_info!("Tracker", "Initialized the X11 (EWMH) window list.");

        1
    }

    /// Replays every window the tracker knows into every dock.
    ///
    /// # Safety
    ///
    /// Must be called on the main thread, with the tracker's connection open.
    pub unsafe extern "C" fn x11_tracker_replay() {
        if lock().display.is_null() {
            return;
        }

        let known: Vec<*mut LambdockToplevel> = lock().windows.iter().map(|(_, top)| *top).collect();
        for top in known {
            crate::toplevel_tracker::lambdock_toplevel_deliver(top);
        }
    }
}

#[cfg(have_x11)]
pub use backend::*;
