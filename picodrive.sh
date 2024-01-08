#!/bin/sh
# Compare md5 of the PicoDrive binary in /usr/games with the OPK one
if [ `md5sum /usr/games/PicoDrive | cut -d' ' -f1` != `md5sum PicoDrive | cut -d' ' -f1` ]; then
	rw
	cp -f PicoDrive /usr/games
	ro
fi
./PicoDrive "$1"&
pid record $!
wait $!
pid erase
