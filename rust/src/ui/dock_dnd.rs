// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! Drag and drop on the dock: `src/ui/dock_dnd.c` in Rust.
//!
//! Three gestures, one module: dragging an icon inside the row reorders it,
//! dragging it out of the dock removes it, and dropping a `.desktop` file or a
//! folder on the dock adds it as a favourite. A trash item accepts files dragged
//! onto it and trashes them off the main thread.
//!
//! The payload format is this module's (`lambdock-item:<key>`), so a drag that
//! started inside the dock and a drag that came from a file manager can be told
//! apart by the same drop target.

use crate::app_window::GList;
use crate::dock_item::{DockItem, ITEM_SEPARATOR, ITEM_TRASH};
use crate::state::LambdockState;
use crate::ui::desktop_entry::{lambdock_desktop_entry_free, lambdock_desktop_entry_new_from_file};
use crate::ui::dock_item_windows::find_dock_item_by_app_id;
use gtk4::gdk;
use gtk4::gio;
use gtk4::glib;
use gtk4::glib::translate::{from_glib_full, from_glib_none, IntoGlib, ToGlibPtr};
use gtk4::prelude::*;
use gtk4::{ffi, graphene};
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_double, c_int, c_void};
use crate::bridge::{dock_layout_add_item, dock_layout_remove_item, dock_layout_set_order};
use crate::dock_item::{create_dock_item, create_stack_dock_item, dock_item_from_widget, dock_item_key, dock_item_refresh_trash, free_dock_item};
use crate::dock_window::{update_dock_dimensions};

/// The prefix that makes a dragged dock icon its own kind of payload: a drag that
/// started inside the dock carries it, a drag from a file manager does not.
const ITEM_PREFIX: &str = "lambdock-item:";

// Where drop handling reaches back into the modules that are still C.
extern "C" {
    fn g_list_append(list: *mut GList, data: *mut c_void) -> *mut GList;
    /// Only the tests free a list without its elements, so the library build sees
    /// this as unused.
    #[allow(dead_code)]
    fn g_list_free(list: *mut GList);
    fn g_list_free_full(list: *mut GList, free: Option<unsafe extern "C" fn(*mut c_void)>);
    fn g_list_length(list: *mut GList) -> u32;
    fn g_list_find(list: *mut GList, data: *const c_void) -> *mut GList;
    fn g_list_delete_link(list: *mut GList, link: *mut GList) -> *mut GList;
    fn g_list_insert(list: *mut GList, data: *mut c_void, position: c_int) -> *mut GList;
    fn g_list_remove(list: *mut GList, data: *const c_void) -> *mut GList;
    fn g_list_index(list: *mut GList, data: *const c_void) -> c_int;
}

/// Moves `data` to `index`, clamped into the list.
///
/// # Safety
///
/// `list` must be null or a `GList`; `data` must be one of its elements or not in
/// it at all, in which case the list comes back unchanged.
pub unsafe fn dock_list_move(
    list: *mut GList,
    data: *mut c_void,
    index: c_int,
) -> *mut GList {
    let link = g_list_find(list, data);
    if link.is_null() {
        return list;
    }

    let length = g_list_length(list) as c_int;
    let list = g_list_delete_link(list, link);

    // After the removal the list is one element shorter, so the last valid
    // insertion index is length - 1 (which appends).
    let index = index.clamp(0, length - 1);

    g_list_insert(list, data, index)
}

/// The insertion index for a pointer at `position`: the first item whose centre
/// lies past the pointer wins, so dropping on the left half of an icon inserts
/// before it and on the right half inserts after it.
pub fn drop_index_for_position(centers: &[f64], position: f64) -> usize {
    centers
        .iter()
        .position(|centre| position < *centre)
        .unwrap_or(centers.len())
}

/// The insertion index for a pointer, as C asks for it.
///
/// # Safety
///
/// `centers` must be null or point at `n` doubles.
pub unsafe fn dock_drop_index_for_position(
    centers: *const c_double,
    n: c_int,
    position: c_double,
) -> c_int {
    if centers.is_null() || n <= 0 {
        return 0;
    }

    let centers = std::slice::from_raw_parts(centers, n as usize);

    drop_index_for_position(centers, position) as c_int
}

