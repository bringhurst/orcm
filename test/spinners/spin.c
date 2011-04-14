/* -*- C -*-
 *
 * $HEADER$
 *
 * A program that just spins - provides mechanism for testing user-driven
 * abnormal program termination
 */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char* argv[])
{
    int j;
    double pi;
    char hostname[1024];
    
    gethostname(hostname, 1024);
    
    fprintf(stderr, "%d is on node %s\n", getpid(), hostname);
    sleep(1);
    while (1) {
        for (j=0; j < 100; j++) {
            pi = j / 3.14159256;
        }
    }
}
