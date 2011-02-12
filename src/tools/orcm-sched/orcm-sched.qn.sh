#!/bin/sh
#
# Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.

if (( $# < 1 )) ; then
    echo "usage: orcm-sched.qn.sh <delay in sec> <options>\n"
    exit 1
fi

# take the first arg
var=$1
shift 1

sleep "${var}"

#exec the app with the remaining args
exec "orcm-sched" "-mca" "orte_ess_jobid" "0" "-mca" "orte_ess_vpid" "1" "$@"
