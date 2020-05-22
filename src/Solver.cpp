/**
 * @file Solver.cpp
 * @author Thomas Gillis and Denis-Gabriel Caprace
 * @copyright Copyright © UCLouvain 2020
 * 
 * FLUPS is a Fourier-based Library of Unbounded Poisson Solvers.
 * 
 * Copyright <2020> <Université catholique de Louvain (UCLouvain), Belgique>
 * 
 * List of the contributors to the development of FLUPS, Description and complete License: see LICENSE and NOTICE files.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *  http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 */

#include "Solver.hpp"

/**
 * @brief Constructs a fftw Poisson solver, initilizes the plans and determines their order of execution
 * 
 * @param topo the input topology of the data (in physical space)
 * @param rhsbc the boundary conditions of the computational domain (for the right hand side!!), the first index corresponds to the dimension, the second is left (0) or right (1) side and the last one to the component
 * @param h the grid spacing
 * @param L the domain size
 * @param orderDiff the differential order used for the rotational case. If no need of rotational, set 0. (order 1 = spectral, order 2 = finite diff 2nd order)
 * @param prof the profiler to use for the solve timing
 */
Solver::Solver(Topology *topo, BoundaryType* rhsbc[3][2], const double h[3], const double L[3], const int orderDiff, Profiler *prof){
    BEGIN_FUNC;

    // //-------------------------------------------------------------------------
    // /** - Initialize the OpenMP threads for FFTW */
    // //-------------------------------------------------------------------------
    fftw_init_threads();

    //-------------------------------------------------------------------------
    /** - Check the alignement in memory between FFTW and the one defines in @ref flups.h */
    //-------------------------------------------------------------------------
    // align a random array
    int     alignSize = FLUPS_ALIGNMENT / sizeof(double);
    double *data      = (double *)flups_malloc(10 * alignSize * sizeof(double));
    // initialize the fftw alignement
    _fftwalignment = (fftw_alignment_of(&(data[0])) == 0) ? sizeof(double) : 0;
    // get the fftw alignement and stop if it is lower than the one we assumed
    for (int i = 1; i < 10 * alignSize; i++) {
        if (fftw_alignment_of(&(data[i])) == 0) {
            // if we are above the minimum requirement, generate an error
            if (i > alignSize) {
                FLUPS_ERROR("The FLUPS alignement has to be a multiple integer of the FFTW alignement, please change the constant variable FLUPS_ALIGNEMENT into file flups.h accordingly: FFTW=%d vs FLUPS=%d", _fftwalignment, FLUPS_ALIGNMENT, LOCATION);
            }
            // else, just stop and advise the user to change
            break;
        }
        _fftwalignment += sizeof(double);
    }
    if (_fftwalignment != FLUPS_ALIGNMENT) {
        FLUPS_WARNING("FFTW alignement is OK, yet not optimal: FFTW = %d vs FLUPS = %d", _fftwalignment, FLUPS_ALIGNMENT, LOCATION);
        FLUPS_WARNING("Consider using an alignment of 32 for the AVX (128bit registers) and 64 for the AVX2 (256bit registers)", LOCATION);
    } else {
        FLUPS_INFO("FFTW alignement is OK: FFTW = %d vs FLUPS = %d", _fftwalignment, FLUPS_ALIGNMENT);
        FLUPS_INFO("Consider using an alignment of 32 for the AVX (128bit registers) and 64 for the AVX2 (256bit registers)");
    }

    flups_free(data);

    //-------------------------------------------------------------------------
    /** - Create the timer */
    //-------------------------------------------------------------------------
    if (prof != NULL) {
        _prof = prof;
    } else {
        _prof = NULL;
    }
    if (_prof != NULL) _prof->create("init", "root");
    if (_prof != NULL) _prof->create("setup", "root");
    if (_prof != NULL) _prof->create("alloc_data", "setup");
    if (_prof != NULL) _prof->create("alloc_plans", "setup");
    if (_prof != NULL) _prof->create("green", "setup");
    if (_prof != NULL) _prof->create("green_plan", "green");
    if (_prof != NULL) _prof->create("green_func", "green");
    if (_prof != NULL) _prof->create("green_final", "green");
    if (_prof != NULL) _prof->create("solve", "root");
    if (_prof != NULL) _prof->create("copy", "solve");
    if (_prof != NULL) _prof->create("fftw", "solve");
    if (_prof != NULL) _prof->create("domagic", "solve");
    if (_prof != NULL) _prof->start("init");

    

    //-------------------------------------------------------------------------
    /** - store the meshsize, the lda and the orderDiff */
    //-------------------------------------------------------------------------
    for (int id = 0; id < 3; id++) {
        _hgrid[id] = h[id];
    }
    _lda       = topo->lda();
    _odiff = orderDiff;

    //-------------------------------------------------------------------------
    /** - initialize the diff bc */
    //-------------------------------------------------------------------------
    FLUPS_BoundaryType* diffbc[3][2];
    if(_odiff){
        for (int id = 0; id < 3; id++) {
            for(int is=0; is<2; is++){
                diffbc[id][is] =(FLUPS_BoundaryType*) flups_malloc(sizeof(int)*_lda);
                for(int lia=0; lia<_lda; lia++){
                    if(rhsbc[id][is][lia] == EVEN){
                        diffbc[id][is][lia] = ODD;
                    } else if (rhsbc[id][is][lia] == ODD){
                        diffbc[id][is][lia] = EVEN;
                    } else {
                        diffbc[id][is][lia] = rhsbc[id][is][lia];
                    }
                }
            }
        }
    }

    //-------------------------------------------------------------------------
    /** - For each dim, create the plans given the BC and sort them by type */
    //-------------------------------------------------------------------------

    // we allocate 3 plans
    // it might be empty ones but we keep them since we need some information inside...
    for (int id = 0; id < 3; id++) {
        _plan_forward[id]  = new FFTW_plan_dim(_lda, id, h, L, rhsbc[id], FLUPS_FORWARD, false);
        _plan_backward[id] = new FFTW_plan_dim(_lda, id, h, L, rhsbc[id], FLUPS_BACKWARD, false);
        _plan_green[id]    = new FFTW_plan_dim(1, id, h, L, rhsbc[id], FLUPS_FORWARD, true);
    }

    _sort_plans(_plan_forward);
    _sort_plans(_plan_backward);
    _sort_plans(_plan_green);
    FLUPS_INFO("I will proceed with forward transforms in the following direction order: %d, %d, %d", _plan_forward[0]->dimID(), _plan_forward[1]->dimID(), _plan_forward[2]->dimID());

    // create the backward plan in the EXACT same order as the backward one
    if(_odiff){
        for (int id = 0; id < 3; id++) {
            // get the corresponding direction
            const int dimID = _plan_backward[id]->dimID();
            // initialize the plan with the BC of the considered direction
            _plan_backward_diff[id] = new FFTW_plan_dim(_lda, dimID, h, L, diffbc[dimID], FLUPS_BACKWARD, false);
        }
    }

    //-------------------------------------------------------------------------
    /** - compute the real problem size using forward plans, i.e. are we 2D or 3D? */
    //-------------------------------------------------------------------------
    _ndim = 3;
    for(int id=0; id<3; id++){
        if(_plan_forward[id]->type() == FFTW_plan_dim::EMPTY){
            _ndim --;
        }
    }

    //-------------------------------------------------------------------------
    /** - Initialise the topos, the plans and the SwitchTopos */
    //-------------------------------------------------------------------------
    _topo_phys = topo; //store pointer to the topo of the user
    _init_plansAndTopos(topo, _topo_hat, _switchtopo, _plan_forward, false);
    _init_plansAndTopos(topo, NULL, NULL, _plan_backward, false);
    _init_plansAndTopos(topo, _topo_green, _switchtopo_green, _plan_green, true);
    if(_odiff){
        _init_plansAndTopos(topo, NULL, NULL, _plan_backward_diff, false);
    }

    //-------------------------------------------------------------------------
    /** - Get the factors #_normfact, #_volfact, #_shiftgreen */
    //-------------------------------------------------------------------------
    // init the volumes + norm fact
    _normfact  = 1.0;
    _volfact   = 1.0;

    // compute the coefficients and the phase correction
    for (int ip = 0; ip < 3; ip++) {
        _normfact *= _plan_forward[ip]->normfact();
        _volfact *= _plan_forward[ip]->volfact();
    }

    //-------------------------------------------------------------------------
    /** - free temp arrays and stop the timer */
    //-------------------------------------------------------------------------

    // free some stuffs
    if (_odiff) {
        for (int id = 0; id < 3; id++) {
            for (int is = 0; is < 2; is++) {
                flups_free(diffbc[id][is]);
            }
        }
    }

    if (_prof != NULL) _prof->stop("init");
    END_FUNC;
}

/**
 * @brief Sets up the Solver
 * 
 * @param changeTopoComm determine if the user allows the solver to rewrite the communicator associated with the provided topo at the init (if compiled with REODRED_RANKS)
 * 
 * @warning
 * To be able to change the communicator, the user MUST ensure that NOTHING has been done with the topology yet, except creating it.
 * Otherwise he will get unpredictable datas
 * 
 * @warning
 * After this function the parameter of the solver (size etc) cannot be changed anymore
 * 
 * -------------------------------------------
 * We do the following operations
 */
