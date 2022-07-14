#!/bin/sh
# Compare md5 of the PicoDrive binary in /usr/games with the OPK one
# Replace Mednafen by PicoDrive for Game Gear emulation in RetroFE
if [ `md5sum /usr/games/PicoDrive | cut -d' ' -f1` != `md5sum PicoDrive | cut -d' ' -f1` ]; then
	rw
	cp -f PicoDrive /usr/games
	cp -f /usr/games/launchers/megadrive_launch.sh /usr/games/launchers/gamegear_launch.sh
	sed -i 's|list.extensions.*|list.extensions = zip,ZIP,sms,SMS,bin,BIN,sg,SG|' /usr/games/collections/Sega\ Master\ System/settings.conf
	ro
fi
# mpstat for CPU bench (uncomment lines 13 and 15 to use it)
# By default 30 iterations every 2 seconds during 1 minute after ROM launch
# mpstat 2 30 > /mnt/FunKey/.picodrive/stats.log &
./PicoDrive "$1"&
# pkill -9 mpstat
pid record $!
wait $!
pid erase
