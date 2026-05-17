# data/Makefile.am - Data files for GNOME Mail.

desktopdir   = $(datadir)/applications
desktop_DATA = org.gnome.Mail.desktop

org.gnome.Mail.desktop: org.gnome.Mail.desktop.in
	cp $< $@

EXTRA_DIST = \
    org.gnome.Mail.desktop.in \
    org.gnome.Mail.gresource.xml \
    ui/window.ui \
    ui/style.css

CLEANFILES = org.gnome.Mail.desktop
