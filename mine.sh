#!/bin/sh

# cd /home/pi/test2/cgminer
export DISPLAY=:0

ts=`date +%F_%H-%M`
echo $ts' Starting mine  ' >> /var/log/mining.log 

screen -q -dmS "MoneyFab" mine_start.sh
 
