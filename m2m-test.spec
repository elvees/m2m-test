Name:           m2m-test
Version:        1.3
Release:        alt1

Summary:        Tools to test V4L2 M2M devices
License:        GPLv2
Group:          Video

ExclusiveArch:  armh

Url:            http://multicore.ru
Source:         %name-%version.tar

BuildRequires(pre): rpm-macros-cmake

BuildRequires: cmake
BuildRequires: ffmpeg
BuildRequires: pkgconfig(libavformat) >= 57.40
BuildRequires: pkgconfig(libavdevice)
BuildRequires: pkgconfig(libavcodec) >= 57.48
BuildRequires: pkgconfig(libswscale)
BuildRequires: libswresample-devel
BuildRequires: pkgconfig(libavutil)
BuildRequires: pkgconfig(libavfilter)
BuildRequires: pkgconfig(libavresample)
BuildRequires: pkgconfig(libpostproc)
BuildRequires: pkgconfig(libdrm)
BuildRequires: pkgconfig(libkms)


%description
%summary

%prep
%setup

%build
%cmake
%cmake_build

%install
%cmakeinstall_std

%files
%_bindir/*

%changelog
* Thu Apr 26 2018 Andrey Solodovnikov <hepoh@altlinux.org> 1.3-alt1
Initial build for ALT