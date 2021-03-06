noinst_LTLIBRARIES = libsql.la

SQL_DRIVER_PLUGINS =

if SQL_PLUGINS
if BUILD_MYSQL
MYSQL_LIB = libdriver_mysql.la
SQL_DRIVER_PLUGINS += mysql
endif
if BUILD_PGSQL
PGSQL_LIB = libdriver_pgsql.la
SQL_DRIVER_PLUGINS += pgsql
endif
if BUILD_SQLITE
SQLITE_LIB = libdriver_sqlite.la
SQL_DRIVER_PLUGINS += sqlite
endif

sql_module_LTLIBRARIES = \
	$(MYSQL_LIB) \
	$(PGSQL_LIB) \
	$(SQLITE_LIB)

sql_moduledir = $(moduledir)
endif

sql_drivers = @sql_drivers@

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	$(SQL_CFLAGS)

dist_sources = \
	sql-api.c \
	sql-db-cache.c

if ! SQL_PLUGINS
driver_sources = \
	driver-mysql.c \
	driver-pgsql.c \
	driver-sqlite.c
endif

libsql_la_SOURCES = \
	$(dist_sources) \
	$(driver_sources) \
	driver-sqlpool.c
nodist_libsql_la_SOURCES = sql-drivers-register.c

if SQL_PLUGINS
libdriver_mysql_la_LDFLAGS = -module -avoid-version
libdriver_mysql_la_LIBADD = $(MYSQL_LIBS)
libdriver_mysql_la_CPPFLAGS = -I$(top_srcdir)/src/lib $(MYSQL_CFLAGS)
libdriver_mysql_la_SOURCES = driver-mysql.c

libdriver_pgsql_la_LDFLAGS = -module -avoid-version
libdriver_pgsql_la_LIBADD = $(PGSQL_LIBS)
libdriver_pgsql_la_CPPFLAGS = -I$(top_srcdir)/src/lib $(PGSQL_CFLAGS)
libdriver_pgsql_la_SOURCES = driver-pgsql.c

libdriver_sqlite_la_LDFLAGS = -module -avoid-version
libdriver_sqlite_la_LIBADD = $(SQLITE_LIBS)
libdriver_sqlite_la_CPPFLAGS = -I$(top_srcdir)/src/lib $(SQLITE_CFLAGS)
libdriver_sqlite_la_SOURCES = driver-sqlite.c
endif

pkglib_LTLIBRARIES = libdovecot-sql.la
libdovecot_sql_la_SOURCES = 
libdovecot_sql_la_LIBADD = libsql.la $(MODULE_LIBS)
libdovecot_sql_la_DEPENDENCIES = libsql.la
libdovecot_sql_la_LDFLAGS = -export-dynamic

headers = \
	sql-api.h \
	sql-api-private.h \
	sql-db-cache.h

pkginc_libdir=$(pkgincludedir)
pkginc_lib_HEADERS = $(headers)

sql-drivers-register.c: Makefile
	rm -f $@
	echo '/* this file automatically generated by Makefile */' >$@
	echo '#include "lib.h"' >>$@
	echo '#include "sql-api.h"' >>$@
if ! SQL_PLUGINS
	for i in $(sql_drivers) null; do \
	  if [ "$${i}" != "null" ]; then \
	        echo "extern struct sql_db driver_$${i}_db;" >>$@ ; \
	  fi; \
	done
endif
	echo 'void sql_drivers_register_all(void) {' >>$@
if ! SQL_PLUGINS
	for i in $(sql_drivers) null; do \
	  if [ "$${i}" != "null" ]; then \
	        echo "sql_driver_register(&driver_$${i}_db);" >>$@ ; \
	  fi; \
	done
endif
	echo '}' >>$@

if SQL_PLUGINS
install-exec-local:
	for d in auth dict; do \
	  $(mkdir_p) $(DESTDIR)$(moduledir)/$$d; \
	  for driver in $(SQL_DRIVER_PLUGINS); do \
	    rm -f $(DESTDIR)$(moduledir)/$$d/libdriver_$$driver.so; \
	    $(LN_S) ../libdriver_$$driver.so $(DESTDIR)$(moduledir)/$$d; \
	  done; \
	done
endif


distclean-generic:
	rm -f Makefile sql-drivers-register.c
