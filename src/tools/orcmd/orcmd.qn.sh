#!/bin/sh
#
# Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.

if (( $# < 1 )) ; then
    echo "usage: orcmd.qn.sh <delay in sec> <options>\n"
    exit 1
fi

# take the first arg
var=$1
shift 1

sleep "${var}"

#exec the app with the remaining args
exec "orcmd" "$@"
