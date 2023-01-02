
#CFLAGS += -g -D_LARGEFILE_SOURCE=1 -D_LARGEFILE64_SOURCE=1
#LDFLAGS += -g -ldl
CFLAGS += -D_LARGEFILE_SOURCE=1 -D_LARGEFILE64_SOURCE=1
LDFLAGS += -ldl -L. -lanolis -Wl,-rpath=/dso/lib
#-fPIC -shared 
CC:=arm-linux-gnueabi-gcc-9

ANOLIS_CFLAGS := '-DANOLIS_FN(x, r)= x { return r; }'


hantek-util: hantek-util.c
	@#strace -f -tt -s 2000 -o /tmp/asdgfa 
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@
hantek-util: libanolis.so
hantek-util: numbers.c

libanolis.E: libanolis.h
	$(CC) $(ANOLIS_CFLAGS) $< -E -o $@
libanolis.la: libanolis.c
	$(CC) $(ANOLIS_CFLAGS) -c $< -o $@
libanolis.so: libanolis.c
	$(CC) $(ANOLIS_CFLAGS) -pie -shared $< -o $@ 

sz:
	sz -8 --verbose hantek-util < /dev/ttyACM0 > /dev/ttyACM0

cp:
	ssh 172.31.254.254 'cat > /tmp/hantek-util' < hantek-util
