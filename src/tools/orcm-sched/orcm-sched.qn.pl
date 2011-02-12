#!/usr/bin/perl
#
# Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
#

if ($#ARGV < 0) {
    printf "usage: orcm-sched.qn.pl <delay in sec> <options>\n";
    exit;
}

$t = shift @ARGV;

sleep($t);

exec "orcm-sched", "-mca", "orte_ess_jobid", "0", "-mca", "orte_ess_vpid", "1", @ARGV;
