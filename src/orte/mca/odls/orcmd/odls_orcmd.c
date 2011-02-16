/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2010 Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2007      Evergrid, Inc. All rights reserved.
 * Copyright (c) 2008-2010 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2010      IBM Corporation.  All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * There is a complicated sequence of events that occurs when the
 * parent forks a child process that is intended to launch the target
 * executable.
 *
 * Before the child process exec's the target executable, it might tri
 * to set the affinity of that new child process according to a
 * complex series of rules.  This binding may fail in a myriad of
 * different ways.  A lot of this code deals with reporting that error
 * occurately to the end user.  This is a complex task in itself
 * because the child process is not "really" an ORTE process -- all
 * error reporting must be proxied up to the parent who can use normal
 * ORTE error reporting mechanisms.
 *
 * Here's a high-level description of what is occurring in this file:
 *
 * - parent opens a pipe
 * - parent forks a child
 * - parent blocks reading on the pipe: the pipe will either close
 *   (indicating that the child successfully exec'ed) or the child will
 *   write some proxied error data up the pipe
 *
 * - the child tries to set affinity and do other housekeeping in
 *   preparation of exec'ing the target executable
 * - if the child fails anywhere along the way, it sends a message up
 *   the pipe to the parent indicating what happened -- including a 
 *   rendered error message detailing the problem (i.e., human-readable).
 * - it is important that the child renders the error message: there
 *   are so many errors that are possible that the child is really the
 *   only entity that has enough information to make an accuate error string
 *   to report back to the user.
 * - the parent reads this message + rendered string in and uses ORTE
 *   reporting mechanisms to display it to the user
 * - if the problem was only a warning, the child continues processing
 *   (potentially eventually exec'ing the target executable).
 * - if the problem was an error, the child exits and the parent
 *   handles the death of the child as appropriate (i.e., this ODLS
 *   simply reports the error -- other things decide what to do).
 */

#include "orte_config.h"
#include "orte/constants.h"
#include "orte/types.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#include <signal.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif  /* HAVE_SYS_STAT_H */
#ifdef HAVE_STDARG_H
#include <stdarg.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "opal/mca/maffinity/base/base.h"
#include "opal/mca/paffinity/base/base.h"
#include "opal/class/opal_pointer_array.h"
#include "opal/util/opal_environ.h"
#include "opal/util/show_help.h"
#include "opal/util/fd.h"

#include "orte/util/show_help.h"
#include "orte/runtime/orte_wait.h"
#include "orte/runtime/orte_globals.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/ess/ess.h"
#include "orte/mca/iof/base/iof_base_setup.h"
#include "orte/mca/plm/plm.h"
#include "orte/util/name_fns.h"
#include "orte/mca/rmaps/rmaps_types.h"

#include "orte/mca/odls/base/base.h"
#include "orte/mca/odls/base/odls_private.h"
#include "orte/mca/odls/orcmd/odls_orcmd.h"

/*
 * Struct written up the pipe from the child to the parent.
 */
typedef struct {
    /* True if the child has died; false if this is just a warning to
       be printed. */
    bool fatal;
    /* Relevant only if fatal==true */
    int exit_status;

    /* Length of the strings that are written up the pipe after this
       struct */
    int file_str_len;
    int topic_str_len;
    int msg_str_len;
} pipe_err_msg_t;

/* 
 * Max length of strings from the pipe_err_msg_t
 */
#define MAX_FILE_LEN 511
#define MAX_TOPIC_LEN MAX_FILE_LEN

/*
 * Module functions (function pointers used in a struct)
 */
static int get_add_procs_data(opal_buffer_t *data, orte_jobid_t job);
static int launch_local_procs(opal_buffer_t *data);
static int kill_local_procs(opal_pointer_array_t *procs);
static int signal_local_procs(const orte_process_name_t *proc, int32_t signal);
static int restart_proc(orte_odls_child_t *child);

/*
 * Explicitly declared functions so that we can get the noreturn
 * attribute registered with the compiler.
 */
static void send_error_show_help(int fd, int exit_status, 
                                 const char *file, const char *topic, ...)
    __opal_attribute_noreturn__;
static int do_child(orte_app_context_t* context,
                    orte_odls_child_t *child,
                    char **environ_copy,
                    orte_odls_job_t *jobdat, int write_fd,
                    orte_iof_base_io_conf_t opts)
    __opal_attribute_noreturn__;

static int construct_child_list(opal_buffer_t *data, orte_jobid_t *job);

/*
 * Module
 */
orte_odls_base_module_t orte_odls_orcmd_module = {
    get_add_procs_data,
    launch_local_procs,
    kill_local_procs,
    signal_local_procs,
    orte_odls_base_default_deliver_message,
    orte_odls_base_default_require_sync,
    restart_proc
};

static int get_add_procs_data(opal_buffer_t *data, orte_jobid_t job)
{
    int rc;
    orte_job_t *jdata=NULL;
    orte_proc_t *proc;
    orte_job_map_t *map=NULL;
    orte_proc_state_t *states;
    orte_vpid_t *locations;
    int32_t *restarts;
    orte_app_idx_t *app_idx;
    orte_vpid_t i;
    int j;
    orte_daemon_cmd_flag_t command;

    /* get the job data pointer */
    if (NULL == (jdata = orte_get_job_data_object(job))) {
        ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
        return ORTE_ERR_BAD_PARAM;
    }
    
    /* get a pointer to the job map */
    map = jdata->map;
    /* if there is no map, just return */
    if (NULL == map) {
        return ORTE_SUCCESS;
    }
    
    /* insert an "add-procs" command here so we can cleanly process it on the
     * other end
     */
    command = ORTE_DAEMON_ADD_LOCAL_PROCS;
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, &command, 1, ORTE_DAEMON_CMD))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* pack the jobid so it can be extracted later */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, &job, 1, ORTE_JOBID))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* pack the instance so it can be extracted later */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, &jdata->instance, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* pack the job name so it can be extracted later */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, &jdata->name, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* pack the job state so it can be extracted later */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, &jdata->state, 1, ORTE_JOB_STATE))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* pack the number of nodes involved in this job */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, &map->num_nodes, 1, ORTE_STD_CNTR))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* pack the number of procs in this launch */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, &jdata->num_procs, 1, ORTE_VPID))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* pack the total slots allocated to us */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, &jdata->total_slots_alloc, 1, ORTE_STD_CNTR))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* pack the map & binding policy for this job */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, &map->policy, 1, ORTE_MAPPING_POLICY))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* pack the cpus_per_rank for this job */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, &map->cpus_per_rank, 1, OPAL_INT16))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* pack the stride for this job */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, &map->stride, 1, OPAL_INT16))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* pack the control flags for this job */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, &jdata->controls, 1, ORTE_JOB_CONTROL))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* pack the stdin target  */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, &jdata->stdin_target, 1, ORTE_VPID))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* pack whether or not process recovery is allowed for this job */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, &jdata->enable_recovery, 1, OPAL_BOOL))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* pack the number of app_contexts for this job */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, &jdata->num_apps, 1, ORTE_APP_IDX))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* pack the app_contexts for this job - we already checked early on that
     * there must be at least one, so don't bother checking here again
     */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, jdata->apps->addr, jdata->num_apps, ORTE_APP_CONTEXT))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* transfer and pack the app_idx and restart arrays for this job */
    app_idx = (orte_app_idx_t*)malloc(jdata->num_procs * sizeof(orte_app_idx_t));
    states = (orte_proc_state_t*)malloc(jdata->num_procs * sizeof(orte_proc_state_t));
    locations = (orte_vpid_t*)malloc(jdata->num_procs * sizeof(orte_vpid_t));
    restarts = (int32_t*)malloc(jdata->num_procs * sizeof(int32_t));
    for (j=0, i=0; i < jdata->num_procs && j < jdata->procs->size; j++) {
        if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, j))) {
            continue;
        }
        app_idx[i] = proc->app_idx;
        if (NULL == proc->node || NULL == proc->node->daemon) {
            /* got an error */
            opal_output(0, "%s PROC WITH NO NODE OR DAEMON",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            free(app_idx);
            free(states);
            free(locations);
            return ORTE_ERR_BAD_PARAM;
        }
        locations[i] = proc->node->daemon->name.vpid;
        restarts[i] = proc->restarts;
        states[i++] = proc->state;
    }
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, app_idx, jdata->num_procs, ORTE_APP_IDX))) {
        ORTE_ERROR_LOG(rc);
        free(app_idx);
        free(states);
        free(locations);
        return rc;
    }
    free(app_idx);
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, states, jdata->num_procs, ORTE_PROC_STATE))) {
        ORTE_ERROR_LOG(rc);
        free(states);
        free(locations);
        return rc;
    }
    free(states);
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, locations, jdata->num_procs, ORTE_VPID))) {
        ORTE_ERROR_LOG(rc);
        free(locations);
        return rc;
    }
    free(locations);
    if (ORTE_SUCCESS != (rc = opal_dss.pack(data, restarts, jdata->num_procs, OPAL_INT32))) {
        ORTE_ERROR_LOG(rc);
        free(restarts);
        return rc;
    }
    free(restarts);
        
    return ORTE_SUCCESS;
}


