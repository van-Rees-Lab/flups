/**
 * @file SwitchTopo.cpp
 * @author Thomas Gillis
 * @brief
 * @version
 * @date 2019-07-25
 *
 * @copyright Copyright © UCLouvain 2019
 *
 */

#include "SwitchTopo.hpp"

SwitchTopo::SwitchTopo(const Topology* topo_input, const Topology* topo_output,
                       const int shift[3]) {
    BEGIN_FUNC

    // UP_CHECK0(topo_input->globmemsize() == topo_output->globmemsize(),"global
    // memory size have to match between topos");
    UP_CHECK0(topo_input->isComplex() == topo_output->isComplex(),
              "both topologies have to be the same kind");

    int rank, comm_size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &comm_size);

    _topo_in  = topo_input;
    _topo_out = topo_output;

    //-------------------------------------------------------------------------
    /** - allocate the size arrays */
    //-------------------------------------------------------------------------
    _nsend = (int*)fftw_malloc(comm_size * sizeof(int));
    _nrecv = (int*)fftw_malloc(comm_size * sizeof(int));
    _ssend = (int*)fftw_malloc(comm_size * sizeof(int));
    _srecv = (int*)fftw_malloc(comm_size * sizeof(int));
    _count = (int*)fftw_malloc(comm_size * sizeof(int));
    // n axis arrays
    _naxis_i2o_send = (int*)fftw_malloc(comm_size * sizeof(int));
    _naxis_o2i_send = (int*)fftw_malloc(comm_size * sizeof(int));
    _naxis_i2o_recv = (int*)fftw_malloc(comm_size * sizeof(int));
    _naxis_o2i_recv = (int*)fftw_malloc(comm_size * sizeof(int));

    UP_CHECK1(UP_ISALIGNED(_nsend), "FFTW alignement not compatible with UP_ALIGNMENT (=%d)", UP_ALIGNMENT);
    UP_CHECK1(UP_ISALIGNED(_nrecv), "FFTW alignement not compatible with UP_ALIGNMENT (=%d)", UP_ALIGNMENT);
    UP_CHECK1(UP_ISALIGNED(_ssend), "FFTW alignement not compatible with UP_ALIGNMENT (=%d)", UP_ALIGNMENT);
    UP_CHECK1(UP_ISALIGNED(_srecv), "FFTW alignement not compatible with UP_ALIGNMENT (=%d)", UP_ALIGNMENT);
    UP_CHECK1(UP_ISALIGNED(_count), "FFTW alignement not compatible with UP_ALIGNMENT (=%d)", UP_ALIGNMENT);
    UP_CHECK1(UP_ISALIGNED(_naxis_i2o_send), "FFTW alignement not compatible with UP_ALIGNMENT (=%d)", UP_ALIGNMENT);
    UP_CHECK1(UP_ISALIGNED(_naxis_o2i_send), "FFTW alignement not compatible with UP_ALIGNMENT (=%d)", UP_ALIGNMENT);
    UP_CHECK1(UP_ISALIGNED(_naxis_i2o_recv), "FFTW alignement not compatible with UP_ALIGNMENT (=%d)", UP_ALIGNMENT);
    UP_CHECK1(UP_ISALIGNED(_naxis_o2i_recv), "FFTW alignement not compatible with UP_ALIGNMENT (=%d)", UP_ALIGNMENT);

    std::memset(_nsend, 0, sizeof(int) * comm_size);
    std::memset(_nrecv, 0, sizeof(int) * comm_size);
    std::memset(_ssend, 0, sizeof(int) * comm_size);
    std::memset(_srecv, 0, sizeof(int) * comm_size);
    std::memset(_count, 0, sizeof(int) * comm_size);

    //-------------------------------------------------------------------------
    /** - Compute the axis for loop go through */
    //-------------------------------------------------------------------------
    const int ax0 = _topo_in->axis();
    const int ax1 = (ax0 + 1) % 3;
    const int ax2 = (ax0 + 2) % 3;

    //-------------------------------------------------------------------------
    /** - for each dimension, get the shared zone */
    //-------------------------------------------------------------------------
    for (int id = 0; id < 3; id++) {
        _ishift[id] = shift[id];
        _oshift[id] = -shift[id];
    }
    _topo_in->cmpt_intersect_id(_ishift, _topo_out, _istart, _iend);
    _topo_out->cmpt_intersect_id(_oshift, _topo_in, _ostart, _oend);

    //-------------------------------------------------------------------------
    /** - for each proc get its naxis */
    //-------------------------------------------------------------------------
    // get the global id of start for the current proc
    _topo_in->cmpt_intersect_naxis(_topo_out, _istart, _iend, _naxis_i2o_send);
    _topo_out->cmpt_intersect_naxis(_topo_in, _ostart, _oend, _naxis_o2i_send);

    //-------------------------------------------------------------------------
    /** - for each data get its destination rank */
    //-------------------------------------------------------------------------
    int dest_rankd[3];
    for (int i2 = _istart[ax2]; i2 < _iend[ax2]; ++i2) {
        dest_rankd[ax2] = _topo_in->cmpt_matchrank(ax2, _topo_out, i2 + _ishift[ax2]);
        for (int i1 = _istart[ax1]; i1 < _iend[ax1]; ++i1) {
            dest_rankd[ax1] = _topo_in->cmpt_matchrank(ax1, _topo_out, i1 + _ishift[ax1]);
            for (int i0 = _istart[ax0]; i0 < _iend[ax0]; ++i0) {
                dest_rankd[ax0] = _topo_in->cmpt_matchrank(ax0, _topo_out, i0 + _ishift[ax0]);
                // add one info to the destination index
                _nsend[rankindex(dest_rankd, _topo_out)] += _topo_in->nf();
            }
        }
    }

    //-------------------------------------------------------------------------
    /** - send the size to each proc and store it */
    //-------------------------------------------------------------------------
    MPI_Alltoall(_nsend, 1, MPI_INT, _nrecv, 1, MPI_INT, MPI_COMM_WORLD);
    for (int i = 1; i < comm_size; ++i) {
        _ssend[i] = _ssend[i - 1] + _nsend[i - 1];
        _srecv[i] = _srecv[i - 1] + _nrecv[i - 1];
    }
    MPI_Alltoall(_naxis_i2o_send, 1, MPI_INT, _naxis_i2o_recv, 1, MPI_INT, MPI_COMM_WORLD);
    MPI_Alltoall(_naxis_o2i_send, 1, MPI_INT, _naxis_o2i_recv, 1, MPI_INT, MPI_COMM_WORLD);

    //-------------------------------------------------------------------------
    /** - allocate the buffer if needed */
    //-------------------------------------------------------------------------
    _bufsend = (double*)fftw_malloc(sizeof(double) * _topo_in->locmemsize());
    _bufrecv = (double*)fftw_malloc(sizeof(double) * _topo_out->locmemsize());

    UP_CHECK1(UP_ISALIGNED(_bufsend),
              "FFTW alignement not compatible with UP_ALIGNMENT (=%d)",
              UP_ALIGNMENT);
    UP_CHECK1(UP_ISALIGNED(_bufrecv),
              "FFTW alignement not compatible with UP_ALIGNMENT (=%d)",
              UP_ALIGNMENT);
}

