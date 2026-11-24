// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The settings window: `src/ui/preferences.c` in Rust.
//!
//! A normal GTK4 window (not a layer-shell surface) listing the settings people
//! usually change, grouped the way the settings file groups them.
//!
//! Every control applies the same way the REPL does: it asks `(lambdock prefs)` to
//! run the ordinary `set-dock-*!` setter -- which mutates the in-memory config and
//! triggers `(reload-dock!)` -- and to remember the value in the GUI override
//! file. That keeps the generated settings file untouched.
//!
//! Sliders fire `value-changed` on every pixel and each edit rebuilds the dock, so
//! applying is debounced: the newest value is written once the control has been
//! still for [`APPLY_DEBOUNCE_MS`].
//!
//! The file follows one order, and it is worth keeping it:
//!
//! 1. types -- what a control knows, what a row declares
//! 2. pure helpers -- paths, choices, formatting, with no widgets and no I/O
//! 3. choices -- the vocabularies a dropdown can offer
//! 4. readers -- settings file -> plain value, with no widgets
//! 5. tables -- the form itself, as data
//! 6. builders -- small constructors that turn one declared row into widgets
//! 7. state -- the per-control state and the debounced apply path
//! 8. content -- the form walker
//! 9. windows -- window state, the registry, callbacks and the public API

use gtk4::glib;
use gtk4::glib::translate::{from_glib_none, IntoGlibPtr};
use gtk4::prelude::*;
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_uint, c_void};
use std::rc::Rc;

use crate::bridge::{
    dock_layout_add_preset_item, get_autohide_config_for, get_autohide_delay_config_for,
    get_capsule_padding_config_for, get_hover_animate_config_for, get_icon_size_config_for,
    get_item_spacing_config_for, get_launchpad_blur_config_for,
    get_launchpad_close_on_right_config_for, get_launchpad_columns_config_for,
    get_launchpad_icon_size_config_for, get_position_config_for, get_reduce_motion_config_for,
    get_shelf_config_for, get_show_when_clear_config_for, get_show_window_titles_config_for,
    get_taskbar_config_for, get_theme_config_for, get_track_windows_config_for,
    load_dock_config_scm, reset_dock_preferences, set_dock_setting_bool, set_dock_setting_double,
    set_dock_setting_int, set_dock_setting_symbol,
};
use crate::core::config_values::{
    lambdock_edge_name, ShelfParams, AUTOHIDE_DELAY_MAX_MS, LAUNCHPAD_COLUMNS_MAX,
    LAUNCHPAD_ICON_SIZE_MAX, LAUNCHPAD_ICON_SIZE_MIN, SHELF_PERSPECTIVE_MAX, SHELF_PERSPECTIVE_MIN,
    SHELF_TILT_MAX_DEG,
};
use crate::{log_info, log_warn};
use crate::dock_item::{reload_dock_instance};

/// How long a control must be still before its value is written.
const APPLY_DEBOUNCE_MS: u32 = 140;

/// Where a control's state hangs off its widget: a `g_object` data key, so it is
/// a quark name and not a C string.
const CONTROL_KEY: &str = "lambdock-pref-control";

/// Where the option name hangs off its widget. This is also the contract the unit
/// test and any tooling use to find a control by the option it edits, so it is a
/// `g_strdup`ed string set with `g_object_set_data_full`, not a quark.
const KEY_KEY: &CStr = c"lambdock-key";

/// Where a preferences window keeps its own state.
const WINDOW_KEY: &str = "lambdock-prefs-window";


// ============================================================================
// 1. TYPES
// ============================================================================

/// How a value is written back to the configuration. It also picks the writer, so
/// adding a kind is one arm in [`apply_value`] plus (rarely) a new reader.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    Bool,
    Int,
    Double,
    Symbol,
}

impl Kind {
    /// How many digits a numeric control shows: integers show none.
    fn digits(self) -> u32 {
        if self == Kind::Int {
            0
        } else {
            2
        }
    }
}

/// The state of one control, attached to its widget.
///
/// Everything except `timer` is written once at build time; grouping it here is
/// what keeps the "which option does this widget edit?" question answerable from
/// the widget alone.
struct Control {
    /// Settings path, "" for the default dock.
    config_path: String,
    /// Option name, e.g. `dock-icon-size`.
    key: String,
    kind: Kind,
    /// The symbol behind each row of a dropdown.
    symbols: Vec<String>,
    /// The pending debounced apply, so a newer edit can cancel it.
    timer: Cell<Option<glib::SourceId>>,
}

type ControlHandle = Rc<Control>;

/// A dropdown's choices: a label for the user, a symbol for the settings file.
///
/// The labels are plain literals, exactly as they were when this form was written
/// as inline code; marking them translatable would add new msgids to the
/// catalogues and is a separate change from this port.
struct Choice {
    label: &'static CStr,
    symbol: &'static str,
}

/// A reader: "what is the value right now?", from the settings file.
///
/// The switch readers are the bridge's own getters; the numeric and choice
/// readers are adapters local to this file.
type ReadSwitch = unsafe fn(*const c_char) -> c_int;
type ReadNumber = unsafe fn(*const c_char) -> f64;
type ReadChoice = unsafe fn(*const c_char) -> c_uint;

/// One declared row of the form.
struct SwitchSpec {
    key: &'static str,
    title: &'static CStr,
    subtitle: &'static CStr,
    read: ReadSwitch,
}

struct ScaleSpec {
    key: &'static str,
    title: &'static CStr,
    subtitle: &'static CStr,
    kind: Kind,
    minimum: f64,
    maximum: f64,
    step: f64,
    read: ReadNumber,
}

struct ChoiceSpec {
    key: &'static str,
    title: &'static CStr,
    subtitle: &'static CStr,
    choices: &'static [Choice],
    read: ReadChoice,
}

/// An action row is a dropdown (what to act on) and a button (do it). It edits no
/// setting: it runs a command, which is what "add a widget to the dock" is. The
/// dropdown still carries a [`Control`], so it is keyed like every other control
/// and the row can read the selected symbol the same way.
struct ActionSpec {
    key: &'static str,
    title: &'static CStr,
    subtitle: &'static CStr,
    button_label: &'static CStr,
    choices: &'static [Choice],
}

/// One card of the form: its heading, its explanation and its rows.
struct Section {
    title: &'static CStr,
    subtitle: &'static CStr,
    switches: &'static [SwitchSpec],
    scales: &'static [ScaleSpec],
    choices: &'static [ChoiceSpec],
    actions: &'static [ActionSpec],
}

/// What a preferences window knows about itself.
struct WindowState {
    /// The dock being edited.
    config_path: String,
    /// The viewport whose child is rebuilt on a reset.
    scrolled: glib::WeakRef<gtk4::ScrolledWindow>,
}