static bool child_died(orte_odls_child_t *child)
{
    time_t end;
    pid_t ret;
    struct timeval t;
    fd_set bogus;
        
    end = time(NULL) + orte_odls_globals.timeout_before_sigkill;
    do {
        ret = waitpid(child->pid, &child->exit_code, WNOHANG);
        if (child->pid == ret) {
            OPAL_OUTPUT_VERBOSE((2, orte_odls_globals.output,
                                 "%s odls:orcmd:WAITPID INDICATES PROC %d IS DEAD",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)(child->pid)));
            /* It died -- return success */
            return true;
        } else if (0 == ret) {
            /* with NOHANG specified, if a process has already exited
             * while waitpid was registered, then waitpid returns 0
             * as there is no error - this is a race condition problem
             * that occasionally causes us to incorrectly report a proc
             * as refusing to die. Unfortunately, errno may not be reset
             * by waitpid in this case, so we cannot check it - just assume
             * the proc has indeed died
             */
            OPAL_OUTPUT_VERBOSE((2, orte_odls_globals.output,
                                 "%s odls:orcmd:WAITPID INDICATES PROC %d HAS ALREADY EXITED",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)(child->pid)));
            return true;
        } else if (-1 == ret && ECHILD == errno) {
            /* The pid no longer exists, so we'll call this "good
               enough for government work" */
            OPAL_OUTPUT_VERBOSE((2, orte_odls_globals.output,
                                 "%s odls:orcmd:WAITPID INDICATES PID %d NO LONGER EXISTS",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)(child->pid)));
            return true;
        }
        
        /* Bogus delay for 1 usec (sched_yeild() -- even if we have it
           -- changed behavior in 2.6.3x Linux flavors to be
           undesirable. */
        t.tv_sec = 0;
        t.tv_usec = 1;
        FD_ZERO(&bogus);
        FD_SET(0, &bogus);
        select(1, &bogus, NULL, NULL, &t);
    } while (time(NULL) < end);

    /* The child didn't die, so return false */
    return false;
}