/// The key of an item, or `None` when it has none.
unsafe fn item_key(item: *const DockItem) -> Option<String> {
    if item.is_null() {
        return None;
    }

    let key = dock_item_key(item);
    if key.is_null() {
        return None;
    }

    let text = CStr::from_ptr(key).to_string_lossy().into_owned();
    crate::glib::g_free(key.cast());

    Some(text)
}

/// Stores the current item order in the layout overrides.
///
/// # Safety
///
/// `state` must be null or a live dock.
pub unsafe fn dock_dnd_persist_order(state: *mut LambdockState) {
    if state.is_null() || (*state).config_path.is_null() {
        return;
    }

    let mut keys: *mut GList = std::ptr::null_mut();

    let mut node = (*state).dock_items.cast::<GList>();
    while !node.is_null() {
        let item = (*node).data.cast::<DockItem>();

        // Separators have no stable identity: they are re-placed between their
        // neighbours when the layout is replayed.
        if !item.is_null() && (*item).kind != ITEM_SEPARATOR {
            if let Some(key) = item_key(item) {
                let key = CString::new(key).unwrap_or_default();
                keys = g_list_append(keys, crate::glib::g_strdup(key.as_ptr()).cast());
            }
        }

        node = (*node).next;
    }

    dock_layout_set_order((*state).config_path, keys.cast());
    g_list_free_full(keys, Some(crate::glib::g_free));
}

/// Removes an item: forgets it in the layout overrides, drops it and re-measures
/// the dock.
///
/// # Safety
///
/// `item` must be null or a live item; `state` null or its dock.
pub unsafe fn remove_dock_item(state: *mut LambdockState, item: *mut DockItem) {
    if item.is_null() {
        return;
    }

    let state = if state.is_null() {
        (*item).state
    } else {
        state
    };

    if !state.is_null() {
        if (*item).kind != ITEM_SEPARATOR {
            if let Some(key) = item_key(item) {
                let key = CString::new(key).unwrap_or_default();
                dock_layout_remove_item((*state).config_path, key.as_ptr());
            }
        }

        (*state).dock_items = g_list_remove((*state).dock_items.cast(), item.cast()).cast();
    }

    let name = if (*item).name.is_null() {
        String::from("(unnamed)")
    } else {
        CStr::from_ptr((*item).name).to_string_lossy().into_owned()
    };
    let name = if name.is_empty() {
        String::from("(unnamed)")
    } else {
        name
    };
    crate::log_info!("UI", "Removing dock item: {}", name);

    free_dock_item(item);

    if !state.is_null() {
        update_dock_dimensions(state);
        dock_dnd_persist_order(state);
    }
}

/// Removes an item from an idle callback.
///
/// Destroying an item from inside a handler of one of its own widgets (the context
/// menu's "Remove from Dock", the end of a drag) is unsafe while that widget is
/// still emitting, so the work is deferred and re-resolved through the item's
/// widget data.
///
/// # Safety
///
/// `item` must be null or a live item whose button is set.
pub unsafe fn schedule_remove_dock_item(item: *mut DockItem) {
    if item.is_null() || (*item).button.is_null() {
        return;
    }

    let button: gtk4::Widget = from_glib_none((*item).button.cast::<ffi::GtkWidget>());

    if !(*item).popover.is_null() {
        let popover: gtk4::Popover = from_glib_none((*item).popover.cast::<ffi::GtkPopover>());
        if crate::ui::is_shown(popover.upcast_ref()) {
            popover.popdown();
        }
    }

    // The button is held for as long as the idle is pending, so the item is found
    // through it even if a config reload replaced the list in the meantime.
    glib::idle_add_local_full(glib::Priority::DEFAULT_IDLE, move || {
        let item = dock_item_from_widget(button.as_ptr().cast());

        if !item.is_null() {
            // The puff takes the icon's place before it is removed, so the smoke is
            // where the icon was and the dock keeps its width until it clears.
            crate::ui::poof::lambdock_poof_take_slot(item);
            remove_dock_item((*item).state, item);
        }

        glib::ControlFlow::Break
    });
}