double* Solver::setup(const bool changeTopoComm) {
    BEGIN_FUNC;
    if (_prof != NULL) _prof->start("setup");

    //-------------------------------------------------------------------------
    /** [IF REORDER_RANKS IS DEFINED] */
    //-------------------------------------------------------------------------
    
#ifdef REORDER_RANKS
    //-------------------------------------------------------------------------
    /** - Precompute the communication graph */
    //-------------------------------------------------------------------------
    // get the communication size
    int worldsize, rank;
    MPI_Comm_size(_topo_phys->get_comm(), &worldsize);
    MPI_Comm_rank(_topo_phys->get_comm(), &rank);
    
    // initialize the sources, sources weights, destination and destination weights
    int* sources  = (int*)flups_malloc(worldsize * sizeof(int));
    int* sourcesW = (int*)flups_malloc(worldsize * sizeof(int));
    int* dests    = (int*)flups_malloc(worldsize * sizeof(int));
    int* destsW   = (int*)flups_malloc(worldsize * sizeof(int));

    //Preparing the graph:
    // we setup the thing as if every node was to communicate with every other node
    // the default communication weight is null
    memset(sourcesW,0,sizeof(int)*worldsize);
    memset(destsW,0,sizeof(int)*worldsize);
    for(int i =0;i<worldsize;i++){
        sources[i] = i;
        dests[i] = i;
    }

    //Count the total number of edges for the switchtopos
    // if we are not allowed to change the physical topology,
    // do it only for the 2nd and 3rd switchtopo.
    // These are the switches that we hope to optimize with the rank
    // reordering. We do not account the 1st switchtopo because that
    // one will be used to reach the optimized layout associated with
    // the graph_comm, and it is thus very likely that the communication
    // involved in the first switchtopo is a real all 2 all (with some
    // ranks not having a self block) !
    // if we can change the topology, do it for every swithTopo
    if (changeTopoComm) {
        for (int i = 0; i < _ndim; i++) {
            _switchtopo[i]->add_toGraph(sourcesW, destsW);
        }
    } else {
        for (int i = 1; i < _ndim; i++) {
            _switchtopo[i]->add_toGraph(sourcesW, destsW);
        }
    }

    //-------------------------------------------------------------------------
    /** - Build the new comm based on that graph using metis if available, graph_topo if not */
    //-------------------------------------------------------------------------
    MPI_Comm graph_comm;
#ifndef HAVE_METIS
    MPI_Dist_graph_create_adjacent(_topo_phys->get_comm(), worldsize, sources, sourcesW,
                                   worldsize, dests, destsW,
                                   MPI_INFO_NULL, 1, &graph_comm);

#if defined(VERBOSE) && VERBOSE == 2
    int inD, outD, wei;
    MPI_Dist_graph_neighbors_count(graph_comm, &inD, &outD, &wei);
    printf("[FGRAPH] inD:%d outD:%d wei:%d\n", inD, outD, wei);

    int *Sour  = (int *)malloc(sizeof(int) * inD);
    int *SourW = (int *)malloc(sizeof(int) * inD);
    int *Dest  = (int *)malloc(sizeof(int) * outD);
    int *DestW = (int *)malloc(sizeof(int) * outD);

    MPI_Dist_graph_neighbors(graph_comm, inD, Sour, SourW,
                             outD, Dest, DestW);

    printf("[FGRAPH] INedges: ");
    for (int i = 0; i < inD; i++) {
        printf("%d (%d), ", Sour[i], SourW[i]);
    }
    printf("\n[FGRAPH] OUTedges: ");
    for (int i = 0; i < outD; i++) {
        printf("%d (%d), ", Dest[i], DestW[i]);
    }
    printf("\n");

    free(Sour);
    free(SourW);
    free(Dest);
    free(DestW);
#endif

    //-------------------------------------------------------------------------
    /** - if asked by the user, we overwrite the graph comm by a forced version (for test purpose) */
    //-------------------------------------------------------------------------
#ifdef DEV_SIMULATE_GRAPHCOMM
    //switch indices by a random number:
    #ifdef DEV_REORDER_SHIFT
        int shift = DEV_REORDER_SHIFT;
    #else
        int shift = worldsize/2;
    #endif

    int* outRanks = (int*) flups_malloc(sizeof(int)*worldsize);
    if(rank == 0){
        FLUPS_INFO("SIMULATED GRAPH_COMM with shift = %d : REORDERING RANKS AS FOLLOWS",shift);
    }
    for (int i=0;i<worldsize;i++){
        outRanks[i] = (i + shift)%worldsize;
        if(rank == 0){
            FLUPS_INFO("old rank: %d \t new rank: %d",i,outRanks[i]);
        }
    }
    
    MPI_Group group_in, group_out;
    MPI_Comm_group(_topo_phys->get_comm(), &group_in);                //get the group of the current comm
    MPI_Group_incl(group_in, worldsize, outRanks, &group_out);        //manually reorder the ranks
    MPI_Comm_create(_topo_phys->get_comm(), group_out, &graph_comm);  // create the new comm

    flups_free(outRanks);
#endif
//end simulate_graph

    #ifdef PROF
    //writing reordering to console
    int newrank;
    MPI_Comm_rank(graph_comm, &newrank);
    printf("[MPI ORDER] %i : %i \n", rank, newrank);
    #endif

#else
    //Use METIS to find a smart partition of the graph
    int *order = (int *)flups_malloc(sizeof(int) * worldsize);
    _reorder_metis(_topo_phys->get_comm(), sources, sourcesW, dests, destsW, order);
    // create a new comm based on the order given by metis
    MPI_Group group_in, group_out;
    MPI_Comm_group(_topo_phys->get_comm(), &group_in);                //get the group of the current comm
    MPI_Group_incl(group_in, worldsize, order, &group_out);           //manually reorder the ranks
    MPI_Comm_create(_topo_phys->get_comm(), group_out, &graph_comm);  // create the new comm
    flups_free(order);
#endif // METIS

    flups_free(sources);
    flups_free(sourcesW);
    flups_free(dests);
    flups_free(destsW);

    std::string commname = "graph_comm";
    MPI_Comm_set_name(graph_comm, commname.c_str());

    // Advise the topologies that they will be associated with an other comm
    // if we cannot change topo phys, the _topo_phys remains without graph_comm.
    // The first switch topo will serve to redistribute
    // data following the optimized topology on the cluster, with reordered 
    // ranks
    for(int i=0;i<_ndim;i++){
        _topo_hat[i]->change_comm(graph_comm);
        _topo_green[i]->change_comm(graph_comm);
    }
    if(changeTopoComm){
        _topo_phys->change_comm(graph_comm);
    }

    #ifdef PERF_VERBOSE
    _topo_hat[0]->disp_rank();
    #endif

#endif //REORDER_RANKS

    //-------------------------------------------------------------------------
    /** In every cases, we do */
    //-------------------------------------------------------------------------

    //-------------------------------------------------------------------------
    /** - allocate the data for the Green's function */
    //-------------------------------------------------------------------------
    if (_prof != NULL) _prof->start("alloc_data");
    _allocate_data(_topo_green, NULL, &_green);
    if (_prof != NULL) _prof->stop("alloc_data");

    //-------------------------------------------------------------------------
    /** - allocate the plan and comnpute the Green's function */
    //-------------------------------------------------------------------------
    if (_prof != NULL) _prof->start("green");
    if (_prof != NULL) _prof->start("green_plan");
    _allocate_plans(_topo_green, _plan_green, _green);
    if (_prof != NULL) _prof->stop("green_plan");
    // setup the buffers for Green
    _allocate_switchTopo(3, _switchtopo_green, &_sendBuf, &_recvBuf);
    if (_prof != NULL) _prof->start("green_func");
    _cmptGreenFunction(_topo_green, _green, _plan_green);
    if (_prof != NULL) _prof->stop("green_func");
    // finalize green by replacing some data in full spectral if needed by the kernel,
    // and by doing a last switch to the field topo
    if (_prof != NULL) _prof->start("green_final");
    _finalizeGreenFunction(_topo_hat[_ndim-1], _green, _topo_green[_ndim-1], _plan_green);
    if (_prof != NULL) _prof->stop("green_final");

    //-------------------------------------------------------------------------
    /** - Clean the Green's function accessories (allocated topo and plans) */
    //-------------------------------------------------------------------------
    // delete the switchTopos
    _deallocate_switchTopo(_switchtopo_green, &_sendBuf, &_recvBuf);
    _delete_switchtopos(_switchtopo_green);
    // delete the topologies and plans not needed anymore
    _delete_topologies(_topo_green);
    _delete_plans(_plan_green);
    if (_prof != NULL) _prof->stop("green");

    //-------------------------------------------------------------------------
    /** - allocate the data for the field */
    //-------------------------------------------------------------------------
    if (_prof != NULL) _prof->start("alloc_data");
    _allocate_data(_topo_hat, _topo_phys, &_data);
    if (_prof != NULL) _prof->stop("alloc_data");

    //-------------------------------------------------------------------------
    /** - allocate the plans forward, forward_diff and backward for the field */
    //-------------------------------------------------------------------------
    if (_prof != NULL) _prof->start("alloc_plans");
    _allocate_plans(_topo_hat, _plan_forward, _data);
    _allocate_plans(_topo_hat, _plan_backward, _data);
    if (_odiff) {
        _allocate_plans(_topo_hat, _plan_backward_diff, _data);
    }
    if (_prof != NULL) _prof->stop("alloc_plans");

    //-------------------------------------------------------------------------
    /** - Setup the SwitchTopo, this will take the latest comm into account */
    //-------------------------------------------------------------------------
    _allocate_switchTopo(_ndim, _switchtopo, &_sendBuf, &_recvBuf);

    if (_prof != NULL) _prof->stop("setup");

    FLUPS_INFO(">>>>>>>>>> DONE WITH SOLVER INITIALIZATION <<<<<<<<<<");

    END_FUNC;
    return _data;
}

