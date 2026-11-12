#!/bin/bash
# Side-effect-free assignments; ports-path-fixtures.sh evaluates them before and
# after the rewrite.
v1="/$directory/ports/a b"
v2=/$directory/ports/a
v3=$(printf '%s\n' /$directory/ports/c)
v4="$(printf '%s' "/$directory/ports/d")"
v5=`printf '%s' /$directory/ports/e`
v6=${UNSET_X:-/$directory/ports}
v7="${UNSET_X:-/$directory/ports}/f"
v8=(/$directory/ports/g /$directory/ports/h)
case /$directory/ports/i in /$directory/ports/*) v9=match ;; *) v9=miss ;; esac
v10="/usr/lib:/$directory/ports/lib:$UNSET_X"
v11='/$directory/ports/literal'
v12=/${directory}/ports/j
v13=$(cat <<EOF
/$directory/ports/k
EOF
)
