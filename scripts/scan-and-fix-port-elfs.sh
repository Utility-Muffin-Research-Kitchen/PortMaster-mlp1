#!/usr/bin/env bash
set -euo pipefail

platform="${PLATFORM:-mlp1}"
script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
resolver="$script_dir/resolve-sdcard-root.sh"
if [ ! -f "$resolver" ]; then
  echo "leaf armhf scan: SD resolver missing: $resolver" >&2
  exit 1
fi
if ! sdcard_path="$(PLATFORM="$platform" "$resolver" "$script_dir/.." 2>&1)"; then
  echo "leaf armhf scan: SD root error: $sdcard_path" >&2
  exit 1
fi

userdata_path="${USERDATA_PATH:-$sdcard_path/.userdata/$platform}"
data_dir="${PORTMASTER_MLP1_DATA_DIR:-$userdata_path/portmaster}"
tools_bin="${LEAF_PM_TOOLS_DIR:-$data_dir/compat/tools/aarch64/bin}"
if [ -d "$tools_bin" ]; then
  case ":${PATH:-}:" in
    *:"$tools_bin":*) ;;
    *) export PATH="$tools_bin:${PATH:-/usr/bin:/usr/sbin:/bin:/sbin}" ;;
  esac
fi
controlfolder="${PORTMASTER_CONTROLFOLDER:-$data_dir/PortMaster}"
compat_root="${LEAF_PM_ARMHF_ROOT:-$data_dir/compat/armhf}"
aarch64_compat_lib_dir="${LEAF_PM_AARCH64_COMPAT_LIB_DIR:-$data_dir/compat/libs/aarch64}"
aarch64_compat_optout="${LEAF_PM_AARCH64_COMPAT_LIBS_OPTOUT:-$data_dir/compat-libs-optout.txt}"
aarch64_drm_rotate_shim="${LEAF_PM_AARCH64_DRM_ROTATE_SHIM:-$data_dir/compat/drm/aarch64/leaf-drm-rotate.so}"
ports_dir="${1:-${ROMS_PATH:-$sdcard_path/Roms}/PORTS}"
leaf_dir="$data_dir/.leaf"
report_tsv="${LEAF_PM_ARMHF_SCAN_TSV:-$leaf_dir/armhf-scan.tsv}"
report_json="${LEAF_PM_ARMHF_SCAN_JSON:-$leaf_dir/armhf-scan.json}"
RULESET_VERSION=9
manifest_path="${LEAF_PM_ARMHF_SCAN_MANIFEST:-$leaf_dir/armhf-scan.manifest}"
hook_path="$controlfolder/leaf-armhf-env.sh"
full_port_scan="${LEAF_PM_FULL_PORT_SCAN:-0}"
scan_no_cache="${LEAF_PM_SCAN_NO_CACHE:-0}"
scan_mode="fast"
if [ "$full_port_scan" = "1" ] || [ "$full_port_scan" = "true" ] || [ "$full_port_scan" = "yes" ]; then
  scan_mode="full"
fi
log() {
  printf 'leaf armhf scan: %s\n' "$*" >&2
}

json_escape() {
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g; s/	/\\t/g'
}

shell_quote() {
  printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

sha_tool="none"
if command -v sha256sum >/dev/null 2>&1; then
  sha_tool="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
  sha_tool="shasum"
fi

mkdir -p "$leaf_dir"

if [ ! -d "$ports_dir" ]; then
  log "ports dir missing: $ports_dir"
  exit 1
fi

compat_available=0
if [ -f "$compat_root/lib/ld-linux-armhf.so.3" ] &&
   [ -f "$compat_root/bin/leaf-armhf-run" ]; then
  compat_available=1
  chmod 755 "$compat_root/lib/ld-linux-armhf.so.3" "$compat_root/bin/leaf-armhf-run" 2>/dev/null || true
fi
sdl2_fullscreen_shim="$compat_root/bin/leaf-sdl2-fullscreen.so"
sdl2_fullscreen_available=0
if [ "$compat_available" -eq 1 ] &&
   [ -f "$sdl2_fullscreen_shim" ] &&
   grep -q 'LEAF_PM_ARMHF_PRELOAD' "$compat_root/bin/leaf-armhf-run" 2>/dev/null; then
  sdl2_fullscreen_available=1
  chmod 755 "$sdl2_fullscreen_shim" 2>/dev/null || true
fi
aarch64_sdl2_fullscreen_shim="$data_dir/compat/sdl2/aarch64/leaf-sdl2-fullscreen.so"
aarch64_sdl2_fullscreen_available=0
if [ -f "$aarch64_sdl2_fullscreen_shim" ]; then
  aarch64_sdl2_fullscreen_available=1
  chmod 755 "$aarch64_sdl2_fullscreen_shim" 2>/dev/null || true
fi
aarch64_compat_libs_available=0
if [ -d "$aarch64_compat_lib_dir" ]; then
  aarch64_compat_libs_available=1
  chmod 755 "$aarch64_compat_lib_dir"/lib*.so* 2>/dev/null || true
fi
aarch64_drm_rotate_available=0
if [ -f "$aarch64_drm_rotate_shim" ]; then
  aarch64_drm_rotate_available=1
  chmod 755 "$aarch64_drm_rotate_shim" 2>/dev/null || true
fi
manifest_key="$RULESET_VERSION|$scan_mode|$compat_available|$sdl2_fullscreen_available|$aarch64_sdl2_fullscreen_available|$aarch64_compat_libs_available|$aarch64_drm_rotate_available|$ports_dir"

write_leaf_hook() {
  hook_writer="$script_dir/write-leaf-runtime-hook.sh"
  if [ ! -f "$hook_writer" ]; then
    log "hook writer missing: $hook_writer"
    return 1
  fi

  PLATFORM="$platform" \
  SDCARD_PATH="$sdcard_path" \
  USERDATA_PATH="$userdata_path" \
  ROMS_PATH="${ROMS_PATH:-$sdcard_path/Roms}" \
  PORTMASTER_MLP1_DATA_DIR="$data_dir" \
  PORTMASTER_CONTROLFOLDER="$controlfolder" \
    bash "$hook_writer" "$hook_path"
}

patch_control_txt() {
  control_txt="$controlfolder/control.txt"
  [ -f "$control_txt" ] || return 0
  if grep -q 'leaf-armhf-env.sh' "$control_txt"; then
    return 0
  fi

  tmp="$control_txt.tmp.$$"
  awk '
    {
      print
      if ($0 == "source $controlfolder/funcs.txt" && inserted == 0) {
        print ""
        print "# Leaf runtime compatibility hook"
        print "if [ -f \"$controlfolder/leaf-armhf-env.sh\" ]; then"
        print "  source \"$controlfolder/leaf-armhf-env.sh\""
        print "fi"
        inserted = 1
      }
    }
    END {
      if (inserted == 0) {
        print ""
        print "# Leaf runtime compatibility hook"
        print "if [ -f \"$controlfolder/leaf-armhf-env.sh\" ]; then"
        print "  source \"$controlfolder/leaf-armhf-env.sh\""
        print "fi"
      }
    }
  ' "$control_txt" >"$tmp"
  mv "$tmp" "$control_txt"
  chmod 755 "$control_txt" 2>/dev/null || true
}

elf_header_hex() {
  local header
  header="$(od -An -tx1 -N20 "$1" 2>/dev/null || true)"
  header="${header// /}"
  header="${header//$'\t'/}"
  header="${header//$'\n'/}"
  printf '%s' "$header"
}

is_armhf_elf() {
  header="$1"
  [ "${#header}" -ge 40 ] || return 1
  [ "${header:0:8}" = "7f454c46" ] || return 1
  [ "${header:8:2}" = "01" ] || return 1
  [ "${header:10:2}" = "01" ] || return 1
  [ "${header:36:4}" = "2800" ] || return 1
}

is_aarch64_elf() {
  header="$1"
  [ "${#header}" -ge 40 ] || return 1
  [ "${header:0:8}" = "7f454c46" ] || return 1
  [ "${header:8:2}" = "02" ] || return 1
  [ "${header:10:2}" = "01" ] || return 1
  [ "${header:36:4}" = "b700" ] || return 1
}

elf_kind() {
  header="$1"
  case "${header:32:4}" in
    0200) printf 'executable' ;;
    0300) printf 'shared-or-pie' ;;
    *) printf 'elf-type-%s' "${header:32:4}" ;;
  esac
}

armhf_interpreter() {
  local file="$1"
  head -c 65536 "$file" 2>/dev/null | grep -a -m 1 -o '/[^[:space:]]*ld-linux-armhf\.so\.3' 2>/dev/null || true
}

elf_links_sdl2() {
  LC_ALL=C grep -a -m1 -q 'libSDL2-2\.0\.so' "$1" 2>/dev/null
}

file_sha256() {
  local out hash
  case "$sha_tool" in
    sha256sum) out="$(sha256sum "$1" 2>/dev/null || true)" ;;
    shasum) out="$(shasum -a 256 "$1" 2>/dev/null || true)" ;;
    *) printf 'unknown'; return 0 ;;
  esac
  if [ -z "$out" ]; then
    printf 'unknown'
    return 0
  fi
  read -r hash _ <<<"$out" || true
  printf '%s' "${hash:-unknown}"
}

wrapper_header_contains() {
  local file="$1"
  local needle="$2"
  local line
  local n=0
  while [ "$n" -lt 4 ] && IFS= read -r line; do
    case "$line" in
      *"$needle"*) return 0 ;;
    esac
    n=$((n + 1))
  done <"$file" 2>/dev/null
  return 1
}

is_leaf_armhf_wrapper() {
  wrapper_header_contains "$1" 'LEAF_PM_ARMHF_WRAPPER=1'
}

is_leaf_armhf_wrapper_v3() {
  wrapper_header_contains "$1" 'LEAF_PM_ARMHF_WRAPPER_VERSION=3'
}

wrapper_path_for() {
  file="$1"
  dir="$(dirname "$file")"
  base="$(basename "$file")"
  printf '%s/.leaf-armhf/%s' "$dir" "$base"
}

port_dir_for() {
  local file="$1"
  local root="${ports_dir%/}"
  local rel port
  case "$file" in
    "$root"/*) rel="${file#"$root"/}" ;;
    *) return 1 ;;
  esac
  case "$rel" in
    */*) port="${rel%%/*}" ;;
    *) return 1 ;;
  esac
  [ -n "$port" ] || return 1
  printf '%s' "$port"
}

