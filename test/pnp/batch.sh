#!/bin/bash

# Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
#

if (( $# < 1 )) ; then
    echo "usage: batch.sh report-uri-file other-args-to-orcm"
    exit 1
fi

# take the first arg as the file to store the uri in
urifile=$1
shift 1

orcm  -report-uri "$urifile" "$@" &
sleep 5
orcm-start -uri file:"$urifile" -n 1 ./server &
orcm-start -uri file:"$urifile" -n 1 ./client &
orcm-start -uri file:"$urifile" -n 1 ./client2
