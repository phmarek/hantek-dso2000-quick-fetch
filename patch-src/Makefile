
#CFLAGS += -g -D_LARGEFILE_SOURCE=1 -D_LARGEFILE64_SOURCE=1
#LDFLAGS += -g -ldl
CFLAGS += -D_LARGEFILE_SOURCE=1 -D_LARGEFILE64_SOURCE=1
LDFLAGS += -ldl

CC:=arm-linux-gnueabi-gcc-9

# to patch the executable with a direct jump we need to be in the vicinity
START=0x00e00000

quick-fetch.so: quick-fetch.c
	$(CC) $(CFLAGS) $(LDFLAGS) -fPIC -shared -Wl,-Ttext-segment=$(START) $< -o $@

send-tar-to-hantek:
	sudo tar -cf /dev/disk/by-id/usb-Waveform_Dump*-0:0 quick-fetch.so

sz:
	SZ quick-fetch.so
