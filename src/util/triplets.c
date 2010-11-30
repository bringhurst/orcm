/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "openrcm_config_private.h"
#include "constants.h"

#include <stdio.h>

#include "orte/util/name_fns.h"
#include "orte/threads/threads.h"
#include "orte/runtime/orte_globals.h"

#include "runtime/orcm_globals.h"
#include "util/triplets.h"

orcm_triplet_t* orcm_get_triplet_jobid(const orte_jobid_t jobid)
{
    int i, j;
    orcm_triplet_t *triplet;
    orcm_triplet_group_t *grp;

    /* lock the global array for our use */
    ORTE_ACQUIRE_THREAD(&orcm_triplets->ctl);

    for (i=0; i < orcm_triplets->array.size; i++) {
        if (NULL == (triplet = (orcm_triplet_t*)opal_pointer_array_get_item(&orcm_triplets->array, i))) {
            continue;
        }
        for (j=0; j < triplet->groups.size; j++) {
            if (NULL == (grp = (orcm_triplet_group_t*)opal_pointer_array_get_item(&triplet->groups, j))) {
                continue;
            }
            if (grp->jobid == jobid) {
                OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                     "%s pnp:default:get_triplet_jobid match found",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
                /* lock the triplet for use - the caller is responsible for
                 * unlocking it!
                 */
                ORTE_ACQUIRE_THREAD(&triplet->ctl);
 
                /* release the global array */
                ORTE_RELEASE_THREAD(&orcm_triplets->ctl);
                return triplet;
            }
        }
    }

    /* release the global array */
    ORTE_RELEASE_THREAD(&orcm_triplets->ctl);
    return NULL;
}

orcm_triplet_t* orcm_get_triplet_stringid(const char *stringid)
{
    int i;
    orcm_triplet_t *triplet;
    opal_pointer_array_t *array;

    /* lock the global array for our use */
    ORTE_ACQUIRE_THREAD(&orcm_triplets->ctl);

    /* if the string_id contains a wildcard, then we have to look
     * in the wildcard array
     */
    if (NULL != strchr(stringid, '@')) {
        array = &orcm_triplets->wildcards;
    } else {
        array = &orcm_triplets->array;
    }

    for (i=0; i < array->size; i++) {
        if (NULL == (triplet = (orcm_triplet_t*)opal_pointer_array_get_item(array, i))) {
            continue;
        }
        /* require an exact match */
        if (0 == strcasecmp(stringid, triplet->string_id)) {
            /* we have a match */
            OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                 "%s pnp:default:get_triplet_stringid match found",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
            /* lock the triplet for use - the caller is responsible for
             * unlocking it!
             */
            ORTE_ACQUIRE_THREAD(&triplet->ctl);
 
            /* release the global array */
            ORTE_RELEASE_THREAD(&orcm_triplets->ctl);
            return triplet;
        }
    }

    /* release the global array */
    ORTE_RELEASE_THREAD(&orcm_triplets->ctl);
    return NULL;
}

orcm_triplet_t* orcm_get_triplet(const char *app,
                                 const char *version,
                                 const char *release,
                                 bool create)
{
    int i;
    orcm_triplet_t *triplet;
    char *string_id;
    opal_pointer_array_t *array;

    OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                         "%s pnp:default:get_triplet app %s version %s release %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == app) ? "NULL" : app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release));
    
    ORCM_CREATE_STRING_ID(&string_id, app, version, release);

    /* lock the global array for our use */
    ORTE_ACQUIRE_THREAD(&orcm_triplets->ctl);

    /* if the string_id contains a wildcard, then we have to look
     * in the wildcard array
     */
    if (NULL != strchr(string_id, '@')) {
        array = &orcm_triplets->wildcards;
    } else {
        array = &orcm_triplets->array;
    }

    for (i=0; i < array->size; i++) {
        if (NULL == (triplet = (orcm_triplet_t*)opal_pointer_array_get_item(array, i))) {
            continue;
        }
        /* require an exact match */
        if (0 == strcasecmp(string_id, triplet->string_id)) {
            /* we have a match */
            OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                 "%s pnp:default:get_triplet_stringid match found",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
            goto process;
        }
    }

    /* if we get here, then this triplet doesn't exist - create it if requested */
    if (create) {
        triplet = OBJ_NEW(orcm_triplet_t);
        triplet->string_id = strdup(string_id);
        /* add it to the appropriate array */
        opal_pointer_array_add(array, triplet);
    }
    
 process:
    free(string_id);

    /* lock this triplet so we have exclusive use of it - the caller is
     * responsible for unlocking it!
     */
    if (NULL != triplet) {
        ORTE_ACQUIRE_THREAD(&triplet->ctl);
    }

    /* release the global array lock */
    ORTE_RELEASE_THREAD(&orcm_triplets->ctl);

    OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                         "%s pnp:default:get_triplet created triplet %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == triplet) ? "NULL" : triplet->string_id));
    
    return triplet;
}