// ============================================================================
// 2. PURE HELPERS
// ============================================================================

/// The settings path to use, normalising "no dock" to the default dock.
fn config_path_or_default(config_path: Option<&str>) -> &str {
    config_path.filter(|path| !path.is_empty()).unwrap_or("")
}

/// Whether this path names the default dock.
fn config_path_is_default(config_path: &str) -> bool {
    config_path.is_empty()
}

/// The index of `symbol` in `choices`, or 0 (a safe default) when it is absent.
fn choice_index_of(choices: &[Choice], symbol: Option<&str>) -> u32 {
    let Some(symbol) = symbol else {
        return 0;
    };

    choices
        .iter()
        .position(|choice| choice.symbol == symbol)
        .unwrap_or(0) as u32
}

/// Whether two slider values differ enough to be worth mirroring: a slider and
/// its exact-value field mirror each other, and must stop once they agree.
fn values_differ(left: f64, right: f64) -> bool {
    (left - right).abs() > 1e-9
}

// ============================================================================
// 4. READERS (settings file -> plain value)
// ============================================================================

/// One field of the shelf, read as the number a scale row wants.
///
/// The shelf is read as one struct, so each of its rows needs a small adapter;
/// keeping them here means every scale row is a `fn(*const c_char) -> f64`,
/// whether the setting lives in a struct or has a getter of its own.
unsafe fn shelf_value(path: *const c_char, pick: impl Fn(&ShelfParams) -> f64) -> f64 {
    let mut shelf = ShelfParams::default();
    get_shelf_config_for(path, &mut shelf);

    pick(&shelf)
}

unsafe fn read_shelf_margin(path: *const c_char) -> f64 {
    shelf_value(path, |shelf| f64::from(shelf.margin_px))
}

unsafe fn read_shelf_front(path: *const c_char) -> f64 {
    shelf_value(path, |shelf| f64::from(shelf.front_px))
}

unsafe fn read_shelf_tilt(path: *const c_char) -> f64 {
    shelf_value(path, |shelf| shelf.tilt_deg)
}

unsafe fn read_shelf_flare(path: *const c_char) -> f64 {
    shelf_value(path, |shelf| f64::from(shelf.flare_px))
}

unsafe fn read_shelf_perspective(path: *const c_char) -> f64 {
    shelf_value(path, |shelf| shelf.perspective)
}

unsafe fn read_shelf_reflection(path: *const c_char) -> f64 {
    shelf_value(path, |shelf| shelf.reflection_ratio)
}

unsafe fn read_shelf_reflection_intensity(path: *const c_char) -> f64 {
    shelf_value(path, |shelf| shelf.reflection_intensity)
}

unsafe fn read_shelf_reflection_blur(path: *const c_char) -> f64 {
    shelf_value(path, |shelf| shelf.reflection_blur_ratio)
}

/// A dock-wide integer setting, read as the number a scale row wants.
unsafe fn int_reader(
    path: *const c_char,
    getter: unsafe fn(*const c_char) -> c_int,
) -> f64 {
    f64::from(getter(path))
}

unsafe fn read_icon_size(path: *const c_char) -> f64 {
    int_reader(path, get_icon_size_config_for)
}

unsafe fn read_item_spacing(path: *const c_char) -> f64 {
    int_reader(path, get_item_spacing_config_for)
}

unsafe fn read_capsule_padding(path: *const c_char) -> f64 {
    int_reader(path, get_capsule_padding_config_for)
}

unsafe fn read_autohide_delay(path: *const c_char) -> f64 {
    int_reader(path, get_autohide_delay_config_for)
}

unsafe fn read_launchpad_icon_size(path: *const c_char) -> f64 {
    int_reader(path, get_launchpad_icon_size_config_for)
}

unsafe fn read_launchpad_columns(path: *const c_char) -> f64 {
    int_reader(path, get_launchpad_columns_config_for)
}

// The choice readers map the settings vocabulary onto a dropdown's, so the tables
// can just state a row.

unsafe fn read_position_choice(path: *const c_char) -> c_uint {
    let edge = get_position_config_for(path);
    let name = lambdock_edge_name(edge);
    let name = crate::cstr::text(name);

    choice_index_of(POSITION_CHOICES, name.as_deref())
}

unsafe fn read_mode_choice(path: *const c_char) -> c_uint {
    c_uint::from(get_taskbar_config_for(path) != 0)
}

unsafe fn read_launchpad_close_side_choice(path: *const c_char) -> c_uint {
    c_uint::from(get_launchpad_close_on_right_config_for(path) != 0)
}

unsafe fn read_theme_choice(path: *const c_char) -> c_uint {
    let theme = get_theme_config_for(path);
    // `get_theme_config_for` is `(transfer full)`: read it, then let it go.
    let symbol = crate::cstr::text(theme);
    crate::glib::g_free(theme.cast());

    choice_index_of(THEME_CHOICES, symbol.as_deref())
}

// ============================================================================
// 3. CHOICES
// ============================================================================

const THEME_CHOICES: &[Choice] = &[
    Choice {
        label: c"Vanilla",
        symbol: "vanilla",
    },
    Choice {
        label: c"Vanilla Light",
        symbol: "vanilla-light",
    },
    Choice {
        label: c"Frost",
        symbol: "frost",
    },
    Choice {
        label: c"Frost Dark",
        symbol: "frost-dark",
    },
    Choice {
        label: c"Solarized Light",
        symbol: "solarized-light",
    },
    Choice {
        label: c"Nature",
        symbol: "nature",
    },
    Choice {
        label: c"Ambiance",
        symbol: "ambiance",
    },
    Choice {
        label: c"Brushed",
        symbol: "brushed",
    },
    Choice {
        label: c"Dune",
        symbol: "dune",
    },
    Choice {
        label: c"Stage",
        symbol: "stage",
    },
    Choice {
        label: c"Glass",
        symbol: "glass",
    },
    Choice {
        label: c"Glass Dark",
        symbol: "glass-dark",
    },
    Choice {
        label: c"Platinum",
        symbol: "platinum",
    },
    Choice {
        label: c"Platinum Dark",
        symbol: "platinum-dark",
    },
    Choice {
        label: c"Luna",
        symbol: "luna",
    },
    Choice {
        label: c"Aurora",
        symbol: "aurora",
    },
    Choice {
        label: c"Seven",
        symbol: "seven",
    },
    Choice {
        label: c"Tiles",
        symbol: "tiles",
    },
    Choice {
        label: c"Island",
        symbol: "island",
    },
    Choice {
        label: c"None",
        symbol: "none",
    },
];

const POSITION_CHOICES: &[Choice] = &[
    Choice {
        label: c"Bottom",
        symbol: "bottom",
    },
    Choice {
        label: c"Top",
        symbol: "top",
    },
    Choice {
        label: c"Left",
        symbol: "left",
    },
    Choice {
        label: c"Right",
        symbol: "right",
    },
];

