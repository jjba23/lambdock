// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The full-screen application grid: `src/ui/launchpad.c` in Rust.
//!
//! There is exactly one launchpad (there is exactly one screen to cover), so it
//! is one model rather than a dock: built on first use, destroyed on close and
//! rescanned every time it opens -- a hidden full-screen layer surface keeps its
//! buffers, and parking it off screen cost about 9 MB a use.
//!
//! The file follows one order:
//!
//! 1. the model, and the accessors the rest of the file reads it through
//! 2. pure helpers -- filtering and the column maths: no widgets, no I/O
//! 3. tiles -- one tile, and the menu of its other installed copies
//! 4. the grid -- filling it, and the arrow keys over it
//! 5. closing -- the backdrop, Escape and the close button
//! 6. the header and the shelf of stacks and the trash
//! 7. the window -- layer shell, search field, showing and hiding
//! 8. the public entry points, which is what `ui/launchpad.h` declares
//!
//! The one rule worth stating: a model borrow is never held across a GTK call
//! that can call back into this file (a popover's "closed", an entry's
//! "search-changed"), so a re-entrance takes the `RefCell` after the last one
//! has let it go.

use gtk4::gdk;
use gtk4::glib;
use gtk4::glib::translate::{from_glib_borrow, from_glib_none, Borrowed, IntoGlibPtr};
use gtk4::prelude::*;
use std::cell::RefCell;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int};

use crate::app_window::GList;
use crate::cstr::{cstring, text, text_or_empty, translated};
use crate::core::config_values::{
    LAUNCHPAD_COLUMNS_AUTO, LAUNCHPAD_COLUMNS_MIN, LAUNCHPAD_ICON_SIZE,
};
use crate::dock_item::{dock_item_from_widget, lambdock_item_title, DockItem, ITEM_STACK, ITEM_TRASH};
use crate::glib::g_free;
use crate::state::LambdockState;
use crate::ui::desktop_entry::{
    lambdock_desktop_entries_free, lambdock_desktop_entries_scan, lambdock_desktop_entry_source_label,
    lambdock_spawn_command, DesktopEntry,
};

/// The grid lives inside these margins: the maths below subtracts them so the
/// last column never hangs off the screen.
const SIDE_MARGIN_PX: c_int = 48;
const GRID_INSET_PX: c_int = 16;

/// A tile is at least this wide, and its icon is this much narrower than the
/// tile so a two-line label fits under it.
const TILE_MIN_WIDTH_PX: c_int = 150;
const TILE_CHROME_PX: c_int = 86;

/// `LAUNCHPAD_AUTO_COLUMNS_*` and `LAUNCHPAD_FALLBACK_COLUMNS` in
/// `ui/launchpad.h`: the band a width-derived column count stays inside, and the
/// answer before the screen can be measured.
const AUTO_COLUMNS_MIN: c_int = 4;
const AUTO_COLUMNS_MAX: c_int = 10;
const FALLBACK_COLUMNS: u32 = 8;

// ============================================================================
// 1. THE MODEL
// ============================================================================

/// How the launchpad looks, resolved from the settings of the dock that opened
/// it and re-read on every show.
#[derive(Clone, Copy)]
struct Appearance {
    icon_size: c_int,
    /// 0 = as many as fit.
    columns: c_int,
    blur: bool,
    /// The close button: the left end unless asked otherwise.
    close_on_right: bool,
}

impl Default for Appearance {
    fn default() -> Self {
        Self {
            icon_size: LAUNCHPAD_ICON_SIZE,
            columns: LAUNCHPAD_COLUMNS_AUTO,
            blur: false,
            close_on_right: false,
        }
    }
}

/// What the launchpad knows about itself.
struct Launchpad {
    window: Option<gtk4::ApplicationWindow>,
    grid: Option<gtk4::FlowBox>,
    search: Option<gtk4::SearchEntry>,
    header: Option<gtk4::Box>,
    close: Option<gtk4::Button>,
    /// The one copy menu that may be open: it is the thing closest to the user,
    /// so Escape closes it before the launchpad.
    open_copy_menu: Option<gtk4::Popover>,
    query: String,
    /// Borrowed: the dock whose shelf the launchpad shows.
    state: *mut LambdockState,
    /// Owned, rescanned on every show.
    entries: *mut glib::ffi::GPtrArray,
    appearance: Appearance,
    /// Tiles per row the grid last laid out; what the arrow keys move by.
    applied_columns: u32,
}

impl Default for Launchpad {
    fn default() -> Self {
        Self {
            window: None,
            grid: None,
            search: None,
            header: None,
            close: None,
            open_copy_menu: None,
            query: String::new(),
            state: std::ptr::null_mut(),
            entries: std::ptr::null_mut(),
            appearance: Appearance::default(),
            applied_columns: FALLBACK_COLUMNS,
        }
    }
}

thread_local! {
    /// One launchpad per process, like the one screen it covers.
    static MODEL: RefCell<Launchpad> = RefCell::new(Launchpad::default());
}

/// Reads the model. Keep the closure to reading: a GTK call inside it may come
/// straight back here.
fn model<R>(read: impl FnOnce(&Launchpad) -> R) -> R {
    MODEL.with_borrow(read)
}

/// Writes the model, on the same terms as [`model`].
fn model_mut<R>(write: impl FnOnce(&mut Launchpad) -> R) -> R {
    MODEL.with_borrow_mut(write)
}

// ============================================================================
// 2. PURE HELPERS
// ============================================================================

/// Casefolds `text` the way the filter compares: GLib's UTF-8-aware casefold,
/// not an ASCII lowercase.
fn casefold(text: &str) -> String {
    extern "C" {
        fn g_utf8_casefold(text: *const c_char, len: isize) -> *mut c_char;
}

    let text = cstring(text);
    unsafe {
        let folded = g_utf8_casefold(text.as_ptr(), -1);
        if folded.is_null() {
            return String::new();
        }

        let folded_text = text_or_empty(folded);
        g_free(folded.cast());

        folded_text
    }
}

/// How many other installed copies `entry` carries.
fn alternate_count(entry: &DesktopEntry) -> usize {
    if entry.alternates.is_null() {
        0
    } else {
        unsafe { (*entry.alternates).len as usize }
    }
}

/// One of `entry`'s other installed copies.
///
/// # Safety
///
/// `entry` must be a live entry, and `index` below its alternate count.
unsafe fn alternate_at(entry: &DesktopEntry, index: usize) -> Option<&DesktopEntry> {
    let pointer = *(*entry.alternates).pdata.add(index);

    pointer.cast::<DesktopEntry>().as_ref()
}

/// Whether `entry` belongs in the grid for `query`.
///
/// # Safety
///
/// `entry` must be null or a live entry; `query` null or NUL-terminated.
pub unsafe fn lambdock_launchpad_entry_matches(
    entry: *const DesktopEntry,
    query: *const c_char,
) -> c_int {
    let Some(entry) = entry.as_ref() else {
        return 0;
    };

    // Nothing typed means everything is shown.
    let Some(query) = text(query) else {
        return 1;
    };
    if query.is_empty() {
        return 1;
    }

    let needle = casefold(&query);

    // Both the name and the command line are searched, so "fire" and
    // "--private-window" find the same entry.
    let name = casefold(&text_or_empty(entry.name));
    let exec = casefold(&text_or_empty(entry.exec));

    c_int::from(name.contains(&needle) || exec.contains(&needle))
}