write_armhf_wrapper() {
  file="$1"
  original="$2"
  base="$(basename "$file")"
  quoted_original="$(shell_quote ".leaf-armhf/$base")"
  tmp="$file.tmp.$$"

  cat >"$tmp" <<EOF
#!/bin/sh
# LEAF_PM_ARMHF_WRAPPER=1
# LEAF_PM_ARMHF_WRAPPER_VERSION=3
set -eu

self_dir="\$(CDPATH= cd -- "\$(dirname -- "\$0")" && pwd)"
control_dir="\${PORTMASTER_CONTROLFOLDER:-\${controlfolder:-}}"
if [ -n "\$control_dir" ] && [ -f "\$control_dir/leaf-armhf-env.sh" ]; then
  . "\$control_dir/leaf-armhf-env.sh"
fi

if [ -z "\${LEAF_PM_ARMHF_RUN:-}" ] || [ -z "\${LEAF_PM_BOX86:-}" ]; then
  platform="\${PLATFORM:-mlp1}"
  sdcard="\${SDCARD_PATH:-}"
  if [ -z "\$sdcard" ]; then
    case "\$self_dir" in
      */Roms/PORTS|*/Roms/PORTS/*) sdcard="\${self_dir%%/Roms/PORTS*}" ;;
    esac
  fi
  if [ -z "\$sdcard" ]; then
    echo "Leaf PortMaster: cannot resolve SD root; set SDCARD_PATH" >&2
    exit 127
  fi
  userdata="\${USERDATA_PATH:-\$sdcard/.userdata/\$platform}"
fi

if [ -z "\${LEAF_PM_ARMHF_RUN:-}" ]; then
  LEAF_PM_ARMHF_RUN="\${LEAF_PM_ARMHF_ROOT:-\$userdata/portmaster/compat/armhf}/bin/leaf-armhf-run"
fi

if [ -z "\${LEAF_PM_BOX86:-}" ]; then
  LEAF_PM_BOX86="\${LEAF_PM_ARMHF_ROOT:-\$userdata/portmaster/compat/armhf}/bin/box86"
fi

export SDL_VIDEO_EGL_DRIVER="\${SDL_VIDEO_EGL_DRIVER:-\${LEAF_PM_ARMHF_SDL_VIDEO_EGL_DRIVER:-libEGL.so}}"
export SDL_VIDEO_GL_DRIVER="\${SDL_VIDEO_GL_DRIVER:-\${LEAF_PM_ARMHF_SDL_VIDEO_GL_DRIVER:-libGLESv2.so}}"

if [ "$base" = "box86" ] && [ -x "\$LEAF_PM_BOX86" ]; then
  exec "\$LEAF_PM_ARMHF_RUN" "\$LEAF_PM_BOX86" "\$@"
fi

exec "\$LEAF_PM_ARMHF_RUN" "\$self_dir"/$quoted_original "\$@"
EOF
  mv "$tmp" "$file"
  chmod 755 "$file" "$original" 2>/dev/null || true
}

is_godot_script() {
  file="$1"
  case "$file" in
    *.sh) ;;
    *) return 1 ;;
  esac
  grep -Eq 'godot_runtime=|godot_executable=|--rendering-driver[[:space:]]+opengl3_es|godot[0-9]+.*DEVICE_ARCH|runtime="?frt_[0-9]' "$file" 2>/dev/null
}

is_gothic_machismo_script() {
  file="$1"
  case "$file" in
    *.sh) ;;
    *) return 1 ;;
  esac
  grep -Eq 'bin/machismo|MACHISMO_CONFIG' "$file" 2>/dev/null &&
    grep -Eq 'GOTHIC_BACKEND|GOTHIC_SHADER_DIR|libgothic_patches' "$file" 2>/dev/null
}

is_ship_of_harkinian_script() {
  file="$1"
  case "$file" in
    *.sh) ;;
    *) return 1 ;;
  esac
  grep -Eq 'soh[.]elf|shipofharkinian[.]json|oot[-]?mq[.]o2r|oot[.]o2r' "$file" 2>/dev/null &&
    grep -Eq 'GAMEDIR=.*[/]soh|Ship of Harkinian' "$file" 2>/dev/null
}

is_love_runtime_script() {
  file="$1"
  case "$file" in
    *.sh) ;;
    *) return 1 ;;
  esac
  grep -Eq '(^|[[:space:];|&])(\./)?love([[:space:];|&]|$)|[$][{]?GAMEDIR[}]?/love|liblove-11[.]5[.]so|love_11[.]5' "$file" 2>/dev/null
}

is_sdl2_fullscreen_optout_port() {
  case "$1" in
    soh|shipofharkinian|ship-of-harkinian) return 0 ;;
    *) return 1 ;;
  esac
}

script_port_dir() {
  file="$1"
  awk '
    /^[[:space:]]*#/ { next }
    {
      line = $0
      sub(/^[[:space:]]*/, "", line)
      if (line !~ /^GAMEDIR=/) {
        next
      }
      sub(/^GAMEDIR=/, "", line)
      sub(/[[:space:]]*#.*/, "", line)
      sub(/^[[:space:]]+/, "", line)
      sub(/[[:space:]]+$/, "", line)
      gsub(/^["\047]/, "", line)
      gsub(/["\047]$/, "", line)
      sub(/[[:space:]].*$/, "", line)
      gsub(/^["\047]/, "", line)
      gsub(/["\047]$/, "", line)
      n = split(line, parts, "/")
      port = parts[n]
      gsub(/["\047;]/, "", port)
      if (port != "" && port !~ /[$(){}]/) {
        print port
        exit
      }
    }
  ' "$file"
}

sdl2_arch_csv_for_port() {
  local port="$1"
  local csv=""
  if [ -n "${port_sdl2_aarch64[$port]+x}" ]; then
    csv="aarch64"
  fi
  if [ -n "${port_sdl2_armhf[$port]+x}" ]; then
    if [ -n "$csv" ]; then
      csv="$csv,armhf"
    else
      csv="armhf"
    fi
  fi
  printf '%s' "$csv"
}

script_sdl2_cache_tag() {
  local file="$1"
  local port arch_csv compat_csv unresolved_csv
  port="$(script_port_dir "$file" || true)"
  if [ -z "$port" ]; then
    printf 'no-gamedir'
    return 0
  fi
  if is_godot_script "$file" || is_ship_of_harkinian_script "$file" ||
     is_sdl2_fullscreen_optout_port "$port"; then
    printf '%s:optout|compat:-|unresolved:-' "$port"
    return 0
  fi
  arch_csv="$(sdl2_arch_csv_for_port "$port")"
  [ -n "$arch_csv" ] || arch_csv="-"
  compat_csv="${port_aarch64_compat_needed[$port]:--}"
  unresolved_csv="${port_aarch64_unresolved[$port]:--}"
  if is_aarch64_compat_libs_optout_port "$port"; then
    compat_csv="optout"
  fi
  printf '%s:%s|compat:%s|unresolved:%s' "$port" "$arch_csv" "$compat_csv" "$unresolved_csv"
}

csv_has_value() {
  local csv="$1"
  local value="$2"
  case ",$csv," in
    *,"$value",*) return 0 ;;
    *) return 1 ;;
  esac
}

csv_append_unique() {
  local csv="$1"
  local value="$2"
  if [ -z "$value" ]; then
    printf '%s' "$csv"
  elif [ -z "$csv" ] || [ "$csv" = "-" ]; then
    printf '%s' "$value"
  elif csv_has_value "$csv" "$value"; then
    printf '%s' "$csv"
  else
    printf '%s,%s' "$csv" "$value"
  fi
}

record_port_aarch64_compat_soname() {
  local port="$1"
  local soname="$2"
  [ -n "$port" ] && [ -n "$soname" ] || return 0
  port_aarch64_compat_needed["$port"]="$(csv_append_unique "${port_aarch64_compat_needed[$port]:-}" "$soname")"
  aarch64_compat_sonames_seen["$soname"]=1
}

record_port_aarch64_unresolved_soname() {
  local port="$1"
  local soname="$2"
  [ -n "$port" ] && [ -n "$soname" ] || return 0
  port_aarch64_unresolved["$port"]="$(csv_append_unique "${port_aarch64_unresolved[$port]:-}" "$soname")"
  aarch64_unresolved_sonames_seen["$soname"]=1
}

is_aarch64_compat_libs_optout_port() {
  local port="$1"
  [ -n "$port" ] || return 1
  [ -f "$aarch64_compat_optout" ] || return 1
  awk -v port="$port" '
    /^[[:space:]]*(#|$)/ { next }
    {
      line = $0
      sub(/[[:space:]]*#.*/, "", line)
      sub(/^[[:space:]]+/, "", line)
      sub(/[[:space:]]+$/, "", line)
      if (line == "*" || line == port) {
        found = 1
      }
    }
    END { exit found ? 0 : 1 }
  ' "$aarch64_compat_optout"
}

load_aarch64_compat_sonames() {
  local manifest name
  [ "$aarch64_compat_libs_available" -eq 1 ] || return 0
  manifest="$aarch64_compat_lib_dir/manifest.json"
  if [ -f "$manifest" ]; then
    while IFS= read -r name; do
      [ -n "$name" ] || continue
      aarch64_compat_sonames["$name"]="manifest"
    done < <(sed -n 's/^[[:space:]]*"name":[[:space:]]*"\([^"]*\)".*/\1/p' "$manifest" 2>/dev/null || true)
  fi
  while IFS= read -r name; do
    [ -n "$name" ] || continue
    aarch64_compat_sonames["$name"]="file"
  done < <(find "$aarch64_compat_lib_dir" -maxdepth 1 -type f -name 'lib*.so*' -exec basename {} \; 2>/dev/null || true)
}

readelf_needed_sonames() {
  local file="$1"
  command -v readelf >/dev/null 2>&1 || return 1
  LC_ALL=C readelf -d "$file" 2>/dev/null |
    sed -n 's/.*Shared library:[[:space:]]*\[\([^]]*\)\].*/\1/p'
}

load_port_lib_index() {
  local port="$1"
  local port_path file base key
  [ -n "${port_lib_index_loaded[$port]+x}" ] && return 0
  port_lib_index_loaded["$port"]=1
  port_path="$ports_dir/$port"
  [ -d "$port_path" ] || return 0

  while IFS= read -r -d '' file; do
    base="${file##*/}"
    [ -n "$base" ] || continue
    key="$port|$base"
    port_lib_index["$key"]=1
  done < <(find "$port_path" -maxdepth 5 -type f -name '*.so*' -print0 2>/dev/null || true)
}

soname_in_port_local_tree() {
  local port="$1"
  local soname="$2"
  local port_path key
  [ -n "$port" ] || return 1
  case "$soname" in
    ""|*/*) return 1 ;;
  esac
  port_path="$ports_dir/$port"
  [ -d "$port_path" ] || return 1
  [ -e "$port_path/$soname" ] && return 0
  load_port_lib_index "$port"
  key="$port|$soname"
  [ -n "${port_lib_index[$key]+x}" ]
}

soname_in_stock_paths() {
  local soname="$1"
  local dir
  for dir in /usr/lib /lib /usr/local/lib /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu; do
    [ -e "$dir/$soname" ] && return 0
  done
  return 1
}

resolve_aarch64_soname_source() {
  local port="$1"
  local soname="$2"
  if soname_in_port_local_tree "$port" "$soname"; then
    printf 'port'
  elif soname_in_stock_paths "$soname"; then
    printf 'stock'
  elif [ -n "${aarch64_compat_sonames[$soname]+x}" ]; then
    printf 'compat'
  else
    printf 'missing'
  fi
}

aarch64_needed_tag_for_file() {
  local file="$1"
  local port soname source needed compat_csv missing_csv
  port="$(port_dir_for "$file" || true)"
  if [ -z "$port" ]; then
    printf '-'
    return 0
  fi
  if is_aarch64_compat_libs_optout_port "$port"; then
    printf 'optout'
    return 0
  fi
  if ! command -v readelf >/dev/null 2>&1; then
    printf 'no-readelf'
    return 0
  fi
  needed="$(readelf_needed_sonames "$file" || true)"
  [ -n "$needed" ] || {
    printf '-'
    return 0
  }
  compat_csv=""
  missing_csv=""
  while IFS= read -r soname; do
    [ -n "$soname" ] || continue
    source="$(resolve_aarch64_soname_source "$port" "$soname")"
    case "$source" in
      compat)
        compat_csv="$(csv_append_unique "$compat_csv" "$soname")"
        ;;
      missing)
        missing_csv="$(csv_append_unique "$missing_csv" "$soname")"
        ;;
    esac
  done <<<"$needed"
  if [ -n "$compat_csv" ] || [ -n "$missing_csv" ]; then
    printf 'c=%s;m=%s' "$compat_csv" "$missing_csv"
  else
    printf '-'
  fi
}

apply_manifest_needed_tag() {
  local file="$1"
  local tag="$2"
  local port part compat_csv missing_csv soname
  local -a parts=()
  local -a compat_parts=()
  local -a missing_parts=()
  case "$tag" in
    ""|-|optout|no-readelf) return 0 ;;
  esac
  port="$(port_dir_for "$file" || true)"
  [ -n "$port" ] || return 0
  compat_csv=""
  missing_csv=""
  IFS=';' read -r -a parts <<<"$tag"
  for part in "${parts[@]}"; do
    case "$part" in
      c=*) compat_csv="${part#c=}" ;;
      m=*) missing_csv="${part#m=}" ;;
    esac
  done
  if [ -n "$compat_csv" ]; then
    IFS=',' read -r -a compat_parts <<<"$compat_csv"
    for soname in "${compat_parts[@]}"; do
      record_port_aarch64_compat_soname "$port" "$soname"
    done
  fi
  if [ -n "$missing_csv" ]; then
    IFS=',' read -r -a missing_parts <<<"$missing_csv"
    for soname in "${missing_parts[@]}"; do
      record_port_aarch64_unresolved_soname "$port" "$soname"
    done
  fi
}

json_csv_string_array() {
  local csv="$1"
  local sep=""
  local soname
  local -a sonames=()
  printf '['
  IFS=',' read -r -a sonames <<<"$csv"
  for soname in "${sonames[@]}"; do
    [ -n "$soname" ] || continue
    printf '%s"%s"' "$sep" "$(json_escape "$soname")"
    sep=', '
  done
  printf ']'
}

json_port_soname_array() {
  local field="$1"
  local kind="$2"
  local first=1
  local port csv sep
  local -a ports=()
  printf '  "%s": [' "$field"
  case "$kind" in
    compat) ports=("${!port_aarch64_compat_needed[@]}") ;;
    unresolved) ports=("${!port_aarch64_unresolved[@]}") ;;
    *) ports=() ;;
  esac
  if [ "${#ports[@]}" -gt 0 ]; then
    printf '\n'
    while IFS= read -r port; do
      [ -n "$port" ] || continue
      case "$kind" in
        compat) csv="${port_aarch64_compat_needed[$port]:-}" ;;
        unresolved) csv="${port_aarch64_unresolved[$port]:-}" ;;
        *) csv="" ;;
      esac
      [ -n "$csv" ] || continue
      if [ "$first" -eq 1 ]; then
        sep=''
        first=0
      else
        sep=",\n"
      fi
      printf '%b    {"port": "%s", "sonames": ' "$sep" "$(json_escape "$port")"
      json_csv_string_array "$csv"
      printf '}'
    done < <(printf '%s\n' "${ports[@]}" | sort)
    printf '\n  ],\n'
  else
    printf '],\n'
  fi
}

large_file_threshold_bytes=3758096384
large_file_threshold_mib=3584
large_file_count=0

file_size_for_report() {
  local file="$1"
  local line size
  if line="$(stat_one_line "$file" 2>/dev/null)"; then
    size="${line%%$'\t'*}"
    case "$size" in
      ''|*[!0-9]*) ;;
      *) printf '%s\n' "$size"; return 0 ;;
    esac
  fi
  wc -c <"$file" 2>/dev/null | tr -d ' '
}

collect_large_files() {
  local out_file="$1"
  local file size
  : >"$out_file"
  large_file_count=0
  while IFS= read -r -d '' file; do
    path_has_manifest_unsafe_chars "$file" && continue
    size="$(file_size_for_report "$file" || true)"
    case "$size" in
      ''|*[!0-9]*) continue ;;
    esac
    large_file_count=$((large_file_count + 1))
    if [ "$large_file_count" -le 50 ]; then
      printf '%s\t%s\n' "$size" "$file" >>"$out_file"
    fi
  done < <(find "$ports_dir" -type f -size +"${large_file_threshold_mib}"M -print0 2>/dev/null || true)
}

json_large_file_array() {
  local first=1
  local line size path tab sep
  tab=$'\t'
  printf '  "large_files": ['
  if [ -s "$large_file_records" ]; then
    printf '\n'
    while IFS= read -r line; do
      case "$line" in
        *"$tab"*) ;;
        *) continue ;;
      esac
      size="${line%%${tab}*}"
      path="${line#*${tab}}"
      if [ "$first" -eq 1 ]; then
        sep=''
        first=0
      else
        sep=",\n"
      fi
      printf '%b    {"path": "%s", "size": %s}' "$sep" "$(json_escape "$path")" "$size"
    done <"$large_file_records"
    printf '\n  ],\n'
  else
    printf '],\n'
  fi
}

normalize_port_env_script() {
  file="$1"
  case "$file" in
    *.sh) ;;
    *) printf 'not-shell'; return 0 ;;
  esac
  if grep -q 'LEAF_PM_PORT_ENV=1' "$file" 2>/dev/null; then
    if grep -q 'LEAF_PM_PORT_ENV_VERSION=3' "$file" 2>/dev/null &&
       grep -q 'PORTMASTER_LEAF_DEVICE_INFO' "$file" 2>/dev/null &&
       grep -q 'LEAF_PM_TOOLS_DIR' "$file" 2>/dev/null; then
      printf 'port-env-already'
      return 0
    fi
    tmp="$file.tmp.$$"
    awk '
      function insert_block() {
        print "# LEAF_PM_PORT_ENV=1"
        print "# LEAF_PM_PORT_ENV_VERSION=3"
        print "_leaf_pm_platform=\"${PLATFORM:-mlp1}\""
        print "export PLATFORM=\"${PLATFORM:-$_leaf_pm_platform}\""
        print "export PORTMASTER_LEAF_DEVICE_INFO=\"${PORTMASTER_LEAF_DEVICE_INFO:-1}\""
        print "if [ -z \"${XDG_DATA_HOME:-}\" ] || [ ! -d \"$XDG_DATA_HOME/PortMaster\" ]; then"
        print "  _leaf_pm_script_dir=\"$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\""
        print "  _leaf_pm_script_sd=\"\""
        print "  case \"$_leaf_pm_script_dir\" in"
        print "    */Roms/PORTS|*/Roms/PORTS/*) _leaf_pm_script_sd=\"${_leaf_pm_script_dir%%/Roms/PORTS*}\" ;;"
        print "  esac"
        print "  for _leaf_pm_sd in \"${SDCARD_PATH:-}\" \"${JAWAKA_SDCARD_ROOT:-}\" \"$_leaf_pm_script_sd\"; do"
        print "    [ -n \"$_leaf_pm_sd\" ] || continue"
        print "    _leaf_pm_userdata=\"${USERDATA_PATH:-$_leaf_pm_sd/.userdata/$_leaf_pm_platform}\""
        print "    if [ -d \"$_leaf_pm_userdata/portmaster/PortMaster\" ]; then"
        print "      export SDCARD_PATH=\"${SDCARD_PATH:-$_leaf_pm_sd}\""
        print "      export USERDATA_PATH=\"$_leaf_pm_userdata\""
        print "      export ROMS_PATH=\"${ROMS_PATH:-$SDCARD_PATH/Roms}\""
        print "      export IMAGES_PATH=\"${IMAGES_PATH:-$SDCARD_PATH/Images}\""
        print "      export XDG_DATA_HOME=\"$_leaf_pm_userdata/portmaster\""
        print "      export PORTMASTER_CONTROLFOLDER=\"$_leaf_pm_userdata/portmaster/PortMaster\""
        print "      break"
        print "    fi"
        print "  done"
        print "fi"
        print "_leaf_pm_tools_dir=\"${LEAF_PM_TOOLS_DIR:-${XDG_DATA_HOME:-}/compat/tools/aarch64/bin}\""
        print "if [ -d \"$_leaf_pm_tools_dir\" ]; then"
        print "  export LEAF_PM_TOOLS_DIR=\"$_leaf_pm_tools_dir\""
        print "  case \":${PATH:-}:\" in"
        print "    *:\"$_leaf_pm_tools_dir\":*) ;;"
        print "    *) export PATH=\"$_leaf_pm_tools_dir:${PATH:-/usr/bin:/usr/sbin:/bin:/sbin}\" ;;"
        print "  esac"
        print "fi"
        print "unset _leaf_pm_sd _leaf_pm_userdata _leaf_pm_platform _leaf_pm_script_dir _leaf_pm_script_sd _leaf_pm_tools_dir"
        inserted = 1
      }
      {
        if (!inserted && $0 ~ /^# LEAF_PM_PORT_ENV=1$/) {
          insert_block()
          skipping = 1
          next
        }
        if (skipping) {
          if ($0 ~ /^unset _leaf_pm_sd _leaf_pm_userdata _leaf_pm_platform/) {
            skipping = 0
          }
          next
        }
        print
      }
      END {
        if (!inserted) {
          insert_block()
        }
      }
    ' "$file" >"$tmp"
    mv "$tmp" "$file"
    chmod 755 "$file" 2>/dev/null || true
    printf 'port-env-patched'
    return 0
  fi

  tmp="$file.tmp.$$"
  awk '
    function insert_block() {
      print ""
      print "# LEAF_PM_PORT_ENV=1"
      print "# LEAF_PM_PORT_ENV_VERSION=3"
      print "_leaf_pm_platform=\"${PLATFORM:-mlp1}\""
      print "export PLATFORM=\"${PLATFORM:-$_leaf_pm_platform}\""
      print "export PORTMASTER_LEAF_DEVICE_INFO=\"${PORTMASTER_LEAF_DEVICE_INFO:-1}\""
      print "if [ -z \"${XDG_DATA_HOME:-}\" ] || [ ! -d \"$XDG_DATA_HOME/PortMaster\" ]; then"
      print "  _leaf_pm_script_dir=\"$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\""
      print "  _leaf_pm_script_sd=\"\""
      print "  case \"$_leaf_pm_script_dir\" in"
      print "    */Roms/PORTS|*/Roms/PORTS/*) _leaf_pm_script_sd=\"${_leaf_pm_script_dir%%/Roms/PORTS*}\" ;;"
      print "  esac"
      print "  for _leaf_pm_sd in \"${SDCARD_PATH:-}\" \"${JAWAKA_SDCARD_ROOT:-}\" \"$_leaf_pm_script_sd\"; do"
      print "    [ -n \"$_leaf_pm_sd\" ] || continue"
      print "    _leaf_pm_userdata=\"${USERDATA_PATH:-$_leaf_pm_sd/.userdata/$_leaf_pm_platform}\""
      print "    if [ -d \"$_leaf_pm_userdata/portmaster/PortMaster\" ]; then"
      print "      export SDCARD_PATH=\"${SDCARD_PATH:-$_leaf_pm_sd}\""
      print "      export USERDATA_PATH=\"$_leaf_pm_userdata\""
      print "      export ROMS_PATH=\"${ROMS_PATH:-$SDCARD_PATH/Roms}\""
      print "      export IMAGES_PATH=\"${IMAGES_PATH:-$SDCARD_PATH/Images}\""
      print "      export XDG_DATA_HOME=\"$_leaf_pm_userdata/portmaster\""
      print "      export PORTMASTER_CONTROLFOLDER=\"$_leaf_pm_userdata/portmaster/PortMaster\""
      print "      break"
      print "    fi"
      print "  done"
      print "fi"
      print "_leaf_pm_tools_dir=\"${LEAF_PM_TOOLS_DIR:-${XDG_DATA_HOME:-}/compat/tools/aarch64/bin}\""
      print "if [ -d \"$_leaf_pm_tools_dir\" ]; then"
      print "  export LEAF_PM_TOOLS_DIR=\"$_leaf_pm_tools_dir\""
      print "  case \":${PATH:-}:\" in"
      print "    *:\"$_leaf_pm_tools_dir\":*) ;;"
      print "    *) export PATH=\"$_leaf_pm_tools_dir:${PATH:-/usr/bin:/usr/sbin:/bin:/sbin}\" ;;"
      print "  esac"
      print "fi"
      print "unset _leaf_pm_sd _leaf_pm_userdata _leaf_pm_platform _leaf_pm_script_dir _leaf_pm_script_sd _leaf_pm_tools_dir"
      inserted = 1
    }
    {
      print
      if (!inserted && $0 ~ /^XDG_DATA_HOME=/) {
        insert_block()
      } else if (!inserted && $0 ~ /source[[:space:]]+\$?controlfolder\/control[.]txt/) {
        insert_block()
      }
    }
    END {
      if (!inserted) {
        insert_block()
      }
    }
  ' "$file" >"$tmp"
  mv "$tmp" "$file"
  chmod 755 "$file" 2>/dev/null || true
  printf 'port-env-patched'
}

normalize_port_paths_script() {
  file="$1"
  case "$file" in
    *.sh) ;;
    *) printf 'not-shell'; return 0 ;;
  esac
  if ! grep -Eq '[Gg][Aa][Mm][Ee][Dd][Ii][Rr]="?/\$directory/ports/' "$file" 2>/dev/null; then
    printf 'port-paths-already'
    return 0
  fi

  tmp="$file.tmp.$$"
  awk '
    /^[[:space:]]*(export[[:space:]]+)?[Gg][Aa][Mm][Ee][Dd][Ii][Rr]="\/\$directory\/ports\/[^"#[:space:]]+"[[:space:]]*$/ {
      line = $0
      sub(/="\/\$directory\/ports\//, "=\"${HM_PORTS_DIR:-/$directory/ports}/", line)
      print line
      next
    }
    /^[[:space:]]*(export[[:space:]]+)?[Gg][Aa][Mm][Ee][Dd][Ii][Rr]=\/\$directory\/ports\/[^[:space:]#]+[[:space:]]*$/ {
      line = $0
      sub(/=\/\$directory\/ports\//, "=\"${HM_PORTS_DIR:-/$directory/ports}/", line)
      print line "\""
      next
    }
    { print }
  ' "$file" >"$tmp"
  mv "$tmp" "$file"
  chmod 755 "$file" 2>/dev/null || true
  printf 'port-paths-patched'
}

normalize_libretro_retroarch_script() {
  file="$1"
  case "$file" in
    *.sh) ;;
    *) printf 'not-shell'; return 0 ;;
  esac
  if grep -q 'leaf_pm_run_retroarch' "$file" 2>/dev/null; then
    printf 'libretro-retroarch-already'
    return 0
  fi
  if ! grep -Eq 'retroarch([^[:alnum:]_]|$).*([[:space:]]|^)-L([[:space:]]|$)' "$file" 2>/dev/null; then
    printf 'libretro-retroarch-missing'
    return 0
  fi

  tmp="$file.tmp.$$"
  awk '
    {
      line = $0
      if (line ~ /retroarch/ && line ~ /(^|[[:space:]])-L([[:space:]]|$)/) {
        gsub(/"?[$][{]?raloc[}]?\/retroarch"?[[:space:]]+[$][{]?raconf[}]?[[:space:]]+/, "leaf_pm_run_retroarch ", line)
        gsub(/"?[$][{]?raloc[}]?\/retroarch"?[[:space:]]+/, "leaf_pm_run_retroarch ", line)
        gsub(/"?\/[^"[:space:]]*\/retroarch"?[[:space:]]+[$][{]?raconf[}]?[[:space:]]+/, "leaf_pm_run_retroarch ", line)
        gsub(/^[[:space:]]*retroarch[[:space:]]+[$][{]?raconf[}]?[[:space:]]+/, "leaf_pm_run_retroarch ", line)
        gsub(/^[[:space:]]*retroarch[[:space:]]+/, "leaf_pm_run_retroarch ", line)
        gsub(/[[:space:]]+[$][{]?raconf[}]?[[:space:]]+/, " ", line)
        if (line != $0) {
          changed = 1
        }
      }
      print line
    }
    END { exit changed ? 0 : 1 }
  ' "$file" >"$tmp" || {
    rm -f "$tmp"
    printf 'libretro-retroarch-missing'
    return 0
  }
  mv "$tmp" "$file"
  chmod 755 "$file" 2>/dev/null || true
  printf 'libretro-retroarch-patched'
}

normalize_godot_wayland_script() {
  file="$1"
  if ! is_godot_script "$file"; then
    printf 'not-godot'
    return 0
  fi
  if grep -Eq 'LEAF_PM_(GODOT_WAYLAND|EGL_GLES_SHIM)=1' "$file" 2>/dev/null; then
    printf 'godot-wayland-already'
    return 0
  fi

  tmp="$file.tmp.$$"
  awk '
    function insert_block() {
      print ""
      print "# LEAF_PM_GODOT_WAYLAND=1"
      print "if declare -f leaf_pm_enable_godot_wayland_runtime >/dev/null 2>&1; then"
      print "  leaf_pm_enable_godot_wayland_runtime"
      print "fi"
      inserted = 1
    }
    {
      print
      if (!inserted && $0 ~ /^[[:space:]]*get_controls([[:space:]]|$)/) {
        insert_block()
      } else if (!inserted && $0 ~ /source[[:space:]].*control[.]txt/) {
        insert_block()
      }
    }
    END {
      if (!inserted) {
        insert_block()
      }
    }
  ' "$file" >"$tmp"
  mv "$tmp" "$file"
  chmod 755 "$file" 2>/dev/null || true
  printf 'godot-wayland-patched'
}

normalize_godot_wayland_runtime_script() {
  file="$1"
  if ! is_godot_script "$file"; then
    return 0
  fi
  if grep -q 'LEAF_PM_GODOT_WAYLAND_RUNTIME_UPGRADE_VERSION=1' "$file" 2>/dev/null; then
    printf 'godot-wayland-runtime-already'
    return 0
  fi
  if ! grep -Eq '^[[:space:]]*godot_runtime="?godot_4[.]2[.]2"?' "$file" 2>/dev/null ||
     ! grep -Eq '^[[:space:]]*godot_executable="?godot422[.]' "$file" 2>/dev/null ||
     ! grep -q 'westonwrap.sh' "$file" 2>/dev/null; then
    return 0
  fi

  tmp="$file.tmp.$$"
  if awk '
    function insert_block() {
      print ""
      print "# LEAF_PM_GODOT_WAYLAND_RUNTIME_UPGRADE=1"
      print "# LEAF_PM_GODOT_WAYLAND_RUNTIME_UPGRADE_VERSION=1"
      print "# Godot 4.2.2 PortMaster runtime only exposes x11/headless; use the Wayland-capable 4.3 runtime on Leaf."
      print "if [ \"${LEAF_PM_GODOT_422_WAYLAND_UPGRADE:-1}\" != \"0\" ]; then"
      print "  godot_runtime=\"${LEAF_PM_GODOT_WAYLAND_RUNTIME:-godot_4.3}\""
      print "  godot_executable=\"${LEAF_PM_GODOT_WAYLAND_EXECUTABLE:-godot43.$DEVICE_ARCH}\""
      print "fi"
      inserted = 1
      changed = 1
    }
    {
      print
      if (!inserted && $0 ~ /^[[:space:]]*godot_executable="?godot422[.]/) {
        insert_block()
      }
    }
    END {
      if (!inserted || !changed) {
        exit 2
      }
    }
  ' "$file" >"$tmp"; then
    mv "$tmp" "$file"
    chmod 755 "$file" 2>/dev/null || true
    printf 'godot-wayland-runtime-patched'
    return 0
  else
    rc=$?
  fi

  rm -f "$tmp"
  if [ "$rc" -eq 2 ]; then
    printf 'godot-wayland-runtime-missing-anchor'
    return 0
  fi
  printf 'godot-wayland-runtime-error'
}

normalize_godot_weston_cleanup_script() {
  file="$1"
  if ! is_godot_script "$file"; then
    printf 'not-godot'
    return 0
  fi
  if grep -q 'LEAF_PM_WESTONPACK_CLEANUP_GUARD=1' "$file" 2>/dev/null; then
    printf 'weston-cleanup-already'
    return 0
  fi

  tmp="$file.tmp.$$"
  awk '
    /^[[:space:]]*#/ { print; next }
    index($0, "westonwrap.sh cleanup") > 0 {
      indent = $0
      sub(/[^[:space:]].*$/, "", indent)
      cmd = substr($0, length(indent) + 1)
      print indent "# LEAF_PM_WESTONPACK_CLEANUP_GUARD=1"
      print indent "if [ \"${LEAF_PM_SKIP_WESTONPACK_CLEANUP:-0}\" != \"1\" ]; then"
      print indent "  " cmd
      print indent "fi"
      changed = 1
      next
    }
    { print }
    END {
      if (!changed) {
        exit 2
      }
    }
  ' "$file" >"$tmp"
  rc=$?
  if [ "$rc" -eq 2 ]; then
    rm -f "$tmp"
    printf 'weston-cleanup-missing'
    return 0
  fi
  if [ "$rc" -ne 0 ]; then
    rm -f "$tmp"
    printf 'weston-cleanup-error'
    return 0
  fi
  mv "$tmp" "$file"
  chmod 755 "$file" 2>/dev/null || true
  printf 'weston-cleanup-patched'
}

normalize_godot_direct_sdl2_script() {
  file="$1"
  if ! is_godot_script "$file"; then
    printf 'not-godot'
    return 0
  fi
  if grep -q 'LEAF_PM_GODOT_DIRECT_SDL2=1' "$file" 2>/dev/null; then
    printf 'godot-direct-sdl2-already'
    return 0
  fi
  if ! grep -Eq '"\$GAMEDIR/runtime/\$runtime".*--main-pack' "$file" 2>/dev/null; then
    printf 'godot-direct-sdl2-missing'
    return 0
  fi

  tmp="$file.tmp.$$"
  awk '
    index($0, "\"$GAMEDIR/runtime/$runtime\"") > 0 &&
    index($0, "--main-pack") > 0 &&
    index($0, "leaf_pm_run_godot_sdl2_runtime") == 0 {
      indent = $0
      sub(/[^[:space:]].*$/, "", indent)
      cmd = substr($0, length(indent) + 1)
      patched = cmd
      sub(/"\$GAMEDIR\/runtime\/\$runtime"/,
          "leaf_pm_run_godot_sdl2_runtime \"$GAMEDIR/runtime/$runtime\"",
          patched)
      print indent "# LEAF_PM_GODOT_DIRECT_SDL2=1"
      print indent "if declare -f leaf_pm_run_godot_sdl2_runtime >/dev/null 2>&1; then"
      print indent "  " patched
      print indent "else"
      print indent "  " cmd
      print indent "fi"
      changed = 1
      next
    }
    { print }
    END {
      if (!changed) {
        exit 2
      }
    }
  ' "$file" >"$tmp"
  rc=$?
  if [ "$rc" -eq 2 ]; then
    rm -f "$tmp"
    printf 'godot-direct-sdl2-missing'
    return 0
  fi
  if [ "$rc" -ne 0 ]; then
    rm -f "$tmp"
    printf 'godot-direct-sdl2-error'
    return 0
  fi
  mv "$tmp" "$file"
  chmod 755 "$file" 2>/dev/null || true
  printf 'godot-direct-sdl2-patched'
}

normalize_godot_frt_sdl2_script() {
  file="$1"
  if ! is_godot_script "$file"; then
    printf 'not-godot'
    return 0
  fi
  if grep -q 'LEAF_PM_GODOT_FRT_SDL2=1' "$file" 2>/dev/null; then
    printf 'godot-frt-sdl2-already'
    return 0
  fi
  if ! grep -Eq 'runtime="?frt_[0-9]' "$file" 2>/dev/null ||
     ! grep -Eq '"\$runtime".*--main-pack' "$file" 2>/dev/null; then
    printf 'godot-frt-sdl2-missing'
    return 0
  fi

  tmp="$file.tmp.$$"
  awk '
    index($0, "umount \"$godot_file\"") > 0 {
      patched = $0
      sub(/"\$godot_file"/, "\"$godot_dir\"", patched)
      print patched
      changed = 1
      next
    }
    index($0, "\"$runtime\"") > 0 &&
    index($0, "--main-pack") > 0 &&
    index($0, "leaf_pm_run_godot_sdl2_runtime") == 0 {
      indent = $0
      sub(/[^[:space:]].*$/, "", indent)
      cmd = substr($0, length(indent) + 1)
      patched = cmd
      sub(/"\$runtime"/,
          "leaf_pm_run_godot_sdl2_runtime \"$runtime\"",
          patched)
      print indent "# LEAF_PM_GODOT_FRT_SDL2=1"
      print indent "if declare -f leaf_pm_run_godot_sdl2_runtime >/dev/null 2>&1; then"
      print indent "  " patched
      print indent "else"
      print indent "  " cmd
      print indent "fi"
      changed = 1
      next
    }
    { print }
    END {
      if (!changed) {
        exit 2
      }
    }
  ' "$file" >"$tmp"
  rc=$?
  if [ "$rc" -eq 2 ]; then
    rm -f "$tmp"
    printf 'godot-frt-sdl2-missing'
    return 0
  fi
  if [ "$rc" -ne 0 ]; then
    rm -f "$tmp"
    printf 'godot-frt-sdl2-error'
    return 0
  fi
  mv "$tmp" "$file"
  chmod 755 "$file" 2>/dev/null || true
  printf 'godot-frt-sdl2-patched'
}

normalize_sdl2_fullscreen_env_script() {
  file="$1"
  case "$file" in
    *.sh) ;;
    *) printf 'not-shell'; return 0 ;;
  esac
  if grep -q 'LEAF_PM_SDL2_FULLSCREEN_ENV_VERSION=1' "$file" 2>/dev/null; then
    printf 'sdl2-fullscreen-env-already'
    return 0
  fi

  port="$(script_port_dir "$file" || true)"
  if [ -z "$port" ]; then
    printf 'sdl2-fullscreen-env-no-gamedir'
    return 0
  fi
  if is_godot_script "$file" || is_ship_of_harkinian_script "$file" ||
     is_sdl2_fullscreen_optout_port "$port"; then
    printf 'sdl2-fullscreen-env-opted-out'
    return 0
  fi

  arch_csv="$(sdl2_arch_csv_for_port "$port")"
  if [ -z "$arch_csv" ]; then
    printf 'sdl2-fullscreen-env-not-sdl2'
    return 0
  fi

  missing_shim=0
  case ",$arch_csv," in
    *,aarch64,*)
      [ "$aarch64_sdl2_fullscreen_available" -eq 1 ] || missing_shim=1
      ;;
  esac
  case ",$arch_csv," in
    *,armhf,*)
      [ "$sdl2_fullscreen_available" -eq 1 ] || missing_shim=1
      ;;
  esac
  if [ "$missing_shim" -ne 0 ]; then
    printf 'sdl2-fullscreen-env-missing-shim'
    return 0
  fi

  tmp="$file.tmp.$$"
  port_arg="$(shell_quote "$port")"
  arch_arg="$(shell_quote "$arch_csv")"
  if awk -v port_arg="$port_arg" -v arch_arg="$arch_arg" '
    NR == FNR {
      if ($0 ~ /^[[:space:]]*get_controls([[:space:]]|$)/) {
        has_get_controls = 1
      }
      next
    }
    function insert_block() {
      print ""
      print "# LEAF_PM_SDL2_FULLSCREEN_ENV=1"
      print "# LEAF_PM_SDL2_FULLSCREEN_ENV_VERSION=1"
      print "if declare -f leaf_pm_enable_sdl2_fullscreen_env >/dev/null 2>&1; then"
      print "  leaf_pm_enable_sdl2_fullscreen_env " port_arg " " arch_arg
      print "fi"
      inserted = 1
      changed = 1
    }
    {
      print
      if (!inserted && has_get_controls && $0 ~ /^[[:space:]]*get_controls([[:space:]]|$)/) {
        insert_block()
      } else if (!inserted && !has_get_controls && $0 ~ /source[[:space:]].*control[.]txt/) {
        insert_block()
      }
    }
    END {
      if (!inserted) {
        exit 2
      }
    }
  ' "$file" "$file" >"$tmp"; then
    mv "$tmp" "$file"
    chmod 755 "$file" 2>/dev/null || true
    printf 'sdl2-fullscreen-env-patched'
    return 0
  else
    rc=$?
  fi

  rm -f "$tmp"
  if [ "$rc" -eq 2 ]; then
    printf 'sdl2-fullscreen-env-missing-anchor'
    return 0
  fi
  printf 'sdl2-fullscreen-env-error'
}

normalize_aarch64_compat_libs_script() {
  file="$1"
  case "$file" in
    *.sh) ;;
    *) printf 'not-shell'; return 0 ;;
  esac

  port="$(script_port_dir "$file" || true)"
  if [ -z "$port" ]; then
    printf 'aarch64-compat-libs-no-gamedir'
    return 0
  fi
  if is_aarch64_compat_libs_optout_port "$port"; then
    printf 'aarch64-compat-libs-opted-out'
    return 0
  fi
  if [ -z "${port_aarch64_compat_needed[$port]:-}" ]; then
    printf 'aarch64-compat-libs-not-needed'
    return 0
  fi
  if [ "$aarch64_compat_libs_available" -ne 1 ]; then
    printf 'aarch64-compat-libs-missing-pack'
    return 0
  fi
  if grep -q 'LEAF_PM_AARCH64_COMPAT_LIBS_VERSION=1' "$file" 2>/dev/null; then
    printf 'aarch64-compat-libs-already'
    return 0
  fi

  tmp="$file.tmp.$$"
  if awk '
    NR == FNR {
      if ($0 ~ /^[[:space:]]*get_controls([[:space:]]|$)/) {
        has_get_controls = 1
      }
      next
    }
    function insert_block() {
      print ""
      print "# LEAF_PM_AARCH64_COMPAT_LIBS=1"
      print "# LEAF_PM_AARCH64_COMPAT_LIBS_VERSION=1"
      print "if declare -f leaf_pm_enable_aarch64_compat_libs >/dev/null 2>&1; then"
      print "  leaf_pm_enable_aarch64_compat_libs"
      print "fi"
      inserted = 1
      changed = 1
    }
    {
      print
      if (!inserted && has_get_controls && $0 ~ /^[[:space:]]*get_controls([[:space:]]|$)/) {
        insert_block()
      } else if (!inserted && !has_get_controls && $0 ~ /source[[:space:]].*control[.]txt/) {
        insert_block()
      }
    }
    END {
      if (!inserted) {
        exit 2
      }
    }
  ' "$file" "$file" >"$tmp"; then
    mv "$tmp" "$file"
    chmod 755 "$file" 2>/dev/null || true
    printf 'aarch64-compat-libs-patched'
    return 0
  else
    rc=$?
  fi

  rm -f "$tmp"
  if [ "$rc" -eq 2 ]; then
    printf 'aarch64-compat-libs-missing-anchor'
    return 0
  fi
  printf 'aarch64-compat-libs-error'
}

runtime_compat_gothic_machismo_vulkan_rotate_script() {
  file="$1"
  if ! is_gothic_machismo_script "$file"; then
    printf 'not-gothic-machismo'
    return 0
  fi
  if grep -q 'LEAF_PM_RUNTIME_COMPAT_GOTHIC_MACHISMO_VULKAN_ROTATE_VERSION=1' "$file" 2>/dev/null &&
     grep -q 'LEAF_DRM_ROTATE="${LEAF_DRM_ROTATE:-}"' "$file" 2>/dev/null &&
     grep -q 'leaf_pm_begin_gothic_machismo_vulkan_rotate' "$file" 2>/dev/null &&
     ! grep -q 'LEAF_PM_RUNTIME_COMPAT_GOTHIC_MACHISMO_GLES=1' "$file" 2>/dev/null &&
     ! grep -q 'LEAF_PM_MINA_GLES=1' "$file" 2>/dev/null; then
    printf 'runtime-compat-gothic-machismo-vulkan-rotate-already'
    return 0
  fi

  tmp="$file.tmp.$$"
  awk '
    NR == FNR {
      if ($0 ~ /LEAF_PM_RUNTIME_COMPAT_GOTHIC_MACHISMO_VULKAN_ROTATE_VERSION=1/) {
        has_block = 1
      }
      if ($0 ~ /^[[:space:]]*LEAF_DRM_ROTATE="\$\{LEAF_DRM_ROTATE:-\}"[[:space:]]*\\[[:space:]]*$/) {
        has_env_forward = 1
      }
      if ($0 ~ /leaf_pm_begin_gothic_machismo_vulkan_rotate/) {
        has_begin = 1
      }
      if ($0 ~ /^[[:space:]]*"\$\{SDLVID_ARG\[@\]\}"[[:space:]]*\\[[:space:]]*$/) {
        has_sdlvid_arg = 1
      }
      next
    }
    function insert_block() {
      print ""
      print "# LEAF_PM_RUNTIME_COMPAT_GOTHIC_MACHISMO_VULKAN_ROTATE=1"
      print "# LEAF_PM_RUNTIME_COMPAT_GOTHIC_MACHISMO_VULKAN_ROTATE_VERSION=1"
      print "# MLP1 stock Vulkan direct-display needs the Leaf DRM rotate shim; fall back to GLES when it is unavailable."
      print "# Pre-set GOTHIC_BACKEND=gles or LEAF_PM_GOTHIC_MACHISMO_VULKAN_ROTATE=0 to opt out."
      print "if declare -f leaf_pm_enable_gothic_machismo_vulkan_rotate >/dev/null 2>&1 &&"
      print "   leaf_pm_enable_gothic_machismo_vulkan_rotate; then"
      print "  :"
      print "else"
      print "  export GOTHIC_BACKEND=\"${GOTHIC_BACKEND:-gles}\""
      print "fi"
      inserted = 1
      changed = 1
    }
    function insert_begin(indent) {
      print indent "# LEAF_PM_GOTHIC_MACHISMO_VULKAN_ROTATE_BEGIN=1"
      print indent "if declare -f leaf_pm_begin_gothic_machismo_vulkan_rotate >/dev/null 2>&1; then"
      print indent "  leaf_pm_begin_gothic_machismo_vulkan_rotate"
      print indent "fi"
      begin_inserted = 1
      changed = 1
    }
    {
      if (skip_old_gles) {
        if ($0 ~ /^[[:space:]]*$/) {
          skip_old_gles = 0
        }
        changed = 1
        next
      }
      if (skip_old_mina) {
        if ($0 ~ /^[[:space:]]*fi[[:space:]]*$/) {
          skip_old_mina = 0
          skip_following_blank = 1
        }
        changed = 1
        next
      }
      if (skip_following_blank && $0 ~ /^[[:space:]]*$/) {
        skip_following_blank = 0
        changed = 1
        next
      }
      skip_following_blank = 0
      if ($0 ~ /# LEAF_PM_RUNTIME_COMPAT_GOTHIC_MACHISMO_GLES=1/) {
        skip_old_gles = 1
        changed = 1
        next
      }
      if ($0 ~ /# LEAF_PM_MINA_GLES=1/) {
        skip_old_mina = 1
        changed = 1
        next
      }
      if (!has_begin && !begin_inserted &&
          ($0 ~ /[$]GPTOKEYB[[:space:]].*machismo.*&/ ||
           $0 ~ /pm_platform_helper[[:space:]].*machismo/ ||
           $0 ~ /^[[:space:]]*[$]ESUDO[[:space:]]+env[[:space:]]*\\/)) {
        indent = $0
        sub(/[^[:space:]].*$/, "", indent)
        insert_begin(indent)
        print ""
      }
      print
      if (!has_block && !inserted && $0 ~ /^[[:space:]]*get_controls([[:space:]]|$)/) {
        insert_block()
      } else if (!has_block && !inserted && $0 ~ /source[[:space:]].*control[.]txt/) {
        insert_block()
      }
      if (!has_env_forward && !forwarded &&
          ((has_sdlvid_arg && $0 ~ /^[[:space:]]*"\$\{SDLVID_ARG\[@\]\}"[[:space:]]*\\[[:space:]]*$/) ||
           (!has_sdlvid_arg && $0 ~ /^[[:space:]]*GOTHIC_BACKEND="\$GOTHIC_BACKEND"[[:space:]]*\\[[:space:]]*$/))) {
        indent = $0
        sub(/[^[:space:]].*$/, "", indent)
        print indent "LEAF_DRM_ROTATE=\"${LEAF_DRM_ROTATE:-}\" \\"
        print indent "LEAF_DRM_ROTATE_DEBUG=\"${LEAF_DRM_ROTATE_DEBUG:-}\" \\"
        print indent "LEAF_DRM_ROTATE_DUMP=\"${LEAF_DRM_ROTATE_DUMP:-}\" \\"
        print indent "LEAF_DRM_ROTATE_DUMP_FRAME=\"${LEAF_DRM_ROTATE_DUMP_FRAME:-}\" \\"
        print indent "LD_PRELOAD=\"${LD_PRELOAD:-}\" \\"
        print indent "SDL_VIDEODRIVER=\"${SDL_VIDEODRIVER:-}\" \\"
        print indent "VK_ICD_FILENAMES=\"${VK_ICD_FILENAMES:-}\" \\"
        print indent "VK_LOADER_LAYERS_DISABLE=\"${VK_LOADER_LAYERS_DISABLE:-}\" \\"
        forwarded = 1
        changed = 1
      }
    }
    END {
      if (!has_block && !inserted) {
        insert_block()
      }
    }
  ' "$file" "$file" >"$tmp"
  mv "$tmp" "$file"
  chmod 755 "$file" 2>/dev/null || true
  printf 'runtime-compat-gothic-machismo-vulkan-rotate-patched'
}

runtime_compat_soh_display_script() {
  file="$1"
  if ! is_ship_of_harkinian_script "$file"; then
    printf 'not-soh'
    return 0
  fi
  if grep -q 'LEAF_PM_RUNTIME_COMPAT_SOH_DISPLAY_VERSION=1' "$file" 2>/dev/null; then
    printf 'runtime-compat-soh-display-already'
    return 0
  fi

  tmp="$file.tmp.$$"
  block="$file.leaf-soh-display.$$"
  cat >"$block" <<'LEAF_PM_SOH_DISPLAY_BLOCK'

# LEAF_PM_RUNTIME_COMPAT_SOH_DISPLAY=1
# LEAF_PM_RUNTIME_COMPAT_SOH_DISPLAY_VERSION=1
# Normalize Ship of Harkinian's window state for MLP1's 960x720 landscape UI.
leaf_pm_soh_configure_mlp1_display() {
    [ "${LEAF_PM_SOH_DISPLAY_NORMALIZE:-1}" != "0" ] || return 0
    [ -n "${GAMEDIR:-}" ] || return 0

    _leaf_pm_soh_w="${DISPLAY_WIDTH:-960}"
    _leaf_pm_soh_h="${DISPLAY_HEIGHT:-720}"
    case "$_leaf_pm_soh_w" in *[!0-9]*|"") _leaf_pm_soh_w=960 ;; esac
    case "$_leaf_pm_soh_h" in *[!0-9]*|"") _leaf_pm_soh_h=720 ;; esac

    if [ -d "$GAMEDIR/baseroms" ]; then
        for _leaf_pm_soh_o2r in oot.o2r oot-mq.o2r; do
            if [ ! -f "$GAMEDIR/$_leaf_pm_soh_o2r" ] && [ -f "$GAMEDIR/baseroms/$_leaf_pm_soh_o2r" ]; then
                cp -f "$GAMEDIR/baseroms/$_leaf_pm_soh_o2r" "$GAMEDIR/$_leaf_pm_soh_o2r" 2>/dev/null || true
            fi
        done
    fi

    _leaf_pm_soh_python="$(command -v python3 2>/dev/null || true)"
    if [ -z "$_leaf_pm_soh_python" ]; then
        echo "Leaf PortMaster: python3 unavailable; skipping SoH display normalization" >&2
        unset _leaf_pm_soh_w _leaf_pm_soh_h _leaf_pm_soh_o2r _leaf_pm_soh_python
        return 0
    fi

    if ! LEAF_PM_SOH_GAME_DIR="$GAMEDIR" LEAF_PM_SOH_WIDTH="$_leaf_pm_soh_w" LEAF_PM_SOH_HEIGHT="$_leaf_pm_soh_h" "$_leaf_pm_soh_python" <<'LEAF_PM_SOH_CONFIG'
import json
import os
import re
import sys
from pathlib import Path


def positive_int(name, fallback):
    try:
        value = int(os.environ.get(name, "") or fallback)
    except ValueError:
        return fallback
    return value if value > 0 else fallback


def atomic_write(path, text):
    tmp = path.with_name(path.name + ".leaf-tmp")
    tmp.write_text(text, encoding="utf-8")
    tmp.replace(path)


game_dir = Path(os.environ.get("LEAF_PM_SOH_GAME_DIR", "."))
width = positive_int("LEAF_PM_SOH_WIDTH", 960)
height = positive_int("LEAF_PM_SOH_HEIGHT", 720)

config_path = game_dir / "shipofharkinian.json"
data = None
if config_path.exists():
    try:
        data = json.loads(config_path.read_text(encoding="utf-8"))
    except Exception as exc:
        print("Leaf PortMaster: could not parse {}: {}".format(config_path, exc), file=sys.stderr)
else:
    data = {}

if isinstance(data, dict):
    cvars = data.setdefault("CVars", {})
    if not isinstance(cvars, dict):
        cvars = {}
        data["CVars"] = cvars
    settings = cvars.setdefault("gSettings", {})
    if not isinstance(settings, dict):
        settings = {}
        cvars["gSettings"] = settings
    settings["SdlWindowedFullscreen"] = 1

    window = data.setdefault("Window", {})
    if not isinstance(window, dict):
        window = {}
        data["Window"] = window
    fullscreen = window.setdefault("Fullscreen", {})
    if not isinstance(fullscreen, dict):
        fullscreen = {}
        window["Fullscreen"] = fullscreen
    window["Width"] = width
    window["Height"] = height
    fullscreen["Enabled"] = 1
    fullscreen["Width"] = width
    fullscreen["Height"] = height
    atomic_write(config_path, json.dumps(data, indent=4) + "\n")

imgui_path = game_dir / "imgui.ini"
try:
    lines = imgui_path.read_text(encoding="utf-8").splitlines() if imgui_path.exists() else []
except Exception as exc:
    print("Leaf PortMaster: could not read {}: {}".format(imgui_path, exc), file=sys.stderr)
    lines = []

main_headers = ("[Window][Main Game]", "[Window][Main - Deck]")


def normalize_window_section(header, section):
    rest = []
    for line in section[1:]:
        if line.startswith("Pos=") or line.startswith("Size="):
            continue
        rest.append(line)
    return [header, "Pos=0,0", "Size={},{}".format(width, height)] + rest


def default_window_section(header):
    rest = ["Collapsed=0"]
    if header == "[Window][Main Game]":
        rest.append("DockId=0x4C2ED000")
    return [header, "Pos=0,0", "Size={},{}".format(width, height)] + rest


out = []
seen = set()
has_dockspace = False
i = 0
while i < len(lines):
    line = lines[i]
    if line in main_headers:
        section = [line]
        i += 1
        while i < len(lines) and not lines[i].startswith("["):
            section.append(lines[i])
            i += 1
        out.extend(normalize_window_section(line, section))
        seen.add(line)
        continue
    if line.startswith("DockSpace "):
        has_dockspace = True
        line = re.sub(r" Pos=[0-9]+,[0-9]+", " Pos=0,0", line)
        if " Pos=" not in line:
            line += " Pos=0,0"
        line = re.sub(r" Size=[0-9]+,[0-9]+", " Size={},{}".format(width, height), line)
        if " Size=" not in line:
            line += " Size={},{}".format(width, height)
    out.append(line)
    i += 1

for header in main_headers:
    if header not in seen:
        if out and out[-1] != "":
            out.append("")
        out.extend(default_window_section(header))

if not has_dockspace:
    if out and out[-1] != "":
        out.append("")
    if "[Docking][Data]" not in out:
        out.append("[Docking][Data]")
    out.append(
        "DockSpace ID=0x4C2ED000 Window=0xD2FD9B6B Pos=0,0 "
        "Size={},{} CentralNode=1 NoTabBar=1 Selected=0x360E12CF".format(width, height)
    )

atomic_write(imgui_path, "\n".join(out).rstrip() + "\n")
LEAF_PM_SOH_CONFIG
    then
        echo "Leaf PortMaster: SoH display normalization failed" >&2
    fi

    unset _leaf_pm_soh_w _leaf_pm_soh_h _leaf_pm_soh_o2r _leaf_pm_soh_python
}
LEAF_PM_SOH_DISPLAY_BLOCK

  if awk '
    NR == FNR {
      block[++block_n] = $0
      next
    }
    function insert_block(i) {
      for (i = 1; i <= block_n; i++) {
        print block[i]
      }
      inserted = 1
      changed = 1
    }
    function print_call(indent) {
      print indent "leaf_pm_soh_configure_mlp1_display"
      pre_called = 1
      changed = 1
    }
    {
      if (!inserted && $0 ~ /^[[:space:]]*# --------------------- END FUNCTIONS ---------------------/) {
        insert_block()
      } else if (!inserted && $0 ~ /^[[:space:]]*# Perform functions/) {
        insert_block()
      }
      if (!pre_called && $0 ~ /^[[:space:]]*otr_check([[:space:]]|$)/) {
        indent = $0
        sub(/[^[:space:]].*$/, "", indent)
        print_call(indent)
      } else if (!pre_called && $0 ~ /^[[:space:]]*# Run the game/) {
        print_call("")
      }
      print
      if (!post_called && $0 ~ /^[[:space:]]*imgui_reset([[:space:]]|$)/) {
        indent = $0
        sub(/[^[:space:]].*$/, "", indent)
        print indent "leaf_pm_soh_configure_mlp1_display"
        post_called = 1
        changed = 1
      }
    }
    END {
      if (!inserted || !pre_called || !changed) {
        exit 2
      }
    }
  ' "$block" "$file" >"$tmp"; then
    rm -f "$block"
    mv "$tmp" "$file"
    chmod 755 "$file" 2>/dev/null || true
    printf 'runtime-compat-soh-display-patched'
    return 0
  else
    rc=$?
  fi

  rm -f "$tmp" "$block"
  if [ "$rc" -eq 2 ]; then
    printf 'runtime-compat-soh-display-missing-anchor'
    return 0
  fi
  printf 'runtime-compat-soh-display-error'
}

runtime_compat_love_11_5_libs_script() {
  file="$1"
  if ! is_love_runtime_script "$file"; then
    printf 'not-love-runtime'
    return 0
  fi
  if grep -q 'LEAF_PM_RUNTIME_COMPAT_LOVE_11_5_LIBS_VERSION=1' "$file" 2>/dev/null; then
    printf 'runtime-compat-love-11-5-libs-already'
    return 0
  fi

  tmp="$file.tmp.$$"
  if awk '
    NR == FNR {
      if ($0 ~ /^[[:space:]]*export[[:space:]]+LD_LIBRARY_PATH=/) {
        has_ld_library_path = 1
      }
      if ($0 ~ /^[[:space:]]*get_controls([[:space:]]|$)/ ||
          $0 ~ /source[[:space:]].*control[.]txt/) {
        has_control_anchor = 1
      }
      next
    }
    function insert_block() {
      print ""
      print "# LEAF_PM_RUNTIME_COMPAT_LOVE_11_5_LIBS=1"
      print "# LEAF_PM_RUNTIME_COMPAT_LOVE_11_5_LIBS_VERSION=1"
      print "_leaf_pm_love_control=\"${PORTMASTER_CONTROLFOLDER:-${controlfolder:-}}\""
      print "_leaf_pm_love_libs=\"\""
      print "if [ -n \"$_leaf_pm_love_control\" ]; then"
      print "  _leaf_pm_love_libs=\"$_leaf_pm_love_control/runtimes/love_11.5/libs.aarch64\""
      print "fi"
      print "if [ -d \"$_leaf_pm_love_libs\" ]; then"
      print "  case \":${LD_LIBRARY_PATH:-}:\" in"
      print "    *:\"$_leaf_pm_love_libs\":*) ;;"
      print "    *) export LD_LIBRARY_PATH=\"${LD_LIBRARY_PATH:+$LD_LIBRARY_PATH:}$_leaf_pm_love_libs\" ;;"
      print "  esac"
      print "fi"
      print "unset _leaf_pm_love_control _leaf_pm_love_libs"
      inserted = 1
      changed = 1
    }
    function is_love_launch(line) {
      return line ~ /^[[:space:]]*(env[[:space:]].*)?(\.\/)?love([[:space:]]|$)/
    }
    {
      if (!inserted && !has_ld_library_path && !has_control_anchor && is_love_launch($0)) {
        insert_block()
      }
      print
      if (!inserted && has_ld_library_path && $0 ~ /^[[:space:]]*export[[:space:]]+LD_LIBRARY_PATH=/) {
        insert_block()
      } else if (!inserted && !has_ld_library_path && $0 ~ /^[[:space:]]*get_controls([[:space:]]|$)/) {
        insert_block()
      } else if (!inserted && !has_ld_library_path && $0 ~ /source[[:space:]].*control[.]txt/) {
        insert_block()
      }
    }
    END {
      if (!inserted || !changed) {
        exit 2
      }
    }
  ' "$file" "$file" >"$tmp"; then
    mv "$tmp" "$file"
    chmod 755 "$file" 2>/dev/null || true
    printf 'runtime-compat-love-11-5-libs-patched'
    return 0
  else
    rc=$?
  fi

  rm -f "$tmp"
  if [ "$rc" -eq 2 ]; then
    printf 'runtime-compat-love-11-5-libs-missing-anchor'
    return 0
  fi
  printf 'runtime-compat-love-11-5-libs-error'
}

normalize_lowercase_path_moves_script() {
  file="$1"
  case "$file" in
    *.sh) ;;
    *) return 0 ;;
  esac
  if grep -q 'LEAF_PM_LOWERCASE_BASENAME_MOVE_VERSION=3' "$file" 2>/dev/null; then
    printf 'lowercase-path-move-already'
    return 0
  fi
  if ! grep -F 'lowercase_file=$(echo "$file" | tr' "$file" >/dev/null 2>&1 &&
     ! grep -Eq 'LEAF_PM_LOWERCASE_BASENAME_MOVE_VERSION=[12]' "$file" 2>/dev/null; then
    return 0
  fi

  tmp="$file.tmp.$$"
  if awk '
    {
      if (skip_lowercase_if) {
        if ($0 ~ /^[[:space:]]*fi[[:space:]]*$/) {
          skip_lowercase_if = 0
        }
        next
      }
      line = $0
      if (index(line, "lowercase_file=$(echo \"$file\" | tr") > 0 &&
          index(line, "[:upper:]") > 0 &&
          index(line, "[:lower:]") > 0) {
        indent = $0
        sub(/[^[:space:]].*$/, "", indent)
        print indent "# LEAF_PM_LOWERCASE_BASENAME_MOVE=1"
        print indent "# LEAF_PM_LOWERCASE_BASENAME_MOVE_VERSION=3"
        print indent "lowercase_file=\"$(dirname \"$file\")/$(basename \"$file\" | tr \"[:upper:]\" \"[:lower:]\")\""
        changed = 1
        next
      }
      if (line ~ /# LEAF_PM_LOWERCASE_BASENAME_MOVE_VERSION=[12]/) {
        sub(/LEAF_PM_LOWERCASE_BASENAME_MOVE_VERSION=[12]/, "LEAF_PM_LOWERCASE_BASENAME_MOVE_VERSION=3", line)
        print line
        changed = 1
        next
      }
      if (index(line, "if [ \"$file\" != \"$lowercase_file\" ]; then") > 0 ||
          index(line, "if [ \"$file\" != \"$lowercase_file\" ] && [ ! -e \"$lowercase_file\" ]; then") > 0) {
        indent = $0
        sub(/[^[:space:]].*$/, "", indent)
        print indent "if [ \"$file\" != \"$lowercase_file\" ]; then"
        print indent "    if [ -e \"$lowercase_file\" ]; then"
        print indent "        :"
        print indent "    else"
        print indent "        _leaf_pm_lowercase_tmp=\"$(dirname \"$file\")/.leaf-lowercase-$(basename \"$file\").$$\""
        print indent "        while [ -e \"$_leaf_pm_lowercase_tmp\" ]; do"
        print indent "            _leaf_pm_lowercase_tmp=\"${_leaf_pm_lowercase_tmp}.x\""
        print indent "        done"
        print indent "        if mv \"$file\" \"$_leaf_pm_lowercase_tmp\"; then"
        print indent "            mv \"$_leaf_pm_lowercase_tmp\" \"$lowercase_file\" || mv \"$_leaf_pm_lowercase_tmp\" \"$file\""
        print indent "        fi"
        print indent "        unset _leaf_pm_lowercase_tmp"
        print indent "    fi"
        print indent "fi"
        skip_lowercase_if = 1
        changed = 1
        next
      }
      print
    }
    END {
      if (!changed) {
        exit 2
      }
    }
  ' "$file" >"$tmp"; then
    mv "$tmp" "$file"
    chmod 755 "$file" 2>/dev/null || true
    printf 'lowercase-path-move-patched'
    return 0
  else
    rc=$?
  fi

  rm -f "$tmp"
  if [ "$rc" -eq 2 ]; then
    return 0
  fi
  printf 'lowercase-path-move-error'
}

apply_runtime_compat_rules_script() {
  file="$1"
  case "$file" in
    *.sh) ;;
    *) return 0 ;;
  esac

  normalize_lowercase_path_moves_script "$file"
  printf '\n'
  if is_gothic_machismo_script "$file"; then
    runtime_compat_gothic_machismo_vulkan_rotate_script "$file"
    printf '\n'
  fi
  if is_ship_of_harkinian_script "$file"; then
    runtime_compat_soh_display_script "$file"
    printf '\n'
  fi
  if is_love_runtime_script "$file"; then
    runtime_compat_love_11_5_libs_script "$file"
    printf '\n'
  fi
}

find_shell_script_candidates() {
  find "$ports_dir" -type f -name '*.sh' ! -path '*/.leaf-armhf/*' -print0
}

find_elf_candidates() {
  if [ "$scan_mode" = "full" ]; then
    find "$ports_dir" -type f ! -path '*/.leaf-armhf/*' -print0
    return 0
  fi

  ports_root="${ports_dir%/}"
  find "$ports_dir" -type f ! -path '*/.leaf-armhf/*' ! -name '*.sh' \
    \( \
      \( -path "$ports_root/*/*" ! -path "$ports_root/*/*/*" \) -o \
      -path '*/bin/*' -o \
      -path '*/box86/*' -o \
      -path '*/libs/*' -o \
      -path '*/lib/*' -o \
      -name '*.so' -o \
      -name '*.so.*' \
    \) -print0
}

normalize_armhf_executable() {
  file="$1"
  [ "$compat_available" -eq 1 ] || {
    printf 'needs-wrapper-compat-missing'
    return 0
  }

  original="$(wrapper_path_for "$file")"
  mkdir -p "$(dirname "$original")"
  if [ -f "$original" ]; then
    if is_leaf_armhf_wrapper "$file" && ! is_leaf_armhf_wrapper_v3 "$file"; then
      write_armhf_wrapper "$file" "$original"
      printf 'rewrapped'
      return 0
    fi
    printf 'already-normalized'
    return 0
  fi

  mv "$file" "$original"
  write_armhf_wrapper "$file" "$original"
  printf 'wrapped'
}

is_truthy() {
  case "$1" in
    1|true|yes|TRUE|YES) return 0 ;;
    *) return 1 ;;
  esac
}

path_has_manifest_unsafe_chars() {
  case "$1" in
    *$'\t'*|*$'\n'*) return 0 ;;
    *) return 1 ;;
  esac
}

stat_cmd=""
stat_style="none"
if command -v gstat >/dev/null 2>&1; then
  stat_cmd="$(command -v gstat)"
  stat_style="gnu"
elif stat -c '%s' "$ports_dir" >/dev/null 2>&1; then
  stat_cmd="stat"
  stat_style="gnu"
elif stat -f '%z' "$ports_dir" >/dev/null 2>&1; then
  stat_cmd="stat"
  stat_style="bsd"
fi

declare -A file_size=()
declare -A file_mtime=()
declare -A manifest_type=()
declare -A manifest_size=()
declare -A manifest_mtime=()
declare -A manifest_kind=()
declare -A manifest_interpreter=()
declare -A manifest_sha=()
declare -A manifest_action=()
declare -A manifest_sdl2_tag=()
declare -A manifest_needed_tag=()
declare -A manifest_outcomes=()
declare -A manifest_script_sdl2_tag=()
declare -A port_sdl2_aarch64=()
declare -A port_sdl2_armhf=()
declare -A aarch64_compat_sonames=()
declare -A port_lib_index_loaded=()
declare -A port_lib_index=()
declare -A port_aarch64_compat_needed=()
declare -A port_aarch64_unresolved=()
declare -A aarch64_compat_sonames_seen=()
declare -A aarch64_unresolved_sonames_seen=()

load_aarch64_compat_sonames

stat_one_line() {
  local file="$1"
  case "$stat_style" in
    gnu) "$stat_cmd" -c $'%s\t%Y\t%n' "$file" 2>/dev/null ;;
    bsd) "$stat_cmd" -f $'%z\t%m\t%N' "$file" 2>/dev/null ;;
    *) return 1 ;;
  esac
}

stat_candidate_list() {
  local list_file="$1"
  local out_file="$2"
  : >"$out_file"
  [ "$stat_style" != "none" ] || return 0
  [ -s "$list_file" ] || return 0

  if xargs -0 printf '' </dev/null >/dev/null 2>&1; then
    case "$stat_style" in
      gnu) xargs -0 "$stat_cmd" -c $'%s\t%Y\t%n' <"$list_file" >"$out_file" 2>/dev/null || true ;;
      bsd) xargs -0 "$stat_cmd" -f $'%z\t%m\t%N' <"$list_file" >"$out_file" 2>/dev/null || true ;;
    esac
    return 0
  fi

  local file
  while IFS= read -r -d '' file; do
    stat_one_line "$file" >>"$out_file" || true
  done <"$list_file"
}

