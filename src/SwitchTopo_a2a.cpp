/**
 * @file SwitchTopo_a2a.cpp
 * @author Thomas Gillis and Denis-Gabriel Caprace
 * @brief 
 * @version
 * @date 2019-09-25
 * 
 * @copyright Copyright © UCLouvain 2019
 * 
 * FLUPS is a Fourier-based Library of Unbounded Poisson Solvers.
 * 
 * Copyright (C) <2019> <Université catholique de Louvain (UCLouvain), Belgique>
 * 
 * List of the contributors to the development of FLUPS, Description and complete License: see LICENSE file.
 * 
 * This program (FLUPS) is free software: 
 * you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program (see COPYING file).  If not, 
 * see <http://www.gnu.org/licenses/>.
 * 
 */

#include "SwitchTopo_a2a.hpp"

/**
 * @brief Construct a Switch Topo object
 * 
 * Let us consider the switch from the TOPO_IN to the TOPO_OUT.
 *
 * ```
 * +------------------------------------+
 * |  TOPO_OUT  |                       |
 * |            |                       |
 * |            |  n=5                  |
 * |            |                       |
 * |            v                       |
 * |  --------> +-------------+         |
 * |    n=3     | TOPO_IN     |         |
 * |            |             |         |
 * |            |             |         |
 * |            |             |         |
 * |            +-------------+         |
 * |                                    |
 * |                                    |
 * |                                    |
 * +------------------------------------+
 * ```
 * 
 * The shift argument will then be (3,5) since we need to add (3,5) points in the topo_output
 * to reach the (0,0,0) point in the topo_input.
 * 
 * The switch between topologies works using blocks. 
 * A block is defined as a memory block on one proc that goes on another proc.
 * The size of the block will always have the same size on every process.
 * The number of block changes from one process to another.
 * Therefore we have to initialize the block structure and then use it during the execute.
 * 
 * 
 * @param topo_input the input topology
 * @param topo_output the output topology 
 * @param shift the shift is the position of the (0,0,0) of topo_input in the topo_output indexing (in XYZ-indexing)
 * @param prof the profiler to use to profile the execution of the SwitchTopo
 */
using namespace FLUPS;

SwitchTopo_a2a::SwitchTopo_a2a(const Topology* topo_input, const Topology* topo_output, const int shift[3], Profiler* prof) {
    BEGIN_FUNC;

    FLUPS_CHECK(topo_input->isComplex() == topo_output->isComplex(), "both topologies have to be the same kind", LOCATION);

    int rank, comm_size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &comm_size);

    _topo_in  = topo_input;
    _topo_out = topo_output;
    _prof     = prof;

    //-------------------------------------------------------------------------
    /** - get the starting and ending index of the shared zone */
    //-------------------------------------------------------------------------
    // get the blockshift
    _topo_in->cmpt_intersect_id(shift, _topo_out, _istart, _iend);
    int tmp[3] = {-shift[0],-shift[1],-shift[2]};
    _topo_out->cmpt_intersect_id(tmp, _topo_in, _ostart, _oend);

    //-------------------------------------------------------------------------
    /** - get the block size as the GCD of the memory among every process between send and receive */
    //-------------------------------------------------------------------------
    int* nperProc = (int*)fftw_malloc(comm_size * sizeof(int));
    for (int id = 0; id < 3; id++) {
        // get the gcd between send and receive
        int isend = (_iend[id] - _istart[id]);
        int osend = (_oend[id] - _ostart[id]);
        
        // compute the exchanged size same if from the input or output
        MPI_Allreduce(&isend,&_exSize[id],1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);
        
        // if I am the last one, I decrease the blocksize by one if needed
        if(_topo_in->rankd(id) == (_topo_in->nproc(id)-1)){
            isend = isend - _exSize[id]%2;
        }
        if(_topo_out->rankd(id) == (_topo_out->nproc(id)-1)){
            osend = osend - _exSize[id]%2;
        }
        int npoints = gcd(isend,osend);
        // gather on each proc the gcd
        MPI_Allgather(&npoints, 1, MPI_INT, nperProc, 1, MPI_INT, MPI_COMM_WORLD);
        // get the Greatest Common Divider among every process
        int my_gcd = nperProc[0];
        for (int ip = 1; ip < comm_size; ip++) {
            my_gcd = gcd(my_gcd, nperProc[ip]);
        }
        // store it as the block dimension
        _nByBlock[id] = my_gcd;
    }
    fftw_free(nperProc);

