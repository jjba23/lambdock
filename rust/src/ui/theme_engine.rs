// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! Themes and CSS: `src/ui/theme_engine.c` in Rust.
//!
//! Loads the built-in theme from the GResource bundle, scopes its selectors to
//! one dock's CSS class, and layers the user's `styles.css` over it.
//!
//! Two halves, and the split is the point: what to load is a *value*
//! ([`resolve_theme`]) and how to rewrite a selector is a *string function*
//! ([`scope_selectors`]), so both are tested without a display. Only the appliers
//! at the bottom touch GTK, and they only do what the value says.

use crate::glib::{g_free, strdup_string};
use crate::log_info;
use crate::log_warn;
use crate::state::LambdockState;
use gtk4::gdk;
use gtk4::gio;
use gtk4::glib;
use gtk4::prelude::*;
use gtk4::CssProvider;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int};
use crate::ui::shelf::{lambdock_shelf_active, lambdock_shelf_geometry_for};

// Where the theme engine reaches back into the modules that are still C.

/// What `load_custom_css` has to decide before it touches a widget.
///
/// Everything here comes from plain arguments, so the rules below are testable
/// without a display.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ThemeResolution {
    pub theme_name: String,
    pub theme_class: String,
    pub disabled: bool,
    pub taskbar_modifier: bool,
    pub shelf_modifier: bool,
    /// The glass draws the running indicator itself, so the theme's must go.
    pub shelf_indicator: bool,
}

/// The theme request, as `src/ui/theme_engine.h` declares it: the field order is
/// the contract, and the two strings are `(transfer full)`.
#[repr(C)]
pub struct ThemeRequest {
    pub theme_name: *mut c_char,
    pub theme_class: *mut c_char,
    pub disabled: c_int,
    pub taskbar_modifier: c_int,
    pub shelf_modifier: c_int,
    pub shelf_indicator: c_int,
}

/// Which theme to load and which modifiers to layer, from the dock's settings.
///
/// `None` or an empty name means `vanilla`; the comparison for `none` is
/// case-insensitive, matching the stylesheet names on disk.
pub fn resolve_theme(
    requested_theme: Option<&str>,
    taskbar: bool,
    show_window_titles: bool,
    shelf_active: bool,
    shelf_indicator: bool,
) -> ThemeResolution {
    let theme_name = match requested_theme {
        Some(name) if !name.is_empty() => name,
        _ => "vanilla",
    };

    ThemeResolution {
        theme_name: theme_name.to_owned(),
        theme_class: format!("theme-{theme_name}"),
        disabled: theme_name.eq_ignore_ascii_case("none"),
        taskbar_modifier: taskbar || show_window_titles,
        shelf_modifier: shelf_active,
        shelf_indicator: shelf_active && shelf_indicator,
    }
}

/// Rewrites every rule selector so it only applies inside a scope wrapper.
///
/// Element selectors like `window` are compounded (`window.theme-vanilla`),
/// everything else is nested (`.theme-vanilla .foo`). Comments and top-level
/// @-rules pass through untouched.
pub fn scope_selectors(css: &str, scope_class: &str) -> String {
    if css.is_empty() {
        return String::new();
    }

    let mut result = String::new();
    // A selector list may span lines (`button.window-focused,` then `:hover {`),
    // so the text before a `{` is accumulated until the block actually opens.
    let mut pending = String::new();
    let mut in_ruleset = false;

    for line in css.split('\n') {
        let trimmed = line.trim();

        // Inside a declaration block nothing is a selector: pass it through.
        if in_ruleset {
            if line.contains('}') {
                in_ruleset = false;
            }

            result.push_str(line);
            result.push('\n');
            continue;
        }

        // Empty lines, comments, top-level @-rules and the star rule too.
        if trimmed.is_empty()
            || trimmed.starts_with("/*")
            || trimmed.starts_with('@')
            || trimmed.starts_with('*')
        {
            result.push_str(line);
            result.push('\n');
            continue;
        }

        let Some(brace) = line.find('{') else {
            pending.push_str(line);
            pending.push('\n');
            continue;
        };

        pending.push_str(&line[..brace]);
        scope_selector_list(&mut result, &pending, scope_class);
        pending.clear();

        let body = &line[brace + 1..];
        in_ruleset = !body.contains('}');

        result.push_str(" {");
        result.push_str(body);
        result.push('\n');
    }

    result
}

