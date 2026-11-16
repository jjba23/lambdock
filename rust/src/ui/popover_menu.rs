// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! How the item menu answers the keyboard: the first half of
//! `src/ui/popover_menu.c` in Rust.
//!
//! A context menu is expected to answer the arrow keys, and to have somewhere
//! sensible to start: the focus lands on the first row when the menu opens, Up and
//! Down move it, Home and End jump, and Escape closes the popover.
//!
//! Both controllers live on the popover widget, which is also what makes them
//! survive a rebuild: a reload replaces the menu's contents, not the popover. They
//! only act on an *item menu* -- a stack panel reuses the same popover and has its
//! own arrow handling in its view, so a menu-level handler there would swallow the
//! keys it needs.
//!
//! What is still C is the menu's content: the rows, the sections (windows, app
//! actions, views, folders, removals) and their callbacks. Those move in their own
//! step.
//!
//! There are no `cargo test` cases here on purpose: every one of these decisions
//! needs a popover with rows in it and a window to hold the focus, and `cargo
//! test` stays display-free. `tests/test_popover_menu.c` drives this code through
//! the C ABI -- its `keyboard` and `escape_controller` subtests are exactly these
//! two controllers.

use crate::app_window::{AppWindow, GList, LambdockToplevel};
use crate::dock_item::{DockItem, ITEM_APP, ITEM_LAUNCHPAD, ITEM_STACK, ITEM_TRASH};
use crate::state::LambdockState;
use crate::ui::dock_dnd::SendRef;
use gtk4::gdk;
use gtk4::glib;
use gtk4::glib::object::ObjectExt;
use gtk4::glib::translate::from_glib_none;
use gtk4::prelude::*;
use gtk4::{ffi, pango};
use std::os::raw::c_int;

/// Class of the popover child a menu builds, which is how the navigation below
/// tells an item menu from a stack panel.
const MENU_CLASS: &str = "popover-menu";

/// Where the marker for "this popover already has its controllers" lives.
const ESCAPE_MARKER: &str = "lambdock-escape-close";
const KEYS_MARKER: &str = "lambdock-menu-keys";

/// Every sensitive, focusable button of the menu, in top-to-bottom order.
fn collect_rows(widget: &gtk4::Widget, out: &mut Vec<gtk4::Widget>) {
    let mut child = widget.first_child();

    while let Some(each) = child {
        if each.is::<gtk4::Button>() && each.is_sensitive() && each.is_focusable() {
            out.push(each.clone());
        }

        collect_rows(&each, out);
        child = each.next_sibling();
    }
}

/// The rows of an item menu, or none when the popover is showing something else.
fn rows(popover: &gtk4::Popover) -> Vec<gtk4::Widget> {
    let mut rows = Vec::new();

    if let Some(child) = popover
        .child()
        .filter(|child| child.has_css_class(MENU_CLASS))
    {
        collect_rows(&child, &mut rows);
    }

    rows
}

/// Index of the row that holds the focus, if any.
fn focused_row(popover: &gtk4::Popover, rows: &[gtk4::Widget]) -> Option<usize> {
    let window = popover
        .root()
        .and_then(|root| root.downcast::<gtk4::Window>().ok())?;
    let focus: gtk4::Widget = gtk4::prelude::GtkWindowExt::focus(&window)?;

    for (index, row) in rows.iter().enumerate() {
        // The focus can sit on the label inside a row's box, so the row is the
        // nearest ancestor that matches -- and the popover itself is the wall.
        let mut widget = Some(focus.clone());
        while let Some(each) = widget {
            if &each == row {
                return Some(index);
            }
            if each == *popover.upcast_ref::<gtk4::Widget>() {
                break;
            }

            widget = each.parent();
        }
    }

    None
}

/// Moves the focus to `index`, clamped into the rows.
fn focus_row(popover: &gtk4::Popover, index: usize) -> bool {
    let rows = rows(popover);

    let Some(row) = rows.get(index.min(rows.len().saturating_sub(1))) else {
        return false;
    };

    row.grab_focus();

    true
}

/// Moves the focus by `delta` rows from wherever it is, or to the end it is
/// heading for when nothing holds it yet.
fn focus_step(popover: &gtk4::Popover, delta: i32) -> bool {
    let rows = rows(popover);
    if rows.is_empty() {
        return false;
    }

    let last = rows.len() as i32 - 1;

    let next = match focused_row(popover, &rows) {
        Some(current) => (current as i32 + delta).clamp(0, last),
        None if delta > 0 => 0,
        None => last,
    };

    focus_row(popover, next as usize)
}