static int kill_local(pid_t pid, int signum)
{
    if (orte_forward_job_control) {
        pid = -pid;
    }
    if (0 != kill(pid, signum)) {
        if (ESRCH != errno) {
            OPAL_OUTPUT_VERBOSE((2, orte_odls_globals.output,
                                 "%s odls:orcmd:SENT KILL %d TO PID %d GOT ERRNO %d",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), signum, (int)pid, errno));
            return errno;
        }
    }
    OPAL_OUTPUT_VERBOSE((2, orte_odls_globals.output,
                         "%s odls:orcmd:SENT KILL %d TO PID %d SUCCESS",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), signum, (int)pid));
    return 0;
}

int kill_local_procs(opal_pointer_array_t *procs)
{
    int rc;
    
    if (ORTE_SUCCESS != (rc = orte_odls_base_default_kill_local_procs(procs, kill_local, child_died))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    return ORTE_SUCCESS;
}


static void set_handler_default(int sig)
{
    struct sigaction act;

    act.sa_handler = SIG_DFL;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);

    sigaction(sig, &act, (struct sigaction *)0);
}

/*
 * Internal function to write a rendered show_help message back up the
 * pipe to the waiting parent.
 */
static int write_help_msg(int fd, pipe_err_msg_t *msg, const char *file,
                          const char *topic, va_list ap)
{
    int ret;
    char *str;

    if (NULL == file || NULL == topic) {
        return OPAL_ERR_BAD_PARAM;
    }

    str = opal_show_help_vstring(file, topic, true, ap);

    msg->file_str_len = (int) strlen(file);
    if (msg->file_str_len > MAX_FILE_LEN) {
        ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
        return ORTE_ERR_BAD_PARAM;
    }
    msg->topic_str_len = (int) strlen(topic);
    if (msg->topic_str_len > MAX_TOPIC_LEN) {
        ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
        return ORTE_ERR_BAD_PARAM;
    }
    msg->msg_str_len = (int) strlen(str);

    /* Only keep writing if each write() succeeds */
    if (OPAL_SUCCESS != (ret = opal_fd_write(fd, sizeof(*msg), msg))) {
        goto out;
    }
    if (msg->file_str_len > 0 &&
        OPAL_SUCCESS != (ret = opal_fd_write(fd, msg->file_str_len, file))) {
        goto out;
    }
    if (msg->topic_str_len > 0 &&
        OPAL_SUCCESS != (ret = opal_fd_write(fd, msg->topic_str_len, topic))) {
        goto out;
    }
    if (msg->msg_str_len > 0 &&
        OPAL_SUCCESS != (ret = opal_fd_write(fd, msg->msg_str_len, str))) {
        goto out;
    }

 out:
    free(str);
    return ret;
}


/* Called from the child to send a warning show_help message up the
   pipe to the waiting parent. */
static int send_warn_show_help(int fd, const char *file, 
                               const char *topic, ...)
{
    int ret;
    va_list ap;
    pipe_err_msg_t msg;

    msg.fatal = false;
    msg.exit_status = 0; /* ignored */

    /* Send it */
    va_start(ap, topic);
    ret = write_help_msg(fd, &msg, file, topic, ap);
    va_end(ap);

    return ret;
}


/* Called from the child to send an error message up the pipe to the
   waiting parent. */
static void send_error_show_help(int fd, int exit_status,
                                 const char *file, const char *topic, ...)
{
    int ret;
    va_list ap;
    pipe_err_msg_t msg;

    msg.fatal = true;
    msg.exit_status = exit_status;

    /* Send it */
    va_start(ap, topic);
    ret = write_help_msg(fd, &msg, file, topic, ap);
    va_end(ap);

    exit(exit_status);
}


