 #
##
## Makefile for QuickDC core library
## Copyright (c) 2001-2010, Jan Vidar Krey
##
 #

BUILDDIR ?= .
COREDIR  ?= $(BUILDDIR)/../

include Makefile.common
include Makefile.sources

libquickdc_OBJECTS := $(libquickdc_SOURCES:.cpp=.o)
libquickdc_DEPENDS := $(libquickdc_SOURCES:.cpp=.d)
libquickdc_ARCHIVE := libquickdc.a

.PHONY: tests $(libquickdc_ARCHIVE)
all: tests $(ALL)

### tests
include Makefile.tests

###### TARGETS - creating binaries
$(libquickdc_ARCHIVE): $(libquickdc_OBJECTS)
	@echo "    AR " $(subst $(COREDIR)/,,$@)
	@$(AR) $(libquickdc_ARCHIVE) $(libquickdc_OBJECTS)

### Include dir
CXXFLAGS += -I$(COREDIR) -I$(SAMURAI_INC)

.PHONY: clean
clean:
	@$(RM) $(libquickdc_OBJECTS)
	@$(RM) $(ALL)
	@$(RM) $(TESTS)
	@$(RM) $(libquickdc_ARCHIVE)

.PHONY: autotest
autotest: $(libquickdc_ARCHIVE)
	@$(MAKE) clean all -C $(COREDIR)/test/autotests
	@gdb -x $(COREDIR)/test/autotests/autotest.gdb --args $(COREDIR)/test/autotests/autotest -s -f



