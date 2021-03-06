/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */
#include "ompi_config.h"
#include <stdio.h>

#include "ompi/mpi/c/bindings.h"
#include "ompi/attribute/attribute.h"

#if OMPI_HAVE_WEAK_SYMBOLS && OMPI_PROFILING_DEFINES
#pragma weak MPI_Comm_create_keyval = PMPI_Comm_create_keyval
#endif

#if OMPI_PROFILING_DEFINES
#include "ompi/mpi/c/profile/defines.h"
#endif

static const char FUNC_NAME[] = "MPI_Comm_create_keyval";


int MPI_Comm_create_keyval(MPI_Comm_copy_attr_function *comm_copy_attr_fn,
                           MPI_Comm_delete_attr_function *comm_delete_attr_fn,
			   int *comm_keyval, void *extra_state)
{
    int ret;
    ompi_attribute_fn_ptr_union_t copy_fn;
    ompi_attribute_fn_ptr_union_t del_fn;

    if (MPI_PARAM_CHECK) {
        OMPI_ERR_INIT_FINALIZE(FUNC_NAME);
	if ((NULL == comm_copy_attr_fn) || (NULL == comm_delete_attr_fn) ||
	    (NULL == comm_keyval)) {
	    return OMPI_ERRHANDLER_INVOKE(MPI_COMM_WORLD, MPI_ERR_ARG, 
                                          FUNC_NAME);
	}
    }
    
    copy_fn.attr_communicator_copy_fn = (MPI_Comm_internal_copy_attr_function*)comm_copy_attr_fn;
    del_fn.attr_communicator_delete_fn = comm_delete_attr_fn;

    ret = ompi_attr_create_keyval(COMM_ATTR, copy_fn, 
                                  del_fn, comm_keyval, extra_state, 0, NULL);

    OMPI_ERRHANDLER_RETURN(ret, MPI_COMM_WORLD, MPI_ERR_OTHER, FUNC_NAME);
}