static int do_child(orte_app_context_t* context,
                    orte_odls_child_t *child,
                    char **environ_copy,
                    orte_odls_job_t *jobdat, int write_fd,
                    orte_iof_base_io_conf_t opts)
{
    int i;
    sigset_t sigs;
    long fd, fdmax = sysconf(_SC_OPEN_MAX);
    bool paffinity_enabled = false;
    char *param, *tmp;
    opal_paffinity_base_cpu_set_t mask;
    
    if (orte_forward_job_control) {
        /* Set a new process group for this child, so that a
           SIGSTOP can be sent to it without being sent to the
           orted. */
        setpgid(0, 0);
    }
    
    /* Setup the pipe to be close-on-exec */
    fcntl(write_fd, F_SETFD, FD_CLOEXEC);

    if (NULL != child) {
        /* setup stdout/stderr so that any error messages that we
           may print out will get displayed back at orterun.
           
           NOTE: Definitely do this AFTER we check contexts so
           that any error message from those two functions doesn't
           come out to the user. IF we didn't do it in this order,
           THEN a user who gives us a bad executable name or
           working directory would get N error messages, where
           N=num_procs. This would be very annoying for large
           jobs, so instead we set things up so that orterun
           always outputs a nice, single message indicating what
           happened
        */
        if (ORTE_SUCCESS != (i = orte_iof_base_setup_child(&opts, &environ_copy))) {
            ORTE_ERROR_LOG(i);
            send_error_show_help(write_fd, 1, 
                                 "help-orte-odls-orcmd.txt", 
                                 "iof setup failed",
                                 orte_process_info.nodename, context->app);
            /* Does not return */
        }
        
    } else if (!(ORTE_JOB_CONTROL_FORWARD_OUTPUT & jobdat->controls)) {
        /* tie stdin/out/err/internal to /dev/null */
        int fdnull;
        for (i=0; i < 3; i++) {
            fdnull = open("/dev/null", O_RDONLY, 0);
            if (fdnull > i && i != write_fd) {
                dup2(fdnull, i);
            }
            close(fdnull);
        }
        fdnull = open("/dev/null", O_RDONLY, 0);
        if (fdnull > opts.p_internal[1]) {
            dup2(fdnull, opts.p_internal[1]);
        }
        close(fdnull);
    }
    
    /* close all file descriptors w/ exception of stdin/stdout/stderr,
       the pipe used for the IOF INTERNAL messages, and the pipe up to
       the parent. */
    for(fd=3; fd<fdmax; fd++) {
        if (fd != opts.p_internal[1] && fd != write_fd) {
            close(fd);
        }
    }
    
    if (context->argv == NULL) {
        context->argv = malloc(sizeof(char*)*2);
        context->argv[0] = strdup(context->app);
        context->argv[1] = NULL;
    }
    
    /* Set signal handlers back to the default.  Do this close to
       the exev() because the event library may (and likely will)
       reset them.  If we don't do this, the event library may
       have left some set that, at least on some OS's, don't get
       reset via fork() or exec().  Hence, the launched process
       could be unkillable (for example). */
    
    set_handler_default(SIGTERM);
    set_handler_default(SIGINT);
    set_handler_default(SIGHUP);
    set_handler_default(SIGPIPE);
    set_handler_default(SIGCHLD);
    
    /* Unblock all signals, for many of the same reasons that we
       set the default handlers, above.  This is noticable on
       Linux where the event library blocks SIGTERM, but we don't
       want that blocked by the launched process. */
    sigprocmask(0, 0, &sigs);
    sigprocmask(SIG_UNBLOCK, &sigs, 0);
    
    /* Exec the new executable */
    
    if (10 < opal_output_get_verbosity(orte_odls_globals.output)) {
        int jout;
        opal_output(0, "%s STARTING %s", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), context->app);
        for (jout=0; NULL != context->argv[jout]; jout++) {
            opal_output(0, "%s\tARGV[%d]: %s", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), jout, context->argv[jout]);
        }
        for (jout=0; NULL != environ_copy[jout]; jout++) {
            opal_output(0, "%s\tENVIRON[%d]: %s", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), jout, environ_copy[jout]);
        }
    }
    
    execve(context->app, context->argv, environ_copy);
    send_error_show_help(write_fd, 1, 
                         "help-orte-odls-orcmd.txt", "execve error",
                         context->app, strerror(errno));
    /* Does not return */
}


static int do_parent(orte_app_context_t* context,
                     orte_odls_child_t *child,
                     char **environ_copy,
                     orte_odls_job_t *jobdat, int read_fd,
                     orte_iof_base_io_conf_t opts)
{
    int rc;
    pipe_err_msg_t msg;
    char file[MAX_FILE_LEN + 1], topic[MAX_TOPIC_LEN + 1], *str = NULL;

    if (NULL != child && (ORTE_JOB_CONTROL_FORWARD_OUTPUT & jobdat->controls)) {
        /* connect endpoints IOF */
        rc = orte_iof_base_setup_parent(child->name, &opts);
        if (ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            close(read_fd);

            if (NULL != child) {
                child->state = ORTE_PROC_STATE_UNDEF;
            }
            return rc;
        }
    }
    
    /* Block reading a message from the pipe */
    while (1) {
        rc = opal_fd_read(read_fd, sizeof(msg), &msg);

        /* If the pipe closed, then the child successfully launched */
        if (OPAL_ERR_TIMEOUT == rc) {
            break;
        }
        
        /* If Something Bad happened in the read, error out */
        if (OPAL_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            close(read_fd);
            
            if (NULL != child) {
                child->state = ORTE_PROC_STATE_UNDEF;
            }
            return rc;
        }

        /* Otherwise, we got a warning or error message from the child */
        if (NULL != child) {
            child->alive = msg.fatal ? 0 : 1;
        }

        /* Read in the strings; ensure to terminate them with \0 */
        if (msg.file_str_len > 0) {
            rc = opal_fd_read(read_fd, msg.file_str_len, file);
            if (OPAL_SUCCESS != rc) {
                orte_show_help("help-orte-odls-orcmd.txt", "syscall fail", 
                               true,
                               orte_process_info.nodename, context->app,
                               "opal_fd_read", __FILE__, __LINE__);
                if (NULL != child) {
                    child->state = ORTE_PROC_STATE_UNDEF;
                }
                return rc;
            }
            file[msg.file_str_len] = '\0';
        }
        if (msg.topic_str_len > 0) {
            rc = opal_fd_read(read_fd, msg.topic_str_len, topic);
            if (OPAL_SUCCESS != rc) {
                orte_show_help("help-orte-odls-orcmd.txt", "syscall fail", 
                               true,
                               orte_process_info.nodename, context->app,
                               "opal_fd_read", __FILE__, __LINE__);
                if (NULL != child) {
                    child->state = ORTE_PROC_STATE_UNDEF;
                }
                return rc;
            }
            topic[msg.topic_str_len] = '\0';
        }
        if (msg.msg_str_len > 0) {
            str = calloc(1, msg.msg_str_len + 1);
            if (NULL == str) {
                orte_show_help("help-orte-odls-orcmd.txt", "syscall fail", 
                               true,
                               orte_process_info.nodename, context->app,
                               "opal_fd_read", __FILE__, __LINE__);
                if (NULL != child) {
                    child->state = ORTE_PROC_STATE_UNDEF;
                }
                return rc;
            }
            rc = opal_fd_read(read_fd, msg.msg_str_len, str);
        }

        /* Print out what we got.  We already have a rendered string,
           so use orte_show_help_norender(). */
        if (msg.msg_str_len > 0) {
            orte_show_help_norender(file, topic, false, str);
            free(str);
            str = NULL;
        }

        /* If msg.fatal is true, then the child exited with an error.
           Otherwise, whatever we just printed was a warning, so loop
           around and see what else is on the pipe (or if the pipe
           closed, indicating that the child launched
           successfully). */
        if (msg.fatal) {
            if (NULL != child) {
                child->state = ORTE_PROC_STATE_FAILED_TO_START;
                child->alive = false;
            }
            close(read_fd);
            return ORTE_SUCCESS;
        }
    }

    /* If we got here, it means that the pipe closed without
       indication of a fatal error, meaning that the child process
       launched successfully. */
    if (NULL != child) {
        child->state = ORTE_PROC_STATE_LAUNCHED;
        child->alive = true;
    }
    close(read_fd);
    
    return ORTE_SUCCESS;
}


