Name:	moxi
Version: 1.7.1_6_gd5076d9
Release:	1%{?dist}
Summary:	a memcached proxy with energy and pep
Group:		System Environment/Daemons
License:	BSD
URL:		http://northscale.com
Source0:	http://northscale.com/moxi/dist/%{name}-%{version}.tar.gz
BuildRoot:	%{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

BuildRequires:	automake
BuildRequires:	autoconf
BuildRequires:	libtool
BuildRequires:	openssl-devel
BuildRequires:  libevent-devel
BuildRequires:  pkgconfig
BuildRequires:  sqlite-devel
BuildRequires:  check-devel
Requires:       openssl
Requires:	libevent
Requires:       sqlite


%description
moxi is a memcached proxy with several optimizations to bring efficiency to
many memcached deployments, especially those with heavy workloads or
complex network topologies.  Optimizations include handling timeouts for
the client, deduplication of requests, a 'front' cache and protocol
(ascii to binary) conversion.  These optimizations keep the 'contract'
of the memcached protocol whole for clients.

%prep
%setup -q

%build
CONFLATE_DB_PATH=/var/lib/moxi %configure --disable-coverage --disable-debug --disable-shared --without-memcached --with-bundled-libstrophe --with-libconflate=bundled

make %{?_smp_mflags}


%install
rm -rf %{buildroot}
make install DESTDIR="%{buildroot}" AM_INSTALL_PROGRAM_FLAGS=""
# don't include libs and headers for conflate & strophe
rm -rf %{buildroot}/usr/lib
rm -rf %{buildroot}/usr/include

# Init script
sed -e 's/%%{version}/%{version}/g' < scripts/moxi-init.rhat.in > scripts/moxi-init.rhat
install -Dp -m0755 scripts/moxi-init.rhat %{buildroot}%{_initrddir}/moxi
# Default configs
mkdir -p %{buildroot}/%{_sysconfdir}/sysconfig
cat <<EOF >%{buildroot}/%{_sysconfdir}/sysconfig/%{name}
USER="nobody"
MAXCONN="1024"
CPROXY_ARG="/etc/moxi.conf"
OPTIONS=""
EOF

# pid directory
mkdir -p %{buildroot}/%{_localstatedir}/run/moxi
mkdir -p %{buildroot}/%{_localstatedir}/lib/moxi

%clean
rm -rf %{buildroot}

%files
%defattr(-,root,root,-)
%doc AUTHORS COPYING README doc/CONTRIBUTORS scripts/examples/
%doc /usr/share/man/man1/moxi.1.gz
%config(noreplace) %{_sysconfdir}/sysconfig/%{name}
%dir %attr(750,nobody,nobody) %{_localstatedir}/run/moxi
%dir %attr(750,nobody,nobody) %{_localstatedir}/lib/moxi
%{_bindir}/moxi
%{_initrddir}/moxi

%changelog
* Tue Jul 28 2009 Aliaksey Kandratsenka <alk@tut.by>
- packaged documentation and config-file examples
* Mon Jul 27 2009 Aliaksey Kandratsenka <alk@tut.by>
- startup script
* Fri Jul 17 2009 Matt Ingenthron <ingenthr@cep.net>
- Updated install locations and removed memcached dependency
* Fri Jul 17 2009 Aliaksey Kandratsenka <alk@tut.by>
- initial
