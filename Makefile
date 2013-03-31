APP = pdfgrep
CFLAGS = -g3 -O0 -Wall -pedantic -std=c99 -DDEBUG

$(APP): main.c
	$(CC) $^ $(CFLAGS) -o $(APP)

test: $(APP)
	./$(APP) -e "foo" test.pdf

debug: $(APP)
	exec gdb --args ./$(APP) -e "foo" test.pdf

clean:
	$(RM) -fv $(APP)
