#!/bin/sh
# Provision the PT1500 RTLFACT1 factory volume from the known unit identity.
# Usage: provision-factory.sh <device-ip> [base-mac] [pon-mac] [serial] [mode]
set -eu

ip=${1:?usage: $0 <device-ip> [base-mac] [pon-mac] [serial] [mode]}
base_mac=${2:-fc:34:e2:77:5c:62}
pon_mac=${3:-fc:34:e2:77:5c:63}
serial=${4:-59485443E2775C62}
mode=${5:-xgpon}

here=$(dirname "$0")
image=$(mktemp)
trap 'rm -f "$image"' EXIT

python3 "$here/mk-factory.py" \
	--base-mac "$base_mac" --pon-mac "$pon_mac" \
	--serial "$serial" --mode "$mode" --output "$image"
scp -O -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
	"$image" "root@$ip:/tmp/factory.bin"
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "root@$ip" \
	'ubiupdatevol /dev/ubi0_0 /tmp/factory.bin && sync'
echo "factory written to $ip; reboot and check /var/run/tyfza1-factory/pon_factory"
