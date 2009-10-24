DEFINES += _POSIX_C_SOURCE=200112L _XOPEN_SOURCE=700
CFLAGS  += -std=c99 -W -Wall -Werror -Wextra -pedantic-errors \
		   $(addprefix -D,$(DEFINES)) -Wno-unused-parameter
CFLAGS  += -g # for debugging only
LDLIBS  += -lyaml
CFILES  += $(wildcard *.c)
TARGETS += hd2yaml

vpath %.c parser

all: $(TARGETS)

hd2yaml: hd2yaml.o hd_parser.o
ifeq ($(USE_MMAP),1)
hd2yaml: DEFINES += USE_MMAP
hd2yaml: mmapstore.o
else
hd2yaml: filestore.o
endif

CLEANFILES += $(TARGETS) *.[od]

.PHONY: clean
clean:
	-$(RM) -r $(CLEANFILES)

ifneq ($(MAKECMDGOALS),clean)
-include $(CFILES:.c=.d)
endif

%.d: %.c
	@set -e; rm -f $@; \
	$(CC) $(CFLAGS) -MM -MG -MF $@.$$$$ $<; \
	sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' < $@.$$$$ > $@; \
	rm -f $@.$$$$

