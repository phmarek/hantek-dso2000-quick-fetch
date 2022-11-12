#!/bin/sh

paplay /usr/share/sounds/freedesktop/stereo/message-new-instant.oga &
notify-send  -t 2000 -a "Hantek - Quick fetch" "File '$RECEIVED_HANTEK_FILE' received" -e -i /usr/share/icons/Papirus/32x32/apps/microscope.svg