/**
 * @brief Destroy the fftw solver
 * 
 */
Solver::~Solver() {
    BEGIN_FUNC;
    // for Green
    if (_green != NULL) flups_free(_green);
    // delete the plans
    _delete_plans(_plan_forward);
    _delete_plans(_plan_backward);
    if(_odiff){
        _delete_plans(_plan_backward_diff);
    }
    
    // free the sendBuf,recvBuf
    _deallocate_switchTopo(_switchtopo, &_sendBuf, &_recvBuf);
    // deallocate the swithTopo
    _delete_switchtopos(_switchtopo);

    // cleanup the communicator if any
#ifdef REORDER_RANKS
    MPI_Comm mycomm = _topo_hat[_ndim-1]->get_comm();
    MPI_Comm_free(&mycomm);
#endif
    _delete_topologies(_topo_hat);
    
    if (_data != NULL) flups_free(_data);

    //cleanup
    fftw_cleanup_threads();
    fftw_cleanup();

    END_FUNC;
}

/**
 * @brief returns a copy of the topology corresponding to the physical space
 * 
 */
const Topology* Solver::get_innerTopo_physical() {
    return _topo_hat[0];
}

/**
 * @brief returns a copy of the topology corresponding to the fully transformed space
 * 
 */
const Topology* Solver::get_innerTopo_spectral() {
    return _topo_hat[_ndim-1];
}

/**
 * @brief delete the FFTW_plan_dim stored in planmap
 * 
 * @param planmap 
 */
void Solver::_delete_plans(FFTW_plan_dim *planmap[3]) {
    BEGIN_FUNC;
    // deallocate the plans
    for (int ip = 0; ip < 3; ip++) {
        delete planmap[ip];
        planmap[ip] = NULL;
    }
    END_FUNC;
}

/**
 * @brief delete the switchtopo objects
 * 
 * @param switchtopo 
 */
void Solver::_delete_switchtopos(SwitchTopo *switchtopo[3]) {
    BEGIN_FUNC;
    // deallocate the plans
    for (int ip = 0; ip < 3; ip++) {
        delete switchtopo[ip];
        switchtopo[ip] = NULL;
    }
    END_FUNC;
}

/**
 * @brief delete the topologies
 * 
 * @param topo 
 */
void Solver::_delete_topologies(Topology *topo[3]) {
    BEGIN_FUNC;
    // deallocate the plans
    for (int ip = 0; ip < 3; ip++) {
        delete topo[ip];
        topo[ip] = NULL;
    }
    END_FUNC;
}

/**
 * @brief smartly determines in which order the FFTs will be executed
 * 
 * @param plan the list of plan, which will be reordered
 */
void Solver::_sort_plans(FFTW_plan_dim *plan[3]) {
    BEGIN_FUNC;
    int id_min, val_min = INT_MAX;
    int priority[3];
    for (int id = 0; id < 3; id++) {
        priority[id] = plan[id]->type();
        if (priority[id] < val_min) {
            id_min  = id;
            val_min = priority[id];
        }
    }
    if (id_min == 0) {
        if (priority[1] > priority[2]) {
            FFTW_plan_dim *temp_plan = plan[2];
            plan[2]                  = plan[1];
            plan[1]                  = temp_plan;
        }
    } else {
        // do the sort by hand...
        int            temp_priority = priority[id_min];
        FFTW_plan_dim *temp_plan     = plan[id_min];
        plan[id_min]                 = plan[0];
        plan[0]                      = temp_plan;
        priority[id_min]             = priority[0];
        priority[0]                  = temp_priority;

        // printf("priority now = %d %d %d -> idim = %d",plan[0]->type(), plan[1]->type(),plan[2]->type());

        if (priority[1] > priority[2]) {
            FFTW_plan_dim *temp_plan = plan[2];
            plan[2]                  = plan[1];
            plan[1]                  = temp_plan;
        }
    }

    FLUPS_CHECK((plan[0]->type() <= plan[1]->type()) && (plan[1]->type() <= plan[2]->type()), "Wrong order in the plans: %d %d %d", plan[0]->type(), plan[1]->type(), plan[2]->type(), LOCATION);
    END_FUNC;
}

/**
 * @brief Initializes a set of 3 plans by doing a dry run through the plans
 * 
 * @param topo the starting topology
 * @param topomap the topology array to go through each dim ( may be NULL) it corresponds to the topology AFTER the plan
 * @param switchtopo the switchtopo array to switch between topologies (may be NULL, if so it is not computed)
 * @param planmap the plan that will be created
 * @param isGreen indicates if the plans are for Green
 */
