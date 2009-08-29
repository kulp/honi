DEFINES += _POSIX_C_SOURCE
CFLAGS  += -std=c99 -W -Wall -Werror -Wextra -pedantic-errors $(addprefix -D,$(DEFINES))
# for debugging only
CFLAGS  += -g3 -Wno-unused

all: lexer

