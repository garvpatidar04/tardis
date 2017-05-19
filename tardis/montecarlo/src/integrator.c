#define _USE_MATH_DEFINES

#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#include "storage.h"
#include "integrator.h"
#include "cmontecarlo.h"

#include <omp.h>

#define NULEN   0
#define LINELEN 1
#define PLEN    2
#define SHELLEN 3

#define C_INV 3.33564e-11
#define M_PI acos (-1)
#define KB_CGS 1.3806488e-16
#define H_CGS 6.62606957e-27

double intensity_black_body (double nu, double T)
{
    /*
        Calculate the intensity of a black-body according to the following formula

        .. math::
            I(\\nu, T) = \\frac{2h\\nu^3}{c^2}\frac{1}{e^{h\\nu \\beta_\\textrm{rad}} - 1}

    */
    double beta_rad = 1 / (KB_CGS * T);
    double coefficient = 2 * H_CGS * C_INV * C_INV;
    return coefficient * nu * nu * nu / (exp(H_CGS * nu * beta_rad) - 1 );
}

double integrate_intensity(const double* I_nu, const double h, int N)
{
    double result = (I_nu[0] + I_nu[N-1])/2;
    for (int idx = 1; idx < N-1; ++idx)
    {
        result += I_nu[idx];
    }
    return result*h;
}

void debug_print_arg(double* arg,int len)
{
    for (int64_t i = 0; i < len; i++)
    {
        printf("%e, ",arg[i]);
    }
}
void debug_print_2d_arg(double* arg,int len1, int len2)
{
    for (int64_t i = 0; i < len1; ++i)
    {
        for(int64_t j = 0; j < len2; ++j)
        {
        printf("[%ld,%ld,%ld]: %.8f, ",i,j,i*len2+j,arg[i*len2+j]);
        }
        printf("\n\n");
    }
}

static inline double
calculate_z(double r, double p, double inv_t)
{
    return (r > p) ? sqrt(r * r - p * p) * C_INV * inv_t : 0;
}


int64_t
populate_z(const storage_model_t *storage, const double p, double *oz, int64_t *oshell_id)
{
    /*
     * Parameters:
     *      storage_model_t storage: A storage model containing the environment
     *      const double p: distance of the integration line to the center
     *      double *oz: OUTPUT: will be set with z values
     *      int64_t *oshell_id: OUTPUT: will be set with the corresponding shell_ids
     * Returns:
     *      number of shells intersected by the p-line
     *
     * This function calculates the intersection points of the p-line with each shell
     */
    // Abbreviations
    double *r = storage->r_outer;
    const int64_t N = storage->no_of_shells;
    double inv_t = storage->inverse_time_explosion;
    double z = 0;

    int64_t i = 0, offset = -1, i_low, i_up;

    if (p <= storage->r_inner[0])
    {
        // Intersect the photosphere
        for(i = 0; i < N; ++i)
        { // Loop from inside to outside
            oz[i] = 1 - calculate_z(r[i], p, inv_t);
            oshell_id[i] = i;
        }
        oz[N] = 1;
        return N + 1;
    }
    else
    {
      // Do not intersect the photosphere
      // that means we pass through all shells twice (except the innermost)
      for(i = 0; i < N; ++i)
        { // Loop from inside to outside
            z = calculate_z(r[i], p, inv_t);
            if (z==0)
                continue;
            if (offset == -1)
            {
                offset = i;
            }
            // Calculate the index in the resulting array
            i_low = N - i - 1;  // the far intersection with the shell
            i_up = N + i - 2 * offset; // the nearer intersection with the shell

            // Setting the arrays
            oz[i_low] = 1 + z;
            oshell_id[i_low] = i;
            oz[i_up] = 1 - z;
            oshell_id[i_up] = i;
        }
      oz[2 * (N - offset)] = 1;
      return 2 * (N - offset);
    }
}


double *_formal_integral(
        storage_model_t *storage,
        double iT,
        double *inu, int64_t inu_size,
        double *att_S_ul, int N)
{
    // Initialization phase
    //double L[N] = calloc(inu_size, sizeof(double));
    double *L = calloc(inu_size, sizeof(double));
#pragma omp parallel shared(L)
  {
    #pragma omp master
      {
        printf("Doing the formal integral with %d threads", omp_get_num_threads());
      }

    int64_t offset = 0, i = 0,
            size_line = storage->no_of_lines,
            size_shell = storage->no_of_shells,
            size_tau = size_line * size_shell,
            size_z = 2 * size_shell + 1,
            idx_nu_start = 0;


    double I_nu[N];  // = calloc(N, sizeof(double));
    double z[2 * storage->no_of_shells + 1];
    int64_t shell_id[2 * storage->no_of_shells + 1];
    double exp_tau[size_tau];  // = malloc(size_tau * sizeof(double));
    //double exp_tau[size_tau];

    double R_ph = storage->r_inner[0];
    double R_max = storage->r_outer[size_shell - 1];
    double p = 0, nu_start, nu_end, nu, exp_factor;

    double *pexp_tau, *patt_S_ul, *pline;

    // Prepare exp_tau
    for (i = 0; i < size_tau; ++i) {
        exp_tau[i] = exp( -storage->line_lists_tau_sobolevs[i]);
    }

    // Loop over wavelengths in spectrum
#pragma omp for
    for (int nu_idx = 0; nu_idx < inu_size ; ++nu_idx)
      {
        nu = inu[nu_idx];

        // Loop over discrete values along line
        for (int p_idx = 1; p_idx < N; ++p_idx)
        {
            // Trapezoid integration points
            p = R_max/(N -1) * (p_idx);

            populate_z(storage, p, z, shell_id);

            // initialize I_nu
            if (p <= R_ph)
                I_nu[p_idx] = intensity_black_body(nu, iT);
            else
                I_nu[p_idx] = 0;

            // TODO: Ugly loop
            // Loop over all intersections

            // TODO: replace by number of intersections and remove break
            for (i = 0; i < size_z; ++i)
            {
                if (z[i] == 1)
                    break;
                nu_start = nu * z[i];
                nu_end = nu * z[i+1];

                // Calculate offset properly
                // Which shell is important for photosphere?
                offset = shell_id[i] * size_line;

                // Find first contributing line
                line_search(
                        storage->line_list_nu,
                        nu_start,
                        size_line,
                        &idx_nu_start
                        );

                // Initialize pointers for inner loop
                pline = storage->line_list_nu + idx_nu_start;
                pexp_tau = exp_tau + offset + idx_nu_start;
                patt_S_ul = att_S_ul + offset + idx_nu_start;

                for (;pline < storage->line_list_nu + size_line;
                        // We have to increment all pointers simultanously
                        ++pline,
                        ++pexp_tau,
                        ++patt_S_ul)
                {
                    if (*pline < nu_end)
                        break;
                    I_nu[p_idx] = I_nu[p_idx] * (*pexp_tau) + *patt_S_ul;

                }
            }
            I_nu[p_idx] *= p;
        }
        L[nu_idx] = 8 * M_PI * M_PI * integrate_intensity(I_nu, R_max/N, N);
    }

    // Free everything allocated on heap
    printf("\n\n");
  }
  return L;
}