void Solver::_init_plansAndTopos(const Topology *topo, Topology *topomap[3], SwitchTopo *switchtopo[3], FFTW_plan_dim *planmap[3], bool isGreen) {
    BEGIN_FUNC;

    // @Todo: check that _plan_forward exists before doing _plan_green !

    int comm_size;
    MPI_Comm_size(topo->get_comm(), &comm_size);

    //-------------------------------------------------------------------------
    /** - Store the current topology */
    //-------------------------------------------------------------------------
    const Topology *current_topo = topo;

    //-------------------------------------------------------------------------
    /** - The size is initilized to that of the physical space. Then, with the 
     * dry run, it will grow/shrink in every dimension, and this will be used
     * as the size for the intermediate topos.
     * Eventually, the finial size of the data will be that of the largest 
     * topo. */
    //-------------------------------------------------------------------------
    int size_tmp[3];
    for (int id = 0; id < 3; id++) {
        size_tmp[id] = topo->nglob(id);
    }
    //-------------------------------------------------------------------------
    /** - get the dimension order of the plan  */
    //-------------------------------------------------------------------------
    int dimOrder[3] = {planmap[0]->dimID(), planmap[1]->dimID(), planmap[2]->dimID()};

    //-------------------------------------------------------------------------
    /** - creates the plans and the intermediate topologies (if not Green).
     *    This performs a dry run in order to determine the final amount of 
     *    memmory required. It also prepares switchtopo which allows to switch
     *    between two successive topologies.   */
    //-------------------------------------------------------------------------
    bool isComplex = false;  //this refers to the "current state" of the data during dry run
    int  nproc[3];
    for (int ip = 0; ip < _ndim; ip++) {
        // initialize the plan (for Green only, using info from _plan_forward)
        planmap[ip]->init(size_tmp, isComplex);
        // update the size_tmp variable and get the complex information
        planmap[ip]->get_outsize(size_tmp);
        // virtually execute the plan and determine the output
        planmap[ip]->get_isNowComplex(&isComplex);
        // determines the fastest rotating index
        int dimID = planmap[ip]->dimID();

        // we store a new topology BEFORE the plan is executed
        if (!isGreen && topomap != NULL && switchtopo != NULL) {
            // determines the proc repartition using the previous one if available
            if (ip == 0) {
                // for the first switchTopo, we keep the number of proc constant in the 3rd direction
                const int nproc_hint[3] = {topo->nproc(0), topo->nproc(1), topo->nproc(2)};
                pencil_nproc_hint(dimID, nproc, comm_size, dimOrder[1], nproc_hint);
            } else {
                const int nproc_hint[3] = {current_topo->nproc(0), current_topo->nproc(1), current_topo->nproc(2)};
                // for the other switchtopos, we keep constant the id that is not mine, neither the old topo id
                pencil_nproc_hint(dimID, nproc, comm_size, planmap[ip - 1]->dimID(), nproc_hint);
            }
            // create the new topology corresponding to planmap[ip] in the output layout (size and isComplex)
            topomap[ip] = new Topology(dimID, _lda, size_tmp, nproc, isComplex, dimOrder, _fftwalignment, _topo_phys->get_comm());
            // determines fieldstart = the point where the old topo has to begin in the new one
            // There are cases (typically for MIXUNB) where the data after being switched starts with an offset in memory in the new topo.
            int fieldstart[3] = {0};
            planmap[ip]->get_fieldstart(fieldstart);
            // compute the Switch between the current topo (the one from which we come) and the new one (the one we just created).
            // if the topo was real before the plan and is now complex
            if (planmap[ip]->isr2c()) {
                topomap[ip]->switch2real();
#if defined(COMM_NONBLOCK)
                switchtopo[ip] = new SwitchTopo_nb(current_topo, topomap[ip], fieldstart, _prof);
#else
                switchtopo[ip] = new SwitchTopo_a2a(current_topo, topomap[ip], fieldstart, _prof);
#endif
                topomap[ip]->switch2complex();

            } else {
                // create the switchtopoMPI to change topology
#if defined(COMM_NONBLOCK)
                switchtopo[ip] = new SwitchTopo_nb(current_topo, topomap[ip], fieldstart, _prof);
#else
                switchtopo[ip] = new SwitchTopo_a2a(current_topo, topomap[ip], fieldstart, _prof);
#endif
            }
            // #ifdef PERF_VERBOSE
            // switchtopo[ip]->disp_rankgraph(ip - 1, ip);
            // #endif
            // update the current topo to the new one
            current_topo = topomap[ip];

            current_topo->disp();
            switchtopo[ip]->disp();
        }
        planmap[ip]->disp();
    }

    // -- at this point, size_tmp is the size that I need for the Green function in
    //    the last topo, and isComplex describes if the Green function in that topo is
    //    expressed in Complex or not.

    //-------------------------------------------------------------------------
    /** - For Green we need to compute the topologies using the full size of the domain.
     *    We proceed backward (from the last to the first topo), and we adapt the size
     *    in case of r2c, in order to obtain the correct size of Green in topo[0], which
     *    is the topo in which we fill the Green function.      */
    //-------------------------------------------------------------------------
    current_topo = NULL;
    // isComplex = false; //Change this for Helmolz: we will always need to fill Green in complex
    if (isGreen && topomap != NULL && switchtopo != NULL) {
        for (int ip = _ndim-1; ip >= 0; ip--) {
            // get the fastest rotating index
            int dimID = planmap[ip]->dimID();  // store the correspondance of the transposition

            // get the proc repartition
            if(ip>_ndim-2){
                //it has to be the same as the field in full spectral
                for(int i = 0;i<3;i++){
                    nproc[i]=_topo_hat[_ndim-1]->nproc(i);
                }
            }else{
                const int nproc_hint[3] = {current_topo->nproc(0), current_topo->nproc(1), current_topo->nproc(2)};
                pencil_nproc_hint(dimID, nproc, comm_size, planmap[ip+1]->dimID(), nproc_hint);
            }

            // create the new topology in the output layout (size and isComplex). lda of Green is always 1.
            topomap[ip] = new Topology(dimID, 1, size_tmp, nproc, isComplex, dimOrder, _fftwalignment, _topo_phys->get_comm());
            //switchmap only to be done for topo0->topo1 and topo1->topo2
            if (ip < _ndim-1) {
                // get the fieldstart = the point where the old topo has to begin in the new
                int fieldstart[3] = {0};
                // it shouldn't be different from 0 for this case since we are doing green, but safety first
                planmap[ip + 1]->get_fieldstart(fieldstart);
                // we do the link between topomap[ip] and the current_topo
#if defined(COMM_NONBLOCK)
                switchtopo[ip + 1] = new SwitchTopo_nb(topomap[ip], current_topo, fieldstart, NULL);
#else
                switchtopo[ip + 1] = new SwitchTopo_a2a(topomap[ip], current_topo, fieldstart, NULL);
#endif
                switchtopo[ip + 1]->disp();
            }

            // Go to real data if the FFT is really done on green's array.
            // if not, keep it in complex
            if (planmap[ip]->isr2c_doneByFFT()) {
                topomap[ip]->switch2real();
                size_tmp[dimID] *= 2;
                isComplex = false;
            }
            // update the "current topo", which we need to define the switchtopo
            current_topo = topomap[ip];

            current_topo->disp();
        }
    }

    // Implementation Note:
    // If you want to do Helmoltz, you will always have to fill a complex Green function:
    // - we need to ignore all r2cs (bypass the condition on isr2c_doneByFFT)
    // - as there will be only C2C transforms, the size obtained after the init of plans
    //   is already the correct size for Green.
    // -> we need to be able to do SYMSYM directions on a complex number... meaning that we
    //    will need to adapt the plan so that when it needs to do a "real2real" transform on
    //    a complex input, it actually does it separately on the real and imaginary part.
    // - if there are SYMSYM only, the last topo of fiels remains Real while I will have a
    //   complex green function. Need to handle that in solve() ?

    //-------------------------------------------------------------------------
    /** - reset the topologies to real if needed, in order to prepare them for their execution  */
    //-------------------------------------------------------------------------
    for (int ip = 0; ip < _ndim; ip++) {
        if (!isGreen && planmap[ip]->isr2c() && topomap != NULL) {
            topomap[ip]->switch2real();
        }
    }
    END_FUNC;
}

/**
 * @brief 
 * 
 * @param ntopo 
 * @param switchtopo 
 * @param send_buff 
 * @param recv_buff 
 */
void Solver::_allocate_switchTopo(const int ntopo, SwitchTopo **switchtopo, opt_double_ptr *send_buff, opt_double_ptr *recv_buff) {
    BEGIN_FUNC;
    size_t max_mem = 0;

    // setup the communication. During this step, the size of the buffers required by each switchtopo might change.
    for (int id = 0; id < ntopo; id++) {
        if (switchtopo[id] != NULL){
            switchtopo[id]->setup();
            FLUPS_INFO("--------------- switchtopo %d set up ----------",id);
        } 
    }

    //get the maximum size required for the buffers
    for (int id = 0; id < ntopo; id++) {
        if (switchtopo[id] != NULL) {
            max_mem = std::max(max_mem, switchtopo[id]->get_bufMemSize());
        }
    }
    FLUPS_CHECK(max_mem > 0, "number of memory %d should be >0", max_mem, LOCATION);

    *send_buff = (opt_double_ptr)flups_malloc(max_mem * sizeof(double));
    *recv_buff = (opt_double_ptr)flups_malloc(max_mem * sizeof(double));
    std::memset(*send_buff, 0, max_mem * sizeof(double));
    std::memset(*recv_buff, 0, max_mem * sizeof(double));

    // associate the buffers to the switchtopo
    for (int id = 0; id < ntopo; id++) {
        if (switchtopo[id] != NULL){
            switchtopo[id]->setup_buffers(*send_buff, *recv_buff);
        } 
    }
    END_FUNC;
}
void Solver::_deallocate_switchTopo(SwitchTopo **switchtopo, opt_double_ptr *send_buff, opt_double_ptr *recv_buff) {
    flups_free(*send_buff);
    flups_free(*recv_buff);
    (*send_buff) = NULL;
    (*recv_buff) = NULL;
}

/**
 * @brief allocates the plans in planmap according to that computed during the dry run, see \ref _init_plansAndTopos
 * 
 * @param topo the map of topos that will be applied to data
 * @param planmap the list of plans that we need to allocate
 * @param data pointer to data (on which the FFTs will be applied in place)
 */
void Solver::_allocate_plans(const Topology *const topo[3], FFTW_plan_dim *planmap[3], double *data) {
    BEGIN_FUNC;
    for (int ip = 0; ip < _ndim; ip++) {
        planmap[ip]->allocate_plan(topo[ip], data);
    }
    END_FUNC;
}

/**
 * @brief allocates memory depending on the requirements for the combination of topos in topo_hat
 * 
 * @param topo the map of successive topos that will be applied to data
 * @param topo_phys optionally, another topo which might drive the maximum allocated size
 * @param data poiter to the pointer to data
 */
void Solver::_allocate_data(const Topology *const topo[3], const Topology *topo_phys, double **data) {
    BEGIN_FUNC;
    //-------------------------------------------------------------------------
    /** - Sanity checks */
    //-------------------------------------------------------------------------
    FLUPS_CHECK((*data) == NULL, "Pointer has to be NULL for allocation", LOCATION);

    //-------------------------------------------------------------------------
    /** - Do the memory allocation */
    //-------------------------------------------------------------------------
    // the biggest size will be along the pencils
    size_t size_tot = 1;
    for (int id = 0; id < _ndim; id++) {
        size_tot = std::max(topo[id]->memsize(), size_tot);
    }
    if (topo_phys != NULL) {
        size_tot = std::max(topo_phys->memsize(), size_tot);
    }

    FLUPS_INFO_3("Complex memory allocation, size = %ld", size_tot);
    (*data) = (double *)flups_malloc(size_tot * sizeof(double));

    std::memset(*data, 0, size_tot * sizeof(double));

    //-------------------------------------------------------------------------
    /** - Check memory alignement */
    //-------------------------------------------------------------------------
    FLUPS_CHECK(FLUPS_ISALIGNED(*data), "FFTW alignement not compatible with FLUPS_ALIGNMENT (=%d)", FLUPS_ALIGNMENT, LOCATION);
    END_FUNC;
}

/**
 * @brief compute the Green's function
 * 
 * The Green function is always stored as a complex number (even if its complex part is 0).
 * This means that the all topos are turned to complex by this function (including the last one e.g.
 * for the case of a 3dirspectral).
 * 
 * @param topo the list of successive topos for the Green function
 * @param green ptr to the green function
 * @param planmap the list of successive maps to bring the Green function to full spectral
 * 
 * -----------------------------------
 * We do the following operations
 */
