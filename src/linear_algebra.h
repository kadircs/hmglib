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


#ifndef LINEAR_ALGEBRA_H_
#define LINEAR_ALGEBRA_H_

#include "morton.h"
#include "cub/cub.cuh"
#include "linear_algebra.h"
#include <thrust/device_vector.h>
#include <thrust/scan.h>
#include <thrust/unique.h>
#include <cuda_runtime.h>
#include "cublas_v2.h"
#include <thrust/inner_product.h>
#include <thrust/logical.h>
#include <thrust/iterator/zip_iterator.h>
#include <thrust/iterator/counting_iterator.h>
#include <thrust/partition.h>

#ifndef CHECK_CUDA_ERROR
#define CHECK_CUDA_ERROR
void checkCUDAError(const char* msg) {
cudaError_t err = cudaGetLastError();
  if (cudaSuccess != err) {
    fprintf(stderr, "Cuda error: %s: %s.\n", msg, cudaGetErrorString(err));
    exit(EXIT_FAILURE);
  }
}
#endif

cudaEvent_t ssstart, ssstop;
float mmmilliseconds;

#define TIME_ssstart {cudaEventCreate(&ssstart); cudaEventCreate(&ssstop); cudaEventRecord(ssstart);}
#define TIME_ssstop(a) {cudaEventRecord(ssstop); cudaEventSynchronize(ssstop); cudaEventElapsedTime(&mmmilliseconds, ssstart, ssstop); printf("%s: Elapsed time: %lf ms\n", a, mmmilliseconds); }


struct mat_vec_type_smaller
{
  typedef struct work_item first_argument_type;

  typedef struct work_item second_argument_type;

  typedef bool result_type;

  __host__ __device__ bool operator()(const struct work_item &lhs, const struct work_item &rhs) const
  {
//	  return lhs.work_type==WT_DENSE;  <----- caused bug
	  return lhs.work_type>rhs.work_type;
  }
};

void sort_mat_vec_data(struct work_item* mat_vec_data, int mat_vec_data_count)
{
	thrust::device_ptr<struct work_item> mat_vec_data_ptr(mat_vec_data);

	struct mat_vec_type_smaller smaller;
	thrust::stable_sort(mat_vec_data_ptr, mat_vec_data_ptr+mat_vec_data_count, smaller);


}

__host__ __device__ double kernel(double val)
{
	return (1.0 + val) * exp(-val);
}

__global__ void fill_matrix(double* matrix, struct work_item current_mat_vec_data, struct point_set* input_set1, struct point_set* input_set2, int m1, int m2)
{
	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= m1*m2)
		return;

	int i,j;

	j = idx / m1;
	i = idx % m1;

	int dim = input_set1->dim;
	double point1[5];
	double point2[5];

	for (int d=0; d<dim; d++)
	{
		point1[d] = input_set1->coords[d][current_mat_vec_data.set1_l+i];
		point2[d] = input_set2->coords[d][current_mat_vec_data.set2_l+j];
	}

	double val = 0;
	for (int d=0; d<dim; d++)
	{
		val += (point1[d]-point2[d])*(point1[d]-point2[d]);
	}
	val = sqrt(val);

	matrix[idx] = kernel(val);
}

__global__ void fill_kernel_vector(double* vec, int lA, int uA, int iB, struct point_set* input_setA, struct point_set* input_setB)
{
	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= uA-lA+1)
		return;

	int dim = input_setA->dim;
	double pointA[5];
	double pointB[5];

	for (int d=0; d<dim; d++)
	{
		pointA[d] = input_setA->coords[d][lA+idx];
		pointB[d] = input_setB->coords[d][iB];
	}

	double val = 0;
	for (int d=0; d<dim; d++)
	{
		val += (pointA[d]-pointB[d])*(pointA[d]-pointB[d]);
	}
	val = sqrt(val);

	vec[idx] = kernel(val);
}

struct scaled_minus
{
	double scaling;
    scaled_minus(double _scaling)
	{
    	scaling = _scaling;
	}

    __host__ __device__ double operator()(const double &lhs, const double &rhs) const {return lhs - scaling*rhs;}
};

struct divide_by
{
	double val;

	__host__ __device__ double operator()(const double &x) const {return x/val;}
};

struct scale_by
{
	double val;

	__host__ __device__ double operator()(const double &x) const {return x*val;}
};

struct compare_absolute
{
	__host__ __device__	bool operator()(double lhs, double rhs)
	{
		return fabs(lhs) < fabs(rhs);
	}
};

void apply_dense_matrix_for_current_work_item(double* x, double* y, struct work_item current_mat_vec_data, struct point_set* input_set1, struct point_set* input_set2, int vector_size, cublasStatus_t stat, cublasHandle_t handle)
{
	int block_size = 512;

	// getting matrix size
	int m1 = current_mat_vec_data.set1_u-current_mat_vec_data.set1_l+1; // number of rows
	int m2 = current_mat_vec_data.set2_u-current_mat_vec_data.set2_l+1; // number of columns

	// allocating local matrix
	double* matrix;
	cudaMalloc((void**)&matrix, m1*m2*sizeof(double));

	// setup of local matrix
	fill_matrix<<<(m1*m2 + (block_size - 1)) / block_size, block_size>>>(matrix, current_mat_vec_data, input_set1, input_set2, m1, m2);
	cudaThreadSynchronize();
	checkCUDAError("fill_matrix");

	// allocation and extraction of local operand
	double* local_x;
	cudaMalloc((void**)&local_x, m2*sizeof(double));
	cudaMemcpy(local_x, &x[current_mat_vec_data.set2_l], m2*sizeof(double), cudaMemcpyDeviceToDevice);

	// allocation of local result
	double* local_y;
	cudaMalloc((void**)&local_y, m1*sizeof(double));

	// matrix-vector-product
	double one;
	double zero;
	one = 1.0;
	zero = 0.0;
	stat = cublasDgemv(handle, CUBLAS_OP_N, m1, m2, &one, matrix, m1, local_x, 1, &zero, local_y, 1);
	if (stat!=CUBLAS_STATUS_SUCCESS)
	{
		printf("dgemv did not succeed...\n");
		exit(1);
	}

	thrust::device_ptr<double> local_y_ptr(local_y);
	thrust::device_ptr<double> y_ptr(y);

	// adding local result to full vector
	thrust::transform(y_ptr+current_mat_vec_data.set1_l, y_ptr+current_mat_vec_data.set1_l+m1, local_y_ptr, y_ptr+current_mat_vec_data.set1_l, thrust::plus<double>());

	// cleanup
	cudaFree(local_y);
	cudaFree(local_x);
	cudaFree(matrix);

}




__global__ void fill_kernel_vector_and_substract_previous_vectors(double* vec, int lA, int uA, int iB, struct point_set* input_setA, struct point_set* input_setB, int m1, int m2, double* U, double* V, int r, int i_r)
{
	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= m2)
		return;

	int dim = input_setA->dim;
	double pointA[5];
	double pointB[5];

	for (int d=0; d<dim; d++)
	{
		pointA[d] = input_setA->coords[d][lA+idx];
		pointB[d] = input_setB->coords[d][iB];
	}

	double val = 0;
	for (int d=0; d<dim; d++)
	{
		val += (pointA[d]-pointB[d])*(pointA[d]-pointB[d]);
	}
	val = sqrt(val);

	val = kernel(val);

	for (int l=0; l<r; l++)
	{
		double scaling = U[l*m1+i_r];
		val -= scaling*V[l*m2+idx];
	}

	vec[idx] = val;
}

double compute_frobenius_norm_of_low_rank_matrix(double* U, double* V, int m1, int m2, int k, cublasStatus_t stat, cublasHandle_t handle)
{
	// frobenius(U*V') = sqrt(sum(sum((V'*V).*(U'*U))))

	// C = U'*U
	double* C;
	cudaMalloc((void**)&C, k*k*sizeof(double));

	double one;
	double zero;
	one = 1.0;
	zero = 0.0;
	stat = cublasDgemm(handle, CUBLAS_OP_T, CUBLAS_OP_N, k, k, m1, &one, U, m1, U, m1, &zero, C, k);
	if (stat!=CUBLAS_STATUS_SUCCESS)
	{
		printf("dgemm did not succeed...\n");
		exit(1);
	}

	// D = V'*V
	double* D;
	cudaMalloc((void**)&D, k*k*sizeof(double));

	stat = cublasDgemm(handle, CUBLAS_OP_T, CUBLAS_OP_N, k, k, m2, &one, V, m2, U, m2, &zero, D, k);
	if (stat!=CUBLAS_STATUS_SUCCESS)
	{
		printf("dgemm did not succeed...\n");
		exit(1);
	}

	// res = C(:)'*D(:)
	double res;
	stat = cublasDdot(handle, k*k, C, 1, D, 1, &res);

	cudaFree(C);
	cudaFree(D);

	// res = sqrt(res)
	return sqrt(res);
}


