export MakeInc_cmd=${SRCROOT}/makedefs/MakeInc.cmd
export MakeInc_def=${SRCROOT}/makedefs/MakeInc.def
export MakeInc_rule=${SRCROOT}/makedefs/MakeInc.rule
export MakeInc_dir=${SRCROOT}/makedefs/MakeInc.dir


include $(MakeInc_cmd)
include $(MakeInc_def)

SETUP_SUBDIRS = \
	tools

COMP_SUBDIRS = 

INST_SUBDIRS = 

ifndef SECURITY_KERNEL_CONFIG
export SECURITY_KERNEL_CONFIG = $(KERNEL_CONFIG)
endif

ifneq ($(MACHINE_CONFIG), DEFAULT)
export COMPOBJROOT=$(OBJROOT)/$(KERNEL_CONFIG)_$(ARCH_CONFIG)_$(MACHINE_CONFIG)/$(COMPONENT)
else
export COMPOBJROOT=$(OBJROOT)/$(KERNEL_CONFIG)_$(ARCH_CONFIG)/$(COMPONENT)
endif

$(COMPOBJROOT)/doconf:
	@make build_setup 

$(COMPOBJROOT)/$(SECURITY_KERNEL_CONFIG)/Makefile :  $(SOURCE)/MASTER  \
	$(SOURCE)/MASTER.$(ARCH_CONFIG_LC) \
	$(SOURCE)/Makefile.template  \
	$(SOURCE)/Makefile.$(ARCH_CONFIG_LC)  \
	$(SOURCE)/files \
	$(SOURCE)/files.$(ARCH_CONFIG_LC) \
	$(COMPOBJROOT)/doconf
	$(_v)(doconf_target=$(addsuffix /conf, $(TARGET)); \
	$(MKDIR) $${doconf_target}; \
	cd $${doconf_target}; \
	rm -f $(notdir $?); \
	cp  $? $${doconf_target}; \
	$(COMPOBJROOT)/doconf -c -cpu $(ARCH_CONFIG_LC) -d  $(TARGET)/$(SECURITY_KERNEL_CONFIG) $(SECURITY_KERNEL_CONFIG); \
	);

.ORDER: $(COMPOBJROOT)/$(SECURITY_KERNEL_CONFIG)/Makefile

do_setup_conf: $(COMPOBJROOT)/doconf \
		$(COMPOBJROOT)/$(SECURITY_KERNEL_CONFIG)/Makefile

do_all: do_setup_conf
	$(_v)next_source=$(subst conf/,,$(SOURCE));			\
	${MAKE} -C $(COMPOBJROOT)/$(SECURITY_KERNEL_CONFIG)	\
		MAKEFILES=$(TARGET)/$(SECURITY_KERNEL_CONFIG)/Makefile	\
		SOURCE=$${next_source}			\
		TARGET=$(TARGET)					\
		INCL_MAKEDEP=FALSE	\
		KERNEL_CONFIG=$(SECURITY_KERNEL_CONFIG)	\
		build_all;

do_build_all: do_all 

include $(MakeInc_rule)
include $(MakeInc_dir)
