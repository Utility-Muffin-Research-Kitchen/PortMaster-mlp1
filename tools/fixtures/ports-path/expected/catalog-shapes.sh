#!/bin/bash
# One line per shape the PortMaster catalog uses outside GAMEDIR/PORTDIR.
source $controlfolder/control.txt

# Double-quoted assignments to other names, and the braced spelling.
G="${HM_PORTS_DIR:-/$directory/ports}/cuphead"
PORTS_DIR="${HM_PORTS_DIR:-/$directory/ports}"
CONFIGFOLDER="${HM_PORTS_DIR:-/$directory/ports}/hurrican"
PORTDIR="${HM_PORTS_DIR:-/$directory/ports}/fallout1"
PORTDIR="${HM_PORTS_DIR:-/$directory/ports}/"
GAMEDIR="${HM_PORTS_DIR:-/$directory/ports}/crysalis"

# Unquoted assignments.
MOONDIR="${HM_PORTS_DIR:-/$directory/ports}"/moonlightnew/moonlight
export BL2_HOME="${HM_PORTS_DIR:-/$directory/ports}"/blockout2

# Inside a longer double-quoted value.
export LD_LIBRARY_PATH="${HM_PORTS_DIR:-/$directory/ports}/Cannonball-st/lib:$LD_LIBRARY_PATH"
export LD_LIBRARY_PATH="/usr/lib:/usr/lib32:${HM_PORTS_DIR:-/$directory/ports}/sulka/libs:$LD_LIBRARY_PATH"
export XDG_CONFIG_HOME="${HM_PORTS_DIR:-/$directory/ports}"
export XDG_DATA_HOME="${HM_PORTS_DIR:-/$directory/ports}"

# Unquoted command arguments, two on one line.
bind_directories ~/.abe "${HM_PORTS_DIR:-/$directory/ports}"/abes_adventure/conf/.abe
cd "${HM_PORTS_DIR:-/$directory/ports}"/fallout2
cd "${HM_PORTS_DIR:-/$directory/ports}"/SRB2/
if [ ! -f "${HM_PORTS_DIR:-/$directory/ports}/blockout2/conf/.bl2/setup.dat" ]; then
  cp -f "${HM_PORTS_DIR:-/$directory/ports}"/blockout2/conf/res_configs/setup.dat.640 "${HM_PORTS_DIR:-/$directory/ports}"/blockout2/conf/.bl2/setup.dat
fi
rm -f "${HM_PORTS_DIR:-/$directory/ports}/rawgl/oga_controls"

# Comment lines stay as they are.
# ln -sfv /$directory/ports/warmux/conf/.config/warmux ~/