load_stat_file() {
  local stat_file="$1"
  local line size rest mtime path tab
  tab=$'\t'
  while IFS= read -r line; do
    case "$line" in
      *"$tab"*"$tab"*) ;;
      *) continue ;;
    esac
    size="${line%%${tab}*}"
    rest="${line#*${tab}}"
    mtime="${rest%%${tab}*}"
    path="${rest#*${tab}}"
    case "$size:$mtime" in
      *[!0-9:]*|:|:*|*:|'') continue ;;
    esac
    file_size["$path"]="$size"
    file_mtime["$path"]="$mtime"
  done <"$stat_file"
}

refresh_file_stat() {
  local file="$1"
  local line size rest mtime path tab
  tab=$'\t'
  line="$(stat_one_line "$file" || true)"
  case "$line" in
    *"$tab"*"$tab"*) ;;
    *) return 1 ;;
  esac
  size="${line%%${tab}*}"
  rest="${line#*${tab}}"
  mtime="${rest%%${tab}*}"
  path="${rest#*${tab}}"
  case "$size:$mtime" in
    *[!0-9:]*|:|:*|*:|'') return 1 ;;
  esac
  file_size["$path"]="$size"
  file_mtime["$path"]="$mtime"
}

load_manifest() {
  local magic key_line line type size mtime kind interpreter sha action sdl2_tag needed_tag outcomes path extra tab
  tab=$'\t'
  [ -f "$manifest_path" ] || return 1
  {
    IFS= read -r magic || return 1
    IFS= read -r key_line || return 1
    [ "$magic" = "#leaf-armhf-scan-manifest${tab}3" ] || return 1
    [ "$key_line" = "#key${tab}$manifest_key" ] || return 1
    while IFS= read -r line; do
      [ -n "$line" ] || continue
      IFS=$'\t' read -r type size mtime kind interpreter sha action sdl2_tag needed_tag path extra <<<"$line"
      if [ "$type" = "E" ]; then
        [ -z "${extra:-}" ] || return 1
        [ -n "$path" ] || return 1
        case "$size:$mtime" in
          *[!0-9:]*|:|:*|*:|'') return 1 ;;
        esac
        path_has_manifest_unsafe_chars "$path" && return 1
        [ "$kind" = "-" ] && kind=""
        [ "$interpreter" = "-" ] && interpreter=""
        [ "$sha" = "-" ] && sha=""
        [ "$action" = "-" ] && action=""
        [ "$sdl2_tag" = "-" ] && sdl2_tag=""
        [ "$needed_tag" = "-" ] && needed_tag=""
        manifest_type["$path"]="E"
        manifest_size["$path"]="$size"
        manifest_mtime["$path"]="$mtime"
        manifest_kind["$path"]="$kind"
        manifest_interpreter["$path"]="$interpreter"
        manifest_sha["$path"]="$sha"
        manifest_action["$path"]="$action"
        manifest_sdl2_tag["$path"]="$sdl2_tag"
        manifest_needed_tag["$path"]="$needed_tag"
        continue
      fi

      IFS=$'\t' read -r type size mtime outcomes sdl2_tag path extra <<<"$line"
      if [ "$type" = "S" ]; then
        [ -z "${extra:-}" ] || return 1
        [ -n "$path" ] || return 1
        case "$size:$mtime" in
          *[!0-9:]*|:|:*|*:|'') return 1 ;;
        esac
        path_has_manifest_unsafe_chars "$path" && return 1
        [ "$outcomes" = "-" ] && outcomes=""
        [ "$sdl2_tag" = "-" ] && sdl2_tag=""
        manifest_type["$path"]="S"
        manifest_size["$path"]="$size"
        manifest_mtime["$path"]="$mtime"
        manifest_outcomes["$path"]="$outcomes"
        manifest_script_sdl2_tag["$path"]="$sdl2_tag"
        continue
      fi
      return 1
    done
  } <"$manifest_path"
}

