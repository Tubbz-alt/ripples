//===------------------------------------------------------------*- C++ -*-===//
//
// Copyright 2018 Battelle Memorial Institute
//
//===----------------------------------------------------------------------===//

#include <chrono>
#include <cstring>
#include <iostream>
#include <unordered_map>

#include <omp.h>

#include "trng/uniform01_dist.hpp"
#include "trng/uniform_int_dist.hpp"

#include "im/cuda/cuda_generate_rrr_sets.h"
#include "im/cuda/cuda_utils.h"
#include "im/cuda/cuda_graph.cuh"

namespace im {

using mask_word_t = typename cuda_GraphTy::vertex_type;
constexpr size_t MAX_SET_SIZE = 256;
constexpr size_t MAX_PARDEG = 20;

// tested configurations:
// + 1 walk per thread:
// - warp_step = 1
//
// + 1 walk per warp:
// - warp_step = cuda_prop.warpSize
//
// + 1 walk per block:
// - warp step = 1
// - block_size = 1
struct cuda_conf_t {
  cudaDeviceProp cuda_prop;
  const cuda_GraphTy *graph = nullptr;

  // host-side buffers
  mask_word_t **res_masks = nullptr;

  // device-side buffers
  cuda_graph<cuda_GraphTy> *d_graph = nullptr;
  mask_word_t **d_res_masks = nullptr;
  cuda_PRNGeneratorTy **d_trng_states;