const MODE_CHOICES: &[Choice] = &[
    Choice {
        label: c"Dock",
        symbol: "dock",
    },
    Choice {
        label: c"Taskbar",
        symbol: "taskbar",
    },
];

const LAUNCHPAD_CLOSE_SIDE_CHOICES: &[Choice] = &[
    Choice {
        label: c"Left",
        symbol: "left",
    },
    Choice {
        label: c"Right",
        symbol: "right",
    },
];

/// What the "Add an item" row can put on the dock: the ready-made widgets of
/// `(lambdock presets)`, by preset name.
const ADD_ITEM_CHOICES: &[Choice] = &[
    Choice {
        label: c"Trash",
        symbol: "trash",
    },
    Choice {
        label: c"Clock",
        symbol: "clock",
    },
    Choice {
        label: c"RAM usage",
        symbol: "ram",
    },
    Choice {
        label: c"CPU usage",
        symbol: "cpu",
    },
    Choice {
        label: c"CPU temperature",
        symbol: "cpu-temp",
    },
    Choice {
        label: c"Battery",
        symbol: "battery",
    },
    Choice {
        label: c"Swap",
        symbol: "swap",
    },
    Choice {
        label: c"Uptime",
        symbol: "uptime",
    },
    Choice {
        label: c"Load average",
        symbol: "load",
    },
];

// ============================================================================
// 5. THE FORM, AS DATA
// ============================================================================

const BEHAVIOUR_SWITCHES: &[SwitchSpec] = &[
    SwitchSpec {
        key: "dock-auto-hide?",
        title: c"Auto-hide",
        subtitle: c"Slide the dock away when the pointer leaves it.",
        read: get_autohide_config_for,
    },
    SwitchSpec {
        key: "dock-show-when-clear?",
        title: c"Show when no windows",
        subtitle: c"Reveal the dock when the last window is closed.",
        read: get_show_when_clear_config_for,
    },
    SwitchSpec {
        key: "dock-hover-animate?",
        title: c"Hover magnification",
        subtitle: c"Grow the icon under the pointer.",
        read: get_hover_animate_config_for,
    },
    SwitchSpec {
        key: "dock-track-windows?",
        title: c"Track windows",
        subtitle: c"Show running indicators and window menus.",
        read: get_track_windows_config_for,
    },
    SwitchSpec {
        key: "dock-show-window-titles?",
        title: c"Window titles",
        subtitle: c"Show window titles on running taskbar buttons.",
        read: get_show_window_titles_config_for,
    },
];

const BEHAVIOUR_CHOICES: &[ChoiceSpec] = &[ChoiceSpec {
    key: "dock-mode",
    title: c"Window mode",
    subtitle: c"A floating dock or a full-width taskbar.",
    choices: MODE_CHOICES,
    read: read_mode_choice,
}];

/// The one number in the behaviour section: how eager auto-hide is.
const BEHAVIOUR_SCALES: &[ScaleSpec] = &[ScaleSpec {
    key: "dock-autohide-delay",
    title: c"Auto-hide delay",
    subtitle: c"How long the pointer has to be away before the dock slides out. Lower hides sooner; 0 slides it away the moment the pointer leaves.",
    kind: Kind::Int,
    minimum: 0.0,
    maximum: AUTOHIDE_DELAY_MAX_MS as f64,
    step: 25.0,
    read: read_autohide_delay,
}];

const ACCESSIBILITY_SWITCHES: &[SwitchSpec] = &[SwitchSpec {
    key: "dock-reduce-motion?",
    title: c"Reduce motion",
    subtitle: c"Skip the dock's slide and hover magnification. Also follows the desktop's \"enable animations\" setting.",
    read: get_reduce_motion_config_for,
}];

const MATERIAL_SCALES: &[ScaleSpec] = &[
    ScaleSpec {
        key: "dock-icon-size",
        title: c"Icon size",
        subtitle: c"Pixel resolution of the launcher icons.",
        kind: Kind::Int,
        minimum: 16.0,
        maximum: 128.0,
        step: 2.0,
        read: read_icon_size,
    },
    ScaleSpec {
        key: "dock-item-spacing",
        title: c"Item spacing",
        subtitle: c"Gap between icons, in pixels.",
        kind: Kind::Int,
        minimum: 0.0,
        maximum: 32.0,
        step: 1.0,
        read: read_item_spacing,
    },
    ScaleSpec {
        key: "dock-capsule-padding",
        title: c"Capsule padding",
        subtitle: c"Room between the capsule's ends and the icons.",
        kind: Kind::Int,
        minimum: 0.0,
        maximum: 96.0,
        step: 1.0,
        read: read_capsule_padding,
    },
];

const MATERIAL_CHOICES: &[ChoiceSpec] = &[
    ChoiceSpec {
        key: "dock-position",
        title: c"Screen edge",
        subtitle: c"Which edge the dock is attached to.",
        choices: POSITION_CHOICES,
        read: read_position_choice,
    },
    ChoiceSpec {
        key: "dock-theme",
        title: c"Theme",
        subtitle: c"The material the dock is dressed in.",
        choices: THEME_CHOICES,
        read: read_theme_choice,
    },
];

const LAUNCHPAD_SCALES: &[ScaleSpec] = &[
    ScaleSpec {
        key: "dock-launchpad-icon-size",
        title: c"Icon size",
        subtitle: c"Pixel size of an application icon in the grid.",
        kind: Kind::Int,
        minimum: LAUNCHPAD_ICON_SIZE_MIN as f64,
        maximum: LAUNCHPAD_ICON_SIZE_MAX as f64,
        step: 2.0,
        read: read_launchpad_icon_size,
    },
    ScaleSpec {
        key: "dock-launchpad-columns",
        title: c"Columns",
        subtitle: c"Tiles per row; 0 fits as many as the screen allows.",
        kind: Kind::Int,
        minimum: 0.0,
        maximum: LAUNCHPAD_COLUMNS_MAX as f64,
        step: 1.0,
        read: read_launchpad_columns,
    },
];

const LAUNCHPAD_SWITCHES: &[SwitchSpec] = &[SwitchSpec {
    key: "dock-launchpad-blur?",
    title: c"Frosted background",
    subtitle: c"Translucent backdrop. Compositors that blur layer surfaces (KWin by default, niri with a layer-rule) frost it; the tint is dense enough to read the grid without one.",
    read: get_launchpad_blur_config_for,
}];