void Solver::_cmptGreenFunction(Topology *topo[3], double *green, FFTW_plan_dim *planmap[3]) {
    BEGIN_FUNC;

    //-------------------------------------------------------------------------
    /** - get the direction where we need to do spectral diff and count them */
    //-------------------------------------------------------------------------
    bool isSpectral[3] = {false};

    double hfact[3]    = {0.0, 0.0, 0.0};  // multiply the index by this factor to obtain the position (1/2/3 corresponds to x/y/z )
    double kfact[3]    = {0.0, 0.0, 0.0};  // multiply the index by this factor to obtain the wave number (1/2/3 corresponds to x/y/z )
    double koffset[3]  = {0.0, 0.0, 0.0};  // add this to the index to obtain the wave number (1/2/3 corresponds to x/y/z )
    double symstart[3] = {0.0, 0.0, 0.0};
    double epsilon     = _alphaGreen * _hgrid[0];  //the smoothing length scale of the HEJ kernels

    if ((_typeGreen == HEJ_2 || _typeGreen == HEJ_4 || _typeGreen == HEJ_6 || _typeGreen == LGF_2) && ((_ndim == 3 && (_hgrid[0] != _hgrid[1] || _hgrid[1] != _hgrid[2])) || (_ndim == 2 && _hgrid[0] != _hgrid[1]))) {
        FLUPS_ERROR("You are trying to use a regularized kernel or a LGF while not having dx=dy=dz.", LOCATION);
    }

    // get the infor + determine which green function to use:
    for (int ip = 0; ip < _ndim; ip++) {
        const int dimID = planmap[ip]->dimID();
        // get usefull datas
        isSpectral[dimID] = planmap[ip]->isSpectral();
        symstart[dimID]   = planmap[ip]->symstart();
        hfact[dimID]      = _hgrid[dimID];
        kfact[dimID]      = 0.0;
        koffset[dimID]    = 0.0;

        if (isSpectral[dimID]) {
            hfact[dimID]   = 0.0;
            kfact[dimID]   = planmap[ip]->kfact();
            koffset[dimID] = planmap[ip]->koffset(); // GF has a lda of 1
        }
        if (planmap[ip]->type() == FFTW_plan_dim::EMPTY) {
            // kill the hfact to have no influence in the green's functions
            hfact[dimID] = 0.0;
        }
    }

    // count the number of spectral dimensions and the green dimension
    int nbr_spectral = 0;
    for (int id = 0; id < _ndim; id++) {
        if (isSpectral[id]) {
            nbr_spectral++;
        }
    }

    //-------------------------------------------------------------------------
    /** - get the expression of Green in the full domain*/
    //-------------------------------------------------------------------------
    int n_unbounded = _ndim - nbr_spectral;
    if ((n_unbounded) == 3) {
        FLUPS_INFO(">> using Green function type %d on 3 dir unbounded", _typeGreen);
        cmpt_Green_3dirunbounded(topo[0], hfact, symstart, green, _typeGreen, epsilon);
    } else if ((n_unbounded) == 2) {
        FLUPS_CHECK(!(_typeGreen == LGF_2 && nbr_spectral == 1),"You cannot use LGF with one spectral direction!!",LOCATION);
        FLUPS_INFO(">> using Green function of type %d on 2 dir unbounded", _typeGreen);
        cmpt_Green_2dirunbounded(topo[0], hfact, kfact, koffset, symstart, green, _typeGreen, epsilon);
    } else if ((n_unbounded) == 1) {
        FLUPS_INFO(">> using Green function of type %d on 1 dir unbounded", _typeGreen);
        cmpt_Green_1dirunbounded(topo[0], hfact, kfact, koffset, symstart, green, _typeGreen, epsilon);
    } else if ((n_unbounded) == 0) {
        FLUPS_INFO(">> using Green function of type %d on 3 dir spectral", _typeGreen);
        cmpt_Green_0dirunbounded(topo[0], _hgrid[0], kfact, koffset, symstart, green, _typeGreen, epsilon);
    } else {
        FLUPS_ERROR("Sorry, the number of unbounded directions does not match: %d = %d - %d", n_unbounded, _ndim, nbr_spectral, LOCATION);
    }

    // dump the green func
#ifdef DUMP_DBG
    char msg[512];
    sprintf(msg, "green_%d%d%d_%dx%dx%d", planmap[0]->type(), planmap[1]->type(), planmap[2]->type(), topo[0]->nglob(0), topo[0]->nglob(1), topo[0]->nglob(2));
    hdf5_dump(topo[0], msg, green);
#endif

    //-------------------------------------------------------------------------
    /** - compute a symmetry and do the forward transform*/
    //-------------------------------------------------------------------------
    for (int ip = 0; ip < _ndim; ip++) {
        const int dimID = planmap[ip]->dimID();

        // go to the topology for the plan, if we are not already on it
        if (ip > 0) {
            _switchtopo_green[ip]->execute(green, FLUPS_FORWARD);
        }

        // execute the plan, if not already spectral
        if (!isSpectral[dimID]) {
            _plan_green[ip]->execute_plan(topo[ip], green);
        }

        if (_plan_green[ip]->isr2c_doneByFFT()) {
            topo[ip]->switch2complex();
        }
    }

    //-------------------------------------------------------------------------
    /** - scale the Green data using #_volfact */
    //-------------------------------------------------------------------------
    // - Explixitely destroying mode 0 ? no need to do that: we impose Green[0] is 0
    //   in full spectral.
    _scaleGreenFunction(topo[_ndim-1], green, false);

    //-------------------------------------------------------------------------
    /** - Complete the Green function in 2dirunbounded regularized case: we rewrite on the whole domain
     *      except the plane where k=0 in the spectral direction, as this was correctly computed. */
    // No need to scale this as that part of the Green function has a volfact = 1
    if (_ndim == 3 && nbr_spectral == 1 && (_typeGreen==HEJ_2||_typeGreen==HEJ_4||_typeGreen==HEJ_6)) {
        int istart_cstm[3] = {0, 0, 0};  //global

        for (int ip = 0; ip < 3; ip++) {
            const int dimID = planmap[ip]->dimID();
            istart_cstm[ip] = isSpectral[ip] ? 1 : 0;  //avoid rewriting on the part of Green already computed
            kfact[dimID]    = planmap[ip]->kfact();
        }
        cmpt_Green_0dirunbounded(topo[_ndim-1], _hgrid[0], kfact, koffset, symstart, green, _typeGreen, epsilon, istart_cstm, NULL);
    }

#ifdef DUMP_DBG
    hdf5_dump(topo[_ndim-1], "green_h", green);
#endif
    END_FUNC;
}

/**
 * @brief scales the Green's function given the #_volfact factor
 * 
 * @param topo the current topo
 * @param data the Green's function
 * @param killModeZero  specify if you want to kill what's in kx=ky=kz=0
 */
void Solver::_scaleGreenFunction(const Topology *topo, opt_double_ptr data, const bool killModeZero) {
    BEGIN_FUNC;
    // the symmetry is done along the fastest rotating index
    const int ax0 = topo->axis();
    const int ax1 = (ax0 + 1) % 3;
    const int ax2 = (ax0 + 2) % 3;

    const int    nf      = topo->nf();
    const int    nmem[3] = {topo->nmem(0), topo->nmem(1), topo->nmem(2)};
    const size_t onmax   = topo->nloc(ax1) * topo->nloc(ax2);
    const size_t inmax   = topo->nloc(ax0) * topo->nf();
    const double volfact = _volfact;

    FLUPS_CHECK(FLUPS_ISALIGNED(data) && (nmem[ax0] * topo->nf() * sizeof(double)) % FLUPS_ALIGNMENT == 0, "please use FLUPS_ALIGNMENT to align the memory", LOCATION);

    // do the loop
#pragma omp parallel for default(none) proc_bind(close) schedule(static) firstprivate(nf, onmax, inmax, nmem, data, volfact, ax0)
    for (int io = 0; io < onmax; io++) {
        opt_double_ptr dataloc = data + collapsedIndex(ax0, 0, io, nmem, nf);
        // set the alignment
        FLUPS_ASSUME_ALIGNED(dataloc, FLUPS_ALIGNMENT);
        for (size_t ii = 0; ii < inmax; ii++) {
            dataloc[ii] = dataloc[ii] * volfact;
        }
    }

    if (killModeZero) {
        int istart[3];

        topo->get_istart_glob(istart);
        if (istart[0] == 0 && istart[1] == 0 && istart[2] == 0) {
            for (int i0 = 0; i0 < topo->nf(); i0++) {
                data[i0] = 0.0;
            }
            FLUPS_INFO("Imposing Green's function mode 0 to be 0.");
        }
    }
    END_FUNC;
}

/**
 * @brief Finalize the Green function, and make sure it is stored according to the same topo as transformed data in full spectral space.
 * This is done to have the correct shiftgreen for the last plan if required.
 * After this routine, we can delete the green topologies. All we need to know is that now Green is compatible with the last field topo.
 * 
 * @param topo_field the last topology used for data (in full spectral)
 * @param green pointer to green function
 * @param topo the last topology used for green (in full spectral)
 * @param plan the last plan of the Green's function
 */
