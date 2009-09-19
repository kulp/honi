DEFINES += _POSIX_C_SOURCE
CFLAGS  += -std=c99 -W -Wall -Werror -Wextra -pedantic-errors $(addprefix -D,$(DEFINES))
# for debugging only
CFLAGS  += -g3 -Wno-unused
LDLIBS  += -lsyck
CFILES  += $(wildcard *.c)

all: hd2yaml

hd2yaml: hd2yaml.o hd_parser.o filestore.o

CLEANFILES += hd2yaml *.[od]

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

