#!/bin/bash
# One line per shape the PortMaster catalog uses outside GAMEDIR/PORTDIR.
source $controlfolder/control.txt

# Double-quoted assignments to other names, and the braced spelling.
G="/$directory/ports/cuphead"
PORTS_DIR="/$directory/ports"
CONFIGFOLDER="/$directory/ports/hurrican"
PORTDIR="/$directory/ports/fallout1"
PORTDIR="/$directory/ports/"
GAMEDIR="/${directory}/ports/crysalis"

# Unquoted assignments.
MOONDIR=/$directory/ports/moonlightnew/moonlight
export BL2_HOME=/$directory/ports/blockout2

# Inside a longer double-quoted value.
export LD_LIBRARY_PATH="/$directory/ports/Cannonball-st/lib:$LD_LIBRARY_PATH"
export LD_LIBRARY_PATH="/usr/lib:/usr/lib32:/$directory/ports/sulka/libs:$LD_LIBRARY_PATH"
export XDG_CONFIG_HOME="/$directory/ports"
export XDG_DATA_HOME="/$directory/ports"

# Unquoted command arguments, two on one line.
bind_directories ~/.abe /$directory/ports/abes_adventure/conf/.abe
cd /$directory/ports/fallout2
cd /$directory/ports/SRB2/
if [ ! -f "/$directory/ports/blockout2/conf/.bl2/setup.dat" ]; then
  cp -f /$directory/ports/blockout2/conf/res_configs/setup.dat.640 /$directory/ports/blockout2/conf/.bl2/setup.dat
fi
rm -f "/$directory/ports/rawgl/oga_controls"

# Comment lines stay as they are.
# ln -sfv /$directory/ports/warmux/conf/.config/warmux ~/