// #ifdef PERF_VERBOSE
    if (rank == 0) {
        FILE* file = fopen("./prof/blocksize.txt","a+");
        if(file != NULL){
            fprintf(file,"SwitchTopo %d to %d: blocksize = %d %d %d\n",topo_input->axis(),topo_output->axis(),_nByBlock[0],_nByBlock[1],_nByBlock[2]);
            fclose(file);
        }
    }
// #endif

    //-------------------------------------------------------------------------
    /** - get the number of blocks and for each block get the size and the destination rank */
    //-------------------------------------------------------------------------
    int iblockIDStart[3];
    int oblockIDStart[3];
    int* inBlockEachProc = (int*)fftw_malloc(comm_size * 3 * sizeof(int));
    int* onBlockEachProc = (int*)fftw_malloc(comm_size * 3 * sizeof(int));

    cmpt_blockIndexes(_istart, _iend, _nByBlock, _topo_in, _inBlock, iblockIDStart, inBlockEachProc);
    cmpt_blockIndexes(_ostart, _oend, _nByBlock, _topo_out, _onBlock, oblockIDStart, onBlockEachProc);

    // allocte the block size
    for (int id = 0; id < 3; id++) {
        _iBlockSize[id] = (int*)fftw_malloc(_inBlock[0] * _inBlock[1] * _inBlock[2] * sizeof(int));
        _oBlockSize[id] = (int*)fftw_malloc(_onBlock[0] * _onBlock[1] * _onBlock[2] * sizeof(int));
    }

    // allocate the destination ranks
    _i2o_destRank = (opt_int_ptr)fftw_malloc(_inBlock[0] * _inBlock[1] * _inBlock[2] * sizeof(int));
    _o2i_destRank = (opt_int_ptr)fftw_malloc(_onBlock[0] * _onBlock[1] * _onBlock[2] * sizeof(int));

    // get the send destination ranks in the ouput topo
    cmpt_blockSize(_inBlock, iblockIDStart, _nByBlock, _istart, _iend, _iBlockSize);
    cmpt_blockSize(_onBlock, oblockIDStart, _nByBlock, _ostart, _oend, _oBlockSize);

    cmpt_blockDestRank(_inBlock, iblockIDStart, _topo_out, onBlockEachProc, _i2o_destRank);
    cmpt_blockDestRank(_onBlock, oblockIDStart, _topo_in, inBlockEachProc, _o2i_destRank);

    // free the temp arrays
    fftw_free(inBlockEachProc);
    fftw_free(onBlockEachProc);

    //-------------------------------------------------------------------------
    /** - do the communication split */
    //-------------------------------------------------------------------------
    // compute the color among the proc I send to and I recv from
    FLUPS_INFO("Trying to determine the MPI communicators...");
    int mycolor = rank;
    int* colors =(int*) fftw_malloc(comm_size * sizeof(int));
    bool* inMyGroup =(bool*) fftw_malloc(comm_size * sizeof(bool));

    for (int ir = 0; ir < comm_size; ir++) {
        inMyGroup[ir] = false;
    }

    // do a first pass and give a color + who is in my group
    for (int ib = 0; ib < _inBlock[0] * _inBlock[1] * _inBlock[2]; ib++) {
        mycolor = std::min(mycolor, _i2o_destRank[ib]);
        inMyGroup[_i2o_destRank[ib]] = true;
    }
    for (int ib = 0; ib < _onBlock[0] * _onBlock[1] * _onBlock[2]; ib++) {
        mycolor = std::min(mycolor, _o2i_destRank[ib]);
        inMyGroup[_o2i_destRank[ib]] = true;
    }

    // count how much is should be in my group
    // by default we assume that nobody is in the same group
    int nleft = 0;
    for (int ir = 0; ir < comm_size; ir++) {
        if (inMyGroup[ir]) {
            nleft += 1;
        }
    }
    // continue while we haven't found a solution
    while (nleft > 0 ) {
        // gather the color info from everyone
        MPI_Allgather(&mycolor, 1, MPI_INT, colors, 1, MPI_INT, MPI_COMM_WORLD);
        // iterate on the proc
        int n_notInMyGroup = 0;
        for (int ir = 0; ir < comm_size; ir++) {
            // if it is reachable and the color is not already the same
            if (inMyGroup[ir] && (colors[ir] != mycolor)) {
                // we first increment the counter flagging that one is missing
                n_notInMyGroup += 1;
                // then we solve the problem if we are able to do so....
                // remove 1 if we are able to solve the issue <=> my color > colors[ir]
                n_notInMyGroup = n_notInMyGroup - (colors[ir] < mycolor);
                // changing if possible
                mycolor = std::min(mycolor, colors[ir]);
            }
        }
        // compute among everybody, if we need to continue
        MPI_Allreduce(&n_notInMyGroup,&nleft,1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);
        FLUPS_INFO("stil %d to find (@ my proc: %d)",nleft,n_notInMyGroup);
    }
    fftw_free(colors);
    fftw_free(inMyGroup);

    FLUPS_INFO("Group found: my color = %d",mycolor);
    
    // create the new communicator
    MPI_Comm_split(MPI_COMM_WORLD, mycolor, rank, &_subcomm);

    _setup_subComm(_subcomm, _inBlock, _i2o_destRank, &_i2o_count, &_i2o_start);
    _setup_subComm(_subcomm, _onBlock, _o2i_destRank, &_o2i_count, &_o2i_start);

    // determine if we are a All to All
    int subsize;
    MPI_Comm_size(_subcomm,&subsize);

    int tmp_size  = _i2o_count[0];
    _is_all2all = (tmp_size != 0);
    for (int ir = 1; ir < subsize; ir++) {
        // if the count from and to the rank is the same, we can do an A2A
        _is_all2all = _is_all2all && (tmp_size == _i2o_count[ir]);
        _is_all2all = _is_all2all && (tmp_size == _o2i_count[ir]);
    }
    FLUPS_INFO("End of init SwitchTopo with a group of size",subsize);

    //-------------------------------------------------------------------------
    /** - initialize the profiler    */
    //-------------------------------------------------------------------------
    if (_prof != NULL) {
        _prof->create("reorder","solve");
        _prof->create("mem2buf","reorder");
        _prof->create("buf2mem","reorder");
        _prof->create("waiting","reorder");
    }
}

