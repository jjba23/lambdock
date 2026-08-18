#
# spec file for package lambdock
#

Name:           lambdock
Version:        0.6.0
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
Requires:       gtk4-layer-shell
Requires:       guile
Requires:       wayland

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

%files
%license COPYING
%doc README.org NEWS.org
%{_bindir}/lambdock
%dir %{_datadir}/guile/site/3.0/lambdock
%{_datadir}/guile/site/3.0/lambdock/*.scm

%changelog
* Tue Aug 18 2026 Josep Bigorra <jjbigorra@gmail.com>
- Initial openSUSE Tumbleweed package
