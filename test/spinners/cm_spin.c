/* -*- C -*-
 *
 * $HEADER$
 *
 * A program that just spins - provides mechanism for testing user-driven
 * abnormal program termination
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "orte/util/proc_info.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"
#include "orte/runtime/runtime.h"
#include "orte/mca/errmgr/errmgr.h"

int main(int argc, char* argv[])
{
    int i, j, maxrun;
    double pi;
    float fail, randval;
    struct timeval tp;
    char hostname[1024];
    
    /* seed the random number generator */
    gettimeofday (&tp, NULL);
    srand (tp.tv_usec);
    randval = rand();
    fail = randval/RAND_MAX;
    maxrun = fail * 1000000;
    if (maxrun < 10) {
        maxrun = 10;
    }
gethostname(hostname, 1024);
    
    orte_init(NULL, NULL, ORTE_PROC_NON_MPI);

fprintf(stderr, "%s is on node %s\n", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), hostname);
    sleep(1);
    i = 0;
    while (i < maxrun) {
        i++;
        for (j=0; j < 100; j++) {
            pi = (i*j) / 3.14159256;
        }
    }

    orte_errmgr.abort(1,  NULL);
}
