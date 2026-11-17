// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! CSS-class toggles.
//!
//! The dock's styling is state-driven: an item gains `running-app` when a window
//! appears and loses it when the last one closes, and the class list is what the
//! stylesheets key on. This is the only place a class is set, so the "does it
//! already have it?" check lives in one place instead of at every call site --
//! `add_css_class` on a class the widget already has costs a class-list walk and a
//! style invalidation, and this runs on the frame path.

use gtk4::prelude::*;
use gtk4::Widget;

/// Whether a class list needs the change at all.
///
/// Adding a class a widget already has, or removing one it does not, costs a
/// class-list walk and a style invalidation for nothing -- and this runs on the
/// frame path, once per state change per item.
fn should_change(has: bool, enabled: bool) -> bool {
    has != enabled
}

/// Sets or clears one CSS class, touching the widget only when it changes.
///
/// Takes a widget and a `&str`, so there is nothing unsafe about it: a class name
/// is UTF-8 by construction, and GTK's class API is safe once the widget is a real
/// one -- which the reference guarantees.
pub fn set_css_class(widget: &Widget, name: &str, enabled: bool) {
    if !should_change(widget.has_css_class(name), enabled) {
        return;
    }

    if enabled {
        widget.add_css_class(name);
    } else {
        widget.remove_css_class(name);
    }
}

#[cfg(test)]
mod tests {
    use super::should_change;

    /// The only decision in the toggle, and the reason it is cheap: a class that
    /// is already there is not added twice, and one that is not there is not
    /// removed.
    #[test]
    fn only_a_missing_class_needs_adding_and_only_a_present_one_needs_clearing() {
        assert!(should_change(false, true), "missing and wanted: add it");
        assert!(!should_change(true, true), "present and wanted: leave it");
        assert!(should_change(true, false), "present and unwanted: clear it");
        assert!(!should_change(false, false), "missing and unwanted: leave it");
    }
}