can_use_manifest_entry() {
  local file="$1"
  local type="$2"
  local script_sdl2_tag="${3:-}"
  path_has_manifest_unsafe_chars "$file" && return 1
  [ "${manifest_type[$file]:-}" = "$type" ] || return 1
  [ -n "${file_size[$file]+x}" ] || return 1
  [ "${manifest_size[$file]:-}" = "${file_size[$file]}" ] || return 1
  [ "${manifest_mtime[$file]:-}" = "${file_mtime[$file]}" ] || return 1
  if [ "$type" = "S" ]; then
    [ "${manifest_script_sdl2_tag[$file]:-}" = "$script_sdl2_tag" ] || return 1
  fi
}

manifest_write_header() {
  [ "$manifest_write_enabled" -eq 1 ] || return 0
  printf '#leaf-armhf-scan-manifest\t3\n' >"$manifest_tmp"
  printf '#key\t%s\n' "$manifest_key" >>"$manifest_tmp"
}

manifest_record_script() {
  local file="$1"
  local outcomes="$2"
  local sdl2_tag="${3:-}"
  local field_outcomes="$outcomes"
  local field_sdl2_tag="$sdl2_tag"
  [ "$manifest_write_enabled" -eq 1 ] || return 0
  path_has_manifest_unsafe_chars "$file" && return 0
  [ -n "${file_size[$file]+x}" ] || return 0
  [ -n "$field_outcomes" ] || field_outcomes="-"
  [ -n "$field_sdl2_tag" ] || field_sdl2_tag="-"
  printf 'S\t%s\t%s\t%s\t%s\t%s\n' \
    "${file_size[$file]}" \
    "${file_mtime[$file]}" \
    "$field_outcomes" \
    "$field_sdl2_tag" \
    "$file" >>"$manifest_tmp"
}