void SwitchTopo_a2a::setup_buffers(opt_double_ptr sendData, opt_double_ptr recvData) {

    _sendBuf = (double**) fftw_malloc(_inBlock[0] * _inBlock[1] * _inBlock[2]*sizeof(double*));
    _recvBuf = (double**) fftw_malloc(_onBlock[0] * _onBlock[1] * _onBlock[2]*sizeof(double*));

    for (int ib = 0; ib < _inBlock[0] * _inBlock[1] * _inBlock[2]; ib++) {
        // get the destination rank
        int destrank = _i2o_destRank[ib];
        // we count the number of blocks on the ranks bellow me
        size_t memblocks = 0;
        for (int ir = 0; ir < destrank; ir++) {
            memblocks += (size_t)_i2o_count[ir];
        }
        _sendBuf[ib] = sendData + memblocks;
        FLUPS_INFO("linking block %d with an offset of %d",ib,memblocks);
    }

    for (int ib = 0; ib < _onBlock[0] * _onBlock[1] * _onBlock[2]; ib++) {
        // get the destination rank
        int destrank = _o2i_destRank[ib];
        // we count the number of blocks on the ranks bellow me
        size_t memblocks = 0;
        for (int ir = 0; ir < destrank; ir++) {
            memblocks += (size_t)_o2i_count[ir];
        }
        _recvBuf[ib] = recvData + memblocks;
    }

    // if we are all to all, we should destroy info: start and count except the first one
}

void SwitchTopo_a2a::_setup_subComm(MPI_Comm newcomm, const int nBlock[3], int* destRank, int** count,int** start) {

    //-------------------------------------------------------------------------
    /** - get the new destination ranks    */
    //-------------------------------------------------------------------------
    int newrank,worldsize;
    MPI_Comm_rank(newcomm, &newrank);
    MPI_Comm_size(MPI_COMM_WORLD, &worldsize);

    // get the new ranks from my old friends in the old communicator
    int* newRanks =(int*) fftw_malloc(worldsize * sizeof(int));
    MPI_Allgather(&newrank, 1, MPI_INT, newRanks, 1, MPI_INT, MPI_COMM_WORLD);
    // replace the old ranks by the newest ones
    for (int ib = 0; ib < nBlock[0] * nBlock[1] * nBlock[2]; ib++) {
        destRank[ib] = newRanks[destRank[ib]];
    }
    fftw_free(newRanks);

    //-------------------------------------------------------------------------
    /** - build the size vector of block to each procs    */
    //-------------------------------------------------------------------------
    int subsize;
    MPI_Comm_size(_subcomm, &subsize);

    printf("subsize = %d\n",subsize);

    // count the number of blocks to each process
    (*count) =(int*) fftw_malloc(subsize * sizeof(int));
    (*start) =(int*) fftw_malloc(subsize * sizeof(int));
    std::memset((*count), 0, subsize * sizeof(int));
    std::memset((*start), 0, subsize * sizeof(int));
    // get the size per block
    const int blockMem = get_blockMemSize();
    // count the number of blocks per rank
    for (int ib = 0; ib < nBlock[0] * nBlock[1] * nBlock[2]; ib++) {
        printf("the destination rank of block %d  = %d\n",ib,destRank[ib]);
        (*count)[destRank[ib]] += blockMem;
    }
    // compute the start indexes
    (*start)[0] = 0;
    for (int ir = 1; ir < subsize; ir++) {
        (*start)[ir] = (*start)[ir-1] + (*count)[ir];
    }
}

