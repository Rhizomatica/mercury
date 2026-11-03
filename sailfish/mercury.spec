# Sailfish OS package of the standalone modem, built on the phone itself
# (rpmbuild, no Sailfish SDK).  Build it from a release's source archive:
# GitHub writes that release's commit into %{commit} (export-subst in
# .gitattributes), and release.yml sets Version.  See README.md, "Install on
# Sailfish OS".
%global commit $Format:%h$
Name:           mercury
Version:        1.9.22
Release:        1
Summary:        HF modem for store-and-forward communication
License:        GPL-3.0-or-later
URL:            https://github.com/Rhizomatica/mercury
# The release tag's tree, as GitHub archives it
Source0:        mercury-%{version}.tar.gz
BuildRequires:  hamlib-devel
BuildRequires:  alsa-lib-devel
BuildRequires:  pulseaudio-devel
BuildRequires:  openssl-devel

%description
Mercury is the modem of HERMES: ARQ and broadcast data links over HF
radio, with a VARA-compatible TCP TNC interface. This is the standalone
modem, with its own audio and its own radio control through Hamlib; radiod
has mercury built in and does not need it. No GUI.

%prep
%setup -q -n mercury-%{version}

%build
make %{?_smp_mflags} MERCURY_HOST=standalone GIT_HASH=%{commit}

%install
make install DESTDIR=%{buildroot} prefix=%{_prefix} MERCURY_HOST=standalone \
     GIT_HASH=%{commit}

%check
make test MERCURY_HOST=standalone GIT_HASH=%{commit}

%files
%license LICENSE
%{_bindir}/mercury
%{_mandir}/man1/mercury.1*
%doc %{_docdir}/mercury/mercury.ini.example
