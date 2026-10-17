#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Upgrade for the UniFi Cloud Gateway Fiber.
#
# The "kernel" and "root" GPT partitions each hold an ext4 filesystem rather
# than a raw image. The kernel partition holds uImage / uImage.bkp and the
# root partition holds rootfs / rootfs.bkp. U-Boot (bootdebbox) and the
# initramfs /init read those files, so the sysupgrade tar members are
# installed as files. A raw dd to either partition destroys the filesystem
# and the unit no longer boots.

UCGFIBER_MNT="${UCGFIBER_MNT:-/tmp}"

ucgfiber_gz() { # <image>
	[ "$(dd if="$1" bs=2 count=1 2>/dev/null | hexdump -v -e '2/1 "%02x"')" = 1f8b ] && echo z
}

ucgfiber_member() { # <image> <member>   writes the member to stdout
	tar x$(ucgfiber_gz "$1")f "$1" "$2" -O
}

ucgfiber_board_dir() { # <image>
	local dir

	dir="$(tar t$(ucgfiber_gz "$1")f "$1" 2>/dev/null |
		grep -m 1 '^sysupgrade-.*/$')" || return 1
	echo "${dir%/}"
}

ucgfiber_check_image() { # <image>
	local img="$1" dir magic

	dir="$(ucgfiber_board_dir "$img")" || {
		echo "ucgfiber: no sysupgrade-* directory in image" >&2
		return 1
	}
	[ "$dir" = sysupgrade-ubnt_ucgfiber ] || {
		echo "ucgfiber: image is for $dir" >&2
		return 1
	}

	# kernel: flattened device tree (FIT); root: squashfs
	magic="$(ucgfiber_member "$img" "$dir/kernel" |
		hexdump -v -n 4 -e '4/1 "%02x"')"
	[ "$magic" = d00dfeed ] || {
		echo "ucgfiber: kernel is not a FIT image" >&2
		return 1
	}
	magic="$(ucgfiber_member "$img" "$dir/root" |
		hexdump -v -n 4 -e '4/1 "%02x"')"
	[ "$magic" = 68737173 ] || {
		echo "ucgfiber: root is not a squashfs image" >&2
		return 1
	}
	return 0
}

# Unmount every mount of a block device. Fails if it stays busy.
ucgfiber_umount_dev() { # <dev>
	local dev="$1" mp n=0

	while grep -q "^$dev " /proc/mounts; do
		[ "$n" -lt 5 ] || return 1
		mp="$(grep -m 1 "^$dev " /proc/mounts | cut -d' ' -f2)"
		umount "$mp" 2>/dev/null || sleep 1
		n=$((n + 1))
	done
	return 0
}

ucgfiber_mount() { # <dev> <dir>
	local n=0

	mkdir -p "$2" || return 1
	# a lazily detached mount of the same device can still be draining
	while ! mount -t ext4 -o rw,noatime "$1" "$2" 2>/dev/null; do
		n=$((n + 1))
		[ "$n" -lt 5 ] || {
			mount -t ext4 -o rw,noatime "$1" "$2"
			return
		}
		sleep 1
	done
}

# Write a tar member to <mountpoint>/<name>.new and verify it from disk.
ucgfiber_stage() { # <image> <member> <mountpoint> <name>
	local file="$3/$4.new"

	rm -f "$file"
	ucgfiber_member "$1" "$2" >"$file" || return 1
	sync
	{ echo 3 >/proc/sys/vm/drop_caches; } 2>/dev/null
	ucgfiber_member "$1" "$2" | cmp -s - "$file"
}

# Keep the current file as <name>.bkp, then replace it atomically. The hard
# link plus rename means a valid <name> exists at every point.
ucgfiber_swap() { # <mountpoint> <name>
	if [ -e "$1/$2" ]; then
		ln -f "$1/$2" "$1/$2.bkp" || return 1
	fi
	mv -f "$1/$2.new" "$1/$2"
}

ucgfiber_do_upgrade() { # <image>
	local img="$1" dir kdev rdev kmnt rmnt rc=1

	kdev="${EMMC_KERN_DEV:-$(find_mmc_part kernel)}"
	rdev="${EMMC_ROOT_DEV:-$(find_mmc_part root)}"
	[ -b "$kdev" ] && [ -b "$rdev" ] || {
		echo "ucgfiber: kernel/root partitions not found" >&2
		return 1
	}
	dir="$(ucgfiber_board_dir "$img")" || return 1
	kmnt="$UCGFIBER_MNT/ucgfiber-kernel"
	rmnt="$UCGFIBER_MNT/ucgfiber-root"

	# Nothing is written until both new files are staged and verified.
	if ucgfiber_umount_dev "$kdev" && ucgfiber_umount_dev "$rdev" &&
	   ucgfiber_mount "$kdev" "$kmnt" && ucgfiber_mount "$rdev" "$rmnt" &&
	   ucgfiber_stage "$img" "$dir/kernel" "$kmnt" uImage &&
	   ucgfiber_stage "$img" "$dir/root" "$rmnt" rootfs &&
	   ucgfiber_swap "$kmnt" uImage &&
	   ucgfiber_swap "$rmnt" rootfs; then
		rc=0
	else
		echo "ucgfiber: upgrade failed, installed files left unchanged" >&2
		rm -f "$kmnt/uImage.new" "$rmnt/rootfs.new"
	fi
	sync
	umount "$kmnt" "$rmnt" 2>/dev/null
	return $rc
}