orcm_triplet_group_t* orcm_get_triplet_group(orcm_triplet_t *trp,
                                             orte_jobid_t jobid,
                                             bool create)
{
    int j;
    orcm_triplet_group_t *grp;

    /* we assume that the triplet is already locked */

    /* find the group */
    for (j=0; j < trp->groups.size; j++) {
        if (NULL == (grp = (orcm_triplet_group_t*)opal_pointer_array_get_item(&trp->groups, j))) {
            continue;
        }
        if (grp->jobid != jobid) {
            continue;
        }
        /* found it - just return */
        return grp;
    }

    /* if we didn't find it, do we want it created? */
    if (!create) {
        /* nope */
        return NULL;
    }

    /* create the group */
    grp = OBJ_NEW(orcm_triplet_group_t);
    grp->triplet = trp;
    grp->jobid = jobid;
    opal_pointer_array_add(&trp->groups, grp);

    /* the group is locked as part of the triplet */
    return grp;
}

orcm_source_t* orcm_get_source_in_group(orcm_triplet_group_t *grp,
                                        const orte_vpid_t vpid,
                                        bool create)
{
    orcm_source_t *src;

    if (NULL == (src = (orcm_source_t*)opal_pointer_array_get_item(&grp->members, vpid))) {
        if (!create) {
            /* just return NULL to indicate not found */
            return NULL;
        }
        /* create it */
        src = OBJ_NEW(orcm_source_t);
        src->name.jobid = grp->jobid;
        src->name.vpid = vpid;
        src->alive = false;
        opal_pointer_array_set_item(&grp->members, vpid, src);
        /* if this vpid > num_procs, then reset num_procs as there must be
         * at least that many procs in the job
         */
        if (grp->num_procs < vpid+1) {
            /* recompute the triplet num_procs as well */
            grp->triplet->num_procs -= grp->num_procs;
            grp->num_procs = vpid + 1;
            grp->triplet->num_procs += grp->num_procs;
        }
    }

    /* lock the source - the caller is reponsible
     * for unlocking it!
     */
    ORTE_ACQUIRE_THREAD(&src->ctl);
    return src;
}

orcm_source_t* orcm_get_source(orcm_triplet_t *triplet,
                               const orte_process_name_t *proc,
                               bool create)
{
    int j;
    orcm_source_t *src;
    orcm_triplet_group_t *grp;

    /* we assume that the triplet is already locked */

    /* find the group */
    for (j=0; j < triplet->groups.size; j++) {
        if (NULL == (grp = (orcm_triplet_group_t*)opal_pointer_array_get_item(&triplet->groups, j))) {
            continue;
        }
        if (grp->jobid != proc->jobid) {
            continue;
        }
        /* if this vpid > num_procs, then reset num_procs as there must be
         * at least that many procs in the job
         */
        if (grp->num_procs < proc->vpid+1) {
            /* recompute the triplet num_procs as well */
            triplet->num_procs -= grp->num_procs;
            grp->num_procs = proc->vpid + 1;
            triplet->num_procs += grp->num_procs;
        }

        if (NULL == (src = (orcm_source_t*)opal_pointer_array_get_item(&grp->members, proc->vpid))) {
            if (!create) {
                /* just return NULL to indicate not found */
                return NULL;
            }
            /* create it */
            src = OBJ_NEW(orcm_source_t);
            src->name.jobid = proc->jobid;
            src->name.vpid = proc->vpid;
            src->alive = false;
            opal_pointer_array_set_item(&grp->members, proc->vpid, src);
        }

        /* lock the source - the caller is reponsible
         * for unlocking it!
         */
        ORTE_ACQUIRE_THREAD(&src->ctl);
        return src;
    }

    /* if we get here, then we didn't find the group - create it if directed */
    if (!create) {
        return NULL;
    }

    /* create the group */
    grp = OBJ_NEW(orcm_triplet_group_t);
    grp->jobid = proc->jobid;
    grp->num_procs = proc->vpid+1;
    triplet->num_procs += grp->num_procs;
    opal_pointer_array_add(&triplet->groups, grp);
    /* create the source */
    src = OBJ_NEW(orcm_source_t);
    src->name.jobid = proc->jobid;
    src->name.vpid = proc->vpid;
    src->alive = false;
    opal_pointer_array_set_item(&grp->members, proc->vpid, src);
    /* lock the source - the caller is reponsible
     * for unlocking it!
     */
    ORTE_ACQUIRE_THREAD(&src->ctl);

    return src;
}

bool orcm_triplet_cmp(const char *str1, const char *str2)
{
    char *a1, *v1, *r1;
    char *a2, *v2, *r2;
    
    ORCM_DECOMPOSE_STRING_ID(str1, a1, v1, r1);
    ORCM_DECOMPOSE_STRING_ID(str2, a2, v2, r2);

    if (NULL == a1 || NULL == a2) {
        /* we automatically match on this field */
        goto check_version;
    }
    if (0 != strcasecmp(a1, a2)) {
        return false;
    }
    
 check_version:
    if (NULL == v1 || NULL == v2) {
        /* we automatically match on this field */
        goto check_release;
    }
    if (0 != strcasecmp(v1, v2)) {
        return false;
    }
    
 check_release:
    if (NULL == r1 || NULL == r2) {
        /* we automatically match on this field */
        return true;
    }
    if (0 == strcasecmp(r1, r2)) {
        return true;
    }
    return false;
}