/// Installs Escape dismissal on the popover, once.
///
/// # Safety
///
/// `popover` must be null or a live `GtkPopover`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_popover_close_on_escape(popover: *mut ffi::GtkPopover) {
    if popover.is_null() {
        return;
    }

    // gtk-rs asserts its own initialization flag inside the controller machinery,
    // and C's gtk_init does not set it.
    crate::ui::ensure_gtk_initialized();

    let popover: gtk4::Popover = from_glib_none(popover);

    // Called once per popover, at creation: a reload rebuilds menu contents but
    // reuses the popover widget.
    if popover.data::<bool>(ESCAPE_MARKER).is_some() {
        return;
    }

    let keys = gtk4::EventControllerKey::new();
    keys.set_propagation_phase(gtk4::PropagationPhase::Capture);

    let target = popover.clone();
    keys.connect_key_pressed(move |_, keyval, _, _| {
        if keyval != gdk::Key::Escape {
            return glib::Propagation::Proceed;
        }

        if crate::ui::is_shown(target.upcast_ref()) {
            target.popdown();
        }

        glib::Propagation::Stop
    });

    // The marker goes on the controller as well as on the popover: a controller is
    // what a caller can look up by name (`test_popover_menu` does), and the popover
    // is what makes the install idempotent.
    keys.set_data(ESCAPE_MARKER, true);
    popover.add_controller(keys);
    popover.set_data(ESCAPE_MARKER, true);
}

/// Installs the arrow-key navigation on the popover, once.
///
/// # Safety
///
/// `popover` must be null or a live `GtkPopover`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_popover_menu_keys(popover: *mut ffi::GtkPopover) {
    if popover.is_null() {
        return;
    }

    crate::ui::ensure_gtk_initialized();

    let popover: gtk4::Popover = from_glib_none(popover);

    // Once per popover: a rebuild replaces the contents, not the widget.
    if popover.data::<bool>(KEYS_MARKER).is_some() {
        return;
    }

    let keys = gtk4::EventControllerKey::new();
    keys.set_propagation_phase(gtk4::PropagationPhase::Capture);

    let target = popover.clone();
    keys.connect_key_pressed(move |_, keyval, _, _| {
        let moved = match keyval {
            gdk::Key::Down => focus_step(&target, 1),
            gdk::Key::Up => focus_step(&target, -1),
            gdk::Key::Home => focus_row(&target, 0),
            gdk::Key::End => {
                let last = rows(&target).len().saturating_sub(1);
                focus_row(&target, last)
            }
            _ => return glib::Propagation::Proceed,
        };

        if moved {
            glib::Propagation::Stop
        } else {
            glib::Propagation::Proceed
        }
    });

    popover.add_controller(keys);
    popover.set_data(KEYS_MARKER, true);
}

/// Puts the focus on the menu's first row, if it has one.
///
/// A stack panel focuses its first entry itself, so an item menu is the only one
/// this does anything for.
///
/// # Safety
///
/// `popover` must be null or a live `GtkPopover`.
#[no_mangle]
pub unsafe extern "C" fn lambdock_popover_menu_focus_first(popover: *mut ffi::GtkPopover) {
    if popover.is_null() {
        return;
    }

    crate::ui::ensure_gtk_initialized();

    let popover: gtk4::Popover = from_glib_none(popover);
    focus_row(&popover, 0);
}

/// The row's action icon, or `None` for a row without one.
fn action_icon(action: i32) -> Option<&'static str> {
    match action {
        crate::ui::app_actions::ACTION_NEW_WINDOW => Some("window-new-symbolic"),
        crate::ui::app_actions::ACTION_PRIVATE_WINDOW => Some("view-conceal-symbolic"),
        crate::ui::app_actions::ACTION_PLAY_PAUSE => Some("media-playback-start-symbolic"),
        crate::ui::app_actions::ACTION_PREVIOUS => Some("media-skip-backward-symbolic"),
        crate::ui::app_actions::ACTION_NEXT => Some("media-skip-forward-symbolic"),
        crate::ui::app_actions::ACTION_STOP => Some("media-playback-stop-symbolic"),
        _ => None,
    }
}