manifest_record_elf() {
  local file="$1"
  local kind="$2"
  local interpreter="$3"
  local sha="$4"
  local action="$5"
  local sdl2_tag="${6:-}"
  local needed_tag="${7:-}"
  local field_kind="$kind"
  local field_interpreter="$interpreter"
  local field_sha="$sha"
  local field_action="$action"
  local field_sdl2_tag="$sdl2_tag"
  local field_needed_tag="$needed_tag"
  [ "$manifest_write_enabled" -eq 1 ] || return 0
  path_has_manifest_unsafe_chars "$file" && return 0
  [ -n "${file_size[$file]+x}" ] || return 0
  [ -n "$field_kind" ] || field_kind="-"
  [ -n "$field_interpreter" ] || field_interpreter="-"
  [ -n "$field_sha" ] || field_sha="-"
  [ -n "$field_action" ] || field_action="-"
  [ -n "$field_sdl2_tag" ] || field_sdl2_tag="-"
  [ -n "$field_needed_tag" ] || field_needed_tag="-"
  printf 'E\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "${file_size[$file]}" \
    "${file_mtime[$file]}" \
    "$field_kind" \
    "$field_interpreter" \
    "$field_sha" \
    "$field_action" \
    "$field_sdl2_tag" \
    "$field_needed_tag" \
    "$file" >>"$manifest_tmp"
}

