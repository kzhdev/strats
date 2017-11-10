#! /bin/sh

list=`ipcs -m | grep -v CREATOR | grep -vi status | grep -v Shared | awk '{print $2}'`

for shseg in $list ; do
	ipcrm -m $shseg
done
