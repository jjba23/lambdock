Name:           lambdock
Version:        0.1.0
Release:        1%{?dist}
Summary:        Wayland-native GTK4 dock extensible in Guile Scheme

License:        GPL-3.0-or-later
URL:            https://codeberg.org/jjba23/lambdock
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  meson >= 0.62.0
BuildRequires:  ninja-build
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(gtk4-layer-shell-0)
BuildRequires:  pkgconfig(guile-3.0)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(glib2-devel)
BuildRequires:  wayland-scanner

Requires:       guile30

%description
Lambdock is a lightweight, Wayland-native desktop dock built with a C core,
GTK4 layer-shell, and dynamic configuration powered by GNU Guile Scheme.

%prep
%autosetup

%build
%meson
%meson_build

%install
%meson_install

%files
%{_bindir}/lambdock
%{_datadir}/guile/site/3.0/lambdock/
%{_datadir}/lambdock/

%changelog
* Tue Aug 04 2026 Josep Bigorra <jjbigorra@gmail.com> - 0.1.0-1
- Initial RPM package release for lambdock.
