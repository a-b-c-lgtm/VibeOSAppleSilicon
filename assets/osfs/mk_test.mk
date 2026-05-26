# mk_test.mk -- chapter 133b /bin/make smoke fixture.
#
# Exercises everything chapter 133b added:
#
#   1. Variable definitions:  CC, CFLAGS, OBJS, OUT
#   2. Variable expansion:    $(CC), $(CFLAGS), $(OBJS), $(OUT)
#   3. Line continuation:     OBJS spread across two lines
#   4. Pattern rule:          %.o: %.c
#   5. Automatic variables:   $@, $<, $^
#   6. .PHONY:                clean target
#   7. Recipe prefixes:       @ (silent) and - (ignore-errors)
#
# The first rule is `all:` so that `make -f mk_test.mk` (no
# target) picks it up as the default goal.
#
# Run it under /bin/make to produce /tmp/mk_hello, then
# execute the binary -- it should print "hello A=42".

CC = /bin/gcc
CFLAGS = -O0
OUT = /tmp/mk_hello

OBJS = /tmp/mk_helloA.o \
       /tmp/mk_helloB.o

all: $(OUT)

$(OUT): $(OBJS)
	@/bin/echo [Linking $@]
	$(CC) $(CFLAGS) $^ -o $@

/tmp/%.o: /bin/%.c
	@/bin/echo [Compiling $<]
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	-/bin/rm /tmp/mk_hello
	-/bin/rm /tmp/mk_helloA.o
	-/bin/rm /tmp/mk_helloB.o
