#!/bin/sh
echo ""
echo "===== Updating Cyclone..."
cd cpu/cyclone
git checkout master --quiet
git pull
echo ""
echo "===== Updating libchdr..."
cd ../../pico/cd/libchdr
git checkout master --quiet
git pull
echo ""
echo "===== Updating YM2413..."
cd ../../../pico/sound/emu2413
git checkout master --quiet
git pull
echo ""
echo "===== Updating libpicofe..."
cd ../../../platform/libpicofe
git checkout master --quiet
git pull
echo ""
echo "===== Updating dr_libs..."
cd ../../platform/common/dr_libs
git checkout master --quiet
git pull
echo ""
