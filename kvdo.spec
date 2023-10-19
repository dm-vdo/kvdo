%define spec_release 1
%define kmod_name		kvdo
%define kmod_driver_version	8.2.3.3
%define kmod_rpm_release	%{spec_release}
%define kmod_kernel_version	3.10.0-693.el7

# Disable the scanning for a debug package
%global debug_package %{nil}

Source0:        %{kmod_name}-%{kmod_driver_version}.tgz

Name:		kmod-kvdo
Version:	%{kmod_driver_version}
Release:	%{kmod_rpm_release}%{?dist}
Summary:	Kernel Modules for Virtual Data Optimizer
License:	GPLv2+
URL:		http://github.com/dm-vdo/kvdo
BuildRoot:	%(mktemp -ud %{_tmppath}/%{name}-%{version}-%{release}-XXXXXX)
Requires:       dkms
Requires:	kernel-devel >= %{kmod_kernel_version}
Requires:       make
%if 0%{?fedora}
# Fedora requires elfutils-libelf-devel, while rhel does not.
BuildRequires:  elfutils-libelf-devel
%endif
BuildRequires:	glibc
%if 0%{?rhel} && 0%{?rhel} < 9
# Fedora doesn't have abi whitelists,
# And RHEL9 doesn't have it yet.
BuildRequires:  kernel-abi-whitelists
%endif
BuildRequires:  libuuid-devel
BuildRequires:  redhat-rpm-config
ExclusiveArch:	x86_64
ExcludeArch:    s390
ExcludeArch:    s390x
ExcludeArch:    ppc
ExcludeArch:    ppc64
ExcludeArch:    ppc64le
ExcludeArch:    aarch64
ExcludeArch:    i686

%description
Virtual Data Optimizer (VDO) is a device mapper target that delivers
block-level deduplication, compression, and thin provisioning.

This package provides the kernel modules for VDO.

%post
set -x
/usr/sbin/dkms --rpm_safe_upgrade add -m %{kmod_name} -v %{version}
/usr/sbin/dkms --rpm_safe_upgrade build -m %{kmod_name} -v %{version}
/usr/sbin/dkms --rpm_safe_upgrade install -m %{kmod_name} -v %{version}

%preun
# Check whether kvdo is loaded, and if so attempt to remove it.  A
# failure here means there is still something using the module, which
# should be cleared up before attempting to remove again.
for module in kvdo uds; do
  if grep -q "^${module}" /proc/modules; then
    modprobe -r ${module}
  fi
done
/usr/sbin/dkms --rpm_safe_upgrade remove -m %{kmod_name} -v %{version} --all || :

%prep
%setup -n %{kmod_name}-%{kmod_driver_version}

%build
# Nothing doing here, as we're going to build on whatever kernel we end up
# running inside.

%install
mkdir -p $RPM_BUILD_ROOT/%{_usr}/src/%{kmod_name}-%{version}
cp -r * $RPM_BUILD_ROOT/%{_usr}/src/%{kmod_name}-%{version}/
cat > $RPM_BUILD_ROOT/%{_usr}/src/%{kmod_name}-%{version}/dkms.conf <<EOF
PACKAGE_NAME="kvdo"
PACKAGE_VERSION="%{version}"
AUTOINSTALL="yes"

BUILT_MODULE_NAME[0]="kvdo"
BUILT_MODULE_LOCATION[0]="vdo"
DEST_MODULE_LOCATION[0]="/kernel/drivers/block/"
BUILD_DEPENDS[0]=LZ4_COMPRESS
BUILD_DEPENDS[0]=LZ4_DECOMPRESS
STRIP[0]="no"
EOF

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(644,root,root,755)
%{_usr}/src/%{kmod_name}-%{version}

%changelog
* Fri Oct 13 2023 - Red Hat VDO Team <vdo-devel@redhat.com> - 8.2.3.3-1
- See https://github.com/dm-vdo/kvdo.git