/// How many tiles fit in a row.
pub fn lambdock_launchpad_columns_for(
    usable_width: c_int,
    icon_size: c_int,
    configured_columns: c_int,
) -> c_int {
    let tile_width = TILE_MIN_WIDTH_PX.max(icon_size + TILE_CHROME_PX);
    let fit = if usable_width > 0 {
        usable_width / tile_width
    } else {
        0
    };

    // A configured count is an upper bound: asking for 14 tiles on a laptop must
    // not push the last ones off the screen.
    if configured_columns > 0 {
        if fit <= 0 {
            return configured_columns;
        }

        return configured_columns.min(fit.max(LAUNCHPAD_COLUMNS_MIN));
    }

    if fit <= 0 {
        return FALLBACK_COLUMNS as c_int;
    }

    fit.clamp(AUTO_COLUMNS_MIN, AUTO_COLUMNS_MAX)
}

/// The tile's tooltip: the command, and why right-clicking is worth it.
fn tile_tooltip_text(entry: &DesktopEntry) -> String {
    let command = unsafe { text_or_empty(entry.exec) };

    if alternate_count(entry) == 0 {
        return command;
    }

    format!(
        "{command}\n{}",
        translated(c"Right-click for the other installed copies.")
    )
}

/// The tile's accessibility description, which depends on the copies rule.
fn tile_description(entry: &DesktopEntry) -> String {
    if alternate_count(entry) > 0 {
        return translated(
            c"Application with more than one installed copy. Press Enter to open it, or use the menu for the other copies.",
        );
    }

    translated(c"Application. Press Enter to open.")
}

// ============================================================================
// 3. TILES
// ============================================================================

/// Tells a screen reader what `widget` is.
fn describe(widget: &impl IsA<gtk4::Widget>, label: &str, description: &str) {
    let label = cstring(label);
    let description = cstring(description);

    unsafe {
        crate::accessibility::lambdock_accessible_update(
            widget.as_ref().as_ptr().cast(),
            label.as_ptr(),
            description.as_ptr(),
        );
    }
}

/// The tile's contents: the icon over its name.
///
/// # Safety
///
/// `entry` must be a live entry.
unsafe fn make_tile_content(entry: &DesktopEntry, icon_size: c_int) -> gtk4::Box {
    let content = gtk4::Box::new(gtk4::Orientation::Vertical, 8);
    content.set_valign(gtk4::Align::Center);

    let theme = crate::ui::icon_theme();
    let image = crate::ui::icon_resolver::create_image_for_icon(
        theme.map_or(std::ptr::null_mut(), |theme| theme.as_ptr().cast()),
        entry.icon,
        icon_size,
    );
    if !image.is_null() {
        let image: gtk4::Widget = from_glib_none(image);
        content.append(&image);
    }

    let label = gtk4::Label::new(text(entry.name).as_deref());
    label.set_ellipsize(gtk4::pango::EllipsizeMode::End);
    label.set_max_width_chars(16);
    label.set_lines(2);
    label.set_justify(gtk4::Justification::Center);
    label.set_valign(gtk4::Align::Center);
    content.append(&label);

    content
}

/// Starts `command`, then closes the launchpad: the same behaviour as the
/// launcher the shelf look imitated.
fn launch_and_close(command: &CStr) {
    unsafe { lambdock_spawn_command(command.as_ptr()) };

    lambdock_launchpad_hide();
}

/// One row of a copy menu: where the copy comes from, and the command it runs.
fn make_copy_row(source: &str, command: &str) -> gtk4::Button {
    let row = gtk4::Button::new();
    row.add_css_class("flat");
    row.add_css_class("copy-row");

    let box_ = gtk4::Box::new(gtk4::Orientation::Vertical, 2);

    let source_label = gtk4::Label::new(Some(source));
    source_label.add_css_class("copy-source");
    source_label.set_xalign(0.0);
    box_.append(&source_label);

    let command_label = gtk4::Label::new(Some(command));
    command_label.add_css_class("copy-command");
    command_label.set_xalign(0.0);
    command_label.set_ellipsize(gtk4::pango::EllipsizeMode::Middle);
    command_label.set_max_width_chars(44);
    box_.append(&command_label);

    row.set_child(Some(&box_));
    row.set_tooltip_text(Some(command));
    describe(&row, source, command);

    let command = cstring(command);
    row.connect_clicked(move |_| launch_and_close(&command));

    row
}

/// One menu row per installed copy, the listed one first.
///
/// # Safety
///
/// `entry` must be a live entry.
unsafe fn append_copy_rows(box_: &gtk4::Box, entry: &DesktopEntry) {
    for index in 0..=alternate_count(entry) {
        let copy = if index == 0 {
            Some(entry)
        } else {
            alternate_at(entry, index - 1)
        };
        let Some(copy) = copy else {
            continue;
        };

        // Where the copy came from is `(transfer full)`: read it, then hand the
        // allocation back to GLib.
        let source = lambdock_desktop_entry_source_label(copy);
        let source_text = text_or_empty(source);
        g_free(source.cast());

        box_.append(&make_copy_row(&source_text, &text_or_empty(copy.exec)));
    }
}

/// The menu that lists every installed copy of `entry`.
///
/// # Safety
///
/// `entry` must be a live entry.
unsafe fn make_copy_menu(entry: &DesktopEntry) -> gtk4::Popover {
    let popover = gtk4::Popover::new();
    popover.set_has_arrow(false);
    popover.add_css_class("launchpad-copy-menu");

    let box_ = gtk4::Box::new(gtk4::Orientation::Vertical, 2);

    let name = text_or_empty(entry.name);
    let title = gtk4::Label::new(Some(&name));
    title.add_css_class("menu-title");
    title.set_xalign(0.0);
    box_.append(&title);

    append_copy_rows(&box_, entry);

    popover.set_child(Some(&box_));
    describe(
        &popover,
        &name,
        &translated(c"Choose which installed copy of this application to open."),
    );

    popover
}

/// Shows `menu` as the one open copy menu.
fn copy_menu_show(menu: &gtk4::Popover) {
    copy_menu_close();

    // The pointer is stored before the popup, which may report it closed again
    // on a compositor that never maps it.
    model_mut(|launchpad| launchpad.open_copy_menu = Some(menu.clone()));

    menu.popup();
}

/// Closes the open copy menu; `true` when there was one.
fn copy_menu_close() -> bool {
    // Taking the pointer first is what lets the "closed" handler borrow the
    // model from inside `popdown`.
    let Some(menu) = model_mut(|launchpad| launchpad.open_copy_menu.take()) else {
        return false;
    };

    if crate::ui::is_shown(menu.upcast_ref()) {
        menu.popdown();
    }

    true
}

/// The Menu key and Shift+F10 are the keyboard's right click.
fn key_opens_copy_menu(keyval: gdk::Key, modifiers: gdk::ModifierType) -> bool {
    keyval == gdk::Key::Menu
        || (keyval == gdk::Key::F10 && modifiers.contains(gdk::ModifierType::SHIFT_MASK))
}