/// One row's icon: a symbolic image at the menu's size, centred.
fn menu_icon_new(icon_name: &str) -> gtk4::Widget {
    let image = gtk4::Image::from_icon_name(icon_name);
    image.set_pixel_size(MENU_ICON_SIZE);
    image.set_valign(gtk4::Align::Center);
    image.add_css_class("menu-icon");

    image.upcast()
}

/// One row: a flat button holding the icon and a left-aligned label.
///
/// The icons are what make a menu of five kinds of entry (windows, actions, views,
/// folders, removals) scannable at a glance; the label stays the only thing that
/// names the action, so the menu reads the same to a screen reader.
fn menu_row_new(icon_name: Option<&str>, label: &str) -> gtk4::Button {
    let button = gtk4::Button::new();
    button.add_css_class("flat");
    button.add_css_class("menu-row");

    let row = gtk4::Box::new(gtk4::Orientation::Horizontal, 8);
    row.add_css_class("menu-row-box");

    if let Some(icon_name) = icon_name {
        row.append(&menu_icon_new(icon_name));
    }

    let text = gtk4::Label::new(Some(label));
    text.set_xalign(0.0);
    text.set_hexpand(true);
    text.add_css_class("menu-label");
    row.append(&text);

    button.set_child(Some(&row));

    button
}

/// C strings this module borrows.
type Text = *const std::ffi::c_char;

extern "C" {
    fn activate_toplevel_window(window: *mut LambdockToplevel);
    fn close_toplevel_window(window: *mut LambdockToplevel);
    fn dock_item_from_widget(widget: *mut ffi::GtkWidget) -> *mut DockItem;
    fn dock_item_refresh_trash(item: *mut DockItem);
    fn dock_item_key(item: *const DockItem) -> *mut std::ffi::c_char;
    fn lambdock_item_title(item: *const DockItem, fallback: Text) -> Text;
    fn lambdock_stack_set_view(item: *mut DockItem, view: i32);
    fn lambdock_stack_view_from_string(name: Text) -> i32;
    fn lambdock_launchpad_show(state: *mut LambdockState);
}

/// `POPOVER_*` in `lambdock.h`.
const VBOX_SPACING: i32 = 4;
const VBOX_MARGIN: i32 = 8;
const ROW_SPACING: i32 = 6;
const TITLE_MAX_CHARS: i32 = 30;
const MENU_ICON_SIZE: i32 = 16;

/// `LambdockStackView`.
const STACK_VIEW_GRID: i32 = 0;
const STACK_VIEW_LIST: i32 = 1;

/// A C string as a Rust string, empty when null.
///
/// # Safety
///
/// `pointer` must be null or NUL-terminated.
unsafe fn text(pointer: Text) -> String {
    if pointer.is_null() {
        return String::new();
    }

    std::ffi::CStr::from_ptr(pointer)
        .to_string_lossy()
        .into_owned()
}

/// A translated message, as a Rust string.
unsafe fn translated(msgid: &std::ffi::CStr) -> String {
    let message = crate::glib::tr(msgid);
    if message.is_null() {
        return String::new();
    }

    std::ffi::CStr::from_ptr(message)
        .to_string_lossy()
        .into_owned()
}

/// The item a button belongs to, when it is still on the dock.
unsafe fn item_of(button: &gtk4::Widget) -> *mut DockItem {
    dock_item_from_widget(button.as_ptr().cast())
}

/// Dismisses the item's popover, if it has one.
unsafe fn popdown(item: *mut DockItem) {
    if item.is_null() || (*item).popover.is_null() {
        return;
    }

    let popover: gtk4::Popover = from_glib_none((*item).popover.cast::<ffi::GtkPopover>());
    popover.popdown();
}

/// The popover's padded vertical container.
///
/// The menu class is what tells the popover's key controller that what it holds is
/// an item menu and not a stack panel.
fn menu_vbox_new() -> gtk4::Box {
    let vbox = gtk4::Box::new(gtk4::Orientation::Vertical, VBOX_SPACING);
    vbox.add_css_class(MENU_CLASS);
    vbox.set_margin_start(VBOX_MARGIN);
    vbox.set_margin_end(VBOX_MARGIN);
    vbox.set_margin_top(VBOX_MARGIN);
    vbox.set_margin_bottom(VBOX_MARGIN);

    vbox
}

