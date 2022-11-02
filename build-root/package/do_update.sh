#!/bin/sh
set -ex
exec > /mnt/udisk/update.log 2>&1

WORKPATH=/dso/var/run
echo "-------------------update start------------------"

touch /tmp/kill_my_daemons
sleep 5
/dso/etc/S30dbus stop
pidof phoenix dbus-daemon | xargs kill -9

cd /
tar -xvf $WORKPATH/package/acm.tar.gz
rm -rf $WORKPATH

cp -va $WORKPATH/package/quick-fetch.so /root/

APP=/dso/app/app
BAK=$APP.before-qf
test -f $BAK || cp -va $APP $BAK 
sed 's,^.*LD_PRELOAD.*$,,; s,^\./.$APP_REL_NAME,export LD_PRELOAD=/lib/libdl.so.2:/dso/lib/libanolis.so.0:/root/quick-fetch.so\n\1,' < $APP > $APP.tmp
# mv $APP.tmp $APP

echo "-------------------update end------------------"
sync
sync
reboot -f
