/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "openrcm_config_private.h"

/* add the openrcm definitions */
#include "include/constants.h"
#include "runtime/runtime.h"

#include "orte_config.h"
#include "orte/constants.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <stdio.h>
#include <stddef.h>
#include <ctype.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif  /* HAVE_SYS_TIME_H */


#include "opal/mca/event/event.h"
#include "opal/mca/base/base.h"
#include "opal/util/output.h"
#include "opal/util/cmd_line.h"
#include "opal/util/opal_environ.h"
#include "opal/util/os_path.h"
#include "opal/util/printf.h"
#include "opal/util/argv.h"
#include "opal/runtime/opal.h"
#include "opal/mca/base/mca_base_param.h"
#include "opal/util/daemon_init.h"
#include "opal/dss/dss.h"
#include "opal/mca/sysinfo/sysinfo.h"

#include "orte/constants.h"
#include "orte/util/show_help.h"
#include "orte/util/proc_info.h"
#include "orte/util/session_dir.h"
#include "orte/util/name_fns.h"
#include "orte/util/nidmap.h"
#include "orte/runtime/orte_locks.h"
#include "orte/runtime/orte_quit.h"
#include "orte/mca/rml/base/rml_contact.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/odls/odls.h"
#include "orte/mca/odls/base/base.h"
#include "orte/mca/plm/plm.h"
#include "orte/mca/plm/base/plm_private.h"
#include "orte/mca/ras/ras.h"
#include "orte/mca/routed/routed.h"

#include "mca/pnp/pnp.h"
#include "mca/pnp/base/public.h"
#include "runtime/runtime.h"
/*
 * Globals
 */
static bool orcmd_spin_flag=false;

static int orcmd_comm(orte_process_name_t *recipient,
                      opal_buffer_t *buf, orte_rml_tag_t tag,
                      orte_default_cbfunc_t cbfunc);

static struct {
    bool help;
    bool daemonize;
    char *hnp_uri;
} my_globals;

/*
 * define the orcmd context table for obtaining parameters
 */
opal_cmd_line_init_t orte_cmd_line_opts[] = {
    /* Various "obvious" options */
    { NULL, NULL, NULL, 'h', NULL, "help", 0,
      &my_globals.help, OPAL_CMD_LINE_TYPE_BOOL,
      "This help message" },

    { "orte", "daemon_spin", NULL, 's', NULL, "spin", 0,
      &orcmd_spin_flag, OPAL_CMD_LINE_TYPE_BOOL,
      "Have the scheduler spin until we can connect a debugger to it" },

    { "orte", "debug", NULL, 'd', NULL, "debug", 0,
      NULL, OPAL_CMD_LINE_TYPE_BOOL,
      "Debug the OpenRTE" },
        
    { "orte", "daemonize", NULL, '\0', NULL, "daemonize", 0,
      &my_globals.daemonize, OPAL_CMD_LINE_TYPE_BOOL,
      "Daemonize the scheduler into the background" },

    { "tmpdir", "base", NULL, '\0', NULL, "tmpdir", 1,
      NULL, OPAL_CMD_LINE_TYPE_STRING,
      "Set the root for the session directory tree" },

    { NULL, NULL, NULL, '\0', "uri", "uri", 1,
      &my_globals.hnp_uri, OPAL_CMD_LINE_TYPE_STRING,
      "The uri of the ORCM DVM [uri or file:name of file containing it]" },
    
    { "orcm", "sched", "kill_dvm", '\0', "kill-dvm", "kill-dvm", 0,
      NULL, OPAL_CMD_LINE_TYPE_BOOL,
      "Kill daemons in DVM upon termination [default: no]" },
    
    /* End of list */
    { NULL, NULL, NULL, '\0', NULL, NULL, 0,
      NULL, OPAL_CMD_LINE_TYPE_NULL, NULL }
};

