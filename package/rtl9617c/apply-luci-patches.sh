#!/bin/sh
# Apply the LuCI feed patches after "scripts/feeds update luci".
# Re-running is safe: patches that no longer apply are reported and skipped.
set -e
top=${1:-$(git -C "$(dirname "$0")" rev-parse --show-toplevel)}
luci="$top/feeds/luci"

[ -d "$luci" ] || {
	echo "missing $luci; run scripts/feeds update luci first" >&2
	exit 1
}

for p in "$(dirname "$0")"/luci-app-rtl9617c-pon/feed-patches/*.patch; do
	printf "%-45s " "$(basename "$p")"
	if git -C "$luci" apply --check "$p" 2>/dev/null; then
		git -C "$luci" apply "$p"
		echo applied
	else
		echo skipped
	fi
done