/// Gives `tile` the copy menu of `entry`, and the two ways of opening it.
///
/// # Safety
///
/// `entry` must be a live entry.
unsafe fn tile_add_copy_menu(tile: &gtk4::Button, entry: &DesktopEntry) -> gtk4::Popover {
    let menu = make_copy_menu(entry);

    // At most one copy menu is open in the process, so the bookkeeping belongs
    // to the launchpad -- including for a tile built on its own.
    menu.set_parent(tile);
    crate::ui::popover_menu::lambdock_popover_close_on_escape(menu.as_ptr().cast());
    menu.connect_closed(|menu| {
        model_mut(|launchpad| {
            if launchpad.open_copy_menu.as_ref() == Some(menu) {
                launchpad.open_copy_menu = None;
            }
        });
    });

    let menu_for_click = menu.clone();
    let right_click = gtk4::GestureClick::new();
    right_click.set_button(gdk::BUTTON_SECONDARY);
    right_click.connect_pressed(move |_, _, _, _| copy_menu_show(&menu_for_click));
    tile.add_controller(right_click);

    // Capture: the tile must see the Menu key even when the flow box would use
    // it first.
    let menu_for_keys = menu.clone();
    let keys = gtk4::EventControllerKey::new();
    keys.set_propagation_phase(gtk4::PropagationPhase::Capture);
    keys.connect_key_pressed(move |_, keyval, _, modifiers| {
        if !key_opens_copy_menu(keyval, modifiers) {
            return glib::Propagation::Proceed;
        }

        copy_menu_show(&menu_for_keys);

        glib::Propagation::Stop
    });
    tile.add_controller(keys);

    menu
}

/// One tile, with the copy menu it owns parented to it.
///
/// # Safety
///
/// `entry` must be null or a live entry.
unsafe fn tile_new(entry: *const DesktopEntry, icon_size: c_int) -> Option<gtk4::Button> {
    let entry = entry.as_ref()?;

    let button = gtk4::Button::new();
    button.add_css_class("flat");
    button.add_css_class("launchpad-tile");
    button.set_size_request(120.max(icon_size + 56), -1);
    button.set_child(Some(&make_tile_content(entry, icon_size)));

    button.set_tooltip_text(Some(&tile_tooltip_text(entry)));
    describe(
        &button,
        &text_or_empty(entry.name),
        &tile_description(entry),
    );

    // The command travels with the tile: the spawn helper logs it, and the
    // launchpad closes once something has started.
    let command = cstring(&text_or_empty(entry.exec));
    button.connect_clicked(move |_| launch_and_close(&command));

    // The copies travel with the entry and are reachable from the tile and
    // nowhere else: the tile owns the menu from here on.
    if alternate_count(entry) > 0 {
        tile_add_copy_menu(&button, entry);
    }

    Some(button)
}

// ============================================================================
// 4. THE GRID
// ============================================================================

/// A widget borrowed from a pointer GTK's own child walk handed out.
///
/// # Safety
///
/// `pointer` must be a live widget that outlives the borrow.
unsafe fn borrow_widget(pointer: *mut gtk4::ffi::GtkWidget) -> Borrowed<gtk4::Widget> {
    from_glib_borrow(pointer)
}

/// The tile inside `wrapper`, or null when `wrapper` holds something else -- the
/// "nothing matched" label lives in the same flow box.
fn tile_in(wrapper: &gtk4::Widget) -> *mut gtk4::ffi::GtkWidget {
    if wrapper.has_css_class("launchpad-tile") {
        return wrapper.as_ptr();
    }

    // A flow box wraps what it is given, so the tile is usually one level in.
    if let Some(child) = crate::ui::flow_box_child(wrapper) {
        if child.has_css_class("launchpad-tile") {
            return child.as_ptr();
        }
    }

    std::ptr::null_mut()
}

/// The copy menu a tile parents, if it has one.
fn copy_menu_of(tile: &gtk4::Widget) -> Option<Borrowed<gtk4::Widget>> {
    let mut child = crate::ui::first_child_of(tile);

    while let Some(node) = child {
        if node.is::<gtk4::Popover>() {
            return Some(node);
        }

        child = crate::ui::next_sibling_of(&node);
    }

    None
}

fn tile_count(grid: &gtk4::Widget) -> usize {
    let mut count = 0;
    let mut child = crate::ui::first_child_of(grid);

    while let Some(node) = child {
        if !tile_in(&node).is_null() {
            count += 1;
        }

        child = crate::ui::next_sibling_of(&node);
    }

    count
}

/// The `index`-th application tile, skipping the "nothing matched" label.
fn tile_at(grid: &gtk4::Widget, index: usize) -> *mut gtk4::ffi::GtkWidget {
    let mut seen = 0;
    let mut child = crate::ui::first_child_of(grid);

    while let Some(node) = child {
        let tile = tile_in(&node);
        if !tile.is_null() {
            if seen == index {
                return tile;
            }

            seen += 1;
        }

        child = crate::ui::next_sibling_of(&node);
    }

    std::ptr::null_mut()
}

/// Which tile holds the focus, or `None` when the focus is elsewhere -- which is
/// where it starts, in the search field.
fn focused_tile(grid: &gtk4::Widget) -> Option<usize> {
    let root = grid.root()?;
    let window = root.downcast_ref::<gtk4::Window>()?;
    let focus = gtk4::prelude::GtkWindowExt::focus(window)?;

    // Walk up from the focus to the tile it belongs to; asking every widget in
    // the grid would be the same answer for more work.
    let mut current = focus.as_ptr();
    let mut focused = std::ptr::null_mut();

    while !current.is_null() {
        let widget = unsafe { borrow_widget(current) };
        if tile_in(&widget) == current {
            focused = current;
            break;
        }
        if current == grid.as_ptr() {
            break;
        }

        let Some(parent) = crate::ui::parent_of(&widget) else {
            break;
        };
        current = parent.as_ptr();
    }

    if focused.is_null() {
        return None;
    }

    let mut index = 0;
    let mut child = crate::ui::first_child_of(grid);

    while let Some(node) = child {
        let tile = tile_in(&node);
        if !tile.is_null() {
            if tile == focused {
                return Some(index);
            }

            index += 1;
        }

        child = crate::ui::next_sibling_of(&node);
    }

    None
}

fn focus_tile(grid: &gtk4::Widget, index: usize) -> bool {
    let target = tile_at(grid, index);
    if target.is_null() {
        return false;
    }

    unsafe { borrow_widget(target) }.grab_focus();

    true
}

/// Moves the focus by `delta` tiles, clamped to the grid's ends.
fn move_focus(grid: &gtk4::Widget, delta: i64) -> bool {
    let count = tile_count(grid);
    if count == 0 {
        return false;
    }

    let Some(current) = focused_tile(grid) else {
        return focus_tile(grid, 0);
    };

    let next = (current as i64 + delta).clamp(0, count as i64 - 1) as usize;
    if next == current {
        return true;
    }

    focus_tile(grid, next)
}