/**
 *  Fork/exec the specified processes
 */
static int fork_local_proc(orte_app_context_t* context,
                           orte_odls_child_t *child,
                           char **environ_copy,
                           orte_odls_job_t *jobdat)
{
    orte_iof_base_io_conf_t opts;
    int rc, p[2];
    pid_t pid;
    
    if (NULL != child) {
        /* should pull this information from MPIRUN instead of going with
           default */
        opts.usepty = OPAL_ENABLE_PTY_SUPPORT;
        
        /* do we want to setup stdin? */
        if (NULL != child &&
            (jobdat->stdin_target == ORTE_VPID_WILDCARD || child->name->vpid == jobdat->stdin_target)) {
            opts.connect_stdin = true;
        } else {
            opts.connect_stdin = false;
        }
        
        if (ORTE_SUCCESS != (rc = orte_iof_base_setup_prefork(&opts))) {
            ORTE_ERROR_LOG(rc);
            if (NULL != child) {
                child->state = ORTE_PROC_STATE_FAILED_TO_START;
                child->exit_code = rc;
            }
            return rc;
        }
    }
    
    /* A pipe is used to communicate between the parent and child to
       indicate whether the exec ultimately succeeded or failed.  The
       child sets the pipe to be close-on-exec; the child only ever
       writes anything to the pipe if there is an error (e.g.,
       executable not found, exec() fails, etc.).  The parent does a
       blocking read on the pipe; if the pipe closed with no data,
       then the exec() succeeded.  If the parent reads something from
       the pipe, then the child was letting us know why it failed. */
    if (pipe(p) < 0) {
        ORTE_ERROR_LOG(ORTE_ERR_SYS_LIMITS_PIPES);
        if (NULL != child) {
            child->state = ORTE_PROC_STATE_FAILED_TO_START;
            child->exit_code = ORTE_ERR_SYS_LIMITS_PIPES;
        }
        return ORTE_ERR_SYS_LIMITS_PIPES;
    }
    
    /* Fork off the child */
    pid = fork();
    if (NULL != child) {
        child->pid = pid;
    }
    
    if (pid < 0) {
        ORTE_ERROR_LOG(ORTE_ERR_SYS_LIMITS_CHILDREN);
        if (NULL != child) {
            child->state = ORTE_PROC_STATE_FAILED_TO_START;
            child->exit_code = ORTE_ERR_SYS_LIMITS_CHILDREN;
        }
        return ORTE_ERR_SYS_LIMITS_CHILDREN;
    }
    
    if (pid == 0) {
	close(p[0]);
        do_child(context, child, environ_copy, jobdat, p[1], opts);
        /* Does not return */
    } 

    close(p[1]);
    return do_parent(context, child, environ_copy, jobdat, p[0], opts);
}


/**
 * Launch all processes allocated to the current node.
 */