SwitchTopo::~SwitchTopo() {
    if (_nsend != NULL) fftw_free(_nsend);
    if (_nrecv != NULL) fftw_free(_nrecv);
    if (_ssend != NULL) fftw_free(_ssend);
    if (_srecv != NULL) fftw_free(_srecv);
    if (_srecv != NULL) fftw_free(_count);
    if (_bufsend != NULL) fftw_free(_bufsend);
    if (_bufrecv != NULL) fftw_free(_bufrecv);
    if (_naxis_i2o_send != NULL) fftw_free(_naxis_i2o_send);
    if (_naxis_o2i_send != NULL) fftw_free(_naxis_o2i_send);
    if (_naxis_i2o_recv != NULL) fftw_free(_naxis_i2o_recv);
    if (_naxis_o2i_recv != NULL) fftw_free(_naxis_o2i_recv);
}

void SwitchTopo::execute(opt_double_ptr v, const int sign) {
    BEGIN_FUNC

    UP_CHECK0(_topo_in->isComplex() == _topo_out->isComplex(),
              "both topologies have to be complex or real");

    int rank, comm_size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &comm_size);

    opt_int_ptr count = _count;

    //-------------------------------------------------------------------------
    /** - set the size counters*/
    //-------------------------------------------------------------------------
    int istart[3];
    int ostart[3];
    int iend[3];
    int oend[3];
    int ishift[3];
    int oshift[3];

    const Topology* topo_in;
    const Topology* topo_out;

    opt_int_ptr    ssend;
    opt_int_ptr    srecv;
    opt_int_ptr    nsend;
    opt_int_ptr    nrecv;
    opt_double_ptr recvbuf;
    opt_double_ptr sendbuf;

    opt_int_ptr naxis_send;
    opt_int_ptr naxis_recv;

    if (sign == UP_FORWARD) {
        topo_in  = _topo_in;
        topo_out = _topo_out;

        for (int id = 0; id < 3; id++) {
            // input parameters
            istart[id] = _istart[id];
            iend[id]   = _iend[id];
            ishift[id] = _ishift[id];
            // output params
            ostart[id] = _ostart[id];
            oend[id]   = _oend[id];
            oshift[id] = _oshift[id];
        }
        ssend      = _ssend;
        srecv      = _srecv;
        nsend      = _nsend;
        nrecv      = _nrecv;
        sendbuf    = _bufsend;
        recvbuf    = _bufrecv;
        naxis_send = _naxis_i2o_send;
        naxis_recv = _naxis_i2o_recv;

    } else if (sign == UP_BACKWARD) {
        topo_in  = _topo_out;
        topo_out = _topo_in;

        for (int id = 0; id < 3; id++) {
            // input parameters
            istart[id] = _ostart[id];
            iend[id]   = _oend[id];
            ishift[id] = _oshift[id];
            // output params
            ostart[id] = _istart[id];
            oend[id]   = _iend[id];
            oshift[id] = _ishift[id];
        }
        ssend      = _srecv;
        srecv      = _ssend;
        nsend      = _nrecv;
        nrecv      = _nsend;
        sendbuf    = _bufrecv;
        recvbuf    = _bufsend;
        naxis_send = _naxis_o2i_send;
        naxis_recv = _naxis_o2i_recv;
    } else {
        UP_CHECK0(false, "the sign is not UP_FORWARD nor UP_BACKWARD");
    }

    INFOLOG5("previous topo: %d,%d,%d axis=%d\n", topo_in->nglob(0), topo_in->nglob(1), topo_in->nglob(2), topo_in->axis());
    INFOLOG5("new topo: %d,%d,%d  axis=%d\n", topo_out->nglob(0), topo_out->nglob(1), topo_out->nglob(2), topo_out->axis());

    const int ax0 = topo_in->axis();
    const int ax1 = (ax0 + 1) % 3;
    const int ax2 = (ax0 + 2) % 3;

    //-------------------------------------------------------------------------
    /** - fill the buffers */
    //-------------------------------------------------------------------------
    std::memset(count, 0, sizeof(int) * comm_size);

    int dest_rankd[3];

    if (topo_in->nf() == 1) {
        for (int i2 = istart[ax2]; i2 < iend[ax2]; ++i2) {
            dest_rankd[ax2] = topo_in->cmpt_matchrank(ax2, topo_out, i2 + ishift[ax2]);
            for (int i1 = istart[ax1]; i1 < iend[ax1]; ++i1) {
                dest_rankd[ax1] = topo_in->cmpt_matchrank(ax1, topo_out, i1 + ishift[ax1]);
                for (int i0 = istart[ax0]; i0 < iend[ax0]; ++i0) {
                    dest_rankd[ax0] = topo_in->cmpt_matchrank(ax0, topo_out, i0 + ishift[ax0]);
                    // add one info to the destination index
                    const int    dest_rank = rankindex(dest_rankd, topo_out);
                    const int    buf_idx   = ssend[dest_rank] + count[dest_rank];
                    const size_t my_idx    = localindex_ao(i0, i1, i2, topo_in);

                    for (int ib = 0; ib < naxis_send[dest_rank]; ib++) {
                        sendbuf[buf_idx + ib] = v[my_idx + ib];
                    }
                    count[dest_rank] += naxis_send[dest_rank];
                    i0 = i0 + naxis_send[dest_rank] - 1;
                }
            }
        }
    } else if (topo_in->nf() == 2) {
        for (int i2 = istart[ax2]; i2 < iend[ax2]; ++i2) {
            dest_rankd[ax2] = topo_in->cmpt_matchrank(ax2, topo_out, i2 + ishift[ax2]);
            for (int i1 = istart[ax1]; i1 < iend[ax1]; ++i1) {
                dest_rankd[ax1] = topo_in->cmpt_matchrank(ax1, topo_out, i1 + ishift[ax1]);
                for (int i0 = istart[ax0]; i0 < iend[ax0]; ++i0) {
                    dest_rankd[ax0] = topo_in->cmpt_matchrank(ax0, topo_out, i0 + ishift[ax0]);

                    // add one info to the destination index
                    const int    dest_rank = rankindex(dest_rankd, topo_out);
                    const int    buf_idx   = ssend[dest_rank] + count[dest_rank];
                    const size_t my_idx    = localindex_ao(i0, i1, i2, topo_in);

                    for (int ib = 0; ib < naxis_send[dest_rank] * 2; ib++) {
                        sendbuf[buf_idx + ib] = v[my_idx + ib];
                    }
                    count[dest_rank] += naxis_send[dest_rank] * 2;
                    i0 = i0 + naxis_send[dest_rank] - 1;
                }
            }
        }
    } else {
        UP_CHECK0(false, "size of Topological nf not supported");
    }

    //-------------------------------------------------------------------------
    /** - Send it */
    //-------------------------------------------------------------------------
    MPI_Alltoallv(sendbuf, nsend, ssend, MPI_DOUBLE, recvbuf, nrecv, srecv, MPI_DOUBLE, MPI_COMM_WORLD);
    //-------------------------------------------------------------------------
    /** - Fill the memory */
    //-------------------------------------------------------------------------
    std::memset(count, 0, sizeof(int) * comm_size);              // reset counters
    std::memset(v, 0, sizeof(double) * topo_out->locmemsize());  // reset datas

    int orig_rankd[3];
    if (topo_out->nf() == 1) {
        for (int i2 = ostart[ax2]; i2 < oend[ax2]; ++i2) {
            orig_rankd[ax2] = topo_out->cmpt_matchrank(ax2, topo_in, i2 + oshift[ax2]);
            for (int i1 = ostart[ax1]; i1 < oend[ax1]; ++i1) {
                orig_rankd[ax1] = topo_out->cmpt_matchrank(ax1, topo_in, i1 + oshift[ax1]);
                for (int i0 = ostart[ax0]; i0 < oend[ax0]; ++i0) {
                    orig_rankd[ax0] = topo_out->cmpt_matchrank(ax0, topo_in, i0 + oshift[ax0]);

                    const int    orig_rank = rankindex(orig_rankd, topo_in);
                    const size_t buf_idx   = srecv[orig_rank] + count[orig_rank];
                    // get the starting index and the stride obtained when adding one point in i0
                    const size_t my_idx = localindex(topo_in->axis(), i0, i1, i2, topo_out);
                    const size_t stride = localindex(topo_in->axis(), 1, 0, 0, topo_out);

                    for (int ib = 0; ib < naxis_recv[orig_rank]; ib++) {
                        v[my_idx + ib * stride] = recvbuf[buf_idx + ib];
                    }
                    count[orig_rank] += naxis_recv[orig_rank];

                    i0 = i0 + naxis_recv[orig_rank] - 1;
                }
            }
        }
    } else if (topo_out->nf() == 2) {
        for (int i2 = ostart[ax2]; i2 < oend[ax2]; ++i2) {
            orig_rankd[ax2] = topo_out->cmpt_matchrank(ax2, topo_in, i2 + oshift[ax2]);
            for (int i1 = ostart[ax1]; i1 < oend[ax1]; ++i1) {
                orig_rankd[ax1] = topo_out->cmpt_matchrank(ax1, topo_in, i1 + oshift[ax1]);
                for (int i0 = ostart[ax0]; i0 < oend[ax0]; ++i0) {
                    orig_rankd[ax0] = topo_out->cmpt_matchrank(ax0, topo_in, i0 + oshift[ax0]);

                    // get the rank of origin
                    const int orig_rank = rankindex(orig_rankd, topo_in);
                    // get the starting buffer index
                    const size_t buf_idx = srecv[orig_rank] + count[orig_rank];
                    // get the starting index and the stride obtained when adding one point in i0
                    const size_t my_idx = localindex(topo_in->axis(), i0, i1, i2, topo_out);
                    const size_t stride = localindex(topo_in->axis(), 1, 0, 0, topo_out);

                    // for every element
                    for (int ib = 0; ib < naxis_recv[orig_rank]; ib++) {
                        v[my_idx + ib * stride + 0] = recvbuf[buf_idx + ib * 2 + 0];
                        v[my_idx + ib * stride + 1] = recvbuf[buf_idx + ib * 2 + 1];
                    }
                    count[orig_rank] += naxis_recv[orig_rank] * 2;
                    i0 = i0 + naxis_recv[orig_rank] - 1;
                }
            }
        }
    } else {
        UP_CHECK0(false, "size of Topological nf not supported");
    }
}