void Solver::_finalizeGreenFunction(Topology *topo_field, double *green, const Topology *topo, FFTW_plan_dim *planmap[3]) {
    BEGIN_FUNC;
    //-------------------------------------------------------------------------
    /** - If needed, we create a new switchTopo from the current Green topo to the field one */

    //simulate that we have done the transforms
    bool isr2c = false;
    for (int id = 0; id < _ndim; id++) {
        isr2c = isr2c || planmap[id]->isr2c();
    }
    if (isr2c) {
        topo_field->switch2complex();
    }

    FLUPS_CHECK(topo->nf() == topo_field->nf(), "Topo of Green has to be the same as Topo of field", LOCATION);
    FLUPS_CHECK(topo->nloc(0) == topo_field->nloc(0), "Topo of Green has to be the same as Topo of field", LOCATION);
    FLUPS_CHECK(topo->nloc(1) == topo_field->nloc(1), "Topo of Green has to be the same as Topo of field", LOCATION);
    FLUPS_CHECK(topo->nloc(2) == topo_field->nloc(2), "Topo of Green has to be the same as Topo of field", LOCATION);
    FLUPS_CHECK(topo->nglob(0) == topo_field->nglob(0), "Topo of Green has to be the same as Topo of field", LOCATION);
    FLUPS_CHECK(topo->nglob(1) == topo_field->nglob(1), "Topo of Green has to be the same as Topo of field", LOCATION);
    FLUPS_CHECK(topo->nglob(2) == topo_field->nglob(2), "Topo of Green has to be the same as Topo of field", LOCATION);

    //coming back (only if the last plan was r2c. No need it if was c2c or r2r...)
    if (planmap[_ndim - 1]->isr2c()) {
        topo_field->switch2real();
    }
    END_FUNC;
}

/**
 * @brief Solve the Poisson equation of the specified type.
 * 
 * The topology for field and rhs must be the same as that used at initilization of FLUPS.
 * 
 * @param field pointer to the solution 
 * @param rhs pointer to the field
 * @param type type of solver
 * 
 * -----------------------------------------------
 * We perform the following operations:
 */
void Solver::solve(double *field, double *rhs,const FLUPS_SolverType type) {
    BEGIN_FUNC;
    FLUPS_CHECK(!(type == ROT && _odiff == 0),"If calling the ROT solver, you need to initialize it with orderDiff = 1 or orderDiff = 2",LOCATION);
    //-------------------------------------------------------------------------
    /** - sanity checks */
    //-------------------------------------------------------------------------
    FLUPS_CHECK(field != NULL, "field is NULL", LOCATION);
    FLUPS_CHECK(rhs != NULL, "rhs is NULL", LOCATION);

    opt_double_ptr       mydata  = _data;

    if (_prof != NULL) _prof->start("solve");

    //-------------------------------------------------------------------------
    /** - clean the data memory */
    //-------------------------------------------------------------------------
    std::memset(mydata, 0, sizeof(double) * get_allocSize());

    //-------------------------------------------------------------------------
    /** - copy the rhs in the correct order */
    //-------------------------------------------------------------------------
    FLUPS_CHECK(_topo_phys->nf() == 1, "The RHS topology cannot be complex", LOCATION);

    do_copy(_topo_phys, rhs, FLUPS_FORWARD);

#ifdef DUMP_DBG
    hdf5_dump(_topo_phys, "rhs", mydata);
#endif
    //-------------------------------------------------------------------------
    /** - go to Fourier */
    //-------------------------------------------------------------------------
    do_FFT(mydata, FLUPS_FORWARD);

#ifdef DUMP_DBG
    hdf5_dump(_topo_hat[_ndim-1], "rhs_h", mydata);
#endif
    //-------------------------------------------------------------------------
    /** - Perform the magic */
    //-------------------------------------------------------------------------
    do_mult(mydata,type);

#ifdef DUMP_DBG
    // io if needed
    hdf5_dump(_topo_hat[_ndim-1], "sol_h", mydata);
#endif
    //-------------------------------------------------------------------------
    /** - go back to reals */
    //-------------------------------------------------------------------------
    if (type == STD) {
        do_FFT(mydata, FLUPS_BACKWARD);
    } else {
        do_FFT(mydata, FLUPS_BACKWARD_DIFF);
    }

    //-------------------------------------------------------------------------
    /** - copy the solution in the field */
    //-------------------------------------------------------------------------
    do_copy(_topo_phys, field, FLUPS_BACKWARD);

#ifdef DUMP_DBG
    // io if needed
    hdf5_dump(_topo_phys, "sol", field);
#endif
    // stop the whole timer
    if (_prof != NULL) _prof->stop("solve");
    END_FUNC;
}

/**
 * @brief copy from data to the object owned data or from the object owned data to data
 * 
 * @param topo 
 * @param data 
 * @param sign 
 */
void Solver::do_copy(const Topology *topo, double *data, const int sign ){
    BEGIN_FUNC;
    FLUPS_CHECK(data != NULL, "data is NULL", LOCATION);
    FLUPS_CHECK(_lda == topo->lda(),"the solver lda = %d must match the topology one = %d",_lda,topo->lda(),LOCATION);

    double* owndata = _data; 
    double* argdata = data;  

    if (_prof != NULL) {
        _prof->start("copy");
    }

    const int    ax0     = topo->axis();
    const int    ax1     = (ax0 + 1) % 3;
    const int    ax2     = (ax0 + 2) % 3;
    const int    nmem[3] = {topo->nmem(0), topo->nmem(1), topo->nmem(2)};
    const size_t memdim  = topo->memdim();
    const size_t ondim   = topo->nloc(ax1) * topo->nloc(ax2);
    const size_t onmax   = topo->nloc(ax1) * topo->nloc(ax2) * _lda;
    const size_t inmax   = topo->nloc(ax0);

    // if the data is aligned and the FRI is a multiple of the alignment we can go for a full aligned loop
    if (FLUPS_ISALIGNED(argdata) && (nmem[ax0] * topo->nf() * sizeof(double)) % FLUPS_ALIGNMENT == 0) {
        // do the loop
        if (sign == FLUPS_FORWARD) {
            //Copying from arg to own
#pragma omp parallel for default(none) proc_bind(close) schedule(static) firstprivate(onmax, inmax, owndata, argdata, nmem, ax0, ondim, memdim)
            for (int id = 0; id < onmax; id++) {
                // get the lia and the io
                const size_t lia = id / ondim;
                const size_t io  = id % ondim;
                // get the pointers
                opt_double_ptr argloc = argdata + lia * memdim + collapsedIndex(ax0, 0, io, nmem, 1);
                opt_double_ptr ownloc = owndata + lia * memdim + collapsedIndex(ax0, 0, io, nmem, 1);
                // set the alignment
                FLUPS_ASSUME_ALIGNED(argloc, FLUPS_ALIGNMENT);
                FLUPS_ASSUME_ALIGNED(ownloc, FLUPS_ALIGNMENT);
                for (size_t ii = 0; ii < inmax; ii++) {
                    ownloc[ii] = argloc[ii];
                }
            }
        } else {  //FLUPS_BACKWARD
                  //Copying from own to arg
#pragma omp parallel for default(none) proc_bind(close) schedule(static) firstprivate(onmax, inmax, owndata, argdata, nmem, ax0, ondim, memdim)
            for (int id = 0; id < onmax; id++) {
                // get the lia and the io
                const size_t lia = id / ondim;
                const size_t io  = id % ondim;
                // get the pointers
                opt_double_ptr argloc = argdata + lia * memdim + collapsedIndex(ax0, 0, io, nmem, 1);
                opt_double_ptr ownloc = owndata + lia * memdim + collapsedIndex(ax0, 0, io, nmem, 1);
                // set the alignment
                FLUPS_ASSUME_ALIGNED(argloc, FLUPS_ALIGNMENT);
                FLUPS_ASSUME_ALIGNED(ownloc, FLUPS_ALIGNMENT);
                for (size_t ii = 0; ii < inmax; ii++) {
                    argloc[ii] = ownloc[ii];
                }
            }
        }
    } else {
        // do the loop
        FLUPS_WARNING("loop uses unaligned access: alignment(&data[0]) = %d, alignment(data[i]) = %d. Please align your topology using FLUPS_ALIGNEMENT!!", FLUPS_CMPT_ALIGNMENT(argdata), (nmem[ax0] * topo->nf() * sizeof(double)) % FLUPS_ALIGNMENT, LOCATION);
        if (sign == FLUPS_FORWARD) {
            //Copying from arg to own
#pragma omp parallel for default(none) proc_bind(close) schedule(static) firstprivate(onmax, inmax, owndata, argdata, nmem, ax0, ondim, memdim)
            for (int id = 0; id < onmax; id++) {
                // get the lia and the io
                const size_t lia = id / ondim;
                const size_t io  = id % ondim;
                // get the pointers
                double *__restrict argloc = argdata + lia * memdim + collapsedIndex(ax0, 0, io, nmem, 1);
                opt_double_ptr ownloc     = owndata + lia * memdim + collapsedIndex(ax0, 0, io, nmem, 1);
                FLUPS_ASSUME_ALIGNED(ownloc, FLUPS_ALIGNMENT);
                for (size_t ii = 0; ii < inmax; ii++) {
                    ownloc[ii] = argloc[ii];
                }
            }
        } else {  //FLUPS_BACKWARD
                  //Copying from own to arg
#pragma omp parallel for default(none) proc_bind(close) schedule(static) firstprivate(onmax, inmax, owndata, argdata, nmem, ax0, ondim, memdim)
            for (int id = 0; id < onmax; id++) {
                // get the lia and the io
                const size_t lia = id / ondim;
                const size_t io  = id % ondim;
                // get the pointers
                double *__restrict argloc = argdata + lia * memdim + collapsedIndex(ax0, 0, io, nmem, 1);
                opt_double_ptr ownloc     = owndata + lia * memdim + collapsedIndex(ax0, 0, io, nmem, 1);
                FLUPS_ASSUME_ALIGNED(ownloc, FLUPS_ALIGNMENT);
                for (size_t ii = 0; ii < inmax; ii++) {
                    argloc[ii] = ownloc[ii];
                }
            }
        }
    }

    if (_prof != NULL) {
        _prof->stop("copy");
    }
    END_FUNC;
}