void apply_aca_for_current_work_item(double* x, double* y, struct work_item current_mat_vec_data, struct point_set* input_set1, struct point_set* input_set2, int vector_size, cublasStatus_t stat, cublasHandle_t handle, double eta, double epsilon, int k)
{
	int block_size = 512;

	// getting matrix size
	int m1 = current_mat_vec_data.set1_u-current_mat_vec_data.set1_l+1; // number of rows
	int m2 = current_mat_vec_data.set2_u-current_mat_vec_data.set2_l+1; // number of columns

	// if (k>min(m,n))
	//     k= min(m,n);
	// end
	if (k>min(m1, m2))
		k = min(m1, m2);

	double* U;
	cudaMalloc((void**)&U, m1*k*sizeof(double));
	checkCUDAError("cudaMalloc");
	double* V;
	cudaMalloc((void**)&V, m2*k*sizeof(double));
	checkCUDAError("cudaMalloc");

	thrust::device_ptr<double> U_ptr(U);
	thrust::device_ptr<double> V_ptr(V);

	thrust::fill(U_ptr, U_ptr+m1*k, 0.0);
	thrust::fill(V_ptr, V_ptr+m2*k, 0.0);

	double* v_r;
	double* u_r;

	// i_r = 0;
	int i_r = -1;

	struct divide_by div;

//	TIME_ssstart;

	// for r=1:k
	for (int r=0; r<k; r++)
	{
		// while (norm(v_tilde_r,Inf)==0.0)
	    //    i_r = i_r+1;
	    //    v_tilde_r = kernel(input_set1(i_r,:), input_set2);
	    //    for l=1:r-1
	    //        v_tilde_r = v_tilde_r - U(i_r,l) * V(l,:);
	    //    end
	    // end

        // U = [U u_r];
        // V = [V; v_r];
		v_r = &V[r*m2];
		u_r = &U[r*m1];
		thrust::device_ptr<double> u_r_ptr(u_r);
		thrust::device_ptr<double> v_r_ptr(v_r);

		do
		{
//			TIME_ssstart;
			i_r++;

			fill_kernel_vector_and_substract_previous_vectors<<<(m2 + (block_size - 1)) / block_size, block_size>>>(v_r, current_mat_vec_data.set2_l, current_mat_vec_data.set2_u, current_mat_vec_data.set1_l+i_r, input_set2, input_set1, m1, m2, U, V, r, i_r);
			cudaThreadSynchronize();
			checkCUDAError("fill_kernel_vector_and_substract_previous_vectors");

			double norm = sqrt(thrust::inner_product(v_r_ptr, v_r_ptr+m2, v_r_ptr, 0.0));

//			TIME_ssstop("ACA do-loop");

			if (norm>=1.0e-13) break;
		} while (true);


//		TIME_ssstart;

	    // [m,j_r] = max(abs(v_tilde_r));
		thrust::device_ptr<double> max_pos = thrust::max_element(v_r_ptr, v_r_ptr+m2, compare_absolute());
		int j_r = max_pos - v_r_ptr;

//		TIME_ssstop("ACA intermediate 1");
//		TIME_ssstart;

		// v_r = (1.0./(v_tilde_k(j_r)))*v_tilde_r;
	    cudaMemcpy(&div.val, &v_r[j_r], sizeof(double), cudaMemcpyDeviceToHost);
		checkCUDAError("cudaMemcpy2");
		thrust::transform(v_r_ptr, v_r_ptr+m2, v_r_ptr, div);

//		TIME_ssstop("ACA intermediate 2");
//		TIME_ssstart;

//		// u_r = kernel(input_set1(:,:),input_set2(j_r,:));
//	    // for l=1:r-1
//	    //     u_r = u_r - V(l,j_r) * U(:,l);
//	    // end

		fill_kernel_vector_and_substract_previous_vectors<<<(m1 + (block_size - 1)) / block_size, block_size>>>(u_r, current_mat_vec_data.set1_l, current_mat_vec_data.set1_u, current_mat_vec_data.set2_l+j_r, input_set1, input_set2, m2, m1, V, U, r, j_r);
		cudaThreadSynchronize();
		checkCUDAError("fill_kernel_vector_and_substract_previous_vectors");

//		TIME_ssstop("ACA middle");

		if (r%5==0) // apply stopping criterion only in every fifth iteration since it is very expensive
		{
//			TIME_ssstart;

			// frobenius(U*V') = sqrt(sum(sum((V'*V).*(U'*U))))
			double res = compute_frobenius_norm_of_low_rank_matrix(U, V, m1, m2, k, stat, handle);

//			TIME_ssstop("ACA frobenius norm");

//			TIME_ssstart;

			double u_r_2norm;
			double v_r_2norm;

			// factor of 2 in performance (better) when cuBLAS is not used and thrust is used !!!
			u_r_2norm = sqrt(thrust::inner_product(u_r_ptr, u_r_ptr+m1, u_r_ptr, 0.0));
			v_r_2norm = sqrt(thrust::inner_product(v_r_ptr, v_r_ptr+m2, v_r_ptr, 0.0));


//			TIME_ssstop("ACA norm rest");

			//		printf("u v f %le %le %le\n", u_r_2norm, v_r_2norm, res);
			if (u_r_2norm*v_r_2norm <= ((epsilon*(1.0-eta))/(1.0+epsilon))*res)
			{
				//			printf("AAAAUUUUFFFHÖÖÖÖÖRRREEENNNN!!!!!! Schluss jetzt!\n");
				//			printf("r=%d\n", r);
				break;
			}
		}
	}

//	TIME_ssstop("aca approx");

//	TIME_ssstart;

	// allocation and extraction of local operand
	double* local_x;
	cudaMalloc((void**)&local_x, m2*sizeof(double));
	checkCUDAError("cudaMalloc");
	cudaMemcpy(local_x, &x[current_mat_vec_data.set2_l], m2*sizeof(double), cudaMemcpyDeviceToDevice);

	// allocation of local intermediate result
	double* local_tmp;
	cudaMalloc((void**)&local_tmp, k*sizeof(double));
	checkCUDAError("cudaMalloc");

	// allocation of local result
	double* local_y;
	cudaMalloc((void**)&local_y, m1*sizeof(double));
	checkCUDAError("cudaMalloc");

	// matrix-vector-product
	double one;
	double zero;
	one = 1.0;
	zero = 0.0;
	stat = cublasDgemv(handle, CUBLAS_OP_T, m2, k, &one, V, m2, local_x, 1, &zero, local_tmp, 1);
	if (stat!=CUBLAS_STATUS_SUCCESS)
	{
		printf("dgemv did not succeed...\n");
		exit(1);
	}

	stat = cublasDgemv(handle, CUBLAS_OP_N, m1, k, &one, U, m1, local_tmp, 1, &zero, local_y, 1);
	if (stat!=CUBLAS_STATUS_SUCCESS)
	{
		printf("dgemv did not succeed...\n");
		exit(1);
	}

	thrust::device_ptr<double> local_y_ptr(local_y);
	thrust::device_ptr<double> y_ptr(y);

	// adding local result to full vector
	thrust::transform(y_ptr+current_mat_vec_data.set1_l, y_ptr+current_mat_vec_data.set1_l+m1, local_y_ptr, y_ptr+current_mat_vec_data.set1_l, thrust::plus<double>());

//	TIME_ssstop("aca apply");

	cudaFree(local_x);
	cudaFree(local_y);
	cudaFree(local_tmp);
	cudaFree(U);
	cudaFree(V);
//	cudaFree(v_r);
//	cudaFree(u_r);

}

__global__ void set_bounds_for_point_maps(int* point_map1, int* point_map2, int* point_map_offsets1, int* point_map_offsets2, int* m1, int* m2, int work_item_type, struct work_item* current_level_data, int mat_vec_data_count)
{
	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= mat_vec_data_count)
		return;

	struct work_item* work = &current_level_data[idx];

	if (work->work_type!=work_item_type)
		return;

	point_map1[point_map_offsets1[idx]] = work->set1_l;
	point_map1[point_map_offsets1[idx]+m1[idx]-1] = -(work->set1_u-1);

	point_map2[point_map_offsets2[idx]] = work->set2_l;
	point_map2[point_map_offsets2[idx]+m2[idx]-1] = -(work->set2_u-1);
}

__global__ void correct_bounds_for_point_maps(int* point_map1, int* point_map2, int* point_map_offsets1, int* point_map_offsets2, int* m1, int* m2, int work_item_type, struct work_item* current_level_data, int mat_vec_data_count)
{
	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= mat_vec_data_count)
		return;

	struct work_item* work = &current_level_data[idx];

	if (work->work_type!=work_item_type)
		return;

	point_map1[point_map_offsets1[idx]+m1[idx]-1] = work->set1_u;

	point_map2[point_map_offsets2[idx]+m2[idx]-1] = work->set2_u;
}

__global__ void set_bounds_for_work_item_maps(int* work_item_map1, int* work_item_map2, int* point_map_offsets1, int* point_map_offsets2, int* m1, int* m2, int work_item_type, struct work_item* current_level_data, int mat_vec_data_count)
{
	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= mat_vec_data_count)
		return;

	struct work_item* work = &current_level_data[idx];

	if (work->work_type!=work_item_type)
		return;

	work_item_map1[point_map_offsets1[idx]] = idx;
	work_item_map1[point_map_offsets1[idx]+m1[idx]-1] = -idx;

	work_item_map2[point_map_offsets2[idx]] = idx;
	work_item_map2[point_map_offsets2[idx]+m2[idx]-1] = -idx;
}

__global__ void correct_bounds_for_work_item_maps(int* work_item_map1, int* work_item_map2, int* point_map_offsets1, int* point_map_offsets2, int* m1, int* m2, int work_item_type, struct work_item* current_level_data, int mat_vec_data_count)
{
	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= mat_vec_data_count)
		return;

	struct work_item* work = &current_level_data[idx];

	if (work->work_type!=work_item_type)
		return;

	work_item_map1[point_map_offsets1[idx]+m1[idx]-1] = idx;

	work_item_map2[point_map_offsets2[idx]+m2[idx]-1] = idx;
}

//__global__ void set_bounds_for_point_maps_valid_entries(int* point_map_valid_entries1, int* point_map_valid_entries2, int* point_map_offsets1, int* point_map_offsets2, int work_item_type, struct work_item* current_level_data, int mat_vec_data_count)
//{
//	int idx = blockIdx.x * blockDim.x + threadIdx.x;
//
//	if (idx >= mat_vec_data_count)
//		return;
//
//	struct work_item* work = &current_level_data[idx];
//
//	if (work->work_type!=work_item_type)
//		return;
//
//	point_map_valid_entries1[point_map_offsets1[idx]] = 1;
//	point_map_valid_entries1[point_map_offsets1[idx+1]-1] = -1;
//
//	point_map_valid_entries2[point_map_offsets2[idx]] = 1;
//	point_map_valid_entries2[point_map_offsets2[idx+1]-1] = -1;
//}
//
//__global__ void correct_bounds_for_point_maps_valid_entries(int* point_map1, int* point_map2, int* point_map_offsets1, int* point_map_offsets2, int work_item_type, struct work_item* current_level_data, int mat_vec_data_count)
//{
//	int idx = blockIdx.x * blockDim.x + threadIdx.x;
//
//	if (idx >= mat_vec_data_count)
//		return;
//
//	struct work_item* work = &current_level_data[idx];
//
//	if (work->work_type!=work_item_type)
//		return;
//
//	point_map1[point_map_offsets1[idx+1]-1] = 1;
//
//	point_map2[point_map_offsets2[idx+1]-1] = 1;
//}



struct minus_plus_1
{
    __host__ __device__ double operator()(const double &lhs, const double &rhs) const {return lhs - rhs + 1;}
};

__global__ void set_k_per_item(int* k_per_item, int k, int mat_vec_data_count, int* m1, int* m2)
{
	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= mat_vec_data_count)
		return;

	int _m1, _m2;
	_m1 = m1[idx];
	_m2 = m2[idx];

	if (k>min(_m1, _m2))
	{
		k_per_item[idx] = min(_m1, _m2);
	}
	else
		k_per_item[idx] = k;

}