/// Scopes one selector list (the text before a `{`, possibly spanning lines)
/// under `scope_class` and appends it to `out`.
fn scope_selector_list(out: &mut String, selectors: &str, scope_class: &str) {
    let mut first = true;

    for part in selectors.split(',') {
        let selector = part.trim();
        if selector.is_empty() {
            continue;
        }

        if !first {
            out.push_str(", ");
        }
        first = false;

        // Compound the `window` element selector directly with the scope class,
        // but only when `window` is a whole element ("window",
        // "window.background"), never a different element whose name merely
        // starts with it ("windowcontrols").
        match selector
            .strip_prefix("window")
            .map(|rest| rest.chars().next())
        {
            Some(None) | Some(Some('.')) | Some(Some(':')) | Some(Some('[')) => {
                let rest = &selector["window".len()..];
                out.push_str(&format!("window.{scope_class}{rest}"));
            }
            Some(Some(next)) if next.is_ascii_whitespace() => {
                let rest = &selector["window".len()..];
                out.push_str(&format!("window.{scope_class}{rest}"));
            }
            _ => out.push_str(&format!(".{scope_class} {selector}")),
        }
    }
}

/// "default" for the config-less fallback dock, matching every other log line.
fn dock_name(state: &LambdockState) -> String {
    if state.config_path.is_null() {
        return "default".to_owned();
    }

    // Safety: `config_path` is a C string the dock owns for its lifetime.
    unsafe {
        CStr::from_ptr(state.config_path)
            .to_string_lossy()
            .into_owned()
    }
}

/// Scopes `raw_css` under `theme_class`, installs it on `display` and returns it;
/// `None` when there is nothing to load, so callers can chain modifiers freely.
fn provider_from_scoped_css(
    display: &gdk::Display,
    raw_css: &str,
    theme_class: &str,
    priority: u32,
) -> Option<CssProvider> {
    if raw_css.is_empty() {
        return None;
    }

    let provider = CssProvider::new();
    provider.load_from_string(&scope_selectors(raw_css, theme_class));
    gtk4::style_context_add_provider_for_display(display, &provider, priority);

    Some(provider)
}

/// The same, for a stylesheet embedded in the GResource bundle.
fn provider_from_resource(
    display: &gdk::Display,
    resource_path: &str,
    theme_class: &str,
    priority: u32,
) -> Option<CssProvider> {
    let bytes = gio::resources_lookup_data(resource_path, gio::ResourceLookupFlags::NONE).ok()?;
    let raw_css = String::from_utf8_lossy(bytes.as_ref());

    provider_from_scoped_css(display, &raw_css, theme_class, priority)
}

/// Stores `provider` in `slot`, dropping whatever it held, which is what
/// unregisters the previous one. `true` when there was one to store.
fn store_provider(provider: Option<CssProvider>, slot: &mut Option<CssProvider>) -> bool {
    match provider {
        Some(provider) => {
            *slot = Some(provider);
            true
        }
        None => false,
    }
}

/// Removes the previous theme class and tags the window with the new one.
unsafe fn swap_window_class(state: &mut LambdockState, theme_class: &str) {
    let window = state.window.as_ref();

    if !state.active_theme_class.is_null() {
        let previous = CStr::from_ptr(state.active_theme_class)
            .to_string_lossy()
            .into_owned();
        if let Some(window) = window {
            window.remove_css_class(&previous);
        }
        g_free(state.active_theme_class.cast());
    }

    state.active_theme_class = strdup_string(theme_class);
    if let Some(window) = window {
        window.add_css_class(theme_class);
    }
}