  // sizing
  size_t grid_size = 0, block_size = 0, n_blocks = 0;
  size_t warp_step = 0;       // 1: per-thread, warp-size: per-warp
  size_t max_batch_size = 0;  // walks per batch
  size_t mask_words = 0;
} cuda_conf;

__global__ void kernel_trng_setup(cuda_PRNGeneratorTy *d_trng_states, size_t rank, cuda_PRNGeneratorTy r, size_t warp_size,
                                  size_t num_threads) {
  int tid = blockIdx.x * blockDim.x + threadIdx.x;
  if (tid % warp_size == 0) {
    int wid = tid / warp_size;

    d_trng_states[wid] = r;
    d_trng_states[wid].split(MAX_PARDEG * num_threads, rank * MAX_PARDEG + wid);
  }
}

#if CUDA_PROFILE
enum breakdown_tag { OVERALL, KERNEL, COPY, TRANSLATE_ALLOC, TRANSLATE_BUILD };
std::unordered_map<breakdown_tag, std::vector<std::chrono::nanoseconds>>
    profile_breakdown;

void print_profile(breakdown_tag tag, const std::string &label) {
  std::sort(profile_breakdown[tag].begin(), profile_breakdown[tag].end());
  auto &sample(profile_breakdown[tag]);
  std::chrono::microseconds tot{0};
  for (auto &x : sample)
    tot += std::chrono::duration_cast<std::chrono::microseconds>(x);
  std::cout << "*** tag: " << label << "\n*** "
            << "cnt=" << sample.size() << "\tmin=" << sample[0].count()
            << "\tmed=" << sample[sample.size() / 2].count()
            << "\tmax=" << sample.back().count() << "\ttot(us)=" << tot.count()
            << std::endl;
}
#endif

void cuda_init(const cuda_GraphTy &G,
		       const cuda_PRNGeneratorTy &r,
               im::linear_threshold_tag &&model_tag) {
  cudaError_t e;
  cudaGetDeviceProperties(&cuda_conf.cuda_prop, 0);
  // copy graph to device
  cuda_conf.graph = &G;
  cuda_conf.d_graph = make_cuda_graph(G);

  // sizing
  cuda_conf.warp_step = 1;  // per block
  //cuda_conf.warp_step = cuda_conf.cuda_prop.warpSize;  // per warp
  cuda_conf.block_size = cuda_conf.warp_step * (1 << 0);
  cuda_conf.n_blocks = 1 << 12;
  cuda_conf.grid_size = cuda_conf.n_blocks * cuda_conf.block_size;
  cuda_conf.max_batch_size = cuda_conf.grid_size / cuda_conf.warp_step;
  cuda_conf.mask_words = MAX_SET_SIZE;

  // print sizing info
  CUDA_LOG("> *** CUDA_BATCHED sizing ***\n");
  CUDA_LOG("block-size = %d\n", cuda_conf.block_size);
  CUDA_LOG("n. blocks  = %d\n", cuda_conf.n_blocks);
  CUDA_LOG("warp size  = %d\n", cuda_conf.cuda_prop.warpSize);
  CUDA_LOG("grid size  = %d\n", cuda_conf.grid_size);
  CUDA_LOG("batch size = %d\n", cuda_conf.max_batch_size);
  CUDA_LOG("g-mem size = %d\n",
           cuda_conf.grid_size * cuda_conf.mask_words * sizeof(mask_word_t));

  // allocate host-side memory for result masks
  auto mask_size = cuda_conf.mask_words * sizeof(mask_word_t);
  cuda_conf.res_masks = (mask_word_t **)malloc(MAX_PARDEG * sizeof(mask_word_t *));
  for(size_t i = 0; i < MAX_PARDEG; ++i)
      cuda_conf.res_masks[i] = (mask_word_t *)malloc(cuda_conf.max_batch_size * mask_size);

  // allocate device-side memory for results masks
  cuda_conf.d_res_masks = (mask_word_t **)malloc(MAX_PARDEG * sizeof(mask_word_t *));
  for(size_t i = 0; i < MAX_PARDEG; ++i) {
	  e = cudaMalloc(&cuda_conf.d_res_masks[i], cuda_conf.max_batch_size * mask_size);
	  cuda_check(e, __FILE__, __LINE__);
  }

  // init trng
  cuda_conf.d_trng_states = (cuda_PRNGeneratorTy **)malloc(MAX_PARDEG * sizeof(cuda_PRNGeneratorTy *));
  for(size_t i = 0; i < MAX_PARDEG; ++i) {
	  cudaMalloc(&cuda_conf.d_trng_states[i], cuda_conf.max_batch_size * sizeof(cuda_PRNGeneratorTy));
	  cuda_check(e, __FILE__, __LINE__);

	  kernel_trng_setup<<<cuda_conf.n_blocks, cuda_conf.block_size>>>(
	      cuda_conf.d_trng_states[i], i, r, cuda_conf.warp_step,
	      cuda_conf.max_batch_size);
	    cuda_check(__FILE__, __LINE__);
  }
}  // namespace im

void cuda_init(const cuda_GraphTy &G,
               const cuda_PRNGeneratorTy &r,
               im::independent_cascade_tag &&) {}

void cuda_fini(im::linear_threshold_tag &&) {
// print profiling
#if CUDA_PROFILE
  printf("*** profiling summary (time unit: ns) ***\n");

  // print sizing info
  printf("> *** CUDA_BATCHED sizing ***\n");
  printf("block-size = %d\n", cuda_conf.block_size);
  printf("n. blocks  = %d\n", cuda_conf.n_blocks);
  printf("warp size  = %d\n", cuda_conf.cuda_prop.warpSize);
  printf("grid size  = %d\n", cuda_conf.grid_size);
  printf("batch size = %d\n", cuda_conf.max_batch_size);
  printf("g-mem size = %d\n",
         MAX_PARDEG * cuda_conf.grid_size * cuda_conf.mask_words * sizeof(mask_word_t));

  print_profile(breakdown_tag::OVERALL, "overall");
  print_profile(breakdown_tag::KERNEL, "kernel");
  print_profile(breakdown_tag::COPY, "device-to-host copy");
  print_profile(breakdown_tag::TRANSLATE_BUILD, "translate > build");
  print_profile(breakdown_tag::TRANSLATE_ALLOC, "translate > build > alloc");
#endif

  // finalize streams and free memory
  assert(cuda_conf.res_masks);
  for(size_t i = 0; i < MAX_PARDEG; ++i)
	  free(cuda_conf.res_masks[i]);
  free(cuda_conf.res_masks);
  assert(cuda_conf.d_res_masks);
  for(size_t i = 0; i < MAX_PARDEG; ++i)
	  cudaFree(cuda_conf.d_res_masks[i]);
  free(cuda_conf.d_res_masks);
  assert(cuda_conf.d_trng_states);
  for(size_t i = 0; i < MAX_PARDEG; ++i)
	  cudaFree(cuda_conf.d_trng_states[i]);
  free(cuda_conf.d_trng_states);

  // cleanup
  destroy_cuda_graph(cuda_conf.d_graph);
}

void cuda_fini(im::independent_cascade_tag &&) {}

template <typename HostGraphTy>
__global__ void kernel_lt_per_thread(
    size_t bs, typename HostGraphTy::DestinationTy **index, size_t num_nodes,
    size_t warp_size, cuda_PRNGeneratorTy *d_trng_states, mask_word_t *d_res_masks) {
  using destination_type = typename HostGraphTy::DestinationTy;
  using vertex_type = typename HostGraphTy::vertex_type;

  int tid = blockIdx.x * blockDim.x + threadIdx.x;
  if (tid % warp_size == 0) {
    int wid = tid / warp_size;
    if (wid < bs) {
      trng::uniform01_dist<float> u;
      trng::uniform_int_dist root_dist(0, num_nodes);
      size_t res_size = 0;

      // init res memory
      auto d_res_mask = d_res_masks + wid * MAX_SET_SIZE;
      memset(d_res_mask, 0, MAX_SET_SIZE * sizeof(mask_word_t));

      // cache rng state
      auto &r(d_trng_states[wid]);

      // select source node
      vertex_type src = root_dist(r);
      d_res_mask[res_size++] = src;

      float threshold;
      destination_type *first, *last;
      vertex_type v;
      while (src != num_nodes) {
        // rng
        threshold = u(r);

        // scan neighbor list
        first = index[src];
        last = index[src + 1];
        src = num_nodes;
        for (; first != last; ++first) {
          threshold -= first->weight;
          if (threshold > 0) continue;

          // found candidate vertex
          v = first->vertex;

          // insert if not visited
          size_t i = 0;
          while (i < res_size && d_res_mask[i] != v) ++i;
          if (i == res_size) {
              src = v;
              d_res_mask[res_size++] = v;
          }
          break;
        }
      }

      // mark end-of-set
      if (res_size < MAX_SET_SIZE) d_res_mask[res_size] = num_nodes;
    }  // end if active warp
  }    // end if active thread-in-warp
}  // namespace im

void batch_kernel(size_t rank, size_t batch_size) {
  CUDA_LOG("> [batch_kernel] size=%d\n", batch_size);

#if CUDA_PROFILE
  auto start = std::chrono::high_resolution_clock::now();
#endif

  kernel_lt_per_thread<cuda_GraphTy>
      <<<cuda_conf.n_blocks, cuda_conf.block_size>>>(
          batch_size, cuda_conf.d_graph->d_index_, cuda_conf.graph->num_nodes(),
          cuda_conf.warp_step, cuda_conf.d_trng_states[rank], cuda_conf.d_res_masks[rank]);
  cuda_check(__FILE__, __LINE__);

#if CUDA_PROFILE
  // un-comment the following line to measure effective kernel run-time (rather
  // than launch-time)
  // cudaDeviceSynchronize();
  auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::high_resolution_clock::now() - start);
  profile_breakdown[breakdown_tag::KERNEL].push_back(elapsed);
#endif
}

