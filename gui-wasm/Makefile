ROOT=..
include ../Make.config
LIB=libgui.a

OFILES=\
	screen.$O\
	canvas.$O\

default: $(LIB)
$(LIB): $(OFILES)
	$(AR) r $(LIB) $(OFILES)
	$(RANLIB) $(LIB)

