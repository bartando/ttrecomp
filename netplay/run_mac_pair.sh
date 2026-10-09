#!/bin/zsh
# Two system link instances on this Mac: instance 0 hosts, instance 1 joins.
#   netplay/run_mac_pair.sh NAME [seconds]
R=${0:A:h:h}
GAME=${TT_GAME_DATA:?set TT_GAME_DATA to the extracted game folder}
OUT=/tmp/ttnet/$1; SECONDS_TOTAL=${2:-200}
rm -rf $OUT; mkdir -p $OUT; cd $OUT
COMMON=(--game_data_root="$GAME" --user_live_signed_in=true --tabletennis_system_link=true
        --tabletennis_debug_print=true --audio_mute=true)
export DYLD_LIBRARY_PATH=$R/out/netplay/rexglue-sdk/out/macos-arm64 SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0 SDL_MAC_BACKGROUND_APP=1
# Host: title, main menu, Xbox Live, Player Match, Quick Match, create (Yes), Accept, Start.
$R/out/build/macos-netplay/tabletennis $COMMON --net_instance=0 --user_profile_xuid=B13E07DFF9AB6773 \
  --user_profile_name=MacPlayer --log_file=$OUT/host.log \
  --tabletennis_test_input_script="${HOST_SCRIPT:-60:1000,60.8:1000,61.6:1000,62.4:1000,76:2,77.5:2,79:2,81:1000,85:1000,90:1000,100:1000,104:1000,150:1000,155:1000}" \
  > $OUT/host.stdout 2>&1 &
HOST=$!
$R/out/build/macos-netplay/tabletennis $COMMON --net_instance=1 --user_profile_xuid=B13E07DFF9AB6774 \
  --user_profile_name=MacGuest --log_file=$OUT/guest.log \
  --tabletennis_test_input_script="${GUEST_SCRIPT:-60:1000,60.8:1000,61.6:1000,62.4:1000,76:2,77.5:2,79:2,81:1000,85:1000,120:1000}" \
  > $OUT/guest.stdout 2>&1 &
GUEST=$!
for i in $(seq 0 $((SECONDS_TOTAL / 3))); do
  sleep 3
  /tmp/ttnet/winids | while read pid id; do
    [ "$pid" = "$HOST" ] && screencapture -x -o -l$id $OUT/host_$(printf %03d $i).png 2>/dev/null
    [ "$pid" = "$GUEST" ] && screencapture -x -o -l$id $OUT/guest_$(printf %03d $i).png 2>/dev/null
  done
done
kill $HOST $GUEST 2>/dev/null; sleep 1; kill -9 $HOST $GUEST 2>/dev/null
echo done
