# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Paige Thompson

. $(atf_get_srcdir)/../../sys/common/vnet.subr

atf_test_case "libxo_output_consistency" "cleanup"
libxo_output_consistency_head()
{
	atf_set descr "ifconfig output must be identical with and without libxo"
	atf_set require.user root
}

# Run the same command with both binaries and compare stdout and stderr.
libxo_compare_cmd()
{
	local plain_out plain_err xo_out xo_err

	plain_out=$(mktemp)
	plain_err=$(mktemp)
	xo_out=$(mktemp)
	xo_err=$(mktemp)

	atf_check -s exit:0 -o save:${plain_out} -e save:${plain_err} \
	    ${ifc_nolibxo} "$@"
	atf_check -s exit:0 -o save:${xo_out} -e save:${xo_err} \
	    ${ifc_libxo} "$@"

	atf_check -s exit:0 diff -u ${plain_out} ${xo_out}
	atf_check -s exit:0 diff -u ${plain_err} ${xo_err}

	rm -f ${plain_out} ${plain_err} ${xo_out} ${xo_err}
}

libxo_output_consistency_body()
{
	local iftype iface

	vnet_init

	ifc_libxo=$(atf_get_srcdir)/with-libxo/ifconfig
	ifc_nolibxo=$(atf_get_srcdir)/without-libxo/ifconfig

	# Create the interfaces used to exercise the bridge, lagg and vlan
	# printing paths with explicit high unit numbers.
	${ifc_nolibxo} epair254 create >/dev/null 2>&1 || true
	${ifc_nolibxo} bridge254 create >/dev/null 2>&1 || true
	${ifc_nolibxo} lagg254 create >/dev/null 2>&1 || true

	# Wire them up.
	${ifc_nolibxo} epair254a up >/dev/null 2>&1 || true
	${ifc_nolibxo} epair254b up >/dev/null 2>&1 || true
	${ifc_nolibxo} bridge254 addm epair254a >/dev/null 2>&1 || true
	${ifc_nolibxo} lagg254 laggproto failover laggport epair254b \
	    >/dev/null 2>&1 || true
	${ifc_nolibxo} vlan254 create vlan 42 vlandev epair254a \
	    >/dev/null 2>&1 || true

	# Create every cloneable interface type the kernel supports, skipping
	# the types created above and any that cannot be created (e.g. unloaded
	# modules).
	for iftype in $(${ifc_nolibxo} -C); do
		case ${iftype} in
		epair|bridge|lagg|vlan)
			continue
			;;
		esac
		${ifc_nolibxo} ${iftype} create >/dev/null 2>&1 || true
	done

	# Full listing, both summary and verbose.
	libxo_compare_cmd
	libxo_compare_cmd -v
	libxo_compare_cmd -vv
	libxo_compare_cmd -m

	# Per-interface verbose output.
	for iface in $(${ifc_nolibxo} -l); do
		libxo_compare_cmd -vv ${iface}
	done
}
libxo_output_consistency_cleanup()
{
	vnet_cleanup
}

atf_init_test_cases()
{
	atf_add_test_case libxo_output_consistency
}
