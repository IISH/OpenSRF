#!/bin/sh
# autogen.sh - generates configure using the autotools

: ${LIBTOOLIZE=libtoolize}
: ${ACLOCAL=aclocal}
: ${AUTOHEADER=autoheader}
: ${AUTOMAKE=automake}
: ${AUTOCONF=autoconf}


${LIBTOOLIZE} --force --copy
${ACLOCAL}
${AUTOMAKE} --add-missing


${AUTOCONF}

SILENT=`which libtoolize aclocal autoheader automake autoconf`
case "$?" in
    0 )
        echo All build tools found.
        ;;
    1)
        echo
        echo "--------------------------------------------------------------"
        echo "          >>> Some build tools are missing! <<<"
        echo Please make sure your system has the GNU autoconf and automake
        echo toolchains installed.
        echo "--------------------------------------------------------------"
        exit
        ;;
esac

echo 
echo "---------------------------------------------"
echo "autogen finished running, now run ./configure"
echo "---------------------------------------------"
