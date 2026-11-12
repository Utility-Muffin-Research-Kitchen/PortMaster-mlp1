#!/bin/bash
# Side-effect-free assignments; ports-path-fixtures.sh evaluates them before and
# after the rewrite.
v1="${HM_PORTS_DIR:-/$directory/ports}/a b"
v2="${HM_PORTS_DIR:-/$directory/ports}"/a
v3=$(printf '%s\n' "${HM_PORTS_DIR:-/$directory/ports}"/c)
v4="$(printf '%s' "${HM_PORTS_DIR:-/$directory/ports}/d")"
v5=`printf '%s' ${HM_PORTS_DIR:-/$directory/ports}/e`
v6=${UNSET_X:-"${HM_PORTS_DIR:-/$directory/ports}"}
v7="${UNSET_X:-${HM_PORTS_DIR:-/$directory/ports}}/f"
v8=("${HM_PORTS_DIR:-/$directory/ports}"/g "${HM_PORTS_DIR:-/$directory/ports}"/h)
case "${HM_PORTS_DIR:-/$directory/ports}"/i in "${HM_PORTS_DIR:-/$directory/ports}"/*) v9=match ;; *) v9=miss ;; esac
v10="/usr/lib:${HM_PORTS_DIR:-/$directory/ports}/lib:$UNSET_X"
v11='/$directory/ports/literal'
v12="${HM_PORTS_DIR:-/$directory/ports}"/j
v13=$(cat <<EOF
/$directory/ports/k
EOF
)