tag_port_sdl2_arch() {
  local port="$1"
  local arch="$2"
  [ -n "$port" ] || return 0
  case "$arch" in
    aarch64)
      if [ -z "${port_sdl2_aarch64[$port]+x}" ]; then
        port_sdl2_aarch64["$port"]=1
        sdl2_fullscreen_ports_aarch64=$((sdl2_fullscreen_ports_aarch64 + 1))
      fi
      ;;
    armhf)
      if [ -z "${port_sdl2_armhf[$port]+x}" ]; then
        port_sdl2_armhf["$port"]=1
        sdl2_fullscreen_ports_armhf=$((sdl2_fullscreen_ports_armhf + 1))
      fi
      ;;
  esac
}

tag_file_sdl2_arch() {
  local file="$1"
  local arch="$2"
  local port
  port="$(port_dir_for "$file" || true)"
  [ -n "$port" ] || return 0
  tag_port_sdl2_arch "$port" "$arch"
}

apply_manifest_sdl2_tag() {
  local file="$1"
  local tag="$2"
  case "$tag" in
    sdl2-aarch64) tag_file_sdl2_arch "$file" "aarch64" ;;
    sdl2-armhf) tag_file_sdl2_arch "$file" "armhf" ;;
  esac
}

append_csv_action() {
  local action="$1"
  [ -n "$action" ] || return 0
  if [ -n "$script_cache_outcomes" ]; then
    script_cache_outcomes="$script_cache_outcomes,$action"
  else
    script_cache_outcomes="$action"
  fi
}

script_cache_action() {
  case "$1" in
    port-env-patched) printf 'port-env-already' ;;
    port-env-already) printf 'port-env-already' ;;
    port-paths-patched) printf 'port-paths-already' ;;
    port-paths-already) printf 'port-paths-already' ;;
    libretro-retroarch-patched) printf 'libretro-retroarch-already' ;;
    libretro-retroarch-already) printf 'libretro-retroarch-already' ;;
    libretro-retroarch-missing) printf 'libretro-retroarch-missing' ;;
    godot-wayland-patched) printf 'godot-wayland-already' ;;
    godot-wayland-already) printf 'godot-wayland-already' ;;
    godot-wayland-runtime-patched) printf 'godot-wayland-runtime-already' ;;
    godot-wayland-runtime-already) printf 'godot-wayland-runtime-already' ;;
    weston-cleanup-patched) printf 'weston-cleanup-already' ;;
    weston-cleanup-already) printf 'weston-cleanup-already' ;;
    weston-cleanup-missing) printf 'weston-cleanup-missing' ;;
    godot-direct-sdl2-patched) printf 'godot-direct-sdl2-already' ;;
    godot-direct-sdl2-already) printf 'godot-direct-sdl2-already' ;;
    godot-direct-sdl2-missing) printf 'godot-direct-sdl2-missing' ;;
    godot-frt-sdl2-patched) printf 'godot-frt-sdl2-already' ;;
    godot-frt-sdl2-already) printf 'godot-frt-sdl2-already' ;;
    godot-frt-sdl2-missing) printf 'godot-frt-sdl2-missing' ;;
    sdl2-fullscreen-env-patched) printf 'sdl2-fullscreen-env-already' ;;
    sdl2-fullscreen-env-already) printf 'sdl2-fullscreen-env-already' ;;
    sdl2-fullscreen-env-not-sdl2) printf 'sdl2-fullscreen-env-not-sdl2' ;;
    sdl2-fullscreen-env-no-gamedir) printf 'sdl2-fullscreen-env-no-gamedir' ;;
    sdl2-fullscreen-env-missing-shim) printf 'sdl2-fullscreen-env-missing-shim' ;;
    sdl2-fullscreen-env-opted-out) printf 'sdl2-fullscreen-env-opted-out' ;;
    aarch64-compat-libs-patched) printf 'aarch64-compat-libs-already' ;;
    aarch64-compat-libs-already) printf 'aarch64-compat-libs-already' ;;
    aarch64-compat-libs-not-needed) printf 'aarch64-compat-libs-not-needed' ;;
    aarch64-compat-libs-no-gamedir) printf 'aarch64-compat-libs-no-gamedir' ;;
    aarch64-compat-libs-opted-out) printf 'aarch64-compat-libs-opted-out' ;;
    aarch64-compat-libs-missing-pack) printf 'aarch64-compat-libs-missing-pack' ;;
    runtime-compat-gothic-machismo-vulkan-rotate-patched) printf 'runtime-compat-gothic-machismo-vulkan-rotate-already' ;;
    runtime-compat-gothic-machismo-vulkan-rotate-already) printf 'runtime-compat-gothic-machismo-vulkan-rotate-already' ;;
    runtime-compat-soh-display-patched) printf 'runtime-compat-soh-display-already' ;;
    runtime-compat-soh-display-already) printf 'runtime-compat-soh-display-already' ;;
    runtime-compat-love-11-5-libs-patched) printf 'runtime-compat-love-11-5-libs-already' ;;
    runtime-compat-love-11-5-libs-already) printf 'runtime-compat-love-11-5-libs-already' ;;
    lowercase-path-move-patched) printf 'lowercase-path-move-already' ;;
    lowercase-path-move-already) printf 'lowercase-path-move-already' ;;
    runtime-compat-soh-display-missing-anchor|runtime-compat-soh-display-error|runtime-compat-love-11-5-libs-missing-anchor|runtime-compat-love-11-5-libs-error|lowercase-path-move-error|godot-wayland-runtime-missing-anchor|godot-wayland-runtime-error|weston-cleanup-error|godot-direct-sdl2-error|godot-frt-sdl2-error|sdl2-fullscreen-env-missing-anchor|sdl2-fullscreen-env-error|aarch64-compat-libs-missing-anchor|aarch64-compat-libs-error)
      return 1
      ;;
    *) printf '' ;;
  esac
}