const SHELF_SCALES: &[ScaleSpec] = &[
    ScaleSpec {
        key: "dock-shelf-margin",
        title: c"Icon overhang",
        subtitle: c"Glass above the icons; negative lets them overhang the slab.",
        kind: Kind::Int,
        minimum: -200.0,
        maximum: 200.0,
        step: 1.0,
        read: read_shelf_margin,
    },
    ScaleSpec {
        key: "dock-shelf-front",
        title: c"Front glass",
        subtitle: c"Glass in front of the icons, where the mirrors live.",
        kind: Kind::Int,
        minimum: 0.0,
        maximum: 200.0,
        step: 1.0,
        read: read_shelf_front,
    },
    ScaleSpec {
        key: "dock-reflection",
        title: c"Reflection height",
        subtitle: c"Mirror band as a fraction of the icon size (0 = none).",
        kind: Kind::Double,
        minimum: 0.0,
        maximum: 2.0,
        step: 0.05,
        read: read_shelf_reflection,
    },
    ScaleSpec {
        key: "dock-reflection-intensity",
        title: c"Reflection intensity",
        subtitle: c"Alpha of the mirror where it touches the icon.",
        kind: Kind::Double,
        minimum: 0.0,
        maximum: 1.0,
        step: 0.05,
        read: read_shelf_reflection_intensity,
    },
    ScaleSpec {
        key: "dock-reflection-blur",
        title: c"Reflection blur",
        subtitle: c"Blur radius as a fraction of the icon size.",
        kind: Kind::Double,
        minimum: 0.0,
        maximum: 0.25,
        step: 0.01,
        read: read_shelf_reflection_blur,
    },
    ScaleSpec {
        key: "dock-shelf-flare",
        title: c"Keystone flare",
        subtitle: c"How much wider the slab is at the screen edge, per side.",
        kind: Kind::Int,
        minimum: -80.0,
        maximum: 80.0,
        step: 1.0,
        read: read_shelf_flare,
    },
    ScaleSpec {
        key: "dock-shelf-tilt",
        title: c"Shelf tilt",
        subtitle: c"Tilt away from the viewer, in degrees (0 = flat).",
        kind: Kind::Double,
        minimum: 0.0,
        maximum: SHELF_TILT_MAX_DEG,
        step: 1.0,
        read: read_shelf_tilt,
    },
    ScaleSpec {
        key: "dock-shelf-perspective",
        title: c"Shelf perspective",
        subtitle: c"Projection distance in pixels; smaller is a stronger 3D.",
        kind: Kind::Double,
        minimum: SHELF_PERSPECTIVE_MIN,
        maximum: SHELF_PERSPECTIVE_MAX,
        step: 10.0,
        read: read_shelf_perspective,
    },
];

const DOCK_ITEM_ACTIONS: &[ActionSpec] = &[ActionSpec {
    key: "dock-add-item",
    title: c"Add an item",
    subtitle: c"Puts a trash can, a clock or a live meter on the dock. The item is appended and remembered, like the ones you drag in; right-click it in the dock to remove it again.",
    button_label: c"Add",
    choices: ADD_ITEM_CHOICES,
}];

const LAUNCHPAD_CHOICES: &[ChoiceSpec] = &[ChoiceSpec {
    key: "dock-launchpad-close-side",
    title: c"Close button",
    subtitle: c"The end of the launchpad's header the close button sits on.",
    choices: LAUNCHPAD_CLOSE_SIDE_CHOICES,
    read: read_launchpad_close_side_choice,
}];

/// Each section lists whatever kinds of row it has; an absent kind is an empty
/// slice, so the pair (rows, count) cannot get out of step.
const SECTIONS: &[Section] = &[
    Section {
        title: c"Behaviour",
        subtitle: c"How the dock responds to windows, the pointer and the session.",
        switches: BEHAVIOUR_SWITCHES,
        scales: BEHAVIOUR_SCALES,
        choices: BEHAVIOUR_CHOICES,
        actions: &[],
    },
    Section {
        title: c"Dock items",
        subtitle: c"Put a widget on this dock. Nothing is installed or uninstalled: the item is added to the dock and remembered with its layout.",
        switches: &[],
        scales: &[],
        choices: &[],
        actions: DOCK_ITEM_ACTIONS,
    },
    Section {
        title: c"Accessibility",
        subtitle: c"Motion comfort. The desktop's own accessibility settings are honoured too.",
        switches: ACCESSIBILITY_SWITCHES,
        scales: &[],
        choices: &[],
        actions: &[],
    },
    Section {
        title: c"Layout & material",
        subtitle: c"Size, spacing, screen edge and the material the dock is dressed in.",
        switches: &[],
        scales: MATERIAL_SCALES,
        choices: MATERIAL_CHOICES,
        actions: &[],
    },
    Section {
        title: c"Launchpad",
        subtitle: c"How the full screen application grid looks when this dock opens it.",
        switches: LAUNCHPAD_SWITCHES,
        scales: LAUNCHPAD_SCALES,
        choices: LAUNCHPAD_CHOICES,
        actions: &[],
    },
    Section {
        title: c"Glass shelf",
        subtitle: c"The 3D slab, its overhang and the reflections on it.",
        switches: &[],
        scales: SHELF_SCALES,
        choices: &[],
        actions: &[],
    },
];

// ============================================================================
// 6. SMALL WIDGET CONSTRUCTORS
// ============================================================================

/// A left-aligned label: the default alignment is centred, which reads wrong in a
/// form.
fn make_label(text: &str) -> gtk4::Label {
    let label = gtk4::Label::new(Some(text));
    label.set_halign(gtk4::Align::Start);
    label.set_xalign(0.0);

    label
}

/// The smaller, dimmer second line of a row or card.
fn make_caption(text: &str) -> gtk4::Label {
    let label = make_label(text);
    label.set_wrap(true);
    label.add_css_class("dim-label");
    label.add_css_class("caption");

    label
}

/// A row: title (and explanation) on the left, control on the right.
fn make_row(title: &str, subtitle: Option<&str>, control: &gtk4::Widget) -> gtk4::Widget {
    let row = gtk4::Box::new(gtk4::Orientation::Horizontal, 18);
    row.set_margin_top(5);
    row.set_margin_bottom(5);
    row.add_css_class("pref-row");

    let text = gtk4::Box::new(gtk4::Orientation::Vertical, 0);
    text.set_hexpand(true);
    text.set_valign(gtk4::Align::Center);
    text.append(&make_label(title));
    if let Some(subtitle) = subtitle {
        text.append(&make_caption(subtitle));
    }
    row.append(&text);

    control.set_valign(gtk4::Align::Center);
    row.append(control);

    row.upcast()
}

/// A section is a rounded card: the title, an optional explanation and the rows it
/// groups. Cards turn a long flat column of controls into something scannable.
fn make_card(title: &str, subtitle: Option<&str>) -> gtk4::Box {
    let card = gtk4::Box::new(gtk4::Orientation::Vertical, 6);
    card.add_css_class("pref-section");

    let heading = make_label(title);
    heading.add_css_class("heading");
    card.append(&heading);

    if let Some(subtitle) = subtitle {
        let caption = make_caption(subtitle);
        caption.set_margin_bottom(4);
        card.append(&caption);
    }

    card
}

