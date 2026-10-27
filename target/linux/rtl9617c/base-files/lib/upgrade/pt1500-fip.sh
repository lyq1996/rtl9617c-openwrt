#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Youhua PT1500-H10 upgrade path.
#
# The system image is one uImage.FIT in the fip UBI volume; U-Boot reads that
# volume and boots configuration 9617c. An upgrade therefore rewrites fip only:
# ubi_Config (OEM calibration) and rootfs_data (the overlay) are never touched.

platform_check_image() {
	case "$(board_name)" in
	youhua,pt1500-h10) ;;
	*)
		echo "this image is for youhua,pt1500-h10, not $(board_name)" >&2
		return 1
		;;
	esac
	# sysupgrade gets the bare fip.itb artifact, not a sysupgrade tar.
	[ "$(hexdump -n 4 -v -e '4/1 "%02x"' "$1" 2>/dev/null)" = "d00dfeed" ] || {
		echo "image is not a uImage.FIT; use *-squashfs-fip.itb" >&2
		return 1
	}
	return 0
}

platform_do_upgrade() {
	local ubidev fip size leb need ebs avail

	ubidev=$(nand_find_ubi "$CI_UBIPART") || {
		echo "no UBI device on $CI_UBIPART" >&2
		return 1
	}
	fip=$(nand_find_volume "$ubidev" fip) || {
		echo "no fip volume; reinstall from U-Boot recovery" >&2
		return 1
	}
	size=$(wc -c < "$1")
	leb=$(cat "/sys/class/ubi/$fip/usable_eb_size")
	need=$(( (size + leb - 1) / leb ))
	ebs=$(cat "/sys/class/ubi/$fip/reserved_ebs")
	if [ "$need" -gt "$ebs" ]; then
		avail=$(cat "/sys/class/ubi/$ubidev/avail_eraseblocks")
		if [ "$((ebs + avail))" -lt "$need" ]; then
			echo "image needs $need LEBs, fip and free space hold $((ebs + avail))" >&2
			return 1
		fi
		echo "growing fip from $ebs to $need LEBs"
		ubirsvol "/dev/$ubidev" -N fip -s "$((need * leb))" || return 1
	fi
	echo "writing $size bytes ($need LEBs) to $fip"
	ubiupdatevol "/dev/$fip" "$1"
}

# The overlay already lives on rootfs_data, so nothing has to be carried into
# the new image; keep the user configuration in the volume it is already in.
platform_copy_config() {
	return 0
}