int main(int argc, char *argv[])
{
    int ret = 0;
    opal_cmd_line_t *cmd_line = NULL;
    int i;
    opal_buffer_t *buffer;
    char *tmp_env_var = NULL;
    char hostname[ORTE_MAX_HOSTNAME_SIZE];
    char *uri;
    struct hostent *h;
    char *haddr=NULL;

    /* initialize the globals */
    memset(&my_globals, 0, sizeof(my_globals));
    
    /* setup to check common command line options that just report and die */
    cmd_line = OBJ_NEW(opal_cmd_line_t);
    opal_cmd_line_create(cmd_line, orte_cmd_line_opts);
    mca_base_cmd_line_setup(cmd_line);
    if (ORTE_SUCCESS != (ret = opal_cmd_line_parse(cmd_line, false,
                                                   argc, argv))) {
        fprintf(stderr, "Cannot process cmd line - use --help for assistance\n");
        return ret;
    }

    /*
     * Since this process can now handle MCA/GMCA parameters, make sure to
     * process them.
     */
    mca_base_cmd_line_process_args(cmd_line, &environ, &environ);
    
    /*
     * NOTE: (JJH)
     *  We need to allow 'mca_base_cmd_line_process_args()' to process command
     *  line arguments *before* calling opal_init_util() since the command
     *  line could contain MCA parameters that affect the way opal_init_util()
     *  functions. AMCA parameters are one such option normally received on the
     *  command line that affect the way opal_init_util() behaves.
     *  It is "safe" to call mca_base_cmd_line_process_args() before 
     *  opal_init_util() since mca_base_cmd_line_process_args() does *not*
     *  depend upon opal_init_util() functionality.
     */
    if (OPAL_SUCCESS != orcm_init_util()) {
        fprintf(stderr, "ORCM failed to initialize -- orcmd aborting\n");
        exit(1);
    }
    
    /* check for help request */
    if (my_globals.help) {
        char *args = NULL;
        args = opal_cmd_line_get_usage_msg(cmd_line);
        orte_show_help("help-orcmd.txt", "usage", true, args);
        free(args);
        return 1;
    }

    /* see if they want us to spin until they can connect a debugger to us */
    i=0;
    while (orcmd_spin_flag) {
        i++;
        if (1000 < i) i=0;        
    }
    
    /* if we were given HNP contact info, parse it and
     * setup the process_info struct with that info
     */
    if (NULL != my_globals.hnp_uri) {
        if (0 == strncmp(my_globals.hnp_uri, "file", strlen("file")) ||
            0 == strncmp(my_globals.hnp_uri, "FILE", strlen("FILE"))) {
            char input[1024], *filename;
            FILE *fp;
            
            /* it is a file - get the filename */
            filename = strchr(my_globals.hnp_uri, ':');
            if (NULL == filename) {
                /* filename is not correctly formatted */
                orte_show_help("help-openrcm-runtime.txt", "hnp-filename-bad", true, "uri", my_globals.hnp_uri);
                goto DONE;
            }
            ++filename; /* space past the : */
            
            if (0 >= strlen(filename)) {
                /* they forgot to give us the name! */
                orte_show_help("help-openrcm-runtime.txt", "hnp-filename-bad", true, "uri", my_globals.hnp_uri);
                goto DONE;
            }
            
            /* open the file and extract the uri */
            fp = fopen(filename, "r");
            if (NULL == fp) { /* can't find or read file! */
                orte_show_help("help-openrcm-runtime.txt", "hnp-filename-access", true, filename);
                goto DONE;
            }
            if (NULL == fgets(input, 1024, fp)) {
                /* something malformed about file */
                fclose(fp);
                orte_show_help("help-openrcm-runtime.txt", "hnp-file-bad", true, filename);
                goto DONE;
            }
            input[strlen(input)-1] = '\0';  /* remove newline */
            /* put into the process info struct */
            orte_process_info.my_hnp_uri = strdup(input);
        } else {
            /* should just be the uri itself */
            orte_process_info.my_hnp_uri = strdup(my_globals.hnp_uri);
        }
    }

    /* init the ORCM library */
    if (ORCM_SUCCESS != (ret = orcm_init(ORCM_SCHEDULER))) {
        fprintf(stderr, "Failed to init: error %d\n", ret);
        exit(1);
    }
    
    /* detach from controlling terminal
     * otherwise, remain attached so output can get to us
     */
    if (my_globals.daemonize) {
        opal_daemon_init(NULL);
    } else {
        /* set the local debug verbosity */
        orcm_debug_output = 5;
    }

    /* wait to hear we are done */
    opal_event_dispatch(opal_event_base);
    return ret;

    /* should never get here, but if we do... */
 DONE:
    /* cleanup any lingering session directories */
    orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
    
    /* Finalize and clean up ourselves */
    orcm_finalize();
    return ret;
}

static int orcmd_comm(orte_process_name_t *recipient,
                      opal_buffer_t *buf, orte_rml_tag_t tag,
                      orte_default_cbfunc_t cbfunc)
{
    opal_output(0, "%s comm to %s tag %d", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_NAME_PRINT(recipient), tag);
    return ORCM_SUCCESS;
}