// ============================================================================
// 7. CONTROL STATE
// ============================================================================

/// The state attached to `widget`, or `None` for a widget that has none.
fn control_of(widget: &gtk4::Widget) -> Option<ControlHandle> {
    unsafe { widget.data::<ControlHandle>(CONTROL_KEY) }
        .map(|control| unsafe { control.as_ref().clone() })
}

/// Remembers the option name on `widget` as the C string the test and any tooling
/// read: `g_strdup`ed, freed with the widget.
fn attach_key(widget: &gtk4::Widget, key: &str) {
    unsafe {
        glib::gobject_ffi::g_object_set_data_full(
            widget.as_ptr().cast(),
            KEY_KEY.as_ptr(),
            crate::glib::strdup_string(key).cast(),
            Some(crate::glib::g_free),
        );
    }
}

/// Attaches `control` to `widget`; the widget owns it from now on.
fn attach_control(
    widget: &gtk4::Widget,
    config_path: &str,
    key: &str,
    kind: Kind,
    symbols: Vec<String>,
) {
    let control = Rc::new(Control {
        config_path: config_path.to_owned(),
        key: key.to_owned(),
        kind,
        symbols,
        timer: Cell::new(None),
    });

    unsafe { widget.set_data(CONTROL_KEY, control) };
    attach_key(widget, key);
}

/// The symbol of the selected row of a dropdown control.
fn selected_symbol(control: &Control, selected: u32) -> Option<&str> {
    control.symbols.get(selected as usize).map(String::as_str)
}

// ============================================================================
// APPLYING A VALUE (widget -> setter)
// ============================================================================

/// The value of `widget`, written through `control`: that is the whole side effect
/// of an edit.
fn apply_value(control: &Control, widget: &gtk4::Widget) {
    let path = crate::cstr::cstring(&control.config_path);
    let key = crate::cstr::cstring(&control.key);

    unsafe {
        match control.kind {
            Kind::Bool => {
                let switch = widget.downcast_ref::<gtk4::Switch>();
                let active = switch.is_some_and(gtk4::Switch::is_active);
                set_dock_setting_bool(key.as_ptr(), path.as_ptr(), c_int::from(active));
            }
            Kind::Int | Kind::Double => {
                let Some(range) = widget.downcast_ref::<gtk4::Range>() else {
                    return;
                };
                let value = range.value();

                if control.kind == Kind::Int {
                    set_dock_setting_int(key.as_ptr(), path.as_ptr(), value.round() as c_int);
                } else {
                    set_dock_setting_double(key.as_ptr(), path.as_ptr(), value);
                }
            }
            Kind::Symbol => {
                let Some(dropdown) = widget.downcast_ref::<gtk4::DropDown>() else {
                    return;
                };
                let selected = dropdown.selected();
                let Some(symbol) = selected_symbol(control, selected) else {
                    return;
                };

                let symbol = crate::cstr::cstring(symbol);
                set_dock_setting_symbol(key.as_ptr(), path.as_ptr(), symbol.as_ptr());
            }
        }
    }
}

/// Queues the control's value to be written once editing settles.
fn schedule_apply(widget: &gtk4::Widget) {
    let Some(control) = control_of(widget) else {
        return;
    };

    // A newer edit replaces the pending one: the dock is rebuilt once, with the
    // value the user stopped on.
    if let Some(timer) = control.timer.take() {
        timer.remove();
    }

    let weak = widget.downgrade();
    let pending = control.clone();
    let timer = glib::timeout_add_local_once(
        std::time::Duration::from_millis(u64::from(APPLY_DEBOUNCE_MS)),
        move || {
            // The source is gone once it fires: forget its id first, so teardown
            // never tries to remove a dead (possibly recycled) source.
            pending.timer.set(None);

            // The window may have been destroyed while the edit was pending: an
            // unparented widget has nothing left to apply to.
            let Some(widget) = weak.upgrade() else {
                return;
            };
            if widget.root().is_none() {
                return;
            }

            apply_value(&pending, widget.upcast_ref());
        },
    );
    control.timer.set(Some(timer));
}

// ============================================================================
// ROW BUILDERS (declared row -> widgets)
// ============================================================================

fn append_switch_row(box_: &gtk4::Box, config_path: &str, spec: &SwitchSpec) {
    let title = crate::cstr::translated(spec.title);
    let subtitle = crate::cstr::translated(spec.subtitle);

    let switch = gtk4::Switch::new();
    switch.set_active(unsafe { (spec.read)(crate::cstr::cstring(config_path).as_ptr()) } != 0);

    // A bare GtkSwitch has no text of its own: without this a screen reader only
    // says "switch, off" and the user cannot tell which setting it belongs to.
    unsafe {
        let title_c = crate::cstr::cstring(&title);
        let subtitle_c = crate::cstr::cstring(&subtitle);
        crate::accessibility::lambdock_accessible_update(
            switch.as_ptr().cast(),
            title_c.as_ptr(),
            subtitle_c.as_ptr(),
        );
    }

    attach_control(
        switch.upcast_ref(),
        config_path,
        spec.key,
        Kind::Bool,
        Vec::new(),
    );

    switch.connect_active_notify(|switch| schedule_apply(switch.upcast_ref()));

    box_.append(&make_row(&title, Some(&subtitle), switch.upcast_ref()));
}