__global__ void batched_fill_kernel_vector_v_r(double* v_r, int* point_map2, int* point_map1, int* point_map_offsets1, int* work_item_map2, int* i_r, int* compute_v_r, struct point_set* input_set2, struct point_set* input_set1, int m2_total)
{
	//    v_tilde_r = kernel(input_set1(i_r,:), input_set2);

	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= m2_total)
		return;

	int dim = input_set2->dim;
	double point2[5];
	double point1[5];

	int global_point_index2 = point_map2[idx];

// not necessary since all entries are valid
//	if (global_point_index2==-1)
//		return;

	int work_item_index = work_item_map2[idx];

	// this work item has already been successfully computed
	if (compute_v_r[work_item_index]==0)
		return;

//	printf("%d\n", point_map1[point_map_offsets1[work_item_index]]+i_r[work_item_index]);

	for (int d=0; d<dim; d++)
	{
		point2[d] = input_set2->coords[d][global_point_index2];
		point1[d] = input_set1->coords[d][point_map1[point_map_offsets1[work_item_index]]+i_r[work_item_index]];
	}

	double val = 0;
	for (int d=0; d<dim; d++)
	{
		val += (point2[d]-point1[d])*(point2[d]-point1[d]);
	}
	val = sqrt(val);

	v_r[idx] = kernel(val);
}

struct is_zero
{
	__host__ __device__ bool operator()(int x) { return x==0; }
};

struct is_one
{
	__host__ __device__ bool operator()(int x) { return x==1; }
};

struct is_minus_one
{
	__host__ __device__ bool operator()(int x) { return x==-1; }
};

struct is_not_minus_one
{
	__host__ __device__ bool operator()(int x) { return x!=-1; }
};

struct square
{
	__host__ __device__ double operator()(double x) {return x*x;}
};

struct square_root
{
	__host__ __device__ double operator()(double x) {return sqrt(x);}
};

struct bigger_than_eps
{
	__host__ __device__ bool operator()(double x) {return fabs(x)>=1.0e-13;}
};

struct add_one
{
	__host__ __device__ int operator()(int a) { return a+1; }
};

struct is_not_WT_ACA
{
	__host__ __device__ bool operator()(struct work_item a) { return a.work_type!=WT_ACA; }
};

struct is_smaller_or_equal_r
{
	int r;

	is_smaller_or_equal_r(int _r)
	{
		r = _r;
	}

	__host__ __device__ bool operator()(int a) { return a<=r; }
};

struct is_smaller
{
	double val;

	is_smaller(double _val)
	{
		val = _val;
	}

	__host__ __device__ bool operator()(double a) { return a<val; }
};


__global__ void batched_scaled_substraction_for_v_r(double* v_r, int* point_map2, int* point_map_offsets1, int* work_item_map2, int* i_r, int* compute_v_r, double* V, double* U, struct point_set* input_set2, struct point_set* input_set1, int* k_per_item, int r, int m2_total, int m1_total)
{
	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= m2_total)
		return;

	int work_item_index = work_item_map2[idx];

	// nothing to do left for this element
	if (compute_v_r[work_item_index]==0)
		return;

	for (int l=0; (l<r)&&(l<k_per_item[work_item_index]); l++)
	{
		//		cudaMemcpy(&scaling, &U[l*m1+i_r], sizeof(double), cudaMemcpyDeviceToHost);
		//		checkCUDAError("cudaMemcpy1");
		//
		//		struct scaled_minus p(scaling);
		//
		//		thrust::transform(v_r_ptr, v_r_ptr+m2, V_ptr+(l*m2), v_r_ptr, p);

		double scaling = U[l*m1_total + point_map_offsets1[work_item_index] + i_r[work_item_index]];
		v_r[idx] -= scaling * V[l*m2_total+idx];
	}
}

__global__ void batched_fill_kernel_vector_and_scaled_substraction_for_v_r(double* v_r, int* point_map2, int* point_map1, int* point_map_offsets1, int* work_item_map2, int* i_r, int* compute_v_r, struct point_set* input_set2, struct point_set* input_set1, int m2_total, int m1_total, double* V, double* U, int r, int* k_per_item)
{
	//    v_tilde_r = kernel(input_set1(i_r,:), input_set2);

	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= m2_total)
		return;

	int dim = input_set2->dim;
	double point2[5];
	double point1[5];

	int global_point_index2 = point_map2[idx];

// not necessary since all entries are valid
//	if (global_point_index2==-1)
//		return;

	int work_item_index = work_item_map2[idx];

	// this work item has already been successfully computed
	if (compute_v_r[work_item_index]==0)
		return;

//	printf("%d\n", point_map1[point_map_offsets1[work_item_index]]+i_r[work_item_index]);

	for (int d=0; d<dim; d++)
	{
		point2[d] = input_set2->coords[d][global_point_index2];
		point1[d] = input_set1->coords[d][point_map1[point_map_offsets1[work_item_index]]+i_r[work_item_index]];
	}

	double val = 0;
	for (int d=0; d<dim; d++)
	{
		val += (point2[d]-point1[d])*(point2[d]-point1[d]);
	}
	val = sqrt(val);

	val = kernel(val);

	for (int l=0; (l<r)&&(l<k_per_item[work_item_index]); l++)
	{
		//		cudaMemcpy(&scaling, &U[l*m1+i_r], sizeof(double), cudaMemcpyDeviceToHost);
		//		checkCUDAError("cudaMemcpy1");
		//
		//		struct scaled_minus p(scaling);
		//
		//		thrust::transform(v_r_ptr, v_r_ptr+m2, V_ptr+(l*m2), v_r_ptr, p);

		double scaling = U[l*m1_total + point_map_offsets1[work_item_index] + i_r[work_item_index]];
		val -= scaling * V[l*m2_total+idx];
	}

	v_r[idx] = val;
}



__global__ void batched_scaling_of_v_r(double* v_r, int* work_item_to_batch_map, int* work_item_map2, int* k_per_item, int r, int* j_r, int m2_total)
{
	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= m2_total)
		return;

	int work_item_index = work_item_map2[idx];

	if (r>=k_per_item[work_item_index])
		return;

	// not necessary => is always valid by construction
	// if (work_item_index==-1)
	//	return;

	int batch_number = work_item_to_batch_map[work_item_index];

    // v_r = (1.0./(v_tilde_k(j_r)))*v_tilde_r;
	v_r[idx] = v_r[idx] / v_r[j_r[batch_number]];
}


typedef thrust::tuple<double,int> DoubleIntTuple;

struct tuple_absolute_maximum
{
	__host__ __device__ DoubleIntTuple operator()(const DoubleIntTuple lhs, const DoubleIntTuple rhs) const
	{
		double x1 = thrust::get<0>(lhs);
		double x2 = thrust::get<0>(rhs);
		double i1 = thrust::get<1>(lhs);
		double i2 = thrust::get<1>(rhs);
		double abs_max_x;
		double abs_max_i;

		if (fabs(x1)>fabs(x2))
		{
			abs_max_x = x1;
			abs_max_i = i1;
		}
		else
		{
			abs_max_x = x2;
			abs_max_i = i2;
		}

		DoubleIntTuple result;
		thrust::get<0>(result) = abs_max_x;
		thrust::get<1>(result) = abs_max_i;
		return result;
	}
};


__global__ void batched_fill_kernel_vector_u_r(double* u_r, int* point_map1, int* point_map2, int* work_item_to_batch_map, int* work_item_map1, int* k_per_item, int r, int* j_r_global, struct point_set* input_set1, struct point_set* input_set2, int m1_total)
{
	//    // u_r = kernel(input_set1(:,:),input_set2(j_r,:));
	//	fill_kernel_vector<<<(m1 + (block_size - 1)) / block_size, block_size>>>(u_r, current_mat_vec_data.set1_l, current_mat_vec_data.set1_u, current_mat_vec_data.set2_l+j_r, input_set1, input_set2);
	//	cudaThreadSynchronize();
	//	checkCUDAError("fill_kernel_vector2");

	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= m1_total)
		return;

	int dim = input_set1->dim;
	double point1[5];
	double point2[5];

	int global_point_index1 = point_map1[idx];

// is always valid by construction
//	if (global_point_index1==-1)
//		return;

	int work_item_index = work_item_map1[idx];

	if (r>=k_per_item[work_item_index])
		return;

	// oh my gosh, the following is extremely expensive!
	int batch_number = work_item_to_batch_map[work_item_index];
	int global_point_index2 = point_map2[j_r_global[batch_number]];

	for (int d=0; d<dim; d++)
	{
		point1[d] = input_set1->coords[d][global_point_index1];
		point2[d] = input_set2->coords[d][global_point_index2];
	}

	double val = 0;
	for (int d=0; d<dim; d++)
	{
		val += (point2[d]-point1[d])*(point2[d]-point1[d]);
	}
	val = sqrt(val);

	u_r[idx] = kernel(val);
}

__global__ void batched_scaled_substraction_for_u_r(double* u_r, int* point_map1, int* work_item_to_batch_map, int* work_item_map1, int* j_r_global, double* U, double* V, struct point_set* input_set1, struct point_set* input_set2, int* k_per_item, int r, int m1_total, int m2_total)
{
	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= m1_total)
		return;

	int work_item_index = work_item_map1[idx];

	if (r>=k_per_item[work_item_index])
		return;

	int batch_number = work_item_to_batch_map[work_item_index];

	for (int l=0; l<r; l++)
	{
		//	    // for l=1:r-1
		//	    //     u_r = u_r - V(l,j_r) * U(:,l);
		//	    // end
		//		for (int l=0; l<r; l++)
		//		{
		//			double scaling;
		//			cudaMemcpy(&scaling, &V[l*m2+j_r], sizeof(double), cudaMemcpyDeviceToHost);
		//			checkCUDAError("cudaMemcpy3");
		//
		//			struct scaled_minus p(scaling);
		//
		//			thrust::transform(u_r_ptr, u_r_ptr+m1, U_ptr+(l*m1), u_r_ptr, p);
		//		}

		double scaling = V[l*m2_total + j_r_global[batch_number]];
		u_r[idx] -= scaling * U[l*m1_total+idx];
	}
}