/// Applies `columns` to the flow box and remembers them for the arrow keys.
fn apply_columns(columns: c_int) {
    let columns = columns.max(1) as u32;

    model_mut(|launchpad| {
        launchpad.applied_columns = columns;

        if let Some(grid) = &launchpad.grid {
            grid.set_min_children_per_line(columns);
            grid.set_max_children_per_line(columns);
        }
    });
}

/// The grid's arrow keys: movement is by tile index, over the columns the grid
/// actually laid out, rather than over a fixed count that would jump rows.
fn on_grid_key(keyval: gdk::Key) -> glib::Propagation {
    let (grid, columns) = model(|launchpad| (launchpad.grid.clone(), launchpad.applied_columns));
    let Some(grid) = grid else {
        return glib::Propagation::Proceed;
    };

    let grid: &gtk4::Widget = grid.upcast_ref();
    let columns = i64::from(columns.max(1));

    let moved = match keyval {
        gdk::Key::Left => move_focus(grid, -1),
        gdk::Key::Right => move_focus(grid, 1),
        gdk::Key::Up => move_focus(grid, -columns),
        gdk::Key::Down => move_focus(grid, columns),
        gdk::Key::Home => focus_tile(grid, 0),
        gdk::Key::End => tile_count(grid)
            .checked_sub(1)
            .is_some_and(|last| focus_tile(grid, last)),
        _ => return glib::Propagation::Proceed,
    };

    if moved {
        glib::Propagation::Stop
    } else {
        glib::Propagation::Proceed
    }
}

/// Down enters the grid from the search field; Return launches the first match
/// without making the user tab into it.
fn on_search_key(keyval: gdk::Key) -> glib::Propagation {
    let Some(grid) = model(|launchpad| launchpad.grid.clone()) else {
        return glib::Propagation::Proceed;
    };
    let grid: &gtk4::Widget = grid.upcast_ref();

    let handled = match keyval {
        gdk::Key::Down => focus_tile(grid, 0),
        gdk::Key::Return | gdk::Key::KP_Enter => {
            let first = tile_at(grid, 0);
            if first.is_null() {
                false
            } else {
                unsafe { borrow_widget(first) }.activate();
                true
            }
        }
        _ => false,
    };

    if handled {
        glib::Propagation::Stop
    } else {
        glib::Propagation::Proceed
    }
}

/// Applies the search filter, which is what every keystroke ends in.
fn on_search_changed(query: &str) {
    model_mut(|launchpad| launchpad.query = query.to_owned());

    refill();
}

/// The "nothing matched" hint, which a screen reader hears once.
fn show_empty_hint(grid: &gtk4::FlowBox) {
    let message = translated(c"No applications match your search");

    let empty = gtk4::Label::new(Some(&message));
    empty.add_css_class("dim-label");
    grid.append(&empty);

    let message = cstring(&message);
    unsafe {
        crate::accessibility::lambdock_accessible_announce(
            grid.as_ptr().cast(),
            message.as_ptr(),
            0,
        );
    }
}

/// Empties the grid.
fn clear_grid() {
    // The tiles own their menus, so the pointer to the open one has to go before
    // the tiles do.
    copy_menu_close();

    let Some(flow) = model(|launchpad| launchpad.grid.clone()) else {
        return;
    };

    while let Some(child) = crate::ui::first_child_of(flow.upcast_ref()) {
        // A widget has to let go of its popover before GTK finalizes it, or GTK
        // complains about a widget that still has children -- and this grid is
        // rebuilt on every keystroke.
        let tile = tile_in(&child);
        if !tile.is_null() {
            if let Some(menu) = copy_menu_of(unsafe { &borrow_widget(tile) }) {
                menu.unparent();
            }
        }

        flow.remove(&*child);
    }
}

/// Rebuilds the grid for the current query.
fn refill() {
    clear_grid();

    let (grid, icon_size, query, entries) = model(|launchpad| {
        (
            launchpad.grid.clone(),
            launchpad.appearance.icon_size,
            launchpad.query.clone(),
            launchpad.entries,
        )
    });

    let Some(grid) = grid else {
        return;
    };

    let mut shown = 0;
    if !entries.is_null() {
        let count = unsafe { (*entries).len as usize };
        let query = cstring(&query);

        for index in 0..count {
            let entry = unsafe { *(*entries).pdata.add(index) }.cast::<DesktopEntry>();

            if unsafe { lambdock_launchpad_entry_matches(entry, query.as_ptr()) } == 0 {
                continue;
            }

            let Some(tile) = (unsafe { tile_new(entry, icon_size) }) else {
                continue;
            };

            grid.append(&tile);
            shown += 1;
        }
    }

    if shown == 0 {
        show_empty_hint(&grid);
    }
}

// ============================================================================
// 5. CLOSING
// ============================================================================

/// Every part of the launchpad that a click must not treat as "the backdrop".
/// The grid is in the list because a click on its empty area is not a click
/// outside the launchpad.
fn widget_is_launchpad_content(widget: &gtk4::Widget) -> bool {
    widget.has_css_class("launchpad-tile")
        || widget.has_css_class("launchpad-grid")
        || widget.has_css_class("launchpad-search")
        || widget.has_css_class("launchpad-close")
        || widget.is::<gtk4::Entry>()
        || widget.is::<gtk4::Text>()
}

/// Whether `picked` sits inside something a click must not close.
fn click_hits_content(picked: &gtk4::Widget) -> bool {
    if widget_is_launchpad_content(picked) {
        return true;
    }

    let mut current = crate::ui::parent_of(picked);
    while let Some(node) = current {
        if widget_is_launchpad_content(&node) {
            return true;
        }

        current = crate::ui::parent_of(&node);
    }

    false
}

/// A click on the backdrop closes the launchpad, like clicking outside it on the
/// shelf look.
fn on_backdrop_click(x: f64, y: f64) {
    let Some(window) = model(|launchpad| launchpad.window.clone()) else {
        return;
    };

    let window: &gtk4::Widget = window.upcast_ref();
    if let Some(picked) = window.pick(x, y, gtk4::PickFlags::DEFAULT) {
        if !click_hits_content(&picked) {
            lambdock_launchpad_hide();
        }
    }
}

/// Escape closes the copy menu first, then the launchpad.
fn on_window_key(keyval: gdk::Key) -> glib::Propagation {
    if keyval != gdk::Key::Escape {
        return glib::Propagation::Proceed;
    }

    if copy_menu_close() {
        return glib::Propagation::Stop;
    }

    lambdock_launchpad_hide();

    glib::Propagation::Stop
}

// ============================================================================
// 6. THE HEADER AND THE SHELF
// ============================================================================

/// The header: the close button, and the spacer that pushes it to one end.
///
/// There is deliberately no title -- the grid of every installed application is
/// what the screen is, and the search field says what it is for.
fn make_header() -> (gtk4::Box, gtk4::Button) {
    let header = gtk4::Box::new(gtk4::Orientation::Horizontal, 12);
    header.add_css_class("launchpad-top");

    let close = gtk4::Button::from_icon_name("window-close-symbolic");
    close.add_css_class("flat");
    close.add_css_class("launchpad-close");
    close.set_tooltip_text(Some(&translated(c"Close the launchpad (Esc)")));
    describe(
        &close,
        &translated(c"Close launchpad"),
        &translated(c"Closes the application grid. Escape does the same."),
    );
    close.set_size_request(38, 38);
    close.set_valign(gtk4::Align::Center);
    close.connect_clicked(|_| lambdock_launchpad_hide());

    let spacer = gtk4::Box::new(gtk4::Orientation::Horizontal, 0);
    spacer.set_hexpand(true);

    // Left by default: the button first, then the spacer that fills the row.
    header.append(&close);
    header.append(&spacer);

    (header, close)
}

