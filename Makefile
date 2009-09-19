DEFINES += _POSIX_C_SOURCE=200112L
CFLAGS  += -std=c99 -W -Wall -Werror -Wextra -pedantic-errors \
		   $(addprefix -D,$(DEFINES)) -Wno-unused-parameter
CFLAGS  += -g # for debugging only
LDLIBS  += -lsyck
CFILES  += $(wildcard *.c)
TARGETS += hd2yaml

all: $(TARGETS)

hd2yaml: hd2yaml.o hd_parser.o filestore.o

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

