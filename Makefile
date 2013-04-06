APP = pdfgrep
OBJS = main.o pdf.o
CFLAGS = -g3 -O0 -Wall -pedantic -std=c99 -DDEBUG

all: $(OBJS) $(APP)

%.o: %.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $^ -o $@

$(APP): $(OBJS)
	$(CC) $(OBJS) $(CFLAGS) $(EXTRA_CFLAGS) -lz -o $@

test: $(APP)
	./$(APP) -e "foo" test.pdf -d 1

debug: $(APP)
	exec gdb --args ./$(APP) -e "foo" test.pdf -d 1

clean:
	$(RM) -fv $(APP) $(OBJS)
