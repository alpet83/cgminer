#!/bin/sh

# cd /home/pi/test2/cgminer
export DISPLAY=:0

ts=`date +%F_%H-%M`
echo $ts' Starting mine  ' >> /var/log/mining.log 

screen -q -dmS "MoneyFab" /sbin/mine_start.sh
# nice -19 /home/pi/test2/cgminer/cgminer --api-listen --api-port 4028 --api-network -c /home/pi/.cgminer/cgminer.conf 
 