fn separator() -> gtk4::Separator {
    gtk4::Separator::new(gtk4::Orientation::Horizontal)
}

/// The item's name (or a sensible fallback) and the rule under it.
unsafe fn append_header(vbox: &gtk4::Box, item: *const DockItem) {
    let title = text(lambdock_item_title(item, c"Application".as_ptr()));
    let label = gtk4::Label::new(Some(&title));
    label.add_css_class("title");

    vbox.append(&label);
    vbox.append(&separator());
}

/// One detail row of the inspector: a bold title over a wrapped, selectable value.
fn append_detail_row(vbox: &gtk4::Box, title: &str, value: &str) {
    let label = gtk4::Label::new(None);
    label.set_markup(&format!(
        "<b>{}</b>\n{}",
        glib::markup_escape_text(title),
        glib::markup_escape_text(value)
    ));
    label.set_wrap(true);
    label.set_xalign(0.0);
    label.set_max_width_chars(TITLE_MAX_CHARS);
    label.set_selectable(true);
    label.add_css_class("dim-label");

    vbox.append(&label);
}

/// Whether the icon theme (or, for a path, the file system) can resolve the item's
/// icon.
unsafe fn icon_is_known(item: *const DockItem) -> bool {
    let icon = text((*item).icon);
    if icon.is_empty() {
        return false;
    }

    if std::path::Path::new(&icon).is_absolute() {
        return std::path::Path::new(&icon).exists();
    }

    match gdk::Display::default() {
        Some(display) => gtk4::IconTheme::for_display(&display).has_icon(&icon),
        None => false,
    }
}

/// The trash's count label: "3 items in the trash", in the user's language.
unsafe fn trash_count_text() -> String {
    let count = crate::ui::trash::lambdock_trash_count();
    let message = crate::glib::trn(c"%u item in the trash", c"%u items in the trash", count);
    let format = text(message);

    format.replace("%u", &count.to_string())
}

/// How much is in the trash, and the two things to do about it.
unsafe fn append_trash_section(vbox: &gtk4::Box, item: *mut DockItem) {
    let count = crate::ui::trash::lambdock_trash_count();

    let count_label = gtk4::Label::new(Some(&trash_count_text()));
    count_label.add_css_class("dim-label");
    vbox.append(&count_label);

    let open = menu_row_new(Some("user-trash-symbolic"), &translated(c"Open"));
    open.connect_clicked(move |_| unsafe {
        if crate::ui::trash::lambdock_trash_open() == 0 {
            crate::log_warn!("UI", "Could not open the trash");
        }
        popdown(item);
    });
    vbox.append(&open);

    let empty = menu_row_new(Some("edit-delete-symbolic"), &translated(c"Empty Trash"));
    empty.add_css_class("destructive-action");
    empty.set_sensitive(count > 0);
    empty.connect_clicked(move |button| unsafe {
        popdown(item);

        // Emptying can delete thousands of files, so it does not run on the main
        // thread: the dock would stop drawing (and auto-hiding) until it was done.
        let button_ref = SendRef::new(&button.clone().upcast::<gtk4::Widget>());

        std::thread::spawn(move || {
            let removed = crate::ui::trash::lambdock_trash_empty();

            if removed > 0 {
                crate::log_info!("UI", "Emptied the trash ({removed} items)");
            }

            // Back on the main loop: the item is looked up through its button, so a
            // reload that replaced it in the meantime cannot leave a dangling one.
            // The closure runs on the main loop, which is where the reference it
            // holds is dereferenced.
            glib::MainContext::default().invoke(move || {
                let button = button_ref.take();
                let item = item_of(&button);
                if !item.is_null() {
                    dock_item_refresh_trash(item);
                }
            });
        });

        let _ = item;
    });
    vbox.append(&empty);
}

