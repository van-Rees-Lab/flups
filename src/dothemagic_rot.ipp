/**
 * @file dothemagic_rot.ipp
 * @author Thomas Gillis and Denis-Gabriel Caprace
 * @brief contains the domagic functions 
 * @version
 * 
 * @copyright Copyright © UCLouvain 2020
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

#if (KIND == 01)
/**
 * @brief perform the convolution for real to real cases - spectral diff
 * 
 */
void Solver::dothemagic_rot_real_o1(double *data,const double koffset[3],const double kfact[3][3][2], const double symstart[3]){
#elif (KIND == 11)
/**
 * @brief perform the convolution for complex to complex cases - spectral diff
 * 
 */
void Solver::dothemagic_rot_complex_o1(double *data,const double koffset[3],const double kfact[3][3][2], const double symstart[3]){
#elif (KIND == 02)
/**
 * @brief perform the convolution for real to real cases - spectral diff
 * 
 */
void Solver::dothemagic_rot_real_o2(double *data,const double koffset[3],const double kfact[3][3][2], const double symstart[3],const double hgrid[3]){
#elif (KIND == 12)
/**
 * @brief perform the convolution for complex to complex cases - spectral diff
 * 
 */
void Solver::dothemagic_rot_complex_o2(double *data,const double koffset[3],const double kfact[3][3][2], const double symstart[3],const double hgrid[3]){
#endif

    BEGIN_FUNC;
    int cdim = _ndim - 1;  // get current dim
#if (KIND == 01 || KIND == 02)
    FLUPS_CHECK(_topo_hat[cdim]->nf() == 1, "The topo_hat[2] (field) has to be complex", LOCATION);
#else
    FLUPS_CHECK(_topo_hat[cdim]->nf() == 2, "The topo_hat[2] (field) has to be complex", LOCATION);
#endif
    // get the axis
    const int nf  = _topo_hat[cdim]->nf();
    const int ax0 = _topo_hat[cdim]->axis();
    const int ax1 = (ax0 + 1) % 3;
    const int ax2 = (ax0 + 2) % 3;
    
    // get the norm factor
    const double         normfact = _normfact;

    // get the starting indexes of the current block
    int istart[3];
    _topo_hat[cdim]->get_istart_glob(istart);

    // get the adresses
    opt_double_ptr       mydata   = data;
    const opt_double_ptr mygreen  = _green;

    // get the number of pencils for the field and green
    const size_t ondim = _topo_hat[cdim]->nloc(ax1) * _topo_hat[cdim]->nloc(ax2);
    const size_t inmax = _topo_hat[cdim]->nloc(ax0);
    // get the memory details
    const size_t memdim   = _topo_hat[cdim]->memdim();
    const int    nmem[3]  = {_topo_hat[cdim]->nmem(0), _topo_hat[cdim]->nmem(1), _topo_hat[cdim]->nmem(2)};
    const size_t nloc_ax1 = _topo_hat[cdim]->nloc(ax1);

    // check the alignment
    FLUPS_CHECK(FLUPS_ISALIGNED(mygreen) && (nmem[ax0] * _topo_hat[cdim]->nf() * sizeof(double)) % FLUPS_ALIGNMENT == 0, "please use FLUPS_ALIGNMENT to align the memory", LOCATION);
    FLUPS_CHECK(FLUPS_ISALIGNED(mydata) && (nmem[ax0] * _topo_hat[cdim]->nf() * sizeof(double)) % FLUPS_ALIGNMENT == 0, "please use FLUPS_ALIGNMENT to align the memory", LOCATION);
    FLUPS_ASSUME_ALIGNED(mydata, FLUPS_ALIGNMENT);
    FLUPS_ASSUME_ALIGNED(mygreen, FLUPS_ALIGNMENT);
    
    // do the loop
#if (KIND == 01 || KIND == 11)
#pragma omp parallel for default(none) proc_bind(close) schedule(static) firstprivate(ondim, inmax, memdim, nmem, mydata, mygreen, normfact, ax0, nf, nloc_ax1,kfact,koffset,symstart,istart)
#elif (KIND == 02 || KIND == 12)
#pragma omp parallel for default(none) proc_bind(close) schedule(static) firstprivate(ondim, inmax, memdim, nmem, mydata, mygreen, normfact, ax0, nf, nloc_ax1,kfact,koffset,symstart,istart,hgrid)
#endif
    for (size_t io = 0; io < ondim; io++) {
        // get the starting pointer
        opt_double_ptr greenloc = mygreen + collapsedIndex(ax0, 0, io, nmem, nf);  //lda of Green is only 1
        opt_double_ptr dataloc0 = mydata + 0 * memdim + collapsedIndex(ax0, 0, io, nmem, nf);
        opt_double_ptr dataloc1 = mydata + 1 * memdim + collapsedIndex(ax0, 0, io, nmem, nf);
        opt_double_ptr dataloc2 = mydata + 2 * memdim + collapsedIndex(ax0, 0, io, nmem, nf);

        FLUPS_ASSUME_ALIGNED(greenloc, FLUPS_ALIGNMENT);
        FLUPS_ASSUME_ALIGNED(dataloc0, FLUPS_ALIGNMENT);
        FLUPS_ASSUME_ALIGNED(dataloc1, FLUPS_ALIGNMENT);
        FLUPS_ASSUME_ALIGNED(dataloc2, FLUPS_ALIGNMENT);

        // do the actual convolution
        for (size_t ii = 0; ii < inmax; ii++) {
            int is[3];
            cmpt_symID(ax0, ii, io % nloc_ax1, io / nloc_ax1, istart, symstart, 0, is);

#if (KIND == 01 || KIND == 02)
            // data
            const double f0r = dataloc0[ii];
            const double f1r = dataloc1[ii];
            const double f2r = dataloc2[ii];
            // green function
            const double gr = greenloc[ii];
#if (KIND == 01)
            // derivative in the direction 0 - component 1 and 2
            const double k0c1r = (is[0] + koffset[0]) * (kfact[0][1][0] + kfact[0][1][1]);
            const double k0c2r = (is[0] + koffset[0]) * (kfact[0][2][0] + kfact[0][2][1]);
            // derivative in the direction 1 - component 0 and 2
            const double k1c0r = (is[1] + koffset[1]) * (kfact[1][0][0] + kfact[1][0][1]);
            const double k1c2r = (is[1] + koffset[1]) * (kfact[1][2][0] + kfact[1][2][1]);
            // derivative in the direction 2 - component 0 and 1
            const double k2c0r = (is[2] + koffset[2]) * (kfact[2][0][0] + kfact[2][0][1]);
            const double k2c1r = (is[2] + koffset[2]) * (kfact[2][1][0] + kfact[2][1][1]);
#elif (KIND == 02)
            // derivative in the direction 0 - component 1 and 2
            const double k0c1r = sin((is[0] + koffset[0]) * (kfact[0][1][0] + kfact[0][1][1]) * hgrid[0]) / hgrid[0];
            const double k0c2r = sin((is[0] + koffset[0]) * (kfact[0][2][0] + kfact[0][2][1]) * hgrid[0]) / hgrid[0];
            // derivative in the direction 1 - component 0 and 2
            const double k1c0r = sin((is[1] + koffset[1]) * (kfact[1][0][0] + kfact[1][0][1]) * hgrid[1]) / hgrid[1];
            const double k1c2r = sin((is[1] + koffset[1]) * (kfact[1][2][0] + kfact[1][2][1]) * hgrid[1]) / hgrid[1];
            // derivative in the direction 2 - component 0 and 1
            const double k2c0r = sin((is[2] + koffset[2]) * (kfact[2][0][0] + kfact[2][0][1]) * hgrid[2]) / hgrid[2];
            const double k2c1r = sin((is[2] + koffset[2]) * (kfact[2][1][0] + kfact[2][1][1]) * hgrid[2]) / hgrid[2];
#endif
            // d(f0)/d1
            const double df0d1r = f0r * k1c0r;
            // d(f0)/d2
            const double df0d2r = f0r * k2c0r;
            // d(f1)/d0
            const double df1d0r = f1r * k0c1r;
            // d(f1)/d2
            const double df1d2r = f1r * k2c1r;
            // d(f2)/d0
            const double df2d0r = f2r * k0c2r;
            // d(f2)/d1
            const double df2d1r = f2r * k1c2r;
            // rotational
            const double rot0r = df2d1r - df1d2r;
            const double rot1r = df0d2r - df2d0r;
            const double rot2r = df1d0r - df0d1r;
            // convolution
            dataloc0[ii] = normfact * rot0r * gr;
            dataloc1[ii] = normfact * rot1r * gr;
            dataloc2[ii] = normfact * rot2r * gr;
#elif (KIND == 11 || KIND == 12)
            // data
            const double f0r = dataloc0[ii * 2 + 0];
            const double f1r = dataloc1[ii * 2 + 0];
            const double f2r = dataloc2[ii * 2 + 0];
            const double f0c = dataloc0[ii * 2 + 1];
            const double f1c = dataloc1[ii * 2 + 1];
            const double f2c = dataloc2[ii * 2 + 1];
            // green function
            const double gr = greenloc[ii * 2 + 0];
            const double gc = greenloc[ii * 2 + 1];
            // kicj = derivative in the direction i for the component j
            // derivative in the direction 0 - component 1 and 2
#if (KIND == 11)
            const double k0c1r = (is[0] + koffset[0]) * kfact[0][1][0];
            const double k0c1c = (is[0] + koffset[0]) * kfact[0][1][1];
            const double k0c2r = (is[0] + koffset[0]) * kfact[0][2][0];
            const double k0c2c = (is[0] + koffset[0]) * kfact[0][2][1];
            // derivative in the direction 1 - component 0 and 2
            const double k1c0r = (is[1] + koffset[1]) * kfact[1][0][0];
            const double k1c0c = (is[1] + koffset[1]) * kfact[1][0][1];
            const double k1c2r = (is[1] + koffset[1]) * kfact[1][2][0];
            const double k1c2c = (is[1] + koffset[1]) * kfact[1][2][1];
            // derivative in the direction 2 - component 0 and 1
            const double k2c0r = (is[2] + koffset[2]) * kfact[2][0][0];
            const double k2c0c = (is[2] + koffset[2]) * kfact[2][0][1];
            const double k2c1r = (is[2] + koffset[2]) * kfact[2][1][0];
            const double k2c1c = (is[2] + koffset[2]) * kfact[2][1][1];
#elif (KIND == 12)
            const double k0c1r = sin((is[0] + koffset[0]) * kfact[0][1][0] * hgrid[0]) / hgrid[0];
            const double k0c1c = sin((is[0] + koffset[0]) * kfact[0][1][1] * hgrid[0]) / hgrid[0];
            const double k0c2r = sin((is[0] + koffset[0]) * kfact[0][2][0] * hgrid[0]) / hgrid[0];
            const double k0c2c = sin((is[0] + koffset[0]) * kfact[0][2][1] * hgrid[0]) / hgrid[0];
            // derivative in the direction 1 - component 0 and 2
            const double k1c0r = sin((is[1] + koffset[1]) * kfact[1][0][0] * hgrid[1]) / hgrid[1];
            const double k1c0c = sin((is[1] + koffset[1]) * kfact[1][0][1] * hgrid[1]) / hgrid[1];
            const double k1c2r = sin((is[1] + koffset[1]) * kfact[1][2][0] * hgrid[1]) / hgrid[1];
            const double k1c2c = sin((is[1] + koffset[1]) * kfact[1][2][1] * hgrid[1]) / hgrid[1];
            // derivative in the direction 2 - component 0 and 1
            const double k2c0r = sin((is[2] + koffset[2]) * kfact[2][0][0] * hgrid[2]) / hgrid[2];
            const double k2c0c = sin((is[2] + koffset[2]) * kfact[2][0][1] * hgrid[2]) / hgrid[2];
            const double k2c1r = sin((is[2] + koffset[2]) * kfact[2][1][0] * hgrid[2]) / hgrid[2];
            const double k2c1c = sin((is[2] + koffset[2]) * kfact[2][1][1] * hgrid[2]) / hgrid[2];
#endif
            // d(f0)/d1
            const double df0d1r = f0r * k1c0r - f0c * k1c0c;
            const double df0d1c = f0r * k1c0c + f0c * k1c0r;
            // d(f0)/d2
            const double df0d2r = f0r * k2c0r - f0c * k2c0c;
            const double df0d2c = f0r * k2c0c + f0c * k2c0r;
            // d(f1)/d0
            const double df1d0r = f1r * k0c1r - f1c * k0c1c;
            const double df1d0c = f1r * k0c1c + f1c * k0c1r;
            // d(f1)/d2
            const double df1d2r = f1r * k2c1r - f1c * k2c1c;
            const double df1d2c = f1r * k2c1c + f1c * k2c1r;
            // d(f2)/d0
            const double df2d0r = f2r * k0c2r - f2c * k0c2c;
            const double df2d0c = f2r * k0c2c + f2c * k0c2r;
            // d(f2)/d1
            const double df2d1r = f2r * k1c2r - f2c * k1c2c;
            const double df2d1c = f2r * k1c2c + f2c * k1c2r;
            // rotational
            const double rot0r = df2d1r - df1d2r;
            const double rot0c = df2d1c - df1d2c;
            const double rot1r = df0d2r - df2d0r;
            const double rot1c = df0d2c - df2d0c;
            const double rot2r = df1d0r - df0d1r;
            const double rot2c = df1d0c - df0d1c;
            // convolution
            dataloc0[ii * 2 + 0] = normfact * (rot0r * gr - rot0c * gc);
            dataloc0[ii * 2 + 1] = normfact * (rot0r * gc + rot0c * gr);
            dataloc1[ii * 2 + 0] = normfact * (rot1r * gr - rot1c * gc);
            dataloc1[ii * 2 + 1] = normfact * (rot1r * gc + rot1c * gr);
            dataloc2[ii * 2 + 0] = normfact * (rot2r * gr - rot2c * gc);
            dataloc2[ii * 2 + 1] = normfact * (rot2r * gc + rot2c * gr);
#endif
        }
    }

    END_FUNC;
}
