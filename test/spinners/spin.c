/* -*- C -*-
 *
 * $HEADER$
 *
 * A program that just spins - provides mechanism for testing user-driven
 * abnormal program termination
 */

#include <stdio.h>
#include <stdlib.h>

#include "orte/util/proc_info.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"
#include "orte/runtime/runtime.h"
#include "orte/mca/errmgr/errmgr.h"

int main(int argc, char* argv[])
{
    int j;
    double pi;
    char hostname[1024];
    
gethostname(hostname, 1024);
    
    orte_init(NULL, NULL, ORTE_PROC_NON_MPI);

fprintf(stderr, "%s is on node %s\n", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), hostname);
    sleep(1);
    while (1) {
        for (j=0; j < 100; j++) {
            pi = j / 3.14159256;
        }
    }
}