// void SwitchTopo::setup_buffers(opt_double_ptr my_sendBuf,opt_double_ptr my_recvBuf){
//     BEGIN_FUNC;

//     int rank;
//     MPI_Comm_rank(MPI_COMM_WORLD,&rank);

//     //-------------------------------------------------------------------------
//     /** - Store the buffers */
//     //-------------------------------------------------------------------------
//     _sendBuf = my_sendBuf;
//     _recvBuf = my_recvBuf;
    
//     //-------------------------------------------------------------------------
//     /** - for each block we associate the the data buffer and the MPI requests or associate it to NULL */
//     //-------------------------------------------------------------------------
//     int icount = 0;
//     for (int bid = 0; bid < _inBlock[0] * _inBlock[1] * _inBlock[2]; bid++) {
//         //create the request
//         const int datasize = _iBlockSize[0][bid] * _iBlockSize[1][bid] * _iBlockSize[2][bid] * _topo_out->nf();
//         // for the send when doing input 2 output: send to rank i2o with tag _i2o_destTag[bid]
//         if (_i2o_destRank[bid] == rank) {
//             // save the bid
//             _iselfBlockID[icount] = bid;
//             // associate the request to NULL
//             _i2o_sendRequest[bid] = MPI_REQUEST_NULL;
//             _o2i_recvRequest[bid] = MPI_REQUEST_NULL;
//             // increment the counter
//             icount++;
//         } else {
//             MPI_Send_init(_sendBuf[bid], datasize, MPI_DOUBLE, _i2o_destRank[bid], _i2o_destTag[bid], MPI_COMM_WORLD, &(_i2o_sendRequest[bid]));
//             // for the send when doing output 2 input: send to rank o2i with tag o2i
//             MPI_Recv_init(_sendBuf[bid], datasize, MPI_DOUBLE, _i2o_destRank[bid], bid, MPI_COMM_WORLD, &(_o2i_recvRequest[bid]));
//         }
//     }
//     int ocount = 0;
//     for (int bid = 0; bid < _onBlock[0] * _onBlock[1] * _onBlock[2]; bid++) {
//         //create the request
//         const int datasize = _oBlockSize[0][bid] * _oBlockSize[1][bid] * _oBlockSize[2][bid] * _topo_out->nf();
//         if (_o2i_destRank[bid] == rank) {
//             // save the bid
//             _oselfBlockID[ocount] = bid;
//             // associate the request to NULL
//             _i2o_recvRequest[bid] = MPI_REQUEST_NULL;
//             _o2i_sendRequest[bid] = MPI_REQUEST_NULL;
//             // increment the counter
//             ocount++;
//         } else {
//             // for the reception when doing input 2 output: receive from the rank o2i with tag bid
//             MPI_Recv_init(_recvBuf[bid], datasize, MPI_DOUBLE, _o2i_destRank[bid], bid, MPI_COMM_WORLD, &(_i2o_recvRequest[bid]));
//             // for the send when doing output 2 input: send to rank o2i with tag o2i
//             MPI_Send_init(_recvBuf[bid], datasize, MPI_DOUBLE, _o2i_destRank[bid], _o2i_destTag[bid], MPI_COMM_WORLD, &(_o2i_sendRequest[bid]));
//         }
//     }
// }

/**
 * @brief Destroy the Switch Topo
 * 
 */
SwitchTopo_a2a::~SwitchTopo_a2a() {
    BEGIN_FUNC;

    MPI_Comm_free(&_subcomm);

    if (_i2o_destRank != NULL) fftw_free(_i2o_destRank);
    if (_o2i_destRank != NULL) fftw_free(_o2i_destRank);

    if (_i2o_count != NULL) fftw_free(_i2o_count);
    if (_o2i_count != NULL) fftw_free(_o2i_count);

    for(int id=0; id<3; id++){
        if(_iBlockSize[id] != NULL) fftw_free(_iBlockSize[id]);
        if(_oBlockSize[id] != NULL) fftw_free(_oBlockSize[id]);
    }
}

