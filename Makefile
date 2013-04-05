APP = pdfgrep
CFLAGS = -g3 -O0 -Wall -pedantic -std=c99 -DDEBUG

$(APP): main.c
	$(CC) $^ $(CFLAGS) -o $(APP) -lz $(EXTRA_CFLAGS)

test: $(APP)
	./$(APP) -e "foo" test.pdf -d 1

debug: $(APP)
	exec gdb --args ./$(APP) -e "foo" test.pdf -d 1

clean:
	$(RM) -fv $(APP)