/// One of the two view buttons: the active one carries a check at the end of its
/// row, which is the vocabulary the rest of the menu uses.
unsafe fn stack_view_button(item: *mut DockItem, view: i32, label: &str) -> gtk4::Button {
    let active = lambdock_stack_view_from_string((*item).stack_view) == view;

    let icon = if view == STACK_VIEW_LIST {
        "view-list-symbolic"
    } else {
        "view-grid-symbolic"
    };

    let button = menu_row_new(Some(icon), label);
    button.add_css_class("view-mode");

    let description = if active {
        translated(c"The stack is shown this way.")
    } else {
        translated(c"Shows the stack this way.")
    };
    crate::accessibility::lambdock_accessible_update(
        button.as_ptr().cast(),
        std::ffi::CString::new(label).unwrap_or_default().as_ptr(),
        std::ffi::CString::new(description)
            .unwrap_or_default()
            .as_ptr(),
    );

    if active {
        button.add_css_class("active");
        if let Some(row) = button
            .child()
            .and_then(|row| row.downcast::<gtk4::Box>().ok())
        {
            row.append(&menu_icon_new("object-select-symbolic"));
        }
    }

    button.connect_clicked(move |_| unsafe {
        if item.is_null() {
            return;
        }

        lambdock_stack_set_view(item, view);

        // Rebuild the menu so the active marker follows the choice.
        populate_popover_menu(item);
    });

    button
}

/// The stack's view switch plus the folder action.
unsafe fn append_stack_section(vbox: &gtk4::Box, item: *mut DockItem) {
    let views = gtk4::Box::new(gtk4::Orientation::Horizontal, 4);
    views.append(&stack_view_button(
        item,
        STACK_VIEW_GRID,
        &translated(c"Grid"),
    ));
    views.append(&stack_view_button(
        item,
        STACK_VIEW_LIST,
        &translated(c"List"),
    ));
    vbox.append(&views);

    let open = menu_row_new(Some("folder-open-symbolic"), &translated(c"Open"));
    open.connect_clicked(move |_| unsafe {
        if item.is_null() {
            return;
        }

        crate::ui::desktop_entry::lambdock_open_path((*item).stack_path);
        popdown(item);
    });
    vbox.append(&open);
}

/// Launchpad: reopen the overlay.
unsafe fn append_launchpad_section(vbox: &gtk4::Box, item: *mut DockItem) {
    let show = menu_row_new(
        Some("view-app-grid-symbolic"),
        &translated(c"Show Launchpad"),
    );

    show.connect_clicked(move |_| unsafe {
        if item.is_null() {
            return;
        }

        popdown(item);
        lambdock_launchpad_show((*item).state);
    });

    vbox.append(&show);
}

/// One row: focus this window, or close it.
///
/// The row carries the window's own state as classes, so the theme can mark the
/// window being looked at and dim the ones the compositor reported as minimized --
/// the two things a list of titles otherwise cannot show. The close button is
/// icon-only and therefore invisible to a screen reader, so it is named after the
/// window it closes.
unsafe fn append_window_row(vbox: &gtk4::Box, item: *mut DockItem, window: *mut AppWindow) {
    let title = text((*window).title);
    let title = if title.is_empty() {
        translated(c"Window")
    } else {
        title
    };

    let row = gtk4::Box::new(gtk4::Orientation::Horizontal, ROW_SPACING);
    row.add_css_class("window-row");
    if (*window).activated != 0 {
        row.add_css_class("current");
    }
    if (*window).minimized != 0 {
        row.add_css_class("minimized");
    }

    // A dot for the window that holds the focus; always present so the titles line
    // up whether or not one of them is current.
    let marker = gtk4::Image::from_icon_name("media-record-symbolic");
    marker.set_pixel_size(6);
    marker.set_valign(gtk4::Align::Center);
    marker.add_css_class("window-current-marker");
    row.append(&marker);

    let handle = (*window).handle;

    let focus = gtk4::Button::with_label(&title);
    focus.add_css_class("flat");
    focus.add_css_class("window-title");
    focus.set_hexpand(true);
    if let Some(label) = focus
        .child()
        .and_then(|child| child.downcast::<gtk4::Label>().ok())
    {
        label.set_ellipsize(pango::EllipsizeMode::End);
        label.set_max_width_chars(TITLE_MAX_CHARS);
        label.set_xalign(0.0);
    }
    crate::accessibility::lambdock_accessible_update(
        focus.as_ptr().cast(),
        std::ffi::CString::new(title.as_str())
            .unwrap_or_default()
            .as_ptr(),
        c"Focus this window.".as_ptr(),
    );
    focus.connect_clicked(move |_| unsafe {
        activate_toplevel_window(handle);
        popdown(item);
    });
    row.append(&focus);

    let close = gtk4::Button::from_icon_name("window-close-symbolic");
    close.add_css_class("flat");
    close.add_css_class("window-close");
    close.set_tooltip_text(Some(&translated(c"Close this window")));
    crate::accessibility::lambdock_accessible_update(
        close.as_ptr().cast(),
        c"Close window".as_ptr(),
        std::ffi::CString::new(title.as_str())
            .unwrap_or_default()
            .as_ptr(),
    );
    close.connect_clicked(move |_| unsafe {
        close_toplevel_window(handle);
        popdown(item);
    });
    row.append(&close);

    vbox.append(&row);
}