void SwitchTopo::disp() {
    BEGIN_FUNC
    INFO("------------------------------------------\n");
    INFO("## Topo Swticher MPI\n");
    INFO("--- INPUT\n");
    INFO2("  - input axis = %d\n", _topo_in->axis());
    INFO4("  - input local = %d %d %d\n", _topo_in->nloc(0), _topo_in->nloc(1), _topo_in->nloc(2));
    INFO4("  - input global = %d %d %d\n", _topo_in->nglob(0), _topo_in->nglob(1), _topo_in->nglob(2));
    INFO4("  - ishift = %d %d %d\n", _ishift[0], _ishift[1], _ishift[2]);
    INFO4("  - istart = %d %d %d\n", _istart[0], _istart[1], _istart[2]);
    INFO4("  - iend = %d %d %d\n", _iend[0], _iend[1], _iend[2]);
    INFO("--- OUTPUT\n");
    INFO2("  - output axis = %d\n", _topo_out->axis());
    INFO4("  - output local = %d %d %d\n", _topo_out->nloc(0), _topo_out->nloc(1), _topo_out->nloc(2));
    INFO4("  - output global = %d %d %d\n", _topo_out->nglob(0), _topo_out->nglob(1), _topo_out->nglob(2));
    INFO4("  - oshift = %d %d %d\n", _oshift[0], _oshift[1], _oshift[2]);
    INFO4("  - ostart = %d %d %d\n", _ostart[0], _ostart[1], _ostart[2]);
    INFO4("  - oend = %d %d %d\n", _oend[0], _oend[1], _oend[2]);
    INFO("------------------------------------------\n");
}

