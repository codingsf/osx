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
 * Copyright (c) 2006      Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */
#include "ompi_config.h"
#include <stdio.h>
#include <string.h>

#include "ompi/mpi/c/bindings.h"

#if OMPI_HAVE_WEAK_SYMBOLS && OMPI_PROFILING_DEFINES
#pragma weak MPI_Comm_remote_group = PMPI_Comm_remote_group
#endif

#if OMPI_PROFILING_DEFINES
#include "ompi/mpi/c/profile/defines.h"
#endif


static const char FUNC_NAME[] = "MPI_Comm_remote_group";


int MPI_Comm_remote_group(MPI_Comm comm, MPI_Group *group) 
{

    if ( MPI_PARAM_CHECK ) {
        OMPI_ERR_INIT_FINALIZE(FUNC_NAME);
        if (ompi_comm_invalid (comm)) {
            return OMPI_ERRHANDLER_INVOKE(MPI_COMM_WORLD, MPI_ERR_COMM, 
                                          FUNC_NAME);
        }

        if ( NULL == group ) {
            return OMPI_ERRHANDLER_INVOKE(comm, MPI_ERR_ARG, 
                                          FUNC_NAME);
        }
    }

    if ( OMPI_COMM_IS_INTER(comm) ) {        
        OBJ_RETAIN(comm->c_remote_group);
    }
    else {
        return OMPI_ERRHANDLER_INVOKE (comm, MPI_ERR_COMM, 
                                       FUNC_NAME);
    }

    *group = (MPI_Group) comm->c_remote_group;
    ompi_group_increment_proc_count(*group);
    return MPI_SUCCESS;
}
