# Makefile for the Linux MXC Security API
ifeq ($( SCC_DEBUG),y)
EXTRA_CFLAGS += -DDEBUG
endif
ifeq ($(MXC_RTIC_TEST_DEBUG),y)
EXTRA_CFLAGS += -DDEBUG
endif




obj-$(CONFIG_MXC_SECURITY_SCC) += mxc_scc.o
obj-$(CONFIG_MXC_SECURITY_RTIC) += mxc_rtic.o
obj-$(CONFIG_MXC_SECURITY_RNG) += rng/
obj-$(CONFIG_MXC_SECURITY_CORE) += mxc_sec_mod.o
obj-$(CONFIG_MXC_SAHARA) += sahara2/
