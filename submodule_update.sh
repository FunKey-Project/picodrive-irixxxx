#!/bin/sh
echo ""
echo "===== Updating Cyclone..."
cd /opt/picodrive-funkey/cpu/cyclone
git checkout master --quiet
git pull
echo ""
echo "===== Updating libchdr..."
cd /opt/picodrive-funkey/pico/cd/libchdr
git checkout master --quiet
git pull
echo ""
echo "===== Updating YM2413..."
cd /opt/picodrive-funkey/pico/sound/emu2413
git checkout master --quiet
git pull
echo ""
echo "===== Updating libpicofe..."
cd /opt/picodrive-funkey/platform/libpicofe
git checkout FunKey --quiet
git pull
echo ""
echo "===== Updating dr_libs..."
cd /opt/picodrive-funkey/platform/common/dr_libs
git checkout master --quiet
git pull
echo ""
echo "===== Updating minimp3..."
cd /opt/picodrive-funkey/platform/common/minimp3
git checkout master --quiet
git pull
echo ""
