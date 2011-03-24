/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

/* Undefine some things so that we don't get conflicts when we include
   OMPI's header files */

#undef PACKAGE_BUGREPORT
#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_VERSION
#undef HAVE_CONFIG_H

#  if ORCM_C_HAVE_VISIBILITY
#    define ORCM_DECLSPEC           __opal_attribute_visibility__("default")
#    define ORCM_MODULE_DECLSPEC    __opal_attribute_visibility__("default")
#  else
#    define ORCM_DECLSPEC
#    define ORCM_MODULE_DECLSPEC
#  endif

