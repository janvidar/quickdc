 #
##
## Makefile for QuickDC core library
## Copyright (c) 2001-2007, Jan Vidar Krey
##
 #

BUILDDIR ?= $(CURDIR)/../../build/
CXXFLAGS += -I../.. -I/usr/include/samurai
LDFLAGS  += -lexotic -lsamurai -lpthread
libquickdc_ARCHIVE := $(BUILDDIR)/libquickdc.a

ARCHIVES=$(libquickdc_ARCHIVE) -lz -lbz2 -lssl


autotest_SOURCES := adc_parser.tcc user.tcc

.PHONY: all clear

all: autotest

generated_tests.o: generated_tests.cpp

autotest: generated_tests.o
	echo "Creating $@" && $(CXX) -o $@ $^ $(ARCHIVES) $(LDFLAGS) 
	@$(RM) generated_tests.o generated_tests.cpp

generated_tests.cpp:
	$(shell exotic $(autotest_SOURCES) > generated_tests.cpp )

clean:
	@$(RM) *.o 
	@$(RM) autotest generated_tests.cpp 


