/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

/**
 * @file
 *
 */
#ifndef ORCM_TRIPLETS_H
#define ORCM_TRIPLETS_H

#include "openrcm.h"

#include "runtime/orcm_globals.h"

BEGIN_C_DECLS

/* Lookup a triplet object on the global array of known triplets
 * using an orte_jobid_t. Returns NULL if the triplet isn't found.
 *
 * NOTE: returned non-NULL triplet will have its thread-lock active. The
 *       caller is responsible for releasing the thread when done!
 */
ORCM_DECLSPEC orcm_triplet_t* orcm_get_triplet_jobid(const orte_jobid_t jobid);

/* Lookup a triplet object on the global array of known triplets
 * using the triplet stringid. Returns NULL if the triplet isn't found.
 *
 * NOTE: returned non-NULL triplet will have its thread-lock active. The
 *       caller is responsible for releasing the thread when done!
 */
ORCM_DECLSPEC orcm_triplet_t* orcm_get_triplet_stringid(const char *stringid);

/* Lookup a triplet object on the global array of known triplets
 * using the triplet. If the triplet isn't found, the function:
 *
 * (a) returns NULL if create=false
 *
 * (b) creates a new triplet object, places it on the global
 *     array, and returns that object if create=true
 *
 * NOTE: returned non-NULL triplet will have its thread-lock active. The
 *       caller is responsible for releasing the thread when done!
 */
ORCM_DECLSPEC orcm_triplet_t* orcm_get_triplet(const char *app,
                                               const char *version,
                                               const char *release,
                                               bool create);

/* Lookup a triplet group based on jobid. Returns NULL if the group
 * isn't found and create is false
 *
 * NOTE: the caller is responsible for ensuring that the triplet object
 *       has been thread-locked prior to calling this function!
 */
ORCM_DECLSPEC orcm_triplet_group_t* orcm_get_triplet_group(orcm_triplet_t *trp,
                                                           orte_jobid_t jobid,
                                                           bool create);

/* Lookup a source object for a member of the specified triplet group.
 * Returns NULL if the source isn't found and create is false.
 *
 * NOTE: the caller is responsible for ensuring that the triplet object
 *       has been thread-locked prior to calling this function!
 *
 * NOTE: returned non-NULL source will have its thread-lock active. The
 *       caller is responsible for releasing the thread when done!
 */
ORCM_DECLSPEC orcm_source_t* orcm_get_source_in_group(orcm_triplet_group_t *grp,
                                                      const orte_vpid_t vpid,
                                                      bool create);

/* Lookup a source object for a member of the specified triplet. Returns
 * NULL if the source isn't found and create is false
 *
 * NOTE: the caller is responsible for ensuring that the triplet object
 *       has been thread-locked prior to calling this function!
 *
 * NOTE: returned non-NULL source will have its thread-lock active. The
 *       caller is responsible for releasing the thread when done!
 */
ORCM_DECLSPEC orcm_source_t* orcm_get_source(orcm_triplet_t *triplet,
                                             const orte_process_name_t *proc,
                                             bool create);

/* Compare two stringid's, properly accounting for any wildcard
 * fields. Return true if they match and false if they don't
 */
ORCM_DECLSPEC bool orcm_triplet_cmp(const char *stringid1, const char *stringid2);

END_C_DECLS

#endif /* RUNTIME_H */