/**
 * @brief execute the switch from one topo to another
 * 
 * #### Buffer writting
 * The buffer memory writting is done according to the axis of the input topologies.
 * This allows to have a continuous memory access while filling the buffer.
 * 
 * We go through each block and we fill it using the local memory.
 * After a block has been filled it is send using the non-blocking send.
 * Since the writting of buffers is aligned with the topo_in axis, the loops are continuous in memory and fully vectorizable.
 * 
 * @warning
 * Let us note that the block is send with a tag which is its local index in the destination proc.
 * 
 * #### Buffer reading
 * We wait to receive one block of memory. Once one has been received, we do the copy.
 * The buffer reading has to follow the same order as in the buffer writting, so the axis of the topo_in in the inner loop.
 * 
 * The reading of the buffer is hence continuous but the writting inside the memory has an apriori unkown stride.
 * The stride may be computed using the difference of axis between the two topologies.
 * Hence the reading will be a bit slower since the writting due to memory discontinuities
 * 
 * @param v the memory to switch from one topo to another. It has to be large enough to contain both local data's
 * @param sign if the switch is forward (FLUPS_FORWARD) or backward (FLUPS_BACKWARD) w.r.t. the order defined at init.
 * 
 * -----------------------------------------------
 * We do the following:
 */
void SwitchTopo_a2a::execute(opt_double_ptr v, const int sign) {
    BEGIN_FUNC;

    FLUPS_CHECK(_topo_in->isComplex() == _topo_out->isComplex(),"both topologies have to be complex or real", LOCATION);
    FLUPS_CHECK(_topo_in->nf() <= 2, "the value of nf is not supported", LOCATION);

    int comm_size;
    // MPI_Comm_rank(_subcomm, &rank);
    MPI_Comm_size(_subcomm, &comm_size);

    if (_prof != NULL) _prof->start("reorder");

    //-------------------------------------------------------------------------
    /** - setup required memory arrays */
    //-------------------------------------------------------------------------

    const Topology* topo_in;
    const Topology* topo_out;

    MPI_Request* sendRequest;
    MPI_Request* recvRequest;

    int send_nBlock[3];
    int recv_nBlock[3];

    int istart[3];
    int ostart[3];
    int iend[3];
    int oend[3];
    int inloc[3];
    int onloc[3];

    int* iBlockSize[3];
    int* oBlockSize[3];
    
    int* oselfBlockID;
    int* destTag;

    int* send_count;
    int* recv_count;
    int* send_start;
    int* recv_start;

    const int nByBlock[3] = {_nByBlock[0],_nByBlock[1],_nByBlock[2]};

    opt_double_ptr* sendBuf;
    opt_double_ptr* recvBuf;

    if (sign == FLUPS_FORWARD) {
        topo_in     = _topo_in;
        topo_out    = _topo_out;
        sendBuf     = _sendBuf;
        recvBuf     = _recvBuf;

        send_count = _i2o_count;
        recv_count = _o2i_count;
        send_start = _i2o_start;
        recv_start = _o2i_start;

        for (int id = 0; id < 3; id++) {
            send_nBlock[id] = _inBlock[id];
            recv_nBlock[id] = _onBlock[id];
            istart[id]      = _istart[id];
            iend[id]        = _iend[id];
            ostart[id]      = _ostart[id];
            oend[id]        = _oend[id];
            // inloc[id]       = _topo_in->nloc(id);
            // onloc[id]       = _topo_out->nloc(id);
            iBlockSize[id]  = _iBlockSize[id];
            oBlockSize[id]  = _oBlockSize[id];
        }
    } else if (sign == FLUPS_BACKWARD) {
        topo_in     = _topo_out;
        topo_out    = _topo_in;
        sendBuf     = _recvBuf;
        recvBuf     = _sendBuf;

        send_count = _o2i_count;
        recv_count = _i2o_count;
        send_start = _o2i_start;
        recv_start = _i2o_start;

        for (int id = 0; id < 3; id++) {
            send_nBlock[id] = _onBlock[id];
            recv_nBlock[id] = _inBlock[id];
            istart[id]      = _ostart[id];
            iend[id]        = _oend[id];
            ostart[id]      = _istart[id];
            oend[id]        = _iend[id];
            // inloc[id]       = _topo_out->nloc(id);
            // onloc[id]       = _topo_in->nloc(id);
            iBlockSize[id]  = _oBlockSize[id];
            oBlockSize[id]  = _iBlockSize[id];
        }
    } else {
        FLUPS_CHECK(false, "the sign is not FLUPS_FORWARD nor FLUPS_BACKWARD", LOCATION);
    }

    FLUPS_INFO("previous topo: %d,%d,%d axis=%d", topo_in->nglob(0), topo_in->nglob(1), topo_in->nglob(2), topo_in->axis());
    FLUPS_INFO("new topo: %d,%d,%d  axis=%d", topo_out->nglob(0), topo_out->nglob(1), topo_out->nglob(2), topo_out->axis());
    FLUPS_INFO("using %d blocks on send and %d on recv",send_nBlock[0]*send_nBlock[1]*send_nBlock[2],recv_nBlock[0]*recv_nBlock[1]*recv_nBlock[2]);

    // define important constants
    const int ax0 = topo_in->axis();
    const int ax1 = (ax0 + 1) % 3;
    const int ax2 = (ax0 + 2) % 3;
    const int nf  = topo_in->nf();

    if (_prof != NULL) {
        _prof->start("mem2buf");
    }
    //-------------------------------------------------------------------------
    /** - fill the buffers */
    //-------------------------------------------------------------------------
    const int nblocks_send = send_nBlock[0] * send_nBlock[1] * send_nBlock[2];

#pragma omp parallel proc_bind(close) default(none) firstprivate(nblocks_send, send_nBlock, v, sendBuf, istart, nByBlock, iBlockSize, nf, inloc, ax0, ax1, ax2)
    for (int bid = 0; bid < nblocks_send; bid++) {
        // get the split index
        int ib[3];
        localSplit(bid, send_nBlock, 0, ib, 1);

        // get the starting index in the global memory using !!nByBlock!!
        // since only the last block may have a different size
        const int loci0 = istart[ax0] + ib[ax0] * nByBlock[ax0];
        const int loci1 = istart[ax1] + ib[ax1] * nByBlock[ax1];
        const int loci2 = istart[ax2] + ib[ax2] * nByBlock[ax2];
        // get the memory to write to/from
        double* __restrict data = sendBuf[bid];
        double* __restrict my_v = v + localIndex(ax0, loci0, loci1, loci2, ax0, inloc, nf);

        // go inside the block
        const int id_max = iBlockSize[ax1][bid] * iBlockSize[ax2][bid];
#pragma omp for schedule(static)
        for (int id = 0; id < id_max; id++) {
            // get the id from a small modulo
            const int i2 = id / iBlockSize[ax1][bid];
            const int i1 = id % iBlockSize[ax1][bid];
            // get the starting global id for the buffer and the field
            const size_t buf_idx = id * iBlockSize[ax0][bid] * nf;
            const size_t my_idx  = localIndex(ax0, 0, i1, i2, ax0, inloc, nf);
            // get the max counter
            const size_t nmax = iBlockSize[ax0][bid] * nf;
            // do the copy -> vectorized
            for (size_t i0 = 0; i0 < nmax; i0++) {
                data[buf_idx + i0] = my_v[my_idx + i0];
            }
        }
    }
    if (_prof != NULL) {
        _prof->stop("mem2buf");
    }

    //-------------------------------------------------------------------------
    /** - Do the communication */
    //-------------------------------------------------------------------------
    if (_prof != NULL) {
        _prof->start("waiting");
    }
    if (_is_all2all) {
        MPI_Alltoall(sendBuf[0], send_count[0], MPI_DOUBLE, recvBuf[0], recv_count[0], MPI_DOUBLE, _subcomm);

    } else {
        // the count is the count of the first one
        MPI_Alltoallv(sendBuf[0], send_count, send_start, MPI_DOUBLE, recvBuf[0], recv_count, recv_start, MPI_DOUBLE, _subcomm);
    }
    if (_prof != NULL) {
        _prof->stop("waiting");
        // we just have switch the whole exchanged memory
        // size_t loc_mem = 0.0;
        // _prof->addMem("waiting", loc_mem);
    }

    //-------------------------------------------------------------------------
    /** - reset the memory to 0 */
    //-------------------------------------------------------------------------
    // reset the memory to 0
    std::memset(v, 0, sizeof(double) * topo_out->locmemsize());

    //-------------------------------------------------------------------------
    /** - wait for a block and copy when it arrives */
    //-------------------------------------------------------------------------
    // get some counters
    const int nblocks_recv  = recv_nBlock[0] * recv_nBlock[1] * recv_nBlock[2];
    const int out_axis = topo_out->axis();
    // for each block
    if (_prof != NULL) {
        _prof->start("buf2mem");
    }

#pragma omp parallel default(none) proc_bind(close) firstprivate(nblocks_recv, recv_nBlock, v, recvBuf, ostart, nByBlock, oBlockSize, nf, onloc, ax0, ax1, ax2)
    for (int bid = 0; bid < nblocks_recv; bid++) {
        // get the indexing of the block in 012-indexing
        int ibv[3];
        localSplit(bid, recv_nBlock, 0, ibv, 1);

        // get the starting index in the global memory using !!nByBlock!!
        // since only the last block may have a different size
        const int loci0 = ostart[ax0] + ibv[ax0] * nByBlock[ax0];
        const int loci1 = ostart[ax1] + ibv[ax1] * nByBlock[ax1];
        const int loci2 = ostart[ax2] + ibv[ax2] * nByBlock[ax2];
        // get the memory
        double* __restrict data = recvBuf[bid];
        double* __restrict my_v = v + localIndex(ax0, loci0, loci1, loci2, out_axis, onloc, nf);
        // get the stride
        const size_t stride = localIndex(ax0, 1, 0, 0, out_axis, onloc, nf);
        // get the max number of ids not aligned in ax0
        const size_t id_max = oBlockSize[ax1][bid] * oBlockSize[ax2][bid];

        if (nf == 1) {
#pragma omp for schedule(static)
            for (size_t id = 0; id < id_max; id++) {
                // get the id from a small modulo
                const int i2 = id / oBlockSize[ax1][bid];
                const int i1 = id % oBlockSize[ax1][bid];
                // get the starting global id for the buffer and the field
                const size_t buf_idx = id * oBlockSize[ax0][bid] * nf;
                const size_t my_idx  = localIndex(ax0, 0, i1, i2, out_axis, onloc, nf);
                // do the copy
                for (int i0 = 0; i0 < oBlockSize[ax0][bid]; i0++) {
                    my_v[my_idx + i0 * stride] = data[buf_idx + i0];
                }
            }
        } else if (nf == 2) {
#pragma omp for schedule(static)
            for (size_t id = 0; id < id_max; id++) {
                // get the id from a small modulo
                const int i2 = id / oBlockSize[ax1][bid];
                const int i1 = id % oBlockSize[ax1][bid];
                // get the starting global id for the buffer and the field
                const size_t buf_idx = id * oBlockSize[ax0][bid] * nf;
                const size_t my_idx  = localIndex(ax0, 0, i1, i2, out_axis, onloc, nf);
                // do the copy
                for (int i0 = 0; i0 < oBlockSize[ax0][bid]; i0++) {
                    my_v[my_idx + i0 * stride + 0] = data[buf_idx + i0 * 2 + 0];
                    my_v[my_idx + i0 * stride + 1] = data[buf_idx + i0 * 2 + 1];
                }
            }
        }
    }

    if (_prof != NULL) {
        _prof->stop("buf2mem");
        _prof->stop("reorder");
    }
}

