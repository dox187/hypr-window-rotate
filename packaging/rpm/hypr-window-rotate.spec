# Hyprland plugins are ABI-locked to the exact Hyprland build they were compiled against,
# so the package requires the very hyprland version-release whose headers were used.
%global hyprland_vr %(rpm -q --qf '%%{VERSION}-%%{RELEASE}' hyprland-devel >/dev/null 2>&1 && rpm -q --qf '%%{VERSION}-%%{RELEASE}' hyprland-devel || echo 0)

Name:           hypr-window-rotate
Version:        0.57.0+1
Release:        1%{?dist}
Summary:        Hyprland plugin to rotate individual windows in 90 degree steps

License:        BSD-3-Clause
URL:            https://github.com/dox187/hypr-window-rotate
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  pkgconf-pkg-config
BuildRequires:  hyprland-devel
BuildRequires:  pkgconfig(lua)

%if "%{hyprland_vr}" != "0"
Requires:       hyprland%{?_isa} = %{hyprland_vr}
%endif

%description
hypr-window-rotate is a Hyprland plugin that rotates individual windows in
90 degree steps. The window keeps its place in the layout; the application is
configured with the swapped size, drawn rotated, and pointer input is mapped
back, so a rotated window stays fully usable.

%prep
# GitHub replaces the "+" of the tag with "-" in the archive's top directory
%autosetup -n %{lua: print((rpm.expand("%{name}-%{version}"):gsub("%+", "-")))}

%build
%make_build VERSION=%{version} CXXFLAGS="%{optflags}" LDFLAGS="%{build_ldflags}"

%install
%make_install PLUGINDIR=%{_libdir}/hyprland

%files
%license LICENSE
%doc README.md examples/
%{_libdir}/hyprland/hypr-window-rotate.so

%changelog
* Mon Nov 02 2026 dox187 <1054544+dox187@users.noreply.github.com> - 0.57.0+1-1
- Port to Hyprland 0.57.0, version scheme <Hyprland version>+<release>
- Drop the window_rotate dispatcher: Hyprland 0.57 removed hyprlang configs

* Wed Oct 28 2026 dox187 <1054544+dox187@users.noreply.github.com> - 0.1.0-1
- Initial package