__global__ void batched_fill_kernel_vector_and_scaled_substraction_for_u_r(double* u_r, int* point_map1, int* point_map2, int* work_item_to_batch_map, int* work_item_map1, int* k_per_item, int r, int* j_r_global, struct point_set* input_set1, struct point_set* input_set2, int m1_total, int m2_total, double* U, double* V)
{
	//    // u_r = kernel(input_set1(:,:),input_set2(j_r,:));
	//	fill_kernel_vector<<<(m1 + (block_size - 1)) / block_size, block_size>>>(u_r, current_mat_vec_data.set1_l, current_mat_vec_data.set1_u, current_mat_vec_data.set2_l+j_r, input_set1, input_set2);
	//	cudaThreadSynchronize();
	//	checkCUDAError("fill_kernel_vector2");

	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= m1_total)
		return;

	int dim = input_set1->dim;
	double point1[5];
	double point2[5];

	int global_point_index1 = point_map1[idx];

// is always valid by construction
//	if (global_point_index1==-1)
//		return;

	int work_item_index = work_item_map1[idx];

	if (r>=k_per_item[work_item_index])
		return;

	// oh my gosh, the following is extremely expensive!
	int batch_number = work_item_to_batch_map[work_item_index];

	int j_r_global_evaluated = j_r_global[batch_number];

	int global_point_index2 = point_map2[j_r_global_evaluated];

	for (int d=0; d<dim; d++)
	{
		point1[d] = input_set1->coords[d][global_point_index1];
		point2[d] = input_set2->coords[d][global_point_index2];
	}

	double val = 0;
	for (int d=0; d<dim; d++)
	{
		val += (point2[d]-point1[d])*(point2[d]-point1[d]);
	}
	val = sqrt(val);

	val = kernel(val);

	for (int l=0; l<r; l++)
	{
		//	    // for l=1:r-1
		//	    //     u_r = u_r - V(l,j_r) * U(:,l);
		//	    // end
		//		for (int l=0; l<r; l++)
		//		{
		//			double scaling;
		//			cudaMemcpy(&scaling, &V[l*m2+j_r], sizeof(double), cudaMemcpyDeviceToHost);
		//			checkCUDAError("cudaMemcpy3");
		//
		//			struct scaled_minus p(scaling);
		//
		//			thrust::transform(u_r_ptr, u_r_ptr+m1, U_ptr+(l*m1), u_r_ptr, p);
		//		}

		double scaling = V[l*m2_total + j_r_global_evaluated];
		val -= scaling * U[l*m1_total + idx];
	}

	u_r[idx] = val;
}




__device__ double myAtomicAdd(double* address, double val)
{
    unsigned long long int* address_as_ull =
                                          (unsigned long long int*)address;
    unsigned long long int old = *address_as_ull, assumed;
    do {
        assumed = old;
        old = atomicCAS(address_as_ull, assumed,
                        __double_as_longlong(val +
                        __longlong_as_double(assumed)));
    } while (assumed != old);
    return __longlong_as_double(old);
}

__global__ void add_batched_local_results_to_full_vector(double* y, double* y_local, int* point_map1, int* work_item_map1, int m1_total)
{
	// adding batched local results to full vector
	//thrust::transform(y_ptr+current_mat_vec_data.set1_l, y_ptr+current_mat_vec_data.set1_l+m1, local_y_ptr, y_ptr+current_mat_vec_data.set1_l, thrust::plus<double>());

	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= m1_total)
		return;

	double val = y_local[idx];

	myAtomicAdd(&y[point_map1[idx]], val);

}

__global__ void get_work_item_point_set_limits_for_given_type(int* l, int* u, int mat_vec_data_type, struct work_item* mat_vec_data, int mat_vec_data_count, int point_set_nr)
{
	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= mat_vec_data_count)
		return;


	if (mat_vec_data[idx].work_type==mat_vec_data_type)
	{
		if (point_set_nr==1)
		{
			l[idx] = mat_vec_data[idx].set1_l;
			u[idx] = mat_vec_data[idx].set1_u;
		}
		else
		{
			l[idx] = mat_vec_data[idx].set2_l;
			u[idx] = mat_vec_data[idx].set2_u;
		}
	}
	else
	{
		l[idx] = -1;
		u[idx] = -2;
	}

}

// if a work_item has finished, set respective compute_v_r entry to 0
__global__ void update_ir(int* i_r, int* compute_v_r, int mat_vec_data_count, int* keys_output, double* values_output, int output_set_counts)
{
	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= output_set_counts)
		return;

	int work_item_index = keys_output[idx];

	if (compute_v_r[work_item_index]==0)
		return;

	if (fabs(values_output[idx])>=1.0e-14)
		compute_v_r[work_item_index] = 0;
}

__global__ void remove_rubbish_from_maxima(int* i_r, int* compute_v_r, int mat_vec_data_count, int* keys_output, double* values_output, int output_set_counts)
{
	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= output_set_counts)
		return;

	int work_item_index = keys_output[idx];

	if (compute_v_r[work_item_index]==0)
	{
		values_output[idx] = 0.0;
		return;
	}
}

__global__ void finalize_norm_computation(double* values_output,int output_set_counts)
{
	int idx = blockIdx.x * blockDim.x + threadIdx.x;

	if (idx >= output_set_counts)
		return;

	values_output[idx] = sqrt(values_output[idx]);
}


//double* values_output;
//cudaMalloc((void**)&values_output, m_total*sizeof(double));

void compute_batched_norms(double* batched_norms, int* norm_count, double* x, int m_total, thrust::device_ptr<int> work_item_map_ptr, int block_size)
{
		double* x_tmp;
		int* keys_output;
		cudaMalloc((void**)&x_tmp, m_total*sizeof(double));
		cudaMalloc((void**)&keys_output, m_total*sizeof(int));
		thrust::device_ptr<double> x_tmp_ptr(x_tmp);
		thrust::device_ptr<int> keys_output_ptr(keys_output);
		thrust::device_ptr<double> batched_norms_ptr(batched_norms);

		// computing norms of batched vectors
		cudaMemcpy(x_tmp, x, m_total*sizeof(double), cudaMemcpyDeviceToDevice);
		thrust::transform(x_tmp_ptr, x_tmp_ptr+m_total, x_tmp_ptr, square());
		thrust::pair<thrust::device_ptr<int>, thrust::device_ptr<double> > new_end;
		new_end = thrust::reduce_by_key(work_item_map_ptr, work_item_map_ptr+m_total, x_tmp_ptr, keys_output_ptr, batched_norms_ptr, thrust::equal_to<int>(), thrust::plus<double>());

		// output_set_count is NOT equal to mat_mat_vec_data_count, since invalid entries are discarded
		*norm_count = new_end.first - keys_output_ptr;

		finalize_norm_computation<<<(*norm_count + (block_size-1)) / block_size, block_size>>>(batched_norms, *norm_count);
		checkCUDAError("finalize_norm_computation");

		cudaFree(keys_output);
		cudaFree(x_tmp);
}

void compute_batched_norms_with_keys_output(double* batched_norms, int* keys_output, int* norm_count, double* x, int m_total, thrust::device_ptr<int> work_item_map_ptr, int block_size)
{
		double* x_tmp;
		cudaMalloc((void**)&x_tmp, m_total*sizeof(double));
		thrust::device_ptr<double> x_tmp_ptr(x_tmp);
		thrust::device_ptr<int> keys_output_ptr(keys_output);
		thrust::device_ptr<double> batched_norms_ptr(batched_norms);

		// computing norms of batched vectors
		cudaMemcpy(x_tmp, x, m_total*sizeof(double), cudaMemcpyDeviceToDevice);
		thrust::transform(x_tmp_ptr, x_tmp_ptr+m_total, x_tmp_ptr, square());
		thrust::pair<thrust::device_ptr<int>, thrust::device_ptr<double> > new_end;
		new_end = thrust::reduce_by_key(work_item_map_ptr, work_item_map_ptr+m_total, x_tmp_ptr, keys_output_ptr, batched_norms_ptr, thrust::equal_to<int>(), thrust::plus<double>());

		// output_set_count is NOT equal to mat_mat_vec_data_count, since invalid entries are discarded
		*norm_count = new_end.first - keys_output_ptr;

		finalize_norm_computation<<<(*norm_count + (block_size-1)) / block_size, block_size>>>(batched_norms, *norm_count);
		checkCUDAError("finalize_norm_computation");

		cudaFree(x_tmp);
}

void compute_batched_products_for_kxk_matrices(double* batched_products, int* products_count, double* C, double* D, int m_total, thrust::device_ptr<int> work_item_map_ptr, int block_size, bool* stop_aca_as_soon_as_possible)
{
		double* x_tmp;
		int* keys_output;
		cudaMalloc((void**)&x_tmp, m_total*sizeof(double));
		cudaMalloc((void**)&keys_output, m_total*sizeof(int));
		thrust::device_ptr<double> x_tmp_ptr(x_tmp);
		thrust::device_ptr<int> keys_output_ptr(keys_output);
		thrust::device_ptr<double> batched_products_ptr(batched_products);
		thrust::device_ptr<double> C_ptr(C);
		thrust::device_ptr<double> D_ptr(D);
		thrust::device_ptr<bool> stop_aca_as_soon_as_possible_ptr(stop_aca_as_soon_as_possible);

//		double* batched_products_old;
//		cudaMalloc((void**)&batched_products_old, batch_count*sizeof(bool));
//		cudaMemcpy(batched_products_old, batched_products, batch_count*sizeof(bool), cudaMemcpyDeviceToDevice);
//
		// do pointwise product between C and D matrices
		thrust::transform(C_ptr,C_ptr+m_total,D_ptr,x_tmp_ptr, thrust::multiplies<double>());

		// computing batched products
		thrust::pair<thrust::device_ptr<int>, thrust::device_ptr<double> > new_end;
		new_end = thrust::reduce_by_key(work_item_map_ptr, work_item_map_ptr+m_total, x_tmp_ptr, keys_output_ptr, batched_products_ptr, thrust::equal_to<int>(), thrust::plus<double>());

		// output_set_count is NOT equal to mat_mat_vec_data_count, since invalid entries are discarded
		*products_count = new_end.first - keys_output_ptr;

		thrust::replace_if(batched_products_ptr, batched_products_ptr+*products_count, stop_aca_as_soon_as_possible_ptr, thrust::identity<bool>(), 1.0/0.0);

		finalize_norm_computation<<<(*products_count + (block_size-1)) / block_size, block_size>>>(batched_products, *products_count);
		checkCUDAError("finalize_norm_computation");

		cudaFree(keys_output);
		cudaFree(x_tmp);
}