void SwitchTopo_a2a::disp() {
    BEGIN_FUNC;
    FLUPS_INFO("------------------------------------------");
    if(_is_all2all) FLUPS_INFO("## Topo Swticher All to All !! MPI");
    if(!_is_all2all) FLUPS_INFO("## Topo Swticher All to All vector MPI");
    FLUPS_INFO("--- INPUT");
    FLUPS_INFO("  - input axis = %d", _topo_in->axis());
    FLUPS_INFO("  - input local = %d %d %d", _topo_in->nloc(0), _topo_in->nloc(1), _topo_in->nloc(2));
    FLUPS_INFO("  - input global = %d %d %d", _topo_in->nglob(0), _topo_in->nglob(1), _topo_in->nglob(2));
    FLUPS_INFO("  - istart = %d %d %d", _istart[0], _istart[1], _istart[2]);
    FLUPS_INFO("  - iend = %d %d %d", _iend[0], _iend[1], _iend[2]);
    FLUPS_INFO("--- OUTPUT");
    FLUPS_INFO("  - output axis = %d", _topo_out->axis());
    FLUPS_INFO("  - output local = %d %d %d", _topo_out->nloc(0), _topo_out->nloc(1), _topo_out->nloc(2));
    FLUPS_INFO("  - output global = %d %d %d", _topo_out->nglob(0), _topo_out->nglob(1), _topo_out->nglob(2));
    FLUPS_INFO("  - ostart = %d %d %d", _ostart[0], _ostart[1], _ostart[2]);
    FLUPS_INFO("  - oend = %d %d %d", _oend[0], _oend[1], _oend[2]);
    FLUPS_INFO("--- BLOCKS");
    FLUPS_INFO("  - nByBlock  = %d %d %d", _nByBlock[0], _nByBlock[1], _nByBlock[2]);
    FLUPS_INFO("  - inBlock = %d %d %d", _inBlock[0],_inBlock[1],_inBlock[2]);
    FLUPS_INFO("  - onBlock = %d %d %d", _onBlock[0],_onBlock[1],_onBlock[2]);
    FLUPS_INFO("------------------------------------------");
}

