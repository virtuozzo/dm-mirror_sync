#
# Makefile for the Device mapper mirror_sync (i.e. synchronous mirror) driver.
#
# Copyright (C) 2012 OnApp Ltd.
# Author: (C) 2012 Michail Flouris <michail.flouris@onapp.com>

# Add debugging??
#DFLAGS = -g -g3 -ggdb
#EXTRA_CFLAGS += $(DFLAGS)

# try to detect the Linux distro type
ifneq ($(wildcard /etc/redhat-release),) 
    LINUX_TYPE := Redhat
else 
	ifneq ($(wildcard /etc/debian_version),) 
	    LINUX_TYPE := Debian
		ifneq ($(wildcard /etc/lsb-release),) 
		    LINUX_TYPE = Ubuntu
		else 
		    LINUX_TYPE := Unknown
		endif
	else 
		ifneq ($(wildcard /etc/SuSE-release),) 
		    LINUX_TYPE := SuSE
		else
			LINUX_TYPE := Unknown
		endif
	endif
endif

KERNEL_VERSION ?= $(shell uname -r)
KERN_MAJOR_VER := $(shell echo '$(KERNEL_VERSION)' | cut -d '.' -f 1)
KERN_MINOR_VER := $(shell echo '$(KERNEL_VERSION)' | cut -d '.' -f 2)

# 0 or 1 if we compile on 64-bit architecture
IS_64_ARCH := $(shell uname -m | grep 64 | wc -l )

# Directory for building module
BUILDDIR :=

ifeq ($(LINUX_TYPE),Redhat)

CENTOS_REL_STRING := $(shell cat /etc/redhat-release | grep 'CentOS Linux' )
ifeq ($(CENTOS_REL_STRING),)
# try the older Centos string version
CENTOS_VERSION := $(shell cat /etc/redhat-release | grep 'CentOS' | cut -c 16 )
else
CENTOS_VERSION := $(shell cat /etc/redhat-release | grep 'CentOS Linux' | cut -c 22 )
endif

# Check which version of centos to build on... 
ifeq ($(CENTOS_VERSION),)
	BUILDDIR := $(error CentOS not found! Current makefiles support only CentOS. Aborting!)
endif
ifeq ($(CENTOS_VERSION),5)
	BUILDDIR := "centos5_kernel-2.6.18"
endif
ifeq ($(CENTOS_VERSION),$(filter $(CENTOS_VERSION),6 7))
	ifeq ($(KERN_MAJOR_VER),3)
		ifeq ($(shell echo '$(KERN_MINOR_VER) < 13' | bc), 1)
			BUILDDIR := "linux-kernel-3.8"
		else
			BUILDDIR := "linux-kernel-3.13"
		endif
	else
		BUILDDIR := "centos6_kernel-2.6.32"
	endif
endif
else
	# we go by kernel version number in here...
	ifeq ($(LINUX_TYPE),Ubuntu)
		ifeq ($(KERN_MAJOR_VER),3)
			ifeq ($(shell echo '$(KERN_MINOR_VER) < 13' | bc), 1)
				BUILDDIR := "linux-kernel-3.8"
			else
				BUILDDIR := "linux-kernel-3.13"
			endif
		else
			BUILDDIR := $(error Ubuntu Kernel version < 3! Change into a 2.x kernel version subdir and 'make' in there!)
		endif
	else
		BUILDDIR := $(error Unsupported linux distro! Change into a kernel version subdir and 'make' in there!)
	endif
endif

# Do we support the build on the current Linux distro & kernel version?
ifeq ($(BUILDDIR),)
	BUILDDIR := $(error dm-mirror_sync built not supported on current distro/kernel version! Aborting!)
endif

BINS= nbd_print_debug

.PHONY: all dms_mod ins lsm rmm test install clean wc utils

all: dms_mod utils # tags types.vim

dms_mod:
	@echo Detected LINUX_TYPE=\"${LINUX_TYPE}\"
	@echo Building on CENTOS_VERSION=\"$(CENTOS_VERSION)\"
	@echo BUILDDIR= $(BUILDDIR)
	(cd $(BUILDDIR) ; $(MAKE))

utils::
	(cd utils ; make $(TARGET))

ins:
	(cd $(BUILDDIR) ; $(MAKE) $@)

lsm:
	(cd $(BUILDDIR) ; $(MAKE) $@)

rmm:
	(cd $(BUILDDIR) ; $(MAKE) $@)

test:
	(cd $(BUILDDIR) ; $(MAKE) $@)

install:
	(cd $(BUILDDIR) ; $(MAKE) $@)
	(cd utils ; make $@)

clean:
	(cd $(BUILDDIR) ; $(MAKE) $@)
	(cd utils ; make $@)
	\rm -rf *.o .*.o.d .depend *.ko .*.cmd *.mod.c .tmp* Module.markers Module.symvers
	\rm -f types.vim tags $(BINS)

wc:
	(cd $(BUILDDIR) ; $(MAKE) $@)
	@echo -n "Code lines (excl. blank lines): "
	@cat *.[ch] utils/*.[ch] | grep -v "^$$" | grep -v "^[ 	]*$$" | wc -l

tags:: *.[ch]
	(cd $(BUILDDIR) ; $(MAKE) $@)
	@\rm -f tags
	@ctags -R --languages=c

types.vim: *.[ch]
	(cd $(BUILDDIR) ; $(MAKE) $@)
	@echo "==> Updating tags !"
	@\rm -f $@
	@ctags -R --c-types=+gstu -o- *.[ch] utils/*.[ch] | awk '{printf("%s\n", $$1)}' | uniq | sort | \
	awk 'BEGIN{printf("syntax keyword myTypes\t")} {printf("%s ", $$1)} END{print ""}' > $@
	@ctags -R --c-types=+cd -o- *.[ch] utils/*.[ch] | awk '{printf("%s\n", $$1)}' | uniq | sort | \
	awk 'BEGIN{printf("syntax keyword myDefines\t")} {printf("%s ", $$1)} END{print ""}' >> $@
	@ctags -R --c-types=+v-gstucd -o- *.[ch] utils/*.[ch] | awk '{printf("%s\n", $$1)}' | uniq | sort | \
	awk 'BEGIN{printf("syntax keyword myVariables\t")} {printf("%s ", $$1)} END{print ""}' >> $@

