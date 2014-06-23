sent
====

A simple plaintext presentation tool.

sent does not need latex, libreoffice or any other fancy file format, it uses
plaintext files. Every line represents a slide in the presentation. This may
limit the use, but for presentations using the [Takahashi
method](https://en.wikipedia.org/wiki/Takahashi_method) this is very nice and
allows you to write down the presentation for a quick lightning talk within a
few minutes.

The presentation is displayed in a simple X11 window colored black on white for
maximum contrast even if the sun shines directly onto the projected image. The
content of each slide is automatically scaled to fit the window so you don't
have to worry about alignment. Instead you can really concentrate on the
content.

Demo
----

To get a little demo, just type

	make && ./sent example

You can navigate with the arrow keys and quit with `q`.

Configuration
-------------

Edit config.h to fit your needs. The font has to be in the X servers font path,
see `man xset` for how to add it.

Usage
-----

	sent [-f FONTSTRING] FILE1 [FILE2 ...]

If one FILE equals `-`, stdin will be read. A presentation file could look like
this:

	sent
	why?
	easy to use
	few dependencies (X11)
	no bloat
	how?
	sent FILENAME
	thanks / questions?

future features
---------------

* utf8 support
* second window for speakers laptop (progress, time, notes?)
* images
* multiple lines per slide?
* markdown?