void apply_batched_aca(double* x, double* y, struct work_item* mat_vec_data, int mat_vec_data_count, struct point_set* input_set1, struct point_set* input_set2, int vector_size, cublasStatus_t stat, cublasHandle_t handle, double eta, double epsilon, int k)
{
	int block_size = 512;

//	TIME_ssstart;

	int* l1;
	int* u1;
	cudaMalloc((void**)&l1, mat_vec_data_count*sizeof(int));
	cudaMalloc((void**)&u1, mat_vec_data_count*sizeof(int));
	thrust::device_ptr<int> l1_ptr(l1);
	thrust::device_ptr<int> u1_ptr(u1);

	int* l2;
	int* u2;
	cudaMalloc((void**)&l2, mat_vec_data_count*sizeof(int));
	cudaMalloc((void**)&u2, mat_vec_data_count*sizeof(int));
	thrust::device_ptr<int> l2_ptr(l2);
	thrust::device_ptr<int> u2_ptr(u2);

	get_work_item_point_set_limits_for_given_type<<<(mat_vec_data_count + (block_size - 1)) / block_size, block_size>>>(l1, u1, WT_ACA, mat_vec_data, mat_vec_data_count, 1);
	cudaThreadSynchronize();
	checkCUDAError("get_work_item_point_set_limits");

	get_work_item_point_set_limits_for_given_type<<<(mat_vec_data_count + (block_size - 1)) / block_size, block_size>>>(l2, u2, WT_ACA, mat_vec_data, mat_vec_data_count, 2);
	cudaThreadSynchronize();
	checkCUDAError("get_work_item_point_set_limits");

	int* m1;
	int* m2;
	cudaMalloc((void**)&m1, mat_vec_data_count*sizeof(int));
	cudaMalloc((void**)&m2, mat_vec_data_count*sizeof(int));
	thrust::device_ptr<int> m1_ptr(m1);
	thrust::device_ptr<int> m2_ptr(m2);

	// getting matrix sizes
	thrust::transform(u1_ptr, u1_ptr+mat_vec_data_count, l1_ptr, m1_ptr, minus_plus_1()); // numbers of rows
	thrust::transform(u2_ptr, u2_ptr+mat_vec_data_count, l2_ptr, m2_ptr, minus_plus_1()); // numbers of columns

	// l1, u1, l2, u2 are no longer needed
	cudaFree(l1);
	cudaFree(l2);
	cudaFree(u1);
	cudaFree(u2);


	int* k_per_item;
	cudaMalloc((void**)&k_per_item, mat_vec_data_count*sizeof(int));
	thrust::device_ptr<int> k_per_item_ptr(k_per_item);

	// if (k>min(m,n))
	//     k= min(m,n);
	// end
	set_k_per_item<<<(mat_vec_data_count + (block_size - 1)) / block_size, block_size>>>(k_per_item, k, mat_vec_data_count, m1, m2);
	cudaThreadSynchronize();
	checkCUDAError("set_k_per_item");

	int m1_total;
	int m2_total;
	m1_total = thrust::reduce(m1_ptr, m1_ptr+mat_vec_data_count);
	m2_total = thrust::reduce(m2_ptr, m2_ptr+mat_vec_data_count);

	// set upper bound for global k
	int m1_max = thrust::reduce(m1_ptr, m1_ptr+mat_vec_data_count, 0, thrust::maximum<int>());
	int m2_max = thrust::reduce(m2_ptr, m2_ptr+mat_vec_data_count, 0, thrust::maximum<int>());
	if (k>min(m1_max, m2_max))
	{
		k = min(m1_max, m2_max);
	}

	double* U;
	cudaMalloc((void**)&U, m1_total*k*sizeof(double));  // m1_total*k is a bad upper bound, since some k's might be smaller
	checkCUDAError("cudaMalloc");
	double* V;
	cudaMalloc((void**)&V, m2_total*k*sizeof(double));  // m2_total*k is a bad upper bound, since some k's might be smaller
	checkCUDAError("cudaMalloc");

	thrust::device_ptr<double> U_ptr(U);
	thrust::device_ptr<double> V_ptr(V);

	// TODO: Fill this with nan -> algo should still work
	thrust::fill(U_ptr, U_ptr+m1_total*k, 0.0);
	thrust::fill(V_ptr, V_ptr+m2_total*k, 0.0);


//	TIME_ssstop("ACA init stuff");


//	TIME_ssstart;

	double* v_r;
	double* u_r;

	thrust::device_ptr<struct work_item> mat_vec_data_ptr(mat_vec_data);

	// i_r = 0;
//	int i_r = -1;

	struct divide_by div;

	// create mapping of work_items to offset in batched data
	int* point_map_offsets1;
	int* point_map_offsets2;
	cudaMalloc((void**)&point_map_offsets1, mat_vec_data_count*sizeof(int));
	cudaMalloc((void**)&point_map_offsets2, mat_vec_data_count*sizeof(int));
	thrust::device_ptr<int> point_map_offsets1_ptr(point_map_offsets1);
	thrust::device_ptr<int> point_map_offsets2_ptr(point_map_offsets2);
	thrust::exclusive_scan(m1_ptr, m1_ptr+mat_vec_data_count, point_map_offsets1_ptr, 0);
	thrust::exclusive_scan(m2_ptr, m2_ptr+mat_vec_data_count, point_map_offsets2_ptr, 0);

	//--------------------------------------------------------------
	// compute mapping of batch data entries to global point indices
	//--------------------------------------------------------------
	int* point_map1; // map of rows of U to point indices in point_set1
	int* point_map2; // map of rows of V to point indices in point_set2
	cudaMalloc((void**)&point_map1, m1_total*sizeof(int));
	cudaMalloc((void**)&point_map2, m2_total*sizeof(int));
	thrust::device_ptr<int> point_map1_ptr(point_map1);
	thrust::device_ptr<int> point_map2_ptr(point_map2);

	// start with one's
	thrust::fill(point_map1_ptr, point_map1_ptr+m1_total, 1);
	thrust::fill(point_map2_ptr, point_map2_ptr+m2_total, 1);

	// set index bounds to l, -(u-1)
	// 0  0  2  1 -3  0  0  5  1  1  1 -8  2  1  1 -4  0
	set_bounds_for_point_maps<<<(mat_vec_data_count + (block_size - 1)) / block_size, block_size>>>(point_map1, point_map2, point_map_offsets1, point_map_offsets2, m1, m2, WT_ACA, mat_vec_data, mat_vec_data_count);

	// use inclusive scan to generate index map
	// 0  0  2  3  0  0  0  5  6  7  8  0  2  3  4  0  0
	thrust::inclusive_scan(point_map1_ptr, point_map1_ptr+m1_total, point_map1_ptr);
	thrust::inclusive_scan(point_map2_ptr, point_map2_ptr+m2_total, point_map2_ptr);

	// correct upper bounds
	// 0  0  2  3  4  0  0  5  6  7  8  9  2  3  4  5  0
	correct_bounds_for_point_maps<<<(mat_vec_data_count + (block_size - 1)) / block_size, block_size>>>(point_map1, point_map2, point_map_offsets1, point_map_offsets2, m1, m2, WT_ACA, mat_vec_data, mat_vec_data_count);

	int* work_item_map1; // map of rows of U to work item indices in mat_vec_data
	int* work_item_map2; // map of rows of V to work item indices in mat_vec_data
	cudaMalloc((void**)&work_item_map1, m1_total*sizeof(int));
	cudaMalloc((void**)&work_item_map2, m2_total*sizeof(int));
	thrust::device_ptr<int> work_item_map1_ptr(work_item_map1);
	thrust::device_ptr<int> work_item_map2_ptr(work_item_map2);

	// set maps to zero
	// 0  0  0  0  0  0  0  0  0  0  0  0  0  0  0  0  0
	thrust::fill(work_item_map1_ptr, work_item_map1_ptr+m1_total, 0);
	thrust::fill(work_item_map2_ptr, work_item_map2_ptr+m2_total, 0);



	// set bounds for the back mapping of rows to work_items
	// 0  0  2  0 -2  0  0  3  0  0  0 -3  1  0  0 -1  0
	set_bounds_for_work_item_maps<<<(mat_vec_data_count + (block_size - 1)) / block_size, block_size>>>(work_item_map1, work_item_map2, point_map_offsets1, point_map_offsets2, m1, m2, WT_ACA, mat_vec_data, mat_vec_data_count);

	// fill gaps
	// 0  0  2  2  0  0  0  3  3  3  3  0  1  1  1  0  0
	thrust::inclusive_scan(work_item_map1_ptr, work_item_map1_ptr+m1_total, work_item_map1_ptr);
	thrust::inclusive_scan(work_item_map2_ptr, work_item_map2_ptr+m2_total, work_item_map2_ptr);

	// correct upper bounds
	// 0  0  2  2  2  0  0  3  3  3  3  3  1  1  1  1  0
	correct_bounds_for_work_item_maps<<<(mat_vec_data_count + (block_size - 1)) / block_size, block_size>>>(work_item_map1, work_item_map2, point_map_offsets1, point_map_offsets2, m1, m2, WT_ACA, mat_vec_data, mat_vec_data_count);



	// ------------------------------------------------------------------------------------------------------------
	// creating map between work item list (including invalid entries) and batch set list (without invalid entries)
	// ------------------------------------------------------------------------------------------------------------
	int* work_item_to_batch_map;
	cudaMalloc((void**)&work_item_to_batch_map, mat_vec_data_count*sizeof(int));
	thrust::device_ptr<int> work_item_to_batch_map_ptr(work_item_to_batch_map);

	int* tmp_field;
	cudaMalloc((void**)&tmp_field, mat_vec_data_count*sizeof(int));
	thrust::device_ptr<int> tmp_field_ptr(tmp_field);

	// fill tmp_field with sequence
	//              tmp_field =  0  1  2  3  4  5
	thrust::sequence(tmp_field_ptr, tmp_field_ptr+mat_vec_data_count);

	// remove invalid entries
	//              tmp_field =  1  2  4
	thrust::device_ptr<int> end_after_removal;
	end_after_removal = thrust::remove_if(tmp_field_ptr, tmp_field_ptr+mat_vec_data_count, mat_vec_data_ptr, is_not_WT_ACA());
	int batch_count = end_after_removal-tmp_field_ptr;

	// set map to -1
	// work_item_to_batch_map = -1 -1 -1 -1 -1 -1
	thrust::fill(work_item_to_batch_map_ptr, work_item_to_batch_map_ptr+mat_vec_data_count, -1);

	// scatter sequence to appropriate positions (indicated by tmp_field)
	// work_item_to_batch_map = -1  0  1 -1  2 -1
	thrust::scatter(thrust::make_counting_iterator(0), thrust::make_counting_iterator(batch_count), tmp_field_ptr, work_item_to_batch_map_ptr);
	cudaFree(tmp_field);


	int* i_r;
	cudaMalloc((void**)&i_r, mat_vec_data_count*sizeof(int));
	thrust::device_ptr<int> i_r_ptr(i_r);

	thrust::fill(i_r_ptr, i_r_ptr+mat_vec_data_count, -1);
	// on invalid entries (in mat_vec_data) we shall never compute
	thrust::replace_if(i_r_ptr, i_r_ptr+mat_vec_data_count, mat_vec_data_ptr, is_not_WT_ACA(), -1);

	int* compute_v_r;
	cudaMalloc((void**)&compute_v_r, mat_vec_data_count*sizeof(int));
	thrust::device_ptr<int> compute_v_r_ptr(compute_v_r);

//	TIME_ssstop("ACA create mappings etc.");

//	TIME_ssstart;

    cudaStream_t *streams = new cudaStream_t[batch_count];
    for(int b=0; b<batch_count; b++)
        cudaStreamCreate(&streams[b]);

	int* m2_h;
	m2_h = new int[mat_vec_data_count];
	cudaMemcpy(m2_h, m2, mat_vec_data_count*sizeof(int), cudaMemcpyDeviceToHost);

	int* m1_h;
	m1_h = new int[mat_vec_data_count];
	cudaMemcpy(m1_h, m1, mat_vec_data_count*sizeof(int), cudaMemcpyDeviceToHost);

	int* point_map_offsets2_h;
	point_map_offsets2_h = new int[mat_vec_data_count];
	cudaMemcpy(point_map_offsets2_h, point_map_offsets2, mat_vec_data_count*sizeof(int), cudaMemcpyDeviceToHost);

	int* point_map_offsets1_h;
	point_map_offsets1_h = new int[mat_vec_data_count];
	cudaMemcpy(point_map_offsets1_h, point_map_offsets1, mat_vec_data_count*sizeof(int), cudaMemcpyDeviceToHost);

	bool* stop_aca_as_soon_as_possible;
	cudaMalloc((void**)&stop_aca_as_soon_as_possible, batch_count*sizeof(bool));
	thrust::device_ptr<bool> stop_aca_as_soon_as_possible_ptr(stop_aca_as_soon_as_possible);

	thrust::fill(stop_aca_as_soon_as_possible_ptr, stop_aca_as_soon_as_possible_ptr+batch_count, false);

	bool* stop_aca_as_soon_as_possible_h;
	stop_aca_as_soon_as_possible_h = new bool[batch_count];
	cudaMemcpy(stop_aca_as_soon_as_possible_h, stop_aca_as_soon_as_possible, batch_count*sizeof(bool), cudaMemcpyDeviceToHost);

//	TIME_ssstop("ACA streams, copy stuff");

	// for r=1:k
	for (int r=0; r<k; r++)
	{
		// while (norm(v_tilde_r,Inf)==0.0)
	    //    i_r = i_r+1;
	    //    v_tilde_r = kernel(input_set1(i_r,:), input_set2);
	    //    for l=1:r-1
	    //        v_tilde_r = v_tilde_r - U(i_r,l) * V(l,:);
	    //    end
	    // end

//		TIME_ssstart;

        // U = [U u_r];
        // V = [V; v_r];
		v_r = &V[r*m2_total];
		u_r = &U[r*m1_total];
		thrust::device_ptr<double> u_r_ptr(u_r);
		thrust::device_ptr<double> v_r_ptr(v_r);

		int* keys_output;
		double* v_r_norms;
		cudaMalloc((void**)&keys_output, m2_total*sizeof(int));
		cudaMalloc((void**)&v_r_norms, m2_total*sizeof(double));
		thrust::device_ptr<int> keys_output_ptr(keys_output);
		thrust::device_ptr<double> v_r_norms_ptr(v_r_norms);



		// compute on all valid entries at the beginning
		thrust::fill(compute_v_r_ptr, compute_v_r_ptr+mat_vec_data_count, 1);
		thrust::replace_if(compute_v_r_ptr, compute_v_r_ptr+mat_vec_data_count, mat_vec_data_ptr, is_not_WT_ACA(), 0);

		is_smaller_or_equal_r ser(r);

		thrust::replace_if(compute_v_r_ptr, compute_v_r_ptr+mat_vec_data_count, k_per_item_ptr, ser, 0);

//		TIME_ssstop("ACA beginning of r loop");

//		TIME_ssstart;

		while (true)
		{
			// increase i_r entry for all elements on which we shall still compute
			thrust::transform_if(i_r_ptr, i_r_ptr+mat_vec_data_count, compute_v_r_ptr, i_r_ptr, add_one(), is_one());

//			//    v_tilde_r = kernel(input_set1(i_r,:), input_set2);
//			batched_fill_kernel_vector_v_r<<<(m2_total + (block_size - 1)) / block_size, block_size>>>(v_r, point_map2, point_map1, point_map_offsets1, work_item_map2, i_r, compute_v_r, input_set2, input_set1, m2_total);
//			cudaThreadSynchronize();
//			checkCUDAError("fill_kernel_vector1");
//
//			//    for l=1:r-1
//		    //        v_tilde_r = v_tilde_r - U(i_r,l) * V(l,:);
//		    //    end
//			batched_scaled_substraction_for_v_r<<<(m2_total + (block_size - 1)) / block_size, block_size>>>(v_r, point_map2, point_map_offsets1, work_item_map2, i_r, compute_v_r, V, U, input_set2, input_set1, k_per_item, r, m2_total, m1_total);
//			cudaThreadSynchronize();
//			checkCUDAError("batched_scaled_substraction_of_vectors");
//
			batched_fill_kernel_vector_and_scaled_substraction_for_v_r<<<(m2_total + (block_size - 1)) / block_size, block_size>>>(v_r, point_map2, point_map1, point_map_offsets1, work_item_map2, i_r, compute_v_r, input_set2, input_set1, m2_total, m1_total, V, U, r, k_per_item);
			cudaThreadSynchronize();
			checkCUDAError("__batched_fill_kernel_vector_v_r");


//			// computing norms of batched vectors (to check whether to increase i_r)
			int v_r_norms_count;
			compute_batched_norms_with_keys_output(v_r_norms, keys_output, &v_r_norms_count, v_r, m2_total, work_item_map2_ptr, block_size);

// there are no invalid entries by construction
//			// remove potential rubbish in invalid entries
//			thrust::replace_if(v_r_norms_ptr, v_r_norms_ptr+output_set_counts, keys_output_ptr, is_minus_one(), 0.0);

			remove_rubbish_from_maxima<<<(v_r_norms_count + (block_size-1)) / block_size, block_size>>>(i_r, compute_v_r, mat_vec_data_count, keys_output, v_r_norms, v_r_norms_count);
			cudaThreadSynchronize();
			checkCUDAError("remove_rubbish_from_maxima");

			// if a work_item has finished, set respective compute_v_r entry to 0
			update_ir<<<(v_r_norms_count + (block_size-1)) / block_size, block_size>>>(i_r, compute_v_r, mat_vec_data_count, keys_output, v_r_norms, v_r_norms_count);
			cudaThreadSynchronize();
			checkCUDAError("update_ir");

			int max_of_compute_v_r;
			max_of_compute_v_r = thrust::reduce(compute_v_r_ptr, compute_v_r_ptr+mat_vec_data_count, 0, thrust::maximum<int>());

//			print_int(compute_v_r, mat_vec_data_count);
//			printf("max: %d\n", max_of_compute_v_r);

			if (max_of_compute_v_r == 0)
				break;
//			// stop iteration when no v_r is (almost) zero
//			if (thrust::all_of(v_r_norms_ptr, v_r_norms_ptr+output_set_counts, bigger_than_eps()))
//				break;

		} //while (sqrt(thrust::inner_product(v_r_ptr, v_r_ptr+m2, v_r_ptr, 0.0))<1.0e-14);

//		TIME_ssstop("ACA v_r computation loop");

//		TIME_ssstart;

		cudaFree(keys_output);
		checkCUDAError("cudaFree");
		cudaFree(v_r_norms);
		checkCUDAError("cudaFree");

		//// [m,j_r] = max(abs(v_tilde_r));
		//thrust::device_ptr<double> max_pos = thrust::max_element(v_r_ptr, v_r_ptr+m2, compare_absolute());
		//int j_r = max_pos - v_r_ptr;

//		TIME_ssstop("ACA u_r computation 1");
//		TIME_ssstart;

		int* j_r_global; // j_r index (maximum positions) as global indices in the batched vector
		cudaMalloc((void**)&j_r_global, mat_vec_data_count*sizeof(int));  // mat_vec_data_count is an upper bound to the actual amount of batches
		checkCUDAError("cudaMalloc");
		thrust::device_ptr<int> j_r_global_ptr(j_r_global);

		double* maximum_values;
		cudaMalloc((void**)&maximum_values, mat_vec_data_count*sizeof(double));
		checkCUDAError("cudaMalloc");
		thrust::device_ptr<double> maximum_values_ptr(maximum_values);

		int* batch_to_work_item_map; // maps batch set to work item number
		cudaMalloc((void**)&batch_to_work_item_map, mat_vec_data_count*sizeof(double));  // mat_vec_data_count is just an upper bound
		checkCUDAError("cudaMalloc");
		thrust::device_ptr<int> batch_to_work_item_map_ptr(batch_to_work_item_map);

//		TIME_ssstop("ACA u_r computation 2");
//		TIME_ssstart;

		// allocate and fill "indices" with 0, 1, 2, ...
		int* indices;
		cudaMalloc((void**)&indices, m2_total*sizeof(double));
		checkCUDAError("cudaMalloc");
		thrust::device_ptr<int> indices_ptr(indices);
		thrust::sequence(indices_ptr, indices_ptr+m2_total);

		// compute block-wise maximum and maximum positions at the same time
		thrust::pair<thrust::device_ptr<int>, thrust::zip_iterator<thrust::tuple<thrust::device_ptr<double>,thrust::device_ptr<int> > > > new_end2;
		new_end2 = thrust::reduce_by_key(work_item_map2_ptr, work_item_map2_ptr+m2_total, thrust::make_zip_iterator(thrust::make_tuple(v_r_ptr, indices_ptr)), batch_to_work_item_map_ptr, thrust::make_zip_iterator(thrust::make_tuple(maximum_values_ptr, j_r_global_ptr)), thrust::equal_to<int>(), tuple_absolute_maximum());

		// ATTENTION: In the following, I assume that the output size is identical to mat_vec_data_count, which is mandatory!!!

		cudaFree(maximum_values);
		cudaFree(batch_to_work_item_map);

//		TIME_ssstop("ACA u_r computation 3");
//		TIME_ssstart;

// not necessary since they all are valid by construction
//		// invalidate j_r_global entries in case they belong to invalid work items
//		thrust::replace_if(j_r_global_ptr, j_r_global_ptr+mat_vec_data_count, out_keys_ptr, is_minus_one(), -1);


	    // v_r = (1.0./(v_tilde_k(j_r)))*v_tilde_r;
		batched_scaling_of_v_r<<<(m2_total + (block_size-1)) / block_size, block_size>>>(v_r, work_item_to_batch_map, work_item_map2, k_per_item, r, j_r_global, m2_total);
		cudaThreadSynchronize();
		checkCUDAError("batched_scaling_of_v_r");

	    // u_r = kernel(input_set1(:,:),input_set2(j_r,:));
	    // for l=1:r-1
	    //     u_r = u_r - V(l,j_r) * U(:,l);
	    // end


//		TIME_ssstop("ACA u_r computation 4");
//		TIME_ssstart;

//		batched_fill_kernel_vector_u_r<<<(m1_total + (block_size-1)) / block_size, block_size>>>(u_r, point_map1, point_map2, work_item_to_batch_map, work_item_map1, k_per_item, r, j_r_global, input_set1, input_set2, m1_total);
//		cudaThreadSynchronize();
//		checkCUDAError("batched_fill_kernel_vector_u_r");
//		batched_scaled_substraction_for_u_r<<<(m1_total + (block_size-1)) / block_size, block_size>>>(u_r, point_map1, work_item_to_batch_map, work_item_map1, j_r_global, U, V, input_set1, input_set2, k_per_item, r, m1_total, m2_total);
//		cudaThreadSynchronize();
//		checkCUDAError("batched_scaled_substraction_for_u_r");

		batched_fill_kernel_vector_and_scaled_substraction_for_u_r<<<(m1_total + (block_size-1)) / block_size, block_size>>>(u_r, point_map1, point_map2, work_item_to_batch_map, work_item_map1, k_per_item, r, j_r_global, input_set1, input_set2, m1_total, m2_total, U, V);
		cudaThreadSynchronize();
		checkCUDAError("__batched_fill_kernel_vector_u_r");


		cudaFree(j_r_global);


//		TIME_ssstop("ACA u_r computation 5");

		bool check_frobenius = false;

		if (check_frobenius && (r%5==0))
		{

//			TIME_ssstart;


			int current_batch = 0;

			// ATTENTION: C, D will have a different memory layout than U,V
			// U,V  [batch1_column1, batch2_column1, ... , batch1_column2, batch2_column2, ...]
			// C,D  [batch1_column1, batch1_column2, ... , batch2_column1, batch2_column2, ...]
			double* C;
			cudaMalloc((void**)&C, batch_count*(r+1)*(r+1)*sizeof(double));

			double* D;
			cudaMalloc((void**)&D, batch_count*(r+1)*(r+1)*sizeof(double));

			double one;
			double zero;
			one = 1.0;
			zero = 0.0;


			for (int s=0; s<mat_vec_data_count; s++)
			{
				if ((m2_h[s]>0)&&(m1_h[s]>0)) // check whether current work item is valid
				{
					if (!stop_aca_as_soon_as_possible_h[current_batch])
					{
						cublasSetStream(handle, streams[current_batch]);

						// C = U'*U
						cublasDgemm(handle, CUBLAS_OP_T, CUBLAS_OP_N, (r+1), (r+1), m1_h[s], &one, &U[point_map_offsets1_h[s]], m1_total, &U[point_map_offsets1_h[s]], m1_total, &zero, &C[current_batch*(r+1)*(r+1)], (r+1));

						// D = V'*V
						cublasDgemm(handle, CUBLAS_OP_T, CUBLAS_OP_N, (r+1), (r+1), m2_h[s], &one, &V[point_map_offsets2_h[s]], m2_total, &V[point_map_offsets2_h[s]], m2_total, &zero, &D[current_batch*(r+1)*(r+1)], (r+1));
					}

					current_batch++;
				}
			}		// res = C(:)'*D(:)

			cublasSetStream(handle, 0);

//			TIME_ssstop("ACA Frobenius C,D computation");
//
//			TIME_ssstart;

			double* res;
			cudaMalloc((void**)&res, sizeof(double)*batch_count);
			thrust::device_ptr<double> res_ptr(res);
			double* res_h = new double[batch_count];

			double* u_r_2norm;
			double* v_r_2norm;
			cudaMalloc((void**)&u_r_2norm, sizeof(double)*batch_count);
			cudaMalloc((void**)&v_r_2norm, sizeof(double)*batch_count);
			thrust::device_ptr<double> u_r_2norm_ptr(u_r_2norm);
			thrust::device_ptr<double> v_r_2norm_ptr(v_r_2norm);
			double* u_r_2norm_h = new double[batch_count];
			double* v_r_2norm_h = new double[batch_count];


			// ------------------------------------------------------------
			// construct work map for batched dot product of k x k matrices
			// ------------------------------------------------------------
			int* offsets_of_kxk_matrices;
			int* ones_to_scatter;
			int* work_item_map_for_kxk_products;
			cudaMalloc((void**)&offsets_of_kxk_matrices, sizeof(int)*batch_count);
			cudaMalloc((void**)&ones_to_scatter, sizeof(int)*batch_count);
			cudaMalloc((void**)&work_item_map_for_kxk_products, sizeof(int)*batch_count*(r+1)*(r+1));
			thrust::device_ptr<int> offsets_of_kxk_matrices_ptr(offsets_of_kxk_matrices);
			thrust::device_ptr<int> ones_to_scatter_ptr(ones_to_scatter);
			thrust::device_ptr<int> work_item_map_for_kxk_products_ptr(work_item_map_for_kxk_products);

			// compute offsets of the kxk matrix blocks
			thrust::sequence(offsets_of_kxk_matrices_ptr, offsets_of_kxk_matrices_ptr+batch_count, 0, (r+1)*(r+1));

			// fill array with ones to scatter
			thrust::fill(ones_to_scatter_ptr, ones_to_scatter_ptr+batch_count, 1);

			// fill work_item_map with zeros
			thrust::fill(work_item_map_for_kxk_products_ptr, work_item_map_for_kxk_products_ptr+batch_count*(r+1)*(r+1), 0);

			// scatter ones to beginnings of kxk matrix blocks
			thrust::scatter(ones_to_scatter_ptr, ones_to_scatter_ptr+batch_count, offsets_of_kxk_matrices_ptr, work_item_map_for_kxk_products_ptr);

			// create pattern of the form 11112222333344445555666677778888...
			thrust::inclusive_scan(work_item_map_for_kxk_products_ptr, work_item_map_for_kxk_products_ptr+batch_count*(r+1)*(r+1), work_item_map_for_kxk_products_ptr);

			cudaFree(offsets_of_kxk_matrices);
			cudaFree(ones_to_scatter);
			// ------------------------------------------------------------

			// compute Frobenius norms of kxk matrices
			int kxk_products_count;
			compute_batched_products_for_kxk_matrices(res, &kxk_products_count, C, D, batch_count*(r+1)*(r+1), work_item_map_for_kxk_products_ptr, block_size, stop_aca_as_soon_as_possible);

			cudaFree(work_item_map_for_kxk_products);

//			TIME_ssstop("ACA Frobenius norm_eval (part 1)");
//			TIME_ssstart;

			int norm_count;
			compute_batched_norms(u_r_2norm, &norm_count, u_r, m1_total, work_item_map1_ptr, block_size);

			if (norm_count!=batch_count)
			{
				printf("Exiting: norm_count=%d, batch_count=%d\n", norm_count, batch_count);
				exit(1);
			}

			compute_batched_norms(v_r_2norm, &norm_count, v_r, m2_total, work_item_map2_ptr, block_size);

//			TIME_ssstop("ACA Frobenius norm_eval (part 2)");
//			TIME_ssstart;

			cudaFree(C);
			cudaFree(D);

			//		if (u_r_2norm*v_r_2norm <= ((1.0e-8*(1.0-eta))/(1.0+1.0e-8))*res)
			//		{
			//			//			printf("AAAAUUUUFFFHÖÖÖÖÖRRREEENNNN!!!!!! Schluss jetzt!\n");
			//			printf("r=%d\n", r);
			//			break;
			//		}
			thrust::transform(u_r_2norm_ptr, u_r_2norm_ptr+batch_count, v_r_2norm_ptr, u_r_2norm_ptr, thrust::multiplies<double>());
			thrust::transform(u_r_2norm_ptr, u_r_2norm_ptr+batch_count, res_ptr, u_r_2norm_ptr, thrust::divides<double>());
			thrust::replace_if(stop_aca_as_soon_as_possible_ptr, stop_aca_as_soon_as_possible_ptr+batch_count, u_r_2norm_ptr, is_smaller(((epsilon*(1.0-eta))/(1.0+epsilon))), true);

			cudaMemcpy(stop_aca_as_soon_as_possible_h, stop_aca_as_soon_as_possible, batch_count*sizeof(bool), cudaMemcpyDeviceToHost);

			cudaFree(u_r_2norm);
			cudaFree(v_r_2norm);
			cudaFree(res);


			//				cublasDgemv(handle, CUBLAS_OP_T, m2_h[s], k_per_item_h[s], &one, &V[point_map_offsets2_h[s]], m2_total, &local_x[point_map_offsets2_h[s]], 1, &zero, &local_tmp[current_batch*k], 1);
			//				cublasDgemv(handle, CUBLAS_OP_N, m1_h[s], k_per_item_h[s], &one, &U[point_map_offsets1_h[s]], m1_total, &local_tmp[k*current_batch], 1, &zero, &local_y[point_map_offsets1_h[s]], 1);
			//			cublasDgemv(handle, CUBLAS_OP_T, m2_h[s], k, &one, &V[point_map_offsets2_h[s]], m2_total, &local_x[point_map_offsets2_h[s]], 1, &zero, &local_tmp[current_batch*k], 1);
			//			cublasDgemv(handle, CUBLAS_OP_N, m1_h[s], k, &one, &U[point_map_offsets1_h[s]], m1_total, &local_tmp[k*current_batch], 1, &zero, &local_y[point_map_offsets1_h[s]], 1);


//			TIME_ssstop("ACA Frobenius rest");

			bool stop = thrust::all_of(stop_aca_as_soon_as_possible_ptr, stop_aca_as_soon_as_possible_ptr+batch_count, thrust::identity<bool>());

			if (stop)
			{
				//			printf("r %d\n", r);
				break;
			}

		}
	}

//	TIME_ssstart;

	cudaFree(work_item_to_batch_map);
	cudaFree(i_r);
	cudaFree(compute_v_r);

	cudaFree(stop_aca_as_soon_as_possible);
	delete [] stop_aca_as_soon_as_possible_h;

	// allocation and extraction of batched local operands
	double* local_x;
	cudaMalloc((void**)&local_x, m2_total*sizeof(double));
	checkCUDAError("cudaMalloc");
//	cudaMemcpy(local_x, &x[current_mat_vec_data.set2_l], m2*sizeof(double), cudaMemcpyDeviceToDevice);
	thrust::device_ptr<double> local_x_ptr(local_x);
	thrust::device_ptr<double> x_ptr(x);
	thrust::gather(point_map2_ptr, point_map2_ptr+m2_total, x_ptr, local_x_ptr);


	// allocation of batched local intermediate results
	double* local_tmp;
	cudaMalloc((void**)&local_tmp, batch_count*k*sizeof(double));
	checkCUDAError("cudaMalloc");

	// allocation of batched local results
	double* local_y;
	cudaMalloc((void**)&local_y, m1_total*sizeof(double));
	checkCUDAError("cudaMalloc");



	// low-rank matrix-vector-product
	double one;
	double zero;
	one = 1.0;
	zero = 0.0;


	int* k_per_item_h;
	k_per_item_h = new int[mat_vec_data_count];
	cudaMemcpy(k_per_item_h, k_per_item, mat_vec_data_count*sizeof(int), cudaMemcpyDeviceToHost);

	int current_batch = 0;

	for (int s=0; s<mat_vec_data_count; s++)
	{
		if ((m2_h[s]>0)&&(m1_h[s]>0)) // check whether current work item is valid
		{
			cublasSetStream(handle, streams[current_batch]);

			cublasDgemv(handle, CUBLAS_OP_T, m2_h[s], k_per_item_h[s], &one, &V[point_map_offsets2_h[s]], m2_total, &local_x[point_map_offsets2_h[s]], 1, &zero, &local_tmp[current_batch*k], 1);
			cublasDgemv(handle, CUBLAS_OP_N, m1_h[s], k_per_item_h[s], &one, &U[point_map_offsets1_h[s]], m1_total, &local_tmp[k*current_batch], 1, &zero, &local_y[point_map_offsets1_h[s]], 1);
//			cublasDgemv(handle, CUBLAS_OP_T, m2_h[s], k, &one, &V[point_map_offsets2_h[s]], m2_total, &local_x[point_map_offsets2_h[s]], 1, &zero, &local_tmp[current_batch*k], 1);
//			cublasDgemv(handle, CUBLAS_OP_N, m1_h[s], k, &one, &U[point_map_offsets1_h[s]], m1_total, &local_tmp[k*current_batch], 1, &zero, &local_y[point_map_offsets1_h[s]], 1);

			current_batch++;
		}
	}

	cublasSetStream(handle, 0);
	for (int b=0; b<batch_count; b++)
		cudaStreamDestroy(streams[b]);
	delete[] streams;
	delete[] m1_h;
	delete[] m2_h;
	delete[] point_map_offsets1_h;
	delete[] point_map_offsets2_h;
	delete[] k_per_item_h;


	thrust::device_ptr<double> local_y_ptr(local_y);
	thrust::device_ptr<double> y_ptr(y);

	// adding batched local results to full vector
	//thrust::transform(y_ptr+current_mat_vec_data.set1_l, y_ptr+current_mat_vec_data.set1_l+m1, local_y_ptr, y_ptr+current_mat_vec_data.set1_l, thrust::plus<double>());
	add_batched_local_results_to_full_vector<<<(m1_total + (block_size - 1)) / block_size, block_size>>>(y, local_y, point_map1, work_item_map1, m1_total);

//	TIME_ssstop("ACA apply");

	cudaFree(local_x);
	cudaFree(local_y);
	cudaFree(local_tmp);
	cudaFree(U);
	cudaFree(V);
	cudaFree(m1);
	cudaFree(m2);
	cudaFree(k_per_item);
	cudaFree(point_map_offsets1);
	cudaFree(point_map_offsets2);
	cudaFree(point_map1);
	cudaFree(point_map2);
	cudaFree(work_item_map1);
	cudaFree(work_item_map2);
	checkCUDAError("cudaFrees a the end of batched ACA");

//	cudaFree(v_r);
//	cudaFree(u_r);

}


