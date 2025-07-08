#!/bin/ksh

# Exit immediately if a command exits with a non-zero status.
set -e

# Assign the first argument to DIR, or default to /usr/local if no argument is provided.
DIR=${1:-/usr/local}

# Check for essential commands. If they don't exist, exit.
# On OpenBSD, install and strip are usually in /usr/bin.
[ -e /usr/bin/install ] || { echo "Error: /usr/bin/install not found." ; exit 1; }
[ -e /usr/bin/strip ] || { echo "Error: /usr/bin/strip not found." ; exit 1; }

# Create necessary directories if they don't exist.
# -p creates parent directories as needed.
[ -d "${DIR}" ] || /bin/mkdir -p "${DIR}"
[ -d "${DIR}/sbin" ] || /bin/mkdir -p "${DIR}/sbin"
[ -d "${DIR}/man/man8" ] || /bin/mkdir -p "${DIR}/man/man8"

echo "Installing HAMMER2 binaries..."

# Install HAMMER2 binaries to ${DIR}/sbin
# -s: strip symbols (this is a common install flag, not the strip command)
# -m 555: set permissions to r-xr-xr-x (owner, group, others can execute)
/usr/bin/install -s -m 555 ./src/sbin/hammer2/hammer2 "${DIR}/sbin"
/usr/bin/install -s -m 555 ./src/sbin/newfs_hammer2/newfs_hammer2 "${DIR}/sbin"
/usr/bin/install -s -m 555 ./src/sbin/mount_hammer2/mount_hammer2 "${DIR}/sbin"
/usr/bin/install -s -m 555 ./src/sbin/fsck_hammer2/fsck_hammer2 "${DIR}/sbin"

echo "Installing HAMMER2 man pages..."

# Install HAMMER2 man pages to ${DIR}/man/man8
# -m 444: set permissions to r--r--r-- (read-only for all)
/usr/bin/install -m 444 ./src/sbin/hammer2/hammer2.8 "${DIR}/man/man8"
/usr/bin/install -m 444 ./src/sbin/newfs_hammer2/newfs_hammer2.8 "${DIR}/man/man8"
/usr/bin/install -m 444 ./src/sbin/mount_hammer2/mount_hammer2.8 "${DIR}/man/man8"
/usr/bin/install -m 444 ./src/sbin/fsck_hammer2/fsck_hammer2.8 "${DIR}/man/man8"

echo "Stripping symbols from binaries (if not already stripped by install -s)..."

# Strip debug symbols from the installed binaries.
# On OpenBSD, a plain 'strip' usually removes most symbols.
/usr/bin/strip "${DIR}/sbin/hammer2"
/usr/bin/strip "${DIR}/sbin/newfs_hammer2"
/usr/bin/strip "${DIR}/sbin/mount_hammer2"
/usr/bin/strip "${DIR}/sbin/fsck_hammer2"

echo "Install success"