/// The item a dragged key belongs to.
///
/// # Safety
///
/// `state` must be null or a live dock.
unsafe fn find_item_by_key(state: *mut LambdockState, key: &str) -> *mut DockItem {
    if state.is_null() {
        return std::ptr::null_mut();
    }

    let mut node = (*state).dock_items.cast::<GList>();
    while !node.is_null() {
        let item = (*node).data.cast::<DockItem>();

        if item_key(item).is_some_and(|item_key| item_key == key) {
            return item;
        }

        node = (*node).next;
    }

    std::ptr::null_mut()
}

/// The child of the dock's box that stands for `item`.
unsafe fn item_child(item: *const DockItem) -> Option<gtk4::Widget> {
    if (*item).container.is_null() && (*item).button.is_null() {
        return None;
    }

    if (*item).container.is_null() {
        Some(from_glib_none((*item).button.cast::<ffi::GtkWidget>()))
    } else {
        Some(from_glib_none((*item).container.cast::<ffi::GtkWidget>()))
    }
}

/// Moves the item `key` names to the drop position and persists the new order.
///
/// # Safety
///
/// `state` must be a live dock; `key` must be null or a C string. `x` and `y` are
/// window coordinates, as the drop target reports them.
pub unsafe fn dock_drop_handle_item(
    state: *mut LambdockState,
    key: *const c_char,
    x: c_double,
    y: c_double,
) -> c_int {
    if state.is_null() || (*state).box_.is_none() || key.is_null() {
        return 0;
    }

    let key = CStr::from_ptr(key).to_string_lossy();
    let item = find_item_by_key(state, &key);

    if item.is_null() {
        crate::log_warn!("UI", "Dropped dock item is not part of this dock: {}", key);
        return 0;
    }

    let (Some(window), Some(dock_box)) = ((*state).window.clone(), (*state).box_.clone()) else {
        return 0;
    };
    let dock_box: gtk4::Box = match dock_box.downcast() {
        Ok(dock_box) => dock_box,
        Err(_) => return 0,
    };

    let Some(box_point) =
        window.compute_point(&dock_box, &graphene::Point::new(x as f32, y as f32))
    else {
        return 0;
    };

    let horizontal = (*state).orientation == gtk4::Orientation::Horizontal.into_glib();

    // Each child's centre along the dock axis, in the box's own coordinates.
    let mut centers = Vec::new();
    let mut child = dock_box.first_child();
    while let Some(widget) = child {
        if let Some(bounds) = widget.compute_bounds(&dock_box) {
            centers.push(f64::from(if horizontal {
                bounds.x() + bounds.width() / 2.0
            } else {
                bounds.y() + bounds.height() / 2.0
            }));
        }

        child = widget.next_sibling();
    }

    if centers.is_empty() {
        return 0;
    }

    let count = centers.len();
    let drop_index = drop_index_for_position(
        &centers,
        f64::from(if horizontal {
            box_point.x()
        } else {
            box_point.y()
        }),
    ) as c_int;

    let current = g_list_index((*state).dock_items.cast(), item.cast());
    if current < 0 {
        return 0;
    }

    // Removing the item first shifts everything after it one slot left.
    let mut destination = if drop_index > current {
        drop_index - 1
    } else {
        drop_index
    };
    if destination > count as c_int - 1 {
        destination = count as c_int - 1;
    }

    if destination == current {
        return 1;
    }

    (*state).dock_items =
        dock_list_move((*state).dock_items.cast(), item.cast(), destination).cast();

    // Re-order the box children to match the list.
    let mut previous: Option<gtk4::Widget> = None;
    let mut node = (*state).dock_items.cast::<GList>();
    while !node.is_null() {
        let each = (*node).data.cast::<DockItem>();

        if let Some(child) = item_child(each) {
            dock_box.reorder_child_after(&child, previous.as_ref());
            previous = Some(child);
        }

        node = (*node).next;
    }

    dock_dnd_persist_order(state);

    let name = if (*item).name.is_null() {
        String::new()
    } else {
        CStr::from_ptr((*item).name).to_string_lossy().into_owned()
    };
    crate::log_info!(
        "UI",
        "Moved dock item '{}' to position {}",
        name,
        destination
    );

    1
}

