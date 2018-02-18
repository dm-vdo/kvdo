%define spec_release 1

%define kmod_name		kvdo
%define kmod_driver_version	6.1.1.12
%define kmod_rpm_release	%{spec_release}
%define kmod_kernel_version	3.10.0-693.el7
%define kmod_headers_version	%(rpm -qa kernel-devel | sed 's/^kernel-devel-//')
%define kmod_kbuild_dir		.
%define kmod_dependencies       %{nil}
%define kmod_build_dependencies	%{nil}
%define kmod_devel_package	0

%{!?dist: %define dist .el7_4}

%if 0%{?fedora}
Source0:	kmod-%{kmod_name}-%{kmod_driver_version}.tgz
%else
Source0:	%{kmod_name}-%{kmod_driver_version}.tgz
%endif
%{nil}

%define findpat %( echo "%""P" )
%define __find_requires /usr/lib/rpm/redhat/find-requires.ksyms
%if 0%{?rhel}
# Fedora has deprecated this.
%define __find_provides /usr/lib/rpm/redhat/find-provides.ksyms %{kmod_name} %{?epoch:%{epoch}:}%{version}-%{release}
%endif
%define sbindir %( if [ -d "/sbin" -a \! -h "/sbin" ]; then echo "/sbin"; else echo %{_sbindir}; fi )

Name:		kmod-kvdo
Version:	%{kmod_driver_version}
Release:	%{kmod_rpm_release}%{?dist}
Summary:	Kernel Modules for Virtual Data Optimizer
License:	GPLv2+
URL:		http://github.com/dm-vdo/kvdo
BuildRoot:	%(mktemp -ud %{_tmppath}/%{name}-%{version}-%{release}-XXXXXX)
%if 0%{?fedora}
# Fedora requires elfutils-libelf-devel, while rhel does not.
BuildRequires:  elfutils-libelf-devel
%endif
BuildRequires:	glibc
%if 0%{?rhel}
# Fedora doesn't have abi whitelists.
BuildRequires:	kernel-abi-whitelists
%endif
BuildRequires:	kernel-devel >= %{kmod_kernel_version}
BuildRequires:  kernel-debug >= %{kmod_kernel_version}
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
%global kernel_source() /usr/src/kernels/%{kmod_headers_version}

%global _use_internal_dependency_generator 0
Provides:	kernel-modules = %{kmod_kernel_version}.%{_target_cpu}
Provides:	kmod-%{kmod_name} = %{?epoch:%{epoch}:}%{version}-%{release}
Requires(post):	%{sbindir}/weak-modules
Requires(postun):	%{sbindir}/weak-modules
Requires:	kernel >= %{kmod_kernel_version}
%if 0
Requires: firmware(%{kmod_name}) = ENTER_FIRMWARE_VERSION
%endif
%if "%{kmod_build_dependencies}" != ""
BuildRequires:  %{kmod_build_dependencies}
%endif
%if "%{kmod_dependencies}" != ""
Requires:       %{kmod_dependencies}
%endif
# if there are multiple kmods for the same driver from different vendors,
# they should conflict with each other.
Conflicts:	kmod-%{kmod_name}

%description
Virtual Data Optimizer (VDO) is a device mapper target that delivers
block-level deduplication, compression, and thin provisioning.

This package provides the kernel modules for VDO.

%if 0

%package -n kmod-kvdo-firmware
Version:	ENTER_FIRMWARE_VERSION
Summary:	Kernel Modules for Virtual Data Optimizer
Provides:	firmware(%{kmod_name}) = ENTER_FIRMWARE_VERSION
Provides:	kernel-modules = %{kmod_kernel_version}.%{_target_cpu}
%description -n  kmod-kvdo-firmware
Virtual Data Optimizer (VDO) is a device mapper target that delivers
block-level deduplication, compression, and thin provisioning.

This package provides the firmware for VDO.

%files -n kmod-kvdo-firmware
%defattr(644,root,root,755)
%{FIRMWARE_FILES}

%endif

# Development package
%if 0%{kmod_devel_package}
%package -n kmod-kvdo-devel
Version:	%{kmod_driver_version}
Requires:	kernel >= %{kmod_kernel_version}
Summary:	Kernel Modules for Virtual Data Optimizer

%description -n  kmod-kvdo-devel
Virtual Data Optimizer (VDO) is a device mapper target that delivers
block-level deduplication, compression, and thin provisioning.

This package provides the development files for VDO.

%files -n kmod-kvdo-devel
%defattr(644,root,root,755)
/usr/share/kmod-%{kmod_name}/Module.symvers
%endif

