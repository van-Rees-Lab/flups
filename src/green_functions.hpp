/**
 * @file green_functions.hpp
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

#include "defines.hpp"
#include "Topology.hpp"
#include "bessel.hpp"
#include "expint.hpp"

// define macros to strigyfy, both are required!
#define STR(a) ZSTR(a)
#define ZSTR(a) #a


void cmpt_Green_3dirunbounded(const Topology *topo, const double hfact[3],                                                 const double symstart[3], double *green, GreenType typeGreen, const double eps);
void cmpt_Green_2dirunbounded(const Topology *topo, const double hfact[3], const double kfact[3], const double koffset[3], const double symstart[3], double *green, GreenType typeGreen, const double eps);
void cmpt_Green_1dirunbounded(const Topology *topo, const double hfact[3], const double kfact[3], const double koffset[3], const double symstart[3], double *green, GreenType typeGreen, const double eps);
void cmpt_Green_0dirunbounded(const Topology *topo, const double hgrid   , const double kfact[3], const double koffset[3], const double symstart[3], double *green, GreenType typeGreen, const double eps);
void cmpt_Green_0dirunbounded(const Topology *topo, const double hgrid   , const double kfact[3], const double koffset[3], const double symstart[3], double *green, GreenType typeGreen, const double eps, const int istart_custom[3], const int iend_custom[3]);

/**
 * @brief read the LGF file in the KERNEL_PATH folder
 * 
 * @param [in] greendim the dimension of the Green function to use, 2D or 3D
 * @param [out] N the size above which we switch to the approximation, i.e. the size of the pre-stored kernel is N^3
 * @param [out] data the data where we store the 
 */
static void _lgf_readfile(const int greendim, int* N, double** data) {
    BEGIN_FUNC;

    // some defined parameters:
    char lgfname[512];
    char path[] = STR(KERNEL_PATH);
    if (greendim == 3) {
        (*N) = 64;
        sprintf(lgfname, "%s/LGF_3d_sym_acc12_%d.ker", path, (*N));
    } else if (greendim == 2) {
        (*N) = 32;
        sprintf(lgfname, "%s/LGF_2d_sym_acc12_%d.ker", path, (*N));
    } else {
        FLUPS_ERROR("Greendim = %d is not available in this version", greendim, LOCATION);
    }

    // open the file
    FILE *lgf_file = fopen(lgfname, "r");
    // display the information to the user
    FLUPS_INFO_1("loading the LGF kernel function %s", lgfname);

    (*data) = NULL;
    // start to read the file
    if (lgf_file != NULL) {
        // allocate the data
        const int size = (*N) * (*N) * (*N);
        (*data) = (double *)flups_malloc(sizeof(double) * size);
        fread((*data), sizeof(double), size, lgf_file);
        // close the file
        fclose(lgf_file);
    } else {
        FLUPS_ERROR("unable to read file %s", lgfname, LOCATION);
    }
    END_FUNC;
}