/// A box holding the slider and its exact-value field.
fn make_range_control(
    spec: &ScaleSpec,
    config_path: &str,
    value: f64,
    title: &str,
    subtitle: &str,
) -> gtk4::Widget {
    let digits = spec.kind.digits();

    let control = gtk4::Box::new(gtk4::Orientation::Horizontal, 10);
    control.set_valign(gtk4::Align::Center);

    let slider = gtk4::Scale::with_range(
        gtk4::Orientation::Horizontal,
        spec.minimum,
        spec.maximum,
        spec.step,
    );
    slider.set_digits(digits as i32);
    slider.set_value_pos(gtk4::PositionType::Right);
    slider.set_value(value);
    slider.set_size_request(180, -1);
    slider.set_valign(gtk4::Align::Center);

    let spin = gtk4::SpinButton::with_range(spec.minimum, spec.maximum, spec.step);
    spin.set_digits(digits);
    spin.set_numeric(true);
    spin.set_increments(spec.step, spec.step.max(spec.step * 10.0));
    spin.set_width_chars(if digits > 0 { 6 } else { 5 });
    spin.set_value(value);
    spin.set_valign(gtk4::Align::Center);
    spin.set_tooltip_text(Some(&crate::cstr::translated(c"Type an exact value")));

    // The slider and the field are one setting; both say which.
    unsafe {
        let title_c = crate::cstr::cstring(title);
        let subtitle_c = crate::cstr::cstring(subtitle);
        crate::accessibility::lambdock_accessible_update(
            slider.as_ptr().cast(),
            title_c.as_ptr(),
            subtitle_c.as_ptr(),
        );

        let spin_title =
            crate::glib::g_strdup_printf(crate::glib::tr(c"%s, exact value"), title_c.as_ptr());
        let spin_description = crate::cstr::translated_dup(c"Type a value, or use the arrow keys.");
        crate::accessibility::lambdock_accessible_update(
            spin.as_ptr().cast(),
            spin_title,
            spin_description,
        );
        crate::glib::g_free(spin_title.cast());
        crate::glib::g_free(spin_description.cast());
    }

    let spin_for_range = spin.clone();
    slider.connect_value_changed(move |slider| {
        let value = slider.value();
        if values_differ(spin_for_range.value(), value) {
            spin_for_range.set_value(value);
        }
    });
    let slider_for_spin = slider.clone();
    spin.connect_value_changed(move |spin| {
        let value = spin.value();
        if values_differ(slider_for_spin.value(), value) {
            slider_for_spin.set_value(value);
        }
    });

    let slider_widget: gtk4::Widget = slider.clone().upcast();
    attach_control(&slider_widget, config_path, spec.key, spec.kind, Vec::new());
    slider.connect_value_changed(|slider| schedule_apply(slider.upcast_ref()));

    control.append(&slider);
    control.append(&spin);

    control.upcast()
}

fn append_scale_row(box_: &gtk4::Box, config_path: &str, spec: &ScaleSpec) {
    let title = crate::cstr::translated(spec.title);
    let subtitle = crate::cstr::translated(spec.subtitle);

    let path = crate::cstr::cstring(config_path);
    let value = unsafe { (spec.read)(path.as_ptr()) };

    let control = make_range_control(spec, config_path, value, &title, &subtitle);

    box_.append(&make_row(&title, Some(&subtitle), &control));
}

/// The labels of `choices`, as a `GtkStringList` a dropdown can show.
fn choice_model(choices: &[Choice]) -> gtk4::StringList {
    let labels: Vec<&str> = choices
        .iter()
        .map(|choice| choice.label.to_str().unwrap_or_default())
        .collect();

    gtk4::StringList::new(&labels)
}

fn append_choice_row(box_: &gtk4::Box, config_path: &str, spec: &ChoiceSpec) {
    let title = crate::cstr::translated(spec.title);
    let subtitle = crate::cstr::translated(spec.subtitle);

    let model = choice_model(spec.choices);
    let dropdown = gtk4::DropDown::new(Some(model), None::<gtk4::Expression>);

    let path = crate::cstr::cstring(config_path);
    let selected = unsafe { (spec.read)(path.as_ptr()) };
    if (selected as usize) < spec.choices.len() {
        dropdown.set_selected(selected);
    }

    unsafe {
        let title_c = crate::cstr::cstring(&title);
        let subtitle_c = crate::cstr::cstring(&subtitle);
        crate::accessibility::lambdock_accessible_update(
            dropdown.as_ptr().cast(),
            title_c.as_ptr(),
            subtitle_c.as_ptr(),
        );
    }

    let symbols = spec
        .choices
        .iter()
        .map(|choice| choice.symbol.to_owned())
        .collect();
    attach_control(
        dropdown.upcast_ref(),
        config_path,
        spec.key,
        Kind::Symbol,
        symbols,
    );
    dropdown.connect_selected_notify(|dropdown| schedule_apply(dropdown.upcast_ref()));

    box_.append(&make_row(&title, Some(&subtitle), dropdown.upcast_ref()));
}

// ============================================================================
// ACTION ROWS (a dropdown plus a button)
// ============================================================================
//
// Some rows do something instead of storing a value. The dock's own widgets are
// one: the item is appended to the dock's layout override (the same file drag and
// drop writes), which is what makes it survive a restart without touching the
// settings file.

/// The label of the selected row of a dropdown, for messages.
fn dropdown_selected_label(dropdown: &gtk4::DropDown) -> String {
    dropdown
        .selected_item()
        .and_then(|item| item.downcast::<gtk4::StringObject>().ok())
        .map(|item| item.string().to_string())
        .unwrap_or_default()
}

/// Runs the row's command for the symbol currently selected in its dropdown.
fn run_action_row(dropdown: &gtk4::DropDown, config_path: &str, announce_on: &gtk4::Widget) {
    let Some(control) = control_of(dropdown.upcast_ref()) else {
        return;
    };

    let selected = dropdown.selected();
    let Some(preset) = selected_symbol(&control, selected) else {
        return;
    };

    // "" is the default dock: the bridge resolves it to the default settings file's
    // layout, the same file a path-ed dock would write.
    unsafe {
        let path = crate::cstr::cstring(config_path);
        let preset_c = crate::cstr::cstring(preset);
        dock_layout_add_preset_item(path.as_ptr(), preset_c.as_ptr());
    }
    reload_docks_for(config_path);

    let target = if config_path.is_empty() {
        "default"
    } else {
        config_path
    };
    let label = dropdown_selected_label(dropdown);
    log_info!(
        "UI",
        "Added dock item '{}' ({preset}) to [{target}]",
        if label.is_empty() { preset } else { &label }
    );

    // A screen reader has no way of knowing the dock behind the window changed, so
    // say it out loud where the button is.
    unsafe {
        let name = if label.is_empty() { preset } else { &label };
        let name_c = crate::cstr::cstring(name);
        let message =
            crate::glib::g_strdup_printf(crate::glib::tr(c"%s added to the dock"), name_c.as_ptr());
        crate::accessibility::lambdock_accessible_announce(announce_on.as_ptr().cast(), message, 0);
        crate::glib::g_free(message.cast());
    }
}

