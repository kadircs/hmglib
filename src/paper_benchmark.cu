// Copyright (C) 2016 Peter Zaspel
//
// This file is part of hmglib.
//
// hmglib is free software: you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version.
//
// hmglib is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
// A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
// details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with hmglib.  If not, see <http://www.gnu.org/licenses/>.

#include <stdio.h>
#include <curand.h>
#include <thrust/inner_product.h>
#include <thrust/fill.h>
#include "hmglib.h"
#include <gsl/gsl_qrng.h>
#include "kernel_system_assembler.h"

void gen_halton_points(double** point_set, int dim, int point_count)
{
        // allocate temporary CPU point set
	double** point_set_h = new double*[dim];
        for (int d=0; d<dim; d++)
                point_set_h[d] = new double[point_count];

	// allocate random number generator
        gsl_qrng * q = gsl_qrng_alloc (gsl_qrng_halton, dim);

	// allocate temp vector
        double* v = new double[dim];

	// generate points
        for (int i=0; i<point_count; i++)
        {
                gsl_qrng_get (q, v);
                for (int d=0; d<dim; d++)
                        point_set_h[d][i] = v[d];
        }

	// copy points to GPU
	for (int d=0; d<dim; d++)
		cudaMemcpy(point_set[d], point_set_h[d], sizeof(double)*point_count, cudaMemcpyHostToDevice);

	// cleanup
        gsl_qrng_free(q);
	delete [] v;
	for (int d=0; d<dim; d++)
		delete [] point_set_h[d];
	delete [] point_set_h;
}


int main( int argc, char* argv[])
{
	if (argc!=13)
	{
		printf("%s <Nx> <Ny> <k> <c_leaf> <eta> <kernel_type> <dim> <dense_batch_size> <aca_batch_size> <use_aca_precomputing> <use_dense_precomputing> <use_batching>\n", argv[0]);
		return 0;
	}

	// set dimension and morton code size
	int dim = atoi(argv[7]);
	
	int bits;
	if (dim==2)
		bits = 32;
	else if (dim==3)
		bits = 20;
	else
	{
		printf("Dimension %d is not supported. Exiting...\n", dim);
		exit(1);
	}

	// set number of points
	int point_count[2];
	point_count[0] = atoi(argv[1]);
	point_count[1] = atoi(argv[2]);

	// data structure for H matrix data
	struct h_matrix_data data;

	// initialize data structure
	init_h_matrix_data(&data, point_count, dim, bits);

	// set balance	
	data.eta=atof(argv[5]);

	// set maximum level
	data.max_level=50; // DEBUG

	// set maximum leaf size
	data.c_leaf=atoi(argv[4]);

	// set rank in ACA
	data.k = atoi(argv[3]);

	// set root level of clusters
	data.root_level_set_1 = 0;
	data.root_level_set_2 = 0;

// set threshold for ACA (currently not use)
//	data.epsilon = pow(10.0, atoi(argv[5]));  <- removed

	// set kernel
	int kernel_type = atoi(argv[6]);

        // set batching sizes
        data.max_batched_dense_size = atoi(argv[8]);
//	data.dense_batching_ratio = atof(argv[10]);   <- no longer needed
        data.max_batched_aca_size = atoi(argv[9]);

	int use_aca_precomputing;
	use_aca_precomputing = atoi(argv[10]);

	int use_dense_precomputing;
	use_dense_precomputing = atoi(argv[11]);
	
	int use_batching;
	use_batching = atoi(argv[12]);

	// generate Halton points in in both point sets
	gen_halton_points( data.coords_d[0], dim, point_count[0]);
	gen_halton_points( data.coords_d[1], dim, point_count[1]);

	// run setup of H matrix
	setup_h_matrix(&data);



        // setup kernel matrix assembler
	if (kernel_type == 1)
	{
		double regularization = 0.0;
	        struct gaussian_kernel_system_assembler assem;
	        struct gaussian_kernel_system_assembler** assem_d_p;
	        cudaMalloc((void***)&assem_d_p, sizeof(struct gaussian_kernel_system_assembler*));
	        create_gaussian_kernel_system_assembler_object(assem_d_p, regularization);
	        struct gaussian_kernel_system_assembler* assem_d;
	        cudaMemcpy(&assem_d, assem_d_p, sizeof(struct gaussian_kernel_system_assembler*), cudaMemcpyDeviceToHost);
	        data.assem = assem_d;
	}
	else if (kernel_type == 2)
	{
		double regularization = 0.0;
	        struct matern_kernel_system_assembler assem;
	        struct matern_kernel_system_assembler** assem_d_p;
	        cudaMalloc((void***)&assem_d_p, sizeof(struct matern_kernel_system_assembler*));
	        create_matern_kernel_system_assembler_object(assem_d_p, regularization);
	        struct matern_kernel_system_assembler* assem_d;
	        cudaMemcpy(&assem_d, assem_d_p, sizeof(struct matern_kernel_system_assembler*), cudaMemcpyDeviceToHost);
	        data.assem = assem_d;
	}



	// precomputation
	if (use_aca_precomputing)
		precompute_aca(&data);
	if (use_dense_precomputing)
		precompute_dense(&data);

	// allocate vectors for H matrix vs. full matrix test
	double* x;
	double* y;
	double* y_test;
	cudaMalloc((void**)&x, point_count[1]*sizeof(double));
	cudaMalloc((void**)&y, point_count[0]*sizeof(double));
	cudaMalloc((void**)&y_test, point_count[0]*sizeof(double));
	
	// get thrust device_ptr
	thrust::device_ptr<double> y_ptr(y);
	thrust::device_ptr<double> y_test_ptr(y_test);

	// construct random number generator
	curandGenerator_t vec_gen;
	curandCreateGenerator(&vec_gen, CURAND_RNG_PSEUDO_DEFAULT);

	int trials = 5;

	double* errors = new double[trials];

	for (int i=0; i<trials; i++)
	{
	
		thrust::fill(y_ptr, y_ptr+point_count[0], 0.0);
		thrust::fill(y_test_ptr, y_test_ptr+point_count[0], 0.0);
	
		// generate random vector x
		curandGenerateUniformDouble(vec_gen, x, point_count[1]);

		if (use_batching)
		{
			// apply H matrix to vector with batching
			apply_h_matrix_mvp(x, y, &data);
		}
		else
		{
			// apply H matrix to vector without batching
			apply_h_matrix_mvp_without_batching(x, y_test, &data);
		}
	}

	delete [] errors;


	// cleanup of random number generator
	curandDestroyGenerator(vec_gen);

	
	// cleanup of vectors
	cudaFree(y_test);
	cudaFree(x);
	cudaFree(y);

	/* currently commented out since compiler does not understand it
        // cleanup of assembler
	if (kernel_type == 1)
	{
		destroy_gaussian_kernel_system_assembler_object(assem_d_p);
        	cudaFree(assem_d_p);
	}
	else if (kernel_type == 2)
	{
		destroy_matern_kernel_system_assembler_object(assem_d_p);
        	cudaFree(assem_d_p);
	}
	*/

	// cleanup of H matrix
	destroy_h_matrix_data(&data);
}