/// Moves the close button to either end of the header, which is what
/// `dock-launchpad-close-side` decides.
fn place_close(header: &gtk4::Widget, close: &gtk4::Widget, on_right: bool) {
    let Some(box_) = header.downcast_ref::<gtk4::Box>() else {
        return;
    };

    // The header is the close button and one expanding spacer, so the other end
    // is the first child that is not the button.
    let other_end = {
        let mut found = None;
        let mut child = crate::ui::first_child_of(header);

        while let Some(node) = child {
            if node.as_ptr() != close.as_ptr() {
                found = Some(node);
                break;
            }

            child = crate::ui::next_sibling_of(&node);
        }

        found
    };

    if on_right {
        if let Some(other_end) = other_end {
            box_.reorder_child_after(close, Some(&*other_end));
        }

        return;
    }

    // A null sibling puts the button first, which is the default side.
    box_.reorder_child_after(close, None::<&gtk4::Widget>);
}

/// Whether `item` produces content rather than launching an application.
///
/// # Safety
///
/// `item` must be null or a live item.
unsafe fn is_shelf_item(item: *const DockItem) -> bool {
    let Some(item) = item.as_ref() else {
        return false;
    };

    item.kind == ITEM_STACK || item.kind == ITEM_TRASH
}

/// One shelf button: the item's icon and name.
///
/// # Safety
///
/// `item` must be a live item.
unsafe fn make_shelf_button(item: *const DockItem) -> gtk4::Button {
    let button = gtk4::Button::new();
    button.add_css_class("flat");
    button.add_css_class("launchpad-shelf-item");

    let content = gtk4::Box::new(gtk4::Orientation::Horizontal, 8);
    let image = match crate::ui::icon_theme() {
        Some(theme) => crate::ui::icon_resolver::create_image_for_icon(
            theme.as_ptr().cast(),
            (*item).icon,
            22,
        ),
        None => gtk4::ffi::gtk_image_new_from_icon_name((*item).icon),
    };
    if !image.is_null() {
        let image: gtk4::Widget = from_glib_none(image);
        content.append(&image);
    }

    let fallback = crate::cstr::translated_dup(c"Item");
    let title = text_or_empty(lambdock_item_title(item, fallback));
    g_free(fallback.cast());

    let label = gtk4::Label::new(Some(&title));
    content.append(&label);
    button.set_child(Some(&content));

    button.set_tooltip_text(Some(&title));
    describe(
        &button,
        &title,
        &translated(if (*item).kind == ITEM_TRASH {
            c"Opens the trash."
        } else {
            c"Opens this folder as a stack."
        }),
    );

    // The launchpad is closed from an idle: its window owns the button whose
    // handler is running, so it cannot be destroyed from inside that handler.
    // The item is looked up from the button again, because a reload may replace
    // it while the idle is pending.
    let button_for_idle = button.clone();
    button.connect_clicked(move |_| {
        let button = button_for_idle.clone();
        glib::idle_add_local_once(move || {
            let item = unsafe { dock_item_from_widget(button.as_ptr().cast()) };

            lambdock_launchpad_hide();

            let Some(item) = item.as_ref() else {
                return;
            };
            unsafe {
                if item.kind == ITEM_TRASH {
                    if crate::ui::trash::lambdock_trash_open() == 0 {
                        crate::log_warn!("UI", "Could not open the trash");
                    }
                } else {
                    crate::ui::stack::lambdock_stack_show(item as *const DockItem as *mut DockItem);
                }
            }
        });
    });

    button
}

/// The row of stacks and the trash inside the launchpad.
///
/// # Safety
///
/// `state` must be null or a live dock.
pub unsafe fn lambdock_launchpad_shelf_new(
    state: *mut LambdockState,
) -> *mut gtk4::ffi::GtkWidget {
    let Some(state) = state.as_ref() else {
        return std::ptr::null_mut();
    };

    let mut any = false;
    let mut node = state.dock_items.cast::<GList>();
    while !node.is_null() && !any {
        any = is_shelf_item((*node).data.cast::<DockItem>());
        node = (*node).next;
    }

    if !any {
        return std::ptr::null_mut();
    }

    crate::ui::ensure_gtk_initialized();

    let shelf = gtk4::Box::new(gtk4::Orientation::Horizontal, 8);
    shelf.add_css_class("launchpad-shelf");
    shelf.set_halign(gtk4::Align::Center);

    let mut node = state.dock_items.cast::<GList>();
    while !node.is_null() {
        let item = (*node).data.cast::<DockItem>();
        if is_shelf_item(item) {
            shelf.append(&make_shelf_button(item));
        }

        node = (*node).next;
    }

    into_floating_pointer(shelf.upcast())
}

/// Hands `widget` to C as a floating reference, the way a constructor would.
fn into_floating_pointer(widget: gtk4::Widget) -> *mut gtk4::ffi::GtkWidget {
    unsafe { glib::gobject_ffi::g_object_force_floating(widget.as_ptr().cast()) };

    widget.into_glib_ptr()
}

// ============================================================================
// 7. THE WINDOW
// ============================================================================

/// The application to parent the launchpad to, or `None` when there is none.
///
/// # Safety
///
/// `state` must be null or a live dock.
unsafe fn application_for(state: *const LambdockState) -> Option<gtk4::Application> {
    let state = state.as_ref()?;
    if state.window.is_null() {
        return None;
    }

    let window: gtk4::Window = from_glib_none(state.window.cast());

    window.application()?.downcast::<gtk4::Application>().ok()
}

/// The search field, with its own key controller.
fn make_search_entry() -> gtk4::SearchEntry {
    let search = gtk4::SearchEntry::new();
    search.add_css_class("launchpad-search");
    search.set_halign(gtk4::Align::Center);
    search.set_size_request(460, -1);
    search.set_placeholder_text(Some(&translated(c"Search applications")));
    describe(
        &search,
        &translated(c"Search applications"),
        &translated(c"Filters the grid as you type. Down enters the grid."),
    );
    search.connect_search_changed(|search| on_search_changed(&search.text()));

    // Capture: the entry consumes Return for its own "activate" signal, and Down
    // has to leave the field instead of moving the text cursor.
    let keys = gtk4::EventControllerKey::new();
    keys.set_propagation_phase(gtk4::PropagationPhase::Capture);
    keys.connect_key_pressed(|_, keyval, _, _| on_search_key(keyval));
    search.add_controller(keys);

    // Remembered on the model: this is what the window focuses, what a reset
    // clears and what `lambdock_launchpad_search_entry` hands out.
    model_mut(|launchpad| launchpad.search = Some(search.clone()));

    search
}