fn append_action_row(box_: &gtk4::Box, config_path: &str, spec: &ActionSpec) {
    let title = crate::cstr::translated(spec.title);
    let subtitle = crate::cstr::translated(spec.subtitle);
    let button_label = crate::cstr::translated(spec.button_label);

    let model = choice_model(spec.choices);
    let dropdown = gtk4::DropDown::new(Some(model), None::<gtk4::Expression>);
    dropdown.set_selected(0);

    unsafe {
        let title_c = crate::cstr::cstring(&title);
        let subtitle_c = crate::cstr::cstring(&subtitle);
        crate::accessibility::lambdock_accessible_update(
            dropdown.as_ptr().cast(),
            title_c.as_ptr(),
            subtitle_c.as_ptr(),
        );
    }

    // The dropdown is a control like any other (it is keyed by the row's option
    // name), so the selected symbol is read through the same helper. It never
    // writes a setting, so its kind is only there to carry the symbols.
    let symbols = spec
        .choices
        .iter()
        .map(|choice| choice.symbol.to_owned())
        .collect();
    attach_control(
        dropdown.upcast_ref(),
        config_path,
        spec.key,
        Kind::Symbol,
        symbols,
    );

    let button = gtk4::Button::with_label(&button_label);
    button.set_valign(gtk4::Align::Center);
    unsafe {
        let label_c = crate::cstr::cstring(&button_label);
        let subtitle_c = crate::cstr::cstring(&subtitle);
        crate::accessibility::lambdock_accessible_update(
            button.as_ptr().cast(),
            label_c.as_ptr(),
            subtitle_c.as_ptr(),
        );
    }

    // The same keyed contract every control in this form has: "dock-add-item" is
    // the choice, so its button is that name plus "-button".
    attach_key(button.upcast_ref(), &format!("{}-button", spec.key));

    let path = config_path.to_owned();
    let dropdown_for_click = dropdown.clone();
    button.connect_clicked(move |button| {
        run_action_row(&dropdown_for_click, &path, button.upcast_ref());
    });

    let control = gtk4::Box::new(gtk4::Orientation::Horizontal, 10);
    control.set_valign(gtk4::Align::Center);
    control.append(&dropdown);
    control.append(&button);

    box_.append(&make_row(&title, Some(&subtitle), control.upcast_ref()));
}

// ============================================================================
// 8. CONTENT
// ============================================================================

fn append_section(box_: &gtk4::Box, section: &Section, config_path: &str) {
    let card = make_card(
        &crate::cstr::translated(section.title),
        Some(&crate::cstr::translated(section.subtitle)),
    );

    for spec in section.switches {
        append_switch_row(&card, config_path, spec);
    }
    for spec in section.scales {
        append_scale_row(&card, config_path, spec);
    }
    for spec in section.choices {
        append_choice_row(&card, config_path, spec);
    }
    for spec in section.actions {
        append_action_row(&card, config_path, spec);
    }

    box_.append(&card);
}

fn build_content(config_path: Option<&str>) -> gtk4::Widget {
    let path = config_path_or_default(config_path);

    let content = gtk4::Box::new(gtk4::Orientation::Vertical, 18);
    content.set_margin_start(26);
    content.set_margin_end(26);
    content.set_margin_top(14);
    content.set_margin_bottom(26);

    for section in SECTIONS {
        append_section(&content, section, path);
    }

    content.upcast()
}

/// The form on its own, without a window: what the tests drive.
///
/// # Safety
///
/// `config_path` must be null or NUL-terminated. The result is a new widget.
pub unsafe fn lambdock_preferences_content_for(
    config_path: *const c_char,
) -> *mut gtk4::ffi::GtkWidget {
    crate::ui::ensure_gtk_initialized();

    let path = crate::cstr::text(config_path);
    let content = build_content(path.as_deref());

    gtk4::glib::gobject_ffi::g_object_force_floating(content.as_ptr().cast());

    content.into_glib_ptr()
}

// ============================================================================
// DOCK RELOAD (the only thing "Reset" shares with the controls)
// ============================================================================

/// Reloads every dock bound to `config_path`, "" being the default dock.
fn reload_docks_for(config_path: &str) {
    /// What `reload_docks_for` hands the registry walk: the settings path to
    /// reload, as the C string the visitor reads.
    unsafe fn visit(state: *mut crate::state::LambdockState, wanted: *mut c_void) {
        let Some(state) = state.as_ref() else {
            return;
        };
        let Some(wanted) = crate::cstr::text(wanted.cast()) else {
            return;
        };

        let bound = crate::cstr::text(state.config_path).unwrap_or_default();
        if config_path_or_default(Some(&bound)) == wanted {
            reload_dock_instance(state as *const _ as *mut _);
        }
    }

    let wanted = crate::cstr::cstring(config_path);

    // `dock_registry_foreach` walks a snapshot, so a reload that tears a dock down
    // cannot pull the ground out from under the walk.
    unsafe {
        crate::dock_registry::dock_registry_foreach(Some(visit), wanted.as_ptr().cast_mut().cast())
    };
}

// ============================================================================
// 9. WINDOWS
// ============================================================================

/// The state a preferences window keeps on itself.
fn window_state_of(widget: &gtk4::Widget) -> Option<Rc<WindowState>> {
    unsafe { widget.data::<Rc<WindowState>>(WINDOW_KEY) }
        .map(|state| unsafe { state.as_ref().clone() })
}

thread_local! {
    /// The open windows, keyed by the settings path they edit.
    ///
    /// Weak, so a window that was destroyed is simply absent rather than a dead
    /// pointer the C version had to type-check before using. Thread-local
    /// because a widget belongs to the main thread: there is nothing to share,
    /// and a mutex would only invite another thread into GTK.
    static REGISTRY: RefCell<HashMap<String, glib::WeakRef<gtk4::Window>>> =
        RefCell::new(HashMap::new());
}

fn registry_lookup(key: &str) -> Option<gtk4::Window> {
    REGISTRY.with_borrow(|registry| registry.get(key).and_then(glib::WeakRef::upgrade))
}

fn registry_publish(key: &str, window: &gtk4::Window) {
    REGISTRY.with_borrow_mut(|registry| {
        registry.insert(key.to_owned(), window.downgrade());
    });
}

fn registry_forget(key: &str) {
    REGISTRY.with_borrow_mut(|registry| {
        registry.remove(key);
    });
}

/// The header bar: the window title and the "Reset" action.
fn make_header_bar(window: &gtk4::Widget) -> gtk4::HeaderBar {
    let header = gtk4::HeaderBar::new();

    let title = gtk4::Label::new(Some(&crate::cstr::translated(c"Lambdock Preferences")));
    title.add_css_class("title");
    header.set_title_widget(Some(&title));

    let reset = gtk4::Button::with_label(&crate::cstr::translated(c"Reset"));
    reset.set_tooltip_text(Some(&crate::cstr::translated(
        c"Forget the choices made here and go back to the settings file",
    )));

    let window = window.clone();
    reset.connect_clicked(move |_| {
        let Some(state) = window_state_of(&window) else {
            return;
        };
        if config_path_is_default(&state.config_path) {
            return;
        }

        let path = crate::cstr::cstring(&state.config_path);

        // Forget the GUI overrides, reload the settings file on top of the
        // now-empty override, and redraw every dock bound to it.
        unsafe {
            reset_dock_preferences(path.as_ptr());
            load_dock_config_scm(path.as_ptr());
        }
        reload_docks_for(&state.config_path);

        if let Some(scrolled) = state.scrolled.upgrade() {
            let content = build_content(Some(&state.config_path));
            scrolled.set_child(Some(&content));
        }
    });

    header.pack_end(&reset);

    header
}

