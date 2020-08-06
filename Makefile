EXTENSION    = ocgeo_fdw
EXTVERSION   = $(shell grep default_version $(EXTENSION).control | sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")
MODULE_big   = ocgeo_fdw
OBJS         = ocgeo_fdw.o ocgeo_api.o sds.o cJSON.o
# RELEASE      = 1.0.0

DATA         = $(wildcard sql/*--*.sql)
DOCS         = $(wildcard doc/*.md)
TESTS        = $(wildcard test/sql/*.sql)
REGRESS      = $(patsubst test/sql/%.sql,%,$(TESTS))
REGRESS_OPTS = --inputdir=test

CURL_CONFIG = curl-config
PG_CONFIG = pg_config

CFLAGS += $(shell $(CURL_CONFIG) --cflags)
LIBS += $(shell $(CURL_CONFIG) --libs)
SHLIB_LINK := $(LIBS)

ifdef DEBUG
COPT += -O0 -DDEBUG -g
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