void sequential_h_matrix_mvp(double* x, double* y, struct work_item* mat_vec_data, int mat_vec_data_count, int mat_vec_data_array_size, struct point_set* input_set1, struct point_set* input_set2, int vector_size, double eta, double epsilon, int k)
{
    cublasStatus_t stat;
    cublasHandle_t handle;
    stat = cublasCreate(&handle);

	struct work_item current_mat_vec_data;

	// set output vector to zero

		// very dirty way to get point count and dimensionality of points
		int point_count,dim;
		int* point_count_d; cudaMalloc((void**)&point_count_d, sizeof(int));
		int* dim_d; cudaMalloc((void**)&dim_d, sizeof(int));
		get_point_count_dim<<<1,1>>>(point_count_d, dim_d, input_set1);
		cudaMemcpy(&point_count, point_count_d, sizeof(int), cudaMemcpyDeviceToHost);
		cudaMemcpy(&dim, dim_d, sizeof(int), cudaMemcpyDeviceToHost);
		cudaFree(point_count_d); cudaFree(dim_d);
		thrust::device_ptr<double> y_ptr(y);
		thrust::fill(y_ptr, y_ptr+point_count, 0.0);

	sort_mat_vec_data(mat_vec_data, mat_vec_data_count);


//	printf("y_before\n");
//	print_double(y, vector_size);
//

	TIME_ssstart;

	for (int i=0; i<mat_vec_data_count; i++)
	{
		// get current work item to handle
		cudaMemcpy(&current_mat_vec_data, &mat_vec_data[i], sizeof(struct work_item), cudaMemcpyDeviceToHost);

		// handling of dense blocks
		if (current_mat_vec_data.work_type==WT_DENSE)
		{
			apply_dense_matrix_for_current_work_item(x, y, current_mat_vec_data, input_set1, input_set2, vector_size, stat, handle);
		}  // handling of low rank blocks
/*		else if (current_mat_vec_data.work_type==WT_ACA)
		{
			apply_aca_for_current_work_item(x, y, current_mat_vec_data, input_set1, input_set2, vector_size, stat, handle, k);
		}
		else
		{
			printf("Error: Invalid work type. Exiting...\n");
			exit(1);
		}*/
	}
	TIME_ssstop("dense blocks");

	TIME_ssstart;

	thrust::device_ptr<struct work_item> mat_vec_data_ptr(mat_vec_data);
	thrust::device_ptr<struct work_item> dense_end_ptr = thrust::partition_point(mat_vec_data_ptr, mat_vec_data_ptr+mat_vec_data_count, is_not_WT_ACA());

//	int work_size = 16;
//
	int dense_count = dense_end_ptr - mat_vec_data_ptr;
////	printf("dense count: %d\n", dense_count);
	int aca_count = mat_vec_data_count - dense_count;
////	printf("aca_count: %d\n", aca_count);
//
////	printf("bla: %d\n",(aca_count+work_size-1)/work_size);
//
//	for (int i=0; i<(aca_count+work_size-1)/work_size; i++)
//	{
//		int offset = dense_count + i*work_size;
//		int len;
//		if (i<((aca_count+work_size-1)/work_size)-1)
//			len = work_size;
//		else
//			len = aca_count-(((aca_count+work_size-1)/work_size)-1)*work_size;
////		printf("offset: %d  len: %d\n", offset, len);
//		apply_batched_aca(x, y, &mat_vec_data[offset], len, input_set1, input_set2, vector_size, stat, handle, eta, epsilon, k);
//	}
//

	apply_batched_aca(x, y, &mat_vec_data[dense_count], aca_count, input_set1, input_set2, vector_size, stat, handle, eta, epsilon, k);

////	apply_batched_aca(x, y, &mat_vec_data[3], 1, input_set1, input_set2, vector_size, stat, handle, eta, epsilon, k);
//

	TIME_ssstop("batched aca");

//	printf("y_aca_parallel\n");
//	print_double(y, vector_size);

//	double* y_parallel;
//	cudaMalloc((void**)&y_parallel, vector_size*sizeof(double));
//	thrust::device_ptr<double> y_parallel_ptr(y_parallel);
//	thrust::copy(y_ptr, y_ptr+vector_size, y_parallel_ptr);

//	thrust::fill(y_ptr, y_ptr+vector_size, 0.0);


//	TIME_ssstart;
//	for (int i=0; i<mat_vec_data_count; i++)
//	{
////		printf("i: %d\n", i);
////		int i = 3;
//		// get current work item to handle
//		cudaMemcpy(&current_mat_vec_data, &mat_vec_data[i], sizeof(struct work_item), cudaMemcpyDeviceToHost);
//
///*		// handling of dense blocks
//		if (current_mat_vec_data.work_type==WT_DENSE)
//		{
//			apply_dense_matrix_for_current_work_item(x, y, current_mat_vec_data, input_set1, input_set2, vector_size, stat, handle);
//		}  // handling of low rank blocks
//		else */ if (current_mat_vec_data.work_type==WT_ACA)
//		{
////			printf("ACA: %d %d\n", current_mat_vec_data.set1_u-current_mat_vec_data.set1_l, current_mat_vec_data.set2_u-current_mat_vec_data.set2_l);
////			TIME_ssstart;
//			apply_aca_for_current_work_item(x, y, current_mat_vec_data, input_set1, input_set2, vector_size, stat, handle, eta, epsilon, k);
////			TIME_ssstop("aca first block");
////			break;
//		}/*
//		else
//		{
//			printf("Error: Invalid work type. Exiting...\n");
//			exit(1);
//		}*/
//	}
//	TIME_ssstop("sequential aca");

//	printf("y_sequential\n");
//	print_double(y, vector_size);

//	thrust::transform(y_ptr, y_ptr+vector_size, y_parallel_ptr, y_parallel_ptr, thrust::minus<double>());
//	double error = sqrt(thrust::inner_product(y_parallel_ptr, y_parallel_ptr+vector_size, y_parallel_ptr, 0.0)/(double)vector_size) \
//			/ sqrt(thrust::inner_product(y_ptr, y_ptr+vector_size, y_ptr, 0.0)/(double)vector_size);
//	printf("error: %le\n", error);

    cublasDestroy(handle);
}


#endif /* LINEAR_ALGEBRA_H_ */