/// "New Instance" -- or nothing for a browser, whose own section offers a new
/// window instead.
unsafe fn append_new_instance_row(vbox: &gtk4::Box, item: *mut DockItem, kind: i32) {
    if (*item).exec.is_null() || *(*item).exec == 0 || kind == crate::ui::app_actions::KIND_BROWSER
    {
        return;
    }

    let launch = menu_row_new(Some("window-new-symbolic"), &translated(c"New Instance"));
    launch.connect_clicked(move |_| unsafe {
        if !item.is_null() {
            crate::ui::desktop_entry::lambdock_spawn_command((*item).exec);
            popdown(item);
        }
    });
    vbox.append(&launch);
}

/// "Close All", only for an item that has more than one window.
unsafe fn append_close_all_row(vbox: &gtk4::Box, item: *mut DockItem, window_count: i32) {
    if window_count <= 1 {
        return;
    }

    let close_all = menu_row_new(Some("window-close-symbolic"), &translated(c"Close All"));
    close_all.connect_clicked(move |_| unsafe {
        if item.is_null() {
            return;
        }

        // The handles are copied first: closing a window makes the tracker drop its
        // record, and the list must not be modified while it is walked.
        let mut handles = Vec::new();
        let mut node = (*item).windows.cast::<GList>();
        while !node.is_null() {
            let window = (*node).data.cast::<AppWindow>();
            if !window.is_null() && !(*window).handle.is_null() {
                handles.push((*window).handle);
            }
            node = (*node).next;
        }

        for handle in handles {
            close_toplevel_window(handle);
        }

        popdown(item);
    });
    vbox.append(&close_all);
}

/// The running application's windows, plus what to do about them.
unsafe fn append_window_section(vbox: &gtk4::Box, item: *mut DockItem, kind: i32) {
    if (*item).windows.is_null() {
        // Nothing is running: offer to start it.
        if !(*item).exec.is_null() && *(*item).exec != 0 {
            let launch = menu_row_new(Some("system-run-symbolic"), &translated(c"Open"));
            launch.connect_clicked(move |_| unsafe {
                if !item.is_null() {
                    crate::ui::desktop_entry::lambdock_spawn_command((*item).exec);
                    popdown(item);
                }
            });
            vbox.append(&launch);
        }

        return;
    }

    let mut window_count = 0;
    let mut node = (*item).windows.cast::<GList>();
    while !node.is_null() {
        let window = (*node).data.cast::<AppWindow>();
        if !window.is_null() {
            append_window_row(vbox, item, window);
            window_count += 1;
        }
        node = (*node).next;
    }

    vbox.append(&separator());
    append_new_instance_row(vbox, item, kind);
    append_close_all_row(vbox, item, window_count);
}

/// What kind of application the item is, which actions it offers, and the player
/// those actions would drive.
unsafe fn item_app_actions(item: *mut DockItem) -> (i32, Vec<i32>, Option<String>) {
    let lookup_id = if (*item).desktop_id.is_null() || *(*item).desktop_id == 0 {
        (*item).app_id
    } else {
        (*item).desktop_id
    };
    let entry = crate::ui::desktop_entry::lambdock_desktop_entry_lookup(lookup_id);

    let categories = if entry.is_null() {
        None
    } else {
        Some(text((*entry).categories)).filter(|categories| !categories.is_empty())
    };
    let app_id = if !entry.is_null() && !(*entry).stem.is_null() {
        text((*entry).stem)
    } else {
        text((*item).app_id)
    };
    let name = if !(*item).name.is_null() && *(*item).name != 0 {
        text((*item).name)
    } else if !entry.is_null() {
        text((*entry).name)
    } else {
        String::new()
    };
    let exec = Some(text((*item).exec)).filter(|exec| !exec.is_empty());

    let kind = crate::ui::app_actions::app_kind(
        Some(&app_id),
        Some(&name),
        exec.as_deref(),
        categories.as_deref(),
    );

    let music_bus = if kind == crate::ui::app_actions::KIND_MUSIC {
        crate::ui::app_actions::music_player_find(Some(&app_id), Some(&name))
    } else {
        None
    };

    let actions = crate::ui::app_actions::actions(
        Some(&app_id),
        Some(&name),
        exec.as_deref(),
        categories.as_deref(),
        music_bus.as_deref(),
    );

    (kind, actions, music_bus)
}

