#!/usr/bin/env python3
"""Install NetSurf as part of the build, not beside it.

`make` rebuilds disc.img from nothing every time, so anything written into it
by hand is gone the next time the libc changes. This puts NetSurf into the
recipe itself.

It is conditional on purpose. NetSurf is built by the scripts in tools/ out of
sources that live outside this tree, so a fresh clone has no binary to install
and must still build a working system. When there is one, it goes on; when
there is not, the build says so in one line and carries on.
"""
R = "/home/roman/xyuos-neo/"
p = R + "Makefile"
s = open(p).read()

a = """	# Python, and somewhere for its libraries to live. The binary is stripped
	# on the way in: its debug info is four times the size of the program
	# itself, and the disk is thirty-two megabytes."""

b = """	# NetSurf, if it has been built. See tools/nsbuild.py and the scripts
	# beside it; its sources are not in this tree, so a clone without them
	# still builds a whole system and simply has one fewer browser.
	#
	# The stylesheets matter as much as the binary: default.css is the user
	# agent stylesheet, and without it NetSurf has no block layout at all.
	@if [ -f third_party/nsxyuos/netsurf.elf ]; then \\
	    debugfs -w -R "write third_party/nsxyuos/netsurf.elf /bin/netsurf" disk.img >/dev/null 2>&1; \\
	    debugfs -w -R "mkdir /lib/netsurf" disk.img >/dev/null 2>&1; \\
	    for f in third_party/nsxyuos/res/*; do \\
	        debugfs -w -R "write $$f /lib/netsurf/`basename $$f`" disk.img >/dev/null 2>&1; \\
	    done; \\
	    echo "netsurf installed into the disk image"; \\
	else \\
	    echo "netsurf not built; skipping (see tools/nsbuild.py)"; \\
	fi

	# Python, and somewhere for its libraries to live. The binary is stripped
	# on the way in: its debug info is four times the size of the program
	# itself, and the disk is thirty-two megabytes."""

if "netsurf installed into the disk image" in s:
    print("already: netsurf is part of the disc build")
else:
    assert a in s, "the disk.img recipe is not the shape expected"
    s = s.replace(a, b, 1)
    # And make the image depend on it, so installing a newer build actually
    # rebuilds the image rather than leaving yesterday's binary in it.
    dep_a = """          $(CRT0) $(LIBC_A) $(wildcard libc/include/*.h)"""
    dep_b = """          $(CRT0) $(LIBC_A) $(wildcard libc/include/*.h) \\
          $(wildcard third_party/nsxyuos/netsurf.elf) \\
          $(wildcard third_party/nsxyuos/res/*)"""
    assert dep_a in s
    s = s.replace(dep_a, dep_b, 1)
    open(p, "w").write(s)
    print("ok: netsurf is installed by the disc build, and the image "
          "depends on it")
