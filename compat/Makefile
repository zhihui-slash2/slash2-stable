# $Id$

SUBDIRS=$(wildcard */)

all: $(patsubst %/,dir-%,${SUBDIRS})

$(patsubst %/,dir-%,${SUBDIRS}):
	@name='$(patsubst dir-%,%,$@)';					\
	if ${MAKE} -sC $${name}; then					\
		echo PICKLE_HAVE_$$(echo $$name | tr a-z A-Z)=1;	\
	fi