/// Unhooks every provider this engine installed for `state`, in one pass.
///
/// A reload must swap providers, never stack them: the state holds the five of
/// them precisely so they can be released here.
unsafe fn detach_providers(state: &mut LambdockState, display: &gdk::Display) {
    for slot in [
        &mut state.base_css_provider,
        &mut state.taskbar_css_provider,
        &mut state.shelf_css_provider,
        &mut state.indicator_guard_css_provider,
        &mut state.user_css_provider,
    ] {
        if let Some(provider) = slot.take() {
            gtk4::style_context_remove_provider_for_display(display, &provider);
        }
    }
}

/// Layers the taskbar and shelf modifiers in before the theme, so a theme rule of
/// equal specificity still wins over them.
unsafe fn attach_modifiers(
    state: &mut LambdockState,
    display: &gdk::Display,
    theme: &ThemeResolution,
) {
    let priority = gtk4::STYLE_PROVIDER_PRIORITY_APPLICATION;

    if theme.taskbar_modifier {
        let provider = provider_from_resource(
            display,
            "/org/jointhefreeworld/lambdock/css/taskbar.css",
            &theme.theme_class,
            priority,
        );

        if store_provider(provider, &mut state.taskbar_css_provider) {
            log_info!(
                "UI",
                "Loaded taskbar/window-title modifier for [{}]",
                dock_name(&*state)
            );
        }
    }

    if theme.shelf_modifier {
        let provider = provider_from_resource(
            display,
            "/org/jointhefreeworld/lambdock/css/shelf.css",
            &theme.theme_class,
            priority,
        );

        if store_provider(provider, &mut state.shelf_css_provider) {
            log_info!(
                "UI",
                "Loaded glass shelf modifier for [{}]",
                dock_name(&*state)
            );
        }
    }
}

/// Loads the base theme, falling back to vanilla when it is not shipped.
unsafe fn attach_base_theme(
    state: &mut LambdockState,
    display: &gdk::Display,
    theme: &ThemeResolution,
) {
    let priority = gtk4::STYLE_PROVIDER_PRIORITY_APPLICATION;
    let resource_path = format!(
        "/org/jointhefreeworld/lambdock/css/{}.css",
        theme.theme_name
    );

    let mut provider =
        provider_from_resource(display, &resource_path, &theme.theme_class, priority);

    if provider.is_none() {
        log_warn!(
            "UI",
            "Theme resource '{resource_path}' not found, falling back to vanilla"
        );
        provider = provider_from_resource(
            display,
            "/org/jointhefreeworld/lambdock/css/vanilla.css",
            &theme.theme_class,
            priority,
        );
    }

    if store_provider(provider, &mut state.base_css_provider) {
        log_info!(
            "UI",
            "Loaded & scoped CSS theme [{}] for [{}]",
            theme.theme_name,
            dock_name(&*state)
        );
    }
}

/// Silences a theme's own running indicator while the shelf draws one.
///
/// This cannot live in shelf.css with the rest of the shelf material. GTK
/// resolves competing declarations from providers of the same priority by the
/// order the providers were added, *not* by selector specificity, and the shelf
/// modifier is deliberately added before the theme so a theme can override the
/// slab. A `window.shelf-mirror button.running-app` rule there therefore lost to
/// the theme's `button.running-app` no matter how specific it was, and every
/// running app got two indicators: the theme's dot at the bottom of the button
/// and the shelf's bead on the glass.
///
/// Attached after the theme, at the same priority, this one rule is the last word
/// on the indicator while the glass is mirroring -- and the user's own
/// stylesheet, attached after even this, still wins over both.
unsafe fn attach_indicator_guard(
    state: &mut LambdockState,
    display: &gdk::Display,
    theme: &ThemeResolution,
) {
    if !theme.shelf_indicator {
        return;
    }

    const GUARD_CSS: &str = "window.shelf-mirror button.running-app { background-image: none; }";

    let provider = provider_from_scoped_css(
        display,
        GUARD_CSS,
        &theme.theme_class,
        gtk4::STYLE_PROVIDER_PRIORITY_APPLICATION,
    );

    store_provider(provider, &mut state.indicator_guard_css_provider);
}