void SwitchTopo::test() {
    BEGIN_FUNC

    int comm_size;
    MPI_Comm_size(MPI_COMM_WORLD, &comm_size);

    const int nglob[3] = {8, 8, 8};
    // const int nproc[3] = {comm_size, 1, 1};
    const int nproc[3] = {4, 1, 1};

    const int nglob_big[3] = {18, 8, 8};
    const int nproc_big[3] = {1, 2, 2};

    //===========================================================================
    // real numbers
    Topology* topo    = new Topology(0, nglob, nproc, false);
    Topology* topobig = new Topology(0, nglob_big, nproc_big, false);

    double* data = (double*)fftw_malloc(sizeof(double*) * topobig->locmemsize());

    for (int i2 = 0; i2 < topo->nloc(2); i2++) {
        for (int i1 = 0; i1 < topo->nloc(1); i1++) {
            for (int i0 = 0; i0 < topo->nloc(0); i0++) {
                size_t id    = localindex_xyz(i0, i1, i2, topo);
                data[id + 0] = id;
            }
        }
    }
    // try the dump
    hdf5_dump(topo, "test_real", data);

    topobig->switch2complex();
    topobig->switch2real();

    const int   fieldstart[3] = {0, 0, 0};
    SwitchTopo* switchtopo    = new SwitchTopo(topo, topobig, fieldstart);

    switchtopo->execute(data, UP_FORWARD);

    hdf5_dump(topobig, "test_real_padd", data);

    fftw_free(data);
    delete (topo);
    delete (topobig);
}