record_script_action() {
  case "$1" in
    port-env-patched) port_env_patched=$((port_env_patched + 1)) ;;
    port-env-already) port_env_already=$((port_env_already + 1)) ;;
    port-paths-patched) port_paths_patched=$((port_paths_patched + 1)) ;;
    port-paths-already) port_paths_already=$((port_paths_already + 1)) ;;
    libretro-retroarch-patched) libretro_retroarch_patched=$((libretro_retroarch_patched + 1)) ;;
    libretro-retroarch-already) libretro_retroarch_already=$((libretro_retroarch_already + 1)) ;;
    libretro-retroarch-missing) libretro_retroarch_missing=$((libretro_retroarch_missing + 1)) ;;
    godot-wayland-patched) godot_patched=$((godot_patched + 1)) ;;
    godot-wayland-already) godot_already=$((godot_already + 1)) ;;
    godot-wayland-runtime-patched) godot_wayland_runtime_patched=$((godot_wayland_runtime_patched + 1)) ;;
    godot-wayland-runtime-already) godot_wayland_runtime_already=$((godot_wayland_runtime_already + 1)) ;;
    godot-wayland-runtime-missing-anchor)
      godot_wayland_runtime_missing_anchor=$((godot_wayland_runtime_missing_anchor + 1))
      errors=$((errors + 1))
      ;;
    godot-wayland-runtime-error)
      godot_wayland_runtime_errors=$((godot_wayland_runtime_errors + 1))
      errors=$((errors + 1))
      ;;
    weston-cleanup-patched) weston_cleanup_patched=$((weston_cleanup_patched + 1)) ;;
    weston-cleanup-already) weston_cleanup_already=$((weston_cleanup_already + 1)) ;;
    weston-cleanup-missing) weston_cleanup_missing=$((weston_cleanup_missing + 1)) ;;
    godot-direct-sdl2-patched) godot_direct_sdl2_patched=$((godot_direct_sdl2_patched + 1)) ;;
    godot-direct-sdl2-already) godot_direct_sdl2_already=$((godot_direct_sdl2_already + 1)) ;;
    godot-direct-sdl2-missing) godot_direct_sdl2_missing=$((godot_direct_sdl2_missing + 1)) ;;
    godot-frt-sdl2-patched) godot_frt_sdl2_patched=$((godot_frt_sdl2_patched + 1)) ;;
    godot-frt-sdl2-already) godot_frt_sdl2_already=$((godot_frt_sdl2_already + 1)) ;;
    godot-frt-sdl2-missing) godot_frt_sdl2_missing=$((godot_frt_sdl2_missing + 1)) ;;
    godot-frt-sdl2-error)
      godot_frt_sdl2_errors=$((godot_frt_sdl2_errors + 1))
      errors=$((errors + 1))
      ;;
    sdl2-fullscreen-env-patched) sdl2_fullscreen_env_patched=$((sdl2_fullscreen_env_patched + 1)) ;;
    sdl2-fullscreen-env-already) sdl2_fullscreen_env_already=$((sdl2_fullscreen_env_already + 1)) ;;
    sdl2-fullscreen-env-not-sdl2) sdl2_fullscreen_env_not_sdl2=$((sdl2_fullscreen_env_not_sdl2 + 1)) ;;
    sdl2-fullscreen-env-no-gamedir) sdl2_fullscreen_env_no_gamedir=$((sdl2_fullscreen_env_no_gamedir + 1)) ;;
    sdl2-fullscreen-env-missing-shim) sdl2_fullscreen_env_missing_shim=$((sdl2_fullscreen_env_missing_shim + 1)) ;;
    sdl2-fullscreen-env-opted-out) sdl2_fullscreen_env_opted_out=$((sdl2_fullscreen_env_opted_out + 1)) ;;
    sdl2-fullscreen-env-missing-anchor)
      sdl2_fullscreen_env_missing_anchor=$((sdl2_fullscreen_env_missing_anchor + 1))
      errors=$((errors + 1))
      ;;
    sdl2-fullscreen-env-error)
      sdl2_fullscreen_env_errors=$((sdl2_fullscreen_env_errors + 1))
      errors=$((errors + 1))
      ;;
    aarch64-compat-libs-patched) aarch64_compat_libs_patched=$((aarch64_compat_libs_patched + 1)) ;;
    aarch64-compat-libs-already) aarch64_compat_libs_already=$((aarch64_compat_libs_already + 1)) ;;
    aarch64-compat-libs-not-needed) aarch64_compat_libs_not_needed=$((aarch64_compat_libs_not_needed + 1)) ;;
    aarch64-compat-libs-no-gamedir) aarch64_compat_libs_no_gamedir=$((aarch64_compat_libs_no_gamedir + 1)) ;;
    aarch64-compat-libs-opted-out) aarch64_compat_libs_opted_out=$((aarch64_compat_libs_opted_out + 1)) ;;
    aarch64-compat-libs-missing-pack) aarch64_compat_libs_missing_pack=$((aarch64_compat_libs_missing_pack + 1)) ;;
    aarch64-compat-libs-missing-anchor)
      aarch64_compat_libs_missing_anchor=$((aarch64_compat_libs_missing_anchor + 1))
      errors=$((errors + 1))
      ;;
    aarch64-compat-libs-error)
      aarch64_compat_libs_errors=$((aarch64_compat_libs_errors + 1))
      errors=$((errors + 1))
      ;;
    runtime-compat-gothic-machismo-vulkan-rotate-patched)
      runtime_compat_gothic_machismo_vulkan_rotate_patched=$((runtime_compat_gothic_machismo_vulkan_rotate_patched + 1))
      ;;
    runtime-compat-gothic-machismo-vulkan-rotate-already)
      runtime_compat_gothic_machismo_vulkan_rotate_already=$((runtime_compat_gothic_machismo_vulkan_rotate_already + 1))
      ;;
    runtime-compat-soh-display-patched)
      runtime_compat_soh_display_patched=$((runtime_compat_soh_display_patched + 1))
      ;;
    runtime-compat-soh-display-already)
      runtime_compat_soh_display_already=$((runtime_compat_soh_display_already + 1))
      ;;
    runtime-compat-soh-display-missing-anchor)
      runtime_compat_soh_display_missing_anchor=$((runtime_compat_soh_display_missing_anchor + 1))
      errors=$((errors + 1))
      ;;
    runtime-compat-soh-display-error)
      runtime_compat_soh_display_errors=$((runtime_compat_soh_display_errors + 1))
      errors=$((errors + 1))
      ;;
    runtime-compat-love-11-5-libs-patched)
      runtime_compat_love_11_5_libs_patched=$((runtime_compat_love_11_5_libs_patched + 1))
      ;;
    runtime-compat-love-11-5-libs-already)
      runtime_compat_love_11_5_libs_already=$((runtime_compat_love_11_5_libs_already + 1))
      ;;
    runtime-compat-love-11-5-libs-missing-anchor)
      runtime_compat_love_11_5_libs_missing_anchor=$((runtime_compat_love_11_5_libs_missing_anchor + 1))
      errors=$((errors + 1))
      ;;
    runtime-compat-love-11-5-libs-error)
      runtime_compat_love_11_5_libs_errors=$((runtime_compat_love_11_5_libs_errors + 1))
      errors=$((errors + 1))
      ;;
    lowercase-path-move-patched)
      lowercase_path_move_patched=$((lowercase_path_move_patched + 1))
      ;;
    lowercase-path-move-already)
      lowercase_path_move_already=$((lowercase_path_move_already + 1))
      ;;
    lowercase-path-move-error)
      lowercase_path_move_errors=$((lowercase_path_move_errors + 1))
      errors=$((errors + 1))
      ;;
  esac
}

handle_script_action() {
  local action="$1"
  local cache_action
  [ -n "$action" ] || return 0
  record_script_action "$action"
  if cache_action="$(script_cache_action "$action")"; then
    append_csv_action "$cache_action"
  else
    script_cacheable=0
  fi
}

replay_script_outcomes() {
  local outcomes="$1"
  local action rest
  [ -n "$outcomes" ] || return 0
  rest="$outcomes"
  while [ -n "$rest" ]; do
    action="${rest%%,*}"
    if [ "$rest" = "$action" ]; then
      rest=""
    else
      rest="${rest#*,}"
    fi
    record_script_action "$action"
  done
}

replay_elf_manifest_entry() {
  local file="$1"
  local kind="${manifest_kind[$file]:-}"
  local interpreter="${manifest_interpreter[$file]:-}"
  local sha="${manifest_sha[$file]:-}"
  local action="${manifest_action[$file]:-}"
  local sdl2_tag="${manifest_sdl2_tag[$file]:-}"
  local needed_tag="${manifest_needed_tag[$file]:-}"

  apply_manifest_sdl2_tag "$file" "$sdl2_tag"
  apply_manifest_needed_tag "$file" "$needed_tag"

  case "$action" in
    ignored)
      if [ -n "$kind" ]; then
        aarch64_elfs_seen=$((aarch64_elfs_seen + 1))
      fi
      ;;
    wrapper-present)
      already=$((already + 1))
      printf '%s\t%s\t%s\t%s\t%s\n' "$file" "${kind:-wrapper}" "" "$sha" "$action" >>"$tmp_records"
      ;;
    shared-object)
      seen=$((seen + 1))
      shared=$((shared + 1))
      printf '%s\t%s\t%s\t%s\t%s\n' "$file" "$kind" "$interpreter" "$sha" "$action" >>"$tmp_records"
      ;;
    needs-wrapper-compat-missing)
      seen=$((seen + 1))
      needs_wrapper=$((needs_wrapper + 1))
      printf '%s\t%s\t%s\t%s\t%s\n' "$file" "$kind" "$interpreter" "$sha" "$action" >>"$tmp_records"
      ;;
    already-normalized)
      seen=$((seen + 1))
      already=$((already + 1))
      printf '%s\t%s\t%s\t%s\t%s\n' "$file" "$kind" "$interpreter" "$sha" "$action" >>"$tmp_records"
      ;;
    observed)
      seen=$((seen + 1))
      printf '%s\t%s\t%s\t%s\t%s\n' "$file" "$kind" "$interpreter" "$sha" "$action" >>"$tmp_records"
      ;;
  esac
}

write_leaf_hook
patch_control_txt

manifest_read_enabled=1
manifest_write_enabled=1
cache_state="cold"
if [ "$scan_mode" = "full" ]; then
  manifest_read_enabled=0
  manifest_write_enabled=0
  cache_state="disabled"
elif is_truthy "$scan_no_cache"; then
  manifest_read_enabled=0
  cache_state="disabled"
elif [ "$stat_style" = "none" ]; then
  manifest_read_enabled=0
  manifest_write_enabled=0
  cache_state="disabled"
fi

if [ "$manifest_read_enabled" -eq 1 ]; then
  if load_manifest; then
    cache_state="warm"
  else
    manifest_type=()
    manifest_size=()
    manifest_mtime=()
    manifest_kind=()
    manifest_interpreter=()
    manifest_sha=()
    manifest_action=()
    manifest_sdl2_tag=()
    manifest_needed_tag=()
    manifest_outcomes=()
    manifest_script_sdl2_tag=()
    cache_state="cold"
  fi
fi

scan_tmp_dir="$leaf_dir/scan-tmp.$$"
tmp_records="$report_tsv.tmp.$$"
tmp_json="$report_json.tmp.$$"
manifest_tmp=""
cleanup_scan_tmp() {
  rm -rf "$scan_tmp_dir"
  rm -f "$tmp_records" "$tmp_json"
  if [ -n "$manifest_tmp" ]; then
    rm -f "$manifest_tmp"
  fi
}
trap cleanup_scan_tmp EXIT

mkdir -p "$scan_tmp_dir"
script_candidates="$scan_tmp_dir/scripts.list"
elf_candidates="$scan_tmp_dir/elfs.list"
script_stats="$scan_tmp_dir/scripts.stats"
elf_stats="$scan_tmp_dir/elfs.stats"
large_file_records="$scan_tmp_dir/large-files.tsv"

find_shell_script_candidates >"$script_candidates"
find_elf_candidates >"$elf_candidates"
if [ "$manifest_read_enabled" -eq 1 ] || [ "$manifest_write_enabled" -eq 1 ]; then
  stat_candidate_list "$script_candidates" "$script_stats"
  stat_candidate_list "$elf_candidates" "$elf_stats"
  load_stat_file "$script_stats"
  load_stat_file "$elf_stats"
fi

if [ "$manifest_write_enabled" -eq 1 ]; then
  manifest_tmp="$manifest_path.tmp.$$"
  manifest_write_header
fi

printf 'path\tkind\tinterpreter\tsha256\taction\n' >"$tmp_records"

seen=0
aarch64_elfs_seen=0
shell_scripts_seen=0
files_skipped=0
files_processed=0
wrapped=0
shared=0
needs_wrapper=0
already=0
errors=0
godot_patched=0
godot_already=0
godot_wayland_runtime_patched=0
godot_wayland_runtime_already=0
godot_wayland_runtime_missing_anchor=0
godot_wayland_runtime_errors=0
godot_direct_sdl2_patched=0
godot_direct_sdl2_already=0
godot_direct_sdl2_missing=0
godot_frt_sdl2_patched=0
godot_frt_sdl2_already=0
godot_frt_sdl2_missing=0
godot_frt_sdl2_errors=0
sdl2_fullscreen_env_patched=0
sdl2_fullscreen_env_already=0
sdl2_fullscreen_env_not_sdl2=0
sdl2_fullscreen_env_no_gamedir=0
sdl2_fullscreen_env_missing_shim=0
sdl2_fullscreen_env_opted_out=0
sdl2_fullscreen_env_missing_anchor=0
sdl2_fullscreen_env_errors=0
aarch64_compat_libs_patched=0
aarch64_compat_libs_already=0
aarch64_compat_libs_not_needed=0
aarch64_compat_libs_no_gamedir=0
aarch64_compat_libs_opted_out=0
aarch64_compat_libs_missing_pack=0
aarch64_compat_libs_missing_anchor=0
aarch64_compat_libs_errors=0
sdl2_fullscreen_ports_aarch64=0
sdl2_fullscreen_ports_armhf=0
weston_cleanup_patched=0
weston_cleanup_already=0
weston_cleanup_missing=0
port_env_patched=0
port_env_already=0
port_paths_patched=0
port_paths_already=0
libretro_retroarch_patched=0
libretro_retroarch_already=0
libretro_retroarch_missing=0
runtime_compat_gothic_machismo_vulkan_rotate_patched=0
runtime_compat_gothic_machismo_vulkan_rotate_already=0
runtime_compat_soh_display_patched=0
runtime_compat_soh_display_already=0
runtime_compat_soh_display_missing_anchor=0
runtime_compat_soh_display_errors=0
runtime_compat_love_11_5_libs_patched=0
runtime_compat_love_11_5_libs_already=0
runtime_compat_love_11_5_libs_missing_anchor=0
runtime_compat_love_11_5_libs_errors=0
lowercase_path_move_patched=0
lowercase_path_move_already=0
lowercase_path_move_errors=0

