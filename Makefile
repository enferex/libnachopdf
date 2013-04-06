APP = pdfgrep
OBJS = main.o
CFLAGS = -g3 -O0 -Wall -pedantic -std=c99 -fPIC -DDEBUG
LIBNAME = nachopdf
LIBOBJS = pdf.o
LIB = lib$(LIBNAME).so

all: $(OBJS) $(APP) $(LIB)

%.o: %.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $^ -o $@

$(APP): $(OBJS) $(LIB)
	$(CC) $(OBJS) $(CFLAGS) $(EXTRA_CFLAGS) -L. -l$(LIBNAME) -lz -o $@ 

$(LIB): $(LIBOBJS)
	$(CC) $(LIBOBJS) $(CFLAGS) -fPIC -shared -o $@

test: $(APP) exportvars
	./$(APP) -e "foo" test.pdf -d 1

debug: $(APP) exportvars
	exec gdb --args ./$(APP) -e "foo" test.pdf -d 1

.PHONY: exportvars
exportvars:
	export LD_LIBRARY_PATH=$(LD_LIBRARY_PATH):.

clean:
	$(RM) -fv $(APP) $(OBJS) $(LIB) $(LIBOBJS)