/// The flow box of tiles.
fn make_grid() -> gtk4::FlowBox {
    let grid = gtk4::FlowBox::new();
    model_mut(|launchpad| launchpad.grid = Some(grid.clone()));

    grid.set_selection_mode(gtk4::SelectionMode::None);
    grid.set_homogeneous(true);
    grid.set_min_children_per_line(FALLBACK_COLUMNS);
    grid.set_max_children_per_line(FALLBACK_COLUMNS);
    grid.set_row_spacing(16);
    grid.set_column_spacing(16);
    grid.add_css_class("launchpad-grid");
    grid.set_hexpand(true);
    grid.set_vexpand(true);

    describe(
        &grid,
        &translated(c"Applications"),
        &translated(
            c"Application grid. Use the arrow keys to choose an application and Enter to open it.",
        ),
    );

    // Capture: the flow box has arrow bindings of its own, and the grid's
    // movement has to follow the tiles rather than an invisible cursor.
    let keys = gtk4::EventControllerKey::new();
    keys.set_propagation_phase(gtk4::PropagationPhase::Capture);
    keys.connect_key_pressed(|_, keyval, _, _| on_grid_key(keyval));
    grid.add_controller(keys);

    grid
}

/// The scrolling area that holds the grid.
fn make_grid_viewport() -> gtk4::ScrolledWindow {
    let scrolled = gtk4::ScrolledWindow::new();

    scrolled.set_policy(gtk4::PolicyType::Never, gtk4::PolicyType::Automatic);
    scrolled.set_propagate_natural_width(true);
    scrolled.set_propagate_natural_height(true);
    scrolled.set_vexpand(true);
    scrolled.set_child(Some(&make_grid()));

    scrolled
}

/// Header, search field and grid, stacked with the launchpad's margins.
fn make_window_body() -> gtk4::Box {
    let body = gtk4::Box::new(gtk4::Orientation::Vertical, 20);
    body.set_margin_start(SIDE_MARGIN_PX);
    body.set_margin_end(SIDE_MARGIN_PX);
    body.set_margin_top(28);
    body.set_margin_bottom(36);
    body.set_valign(gtk4::Align::Fill);
    body.set_vexpand(true);

    let (header, close) = make_header();
    let on_right = model(|launchpad| launchpad.appearance.close_on_right);
    place_close(header.upcast_ref(), close.upcast_ref(), on_right);
    model_mut(|launchpad| {
        launchpad.header = Some(header.clone());
        launchpad.close = Some(close);
    });
    body.append(&header);

    body.append(&make_search_entry());

    let state = model(|launchpad| launchpad.state);
    let shelf = unsafe { lambdock_launchpad_shelf_new(state) };
    if !shelf.is_null() {
        let shelf: gtk4::Widget = unsafe { from_glib_none(shelf) };
        body.append(&shelf);
    }

    body.append(&make_grid_viewport());

    body
}

/// Columns for a window of `width` pixels.
fn columns_for_width(width: c_int, icon_size: c_int, configured: c_int) -> c_int {
    let usable = 0.max(width - 2 * SIDE_MARGIN_PX - GRID_INSET_PX);

    lambdock_launchpad_columns_for(usable, icon_size, configured)
}

/// Chooses how many tiles fit across the launchpad and applies it.
///
/// GTK4 has no "this widget was resized" signal (GTK3's `size-allocate` is
/// gone), so this runs when the surface maps and when the launchpad is shown.
/// The window is a full-screen layer surface, so the monitor width is what the
/// grid will get; before the first allocation that is the only sensible answer,
/// and on the normal-window fallback the real width is used once it exists.
fn fit_columns(window: &gtk4::ApplicationWindow) {
    let (icon_size, configured) =
        model(|launchpad| (launchpad.appearance.icon_size, launchpad.appearance.columns));

    let mut width = window.width();
    if width <= 0 {
        if let Some(surface) = window.surface() {
            if let Some(display) = gdk::Display::default() {
                if let Some(monitor) = display.monitor_at_surface(&surface) {
                    width = monitor.geometry().width();
                }
            }
        }
    }

    if width <= 0 {
        return;
    }

    apply_columns(columns_for_width(width, icon_size, configured));
}

/// Reads the appearance out of the settings of the dock that opened us.
///
/// # Safety
///
/// `state` must be null or a live dock.
unsafe fn appearance_for(state: *const LambdockState) -> Appearance {
    use crate::bridge::{
        get_launchpad_blur_config_for, get_launchpad_close_on_right_config_for,
        get_launchpad_columns_config_for, get_launchpad_icon_size_config_for,
    };

    let config_path = state.as_ref().map_or(std::ptr::null(), |state| state.config_path);

    Appearance {
        icon_size: get_launchpad_icon_size_config_for(config_path),
        columns: get_launchpad_columns_config_for(config_path),
        blur: get_launchpad_blur_config_for(config_path) != 0,
        close_on_right: get_launchpad_close_on_right_config_for(config_path) != 0,
    }
}

/// Pushes the appearance into the widgets that exist.
fn apply_appearance(appearance: Appearance) {
    model_mut(|launchpad| launchpad.appearance = appearance);

    let (window, header, close) = model(|launchpad| {
        (
            launchpad.window.clone(),
            launchpad.header.clone(),
            launchpad.close.clone(),
        )
    });

    let Some(window) = window else {
        return;
    };

    if appearance.blur {
        window.add_css_class("launchpad-frosted");
    } else {
        window.remove_css_class("launchpad-frosted");
    }

    // The close button follows the setting on every show, so moving it in the
    // preferences window takes effect the next time the launchpad opens.
    if let (Some(header), Some(close)) = (header, close) {
        place_close(header.upcast_ref(), close.upcast_ref(), appearance.close_on_right);
    }

    fit_columns(&window);
}

/// Hands the keyboard to the search field the moment the surface appears, and
/// sizes the grid for the surface it just got.
///
/// The launchpad is a layer-shell overlay whose keyboard mode is EXCLUSIVE, so
/// the compositor gives it the keyboard as soon as it maps. `set_focus` is the
/// right call rather than `grab_focus`: it records the widget the window should
/// focus and lets GTK apply it once the children are mapped too (a
/// `GtkSearchEntry` focuses its inner text widget, not itself).
fn on_window_mapped(window: &gtk4::ApplicationWindow) {
    fit_columns(window);

    if let Some(search) = model(|launchpad| launchpad.search.clone()) {
        gtk4::prelude::GtkWindowExt::set_focus(window, Some(&search));
    }
}

/// Escape (capture phase, because the search entry consumes it itself) and the
/// backdrop click that closes the launchpad.
fn install_window_controllers(window: &gtk4::ApplicationWindow) {
    let keys = gtk4::EventControllerKey::new();
    keys.set_propagation_phase(gtk4::PropagationPhase::Capture);
    keys.connect_key_pressed(|_, keyval, _, _| on_window_key(keyval));
    window.add_controller(keys);

    let click = gtk4::GestureClick::new();
    click.set_button(gdk::BUTTON_PRIMARY);
    click.connect_pressed(|_, _, x, y| on_backdrop_click(x, y));
    window.add_controller(click);

    // Typing must work as soon as the launchpad appears, without a click. Weak,
    // so the handler does not keep the window it belongs to alive.
    let weak = window.downgrade();
    window.connect_map(move |_| {
        if let Some(window) = weak.upgrade() {
            on_window_mapped(&window);
        }
    });
}

