#!/bin/sh
echo ""
echo "===== Updating Cyclone..."
cd /opt/picodrive-irixxxx/cpu/cyclone
git checkout master --quiet
git pull
echo ""
echo "===== Updating libchdr..."
cd /opt/picodrive-irixxxx/pico/cd/libchdr
git checkout master --quiet
git pull
echo ""
echo "===== Updating YM2413..."
cd /opt/picodrive-irixxxx/pico/sound/emu2413
git checkout master --quiet
git pull
echo ""
echo "===== Updating libpicofe..."
cd /opt/picodrive-irixxxx/platform/libpicofe
git checkout FunKey --quiet
git pull
echo ""
echo "===== Updating libpicofe..."
cd /opt/picodrive-irixxxx/platform/common/dr_libs
git checkout master --quiet
git pull
echo ""
echo "===== Updating libpicofe..."
cd /opt/picodrive-irixxxx/platform/common/minimp3
git checkout master --quiet
git pull
echo ""