/// The kind-specific actions, under a separator: a browser's window commands, a
/// running player's playback controls.
unsafe fn append_action_buttons(
    vbox: &gtk4::Box,
    item: *mut DockItem,
    actions: &[i32],
    music_bus: &Option<String>,
) {
    if actions.is_empty() {
        return;
    }

    vbox.append(&separator());

    for chosen in actions {
        let label = text(crate::ui::app_actions::lambdock_app_action_label(*chosen));
        let button = menu_row_new(action_icon(*chosen).as_deref(), &label);

        let chosen = *chosen;
        let music_bus = music_bus.clone();

        button.connect_clicked(move |_| unsafe {
            if item.is_null() {
                return;
            }

            if crate::ui::app_actions::action_is_music(chosen) {
                let bus = std::ffi::CString::new(music_bus.clone().unwrap_or_default())
                    .unwrap_or_default();
                crate::ui::app_actions::lambdock_app_music_invoke(bus.as_ptr(), chosen);
            } else if let Some(command) =
                crate::ui::app_actions::action_command(Some(&text((*item).exec)), chosen)
            {
                let command = std::ffi::CString::new(command).unwrap_or_default();
                crate::ui::desktop_entry::lambdock_spawn_command(command.as_ptr());
            }

            popdown(item);
        });

        vbox.append(&button);
    }
}

/// The desktop entry the dock attached to this item, if it has one.
unsafe fn item_desktop_entry(
    item: *const DockItem,
) -> *const crate::ui::desktop_entry::DesktopEntry {
    if (*item).desktop_id.is_null() || *(*item).desktop_id == 0 {
        return std::ptr::null();
    }

    crate::ui::desktop_entry::lambdock_desktop_entry_lookup((*item).desktop_id)
}

/// The entry's own launchers, with the names the application gave them.
unsafe fn append_desktop_actions(vbox: &gtk4::Box, item: *mut DockItem) {
    let entry = item_desktop_entry(item);
    if entry.is_null() || (*entry).actions.is_null() || (*(*entry).actions).len == 0 {
        return;
    }

    let actions = (*entry).actions;
    vbox.append(&separator());

    for index in 0..(*actions).len as usize {
        let action = *(*actions).pdata.add(index) as *mut crate::ui::desktop_entry::DesktopAction;
        if action.is_null() {
            continue;
        }

        let name = text((*action).name);
        let command = text((*action).exec);
        let button = menu_row_new(Some("system-run-symbolic"), &name);

        button.connect_clicked(move |_| unsafe {
            let command = std::ffi::CString::new(command.clone()).unwrap_or_default();
            crate::ui::desktop_entry::lambdock_spawn_command(command.as_ptr());

            popdown(item);
        });

        vbox.append(&button);
    }
}

/// "Why is this item here?": the chain an item resolves through, in place.
unsafe fn append_inspector_section(vbox: &gtk4::Box, item: *mut DockItem) {
    vbox.append(&separator());

    let key = dock_item_key(item);
    let config_path = if (*item).state.is_null() {
        std::ptr::null()
    } else {
        (*item)
            .state
            .as_ref()
            .map_or(std::ptr::null(), |state| state.config_path)
    };
    let launcher = crate::core::item_text::lambdock_inspector_launcher_text(config_path, key);
    append_detail_row(vbox, &translated(c"Launcher"), &text(launcher));
    crate::glib::g_free(launcher.cast());
    crate::glib::g_free(key.cast());

    let entry = item_desktop_entry(item);
    let entry_text = if entry.is_null() {
        translated(c"none: the item does not resolve to a desktop entry")
    } else {
        format!("{}\n{}", text((*entry).id), text((*entry).path))
    };
    append_detail_row(vbox, &translated(c"Desktop entry"), &entry_text);

    let icon_text = crate::core::item_text::lambdock_inspector_icon_text(
        (*item).icon,
        c_int::from(icon_is_known(item)),
    );
    append_detail_row(vbox, &translated(c"Icon"), &text(icon_text));
    crate::glib::g_free(icon_text.cast());

    let poller_text = crate::core::item_text::lambdock_inspector_poller_text(
        (*item).poll_name,
        (*item).poll_interval,
    );
    append_detail_row(vbox, &translated(c"Poller"), &text(poller_text));
    crate::glib::g_free(poller_text.cast());
}