/// Layers `~/.config/lambdock/styles.css` on top of everything, at user priority,
/// so a hand-written override wins over any shipped theme.
unsafe fn attach_user_css(
    state: &mut LambdockState,
    display: &gdk::Display,
    theme: &ThemeResolution,
) {
    let path = glib::user_config_dir().join("lambdock").join("styles.css");
    let Ok(raw_css) = std::fs::read(&path) else {
        return;
    };
    let raw_css = String::from_utf8_lossy(&raw_css);

    let provider = provider_from_scoped_css(
        display,
        &raw_css,
        &theme.theme_class,
        gtk4::STYLE_PROVIDER_PRIORITY_USER,
    );

    if store_provider(provider, &mut state.user_css_provider) {
        log_info!(
            "UI",
            "Loaded & scoped user CSS override for [{}]",
            dock_name(&*state)
        );
    }
}

/// Computes the theme request for `state` and applies it: swaps the scoping
/// class, detaches the previous providers and loads the new ones.
///
/// # Safety
///
/// `state` must be a live dock whose window is set.
pub unsafe fn load_custom_css(state: &mut LambdockState) {
    if state.window.is_none() {
        return;
    }

    crate::ui::ensure_gtk_initialized();

    let Some(display) = gdk::Display::default() else {
        return;
    };

    let shelf_active = lambdock_shelf_active(&*state) != 0;
    // The glass draws the running indicator only when it shows reflections: that
    // is the band the bead is painted on.
    let shelf_indicator = lambdock_shelf_geometry_for(&*state, state.icon_size).band_px > 0;

    let requested_theme = if state.theme.is_null() {
        None
    } else {
        Some(CStr::from_ptr(state.theme).to_string_lossy())
    };

    let theme = resolve_theme(
        requested_theme.as_deref(),
        state.taskbar != 0,
        state.show_window_titles != 0,
        shelf_active,
        shelf_indicator,
    );

    swap_window_class(state, &theme.theme_class);
    detach_providers(state, &display);

    if theme.disabled {
        log_info!("UI", "Theme set to 'none' for [{}].", dock_name(&*state));
        return;
    }

    // Order matters: modifiers first, then the theme over them, then the guard
    // that silences the theme's indicator, then the user's own stylesheet over
    // everything.
    attach_modifiers(state, &display, &theme);
    attach_base_theme(state, &display, &theme);
    attach_indicator_guard(state, &display, &theme);
    attach_user_css(state, &display, &theme);
}

/// Builds the request `load_custom_css` applies, as a value C can hold.
///
/// # Safety
///
/// `requested_theme` must be null or a C string; the two strings in the result
/// are owned by the caller and freed with `lambdock_theme_request_clear`.
pub unsafe fn lambdock_theme_request(
    requested_theme: *const c_char,
    taskbar: c_int,
    show_window_titles: c_int,
    shelf_active: c_int,
    shelf_indicator: c_int,
) -> ThemeRequest {
    let requested_theme = if requested_theme.is_null() {
        None
    } else {
        Some(CStr::from_ptr(requested_theme).to_string_lossy())
    };

    let theme = resolve_theme(
        requested_theme.as_deref(),
        taskbar != 0,
        show_window_titles != 0,
        shelf_active != 0,
        shelf_indicator != 0,
    );

    ThemeRequest {
        theme_name: strdup_string(&theme.theme_name),
        theme_class: strdup_string(&theme.theme_class),
        disabled: theme.disabled as c_int,
        taskbar_modifier: theme.taskbar_modifier as c_int,
        shelf_modifier: theme.shelf_modifier as c_int,
        shelf_indicator: theme.shelf_indicator as c_int,
    }
}

