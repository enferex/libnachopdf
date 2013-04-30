APP = pdfgrep
OBJS = main.o
CFLAGS = -g3 -O0 -Wall -pedantic -std=c99 -fPIC -DDEBUG -DDEBUG_PDF
LIB_PLAIN_NAME = nachopdf
LIBNAME = lib$(LIB_PLAIN_NAME)
LIBOBJS = pdf.o decode.o
LIB = $(LIBNAME).a

all: $(OBJS) $(APP) $(LIB)

%.o: %.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $^ -o $@

$(APP): $(OBJS) $(LIB)
	$(CC) $(OBJS) $(CFLAGS) $(EXTRA_CFLAGS) -L. -l$(LIB_PLAIN_NAME) -lz -o $@ 

$(LIB): $(LIBOBJS)
	$(AR) cr $@ $(LIBOBJS)

$(LIBNAME): $(LIB)

test: $(APP)
	LD_LIBRARY_PATH=$(LD_LIBRARY_PATH):. \
	./$(APP) -e "foo" test.pdf -d 1

debug: $(APP)
	LD_LIBRARY_PATH=$(LD_LIBRARY_PATH):. \
	exec gdb --args ./$(APP) -e "foo" test.pdf -d 1

clean:
	$(RM) -fv $(APP) $(OBJS) $(LIB) $(LIBOBJS)
