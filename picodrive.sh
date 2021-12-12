#!/bin/sh
# Compare md5 of the PicoDrive binary in /usr/games with the OPK one
if [ `md5sum /usr/games/PicoDrive | cut -d' ' -f1` != `md5sum PicoDrive | cut -d' ' -f1` ]; then
	rw
	cp -f PicoDrive /usr/games
	sed -i 's|list.extensions.*|list.extensions = zip,ZIP,sms,SMS,bin,BIN,gg,GG|' /usr/games/collections/Sega\ Master\ System/settings.conf
	ro
fi
exec /usr/games/launchers/megadrive_launch.sh "$1"
