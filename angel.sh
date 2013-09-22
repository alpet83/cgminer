#/bin/sh

ping nl1.ghash.io -c 1
ping us1.ghash.io -c 1

DATE=`date +%F_%H-%M`
FREEMEM=`head /proc/meminfo | grep MemFree | awk '{ print $2 }'`

if [ $FREEMEM  -lt 50000  ]; then
   echo 'Low freeMemory, trying kill cgminer ' >> /var/log/mining.log
   killall cgminer
fi

if [ `ps aux | grep cgminer | grep -v grep | wc -l` -eq 0 ]; then 
    echo $DATE' cgminer not found, restarting ' >> /var/log/mining.log
    /sbin/mine.sh; 
else
    echo $DATE' cgminer active, freeMemory = '$FREEMEM >> /var/log/mining.log
fi
 