/// Adds a dropped file: a folder becomes a stack, a `.desktop` file an application.
///
/// # Safety
///
/// `state` must be a live dock.
unsafe fn add_file(state: *mut LambdockState, file: &gio::File) -> bool {
    let Some(path) = file.path() else {
        return false;
    };
    let path_text = path.to_string_lossy().into_owned();

    if path.is_dir() {
        // A folder becomes a stack, which is the dock's version of the shelf look
        // Downloads stack.
        let name = path
            .file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_default();
        let name_text = CString::new(name).unwrap_or_default();
        let path_c = CString::new(path_text.as_str()).unwrap_or_default();

        create_stack_dock_item(
            state,
            name_text.as_ptr(),
            c"folder".as_ptr(),
            path_c.as_ptr(),
            c"grid".as_ptr(),
            1,
        );
        dock_layout_add_item(
            (*state).config_path,
            c"stack".as_ptr(),
            name_text.as_ptr(),
            c"folder".as_ptr(),
            path_c.as_ptr(),
            c"grid".as_ptr(),
        );
        crate::log_info!("UI", "Added stack for folder: {}", path_text);

        return true;
    }

    if path_text.ends_with(".desktop") {
        let path_c = CString::new(path_text.as_str()).unwrap_or_default();
        let entry = lambdock_desktop_entry_new_from_file(path_c.as_ptr());

        if entry.is_null() {
            crate::log_warn!("UI", "Not a usable desktop entry: {}", path_text);
            return false;
        }

        let name = crate::cstr::text_or_empty((*entry).name);
        let known = !find_dock_item_by_app_id(state, (*entry).id).is_null()
            || !find_dock_item_by_app_id(state, (*entry).name).is_null();

        let added = if known {
            crate::log_info!("UI", "Application is already in the dock: {}", name);
            false
        } else {
            create_dock_item(
                state,
                (*entry).name,
                (*entry).icon,
                (*entry).exec,
                (*entry).id,
                1,
                1,
            );
            dock_layout_add_item(
                (*state).config_path,
                c"app".as_ptr(),
                (*entry).name,
                (*entry).icon,
                (*entry).exec,
                c"".as_ptr(),
            );
            crate::log_info!(
                "UI",
                "Added dock item from desktop entry: {} ({})",
                name,
                crate::cstr::text_or_empty((*entry).exec)
            );
            true
        };

        lambdock_desktop_entry_free(entry);

        return added;
    }

    crate::log_info!(
        "UI",
        "Ignoring dropped file (only .desktop files and folders become dock items): {}",
        path_text
    );

    false
}

/// A C string as a Rust string, empty when unset.
///
/// A `GObject` reference handed to the trash worker and back to the main loop.
///
/// The reference is taken on the main thread before the worker starts and released
/// there when the closure has run, which is what keeps the button alive in between
/// -- the same lifetime the C kept with `g_object_ref`/`g_object_unref`. It is only
/// ever dereferenced on the main thread, which is where the unsafe impl's promise
/// is kept.
pub(crate) struct SendRef(*mut c_void);

unsafe impl Send for SendRef {}

impl SendRef {
    pub(crate) fn new(widget: &gtk4::Widget) -> Self {
        let pointer: *mut ffi::GtkWidget = widget.to_glib_full();
        Self(pointer.cast())
    }

    /// The widget this holds the reference for; the reference moves into it.
    ///
    /// # Safety
    ///
    /// Main thread only, and only once.
    pub(crate) unsafe fn take(&self) -> gtk4::Widget {
        from_glib_full(self.0.cast::<ffi::GtkWidget>())
    }
}

/// The dock's own drop target: a dragged icon reorders, a dragged file is added.
///
/// # Safety
///
/// `state` must be a live dock.
unsafe fn on_dock_drop(state: *mut LambdockState, value: &glib::Value, x: f64, y: f64) -> bool {
    if glib::types::Type::STRING == value.type_() {
        let Some(payload) = value.get::<String>().ok() else {
            return false;
        };

        let Some(key) = payload.strip_prefix(ITEM_PREFIX) else {
            return false;
        };
        let key = CString::new(key).unwrap_or_default();

        return dock_drop_handle_item(state, key.as_ptr(), x, y) != 0;
    }

    if gdk::FileList::static_type() == value.type_() {
        let Ok(files) = value.get::<gdk::FileList>() else {
            return false;
        };

        let mut added = false;
        for file in files.files() {
            added = add_file(state, &file) || added;
        }

        if added {
            update_dock_dimensions(state);
            dock_dnd_persist_order(state);
        }

        return added;
    }

    false
}