/// Releases the strings a `ThemeRequest` owns; safe to run twice.
///
/// # Safety
///
/// `request` must be null or a record from `lambdock_theme_request`.
pub unsafe fn lambdock_theme_request_clear(request: *mut ThemeRequest) {
    if request.is_null() {
        return;
    }

    let request = &mut *request;

    if !request.theme_name.is_null() {
        g_free(request.theme_name.cast());
        request.theme_name = std::ptr::null_mut();
    }

    if !request.theme_class.is_null() {
        g_free(request.theme_class.cast());
        request.theme_class = std::ptr::null_mut();
    }

    // Every flag goes back to rest, `shelf_indicator` included: the C version
    // forgot that one, which left a stale "the glass draws the bead" behind.
    request.disabled = 0;
    request.taskbar_modifier = 0;
    request.shelf_modifier = 0;
    request.shelf_indicator = 0;
}

/// Scopes every selector in `css` under `scope_class`.
///
/// # Safety
///
/// `css` and `scope_class` must be null or C strings; the result is `(transfer
/// full)` and freed with `g_free`.
pub unsafe fn theme_scope_css_selectors(
    css: *const c_char,
    scope_class: *const c_char,
) -> *mut c_char {
    if css.is_null() || scope_class.is_null() {
        return strdup_string("");
    }

    let css = CStr::from_ptr(css).to_string_lossy();
    let scope_class = CStr::from_ptr(scope_class).to_string_lossy();

    strdup_string(&scope_selectors(&css, &scope_class))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A dock with no theme configured gets vanilla, not an empty class.
    #[test]
    fn a_dock_with_no_theme_gets_vanilla() {
        for requested in [None, Some("")] {
            let theme = resolve_theme(requested, false, false, false, false);

            assert_eq!(theme.theme_name, "vanilla");
            assert_eq!(theme.theme_class, "theme-vanilla");
            assert!(!theme.disabled);
            assert!(!theme.taskbar_modifier);
            assert!(!theme.shelf_modifier);
        }
    }

    /// `none` still names a class -- the old one has to be swapped out -- but
    /// loads nothing, whatever the modifiers say.
    #[test]
    fn the_none_theme_disables_everything() {
        let none = resolve_theme(Some("none"), true, true, true, true);
        assert!(none.disabled);
        assert_eq!(none.theme_class, "theme-none");

        assert!(resolve_theme(Some("NONE"), false, false, false, false).disabled);
        assert!(!resolve_theme(Some("ambiance"), false, false, false, false).disabled);
    }

    /// Taskbar mode and window titles both want the modifier; the shelf wants its
    /// own; and the indicator guard needs the shelf *and* a band to draw on.
    #[test]
    fn the_modifiers_follow_the_dock_mode() {
        assert!(resolve_theme(Some("vanilla"), true, false, false, false).taskbar_modifier);
        assert!(resolve_theme(Some("vanilla"), false, true, false, false).taskbar_modifier);
        assert!(!resolve_theme(Some("vanilla"), false, false, false, false).taskbar_modifier);

        assert!(resolve_theme(Some("vanilla"), false, false, true, false).shelf_modifier);

        assert!(resolve_theme(Some("vanilla"), false, false, true, true).shelf_indicator);
        assert!(!resolve_theme(Some("vanilla"), false, false, true, false).shelf_indicator);
        // A shelf with no mirrors has no bead, so the theme's dot must stay.
        assert!(!resolve_theme(Some("vanilla"), false, false, false, true).shelf_indicator);
    }

    #[test]
    fn nothing_to_scope_is_nothing() {
        assert_eq!(scope_selectors("", "theme-x"), "");
    }

    /// Class selectors are nested under the scope wrapper.
    #[test]
    fn class_selectors_are_nested_under_the_scope() {
        let out = scope_selectors(".lambdock-window {\n  color: red;\n}\n", "theme-x");
        assert!(out.contains(".theme-x .lambdock-window {"), "{out}");
        assert!(out.contains("color: red;"));

        // Single-line rule bodies are handled too.
        let out = scope_selectors(".foo { color: red; }", "theme-x");
        assert!(out.contains(".theme-x .foo {"), "{out}");
    }

    /// The `window` element selector is compounded, not nested, and only when
    /// `window` is a complete element name: `windowcontrols` must keep its
    /// `.theme-x ` prefix.
    #[test]
    fn the_window_element_is_compounded_at_its_boundary() {
        let css = "window {\n  background: transparent;\n}\n\
                   window.background {\n  a: b;\n}\n\
                   window:hover {\n  c: d;\n}\n\
                   window[foo] {\n  e: f;\n}\n\
                   windowcontrols {\n  g: h;\n}\n\
                   .window {\n  i: j;\n}\n";
        let out = scope_selectors(css, "theme-x");

        assert!(out.contains("window.theme-x {"), "{out}");
        assert!(out.contains("window.theme-x.background"), "{out}");
        assert!(out.contains("window.theme-x:hover"), "{out}");
        assert!(out.contains("window.theme-x[foo]"), "{out}");

        assert!(out.contains(".theme-x windowcontrols"), "{out}");
        assert!(!out.contains("window.theme-xcontrols"), "{out}");

        // `.window` is a class, not the element.
        assert!(out.contains(".theme-x .window"), "{out}");
    }

    #[test]
    fn a_selector_list_is_scoped_part_by_part() {
        let out = scope_selectors("button, .foo, window {\n  a: b;\n}", "theme-x");
        assert!(
            out.contains(".theme-x button, .theme-x .foo, window.theme-x {"),
            "{out}"
        );
    }

    /// A selector list may span several lines. Every selector in it still has to
    /// be scoped: an unscoped rule applies to every dock and to the whole
    /// application, which is how one theme's indicator once ended up painted
    /// under another theme's icons.
    #[test]
    fn a_multiline_selector_list_is_still_scoped() {
        let out = scope_selectors(
            "button.window-focused,\nbutton.window-focused:hover {\n  color: red;\n}\n",
            "theme-x",
        );
        assert!(
            out.contains(".theme-x button.window-focused, .theme-x button.window-focused:hover {"),
            "{out}"
        );
        assert!(!out.contains("\nbutton.window-focused,"), "{out}");

        let out = scope_selectors(
            "window,\n.lambdock-window.shelf-mode {\n  a: b;\n}\n",
            "theme-x",
        );
        assert!(
            out.contains("window.theme-x, .theme-x .lambdock-window.shelf-mode {"),
            "{out}"
        );
        assert!(!out.contains("\nwindow,"), "{out}");
    }

    /// The block body passes through, including a one-line rule.
    #[test]
    fn the_rule_body_is_kept_verbatim() {
        let out = scope_selectors(".a,\n.b { color: red; }\n.c {\n  d: e;\n}\n", "theme-x");

        assert!(
            out.contains(".theme-x .a, .theme-x .b { color: red; }"),
            "{out}"
        );
        assert!(out.contains(".theme-x .c {"), "{out}");
        assert!(out.contains("  d: e;"), "{out}");
    }

    /// Comments, top-level @-rules and the star rule are not selectors.
    #[test]
    fn comments_and_at_rules_pass_through() {
        let css = "/* a comment */\n@import \"other.css\";\n* {\n  margin: 0;\n}\n";
        let out = scope_selectors(css, "theme-x");

        assert!(out.contains("/* a comment */"), "{out}");
        assert!(out.contains("@import \"other.css\";"), "{out}");
        assert!(out.contains("* {"), "{out}");
    }

    /// The record is two pointers and four booleans, in the order the callers
    /// read it in.
    #[test]
    fn the_request_record_matches_the_header() {
        use std::mem::{offset_of, size_of};

        assert_eq!(size_of::<ThemeRequest>(), 32);
        assert_eq!(offset_of!(ThemeRequest, theme_name), 0);
        assert_eq!(offset_of!(ThemeRequest, theme_class), 8);
        assert_eq!(offset_of!(ThemeRequest, disabled), 16);
        assert_eq!(offset_of!(ThemeRequest, shelf_indicator), 28);
    }
}
