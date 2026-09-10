############################################################################
# apps/examples/mianyu/Makefile
############################################################################

-include $(TOPDIR)/Make.defs
-include $(APPDIR)/Make.defs

CFLAGS += -Wno-unused-parameter

SRCS = mianyu_app_main.c
SRCS += my_datetime.c my_sched.c my_noise.c my_sleep.c my_fade.c
SRCS += my_memory.c my_breath.c mianyu_hal_sim.c breathe_lvgl.c

OBJS = $(SRCS:.c=$(OBJEXT))

LIB = lib$(APP).a

$(LIB): $(OBJS)
	$(call AR, $(ARFLAGS), $@, $(OBJS))

.depend: Makefile $(SRCS)
	$(MKDEP) $(ROOTDEPPATH) $(CFLAGS) $^ > $@

depend: .depend

clean:
	$(call DELFILE, $(OBJS))
	$(call DELFILE, $(LIB))
	$(call DELFILE, .depend)

# Required by old NuttX build
mianyu_main$(OBJEXT): mianyu_app_main.c
	$(CC) -c $(CFLAGS) -o $@ $<

include .depend