/// Makes a trash item accept files dragged onto it.
///
/// The target sits on the item's whole container (reflection included), which is
/// also what makes it win over the dock-wide target: GTK looks for a drop target
/// from the widget under the pointer upwards, and the first one that accepts the
/// payload gets the drop. A drag of a dock icon carries a string the trash
/// ignores, so reordering across the trash still goes to the dock.
///
/// # Safety
///
/// `item` must be a live trash item whose button is set.
unsafe fn setup_trash_drop(item: *mut DockItem) {
    let zone: gtk4::Widget = if (*item).container.is_null() {
        from_glib_none((*item).button.cast::<ffi::GtkWidget>())
    } else {
        from_glib_none((*item).container.cast::<ffi::GtkWidget>())
    };

    // COPY is accepted as well as MOVE: a file manager dragging from a read-only
    // location may only offer COPY, and either way the file ends up trashed.
    let target = gtk4::DropTarget::new(
        gdk::FileList::static_type(),
        gdk::DragAction::COPY | gdk::DragAction::MOVE,
    );

    let zone_widget = zone.clone();
    target.connect_drop(move |_, value, _, _| {
        let Ok(files) = value.get::<gdk::FileList>() else {
            return false;
        };

        let files = files.files();
        if files.is_empty() {
            // An empty drag is not a drop the trash can take.
            return false;
        }

        let item = dock_item_from_widget(zone_widget.as_ptr().cast());
        if item.is_null() || (*item).kind != ITEM_TRASH || (*item).button.is_null() {
            return false;
        }

        let button = SendRef::new(&from_glib_none::<_, gtk4::Widget>(
            (*item).button.cast::<ffi::GtkWidget>(),
        ));

        // Trashing a folder, or a file GIO has to copy instead of renaming because
        // its trash is on another filesystem, can take long enough to be noticed:
        // the dock must keep drawing (and auto-hiding) until it is done.
        std::thread::spawn(move || {
            let total = files.len();
            let mut trashed = 0;

            for file in &files {
                match file.trash(None::<&gio::Cancellable>) {
                    Ok(()) => trashed += 1,
                    Err(error) => {
                        crate::log_warn!("UI", "Could not trash '{}': {error}", file.parse_name())
                    }
                }
            }

            if trashed == 0 {
                return;
            }

            crate::log_info!("UI", "Trashed {trashed} of {total} dropped item(s)");

            // Back on the main loop: the item is looked up through its button, so a
            // reload that replaced it in the meantime cannot leave a dangling one.
            glib::MainContext::default().invoke(move || unsafe {
                let button = button.take();
                let item = dock_item_from_widget(button.as_ptr().cast());
                if !item.is_null() {
                    dock_item_refresh_trash(item);
                }
            });
        });

        true
    });

    zone.add_controller(target);
}

/// Turns an item's button into a drag source.
///
/// # Safety
///
/// `item` must be null or a live item whose button is set.
pub unsafe fn dock_dnd_setup_item(item: *mut DockItem) {
    if item.is_null() || (*item).button.is_null() {
        return;
    }

    crate::ui::ensure_gtk_initialized();

    let button: gtk4::Widget = from_glib_none((*item).button.cast::<ffi::GtkWidget>());

    let source = gtk4::DragSource::new();
    source.set_actions(gdk::DragAction::MOVE);

    let dragged = item;
    source.connect_prepare(move |_, _, _| {
        let key = item_key(dragged)?;
        let payload = format!("{ITEM_PREFIX}{key}");

        Some(gdk::ContentProvider::for_value(&payload.to_value()))
    });

    source.connect_drag_begin(move |_, _| {
        (*dragged).dragging = 1;

        if !(*dragged).button.is_null() {
            let button: gtk4::Widget = from_glib_none((*dragged).button.cast::<ffi::GtkWidget>());
            button.add_css_class("dragging");
        }

        if !(*dragged).popover.is_null() {
            let popover: gtk4::Popover =
                from_glib_none((*dragged).popover.cast::<ffi::GtkPopover>());
            if crate::ui::is_shown(popover.upcast_ref()) {
                popover.popdown();
            }
        }
    });

    let ended = item;
    source.connect_drag_end(move |_, _, _| {
        (*ended).dragging = 0;

        if !(*ended).button.is_null() {
            let button: gtk4::Widget = from_glib_none((*ended).button.cast::<ffi::GtkWidget>());
            button.remove_css_class("dragging");
        }
    });

    let cancelled = item;
    source.connect_drag_cancel(move |_, _, reason| {
        (*cancelled).dragging = 0;

        if !(*cancelled).button.is_null() {
            let button: gtk4::Widget = from_glib_none((*cancelled).button.cast::<ffi::GtkWidget>());
            button.remove_css_class("dragging");
        }

        if reason == gdk::DragCancelReason::NoTarget && !(*cancelled).button.is_null() {
            // Nothing accepted the drop: the icon was dragged off the dock.
            schedule_remove_dock_item(cancelled);
            return true;
        }

        // Escape (or any other cancellation) just aborts the drag.
        false
    });

    button.add_controller(source);

    if (*item).kind == ITEM_TRASH {
        setup_trash_drop(item);
    }
}

