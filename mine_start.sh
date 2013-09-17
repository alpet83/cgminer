#!/bin/sh
cd /home/pi/test2/cgminer
# export DISPLAY=:0
# screen -q
nice -19 /home/pi/test2/cgminer/cgminer --api-listen --api-port 4028 --api-network --compact -T -Q 10 -c /home/pi/.cgminer/cgminer.conf 
 