// void SwitchTopo::disp_rankgraph(const int id_in,const int id_out) const{
//     int rank;
//     MPI_Comm_rank(MPI_COMM_WORLD,&rank);
//     string name = "./prof/SwitchTopo" + std::to_string(id_in) + "with" + std::to_string(id_out) + "_rank" + std::to_string(rank) + ".txt";
//     FILE* file = fopen(name.c_str(),"w+");
//     if(file != NULL){
//         fprintf(file,"%d SEND:",rank);
//         for(int ib=0; ib<_inBlock[0] * _inBlock[1] * _inBlock[2]; ib++){
//             fprintf(file," %d ",_i2o_destRank[ib]);
//         }
//         fprintf(file,"\n");
//         fprintf(file,"%d RECV:",rank);
//         for(int ib=0; ib<_onBlock[0] * _onBlock[1] * _onBlock[2]; ib++){
//             fprintf(file," %d ",_o2i_destRank[ib]);
//         }
//         fprintf(file,"\n");
//         fclose(file);
//     }
// }

// void SwitchTopo_test() {
//     BEGIN_FUNC;

//     int comm_size;
//     MPI_Comm_size(MPI_COMM_WORLD, &comm_size);

//     const int nglob[3] = {8, 8, 8};
//     const int nproc[3] = {2, 2, 1};

