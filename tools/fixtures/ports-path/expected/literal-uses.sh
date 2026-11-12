#!/bin/bash
# Uses that stay literal or are not the ports directory, plus one quoted use
# followed by a comment with apostrophes.
echo '/$directory/ports/foo'
sed -i 's|/$directory/ports|/x|' conf.txt
printf $'/$directory/ports/foo\n'
cat > quoted.txt <<'EOF'
/$directory/ports/foo
EOF
cat > plain.txt <<EOF
dir=/$directory/ports/foo
EOF
	cat > tabbed.txt <<-"END"
	/$directory/ports/foo
	END
echo "it's ${HM_PORTS_DIR:-/$directory/ports}/next" # don't wrap /$directory/ports here
GAMEDIR="${HM_PORTS_DIR:-/$directory/ports}/already"
cd "${HM_PORTS_DIR:-/$directory/ports}"/already
backup=/$directory/ports.bak
other=/$directory/portsfoo
nested=$HOME/$directory/ports
joined="$HOME"/$directory/ports
escaped=\/$directory/ports
dollar="/\$directory/ports"
echo ${#directory}/$directory/ports