/**
 * @brief do the forward or backward fft on data (in place)
 * 
 * The topo assumed for data is the same as that used at FLUPS init.
 * 
 * @param data pointer to data
 * @param sign FLUPS_FORWARD or FLUPS_BACKWARD
 */
void Solver::do_FFT(double *data, const int sign){
    BEGIN_FUNC;
    FLUPS_CHECK(data != NULL, "data is NULL", LOCATION);
    
    opt_double_ptr  mydata  = data;

    if (sign == FLUPS_FORWARD) {
        for (int ip = 0; ip < _ndim; ip++) {
            // go to the correct topo
            _switchtopo[ip]->execute(mydata, FLUPS_FORWARD);
            // run the FFT
            if (_prof != NULL) _prof->start("fftw");
            _plan_forward[ip]->execute_plan(_topo_hat[ip], mydata);
            _plan_forward[ip]->correct_plan(_topo_hat[ip], mydata);
            if (_prof != NULL) _prof->stop("fftw");
            // get if we are now complex
            if (_plan_forward[ip]->isr2c()) {
                _topo_hat[ip]->switch2complex();
            }
        }
    } 
    else if (sign == FLUPS_BACKWARD) {  //FLUPS_BACKWARD
        for (int ip = _ndim-1; ip >= 0; ip--) {
            if (_prof != NULL) _prof->start("fftw");
            _plan_backward[ip]->correct_plan(_topo_hat[ip], mydata);
            _plan_backward[ip]->execute_plan(_topo_hat[ip], mydata);
            if (_prof != NULL) _prof->stop("fftw");
            // get if we are now complex
            if (_plan_forward[ip]->isr2c()) {
                _topo_hat[ip]->switch2real();
            }
            _switchtopo[ip]->execute(mydata, FLUPS_BACKWARD);
        }
    }
    else if (sign == FLUPS_BACKWARD_DIFF) {  //FLUPS_BACKWARD_DIFF
        for (int ip = _ndim-1; ip >= 0; ip--) {
            if (_prof != NULL) _prof->start("fftw");
            _plan_backward_diff[ip]->correct_plan(_topo_hat[ip], mydata);
            _plan_backward_diff[ip]->execute_plan(_topo_hat[ip], mydata);
            if (_prof != NULL) _prof->stop("fftw");
            // get if we are now complex
            if (_plan_forward[ip]->isr2c()) {
                _topo_hat[ip]->switch2real();
            }
            _switchtopo[ip]->execute(mydata, FLUPS_BACKWARD);
        }
    }
    END_FUNC;
}

/**
 * @brief actually do the convolution, i.e. multiply data by the Green's function (and optionially take the grad or the curl)
 * 
 * 
 * @param data 
 * @param type 
 */
void Solver::do_mult(double *data, const FLUPS_SolverType type) {
    BEGIN_FUNC;
    FLUPS_CHECK(data != NULL, "data is NULL", LOCATION);

    if (_prof != NULL) _prof->start("domagic");

    // every lda is done at once inside the dothemagic functions
    if (type == STD) {
        if (!_topo_hat[_ndim - 1]->isComplex()) {
            dothemagic_std_real(data);
        } else {
            dothemagic_std_complex(data);
        }
    } else {
        // compute the coefficients: kfact, koffset and symstart
        double kabs[3];
        double koffset[3];
        double symstart[3];
        get_spectralInfo(kabs,koffset,symstart);

        // for each dim, comnput the kfact depening on the coordinate
        double kfact[3][3][2];  // kfact is COMPLEX
        for (int ip = 0; ip < 3; ip++) {
            const int dimID = _plan_forward[ip]->dimID();
            for (int lia = 0; lia < _lda; lia++) {
                //compute the number of rotation
                int corrphase = 0;
                if (_plan_forward[ip]->imult(lia)) {  // while doing a DST forward, we need to nultiplied by (-i)
                    corrphase--;
                }
                if (_plan_backward_diff[ip]->imult(lia)) {  // while doing a DST backward, we need to rephase by (i)
                    corrphase++;
                }
                // make the change if needed
                if (corrphase == 0) {  // derivative = * (ik)
                    kfact[dimID][lia][0] = 0.0;
                    kfact[dimID][lia][1] = kabs[dimID];
                } else if (corrphase == +1) {  // deriv = * (i k) * (i) = -k
                    kfact[dimID][lia][0] = -kabs[dimID];
                    kfact[dimID][lia][1] = 0.0;
                } else if (corrphase == -1) {  // deriv = * (i k) * (-i) = k
                    kfact[dimID][lia][0] = kabs[dimID];
                    kfact[dimID][lia][1] = 0.0;
                }
            }
        }
        if (!_topo_hat[_ndim - 1]->isComplex()) {
            if (_odiff == 1) {
                dothemagic_rot_real_o1(data, koffset, kfact, symstart);
            } else if (_odiff == 2) {
                dothemagic_rot_real_o2(data, koffset, kfact, symstart,_hgrid);
            }
        } else {
            if (_odiff == 1) {
                dothemagic_rot_complex_o1(data, koffset, kfact, symstart);
            } else if (_odiff == 2) {
                dothemagic_rot_complex_o2(data, koffset, kfact, symstart,_hgrid);
            }
        }
    }

    if (_prof != NULL) _prof->stop("domagic");
    END_FUNC;
}

//---------------------------------
// kind = 0: real to real case
#define KIND 0
#include "dothemagic_std.ipp"
#undef KIND
// 01 = real to real, spectral acc
#define KIND 01
#include "dothemagic_rot.ipp"
#undef KIND
// 02= real to real, 2nd order acc
#define KIND 02
#include "dothemagic_rot.ipp"
#undef KIND
//---------------------------------
// kind = 1: complex to complex
#define KIND 1
#include "dothemagic_std.ipp"
#undef KIND
// kind = 11: complex to complex spectral acc
#define KIND 11
#include "dothemagic_rot.ipp"
#undef KIND
// kind = 12: complex to complex 2nd order acc
#define KIND 12
#include "dothemagic_rot.ipp"
#undef KIND

/**
 * @brief reorder the MPI-ranks using metis
 * 
 * @warning this functions assume an evenly distributed amount of procs on the nodes
 * 
 * @param comm 
 * @param sources 
 * @param sourcesW 
 * @param dests 
 * @param destsW 
 * @param n_nodes 
 * @param order 
 */
