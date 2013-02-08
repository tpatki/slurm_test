#

AUTOMAKE_OPTIONS = foreign
CLEANFILES = core.*

INCLUDES = -I$(top_srcdir)

bin_PROGRAMS = sim_mgr list_trace trace_builder sim_ctlr update_trace

sim_mgr_SOURCES = sim_mgr.c
list_trace_SOURCES = list_trace.c
trace_builder_SOURCES = trace_builder.c
sim_ctlr_SOURCES = sim_ctlr.c
update_trace_SOURCES = update_trace.c

convenience_libs = $(top_builddir)/src/api/libslurm.o 

sim_mgr_LDADD = $(convenience_libs)
list_trace_LDADD = $(convenience_libs)
trace_builder_LDADD = $(convenience_libs)
update_trace_LDADD = $(convenience_libs)
sim_ctlr_LDADD = $(convenience_libs)

sim_mgr_LDFLAGS = -export-dynamic $(CMD_LDFLAGS) -ldl -lrt -lpthread
list_trace_LDFLAGS = -export-dynamic $(CMD_LDFLAGS) -ldl -lrt -lpthread
trace_builder_LDFLAGS = -export-dynamic $(CMD_LDFLAGS) -ldl -lrt -lpthread
update_trace_LDFLAGS = -export-dynamic $(CMD_LDFLAGS) -ldl -lrt -lpthread
sim_ctlr_LDFLAGS = -export-dynamic $(CMD_LDFLAGS) -ldl -lrt -lpthread

pkglib_LTLIBRARIES = libslurm_sim.la

libslurm_sim_la_SOURCES = sim_lib.c
libslurm_sim_la_LDFLAGS = $(SO_LDFLAGS) -ldl -lrt -lpthread

force:
$(convenience_libs) : force
	      @cd `dirname $@` && $(MAKE) `basename $@`

