#!/usr/bin/perl
#
# Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved. 
# $COPYRIGHT$
# 
# Additional copyrights may follow

# Command line parameters

use Getopt::Long;

my $ok = Getopt::Long::GetOptions("help|h" => \$help_arg,
                                  "qn=s" => \$qn_arg,
                                  "no-confd" => \$no_confd_arg,
                                  "jobfam=s" => \$jobfam,
    );

if (!$ok || $help_arg) {
    print "Invalid command line argument.\n\n"
        if (!$ok);
    print "\norcmrun.pl: Launches orcm daemons and scheduler on nodes listed\n on command line. Options:
  --help | -h                   This help list
  --qn | -q                     Specify a qnanny configuration file
  --no-confd | -no-confd        Disable confd connection
  --jobfam | -jobfam            Job family to use for isolation\n";
    exit($ok ? 0 : 1);
}

if ($no_confd_arg) {
$confd = "-mca orcm_cfgi ^confd";
} else {
$confd = " ";
}

if ($jobfam) {
$use_job = $jobfam;
} else {
$use_job = 0;
}

$i = 2;  # daemons start at vpid=2

# if no hostnames were provided, we just exec things locally
if ($#ARGV < 0) {
    if ($qn_arg) {
        $pid = fork();
        if ($pid == 0) {
            system("qn unix nodaemon qn { config-file $qn_arg }");
            exit;
        }
    } else {
        $pid = fork();
        if ($pid == 0) {
            if ($jobfam) {
                system("orcmd -mca orte_ess_job_family $use_job -mca orte_ess_vpid 2");
            } else {
                system("orcmd -mca orte_ess_vpid 2");
            }
            exit;
        }
# launch the scheduler
        $pid = fork();
        if ($pid == 0) {
            if ($jobfam) {
                system("orcm-sched -mca orte_ess_job_family $use_job $confd");
            } else {
                system("orcm-sched $confd");
            }
            exit;
        }
    }
} else {
# ssh a daemon onto every node
    while ($h = shift @ARGV) {
        printf $h . "\n";

        if ($qn_arg) {
            $pid = fork();
            if ($pid == 0) {
                system("ssh $h qn unix nodaemon qn { config-file $qn_arg }");
                exit;
            }
        } else {
            # save the first location
            if ($i == 2) {
                $hnp = $h;
            }

            $pid = fork();
            if ($pid == 0) {
                if ($jobfam) {
                    system("ssh $h orcmd -mca orte_ess_job_family $use_job -mca orte_ess_vpid $i");
                } else {
                    system("ssh $h orcmd -mca orte_ess_vpid $i");
                }
                exit;
            }
            $i++;
        }
    }
    if (! $qn_arg) {
# now start the scheduler on the first node
        $pid = fork();
        if ($pid == 0) {
            if ($jobfam) {
                system("ssh $hnp orcm-sched -mca orte_ess_job_family $use_job $confd");
            } else {
                system("ssh $hnp orcm-sched $confd");
            }
            exit;
        }
    }
}


# stay alive until killed so that the remote procs don't die
while(1) { sleep(60); }