/// Turns the window into a full-screen layer surface, where the compositor
/// implements the protocol.
///
/// # Safety
///
/// `state` must be null or a live dock.
unsafe fn configure_layer_shell(window: &gtk4::ApplicationWindow, state: *const LambdockState) {
    use crate::ui::layer_shell::{
        self, EDGE_BOTTOM, EDGE_LEFT, EDGE_RIGHT, EDGE_TOP, KEYBOARD_MODE_EXCLUSIVE, LAYER_OVERLAY,
    };

    if layer_shell::lambdock_layer_shell_available() == 0 {
        return;
    }

    let surface: &gtk4::Window = window.upcast_ref();
    layer_shell::init_for_window(surface);
    layer_shell::set_namespace(surface, c"lambdock-launchpad");
    layer_shell::set_layer(surface, LAYER_OVERLAY);
    layer_shell::set_exclusive_zone(surface, -1);
    layer_shell::set_keyboard_mode(surface, KEYBOARD_MODE_EXCLUSIVE);

    for edge in [EDGE_TOP, EDGE_BOTTOM, EDGE_LEFT, EDGE_RIGHT] {
        layer_shell::set_anchor(surface, edge, true);
    }

    let monitor = state.as_ref().map_or(std::ptr::null_mut(), |state| state.monitor);
    let monitor = monitor.cast::<gdk::ffi::GdkMonitor>();
    if layer_shell::is_monitor_valid(monitor) != 0 {
        layer_shell::set_monitor(surface, monitor);
    }
}

/// The window, or `None` when there is no application to open it on.
///
/// # Safety
///
/// `state` must be null or a live dock.
unsafe fn make_window(state: *mut LambdockState) -> Option<gtk4::ApplicationWindow> {
    let app = application_for(state)?;

    let window = gtk4::ApplicationWindow::new(&app);
    window.add_css_class("launchpad");
    window.set_title(Some(&translated(c"Applications")));
    window.set_default_size(1280, 800);
    window.set_hide_on_close(true);

    // The surface fills the whole overlay so the frosted background covers the
    // screen edge to edge; the content keeps its own margins inside it.
    let surface = gtk4::Box::new(gtk4::Orientation::Vertical, 0);
    surface.add_css_class("launchpad-surface");
    surface.append(&make_window_body());
    window.set_child(Some(&surface));

    install_window_controllers(&window);
    configure_layer_shell(&window, state);

    Some(window)
}

/// Loads a fresh application list, replacing the previous one.
unsafe fn rescan() {
    let entries = unsafe { lambdock_desktop_entries_scan() };
    let previous = model_mut(|launchpad| std::mem::replace(&mut launchpad.entries, entries));

    if !previous.is_null() {
        unsafe { lambdock_desktop_entries_free(previous) };
    }
}

/// Clears the search field and the query it feeds.
fn reset_query() {
    model_mut(|launchpad| launchpad.query.clear());

    if let Some(search) = model(|launchpad| launchpad.search.clone()) {
        // Setting the text emits "search-changed", which is the same path typing
        // takes: the grid filters exactly as it would for a person.
        search.set_text("");
    }
}

/// Drops the window and the pointers into it.
///
/// The grid is cleared first because the tiles own popovers, which have to be
/// unparented before GTK finalizes them.
fn release_window() {
    copy_menu_close();
    clear_grid();

    let window = model_mut(|launchpad| {
        launchpad.grid = None;
        launchpad.search = None;
        launchpad.header = None;
        launchpad.close = None;
        launchpad.open_copy_menu = None;

        launchpad.window.take()
    });

    if let Some(window) = window {
        window.destroy();
    }
}

// ============================================================================
// 8. THE PUBLIC ENTRY POINTS
// ============================================================================

/// Shows the launchpad for `state`, rebuilding it if it was closed.
///
/// # Safety
///
/// `state` must be null or a live dock.
pub unsafe fn lambdock_launchpad_show(state: *mut LambdockState) {
    crate::ui::ensure_gtk_initialized();

    let appearance = appearance_for(state);

    // Borrowed for the window's life: the shelf reads this dock's items.
    model_mut(|launchpad| launchpad.state = state);

    if model(|launchpad| launchpad.window.is_none()) {
        let Some(window) = make_window(state) else {
            crate::log_warn!("UI", "Cannot open the launchpad without an application");
            return;
        };

        model_mut(|launchpad| launchpad.window = Some(window));
    }

    // The window exists now, so the appearance reaches the widgets it owns.
    apply_appearance(appearance);

    // Rescan on every show: installing an application is enough for it to
    // appear, without restarting the dock.
    rescan();
    reset_query();
    refill();

    let Some(window) = model(|launchpad| launchpad.window.clone()) else {
        return;
    };

    let count = model(|launchpad| {
        if launchpad.entries.is_null() {
            0
        } else {
            unsafe { (*launchpad.entries).len }
        }
    });
    crate::log_info!("UI", "Opening launchpad with {count} applications");

    window.present();

    // Fade in, unless motion is reduced: the class is dropped on the next idle
    // so the first frame is drawn transparent and the CSS transition runs.
    if crate::accessibility::lambdock_animations_enabled(state) != 0 {
        window.add_css_class("launchpad-fading-in");

        let weak = window.downgrade();
        glib::idle_add_local_once(move || {
            if let Some(window) = weak.upgrade() {
                window.remove_css_class("launchpad-fading-in");
            }
        });
    }

    // Name the focus widget before presenting, and grab it now for the case
    // where the window is already mapped; `on_window_mapped` repeats this for
    // the first open, where the surface appears asynchronously.
    if let Some(search) = model(|launchpad| launchpad.search.clone()) {
        gtk4::prelude::GtkWindowExt::set_focus(&window, Some(&search));
        search.grab_focus();
    }
}

/// Hides the launchpad and releases its window; the next show rebuilds it.
pub fn lambdock_launchpad_hide() {
    release_window();
}

/// Whether the launchpad overlay is shown.
pub fn lambdock_launchpad_is_open() -> c_int {
    model(|launchpad| {
        launchpad
            .window
            .as_ref()
            .is_some_and(|window| crate::ui::is_shown(window.upcast_ref())) as c_int
    })
}

/// The launchpad's search field, or null while the launchpad is closed.
pub fn lambdock_launchpad_search_entry() -> *mut gtk4::ffi::GtkWidget {
    model(|launchpad| {
        launchpad
            .search
            .as_ref()
            .map_or(std::ptr::null_mut(), |search| search.as_ptr().cast())
    })
}

/// Types into the search field for the user, which is what makes the launchpad
/// scriptable.
///
/// # Safety
///
/// `query` must be null or NUL-terminated.
pub unsafe fn lambdock_launchpad_set_query(query: *const c_char) {
    crate::ui::ensure_gtk_initialized();

    let Some(search) = model(|launchpad| launchpad.search.clone()) else {
        return;
    };

    // Setting the text emits "search-changed", which is the same path typing
    // takes.
    search.set_text(&text_or_empty(query));
}

