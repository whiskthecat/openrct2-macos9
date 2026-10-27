#!/usr/bin/env bash
# Compiler launcher for the Mac OS 9 build: compile, then rewrite weak commons
# in the object file (see tools/xcoff_weakfix.py).
"$@" || exit $?
out=""
while [ $# -gt 0 ]; do
  if [ "$1" = "-o" ]; then out="$2"; fi
  shift
done
case "$out" in
  *.obj|*.o) XCOFF_WEAKFIX_QUIET=1 python3 /usr/local/bin/xcoff_weakfix.py "$out" ;;
esac