int launch_local_procs(opal_buffer_t *data)
{
    int rc;
    orte_jobid_t job;
    orte_job_t *jdata;

    /* construct the list of children we are to launch */
    if (ORTE_SUCCESS != (rc = construct_child_list(data, &job))) {
        OPAL_OUTPUT_VERBOSE((2, orte_odls_globals.output,
                             "%s odls:orcmd:launch:local failed to construct child list on error %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_ERROR_NAME(rc)));
        goto CLEANUP;
    }
    
    /* launch the local procs */
    if (ORTE_SUCCESS != (rc = orte_odls_base_default_launch_local(job, fork_local_proc))) {
        OPAL_OUTPUT_VERBOSE((2, orte_odls_globals.output,
                             "%s odls:orcmd:launch:local failed to launch on error %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_ERROR_NAME(rc)));
    }
    
CLEANUP:
    return rc;
}


/**
 * Send a sigal to a pid.  Note that if we get an error, we set the
 * return value and let the upper layer print out the message.  
 */
static int send_signal(pid_t pid, int signal)
{
    int rc = ORTE_SUCCESS;
    
    OPAL_OUTPUT_VERBOSE((1, orte_odls_globals.output,
                         "%s sending signal %d to pid %ld",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         signal, (long)pid));

    if (orte_forward_job_control) {
	/* Send the signal to the process group rather than the
	   process.  The child is the leader of its process group. */
	pid = -pid;
    }
    if (kill(pid, signal) != 0) {
        switch(errno) {
            case EINVAL:
                rc = ORTE_ERR_BAD_PARAM;
                break;
            case ESRCH:
                /* This case can occur when we deliver a signal to a
                   process that is no longer there.  This can happen if
                   we deliver a signal while the job is shutting down. 
                   This does not indicate a real problem, so just 
                   ignore the error.  */
                break;
            case EPERM:
                rc = ORTE_ERR_PERM;
                break;
            default:
                rc = ORTE_ERROR;
        }
    }
    
    return rc;
}

static int signal_local_procs(const orte_process_name_t *proc, int32_t signal)
{
    int rc;
    
    if (ORTE_SUCCESS != (rc = orte_odls_base_default_signal_local_procs(proc, signal, send_signal))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    return ORTE_SUCCESS;
}

static int restart_proc(orte_odls_child_t *child)
{
    int rc;
    
    /* restart the local proc */
    if (ORTE_SUCCESS != (rc = orte_odls_base_default_restart_proc(child, fork_local_proc))) {
        OPAL_OUTPUT_VERBOSE((2, orte_odls_globals.output,
                             "%s odls:orcmd:restart_proc failed to launch on error %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_ERROR_NAME(rc)));
    }
    return rc;
}


static int construct_child_list(opal_buffer_t *data, orte_jobid_t *job)
{
    int rc;
    orte_vpid_t j, host_daemon;
    orte_odls_child_t *child;
    orte_std_cntr_t cnt;
    orte_process_name_t proc;
    orte_odls_job_t *jobdat=NULL;
    opal_list_item_t *item;
    orte_app_idx_t *app_idx=NULL;
    orte_proc_state_t *states=NULL;
    orte_vpid_t *locations=NULL;
    int32_t *restarts=NULL;
    char **slot_str=NULL;
    bool add_child;
    
    orte_job_t *jptr, *daemons;
    orte_proc_t *pptr, *dptr;
    orte_node_t *nptr;
    int32_t ljob;

    OPAL_OUTPUT_VERBOSE((5, orte_odls_globals.output,
                         "%s odls:constructing child list",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));

    /* unpack the jobid we are to launch */
    cnt=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, job, &cnt, ORTE_JOBID))) {
        /* if the buffer was empty, then we know that all we are doing is
         * launching debugger daemons
         */
        if (ORTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
            goto done;
        }
        *job = ORTE_JOBID_INVALID;
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    
    OPAL_OUTPUT_VERBOSE((5, orte_odls_globals.output,
                         "%s odls:construct_child_list unpacking data to launch job %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_JOBID_PRINT(*job)));
    
    /* even though we are unpacking an add_local_procs cmd, we cannot assume
     * that no job record for this jobid exists. A race condition exists that
     * could allow another daemon's procs to call us with a collective prior
     * to our unpacking add_local_procs. So lookup the job record for this jobid
     * and see if it already exists
     */
    for (item = opal_list_get_first(&orte_local_jobdata);
         item != opal_list_get_end(&orte_local_jobdata);
         item = opal_list_get_next(item)) {
        orte_odls_job_t *jdat = (orte_odls_job_t*)item;
        
        /* is this the specified job? */
        if (jdat->jobid == *job) {
            OPAL_OUTPUT_VERBOSE((5, orte_odls_globals.output,
                                 "%s odls:construct_child_list found existing jobdat for job %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_JOBID_PRINT(*job)));
            jobdat = jdat;
            break;
        }
    }
    if (NULL == jobdat) {
        /* setup jobdat object for this job */
        OPAL_OUTPUT_VERBOSE((5, orte_odls_globals.output,
                             "%s odls:construct_child_list adding new jobdat for job %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_JOBID_PRINT(*job)));
        jobdat = OBJ_NEW(orte_odls_job_t);
        jobdat->jobid = *job;
        opal_list_append(&orte_local_jobdata, &jobdat->super);
    }
    
    
    /* UNPACK JOB-SPECIFIC DATA */
    /* unpack the job instance */
    cnt=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, &jobdat->instance, &cnt, OPAL_STRING))) {
        *job = ORTE_JOBID_INVALID;
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    /* unpack the job name */
    cnt=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, &jobdat->name, &cnt, OPAL_STRING))) {
        *job = ORTE_JOBID_INVALID;
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    /* unpack the job state so we can know if this is a restart vs initial launch */
    cnt=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, &jobdat->state, &cnt, ORTE_JOB_STATE))) {
        *job = ORTE_JOBID_INVALID;
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    
    /* unpack the number of nodes involved in this job */
    cnt=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, &jobdat->num_nodes, &cnt, ORTE_STD_CNTR))) {
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }    
    /* unpack the number of procs in this launch */
    cnt=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, &jobdat->num_procs, &cnt, ORTE_VPID))) {
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }    
    /* unpack the total slots allocated to us */
    cnt=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, &jobdat->total_slots_alloc, &cnt, ORTE_STD_CNTR))) {
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    /* unpack the mapping policy for the job */
    cnt=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, &jobdat->policy, &cnt, ORTE_MAPPING_POLICY))) {
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    /* unpack the cpus/rank for the job */
    cnt=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, &jobdat->cpus_per_rank, &cnt, OPAL_INT16))) {
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    /* unpack the stride for the job */
    cnt=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, &jobdat->stride, &cnt, OPAL_INT16))) {
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    /* unpack the control flags for the job */
    cnt=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, &jobdat->controls, &cnt, ORTE_JOB_CONTROL))) {
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    /* unpack the stdin target for the job */
    cnt=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, &jobdat->stdin_target, &cnt, ORTE_VPID))) {
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    /* unpack whether or not process recovery is allowed for this job */
    cnt=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, &jobdat->enable_recovery, &cnt, OPAL_BOOL))) {
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    /* unpack the number of app_contexts for this job */
    cnt=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, &jobdat->num_apps, &cnt, ORTE_APP_IDX))) {
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    OPAL_OUTPUT_VERBOSE((5, orte_odls_globals.output,
                         "%s odls:construct_child_list unpacking %ld app_contexts",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (long)jobdat->num_apps));
    
    /* allocate space and unpack the app_contexts for this job - the HNP checked
     * that there must be at least one, so don't bother checking here again
     */
    if (NULL != jobdat->apps) {
        free(jobdat->apps);
    }
    jobdat->apps = (orte_app_context_t**)malloc(jobdat->num_apps * sizeof(orte_app_context_t*));
    if (NULL == jobdat->apps) {
        ORTE_ERROR_LOG(ORTE_ERR_OUT_OF_RESOURCE);
        goto REPORT_ERROR;
    }
    cnt = jobdat->num_apps;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, jobdat->apps, &cnt, ORTE_APP_CONTEXT))) {
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    
    /* allocate memory for app_idx */
    app_idx = (orte_app_idx_t*)malloc(jobdat->num_procs * sizeof(orte_app_idx_t));
    /* unpack app_idx in one shot */
    cnt=jobdat->num_procs;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, app_idx, &cnt, ORTE_APP_IDX))) {
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    
    /* allocate memory for states */
    states = (orte_proc_state_t*)malloc(jobdat->num_procs  * sizeof(orte_proc_state_t));
    /* unpack states in one shot */
    cnt=jobdat->num_procs;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, states, &cnt, ORTE_PROC_STATE))) {
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    
    /* allocate memory for locations */
    locations = (orte_vpid_t*)malloc(jobdat->num_procs  * sizeof(orte_vpid_t));
    /* unpack locations in one shot */
    cnt=jobdat->num_procs;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, locations, &cnt, ORTE_VPID))) {
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }
    
    /* allocate memory for restarts */
    restarts = (int32_t*)malloc(jobdat->num_procs  * sizeof(int32_t));
    /* unpack restarts in one shot */
    cnt=jobdat->num_procs;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(data, restarts, &cnt, OPAL_INT32))) {
        ORTE_ERROR_LOG(rc);
        goto REPORT_ERROR;
    }

    /* cycle thru the procs and build/update the global arrays */
    daemons = orte_get_job_data_object(ORTE_PROC_MY_NAME->jobid);
    if (NULL == (jptr = orte_get_job_data_object(jobdat->jobid))) {
        jptr = OBJ_NEW(orte_job_t);
        jptr->jobid = jobdat->jobid;
        /* store it on the global job data pool */
        ljob = ORTE_LOCAL_JOBID(jptr->jobid);
        opal_pointer_array_set_item(orte_job_data, ljob, jptr);
    }
    jptr->enable_recovery = jobdat->enable_recovery;
    for (j=0; j < jobdat->num_procs; j++) {
        if (NULL == (pptr = (orte_proc_t*)opal_pointer_array_get_item(jptr->procs, j))) {
            pptr = OBJ_NEW(orte_proc_t);
            pptr->name.jobid = jobdat->jobid;
            pptr->name.vpid = j;
            opal_pointer_array_set_item(jptr->procs, j, pptr);
        }
        pptr->local_rank = 0;
        pptr->node_rank = 0;
        pptr->state = states[j];
        pptr->app_idx = app_idx[j];
        pptr->restarts = restarts[j];
        if (NULL == (nptr = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, locations[j]))) {
            nptr = OBJ_NEW(orte_node_t);
            nptr->index = locations[j];
            opal_pointer_array_set_item(orte_node_pool, locations[j], nptr);
        }
        OBJ_RETAIN(nptr);  /* maintain accounting */
        pptr->node = nptr;
        if (NULL == (dptr = (orte_proc_t*)opal_pointer_array_get_item(daemons->procs, locations[j]))) {
            /* got BIG problem */
            opal_output(0, "%s CANNOT FIND REFERENCED DAEMON %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_VPID_PRINT(locations[j]));
            rc = ORTE_ERR_NOT_FOUND;
            goto REPORT_ERROR;
        }
        OBJ_RETAIN(dptr);
        nptr->daemon = dptr;
    }
    /* cycle through the procs and find mine */
    proc.jobid = jobdat->jobid;
    for (j=0; j < jobdat->num_procs; j++) {
        if (ORTE_PROC_STATE_INIT != states[j]) {
            continue;
        }
        proc.vpid = j;
        host_daemon = locations[j];
#if 0
        /* get the vpid of the daemon that is to host this proc */
        if (ORTE_VPID_INVALID == (host_daemon = orte_ess.proc_get_daemon(&proc))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            rc = ORTE_ERR_NOT_FOUND;
            goto REPORT_ERROR;
        }
#endif
        OPAL_OUTPUT_VERBOSE((5, orte_odls_globals.output,
                             "%s odls:constructing child list - checking proc %s on daemon %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_NAME_PRINT(&proc),
                             ORTE_VPID_PRINT(host_daemon)));

        /* does this proc belong to us? */
        if (ORTE_PROC_MY_NAME->vpid == host_daemon) {
            
            OPAL_OUTPUT_VERBOSE((5, orte_odls_globals.output,
                                 "%s odls:constructing child list - found proc %s for me!",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_NAME_PRINT(&proc)));
            
            add_child = true;
            /* if this job is restarting procs, then we need to treat things
             * a little differently. We may be adding a proc to our local
             * children (if the proc moved here from somewhere else), or we
             * may simply be restarting someone already here.
             */
            if (ORTE_JOB_STATE_RESTART == jobdat->state) {
                /* look for this job on our current list of children */
                for (item = opal_list_get_first(&orte_local_children);
                     item != opal_list_get_end(&orte_local_children);
                     item = opal_list_get_next(item)) {
                    child = (orte_odls_child_t*)item;
                    if (child->name->jobid == proc.jobid &&
                        child->name->vpid == proc.vpid) {
                        /* do not duplicate this child on the list! */
                        OPAL_OUTPUT_VERBOSE((5, orte_odls_globals.output,
                                             "proc %s is on list and is %s",
                                             ORTE_NAME_PRINT(&proc),
                                             (child->alive) ? "ALIVE" : "DEAD"));
                        add_child = false;
                        child->do_not_barrier = true;
                        child->restarts = restarts[j];
                        /* mark that this app_context is being used on this node */
                        jobdat->apps[app_idx[j]]->used_on_node = true;
                        break;
                    }
                }
            }
            
            /* if we need to add the child, do so */
            if (add_child) {
                OPAL_OUTPUT_VERBOSE((5, orte_odls_globals.output,
                                     "adding proc %s to my local list",
                                     ORTE_NAME_PRINT(&proc)));
                /* keep tabs of the number of local procs */
                jobdat->num_local_procs++;
                /* add this proc to our child list */
                child = OBJ_NEW(orte_odls_child_t);
                /* copy the name to preserve it */
                if (ORTE_SUCCESS != (rc = opal_dss.copy((void**)&child->name, &proc, ORTE_NAME))) {
                    ORTE_ERROR_LOG(rc);
                    goto REPORT_ERROR;
                }
                child->app_idx = app_idx[j];  /* save the index into the app_context objects */
                /* if the job is in restart mode, the child must not barrier when launched */
                if (ORTE_JOB_STATE_RESTART == jobdat->state) {
                    child->do_not_barrier = true;
                }
                child->restarts = restarts[j];
                if (NULL != slot_str && NULL != slot_str[j]) {
                    child->slot_list = strdup(slot_str[j]);
                }
                /* mark that this app_context is being used on this node */
                jobdat->apps[app_idx[j]]->used_on_node = true;
                /* protect operation on the global list of children */
                OPAL_THREAD_LOCK(&orte_odls_globals.mutex);
                opal_list_append(&orte_local_children, &child->super);
                opal_condition_signal(&orte_odls_globals.cond);
                OPAL_THREAD_UNLOCK(&orte_odls_globals.mutex);
            }
        }
    }
    
    /* flag that the launch msg has been processed so daemon collectives can proceed */
    OPAL_THREAD_LOCK(&jobdat->lock);
    jobdat->launch_msg_processed = true;
    opal_condition_broadcast(&jobdat->cond);
    OPAL_THREAD_UNLOCK(&jobdat->lock);
    
