
#CFLAGS += -g -D_LARGEFILE_SOURCE=1 -D_LARGEFILE64_SOURCE=1
#LDFLAGS += -g -ldl
CFLAGS += -D_LARGEFILE_SOURCE=1 -D_LARGEFILE64_SOURCE=1
LDFLAGS += -ldl -lpthread

CC:=arm-linux-gnueabi-gcc-9

# to patch the executable with a direct jump we need to be in the vicinity
START=0x00e00000

quick-fetch.so: quick-fetch.c
	$(CC) $(CFLAGS) $(LDFLAGS) -fPIC -shared -Wl,-Ttext-segment=$(START) $< -o $@
quick-fetch.so: mdns.c

send-tar-to-hantek:
	sudo tar -cf /dev/disk/by-id/usb-Waveform_Dump*-0:0 quick-fetch.so

sz:
	sz -8 --verbose quick-fetch.so < /dev/ttyACM0 > /dev/ttyACM0

# Virtual hostname defined in ~/.ssh/config
# No SFTP or SCP
scp:
	tar -cf - quick-fetch.so | ssh root@hantek.local tar -xvf - 
cp:
	tar -cf - quick-fetch.so | ssh root@172.31.254.254 tar -xvf - 
