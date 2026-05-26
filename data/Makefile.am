# data/Makefile.am - Data files for Spool.

desktopdir   = $(datadir)/applications
desktop_DATA = org.gnome.Spool.desktop

org.gnome.Spool.desktop: org.gnome.Spool.desktop.in
	cp $< $@

# Scalable app icon, installed under the hicolor theme so the
# Icon= line in the .desktop file resolves to a real file. No
# gtk-update-icon-cache step here — the theme spec lets the loader
# find a new scalable icon on next lookup, and update-icon-cache
# only matters for the cached PNG sizes (which we don't ship).
appiconsdir   = $(datadir)/icons/hicolor/scalable/apps
appicons_DATA = icons/hicolor/scalable/apps/org.gnome.Spool.svg

EXTRA_DIST = \
    org.gnome.Spool.desktop.in \
    org.gnome.Spool.gresource.xml \
    icons/hicolor/scalable/apps/org.gnome.Spool.svg \
    ui/window.ui \
    ui/style.css

CLEANFILES = org.gnome.Spool.desktop
