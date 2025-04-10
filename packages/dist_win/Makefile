# $Id: Makefile 960 2006-06-01 00:22:05Z charlton $

#
# To run step by step
# 1. make source
# 2. make build_all
# and step 3 may be broken using the following
# 3a. make prep
# 3b. make conf
# 3c. make build
# 3d. make install
# 3e. make pkg
# 3f. make spkg
# 3g. make finish
#

NAME := phast

FORCE_UPDATE := $(shell svn up ..)

RELEASE ?= $(shell svn st -v ../RPM/dist.sh | cut -b 10- | ( read one two ; echo $$one ) )

DATE_STAMP := $(shell date -d $(DATE) "+%m-%d-%Y")

MV_CVS_TAG := version-`echo "$(VERSION)" | sed -e 's/\./_/g'`

MV_CVS_TAG := $(shell echo version-$(VERSION) | sed -e "s/\./_/g" )

TARBALL := $(NAME)-$(VERSION)-$(RELEASE).tar.gz

MV_TARBALL := Mv-$(VERSION).tar.gz

BUILD_SCRIPT := $(NAME)-$(VERSION)-$(RELEASE).sh

REPOS_PATH ?= trunk

# (ie wphast-0.1-584-src.tar.bz2)
SRC_PACKAGE := $(NAME)-$(VERSION)-$(RELEASE)-src.tar.bz2

# (ie wphast-0.1-584.tar.bz2)
BIN_PACKAGE := $(NAME)-$(VERSION)-$(RELEASE).tar.bz2

all : check_args $(BIN_PACKAGE)

build_all : check_args $(BIN_PACKAGE)

prep : check_args source script
	time ./$(BUILD_SCRIPT) prep 2>&1 | tee prep-$(RELEASE).log

conf : check_args source script
	time ./$(BUILD_SCRIPT) conf 2>&1 | tee conf-$(RELEASE).log

build : check_args source script
	time ./$(BUILD_SCRIPT) build 2>&1 | tee build-$(RELEASE).log

install : check_args source script
	time ./$(BUILD_SCRIPT) install 2>&1 | tee install-$(RELEASE).log

pkg : check_args source script
	time ./$(BUILD_SCRIPT) pkg 2>&1 | tee pkg-$(RELEASE).log

spkg : check_args source script
	time ./$(BUILD_SCRIPT) spkg 2>&1 | tee spkg-$(RELEASE).log

finish : check_args source script
	time ./$(BUILD_SCRIPT) finish 2>&1 | tee finish-$(RELEASE).log
	
upto-conf : check_args source script
	time ./$(BUILD_SCRIPT) upto-conf 2>&1 | tee upto-conf-$(RELEASE).log
	
upto-build : check_args source script
	time ./$(BUILD_SCRIPT) upto-build 2>&1 | tee upto-build-$(RELEASE).log

upto-install : check_args source script
	time ./$(BUILD_SCRIPT) upto-install 2>&1 | tee upto-install-$(RELEASE).log

upto-pkg : check_args source script
	time ./$(BUILD_SCRIPT) upto-pkg 2>&1 | tee upto-pkg-$(RELEASE).log
	
upto-spkg : check_args source script
	time ./$(BUILD_SCRIPT) upto-spkg 2>&1 | tee upto-spkg-$(RELEASE).log	

$(BIN_PACKAGE) : $(TARBALL) $(MV_TARBALL) $(BUILD_SCRIPT)
	@echo "Building package $(BIN_PACKAGE) / $(SRC_PACKAGE)"	
	time ./$(BUILD_SCRIPT) all 2>&1 | tee all-$(RELEASE).log
	
source : check_version check_date $(TARBALL) ## $(MV_TARBALL)

$(TARBALL) : ../RPM/dist.sh $(DATE_STAMP).tstamp
	@echo "Creating $(TARBALL)"
	time ../RPM/dist.sh -v $(VERSION) -vp $(PHREEQC_VER) -r $(RELEASE) -d $(DATE) -pr $(REPOS_PATH) -win 2>&1 | tee dist-$(RELEASE).log
	
$(MV_TARBALL) : $(BUILD_SCRIPT)
	@echo "Creating $(MV_TARBALL)"
	time ./$(BUILD_SCRIPT) svnexport 2>&1 | tee svnexport-$(RELEASE).log	
	
script : check_version $(BUILD_SCRIPT)	
	
$(BUILD_SCRIPT) : build.sh
	@echo "Creating $(BUILD_SCRIPT)"
	ln -f build.sh $(BUILD_SCRIPT)
	
$(DATE_STAMP).tstamp : 
	rm -rf *.tstamp
	touch $(DATE_STAMP).tstamp
	
check_args : check_version check_date
	
check_version :
ifeq ($(strip $(VERSION)),)
	@echo "No version specified"
	@make -s usage
	@exit 1
endif	

check_date :
ifeq ($(strip $(DATE)),)
	@echo "No date specified"
	make usage
	exit 1
endif

usage:
	@echo ""
	@echo "usage: make VERSION=<number> DATE=<xx/xx/xxxx> [RELEASE=<number>] [SIG=<0|1>]"
	@echo "    ie make VERSION=0.1 DATE=10/24/2005"
	@echo "       make VERSION=0.1 DATE=10/24/2005 RELEASE=620 SIG=1"
	@echo ""
