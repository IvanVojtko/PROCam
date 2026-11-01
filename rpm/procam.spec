# SPDX-FileCopyrightText: 2013 - 2024 Jolla Ltd.
# SPDX-FileCopyrightText: 2019 - 2020 Open Mobile Platform LLC.
# SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
#
# SPDX-License-Identifier: BSD-3-Clause

Name:       procam
Summary:    Computational photography camera for the Jolla Phone 2
Version:    1.4.0
Release:    1
License:    BSD-3-Clause AND GPL-2.0-or-later AND GPL-3.0-or-later
URL:        https://github.com/IvanVojtko/PROCam
Source0:    %{name}-%{version}.tar.bz2

# libprocamcamera2.so is an Android/Bionic library loaded through libhybris from
# /usr/libexec/droid-hybris. Its NDK dependencies are provided by the Android
# compatibility image, not by Sailfish RPM packages.
%global __requires_exclude_from ^.*/usr/libexec/droid-hybris/system/lib64/libprocamcamera2\.so$
%global __provides_exclude_from ^.*/usr/libexec/droid-hybris/system/lib64/libprocamcamera2\.so$
%global __requires_exclude ^(libandroid\.so.*|libcamera2ndk\.so.*|libmediandk\.so.*|libnativewindow\.so.*|liblog\.so.*|libdl_android\.so.*)$

BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Gui)
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  pkgconfig(Qt5Network)
BuildRequires:  pkgconfig(Qt5Multimedia)
BuildRequires:  pkgconfig(Qt5Test)
BuildRequires:  pkgconfig(Qt5Multimedia)
BuildRequires:  pkgconfig(qdeclarative5-boostable)
BuildRequires:  pkgconfig(libjpeg)
BuildRequires:  pkgconfig(libtiff-4)
BuildRequires:  pkgconfig(mlite5) >= 0.2.5
BuildRequires:  pkgconfig(systemsettings) >= 0.2.13
BuildRequires:  libtiff-devel
BuildRequires:  pkgconfig(libexif)
BuildRequires:  qt5-qttools
BuildRequires:  qt5-qttools-linguist
BuildRequires:  oneshot

Requires:  sailfishsilica-qt5 >= 1.1.79
Requires:  libtiff
Requires:  qt5-qtdeclarative-import-models2
Requires:  qt5-qtdeclarative-import-positioning
Requires:  qt5-qtdeclarative-import-multimedia
Requires:  qt5-qtdeclarative-import-sensors
Requires:  qt5-qtmultimedia-plugin-mediaservice-gstcamerabin >= 5.6.2+git25
Requires:  qt5-qtmultimedia-plugin-mediaservice-gstmediaplayer
Requires:  declarative-transferengine-qt5 >= 0.0.49
Requires:  nemo-qml-plugin-thumbnailer-qt5
Requires:  nemo-qml-plugin-dbus-qt5
Requires:  nemo-qml-plugin-policy-qt5
Requires:  nemo-qml-plugin-time-qt5
Requires:  nemo-qml-plugin-configuration-qt5
Requires:  nemo-qml-plugin-notifications-qt5 >= 1.1.2
Requires:  nemo-qml-plugin-systemsettings >= 0.5.21
Requires:  libkeepalive >= 1.7.0
Requires:  sailfish-components-media-qt5 >= 0.0.18
Requires:  sailfish-components-gallery-qt5 >= 1.1.10
Requires:  sailfish-policy >= 0.2.59
Requires:  jolla-settings-system >= 1.0.70
Requires:  qml(Nemo.Ngf)
Requires:  qr-filter-qml-plugin
Requires:  sailfish-content-graphics >= 1.2.2
Requires:  gstreamer1.0-plugins-good
Requires:  gstreamer1.0-plugins-bad
Requires:  dconf
Requires:  sailjail-launch-approval
Requires:  mapplauncherd-booster-silica-qt5-media

%{_oneshot_requires_post}

%description
ProCam is a camera app for the Jolla Phone 2 with computational photography:
merged RAW bursts (HDR, night mode), multi-frame super-resolution and sharp
2x zoom, rendered on the phone by its own RAW pipeline.

%prep
%setup -q -n %{name}-%{version}

%build

%qmake5 procam.pro
%make_build

%install
%qmake5_install

%post
%{_bindir}/add-oneshot dconf-update || :

%files
%license LICENSES/BSD-3-Clause.txt
%license licenses/RawBracket-MIT.txt
%{_datadir}/applications/procam.desktop
%{_datadir}/icons/hicolor/500x500/apps/procam-sfos.png
%dir %{_datadir}/procam
%{_datadir}/procam/camera.qml
%{_datadir}/procam/pages
%{_datadir}/procam/cover
%{_bindir}/procam
%{_datadir}/dbus-1/services/com.procam.camera.service
%dir %{_datadir}/jolla-settings
%dir %{_datadir}/jolla-settings/pages
%dir %{_datadir}/jolla-settings/pages/procam
%{_datadir}/jolla-settings/pages/procam/SettingsPage.qml
%dir %{_datadir}/jolla-settings/entries
%{_datadir}/jolla-settings/entries/procam.json
%{_libdir}/qt5/qml/com/procam/camera
%dir %{_libexecdir}/procam
%{_libexecdir}/procam/sfos-camera2-probe
%{_libexecdir}/procam/sfos-raw16-to-jpeg
%dir %{_libexecdir}/droid-hybris
%dir %{_libexecdir}/droid-hybris/system
%dir %{_libexecdir}/droid-hybris/system/lib64
%{_libexecdir}/droid-hybris/system/lib64/libprocamcamera2.so
%{_sysconfdir}/dconf/db/vendor.d/00-procam.txt

%changelog
* Sun Nov 01 2026 Ivan Vojtko <ivan.vojtko@gmail.com> - 1.4.0.beta.2-1
- Installs side by side with RAWfish: own QML module (com.procam.camera) and
  Camera2 bridge library (libprocamcamera2.so); the conflict is gone.
- Faces in hard sun keep their colour: the lit side of a face is checked
  after tone mapping and the shadow lift reduced until it no longer clips.
- Gentler saturation on skin, and in selfies less clarity and sharpening on
  skin, so pores and stubble look natural.
- The gallery thumbnail shows the newest photo once processing finishes.
* Sun Nov 01 2026 Ivan Vojtko <ivan.vojtko@gmail.com> - 1.4.0-1
- First ProCam release. RAW-based rendering for every photo: HDR bursts with
  motion-aware exposure, night mode (16 frames), super-resolution and sharp 2x
  zoom, face-aware exposure and tone mapping, refined tone (black and white
  points, clarity), neutral white balance on the front and ultra-wide cameras,
  4:3 or 16:9, RAW/DNG in Normal mode, GPU viewfinder with YUV preview frames.
* Sat Oct 31 2026 Ivan Vojtko <ivan.vojtko@gmail.com> - 1.3.1-9
- Fork of RAWfish renamed to ProCam: enhanced RAW renderer, HDR bursts,
  super-resolution and sharp 2x zoom, fullscreen viewfinder.
- Night mode, YUV bursts for the front and ultra-wide cameras, 16:9 photos,
  RAW files in Normal mode, GPU-drawn viewfinder.
- GitHub CI (host tests, RPM) and tag-based releases.
* Thu Sep 24 2026 RAWfish Contributors <rawfish@users.noreply.github.com> - 1.3.1-3
- Package RAWfish release 3.