void Solver::_reorder_metis(MPI_Comm comm, int *sources, int *sourcesW, int *dests, int *destsW, int *order) {
    int comm_size;
    int comm_rank;
    MPI_Comm_rank(comm, &comm_rank);
    MPI_Comm_size(comm, &comm_size);

#ifdef HAVE_METIS

    //-------------------------------------------------------------------------
    /** - get the total number of nodes */
    //-------------------------------------------------------------------------
    // create a group where everybody can create a shared memory region
    MPI_Comm nodecomm;
    MPI_Info mpinfo;
    MPI_Info_create(&mpinfo);
    MPI_Comm_split_type(comm, MPI_COMM_TYPE_SHARED, comm_rank, mpinfo, &nodecomm);
    // we store the comm size
    int local_nodesize;
    MPI_Comm_size(nodecomm, &local_nodesize);

    // gather on proc 1 the number of proc per node
    int *vec_nodesize = (int *)flups_malloc(sizeof(int) * comm_size);
    MPI_Allgather(&local_nodesize, 1, MPI_INT, vec_nodesize, 1, MPI_INT, comm);
    
    // count the number of partitions we'll need:
    int n_nodes = 0;
    int id = 0;
    while( id < comm_size){
        id += vec_nodesize[id];
        n_nodes++;
    }
    
#ifdef DEV_SIMULATE_GRAPHCOMM
    //CHEATING: imposing that there will be 2 groups (there needs to be at least 4 procs)
    n_nodes = 2;
    for (int ip = 0; ip<comm_size; ip++ ){
        vec_nodesize[ip]=comm_size/3;
    }
    vec_nodesize[0]=comm_size-vec_nodesize[1];
#endif

    real_t* tpwgts = (real_t*) flups_malloc(sizeof(real_t)*n_nodes);
    // deduce the size of each partition:
    id = 0;
    for (int ip = 0; ip<n_nodes; ip++ ){
        tpwgts[ip] = ((real_t)vec_nodesize[id])/((real_t) comm_size);
        id += vec_nodesize[id];
    }
    //______________________________________________

    // free stuffs
    flups_free(vec_nodesize);
    MPI_Comm_free(&nodecomm);

    //-------------------------------------------------------------------------
    /** - get the neighbour list and the associated weights */
    //-------------------------------------------------------------------------
    // we count the number of neighbours
    int n_neighbours = 0;
    // if we have either one way in or one way out, we have a neighbour
    for (int i = 0; i < comm_size; ++i) {
        if ((sourcesW[i] + destsW[i]) > 0 && i != comm_rank) n_neighbours++;
    }
    // allocate the number of neighbours and their weights
    int *neighbours = (int *)flups_malloc(sizeof(int) * n_neighbours);
    int *weights    = (int *)flups_malloc(sizeof(int) * n_neighbours);
    n_neighbours    = 0;
    for (int i = 0; i < comm_size; ++i) {
        if (sourcesW[i] + destsW[i] > 0 && i != comm_rank) {
            neighbours[n_neighbours] = i;
            weights[n_neighbours]    = sourcesW[i] + destsW[i];
            n_neighbours++;
        }
    }

    //-------------------------------------------------------------------------
    /** - build the graph on proc 0 and ask for partioning
     * The graph structure follows metis rules:
     * the edges (= id of the destination of the edges) starting from proc k are located
     * from adj[xadj[k]] to adj[xadj[k+1]-1]
     * Same structure is used for the weights with the ajdw
     * */
    //-------------------------------------------------------------------------
    if (comm_rank == 0) {
        int *xadj = (int *)flups_malloc((comm_size + 1) * sizeof(int));
        int *nadj = (int *)flups_malloc((comm_size) * sizeof(int));

        // get the number of neighbours from everybody
        MPI_Gather(&n_neighbours, 1, MPI_INT, nadj, 1, MPI_INT, 0, comm);
        // get the starting indexes of the neighbour description for everybody
        xadj[0] = 0;
        for (int i = 0; i < comm_size; ++i) {
            xadj[i + 1] = xadj[i] + nadj[i];
        }

        // allocate the adjency list + weights and fill it with the neighbour list from everybody
        int *adj  = (int *)flups_malloc(xadj[comm_size] * sizeof(int));
        int *adjw = (int *)flups_malloc(xadj[comm_size] * sizeof(int));
        MPI_Gatherv(neighbours, n_neighbours, MPI_INT, adj, nadj, xadj, MPI_INT, 0, comm);
        MPI_Gatherv(weights, n_neighbours, MPI_INT, adjw, nadj, xadj, MPI_INT, 0, comm);
#ifdef PROF
        {
            //writing graph to file, CSR format
            string filename = "prof/graph.csr";
            FILE* file      = fopen(filename.c_str(), "w+");
            if(file==NULL){FLUPS_ERROR("Could not create file in ./prof. Did you create the folder?",LOCATION);}
            for(int i=0; i<=comm_size; i++){
                fprintf(file, "%d ",xadj[i]);
            }
            fprintf(file,"\n");
            for(int i=0; i<xadj[comm_size]; i++){
                fprintf(file, "%d (%d), ",adj[i],adjw[i]);
            }
            fprintf(file,"\n");
            fclose(file);

            //writing graph to file, per node
            filename = "prof/graph.txt";
            file     = fopen(filename.c_str(), "w+");
            for(int i=0; i<comm_size; i++){
                fprintf(file, "%d: ",i);
                for(int j = xadj[i]; j<xadj[i+1]; j++){
                        fprintf(file, "%d (%d), ",adj[j],adjw[j]);
                }
                fprintf(file,"\n");
            }
            fclose(file);
        }
#endif

        //prepare vall to metis
        int  ncon = 1;  // the number of balancing constraints
        real_t tol = 1.0001; //tolerance on the constraint 
        int  objval;
        int *part = (int *)flups_malloc(comm_size * sizeof(int));
        int *rids = (int *)flups_malloc(n_nodes * sizeof(int));
        std::memset(rids,0,sizeof(int)*n_nodes);

        // ask of the partitioning. call metis several times in case the tolerance on the partition size is not exactly respected 
        int max_iter = 10;
        if(n_nodes==1){
            max_iter = 0;
            FLUPS_WARNING("METIS: you asked only 1 node. I can't do the partitaioning.",LOCATION);
        }
        int iter;
        idx_t options[METIS_NOPTIONS];
        METIS_SetDefaultOptions(options);

        //METIS options: ncuts and niter seems to have the most effect
        options[METIS_OPTION_SEED] = 1;
        options[METIS_OPTION_NCUTS] = 50;
        options[METIS_OPTION_NITER] = 50;
        options[METIS_OPTION_UFACTOR] = 1.;
        // options[METIS_OPTION_IPTYPE] = 
        //     METIS_IPTYPE_GROW,
        //     METIS_IPTYPE_RANDOM,
        //     METIS_IPTYPE_EDGE,
        //     METIS_IPTYPE_NODE,
        //     METIS_IPTYPE_METISRB
        // options[METIS_OPTION_RTYPE] = 
        //     METIS_RTYPE_FM,
        //     METIS_RTYPE_GREEDY,
        //     METIS_RTYPE_SEP2SIDED,
        //     METIS_RTYPE_SEP1SIDED

        for(iter = 0; iter<max_iter; iter++){
            FLUPS_INFO("METIS: graph partitioning attempt %d",iter+1);
            METIS_PartGraphRecursive(&comm_size, &ncon, xadj, adj, NULL, NULL, adjw, &n_nodes, tpwgts, &tol, options, &objval, part);
            tol=((tol-1.)/2.)+1.;
            options[METIS_OPTION_NCUTS] +=10;
            options[METIS_OPTION_NITER] +=10;
            options[METIS_OPTION_SEED] += 1; 
            // options[METIS_OPTION_UFACTOR] /= 1.;
            
            // compute how many proc in each group, resulting from metis partitioning
            for (int i = 0; i < comm_size; ++i) {
                rids[part[i]]++;
            }
            // check that we did respect the constraint on the size of the partitions
            bool succeed = (rids[0] == (int)(tpwgts[0] * comm_size));
            FLUPS_INFO("METIS:   part %d: size %d (should be %d)",0, rids[0], (int)(tpwgts[0] * comm_size));
            for (int ip = 1; ip < n_nodes; ++ip) {
                succeed &= (rids[ip] == (int)(tpwgts[ip] * comm_size));
                FLUPS_INFO("METIS:   part %d: size %d (should be %d)",ip, rids[ip], (int)(tpwgts[ip] * comm_size));
                rids[ip] += rids[ip-1]; //switch to cumulative numbering
            }
            for (int ip = n_nodes-1; ip > 0; --ip) {
                rids[ip] = rids[ip-1]; //offset by 1
            }
            rids[0] = 0;
            if(!succeed){
                FLUPS_INFO("METIS:   attempt failed.");
            }else{
                // assign the rank value and redistribute
                for (int i = 0; i < comm_size; ++i) {
                    order[i] = rids[part[i]]++ ;
                }
                break;
            }
        }
        // check that we did not reach max_iter
        if(iter>=max_iter){
            FLUPS_WARNING("Failed to find a graph partitioning with the current allocation. I will not change the rank orderegin in the graph_comm!",LOCATION);
            for (int i = 0; i < comm_size; ++i) {
                order[i] = i;
            }
        }

        // result of the partitioning
    #ifdef PART_OF_EQUAL_SIZE   
        FLUPS_INFO("I have partitioned the graph in %d chunks of size %d\n",n_nodes,comm_size/n_nodes);
    #else            
        FLUPS_INFO("I have partitioned the graph in %d chunks.",n_nodes);
    #endif
#ifdef PROF
        //writing graph to file, CSR format
        string filename = "prof/partitions.txt";
        FILE* file      = fopen(filename.c_str(), "w+");
    #ifdef PART_OF_EQUAL_SIZE   
        fprintf(file,"%d partitions of size %d\n",n_nodes,comm_size/n_nodes);
    #else
        fprintf(file,"%d partitions of size:\n",n_nodes);
        for(int i=0; i<n_nodes; i++){
            fprintf(file, "part %d with %d elems\n",i,(int)(comm_size*tpwgts[i]));
        }
    #endif
        fclose(file);
#endif        

        flups_free(xadj);
        flups_free(nadj);
        flups_free(adj);
        flups_free(adjw);

        flups_free(part);
        flups_free(rids);
    } else {
        MPI_Gather(&n_neighbours, 1, MPI_INT, NULL, 1, MPI_INT, 0, comm);
        MPI_Gatherv(neighbours, n_neighbours, MPI_INT, NULL, NULL, NULL, MPI_INT, 0, comm);
        MPI_Gatherv(weights, n_neighbours, MPI_INT, NULL, NULL, NULL, MPI_INT, 0, comm);
    }
    flups_free(neighbours);
    flups_free(weights);
#ifndef PART_OF_EQUAL_SIZE
    flups_free(tpwgts);
#endif

    //-------------------------------------------------------------------------
    /** - give the rank info to everybody */
    //-------------------------------------------------------------------------
    MPI_Bcast(order, comm_size, MPI_INT, 0, comm);
#ifdef PROF        
    if (comm_rank == 3) {
        //writing reordering to file
        string filename = "prof/order.txt";
        FILE* file      = fopen(filename.c_str(), "w+");
        for (int i = 0; i < comm_size; ++i) {
            FLUPS_INFO("METIS ORDER %i : %i \n", i, order[i]);
            printf("%i : %i \n", i, order[i]);
            fprintf(file,"%i : %i \n", i, order[i]);
        }
        fclose(file);
    }
#endif
#else
    for (int i = 0; i < comm_size; ++i) {
        order[i] = i;
    }
#endif
}
