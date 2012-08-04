# microdc2 - Introduction

**[microdc](http://www.nongnu.org/microdc/)** is a command-line based Direct Connect client written in C by [Oskar Liljeblad](mailto:oskar@osk.mine.nu) and designed to compile and run on modern POSIX compatible systems such as Linux.

After releasing microdc 0.12.0 I've renamed the project to microdc2 on Oskar's
request.

**microdc2** available from this site is the future improvement of the **microdc** based on Oskar's code version 0.11.0.

# Features

In addition to features available in the microdc 0.11.0 this version provides
the following new options:

**microdc2 0.15.0**

  * Support browsing remote users using XML filelists.
  * Added standalone **tthsum** utility to calculate TTH for specified files.

**microdc2 0.14.0**

This release incorporates patches made by **Alexey Illarionov** for
microdc-0.12.0.

  * Support different charsets for screen, hub and local file system
  * Support of $ADCGet command (TTHL support is not implemented yet)

**microdc2 0.13.0**

  * Automatic refresh of local file list.
  * Automatic TTH calculation for local files.
  * Multiple shared directory
  * Single storage for file list data ($HOME/.microdc2/filelist)

**microdc 0.12.0**

  * Timestamps in the log file.
  * Automatic re-connection to the last connected hub.
  * Own file list is supported in XML and XML.BZ2 format as well as originally DcLst format.
  * TTH hash for local files.

# News

2006-12-24: microdc2 0.15.6 released.

  * Fixed problem with reconnecting to hub before first connect

2006-12-24: microdc2 0.15.5 released.

  * Fixed problem with special characters like **& " '** in file names
  * Support of MiniSlots is introduced
  * Workaround for misconfigured clients sending chat in UTF-8 format
  * Automatically check state of link to hub in case of no activity
  * Delay 10 seconds before reconnecting to hub

2006-12-09: microdc2 0.15.4 released.

  * Fixed problem with non-working **share** command after executing **set filelist_refresh_interval** command
  * Don't report _Invalid search..._ message in case of empty search string

2006-11-29: microdc2 0.15.3 released.

  * Fixed problem with sharing directories located on different disk drives

2006-11-29: microdc2 0.15.2 released.

  * Fixed serious memory leak in new XML filelist generation
  * Fixed several small memory leaks
  * Fixed problem with printing From string in private chat without converting to screen charset 
  * **filelist_refresh_interval** is 32bit value now
  * Fixed problem with building **tthsum** on FreeBSD 6.0

2006-11-28: microdc2 0.15.1 released.

  * Fixed problem with CFLAGS during build on non-linux systems

2006-11-28: microdc2 0.15.0 released.

  * Fixed the crash in case the user has no access to some of shared directories or subdirs
  * Added new **filelist_refresh_interval** variable that allows to change the frequency of filelist refresh
  * Fixed problem with compilation microdc2 without LIBXML2 support
  * Added new **log_charset** variable to make the log file in different from screen charset
  * Fixed problem with sending private messages via **msg** command
  * Fixed bug with **listenaddr** variable - now microdc2 is listening only on specified address
  * Fixed bug with showing TTH instead file names in case of $ADCGET DC command

2006-11-21: microdc2 0.14.0 released.

  * Fixed the problem with **listingdir** variable
  * Variable **charset** is renamed to **hub_charset**
  * Re-designed generation of local file list - I hope now it doesn't requre a lot of memory :-)

2006-11-14: microdc2 0.13.1 released.

  * Fixed the problem with compilation on OpenBSD 4.0 and FreeBSD 6.0

2006-11-13: microdc2 0.13.0 released.

  * Fixed the problem with subsequent $UGetBlock command
  * Almost fixed memory leakage
  * Files without TTH are added to a XML file list as well as files with TTH
  * Removed variable share_dir 
  * 2 new commands have been introduced: **share** and **unshare**
  * TTH search support for remote client requests

2006-10-25: microdc 0.12.0 released.

# Download

The latest version of microdc2 is 0.15.6, which was released on 2006-12-24:

  * [Latest source code tarball (microdc2-0.15.6.tar.gz)](microdc2-0.15.6.tar.gz)

The previous versions of microdc2 are available in the archive:

  * [Previous source code tarballs](archive/)

# Copyright and License

**microdc** is copyright (C) 2004, 2005 Oskar Liljeblad

**microdc2** is copyright (C) 2006 Vladimir Chugunov

This program is free software; you can redistribute it and/or modify it under
the terms of the [GNU General Public
License](http://www.gnu.org/copyleft/gpl.html) as published by the [Free
Software Foundation](http://www.fsf.org/); either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the [GNU General Public
License](http://www.gnu.org/copyleft/gpl.html) for more details.

You should have received a copy of the [GNU General Public
License](http://www.gnu.org/copyleft/gpl.html) along with this program; if
not, write to the [Free Software Foundation](http://www.fsf.org/), Inc., 51
Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

The source code of this project may contain files from other projects, and
files generated by other projects, including:

  * [GNU Autoconf](http://www.gnu.org/software/autoconf/)
  * [GNU Automake](http://sources.redhat.com/automake/)
  * [GNU gettext](http://www.gnu.org/software/gettext/)
  * [Gnulib](http://www.gnu.org/software/gnulib/)
  * [Autoconf Macro Archive](http://ac-archive.sourceforge.net/)
  * [Tiger: A Fast New Cryptographic Hash Function (Designed in 1995)](http://www.cs.technion.ac.il/~biham/Reports/Tiger/)
  * [bzip2 and libbzip2](http://www.bzip.org)

Such files are licensed under the terms of the [GNU General Public
License](http://www.gnu.org/copyleft/gpl.html) or a license compatible with
the GNU GPL (as listed on [http://www.gnu.org/licenses/license-
list.html](http://www.gnu.org/licenses/license-list.html)). See each file for
copyright details.

The translations in the `po` directory may contain translations from other
projects, including:

  * [GNU C Library](http://www.gnu.org/software/libc/libc.html)
  * [GNU Core Utilities](http://www.gnu.org/software/coreutils/)

See the specific message file (PO file) for copyright of those messages.

# Requirements

The following programs are required to build microdc2:

  * GNU C Compiler (gcc), version 3.0 or later

microdc2 makes use of some gcc 3.x features such as declarations in the middle
of a block and always inlined functions. The GNU C Compiler is part of the
[GNU Compiler Collection](http://gcc.gnu.org/) which can be downloaded from
[http://gcc.gnu.org/](http://gcc.gnu.org/). In Debian and many other
distributions the package is called `gcc`.

  * make, a modern implementation

The make program is required to build microdc2. microdc2 uses Makefiles
generated by GNU Automake. The recommended make is GNU Make which can be
downloaded from
[http://www.gnu.org/software/make/](http://www.gnu.org/software/make/). In
Debian and many other distributions the package is called `make`.

The following libraries are required to run microdc2:

  * [GNU Readline Library](http://cnswww.cns.cwru.edu/php/chet/readline/rltop.html), version 4.0 or later

microdc2 uses Readline for user input. GNU Readline can be downloaded from [ht
tp://cnswww.cns.cwru.edu/php/chet/readline/rltop.html](http://cnswww.cns.cwru.
edu/php/chet/readline/rltop.html). Libraries such as libedit and libeditline
do not support the necessary completion features of GNU Readline, and can as
such not be used with microdc2. In Debian woody the required package is
`libreadline4` (`libreadline4-dev` during build). In testing and later, the
recommended package is `libreadline5` (and `libreadline5-dev` when building).

  * [libxml2](http://xmlsoft.org), version 5.6.16 or later

microdc2 uses libxml2 to generate filelist. libxml2 can be downloaded from
[http://xmlsoft.org/downloads.html](http://xmlsoft.org/downloads.html).

# Building

To build the `microdc2` executable, simply run

    
    
    ./configure
    make
    

If you want to install microdc2 on your system, run `make install`. This will
copy the executable, manual page and locale files to the appropriate
directories.

For more information regarding _configure_ and _make_, see the `INSTALL`
document.

# Usage and Customization

### microdc2

microdc2 is usually started like this:

    
    
    microdc2
    
    

(Some command line options are available - run `microdc2 --help` to get a
list.) Once started, a prompt is displayed and microdc2 is ready for your
command. You can get a list of commands by pressing tab on the empty prompt or
by issuing the `help` command. The `help` command can also be used to get
information on a specific command:

    
    
    help exit
    

### microdc_tth

#### Note: starting from microdc2 0.13.0 release the microdc_tth program is
not built by default. It is unnecessary due to availability of the TTH
calculation within **microdc2** itself.

microdc_tth is an application that can be used to maintain TTH hashes for
shared files. It is usually started like this:

    
    
    microdc_tth /my/share/dir
    
    

It creates .microdc_tth folder in the each subdirectory and puts the TTH hash
to this folder. The XML file list contains only files with existing TTH files.

# Homepage

Web site and file area for **microdc** is hosted on
[Savannah](http://savannah.nongnu.org/):

[http://www.nongnu.org/microdc/](http://www.nongnu.org/microdc/)

Web site and file area for **microdc2** is hosted on
[Corsair626](http://corsair626.no-ip.org/microdc/):

[http://corsair626.no-ip.org/microdc/](http://corsair626.no-ip.org/microdc/)

The latest version of microdc2 should always be available on this site.

# Feedback

Please send bug reports, suggestions, ideas or comments in general to the
mailing list:

[microdc-devel@nongnu.org](http://lists.nongnu.org/mailman/listinfo/microdc-
devel)

The author of original **microdc**, Oskar Liljeblad, can be contacted by
e-mail on the following address:

[oskar@osk.mine.nu](mailto:oskar@osk.mine.nu)

The author of **microdc2**, Vladimir Chugunov, can be contacted by e-mail on
the following address:

[vladch@k804.mainet.msk.su](mailto:vladch@k804.mainet.msk.su)

  

