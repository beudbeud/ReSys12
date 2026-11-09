#!/bin/sh
# Inputs of tlcs_jit_test: for each MEM(P3) set, a RAM dump of its sub CPU and the addresses it ran (interpreted blocks,
# a coin, a start, then mashed buttons), into $OUT (default tests/out/tlcs). Needs the x86_64 core and RetroArch.
#   tests/tlcs_inputs.sh [sets]       then: obj/x86_64/tests/tlcs_jit_test $OUT/<set>.ram $(cat $OUT/<set>.base) $OUT/<set>.pcs
HERE=$(cd "$(dirname "$0")" && pwd)
ROMS=${RS10_ROMS:-$HOME/system10}
OUT=${OUT:-$HERE/out/tlcs}
mkdir -p "$OUT"
cfg="$OUT/ra.cfg"
printf 'video_driver = "null"\naudio_driver = "null"\nvideo_vsync = "false"\naudio_sync = "false"\ninput_driver = "null"\ninput_joypad_driver = "null"\nsavefile_directory = "%s"\nsavestate_directory = "%s"\n' "$OUT" "$OUT" > "$cfg"
# (MEM(P3) sets: the sub CPU's RAM at 0x800000; EXIO sets: 0x000000-0x007fff at 0)
for set in ${@:-g13jnr nicetsuk squizchs gahaha gahaha2 ptblank3 nflclsfb}; do
	RS10_TLCS_NOJIT=1 RS10_TLCS_PROF="$OUT/$set.pcs" RS12_DUMP_DIR="$OUT" RS12_RAMDUMP=2399 \
	RS12_INPUT="900:20000000:0,910:0:0,960:20000000:0,970:0:0,1100:80:0,1110:0:0,1300:80:0,1310:0:0,1500:30:0,1510:0:0,1700:fffff:0" \
		${RETROARCH:-retroarch} --appendconfig "$cfg" -L "$HERE/../resys12_libretro.so" "$ROMS/$set.zip" --max-frames=2400 >/dev/null 2>&1
	if [ -f "$OUT/p3ram_002399.bin" ]; then mv "$OUT/p3ram_002399.bin" "$OUT/$set.ram"; echo 0x800000 > "$OUT/$set.base"
	else mv "$OUT/exioram_002399.bin" "$OUT/$set.ram"; echo 0 > "$OUT/$set.base"; fi
	rm -f "$OUT/ram_002399.bin" "$OUT"/*.ppm
	echo "$set: $(wc -l < "$OUT/$set.pcs") addresses"
done
