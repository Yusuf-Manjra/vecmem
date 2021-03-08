/** VecMem project, part of the ACTS project (R&D line)
 *
 * (c) 2021 CERN for the benefit of the ACTS project
 *
 * Mozilla Public License Version 2.0
 */

// Local include(s).
#include "test_cuda_containers_kernels.cuh"
#include "vecmem/containers/const_device_vector.hpp"
#include "vecmem/containers/device_vector.hpp"
#include "../../cuda/src/utils/cuda_error_handling.hpp"

/// Kernel performing a linear transformation using the vector helper types
__global__
void linearTransformKernel( vecmem::const_device_vector_data< int > input,
                            vecmem::device_vector_data< int > output ) {

   // Find the current index.
   const std::size_t i = blockIdx.x * blockDim.x + threadIdx.x;
   if( i >= input.m_size ) {
      return;
   }

   // Create the helper vectors.
   const vecmem::const_device_vector< int > inputvec( input );
   vecmem::device_vector< int > outputvec( output );

   // Perform the linear transformation.
   outputvec.at( i ) = 3 + inputvec.at( i ) * 2;
   return;
}

void linearTransform( vecmem::const_device_vector_data< int > input,
                      vecmem::device_vector_data< int > output ) {

   // Launch the kernel.
   linearTransformKernel<<< 1, input.m_size >>>( input, output );
   // Check whether it succeeded to run.
   VECMEM_CUDA_ERROR_CHECK( cudaGetLastError() );
   VECMEM_CUDA_ERROR_CHECK( cudaDeviceSynchronize() );
}
