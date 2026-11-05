# Sourced by the scripts that talk to the phone, after they set ROOT. adb and fastboot honour
# ANDROID_SERIAL, so this pins them to one phone. It stops rather than reach whatever happens to be
# plugged in, as other devices may be on the host. The serial isn't kept in this repository: set
# ANDROID_SERIAL, or put it in build/serial, which git ignores.
ANDROID_SERIAL=${ANDROID_SERIAL:-$(cat "$ROOT/build/serial" 2>/dev/null || true)}
if [[ -z $ANDROID_SERIAL ]]; then
	echo "$(basename "$0"): set ANDROID_SERIAL, or put the phone's serial in build/serial" >&2
	exit 1
fi
export ANDROID_SERIAL