/// The scrollable form, which a reset replaces the child of.
fn make_scrolled_content(config_path: &str) -> gtk4::ScrolledWindow {
    let scrolled = gtk4::ScrolledWindow::new();

    scrolled.set_policy(gtk4::PolicyType::Never, gtk4::PolicyType::Automatic);
    scrolled.set_propagate_natural_width(true);
    scrolled.set_child(Some(&build_content(Some(config_path))));

    scrolled
}

fn new_window(app: &gtk4::Application, config_path: &str) -> gtk4::Window {
    let window = gtk4::ApplicationWindow::new(app);

    window.set_title(Some(&crate::cstr::translated(c"Lambdock Preferences")));
    window.set_default_size(760, 860);
    window.set_hide_on_close(true);
    window.add_css_class("lambdock-preferences");

    let scrolled = make_scrolled_content(config_path);
    window.set_child(Some(&scrolled));

    let state = Rc::new(WindowState {
        config_path: config_path.to_owned(),
        scrolled: scrolled.downgrade(),
    });
    unsafe { window.set_data(WINDOW_KEY, state) };

    let window_widget: &gtk4::Widget = window.upcast_ref();
    window.set_titlebar(Some(&make_header_bar(window_widget)));

    // Escape closes the preferences window, as it does the launchpad and the stack
    // popovers.
    let keys = gtk4::EventControllerKey::new();
    keys.set_propagation_phase(gtk4::PropagationPhase::Capture);
    let window_for_keys = window.clone();
    keys.connect_key_pressed(move |_, keyval, _, _| {
        if keyval != gtk4::gdk::Key::Escape {
            return glib::Propagation::Proceed;
        }

        window_for_keys.close();

        glib::Propagation::Stop
    });
    window.add_controller(keys);

    window.connect_destroy(|window| {
        if let Some(state) = window_state_of(window.upcast_ref()) {
            registry_forget(&state.config_path);
        }
    });

    window.upcast()
}

fn show_window(app: &gtk4::Application, config_path: Option<&str>) {
    let key = config_path_or_default(config_path);

    if let Some(existing) = registry_lookup(key) {
        existing.present();
        return;
    }

    let window = new_window(app, key);

    registry_publish(key, &window);
    window.present();
}

/// Opens (or re-presents) the preferences window for one dock.
///
/// # Safety
///
/// `config_path` must be null or NUL-terminated.
pub unsafe fn lambdock_preferences_show(config_path: *const c_char) {
    // `g_application_get_default`, downcast the way the C cast it with
    // `GTK_APPLICATION` -- but refused instead of assumed.
    let Some(app) =
        gtk4::gio::Application::default().and_then(|app| app.downcast::<gtk4::Application>().ok())
    else {
        log_warn!("UI", "Cannot open the preferences without an application");
        return;
    };

    let path = crate::cstr::text(config_path);
    show_window(&app, path.as_deref());
}

/// The `GAction` handler behind the dock's "Preferences..." menu entry.
///
/// The string target is what makes the menu open the pane for *that* dock when
/// several settings files are in use.
///
/// # Safety
///
/// `parameter` must be null or a `GVariant *`; `user_data` the `GtkApplication`.
pub unsafe fn on_preferences_action(
    _action: *mut gtk4::gio::ffi::GSimpleAction,
    parameter: *mut glib::ffi::GVariant,
    user_data: *mut c_void,
) {
    if user_data.is_null() {
        return;
    }

    let app: gtk4::Application = from_glib_none(user_data.cast());

    // `G_VARIANT_TYPE_STRING` is the type string "s"; there is no symbol for it.
    let config_path = if parameter.is_null()
        || glib::ffi::g_variant_is_of_type(parameter, c"s".as_ptr().cast()) == 0
    {
        None
    } else {
        let text = glib::ffi::g_variant_get_string(parameter, std::ptr::null_mut());
        crate::cstr::text(text)
    };

    show_window(&app, config_path.as_deref());
}

/// Destroys every open preferences window, on application teardown.
pub fn lambdock_preferences_shutdown() {
    // Collected first: destroying a window forgets it, which borrows the
    // registry again.
    let windows: Vec<gtk4::Window> = REGISTRY.with_borrow(|registry| {
        registry
            .values()
            .filter_map(glib::WeakRef::upgrade)
            .collect()
    });

    for window in windows {
        window.destroy();
    }

    REGISTRY.with_borrow_mut(HashMap::clear);
}

#[cfg(test)]
mod tests {
    use super::*;

    /// "" is the default dock, and so is no path at all.
    #[test]
    fn no_path_is_the_default_dock() {
        assert_eq!(config_path_or_default(None), "");
        assert_eq!(config_path_or_default(Some("")), "");
        assert_eq!(
            config_path_or_default(Some("/tmp/settings.scm")),
            "/tmp/settings.scm"
        );
        assert!(config_path_is_default(""));
        assert!(!config_path_is_default("/tmp/settings.scm"));
    }

    /// A dropdown shows the index of the stored symbol, and the first row when
    /// the settings file names a symbol this form does not know.
    #[test]
    fn a_choice_is_the_index_of_its_symbol() {
        assert_eq!(choice_index_of(POSITION_CHOICES, Some("bottom")), 0);
        assert_eq!(choice_index_of(POSITION_CHOICES, Some("right")), 3);
        assert_eq!(choice_index_of(POSITION_CHOICES, Some("sideways")), 0);
        assert_eq!(choice_index_of(POSITION_CHOICES, None), 0);
    }

    /// The spin button mirrors the slider, and mirroring stops on its own once
    /// the two agree -- otherwise each would keep setting the other.
    #[test]
    fn mirroring_stops_when_the_values_agree() {
        assert!(values_differ(64.0, 63.0));
        assert!(values_differ(64.0, 64.0000001));
        assert!(!values_differ(0.3, 0.3));
    }

    /// Every row of the form edits exactly one option, and the keys are the ones
    /// `(lambdock prefs)` answers to.
    #[test]
    fn every_row_has_a_distinct_key() {
        let mut keys: Vec<&str> = Vec::new();
        for section in SECTIONS {
            for spec in section.switches {
                keys.push(spec.key);
            }
            for spec in section.scales {
                keys.push(spec.key);
            }
            for spec in section.choices {
                keys.push(spec.key);
            }
            for spec in section.actions {
                keys.push(spec.key);
            }
        }

        let mut unique = keys.clone();
        unique.sort_unstable();
        unique.dedup();

        assert_eq!(unique.len(), keys.len(), "a key is declared twice");

        // The keys the C test drives are all in the form.
        for wanted in [
            "dock-icon-size",
            "dock-auto-hide?",
            "dock-theme",
            "dock-reflection",
        ] {
            assert!(keys.contains(&wanted), "{wanted} is missing from the form");
        }
    }
}