void batch_d2h(size_t rank, size_t batch_size) {
  CUDA_LOG("> [batch_d2h] size=%d\n", batch_size);

#if CUDA_PROFILE
  auto start = std::chrono::high_resolution_clock::now();
#endif
  cudaMemcpy(cuda_conf.res_masks[rank], cuda_conf.d_res_masks[rank],
             batch_size * cuda_conf.mask_words * sizeof(mask_word_t),
             cudaMemcpyDeviceToHost);

#if CUDA_PROFILE
  auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::high_resolution_clock::now() - start);
  profile_breakdown[breakdown_tag::COPY].push_back(elapsed);
#endif
}

void batch_build(size_t rank, cuda_res_t &rrr_sets, size_t bf, size_t batch_size) {
  // translate
  CUDA_LOG("> [batch_build] size=%d first=%d\n", batch_size, bf);
#if CUDA_PROFILE
  auto start = std::chrono::high_resolution_clock::now();
  std::chrono::nanoseconds m_elapsed{0};
#endif

  for (size_t i = 0; i < batch_size; ++i) {
    auto &rrr_set = rrr_sets[bf + i];
#if CUDA_PROFILE
    auto m_start = std::chrono::high_resolution_clock::now();
#endif
    rrr_set.reserve(MAX_SET_SIZE);
#if CUDA_PROFILE
   m_elapsed += std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - m_start);
#endif
    auto res_mask = cuda_conf.res_masks[rank] + (i * cuda_conf.mask_words);
    for (size_t j = 0; j < cuda_conf.mask_words &&
                       res_mask[j] != cuda_conf.graph->num_nodes();
         ++j) {
      rrr_set.push_back(res_mask[j]);
    }

#if CUDA_CHECK
    check_lt(rrr_set, *cuda_conf.graph, bf + i);
#endif

    if (rrr_set.size() == MAX_SET_SIZE) {
      printf("> RRR-set hit the maximum size %d\n", MAX_SET_SIZE);
      exit(1);
    }

    std::stable_sort(rrr_set.begin(), rrr_set.end());
  }

