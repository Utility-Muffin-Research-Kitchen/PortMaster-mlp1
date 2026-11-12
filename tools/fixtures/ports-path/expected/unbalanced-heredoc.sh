#!/bin/bash
# The lexer loses track (no heredoc terminator), so only the assignment rules apply.
GAMEDIR="${HM_PORTS_DIR:-/$directory/ports}/game"
cd /$directory/ports/game
cat <<EOF
never closed