%post
modules=( $(find /lib/modules/%{kmod_headers_version}/extra/kmod-%{kmod_name} | grep '\.ko$') )
printf '%s\n' "${modules[@]}" >> /var/lib/rpm-kmod-posttrans-weak-modules-add

%pretrans -p <lua>
posix.unlink("/var/lib/rpm-kmod-posttrans-weak-modules-add")

%posttrans
if [ -f "/var/lib/rpm-kmod-posttrans-weak-modules-add" ]; then
	modules=( $(cat /var/lib/rpm-kmod-posttrans-weak-modules-add) )
	rm -rf /var/lib/rpm-kmod-posttrans-weak-modules-add
	printf '%s\n' "${modules[@]}" | %{sbindir}/weak-modules --add-modules
fi

%preun
rpm -ql kmod-kvdo-%{kmod_driver_version}-%{kmod_rpm_release}%{?dist}.$(arch) | grep '\.ko$' > /var/run/rpm-kmod-%{kmod_name}-modules
modprobe -r kvdo
modprobe -r uds

%postun
modules=( $(cat /var/run/rpm-kmod-%{kmod_name}-modules) )
rm /var/run/rpm-kmod-%{kmod_name}-modules
printf '%s\n' "${modules[@]}" | %{sbindir}/weak-modules --remove-modules

%files
%defattr(644,root,root,755)
/lib/modules/%{kmod_headers_version}
/etc/depmod.d/%{kmod_name}.conf
/usr/share/doc/kmod-%{kmod_name}/greylist.txt

%prep
%if 0%{?fedora}
%setup -n kmod-%{kmod_name}-%{kmod_driver_version}
%else
%setup -n %{kmod_name}-%{kmod_driver_version}
%endif

%{nil}
set -- *
mkdir source
mv "$@" source/
mkdir obj

%build
rm -rf obj
cp -r source obj
make -C %{kernel_source} M=$PWD/obj/%{kmod_kbuild_dir} V=1 \
	NOSTDINC_FLAGS="-I $PWD/obj/include -I $PWD/obj/include/uapi"
# mark modules executable so that strip-to-file can strip them
find obj/%{kmod_kbuild_dir} -name "*.ko" -type f -exec chmod u+x '{}' +

whitelist="/lib/modules/kabi-current/kabi_whitelist_%{_target_cpu}"
for modules in $( find obj/%{kmod_kbuild_dir} -name "*.ko" -type f -printf "%{findpat}\n" | sed 's|\.ko$||' | sort -u ) ; do
	# update depmod.conf
	module_weak_path=$(echo $modules | sed 's/[\/]*[^\/]*$//')
	if [ -z "$module_weak_path" ]; then
		module_weak_path=%{name}
	else
		module_weak_path=%{name}/$module_weak_path
	fi
	echo "override $(echo $modules | sed 's/.*\///') $(echo %{kmod_headers_version} | sed 's/\.[^\.]*$//').* weak-updates/$module_weak_path" >> source/depmod.conf

	# update greylist
	nm -u obj/%{kmod_kbuild_dir}/$modules.ko | sed 's/.*U //' |  sed 's/^\.//' | sort -u | while read -r symbol; do
		grep -q "^\s*$symbol\$" $whitelist || echo "$symbol" >> source/greylist
	done
done
sort -u source/greylist | uniq > source/greylist.txt

%install
export INSTALL_MOD_PATH=$RPM_BUILD_ROOT
export INSTALL_MOD_DIR=extra/%{name}
make -C %{kernel_source} modules_install V=1 \
	M=$PWD/obj/%{kmod_kbuild_dir}
# Cleanup unnecessary kernel-generated module dependency files.
find $INSTALL_MOD_PATH/lib/modules -iname 'modules.*' -exec rm {} \;

install -m 644 -D source/depmod.conf $RPM_BUILD_ROOT/etc/depmod.d/%{kmod_name}.conf
install -m 644 -D source/greylist.txt $RPM_BUILD_ROOT/usr/share/doc/kmod-%{kmod_name}/greylist.txt
%if 0
%{FIRMWARE_FILES_INSTALL}
%endif
%if 0%{kmod_devel_package}
install -m 644 -D $PWD/obj/%{kmod_kbuild_dir}/Module.symvers $RPM_BUILD_ROOT/usr/share/kmod-%{kmod_name}/Module.symvers
%endif

%clean
rm -rf $RPM_BUILD_ROOT

%changelog
* Sat Feb 17 2018 - J. corwin Coburn <corwin@redhat.com> - 6.1.1.12-1
- Added support for 4.15 kernels.
- Modified spec files to support building on more distros.
- Removed unused code from the UDS module.
