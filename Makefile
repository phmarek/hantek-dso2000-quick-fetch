
all:
	echo Done


BUILD_DIR := build-root

release: /tmp/dso-qf.upk

/tmp/dso-qf.upk: /tmp/dso-qf.tar
	gpg --batch --passphrase dso3000c --cipher-algo AES --output $@ $<

/tmp/dso-qf.tar: /tmp/dso3kb.upk.tar
	tar -cf $@ $<

/tmp/dso3kb.upk.tar: $(shell find $(BUILD_DIR) -type f)
	tar -cvf $@ $*

$(BUILD_DIR)/build-root/package/quick-fetch.so: patch-src/quick-fetch.so
	cp -al $< $@
	#tar -zcvf dso3kb_$1.tar.gz dso3kb.upk.tar
	#rm -f dso3kb_$1.tar.gz dso3kb.upk.tar


include patch-src/Makefile

