# data/Makefile.am - Data files for Magpie.

desktopdir   = $(datadir)/applications
desktop_DATA = org.gnome.Magpie.desktop

org.gnome.Magpie.desktop: org.gnome.Magpie.desktop.in
	cp $< $@

EXTRA_DIST = \
    org.gnome.Magpie.desktop.in \
    org.gnome.Magpie.gresource.xml \
    ui/window.ui \
    ui/style.css

CLEANFILES = org.gnome.Magpie.desktop
