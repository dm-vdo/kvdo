%define spec_release 1
%define kmod_name		kvdo
%define kmod_driver_version	8.0.0.0
%define kmod_rpm_release	%{spec_release}
%define kmod_kernel_version	3.10.0-693.el7

# Disable the scanning for a debug package
%global debug_package %{nil}

Source0:        kmod-%{kmod_name}-%{kmod_driver_version}.tgz

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
/usr/sbin/dkms --rpm_safe_upgrade add -m %{kmod_name} -v %{version}-%{kmod_driver_version}
/usr/sbin/dkms --rpm_safe_upgrade build -m %{kmod_name} -v %{version}-%{kmod_driver_version}
/usr/sbin/dkms --rpm_safe_upgrade install -m %{kmod_name} -v %{version}-%{kmod_driver_version}

%preun
# Check whether kvdo or uds is loaded, and if so attempt to remove it.  A
# failure here means there is still something using the module, which should be
# cleared up before attempting to remove again.
for module in kvdo uds; do
  if grep -q "^${module}" /proc/modules; then
    modprobe -r ${module}
  fi
done
/usr/sbin/dkms --rpm_safe_upgrade remove -m %{kmod_name} -v %{version}-%{kmod_driver_version} --all || :

%prep
%setup -n kmod-%{kmod_name}-%{kmod_driver_version}

%build
# Nothing doing here, as we're going to build on whatever kernel we end up
# running inside.

%install
mkdir -p $RPM_BUILD_ROOT/%{_usr}/src/%{kmod_name}-%{version}-%{kmod_driver_version}
cp -r * $RPM_BUILD_ROOT/%{_usr}/src/%{kmod_name}-%{version}-%{kmod_driver_version}/
cat > $RPM_BUILD_ROOT/%{_usr}/src/%{kmod_name}-%{version}-%{kmod_driver_version}/dkms.conf <<EOF
PACKAGE_NAME="kvdo"
PACKAGE_VERSION="%{version}-%{kmod_driver_version}"
AUTOINSTALL="yes"

BUILT_MODULE_NAME[0]="uds"
BUILT_MODULE_LOCATION[0]="uds"
DEST_MODULE_LOCATION[0]="/kernel/drivers/block/"
STRIP[0]="no"

BUILT_MODULE_NAME[1]="kvdo"
BUILT_MODULE_LOCATION[1]="vdo"
DEST_MODULE_LOCATION[1]="/kernel/drivers/block/"
STRIP[1]="no"
EOF

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(644,root,root,755)
%{_usr}/src/%{kmod_name}-%{version}-%{kmod_driver_version}/*

%changelog
* Thu Sep 12 2019 - corwin@bf30-1 - 8.0.0.0-1
