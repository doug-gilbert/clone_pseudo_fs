%define name    clone_pseudo_fs
%define version 0.90
%define release 1

Summary: 	Clone Linux pseudo file systems such as sysfs
Name: 		%{name}
Version: 	%{version}
Release: 	%{release}
License:	BSD-2-Clause
Group:		Utilities/System
Source0:	https://sg.danny.cz/scsi/%{name}-%{version}.tar.gz
Url:		https://sg.danny.cz/scsi/lsurl.html
BuildRoot:	%{_tmppath}/%{name}-%{version}-root/
Packager:	dgilbert at interlog dot com

%description
Getting a snapshot of the sysfs pseudo file system (e.g. under /sys )
is a challenge with existing tools. For example sysfs regular files
do not populate their 'struct stat' instance with their true file size.
There is a good reason as their file size is dynamic and only decided
when their contents is read(2). The amount of data cloned from each
regular file is 256 bytes by default but may be increased by a
command line option.

Author:
--------
    Doug Gilbert <dgilbert at interlog dot com>

%prep

%setup -q

%build
./autogen.sh
%configure

%install
if [ "$RPM_BUILD_ROOT" != "/" ]; then
        rm -rf $RPM_BUILD_ROOT
fi

make install \
        DESTDIR=$RPM_BUILD_ROOT

%clean
if [ "$RPM_BUILD_ROOT" != "/" ]; then
        rm -rf $RPM_BUILD_ROOT
fi

%files
%defattr(-,root,root)
%doc ChangeLog INSTALL README CREDITS AUTHORS COPYING
%attr(0755,root,root) %{_bindir}/*
%{_mandir}/man8/*


%changelog
* Wed Dec 13 2023 - dgilbert at interlog dot com
- initial version
  * clone_pseudo_fs-0.90
