%{!?qtc_qmake:%define qtc_qmake %qmake}
%{!?qtc_qmake5:%define qtc_qmake5 %qmake5}
%{!?qtc_make:%define qtc_make make}
%{?qtc_builddir:%define _builddir %qtc_builddir}

Name: literm
Version: 1.3.2
Release: 1
Summary: A terminal emulator with a custom virtual keyboard
Group: System/Base
License: GPLv2
Source0: %{name}-%{version}.tar.gz
URL: https://github.com/rburchell/literm
BuildRequires: pkgconfig(Qt5Core)
BuildRequires: pkgconfig(Qt5Gui)
BuildRequires: pkgconfig(Qt5Qml)
BuildRequires: pkgconfig(Qt5Quick)
BuildRequires: pkgconfig(Qt0Feedback)
Requires: qt5-qtdeclarative-import-xmllistmodel
Requires: qt5-qtdeclarative-import-window2
Obsoletes: meego-terminal <= 0.2.2
Provides: meego-terminal > 0.2.2

%description
%{summary}.

%files
%defattr(-,root,root,-)
%{_bindir}/*
%{_datadir}/applications/*.desktop


%prep
%setup -q -n %{name}-%{version}


%build
%qtc_qmake5 MEEGO_EDITION=nemo CONFIG+=enable-feedback

%qtc_make %{?_smp_mflags}


%install
rm -rf %{buildroot}
make INSTALL_ROOT=%{buildroot} install