/// Installs the dock's own drop target on the window.
///
/// # Safety
///
/// `state` must be null or a live dock whose window is set.
pub unsafe fn dock_dnd_setup(state: *mut LambdockState) {
    if state.is_null() || (*state).window.is_none() {
        return;
    }

    crate::ui::ensure_gtk_initialized();

    let Some(window) = (*state).window.clone() else {
        return;
    };

    // Both kinds of payload are offered to the same drop target: a drag that
    // started inside the dock carries a "lambdock-item:" string, while a drag from
    // a file manager carries a file list.
    let target = gtk4::DropTarget::new(glib::Type::INVALID, gdk::DragAction::MOVE);
    target.set_types(&[glib::Type::STRING, gdk::FileList::static_type()]);

    target.connect_drop(move |_, value, x, y| on_dock_drop(state, value, x, y));

    window.add_controller(target);

    let name = if (*state).config_path.is_null() {
        String::from("default")
    } else {
        CStr::from_ptr((*state).config_path)
            .to_string_lossy()
            .into_owned()
    };
    crate::log_info!("UI", "Drag and drop enabled for [{}]", name);
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The first item whose centre lies past the pointer wins, so a drop on the
    /// left half of an icon inserts before it and on the right half after it.
    #[test]
    fn the_drop_index_is_the_first_centre_past_the_pointer() {
        let centers = [10.0, 30.0, 50.0];

        assert_eq!(drop_index_for_position(&centers, 5.0), 0);
        assert_eq!(drop_index_for_position(&centers, 10.0), 1);
        assert_eq!(drop_index_for_position(&centers, 29.0), 1);
        assert_eq!(drop_index_for_position(&centers, 31.0), 2);
        assert_eq!(drop_index_for_position(&centers, 99.0), 3);

        // Nothing to measure, nothing to insert before.
        assert_eq!(drop_index_for_position(&[], 42.0), 0);
    }

    /// Moving an item is a delete and an insert, and an index past the end
    /// appends rather than dropping the item.
    #[test]
    fn moving_an_item_keeps_it_in_the_list() {
        unsafe {
            let mut list: *mut GList = std::ptr::null_mut();
            let mut values = ['a', 'b', 'c'];
            for value in &mut values {
                list = g_list_append(list, (value as *mut char).cast());
            }

            let a = (&mut values[0] as *mut char).cast();
            let c = (&mut values[2] as *mut char).cast();

            // 'a' to the end: the list is one shorter, so index 3 becomes 2.
            list = dock_list_move(list, a, 3);
            assert_eq!(order(list), ['b', 'c', 'a']);

            // 'c' back to the front.
            list = dock_list_move(list, c, 0);
            assert_eq!(order(list), ['c', 'b', 'a']);

            // An element that is not in the list leaves it alone.
            let mut stranger = 'z';
            let before = order(list);
            list = dock_list_move(list, (&mut stranger as *mut char).cast(), 0);
            assert_eq!(order(list), before);

            g_list_free(list);
        }
    }

    /// The values a list holds, for the assertions above.
    unsafe fn order(list: *mut GList) -> Vec<char> {
        let mut values = Vec::new();
        let mut node = list;

        while !node.is_null() {
            values.push(*((*node).data as *const char));
            node = (*node).next;
        }

        values
    }
}