/// Destroys the launchpad window, on application teardown.
pub fn lambdock_launchpad_shutdown() {
    release_window();

    let entries = model_mut(|launchpad| {
        launchpad.appearance = Appearance::default();
        launchpad.applied_columns = FALLBACK_COLUMNS;
        launchpad.query.clear();

        std::mem::replace(&mut launchpad.entries, std::ptr::null_mut())
    });

    if !entries.is_null() {
        unsafe { lambdock_desktop_entries_free(entries) };
    }
}

/// The searchable application grid widget.
///
/// # Safety
///
/// `_entries` is unused: the grid is filled from the launchpad's own model. The
/// result is a new floating widget.
pub unsafe fn lambdock_launchpad_grid_new(
    _entries: *mut glib::ffi::GPtrArray,
) -> *mut gtk4::ffi::GtkWidget {
    crate::ui::ensure_gtk_initialized();

    into_floating_pointer(make_grid().upcast())
}

/// Builds one tile of the grid.
///
/// # Safety
///
/// `entry` must be null or a live entry. The result is a new floating widget, or
/// null for a null entry.
pub unsafe fn lambdock_launchpad_tile_new(
    entry: *const DesktopEntry,
    icon_size: c_int,
) -> *mut gtk4::ffi::GtkWidget {
    crate::ui::ensure_gtk_initialized();

    let Some(tile) = tile_new(entry, icon_size) else {
        return std::ptr::null_mut();
    };

    into_floating_pointer(tile.upcast())
}

/// The launchpad's header row: the close button, and the spacer beside it.
///
/// # Safety
///
/// `out_close` must be null or writable. The result is a new floating widget,
/// and the button it writes is borrowed.
pub unsafe fn lambdock_launchpad_header_new(
    out_close: *mut *mut gtk4::ffi::GtkWidget,
) -> *mut gtk4::ffi::GtkWidget {
    crate::ui::ensure_gtk_initialized();

    let (header, close) = make_header();

    if !out_close.is_null() {
        *out_close = close.as_ptr().cast();
    }

    into_floating_pointer(header.upcast())
}

/// Moves the close button to either end of the header.
///
/// # Safety
///
/// `header` and `close` must be null or live widgets.
pub unsafe fn lambdock_launchpad_header_place_close(
    header: *mut gtk4::ffi::GtkWidget,
    close: *mut gtk4::ffi::GtkWidget,
    on_right: c_int,
) {
    if header.is_null() || close.is_null() {
        return;
    }

    // Borrowed, not owned: the header is still floating, and taking it with an
    // owning wrapper would unref it on the way out.
    let header = crate::ui::widget_from_pointer(header);
    let close = crate::ui::widget_from_pointer(close);

    place_close(&header, &close, on_right != 0);
}

#[cfg(test)]
mod tests {
    use super::*;

    /// An entry as the scan would produce it: two strings, no copies.
    fn entry(name: &str, exec: &str) -> DesktopEntry {
        let mut entry: DesktopEntry = unsafe { std::mem::zeroed() };
        entry.name = unsafe { crate::glib::g_strdup(cstring(name).as_ptr()) };
        entry.exec = unsafe { crate::glib::g_strdup(cstring(exec).as_ptr()) };

        entry
    }

    fn free(entry: DesktopEntry) {
        unsafe {
            g_free(entry.name.cast());
            g_free(entry.exec.cast());
        }
    }

    /// Both the name and the command line are searched, case-insensitively, and
    /// nothing typed shows everything.
    #[test]
    fn the_filter_reads_the_name_and_the_command() {
        let firefox = entry("Firefox", "firefox %u");

        unsafe {
            assert_eq!(lambdock_launchpad_entry_matches(std::ptr::null(), c"fire".as_ptr()), 0);
            assert_eq!(lambdock_launchpad_entry_matches(&firefox, std::ptr::null()), 1);
            assert_eq!(lambdock_launchpad_entry_matches(&firefox, c"".as_ptr()), 1);
            assert_eq!(lambdock_launchpad_entry_matches(&firefox, c"fire".as_ptr()), 1);
            assert_eq!(lambdock_launchpad_entry_matches(&firefox, c"FIREFOX".as_ptr()), 1);
            assert_eq!(lambdock_launchpad_entry_matches(&firefox, c"chromium".as_ptr()), 0);
        }

        free(firefox);

        // A word that is only in the command finds the entry too.
        let writer = entry("Writer", "libreoffice --writer --norestore");
        unsafe {
            assert_eq!(lambdock_launchpad_entry_matches(&writer, c"--norestore".as_ptr()), 1);
            assert_eq!(lambdock_launchpad_entry_matches(&writer, c"writer".as_ptr()), 1);
            assert_eq!(lambdock_launchpad_entry_matches(&writer, c"--calc".as_ptr()), 0);
        }

        free(writer);
    }

    /// Tiles grow with the icon size, so the column count comes from the width,
    /// and a configured count is an upper bound rather than a promise.
    #[test]
    fn the_columns_follow_the_width_and_the_icon_size() {
        // 1280 - 112 px of tiles, and a tile of a 64 px icon is 150 px wide.
        assert_eq!(lambdock_launchpad_columns_for(1168, 64, 0), 7);

        // Narrow screens clamp to the minimum, wide ones to the maximum.
        assert_eq!(lambdock_launchpad_columns_for(300, 64, 0), AUTO_COLUMNS_MIN);
        assert_eq!(lambdock_launchpad_columns_for(5000, 64, 0), AUTO_COLUMNS_MAX);

        // No measurable width yet: a usable default.
        assert_eq!(lambdock_launchpad_columns_for(0, 64, 0), FALLBACK_COLUMNS as c_int);

        // Honoured when it fits, ignored when the screen cannot hold it.
        assert_eq!(lambdock_launchpad_columns_for(1168, 64, 4), 4);
        assert_eq!(lambdock_launchpad_columns_for(400, 64, 14), LAUNCHPAD_COLUMNS_MIN);

        // A bigger icon means wider tiles, so fewer of them fit.
        assert!(
            lambdock_launchpad_columns_for(1168, 160, 0)
                < lambdock_launchpad_columns_for(1168, 32, 0)
        );
    }

    /// The tooltip says the other copies are there, so the menu is discoverable
    /// without a right click that would otherwise do nothing.
    #[test]
    fn a_tooltip_mentions_the_other_copies() {
        use glib::ffi::{g_ptr_array_add, g_ptr_array_new};

        let mut entry = self::entry("LibreOffice Writer", "libreoffice --writer");

        assert_eq!(tile_tooltip_text(&entry), "libreoffice --writer");

        let copy = self::entry("LibreOffice Writer", "/usr/bin/flatpak run org.libreoffice");
        entry.alternates = unsafe { g_ptr_array_new() };
        unsafe { g_ptr_array_add(entry.alternates, (&copy as *const DesktopEntry).cast_mut().cast()) };

        assert!(tile_tooltip_text(&entry).contains("other installed copies"));
        assert!(tile_description(&entry).contains("more than one installed copy"));

        unsafe { glib::ffi::g_ptr_array_free(entry.alternates, glib::ffi::GTRUE) };
        free(copy);
        free(entry);
    }
}