//     const int nglob_big[3] = {17, 8, 8};
//     const int nproc_big[3] = {2, 2, 1};

//     //===========================================================================
//     // real numbers
//     Topology* topo    = new Topology(0, nglob, nproc, false,NULL);
//     Topology* topobig = new Topology(0, nglob_big, nproc_big, false,NULL);

//     double* data = (double*)fftw_malloc(sizeof(double*) * std::max(topo->locmemsize(), topobig->locmemsize()));

//     for (int i2 = 0; i2 < topo->nloc(2); i2++) {
//         for (int i1 = 0; i1 < topo->nloc(1); i1++) {
//             for (int i0 = 0; i0 < topo->nloc(0); i0++) {
//                 size_t id    = localindex_xyz(i0, i1, i2, topo);
//                 data[id + 0] = id;
//             }
//         }
//     }
//     // try the dump
//     hdf5_dump(topo, "test_real", data);

//     const int fieldstart[3] = {0, 0, 0};
//     // printf("\n=============================");
//     SwitchTopo* switchtopo = new SwitchTopo(topo, topobig, fieldstart, NULL);

//     // printf("\n\n============ FORWARD =================");
//     switchtopo->execute(data, FLUPS_FORWARD);

//     hdf5_dump(topobig, "test_real_padd", data);

//     // printf("\n\n============ BACKWARD =================");
//     switchtopo->execute(data, FLUPS_BACKWARD);

//     hdf5_dump(topo, "test_real_returned", data);

//     fftw_free(data);
//     delete (switchtopo);
//     delete (topo);
//     delete (topobig);

//     //===========================================================================
//     // complex numbers
//     topo    = new Topology(0, nglob, nproc, true,NULL);
//     topobig = new Topology(2, nglob_big, nproc_big, true,NULL);

//     data = (double*)fftw_malloc(sizeof(double*) * topobig->locmemsize());

//     for (int i2 = 0; i2 < topo->nloc(2); i2++) {
//         for (int i1 = 0; i1 < topo->nloc(1); i1++) {
//             for (int i0 = 0; i0 < topo->nloc(0); i0++) {
//                 size_t id    = localindex_xyz(i0, i1, i2, topo);
//                 data[id + 0] = 0;
//                 data[id + 1] = id;
//             }
//         }
//     }
//     // try the dump
//     hdf5_dump(topo, "test_complex", data);

//     // topobig->switch2complex();
//     // printf("as complex: nloc topobig = %d %d %d\n",topobig->nloc(0),topobig->nloc(1),topobig->nloc(2));
//     // topobig->switch2real();
//     // printf("as real: nloc topobig = %d %d %d\n",topobig->nloc(0),topobig->nloc(1),topobig->nloc(2));

//     const int fieldstart2[3] = {4, 0, 0};
//     // printf("\n=============================");
//     switchtopo = new SwitchTopo(topo, topobig, fieldstart2, NULL);

//     switchtopo->execute(data, FLUPS_FORWARD);

//     hdf5_dump(topobig, "test_complex_padd", data);

//     switchtopo->execute(data, FLUPS_BACKWARD);

//     hdf5_dump(topo, "test_complex_returned", data);

//     fftw_free(data);
//     delete (switchtopo);
//     delete (topo);
//     delete (topobig);
// }