while IFS= read -r -d '' file; do
  if can_use_manifest_entry "$file" "E"; then
    files_skipped=$((files_skipped + 1))
    replay_elf_manifest_entry "$file"
    manifest_record_elf \
      "$file" \
      "${manifest_kind[$file]:-}" \
      "${manifest_interpreter[$file]:-}" \
      "${manifest_sha[$file]:-}" \
      "${manifest_action[$file]:-}" \
      "${manifest_sdl2_tag[$file]:-}" \
      "${manifest_needed_tag[$file]:-}"
    continue
  fi

  files_processed=$((files_processed + 1))
  header="$(elf_header_hex "$file")"
  if ! is_armhf_elf "$header"; then
    if [ "${header:0:4}" = "2321" ] && is_leaf_armhf_wrapper "$file"; then
      action="wrapper-present"
      original="$(wrapper_path_for "$file")"
      sdl2_tag="-"
      needed_tag="-"
      if [ -f "$original" ]; then
        original_header="$(elf_header_hex "$original")"
        if is_armhf_elf "$original_header" && elf_links_sdl2 "$original"; then
          tag_file_sdl2_arch "$file" "armhf"
          sdl2_tag="sdl2-armhf"
        fi
      fi
      if [ "$compat_available" -eq 1 ] &&
         [ -f "$original" ] &&
         ! is_leaf_armhf_wrapper_v3 "$file"; then
        write_armhf_wrapper "$file" "$original"
        wrapped=$((wrapped + 1))
        action="rewrapped"
      else
        already=$((already + 1))
      fi
      sha="$(file_sha256 "$file")"
      printf '%s\t%s\t%s\t%s\t%s\n' "$file" "wrapper" "" "$sha" "$action" >>"$tmp_records"
      refresh_file_stat "$file" || true
      manifest_record_elf "$file" "wrapper" "" "$sha" "wrapper-present" "$sdl2_tag" "$needed_tag"
    else
      kind=""
      sdl2_tag="-"
      needed_tag="-"
      if is_aarch64_elf "$header"; then
        aarch64_elfs_seen=$((aarch64_elfs_seen + 1))
        kind="$(elf_kind "$header")"
        if elf_links_sdl2 "$file"; then
          tag_file_sdl2_arch "$file" "aarch64"
          sdl2_tag="sdl2-aarch64"
        fi
        needed_tag="$(aarch64_needed_tag_for_file "$file")"
        apply_manifest_needed_tag "$file" "$needed_tag"
      fi
      manifest_record_elf "$file" "$kind" "" "" "ignored" "$sdl2_tag" "$needed_tag"
    fi
    continue
  fi

  seen=$((seen + 1))
  kind="$(elf_kind "$header")"
  interpreter="$(armhf_interpreter "$file")"
  sha="$(file_sha256 "$file")"
  action="observed"
  sdl2_tag="-"
  if elf_links_sdl2 "$file"; then
    tag_file_sdl2_arch "$file" "armhf"
    sdl2_tag="sdl2-armhf"
  fi

  if [ -n "$interpreter" ]; then
    if action="$(normalize_armhf_executable "$file")"; then
      case "$action" in
        wrapped|rewrapped) wrapped=$((wrapped + 1)) ;;
        already-normalized) already=$((already + 1)) ;;
        needs-wrapper-compat-missing) needs_wrapper=$((needs_wrapper + 1)) ;;
      esac
    else
      action="error"
      errors=$((errors + 1))
    fi
  else
    shared=$((shared + 1))
    action="shared-object"
  fi

  printf '%s\t%s\t%s\t%s\t%s\n' "$file" "$kind" "$interpreter" "$sha" "$action" >>"$tmp_records"

  case "$action" in
    wrapped|rewrapped)
      refresh_file_stat "$file" || true
      post_sha="$(file_sha256 "$file")"
      manifest_record_elf "$file" "wrapper" "" "$post_sha" "wrapper-present" "$sdl2_tag" "-"
      ;;
    error)
      ;;
    *)
      refresh_file_stat "$file" || true
      manifest_record_elf "$file" "$kind" "$interpreter" "$sha" "$action" "$sdl2_tag" "-"
      ;;
  esac
done <"$elf_candidates"

while IFS= read -r -d '' file; do
  shell_scripts_seen=$((shell_scripts_seen + 1))
  current_script_sdl2_tag="$(script_sdl2_cache_tag "$file")"

  if can_use_manifest_entry "$file" "S" "$current_script_sdl2_tag"; then
    files_skipped=$((files_skipped + 1))
    replay_script_outcomes "${manifest_outcomes[$file]:-}"
    manifest_record_script "$file" "${manifest_outcomes[$file]:-}" "$current_script_sdl2_tag"
    continue
  fi

  files_processed=$((files_processed + 1))
  script_cache_outcomes=""
  script_cacheable=1

  handle_script_action "$(normalize_port_env_script "$file")"
  handle_script_action "$(normalize_port_paths_script "$file")"
  handle_script_action "$(normalize_libretro_retroarch_script "$file")"
  handle_script_action "$(normalize_godot_wayland_script "$file")"
  handle_script_action "$(normalize_godot_wayland_runtime_script "$file")"
  handle_script_action "$(normalize_godot_weston_cleanup_script "$file")"
  handle_script_action "$(normalize_godot_direct_sdl2_script "$file")"
  handle_script_action "$(normalize_godot_frt_sdl2_script "$file")"
  handle_script_action "$(normalize_sdl2_fullscreen_env_script "$file")"
  handle_script_action "$(normalize_aarch64_compat_libs_script "$file")"

  while IFS= read -r runtime_compat_action; do
    handle_script_action "$runtime_compat_action"
  done < <(apply_runtime_compat_rules_script "$file")

  if [ "$script_cacheable" -eq 1 ]; then
    refresh_file_stat "$file" || true
    current_script_sdl2_tag="$(script_sdl2_cache_tag "$file")"
    manifest_record_script "$file" "$script_cache_outcomes" "$current_script_sdl2_tag"
  fi
done <"$script_candidates"

sdl2_fullscreen_ports_both=0
for sdl2_port in "${!port_sdl2_aarch64[@]}"; do
  if [ -n "${port_sdl2_armhf[$sdl2_port]+x}" ]; then
    sdl2_fullscreen_ports_both=$((sdl2_fullscreen_ports_both + 1))
  fi
done
collect_large_files "$large_file_records"

mv "$tmp_records" "$report_tsv"

cat >"$tmp_json" <<EOF
{
  "schema": 1,
  "platform": "$(json_escape "$platform")",
  "ports_dir": "$(json_escape "$ports_dir")",
  "scan_mode": "$(json_escape "$scan_mode")",
  "controlfolder": "$(json_escape "$controlfolder")",
  "compat_root": "$(json_escape "$compat_root")",
  "compat_available": $([ "$compat_available" -eq 1 ] && printf 'true' || printf 'false'),
  "sdl2_fullscreen_shim": "$(json_escape "$sdl2_fullscreen_shim")",
  "sdl2_fullscreen_available": $([ "$sdl2_fullscreen_available" -eq 1 ] && printf 'true' || printf 'false'),
  "aarch64_sdl2_fullscreen_shim": "$(json_escape "$aarch64_sdl2_fullscreen_shim")",
  "aarch64_sdl2_fullscreen_available": $([ "$aarch64_sdl2_fullscreen_available" -eq 1 ] && printf 'true' || printf 'false'),
  "aarch64_drm_rotate_shim": "$(json_escape "$aarch64_drm_rotate_shim")",
  "aarch64_drm_rotate_available": $([ "$aarch64_drm_rotate_available" -eq 1 ] && printf 'true' || printf 'false'),
  "aarch64_compat_lib_dir": "$(json_escape "$aarch64_compat_lib_dir")",
  "aarch64_compat_libs_available": $([ "$aarch64_compat_libs_available" -eq 1 ] && printf 'true' || printf 'false'),
  "aarch64_compat_lib_sonames_available": ${#aarch64_compat_sonames[@]},
  "aarch64_compat_libs_optout": "$(json_escape "$aarch64_compat_optout")",
  "sdl2_fullscreen_ports_aarch64": $sdl2_fullscreen_ports_aarch64,
  "sdl2_fullscreen_ports_armhf": $sdl2_fullscreen_ports_armhf,
  "sdl2_fullscreen_ports_both": $sdl2_fullscreen_ports_both,
  "hook": "$(json_escape "$hook_path")",
  "records_tsv": "$(json_escape "$report_tsv")",
  "manifest": "$(json_escape "$manifest_path")",
  "large_file_threshold_bytes": $large_file_threshold_bytes,
  "large_file_count": $large_file_count,
  "files_skipped": $files_skipped,
  "files_processed": $files_processed,
  "cache": "$(json_escape "$cache_state")",
  "aarch64_elfs_seen": $aarch64_elfs_seen,
  "aarch64_compat_lib_ports": ${#port_aarch64_compat_needed[@]},
  "aarch64_compat_lib_sonames_seen": ${#aarch64_compat_sonames_seen[@]},
  "aarch64_unresolved_soname_ports": ${#port_aarch64_unresolved[@]},
  "aarch64_unresolved_sonames_seen": ${#aarch64_unresolved_sonames_seen[@]},
  "armhf_elfs_seen": $seen,
  "armhf_execs_wrapped": $wrapped,
  "armhf_execs_already_normalized": $already,
  "armhf_execs_needing_compat": $needs_wrapper,
  "armhf_shared_objects_seen": $shared,
  "shell_scripts_seen": $shell_scripts_seen,
  "port_env_scripts_patched": $port_env_patched,
  "port_env_scripts_already_patched": $port_env_already,
  "port_path_scripts_patched": $port_paths_patched,
  "port_path_scripts_already_patched": $port_paths_already,
  "libretro_retroarch_scripts_patched": $libretro_retroarch_patched,
  "libretro_retroarch_scripts_already_patched": $libretro_retroarch_already,
  "libretro_retroarch_scripts_missing": $libretro_retroarch_missing,
  "godot_wayland_scripts_patched": $godot_patched,
  "godot_wayland_scripts_already_patched": $godot_already,
  "godot_wayland_runtime_scripts_patched": $godot_wayland_runtime_patched,
  "godot_wayland_runtime_scripts_already_patched": $godot_wayland_runtime_already,
  "godot_wayland_runtime_scripts_missing_anchor": $godot_wayland_runtime_missing_anchor,
  "godot_wayland_runtime_script_errors": $godot_wayland_runtime_errors,
  "godot_direct_sdl2_scripts_patched": $godot_direct_sdl2_patched,
  "godot_direct_sdl2_scripts_already_patched": $godot_direct_sdl2_already,
  "godot_direct_sdl2_scripts_missing": $godot_direct_sdl2_missing,
  "godot_frt_sdl2_scripts_patched": $godot_frt_sdl2_patched,
  "godot_frt_sdl2_scripts_already_patched": $godot_frt_sdl2_already,
  "godot_frt_sdl2_scripts_missing": $godot_frt_sdl2_missing,
  "godot_frt_sdl2_script_errors": $godot_frt_sdl2_errors,
  "sdl2_fullscreen_env_scripts_patched": $sdl2_fullscreen_env_patched,
  "sdl2_fullscreen_env_scripts_already_patched": $sdl2_fullscreen_env_already,
  "sdl2_fullscreen_env_scripts_not_sdl2": $sdl2_fullscreen_env_not_sdl2,
  "sdl2_fullscreen_env_scripts_no_gamedir": $sdl2_fullscreen_env_no_gamedir,
  "sdl2_fullscreen_env_scripts_missing_shim": $sdl2_fullscreen_env_missing_shim,
  "sdl2_fullscreen_env_scripts_opted_out": $sdl2_fullscreen_env_opted_out,
  "sdl2_fullscreen_env_scripts_missing_anchor": $sdl2_fullscreen_env_missing_anchor,
  "sdl2_fullscreen_env_script_errors": $sdl2_fullscreen_env_errors,
  "aarch64_compat_lib_scripts_patched": $aarch64_compat_libs_patched,
  "aarch64_compat_lib_scripts_already_patched": $aarch64_compat_libs_already,
  "aarch64_compat_lib_scripts_not_needed": $aarch64_compat_libs_not_needed,
  "aarch64_compat_lib_scripts_no_gamedir": $aarch64_compat_libs_no_gamedir,
  "aarch64_compat_lib_scripts_opted_out": $aarch64_compat_libs_opted_out,
  "aarch64_compat_lib_scripts_missing_pack": $aarch64_compat_libs_missing_pack,
  "aarch64_compat_lib_scripts_missing_anchor": $aarch64_compat_libs_missing_anchor,
  "aarch64_compat_lib_script_errors": $aarch64_compat_libs_errors,
  "godot_weston_cleanup_scripts_patched": $weston_cleanup_patched,
  "godot_weston_cleanup_scripts_already_patched": $weston_cleanup_already,
  "godot_weston_cleanup_scripts_missing_cleanup": $weston_cleanup_missing,
  "runtime_compat_gothic_machismo_vulkan_rotate_scripts_patched": $runtime_compat_gothic_machismo_vulkan_rotate_patched,
  "runtime_compat_gothic_machismo_vulkan_rotate_scripts_already_patched": $runtime_compat_gothic_machismo_vulkan_rotate_already,
  "runtime_compat_soh_display_scripts_patched": $runtime_compat_soh_display_patched,
  "runtime_compat_soh_display_scripts_already_patched": $runtime_compat_soh_display_already,
  "runtime_compat_soh_display_scripts_missing_anchor": $runtime_compat_soh_display_missing_anchor,
  "runtime_compat_soh_display_script_errors": $runtime_compat_soh_display_errors,
  "runtime_compat_love_11_5_libs_scripts_patched": $runtime_compat_love_11_5_libs_patched,
  "runtime_compat_love_11_5_libs_scripts_already_patched": $runtime_compat_love_11_5_libs_already,
  "runtime_compat_love_11_5_libs_scripts_missing_anchor": $runtime_compat_love_11_5_libs_missing_anchor,
  "runtime_compat_love_11_5_libs_script_errors": $runtime_compat_love_11_5_libs_errors,
  "lowercase_path_move_scripts_patched": $lowercase_path_move_patched,
  "lowercase_path_move_scripts_already_patched": $lowercase_path_move_already,
  "lowercase_path_move_script_errors": $lowercase_path_move_errors,
  "godot_egl_scripts_patched": $godot_patched,
  "godot_egl_scripts_already_patched": $godot_already,
$(json_large_file_array)
$(json_port_soname_array "aarch64_compat_lib_port_sonames" "compat")
$(json_port_soname_array "unresolved_sonames" "unresolved")
  "readelf_available": $(command -v readelf >/dev/null 2>&1 && printf 'true' || printf 'false'),
  "errors": $errors
}
EOF
mv "$tmp_json" "$report_json"

if [ "$manifest_write_enabled" -eq 1 ]; then
  mv "$manifest_tmp" "$manifest_path"
fi

log "mode=$scan_mode scripts=$shell_scripts_seen seen=$seen aarch64=$aarch64_elfs_seen wrapped=$wrapped shared=$shared needs_compat=$needs_wrapper skipped=$files_skipped processed=$files_processed cache=$cache_state port_env_patched=$port_env_patched port_paths_patched=$port_paths_patched libretro_retroarch_patched=$libretro_retroarch_patched godot_patched=$godot_patched godot_wayland_runtime_patched=$godot_wayland_runtime_patched godot_direct_sdl2_patched=$godot_direct_sdl2_patched godot_frt_sdl2_patched=$godot_frt_sdl2_patched sdl2_fullscreen_env_patched=$sdl2_fullscreen_env_patched sdl2_fullscreen_env_already=$sdl2_fullscreen_env_already sdl2_fullscreen_env_missing_shim=$sdl2_fullscreen_env_missing_shim sdl2_fullscreen_env_errors=$sdl2_fullscreen_env_errors sdl2_ports_aarch64=$sdl2_fullscreen_ports_aarch64 sdl2_ports_armhf=$sdl2_fullscreen_ports_armhf aarch64_compat_ports=${#port_aarch64_compat_needed[@]} aarch64_compat_patched=$aarch64_compat_libs_patched aarch64_unresolved_ports=${#port_aarch64_unresolved[@]} weston_cleanup_patched=$weston_cleanup_patched runtime_compat_gothic_machismo_vulkan_rotate_patched=$runtime_compat_gothic_machismo_vulkan_rotate_patched runtime_compat_soh_display_patched=$runtime_compat_soh_display_patched runtime_compat_love_11_5_libs_patched=$runtime_compat_love_11_5_libs_patched lowercase_path_move_patched=$lowercase_path_move_patched report=$report_tsv"
