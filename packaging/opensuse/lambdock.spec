#
# spec file for package lambdock
#
# Copyright (c) 2026 SUSE LLC and contributors
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon. The license for this file, and modifications and additions to the
# file, is the same license as for the pristine package itself (unless the
# license for the pristine package is not an Open Source License, in which
# case the license is the MIT License). An "Open Source License" is a
# license that conforms to the Open Source Definition (Version 1.9)
# published by the Open Source Initiative.

# Please submit bugfixes or comments via http://bugs.opensuse.org/
#

Name:           lambdock
Version:        0.6.4
Release:        0
Summary:        Wayland desktop dock customizable with GNU Guile Scheme
License:        GPL-3.0-or-later
Group:          System/GUI/Other
URL:            https://codeberg.org/jjba23/lambdock

Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  ninja
BuildRequires:  pkg-config
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(gtk4-layer-shell-0)
BuildRequires:  pkgconfig(guile-3.0)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-protocols)

Requires:       gtk4
Requires:       guile

%description
Lambdock is a Wayland-native desktop dock built with GTK4,
gtk4-layer-shell, and GNU Guile Scheme.

It provides dynamic Wayland top-level window tracking,
multi-monitor support, configurable application launchers,
smooth auto-hide animations, GTK CSS themes, hot-reloading,
multiple dock instances, and an embedded Guile Scheme runtime
with an optional interactive REPL.

Lambdock is designed for Wayland compositors implementing the
wlr-layer-shell protocol, including Sway, Hyprland, River,
Wayfire, Niri, and other compatible compositors.

%prep
%autosetup

%build
%meson
%meson_build

%install
%meson_install

%check
%meson_test

%files
%license COPYING
%doc README.org NEWS.org
%{_bindir}/lambdock
%dir %{_datadir}/guile
%dir %{_datadir}/guile/site
%{_datadir}/guile/site/3.0/
%dir %{_datadir}/guile/site/3.0/lambdock
%{_datadir}/guile/site/3.0/lambdock/*.scm

%changelog
* Tue Aug 18 2026 Josep Bigorra <jjbigorra@gmail.com>
- Initial openSUSE Tumbleweed package
