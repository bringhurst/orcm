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
#include "orte/runtime/orte_globals.h"

#include "runtime/orcm_globals.h"
#include "util/triplets.h"

orcm_triplet_t* orcm_get_triplet_stringid(const char *stringid)
{
    int i;
    orcm_triplet_t *triplet;

    /* lock the global array for our use */
    OPAL_ACQUIRE_THREAD(&orcm_triplets->lock,
                        &orcm_triplets->cond,
                        &orcm_triplets->in_use);

    for (i=0; i < orcm_triplets->array.size; i++) {
        if (NULL == (triplet = (orcm_triplet_t*)opal_pointer_array_get_item(&orcm_triplets->array, i))) {
            continue;
        }
        if (0 == strcasecmp(stringid, triplet->string_id)) {
            /* we have a match */
            OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                 "%s pnp:default:get_triplet_stringid match found",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
            /* lock the triplet for use - the caller is responsible for
             * unlocking it!
             */
            OPAL_ACQUIRE_THREAD(&triplet->lock, &triplet->cond, &triplet->in_use);
 
            /* release the global array */
            OPAL_RELEASE_THREAD(&orcm_triplets->lock,
                                &orcm_triplets->cond,
                                &orcm_triplets->in_use);
            return triplet;
        }
    }

    /* release the global array */
    OPAL_RELEASE_THREAD(&orcm_triplets->lock,
                        &orcm_triplets->cond,
                        &orcm_triplets->in_use);
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
    
    OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                         "%s pnp:default:get_triplet app %s version %s release %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == app) ? "NULL" : app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release));
    
    ORCM_CREATE_STRING_ID(&string_id, app, version, release);

    /* lock the global array for our use */
    OPAL_ACQUIRE_THREAD(&orcm_triplets->lock,
                        &orcm_triplets->cond,
                        &orcm_triplets->in_use);

    for (i=0; i < orcm_triplets->array.size; i++) {
        if (NULL == (triplet = (orcm_triplet_t*)opal_pointer_array_get_item(&orcm_triplets->array, i))) {
            continue;
        }
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
        /* add it to the array */
        opal_pointer_array_add(&orcm_triplets->array, triplet);
    }
    
 process:
    free(string_id);

    /* lock this triplet so we have exclusive use of it - the caller is
     * responsible for unlocking it!
     */
    if (NULL != triplet) {
        OPAL_ACQUIRE_THREAD(&triplet->lock, &triplet->cond, &triplet->in_use);
    }

    /* release the global array lock */
    OPAL_RELEASE_THREAD(&orcm_triplets->lock,
                        &orcm_triplets->cond,
                        &orcm_triplets->in_use);

    OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                         "%s pnp:default:get_triplet created triplet %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == triplet) ? "NULL" : triplet->string_id));
    
    return triplet;
}

orcm_source_t* orcm_get_source(orcm_triplet_t *triplet, orte_vpid_t vpid)
{
    orcm_source_t *src;

    /* we assume that the triplet is already locked */

    /* if this vpid > num_procs, then reset num_procs as there must be
     * at least that many procs in the triplet job
     */
    if (triplet->num_procs < vpid+1) {
        triplet->num_procs = vpid + 1;
    }

    if (NULL == (src = (orcm_source_t*)opal_pointer_array_get_item(&triplet->members, vpid))) {
        return NULL;
    }

    /* lock the source - the caller is reponsible
     * for unlocking it!
     */
    OPAL_ACQUIRE_THREAD(&src->lock, &src->cond, &src->in_use);
    return src;
}

bool orcm_triplet_cmp(char *str1, char *str2)
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

