#!/bin/bash
# Command substitutions, backticks, defaults, case, arithmetic and continuations.
size="$(du -s "/$directory/ports/foo" | cut -f1)"
list=$(ls /$directory/ports/foo)
old=`ls /$directory/ports/foo`
quoted="`ls /$directory/ports/foo`"
root=${PORTS_ROOT:-/$directory/ports}
root="${PORTS_ROOT:-/$directory/ports}"
case "$PWD" in
  /$directory/ports/*) echo inside ;;
  *) echo outside ;;
esac
shift_by=$(( 1 << 2 ))
(( shift_by <<= 1 ))
tr a b <<< "/$directory/ports/foo"
cp -r /$directory/ports/foo \
  /$directory/ports/bar
banner="first line
/$directory/ports/second line"
[[ -d /$directory/ports/foo ]] && cd /$directory/ports/foo; ls /$directory/ports/foo|wc -l
mkdir -p /$directory/ports/foo>/dev/null
arr=(/$directory/ports/a /$directory/ports/b)
echo hi >/$directory/ports/log.txt
ls /$directory/ports; ls /$directory/ports