#if CUDA_PROFILE
  auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::high_resolution_clock::now() - start);
  profile_breakdown[breakdown_tag::TRANSLATE_BUILD].push_back(elapsed);
  profile_breakdown[breakdown_tag::TRANSLATE_ALLOC].push_back(m_elapsed);
#endif
}

cuda_res_t CudaGenerateRRRSets(size_t theta,
                               im::linear_threshold_tag &&model_tag) {
  CUDA_LOG("> *** CudaGenerateRRRSets theta=%d ***\n", theta);

#if CUDA_PROFILE
  auto start = std::chrono::high_resolution_clock::now();
#endif

  cuda_res_t rrr_sets(theta);

  auto num_batches = (rrr_sets.size() + cuda_conf.max_batch_size - 1) / cuda_conf.max_batch_size;
  printf("> [CudaGenerateRRRSets] BEGIN-phase batches=%d\n", num_batches);

#pragma omp parallel for num_threads(MAX_PARDEG)
  for(size_t bi = 0; bi < num_batches; ++bi) {
	  auto batch_first = bi * cuda_conf.max_batch_size;
	  auto batch_size = std::min(rrr_sets.size() - batch_first, cuda_conf.max_batch_size);
	  auto rank = omp_get_thread_num();

	  printf("> [CudaGenerateRRRSets] rank #%d\n", rank);

	  batch_kernel(rank, batch_size);
	  batch_d2h(rank, batch_size);
	  batch_build(rank, rrr_sets, batch_first, batch_size);
  }

#if CUDA_PROFILE
  auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::high_resolution_clock::now() - start);
  profile_breakdown[breakdown_tag::OVERALL].push_back(elapsed);
#endif

  return rrr_sets;
}  // namespace im

cuda_res_t CudaGenerateRRRSets(size_t theta,
                               im::independent_cascade_tag &&model_tag) {
  assert(false);
  return cuda_res_t{};
}

}  // namespace im
