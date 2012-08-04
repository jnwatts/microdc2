ACLOCAL_AMFLAGS = -I m4

SUBDIRS = po lib src

EXTRA_DIST = \
  doc/INTERNALS \
  doc/MANIFEST.sources \
  doc/TODO \
  debian/changelog \
  debian/control \
  debian/copyright \
  debian/rules \
  m4/gnulib-cache.m4

.PHONY: debian rpm slackware update-gnulib

debian:
	cd $(srcdir) && dpkg-buildpackage -rfakeroot -us -uc -sd -b

rpm: rpm/@PACKAGE@.spec
	fakeroot rpmbuild --clean -bb rpm/@PACKAGE@.spec

slackware: slackware/@PACKAGE@.SlackBuild
	fakeroot ./slackware/@PACKAGE@.SlackBuild

clean-local:
	-rm -f build-stamp configure-stamp stamp-h1
	-rm -f debian/compat debian/files debian/@PACKAGE@.substvars
	-rm -f debian/stamp-*
	-rm -rf debian/@PACKAGE@

# This macro is useful until gnulib-tool is incorporated into autoreconf.
# Unfortunately the ./configure script is first run if configure.ac is
# modified, so it is probably more useful to run the command below
# manually instead.
update-gnulib:
	gnulib-tool --import --symlink
