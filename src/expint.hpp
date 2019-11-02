#ifndef _H_EXPONENTIAL_INTEGRAL
#define _H_EXPONENTIAL_INTEGRAL

/**********************************************************************/
/*                                                                    */
/*                   double ExponentialIntegral()                     */
/*                                                                    */
/**********************************************************************/
/*                                                                    */
/*  DESCRIPTION:                                                      */
/*  Calculation of the exponential integral E (x) using Chebychev     */
/*                                           1                        */
/*  polynomial expansion.                                             */
/*                                                                    */
/**********************************************************************/

#include <math.h>

static const double c_gamma = 0.577215664901532861;

static double expint1(double x);
static double expint2(double x);

static double expint_ei(double x) 
{
    double value;
 
    if (x >= -4. && x <= 4.)
        value = expint1(x);
    else if (x > 4.)
        value = expint2(x);
    else {
        value = 0.;
    }

    return (value);
}

/**********************************************************************/
/*                                                                    */
/*                      double expint1()                              */
/*                                                                    */
/**********************************************************************/
/*                                                                    */
/*  DESCRIPTION:                                                      */
/*  Calculation of the exponential integral  for -4<= x <= 4 using an */
/*  expansion in terms of Chebyshev polynomials.                      */
/*                                                                    */
/**********************************************************************/

static double expint1(double x)
{
    static int MAX = 23; /* The number of coefficients in a[].   */

    static double a[23] = {7.8737715392882774,
                           -8.0314874286705335,
                           3.8797325768522250,
                           -1.6042971072992259,
                           0.5630905453891458,
                           -0.1704423017433357,
                           0.0452099390015415,
                           -0.0106538986439085,
                           0.0022562638123478,
                           -0.0004335700473221,
                           0.0000762166811878,
                           -0.0000123417443064,
                           0.0000018519745698,
                           -0.0000002588698662,
                           0.0000000338604319,
                           -0.0000000041611418,
                           0.0000000004821606,
                           -0.0000000000528465,
                           0.0000000000054945,
                           -0.0000000000005433,
                           0.0000000000000512,
                           -0.0000000000000046,
                           0.0000000000000004};

    int    k;
    double arg, t, value, b0, b1, b2;

    arg = .25 * x; /* Argument in Chebyshev expansion is x/4. */
    t   = 2. * arg;

    b2 = 0.;
    b1 = 0.;
    b0 = a[MAX - 1];

    for (k = MAX - 2; k >= 0; k--) {
        b2 = b1;
        b1 = b0;
        b0 = t * b1 - b2 + a[k];
    }

    value = .5 * (b0 - b2);

    value += log(fabs(x));

    return (-value);

}

/**********************************************************************/
/*                                                                    */
/*                      double expint2()                              */
/*                                                                    */
/**********************************************************************/
/*                                                                    */
/*  DESCRIPTION:                                                      */
/*  Calculation of the exponential integral for x >= 4 using an expan-*/
/*  sionin terms of Chebyshev polynomials.                            */
/*                                                                    */
/**********************************************************************/

static double expint2(double x)
{
    static int MAX = 23; /* The number of coefficients in a[].   */

    static double a[23] = {0.2155283776715125,
                           0.1028106215227030,
                           -0.0045526707131788,
                           0.0003571613122851,
                           -0.0000379341616932,
                           0.0000049143944914,
                           -0.0000007355024922,
                           0.0000001230603606,
                           -0.0000000225236907,
                           0.0000000044412375,
                           -0.0000000009328509,
                           0.0000000002069297,
                           -0.0000000000481502,
                           0.0000000000116891,
                           -0.0000000000029474,
                           0.0000000000007691,
                           -0.0000000000002070,
                           0.0000000000000573,
                           -0.0000000000000163,
                           0.0000000000000047,
                           -0.0000000000000014,
                           0.0000000000000004,
                           -0.0000000000000001};

    int    k;
    double arg, t, value, b0, b1, b2;

    arg = 4. / x; /* Argument in the Chebyshev expansion.       */
    t   = 2. * (2. * arg - 1.);

    b2 = 0.;
    b1 = 0.;
    b0 = a[MAX - 1];

    for (k = MAX - 2; k >= 0; k--) {
        b2 = b1;
        b1 = b0;
        b0 = t * b1 - b2 + a[k];
    }

    value = .5 * (b0 - b2);

    value *= exp(-x);

    return (value);

}

#endif