/// The inspector's own row, plus the details when it is open.
unsafe fn append_inspector_entry(vbox: &gtk4::Box, item: *mut DockItem) {
    vbox.append(&separator());

    let toggle = menu_row_new(
        Some("help-about-symbolic"),
        &translated(c"Why is this item here?"),
    );
    toggle.connect_clicked(move |_| unsafe {
        if !item.is_null() {
            (*item).show_inspector = c_int::from((*item).show_inspector == 0);
            populate_popover_menu(item);
        }
    });
    vbox.append(&toggle);

    if (*item).show_inspector != 0 {
        append_inspector_section(vbox, item);
    }
}

/// The shared trailing entry that unpins an item from the dock.
unsafe fn append_remove_entry(vbox: &gtk4::Box, item: *mut DockItem) {
    if (*item).is_favorite == 0 {
        return;
    }

    vbox.append(&separator());

    let remove = menu_row_new(
        Some("edit-delete-symbolic"),
        &translated(c"Remove from Dock"),
    );
    remove.add_css_class("destructive-action");
    remove.connect_clicked(move |_| unsafe {
        // The item, its popover and this very button are about to be destroyed, so
        // the actual removal happens from an idle callback (and re-resolves the item
        // through this button when it runs).
        crate::ui::dock_dnd::schedule_remove_dock_item(item);
    });
    vbox.append(&remove);
}

/// Installs `vbox` as the popover's child, with the entries every menu ends with.
unsafe fn menu_finish(item: *mut DockItem, vbox: &gtk4::Box) {
    append_inspector_entry(vbox, item);
    append_remove_entry(vbox, item);

    let popover: gtk4::Popover = from_glib_none((*item).popover.cast::<ffi::GtkPopover>());
    // The same popover carries the stack panel; a menu is not one, so the class
    // that lets the themes style the panel goes away here.
    popover.remove_css_class("stack-panel");
    popover.set_child(Some(vbox));
}

/// Rebuilds the item's context menu from the current state.
///
/// # Safety
///
/// `item` must be null or a live dock item whose popover is set.
#[no_mangle]
pub unsafe extern "C" fn populate_popover_menu(item: *mut DockItem) {
    if item.is_null() || (*item).popover.is_null() {
        return;
    }

    crate::ui::ensure_gtk_initialized();

    let popover: gtk4::Popover = from_glib_none((*item).popover.cast::<ffi::GtkPopover>());

    // Rebuild from scratch: the menu always describes the current state.
    if popover.child().is_some() {
        popover.set_child(None::<&gtk4::Widget>);
    }

    // Arrow-key navigation, installed once per popover; inert while the same
    // popover is showing a stack panel instead.
    lambdock_popover_menu_keys((*item).popover.cast());

    let vbox = menu_vbox_new();
    append_header(&vbox, item);

    match (*item).kind {
        ITEM_TRASH => {
            append_trash_section(&vbox, item);
            menu_finish(item, &vbox);
            return;
        }
        ITEM_STACK => {
            append_stack_section(&vbox, item);
            menu_finish(item, &vbox);
            return;
        }
        ITEM_LAUNCHPAD => {
            append_launchpad_section(&vbox, item);
            menu_finish(item, &vbox);
            return;
        }
        _ => {}
    }

    // Application items may have browser or music-player actions. They are computed
    // before the window list because a browser's "New Window" entry replaces the
    // generic "New Instance" one.
    let (kind, actions, music_bus) = if (*item).kind == ITEM_APP {
        item_app_actions(item)
    } else {
        (crate::ui::app_actions::KIND_NONE, Vec::new(), None)
    };

    append_window_section(&vbox, item, kind);
    append_action_buttons(&vbox, item, &actions, &music_bus);
    append_desktop_actions(&vbox, item);

    menu_finish(item, &vbox);
}
