#!/bin/sh
# Check md5 of the PicoDrive binary in /usr/games
myhash=$(md5sum /usr/games/PicoDrive | cut -d' ' -f1)
if [ $myhash != "5b42024e8e3a20e5a8b58ccd203b7c90" ] || [ ! -f /usr/games/PicoDrive ]; then
	rw
	cp -f PicoDrive /usr/games
	ro
fi
	./PicoDrive "$1"
