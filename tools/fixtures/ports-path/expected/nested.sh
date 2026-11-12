#!/bin/bash
# Command substitutions, backticks, defaults, case, arithmetic and continuations.
size="$(du -s "${HM_PORTS_DIR:-/$directory/ports}/foo" | cut -f1)"
list=$(ls "${HM_PORTS_DIR:-/$directory/ports}"/foo)
old=`ls ${HM_PORTS_DIR:-/$directory/ports}/foo`
quoted="`ls ${HM_PORTS_DIR:-/$directory/ports}/foo`"
root=${PORTS_ROOT:-"${HM_PORTS_DIR:-/$directory/ports}"}
root="${PORTS_ROOT:-${HM_PORTS_DIR:-/$directory/ports}}"
case "$PWD" in
  "${HM_PORTS_DIR:-/$directory/ports}"/*) echo inside ;;
  *) echo outside ;;
esac
shift_by=$(( 1 << 2 ))
(( shift_by <<= 1 ))
tr a b <<< "${HM_PORTS_DIR:-/$directory/ports}/foo"
cp -r "${HM_PORTS_DIR:-/$directory/ports}"/foo \
  "${HM_PORTS_DIR:-/$directory/ports}"/bar
banner="first line
${HM_PORTS_DIR:-/$directory/ports}/second line"
[[ -d "${HM_PORTS_DIR:-/$directory/ports}"/foo ]] && cd "${HM_PORTS_DIR:-/$directory/ports}"/foo; ls "${HM_PORTS_DIR:-/$directory/ports}"/foo|wc -l
mkdir -p "${HM_PORTS_DIR:-/$directory/ports}"/foo>/dev/null
arr=("${HM_PORTS_DIR:-/$directory/ports}"/a "${HM_PORTS_DIR:-/$directory/ports}"/b)
echo hi >"${HM_PORTS_DIR:-/$directory/ports}"/log.txt
ls "${HM_PORTS_DIR:-/$directory/ports}"; ls "${HM_PORTS_DIR:-/$directory/ports}"
