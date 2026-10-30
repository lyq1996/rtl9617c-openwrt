#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
REQUIRE_IMAGE_METADATA=1
RAMFS_COPY_BIN='fw_printenv fw_setenv'

platform_check_image() {
	local board="$(board_name)"
	local magic="$(get_magic_long "$1")"

	[ "$magic" = "d00dfeed" ] || { echo "Invalid image type."; return 1; }

	case "$board" in
	tianyi,tyfza1-2p5g|tianyi,tyfza1-4ge|youhua,pt1500-h10-2p5g) ;;
	*) echo "Unsupported board: $board" >&2; return 1 ;;
	esac
	return 0
}

platform_do_upgrade() {
	CI_KERNPART=fit
	CI_UBIPART=ubi_device
	nand_do_upgrade "$1"
}
