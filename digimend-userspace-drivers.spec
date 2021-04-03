%global _hardened_build 1

Name:           digimend-userspace-drivers
Version:        1
Release:        1%{?dist}
Summary:        Terminal I/O logger

Packager:       Nikolai Kondrashov <spbnick@gmail.com>
Group:          Applications/System
License:        GPLv2+
URL:            https://github.com/DIGImend/%{name}
Source:         %{url}/releases/download/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  autoconf
BuildRequires:  automake
BuildRequires:  libtool
BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(libusb)

%description
DIGImend-userspace-driver is a collection of userspace drivers and tools
making various graphics tablets work on Linux.

%prep
%setup -q

%build
%configure --disable-rpath --disable-static --docdir=%{_defaultdocdir}/%{name}
%make_build

%check
%make_build check

%install
%make_install

%files
%{!?_licensedir:%global license %doc}
%license COPYING
%doc %{_defaultdocdir}/%{name}
%{_bindir}/dud-translate

%post
/sbin/ldconfig

%postun
/sbin/ldconfig

%changelog
* Sat Mar 20 2021 Nikolai Kondrashov <spbnick@gmail.com> - 1-1
- Release v1
