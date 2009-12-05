#!/bin/sh
#
# Copyright © 2009 Cisco Systems, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

#
# This file originally came from PLPA's SVN trunk; last import was 11
# Sep 2009, r251.
#

srcfile="$1"
option="$2"

case "$option" in
    # svnversion can take a while to run.  If we don't need it, don't run it.
    --major|--minor|--release|--greek|--base|--help)
        openrcm_ver_need_svn=0
        ;;
    *)
        openrcm_ver_need_svn=1
esac

if test -z "$srcfile"; then
    option="--help"
else

    : ${openrcm_ver_need_svn=1}
    : ${srcdir=.}
    : ${svnversion_result=-1}

        if test -f "$srcfile"; then
        openrcm_vers=`sed -n "
	t clear
	: clear
	s/^major/OPENRCM_MAJOR_VERSION/
	s/^minor/OPENRCM_MINOR_VERSION/
	s/^release/OPENRCM_RELEASE_VERSION/
	s/^greek/OPENRCM_GREEK_VERSION/
	s/^want_svn/OPENRCM_WANT_SVN/
	s/^svn_r/OPENRCM_SVN_R/
	s/^date/OPENRCM_RELEASE_DATE/
	t print
	b
	: print
	p" < "$srcfile"`
	eval "$openrcm_vers"

        # Only print release version if it isn't 0
        if test $OPENRCM_RELEASE_VERSION -ne 0 ; then
            OPENRCM_VERSION="$OPENRCM_MAJOR_VERSION.$OPENRCM_MINOR_VERSION.$OPENRCM_RELEASE_VERSION"
        else
            OPENRCM_VERSION="$OPENRCM_MAJOR_VERSION.$OPENRCM_MINOR_VERSION"
        fi
        OPENRCM_VERSION="${OPENRCM_VERSION}${OPENRCM_GREEK_VERSION}"
        OPENRCM_BASE_VERSION=$OPENRCM_VERSION

        if test $OPENRCM_WANT_SVN -eq 1 && test $openrcm_ver_need_svn -eq 1 ; then
            if test "$svnversion_result" != "-1" ; then
                OPENRCM_SVN_R=$svnversion_result
            fi
            if test "$OPENRCM_SVN_R" = "-1" ; then
                if test -d "$srcdir/.svn" ; then
                    OPENRCM_SVN_R=r`svnversion "$srcdir"`
                elif test -d "$srcdir/.hg" ; then
                    OPENRCM_SVN_R=hg`hg -v -R "$srcdir" tip | grep changeset | cut -d: -f3`
                elif test "$srcdir/../.hg" ; then
                    # Note that openrcm is not at the top of the hg
                    # tree, so also check for ../.hg
                    OPENRCM_SVN_R=hg`hg -v -R "$srcdir/.." tip | grep changeset | cut -d: -f3`
                fi
                if test "OPENRCM_SVN_R" = ""; then
                    OPENRCM_SVN_R=svn`date '+%m%d%Y'`
                fi
            fi
            OPENRCM_VERSION="${OPENRCM_VERSION}${OPENRCM_SVN_R}"
        fi
    fi


    if test "$option" = ""; then
	option="--full"
    fi
fi

case "$option" in
    --full|-v|--version)
	echo $OPENRCM_VERSION
	;;
    --major)
	echo $OPENRCM_MAJOR_VERSION
	;;
    --minor)
	echo $OPENRCM_MINOR_VERSION
	;;
    --release)
	echo $OPENRCM_RELEASE_VERSION
	;;
    --greek)
	echo $OPENRCM_GREEK_VERSION
	;;
    --svn)
	echo $OPENRCM_SVN_R
	;;
    --base)
        echo $OPENRCM_BASE_VERSION
        ;;
    --release-date)
        echo $OPENRCM_RELEASE_DATE
        ;;
    --all)
        echo ${OPENRCM_VERSION} ${OPENRCM_MAJOR_VERSION} ${OPENRCM_MINOR_VERSION} ${OPENRCM_RELEASE_VERSION} ${OPENRCM_GREEK_VERSION} ${OPENRCM_SVN_R}
        ;;
    -h|--help)
	cat <<EOF
$0 <srcfile> <option>

<srcfile> - Text version file
<option>  - One of:
    --full         - Full version number
    --major        - Major version number
    --minor        - Minor version number
    --release      - Release version number
    --greek        - Greek (alpha, beta, etc) version number
    --svn          - Subversion repository number
    --all          - Show all version numbers, separated by :
    --base         - Show base version number (no svn number)
    --release-date - Show the release date
    --help         - This message
EOF
        ;;
    *)
        echo "Unrecognized option $option.  Run $0 --help for options"
        ;;
esac

# All done

exit 0