done:
    if (NULL != app_idx) {
        free(app_idx);
        app_idx = NULL;
    }
    if (NULL != states) {
        free(states);
        states = NULL;
    }
    if (NULL != slot_str) {
        for (j=0; j < jobdat->num_procs; j++) {
            free(slot_str[j]);
        }
        free(slot_str);
        slot_str = NULL;
    }
    
    return ORTE_SUCCESS;

REPORT_ERROR:
    /* we have to report an error back to the HNP so we don't just
     * hang. Although there shouldn't be any errors once this is
     * all debugged, it is still good practice to have a way
     * for it to happen - especially so developers don't have to
     * deal with the hang!
     */
    orte_errmgr.update_state(*job, ORTE_JOB_STATE_NEVER_LAUNCHED,
                             NULL, ORTE_PROC_STATE_UNDEF, 0, rc);
   
    if (NULL != app_idx) {
        free(app_idx);
        app_idx = NULL;
    }
    if (NULL != states) {
        free(states);
        states = NULL;
    }
    if (NULL != slot_str && NULL != jobdat) {
        for (j=0; j < jobdat->num_procs; j++) {
            if (NULL != slot_str[j]) {
                free(slot_str[j]);
            }
        }
        free(slot_str);
        slot_str = NULL;
    }
    
    return rc;
}
