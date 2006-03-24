#!/bin/sh

rm -f debug.log child.log

ulimit -c unlimited

unset LANG
unset LC_MESSAGES

# Enable glibc heap consistency checks, and memory randomization.
MALLOC_CHECK_=2
MALLOC_PERTURB_=`expr $RANDOM % 255 2>/dev/null`
export MALLOC_CHECK_ MALLOC_PERTURB_

for f in $*; do
    if ${HARNESS} ./$f ${SRCDIR}; then
	:
    else
	echo FAILURE
	[ -z "$CARRYON" ] && exit 1
    fi
done

exit 0
