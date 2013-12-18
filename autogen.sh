# Bootstrap script for EosMetrics
# Run this script on a clean source checkout to get ready for building.

FILE_MUST_EXIST=eosmetrics/eosmetrics.h

test -n "$srcdir" || srcdir=`dirname "$0"`
test -n "$srcdir" || srcdir=.
olddir=`pwd`

cd $srcdir
test -f $FILE_MUST_EXIST || {
    echo "You must run this script in the top-level checkout directory"
    exit 1
}

# Install our commit message script if a git repo
if [ -d .git ]; then
    cp commit-msg .git/hooks/commit-msg
    chmod +x .git/hooks/commit-msg
fi

# NOCONFIGURE is used by gnome-common
if test -z "$NOCONFIGURE"; then
    echo "This script will run ./configure automatically. If you wish to pass "
    echo "any arguments to it, please specify them on the $0 "
    echo "command line. To disable this behavior, have NOCONFIGURE=1 in your "
    echo "environment."
fi

# Run the actual tools to prepare the clean checkout
gtkdocize || exit $?
autoreconf -fi || exit $?

cd "$olddir"
test -n "$NOCONFIGURE" || "./configure" "$@"
