/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config_private.h"
#include "include/constants.h"

#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"

#include "mca/pnp/pnp.h"
#include "mca/pnp/base/public.h"
#include "mca/pnp/base/private.h"

int orcm_pnp_base_close(void)
{
    int i;
    orcm_triplet_t *triplet;
    orcm_pnp_channel_obj_t *chan;
    opal_list_item_t *item;

    if (NULL != orcm_pnp.finalize) {
        orcm_pnp.finalize();
    }
    
    if (NULL != orcm_pnp_base.my_string_id) {
        free(orcm_pnp_base.my_string_id);
    }

    OBJ_DESTRUCT(&orcm_pnp_base.recv_process);
    OBJ_DESTRUCT(&orcm_pnp_base.recv_process_ctl);

    /* release the array of known channels */
    for (i=0; i < orcm_pnp_base.channels.size; i++) {
        if (NULL != (chan = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&orcm_pnp_base.channels, i))) {
            OBJ_RELEASE(chan);
        }
    }
    OBJ_DESTRUCT(&orcm_pnp_base.channels);

    /* finalize the print buffers */
    orcm_pnp_print_buffer_finalize();

    /* Close all remaining available components (may be one if this is a
     Open RTE program, or [possibly] multiple if this is ompi_info) */
    
    mca_base_components_close(orcm_pnp_base.output, 
                              &orcm_pnp_base.opened, NULL);
    
    return ORCM_SUCCESS;
}
