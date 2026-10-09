#!/bin/zsh
# Unattended System Link match: PS5 hosts, Mac searches and joins.
#   [PS5_CVARS="--cvar a=b ..."] netplay/run_pair.sh NAME [seconds]
R=${0:A:h:h}
GAME=${TT_GAME_DATA:?set TT_GAME_DATA to the extracted game folder}
OUT=/tmp/ttnet/$1; RUN_SECONDS=${2:-320}; rm -rf $OUT; mkdir -p $OUT
cd $R
python3 ps5/run_match.py --host 192.168.0.151 --no-test-path --seconds $RUN_SECONDS \
  --cvar user_live_signed_in=true --cvar tabletennis_system_link=true --cvar tabletennis_debug_print=true \
  --cvar ps5_capture_final=true --cvar ps5_capture_start_s=140 --cvar ps5_capture_count=60 --cvar ps5_capture_interval_ms=3000 \
  --cvar "ps5_pad_test_script=70:4000,70.8:4000,71.6:4000,72.4:4000,82:40,83:40,84:40,86:4000,90:4000,93:4000,106:4000,112:4000,145:4000,150:4000,190:4000,200:4000,215:4000,230:4000,250:4000,270:4000" \
  ${=PS5_CVARS} --log $OUT/ps5.log > $OUT/run_match.out 2>&1 &
RUNPID=$!
sleep 6
DYLD_LIBRARY_PATH=$R/out/netplay/rexglue-sdk/out/macos-arm64 SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0 SDL_MAC_BACKGROUND_APP=1 \
  $R/out/build/macos-netplay/tabletennis --game_data_root="$GAME" \
  --user_live_signed_in=true --tabletennis_system_link=true --tabletennis_debug_print=true --user_profile_xuid=B13E07DFF9AB6773 --user_profile_name=MacPlayer \
  --audio_mute=true --log_file=$OUT/mac.log \
  --tabletennis_test_input_script="60:1000,60.8:1000,61.6:1000,62.4:1000,76:2,77.5:2,79:2,81:1000,85:1000,120:1000,140:1000,160:1000,180:1000,195:1000,210:1000,230:1000,250:1000" \
  > $OUT/mac.stdout 2>&1 &
MACPID=$!
i=0
while kill -0 $RUNPID 2>/dev/null; do
  sleep 2; i=$((i + 1))
  id=$(/tmp/ttnet/winid)
  [ "$id" != "0" ] && screencapture -x -o -l$id $OUT/mac_$(printf %03d $i).png 2>/dev/null
done
kill $MACPID 2>/dev/null; sleep 2; kill -9 $MACPID 2>/dev/null
wait $RUNPID
python3 - $OUT <<'PY'
import sys
from ftplib import FTP, error_perm
out=sys.argv[1]
f=FTP(); f.connect('192.168.0.151',2121,timeout=30); f.login(); f.voidcmd('TYPE I')
l=[]; f.retrlines('LIST /data/homebrew/PPSA99782/cap', l.append)
for n in sorted(x.split()[-1] for x in l if x.endswith('.ppm')):
    with open(f'{out}/ps5_{n}','wb') as o: f.retrbinary('RETR /data/homebrew/PPSA99782/cap/'+n, o.write)
    f.sendcmd('DELE /data/homebrew/PPSA99782/cap/'+n)
for p in ['/data/homebrew/PPSA99782/ttrecomp/ps5.toml','/data/homebrew/PPSA99782/ttrecomp/settings.toml']:
    try: f.sendcmd('DELE '+p)
    except error_perm: pass
f.quit()
PY
echo done
