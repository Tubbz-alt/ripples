//===------------------------------------------------------------*- C++ -*-===//
//
//             Ripples: A C++ Library for Influence Maximization
//                  Marco Minutoli <marco.minutoli@pnnl.gov>
//                   Pacific Northwest National Laboratory
//
//===----------------------------------------------------------------------===//
//
// Copyright 2018 Battelle Memorial Institute
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors
// may be used to endorse or promote products derived from this software without
// specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
//===----------------------------------------------------------------------===//

#ifndef RIPPLES_STREAMING_RRR_GENERATOR_H
#define RIPPLES_STREAMING_RRR_GENERATOR_H

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <sstream>
#include <vector>

#include "omp.h"

#include "spdlog/spdlog.h"
#include "trng/uniform_int_dist.hpp"

#include "ripples/generate_rrr_sets.h"

#ifndef RIPPLES_DISABLE_CUDA
#include "ripples/cuda/cuda_generate_rrr_sets.h"
#include "ripples/cuda/from_nvgraph/bfs.hxx"
#endif

#if CUDA_PROFILE
#include <chrono>
#endif

namespace ripples {

int streaming_command_line(std::set<size_t> &gpu_mapping,
                           size_t streaming_workers,
                           size_t streaming_gpu_workers,
                           std::string gpu_mapping_string) {
  auto console = spdlog::get("console");
  if (!(streaming_workers > 0 &&
        streaming_gpu_workers <= streaming_workers)) {
    console->error("invalid number of streaming workers");
    return -1;
  }
  if (!gpu_mapping_string.empty()) {
    std::istringstream iss(gpu_mapping_string);
    std::string token;
    while (std::getline(iss, token, ',')) {
      std::stringstream omp_num_ss(token);
      size_t omp_num;
      omp_num_ss >> omp_num;
      if(!(omp_num < streaming_workers)) {
        console->error("invalid OpenMP number in GPU mapping");
        return -1;
      }
      gpu_mapping.insert(omp_num);
    }
    if(gpu_mapping.size() != streaming_gpu_workers) {
      console->error("invalid length of GPU mapping string");
      return -1;
    }
  }
  return 0;
}

template <typename GraphTy>
class WalkWorker {
  using vertex_t = typename GraphTy::vertex_type;

 public:
  using rrr_set_t = std::vector<vertex_t>;
  using rrr_sets_t = std::vector<rrr_set_t>;

  WalkWorker(const GraphTy &G) : G_(G) {}
  virtual ~WalkWorker() {}
  virtual void svc_loop(std::atomic<size_t> &mpmc_head, rrr_sets_t &res) = 0;

 protected:
  const GraphTy &G_;

#if CUDA_PROFILE
 public:
  virtual void begin_prof_iter() = 0;
#endif
};

template <typename GraphTy, typename PRNGeneratorTy, typename diff_model_tag>
class CPUWalkWorker : public WalkWorker<GraphTy> {
  using vertex_t = typename GraphTy::vertex_type;
  using rrr_sets_t = typename WalkWorker<GraphTy>::rrr_sets_t;

 public:
  CPUWalkWorker(const GraphTy &G, const PRNGeneratorTy &rng)
      : WalkWorker<GraphTy>(G), rng_(rng), u_(0, G.num_nodes()) {}
  
  void svc_loop(std::atomic<size_t> &mpmc_head, rrr_sets_t &res) {
    size_t offset = 0;
    while((offset = mpmc_head.fetch_add(batch_size_)) < res.size()) {
      auto first = res.begin();
      std::advance(first, offset);
      auto last = first;
      std::advance(last, batch_size_);
      if(last > res.end())
        last = res.end();
      batch(first, last);
    }
  }

 private:
  static constexpr size_t batch_size_ = 32;
  PRNGeneratorTy rng_;
  trng::uniform_int_dist u_;

  void batch(typename rrr_sets_t::iterator first,
             typename rrr_sets_t::iterator last) {
#if CUDA_PROFILE
    auto start = std::chrono::high_resolution_clock::now();
#endif
    auto size = std::distance(first, last);
    while(first != last) {
      vertex_t root = u_(rng_);
      AddRRRSet(this->G_, root, rng_, *first++, diff_model_tag{});
    }
#if CUDA_PROFILE
    auto &p(prof_bd.back());
    p.d_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - start);
    p.n_ += size;
#endif
  }

#if CUDA_PROFILE
 public:
  struct iter_profile_t {
    size_t n_{0};
    std::chrono::nanoseconds d_{0};
  };
  using profile_t = std::vector<iter_profile_t>;
  profile_t prof_bd;

  void begin_prof_iter() { prof_bd.emplace_back(); }
  void print_prof_iter(size_t i) {
    auto console = spdlog::get("console");
    assert(i < prof_bd.size());
    auto &p(prof_bd[i]);
    if (p.n_)
      console->info(
          "n-sets={}\tns={}\tb={}", p.n_, p.d_.count(),
          (float)p.n_ * 1e03 /
              std::chrono::duration_cast<std::chrono::milliseconds>(p.d_)
                  .count());
    else
      console->info("> idle worker");
  }
#endif
};

template <typename GraphTy, typename PRNGeneratorTy, typename diff_model_tag>
class GPUWalkWorker;

#ifndef RIPPLES_DISABLE_CUDA
template <typename GraphTy, typename PRNGeneratorTy>
class GPUWalkWorker<GraphTy, PRNGeneratorTy, linear_threshold_tag>
    : public WalkWorker<GraphTy> {
  using vertex_t = typename GraphTy::vertex_type;
  using rrr_set_t = typename WalkWorker<GraphTy>::rrr_set_t;
  using rrr_sets_t = typename WalkWorker<GraphTy>::rrr_sets_t;

 public:
  struct config_t {
    config_t(size_t) {
      assert(num_threads_ % block_size_ == 0);
      max_blocks_ = num_threads_ / block_size_;

      printf(
          "*** DBG *** > [GPUWalkWorkerLT::config_t] "
          "block_size_=%d\tnum_threads_=%d\tmax_blocks_=%d\n",
          block_size_, num_threads_, max_blocks_);
    }

    size_t num_gpu_threads() const { return num_threads_; }

    // configuration parameters
    static constexpr size_t block_size_ = 256;
    static constexpr size_t num_threads_ = 1 << 15;
    const size_t mask_words_ = 8;  // maximum walk size

    // inferred configuration
    size_t max_blocks_{0};
  };

  GPUWalkWorker(const config_t &conf, const GraphTy &G, const PRNGeneratorTy &rng,
            cudaStream_t cuda_stream)
      : WalkWorker<GraphTy>(G),
        cuda_stream_(cuda_stream),
        conf_(conf),
        u_(0, G.num_nodes()) {
    // allocate host/device memory
    auto mask_size = conf.mask_words_ * sizeof(mask_word_t);
    lt_res_mask_ = (mask_word_t *)malloc(conf_.num_gpu_threads() * mask_size);
    cuda_malloc((void **)&d_lt_res_mask_, conf_.num_gpu_threads() * mask_size);

    // allocate device-size RNGs
    cuda_malloc((void **)&d_trng_state_,
                conf_.num_gpu_threads() * sizeof(PRNGeneratorTy));
  }

  ~GPUWalkWorker() {
    // free host/device memory
    free(lt_res_mask_);
    cuda_free(d_lt_res_mask_);
    cuda_free(d_trng_state_);
  }

  void rng_setup(const PRNGeneratorTy &master_rng, size_t num_seqs,
                 size_t first_seq) {
    cuda_lt_rng_setup(d_trng_state_, master_rng, num_seqs, first_seq,
                      conf_.max_blocks_, conf_.block_size_);
  }

  void svc_loop(std::atomic<size_t> &mpmc_head, rrr_sets_t &res) {
    size_t offset = 0;
    auto batch_size = conf_.num_gpu_threads();
    while ((offset = mpmc_head.fetch_add(batch_size)) < res.size()) {
      auto first = res.begin();
      std::advance(first, offset);
      auto last = first;
      std::advance(last, batch_size);
      if (last > res.end()) last = res.end();
      batch(first, last);
    }
  }

 private:
  config_t conf_;
  cudaStream_t cuda_stream_;
  PRNGeneratorTy rng_;
  trng::uniform_int_dist u_;

  // memory buffers
  mask_word_t *lt_res_mask_, *d_lt_res_mask_;
  PRNGeneratorTy *d_trng_state_;

  void batch(typename rrr_sets_t::iterator first,
             typename rrr_sets_t::iterator last) {
#if CUDA_PROFILE
    auto &p(prof_bd.back());
    auto start = std::chrono::high_resolution_clock::now();
#endif
    auto size = std::distance(first, last);

    cuda_lt_kernel(conf_.max_blocks_, conf_.block_size_,
                   size, this->G_.num_nodes(),
                   d_trng_state_, d_lt_res_mask_, conf_.mask_words_,
                   cuda_stream_);
#if CUDA_PROFILE
  cuda_sync(cuda_stream_);
  auto t1 = std::chrono::high_resolution_clock::now();
  p.dwalk_ += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - start);
  auto t0 = t1;
#endif

    cuda_d2h(lt_res_mask_, d_lt_res_mask_,
             size * conf_.mask_words_ * sizeof(mask_word_t),
             cuda_stream_);
    cuda_sync(cuda_stream_);
#if CUDA_PROFILE
  t1 = std::chrono::high_resolution_clock::now();
  p.dd2h_ += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
  t0 = t1;
#endif

    batch_lt_build(first, size);
#if CUDA_PROFILE
  t1 = std::chrono::high_resolution_clock::now();
  p.dbuild_ += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
#endif

#if CUDA_PROFILE
    p.d_ += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - start);
    p.n_ += size;
#endif
  }

  void batch_lt_build(typename rrr_sets_t::iterator first, size_t batch_size) {
#if CUDA_PROFILE
    auto &p(prof_bd.back());
#endif

    for (size_t i = 0; i < batch_size; ++i, ++first) {
      auto &rrr_set(*first);
      rrr_set.reserve(conf_.mask_words_);
      auto res_mask = lt_res_mask_ + (i * conf_.mask_words_);
      if (res_mask[0] != this->G_.num_nodes()) {
        // valid walk
        for (size_t j = 0;
             j < conf_.mask_words_ && res_mask[j] != this->G_.num_nodes();
             ++j) {
          rrr_set.push_back(res_mask[j]);
        }
      } else {
// invalid walk
#if CUDA_PROFILE
        p.num_exceedings_++;
#endif
        auto root = res_mask[1];
        AddRRRSet(this->G_, root, rng_, rrr_set,
                  ripples::linear_threshold_tag{});
      }

      std::stable_sort(rrr_set.begin(), rrr_set.end());
    }
  }

#if CUDA_PROFILE
  struct iter_profile_t {
    size_t n_{0}, num_exceedings_{0};
    std::chrono::nanoseconds d_{0}, dwalk_{0}, dd2h_{0}, dbuild_{0};
  };
  using profile_t = std::vector<iter_profile_t>;
  profile_t prof_bd;

 public:
  void begin_prof_iter() { prof_bd.emplace_back(); }
  void print_prof_iter(size_t i) {
    auto console = spdlog::get("console");
    assert(i < prof_bd.size());
    auto &p(prof_bd[i]);
    if (p.n_) {
      console->info(
          "n-sets={}\tn-exc={}\tns={}\tb={}", p.n_, p.num_exceedings_,
          p.d_.count(),
          (float)p.n_ * 1e03 /
              std::chrono::duration_cast<std::chrono::milliseconds>(p.d_)
                  .count());
      console->info("walk={}\td2h={}\tbuild={}", p.dwalk_.count(),
                    p.dd2h_.count(), p.dbuild_.count());
      console->info("n. exceedings={} (/{}={})", p.num_exceedings_, p.n_,
                    (float)p.num_exceedings_ / p.n_);
    } else
      console->info("> idle worker");
  }
#endif
};

template <typename GraphTy, typename PRNGeneratorTy>
class GPUWalkWorker<GraphTy, PRNGeneratorTy, independent_cascade_tag>
    : public WalkWorker<GraphTy> {
  using vertex_t = typename GraphTy::vertex_type;
  using rrr_sets_t = typename WalkWorker<GraphTy>::rrr_sets_t;
  using rrr_set_t = typename WalkWorker<GraphTy>::rrr_set_t;

  using bfs_solver_t = nvgraph::Bfs<int, PRNGeneratorTy>;

 public:
  struct config_t {
    config_t(size_t num_workers)
        : block_size_(bfs_solver_t::traverse_block_size()),
          max_blocks_(num_workers ? cuda_max_blocks() / num_workers : 0) {
      printf(
          "*** DBG *** > [GPUWalkWorkerIC::config_t] "
          "max_blocks_=%d\tblock_size_=%d\n",
          max_blocks_, block_size_);
    }

    size_t num_gpu_threads() const { return max_blocks_ * block_size_; }

    const size_t max_blocks_;
    const size_t block_size_;
  };

  GPUWalkWorker(const config_t &conf, const GraphTy &G, const PRNGeneratorTy &rng,
            cudaStream_t cuda_stream)
      : WalkWorker<GraphTy>(G),
        cuda_stream_(cuda_stream),
        conf_(conf),
        u_(0, G.num_nodes()),
        // TODO stream
        solver(G.num_nodes(), G.num_edges(), cuda_graph_index(),
               cuda_graph_edges(), cuda_graph_weights(), true,
               TRAVERSAL_DEFAULT_ALPHA, TRAVERSAL_DEFAULT_BETA,
               conf_.max_blocks_, cuda_stream) {
    // allocate host/device memory
    ic_predecessors_ = (int *)malloc(
        G.num_nodes() * sizeof(typename cuda_device_graph::vertex_t));
    cudaMalloc((void **)&d_ic_predecessors_,
               G.num_nodes() * sizeof(typename cuda_device_graph::vertex_t));

    // allocate device-size RNGs
    cuda_malloc((void **)&d_trng_state_,
                conf_.num_gpu_threads() * sizeof(PRNGeneratorTy));

    solver.configure(nullptr, d_ic_predecessors_, nullptr);
  }

  ~GPUWalkWorker() {
    // free host/device memory
    free(ic_predecessors_);
    cudaFree(d_ic_predecessors_);
    cuda_free(d_trng_state_);
  }

  void rng_setup(const PRNGeneratorTy &master_rng, size_t num_seqs,
                 size_t first_seq) {
    cuda_ic_rng_setup(d_trng_state_, master_rng, num_seqs, first_seq,
                      conf_.max_blocks_, conf_.block_size_);
    solver.rng(d_trng_state_);
  }

  void svc_loop(std::atomic<size_t> &mpmc_head, rrr_sets_t &res) {
    size_t offset = 0;
    while((offset = mpmc_head.fetch_add(batch_size_)) < res.size()) {
      auto first = res.begin();
      std::advance(first, offset);
      auto last = first;
      std::advance(last, batch_size_);
      if(last > res.end())
        last = res.end();
      batch(first, last);
    }
  }

 private:
  static constexpr size_t batch_size_ = 32;
  config_t conf_;
  cudaStream_t cuda_stream_;
  PRNGeneratorTy rng_;
  trng::uniform_int_dist u_;

  // nvgraph machinery
  bfs_solver_t solver;

  // memory buffers
  typename cuda_device_graph::vertex_t *ic_predecessors_, *d_ic_predecessors_;
  PRNGeneratorTy *d_trng_state_;

  void batch(typename rrr_sets_t::iterator first,
             typename rrr_sets_t::iterator last) {
#if CUDA_PROFILE
    auto &p(prof_bd.back());
    auto start = std::chrono::high_resolution_clock::now();
#endif
    auto size = std::distance(first, last);
    for (size_t wi = 0; wi < size; ++wi) {
#if CUDA_PROFILE
      auto t0 = std::chrono::high_resolution_clock::now();
#endif
      auto root = u_(rng_);
      solver.traverse(reinterpret_cast<int>(root));
#if CUDA_PROFILE
      cuda_sync(cuda_stream_);
      auto t1 = std::chrono::high_resolution_clock::now();
      p.dwalk_ += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
      t0 = t1;
#endif

      cuda_d2h(
          ic_predecessors_, d_ic_predecessors_,
          this->G_.num_nodes() * sizeof(typename cuda_device_graph::vertex_t),
          cuda_stream_);
      cuda_sync(cuda_stream_);
#if CUDA_PROFILE
      t1 = std::chrono::high_resolution_clock::now();
      p.dd2h_ += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
      t0 = t1;
#endif

      ic_predecessors_[root] = root;
      ic_build(first++);
#if CUDA_PROFILE
      t1 = std::chrono::high_resolution_clock::now();
      p.dbuild_ += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
#endif
    }
#if CUDA_PROFILE
    p.d_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - start);
    p.n_ += size;
#endif
  }

  void ic_build(typename rrr_sets_t::iterator dst) {
    auto &rrr_set(*dst);
    for (vertex_t i = 0; i < this->G_.num_nodes(); ++i)
      if (ic_predecessors_[i] != -1) rrr_set.push_back(i);
  }

#if CUDA_PROFILE
  struct iter_profile_t {
    size_t n_{0};
    std::chrono::nanoseconds d_{0}, dwalk_{0}, dd2h_{0}, dbuild_{0};
  };
  using profile_t = std::vector<iter_profile_t>;
  profile_t prof_bd;

 public:
  void begin_prof_iter() { prof_bd.emplace_back(); }
  void print_prof_iter(size_t i) {
    auto console = spdlog::get("console");
    assert(i < prof_bd.size());
    auto &p(prof_bd[i]);
    if (p.n_) {
      console->info(
          "n-sets={}\tns={}\tb={}", p.n_, p.d_.count(),
          (float)p.n_ * 1e03 /
              std::chrono::duration_cast<std::chrono::milliseconds>(p.d_)
                  .count());
      console->info("walk={}\td2h={}\tbuild={}", p.dwalk_.count(),
                    p.dd2h_.count(), p.dbuild_.count());
    } else
      console->info("> idle worker");
  }
#endif
};
#endif // RIPPLES_DISABLE_CUDA

template <typename GraphTy, typename PRNGeneratorTy, typename diff_model_tag>
class StreamingRRRGenerator {
  using vertex_t = typename GraphTy::vertex_type;
  using rrr_set_t = std::vector<vertex_t>;
  using rrr_sets_t = std::vector<rrr_set_t>;

  using worker_t = WalkWorker<GraphTy>;
  using gpu_worker_t = GPUWalkWorker<GraphTy, PRNGeneratorTy, diff_model_tag>;
  using cpu_worker_t = CPUWalkWorker<GraphTy, PRNGeneratorTy, diff_model_tag>;

 public:
  StreamingRRRGenerator(const GraphTy &G, const PRNGeneratorTy &master_rng,
                        size_t num_cpu_workers, size_t num_gpu_workers,
                        std::set<size_t> gpu_mapping)
      : num_cpu_workers_(num_cpu_workers), num_gpu_workers_(num_gpu_workers) {
#ifndef RIPPLES_DISABLE_CUDA
    // init GPU
    if (num_gpu_workers_) cuda_graph_init(G);
    std::vector<cudaStream_t> cuda_streams(num_gpu_workers_);
    typename gpu_worker_t::config_t gpu_conf(num_gpu_workers_);
    assert(gpu_conf.max_blocks_ * num_gpu_workers_ <= cuda_max_blocks());
    auto num_gpu_threads_per_worker = gpu_conf.num_gpu_threads();
    auto num_rng_sequences =
        num_cpu_workers_ + num_gpu_workers_ * (num_gpu_threads_per_worker + 1);
    auto gpu_seq_offset = num_cpu_workers_ + num_gpu_workers_;
#else
    assert(num_gpu_workers_ == 0);
    size_t num_rng_sequences = num_cpu_workers_;
#endif

#ifndef RIPPLES_DISABLE_CUDA
    // GPU workers
    for (size_t i = 0; i < num_gpu_workers_; ++i) {
      auto rng = master_rng;
      rng.split(num_rng_sequences, num_cpu_workers_ + i);
      auto &stream(cuda_streams[i]);
      cudaStreamCreate(&stream);
      auto w = new gpu_worker_t(gpu_conf, G, rng, stream);
      w->rng_setup(master_rng, num_rng_sequences,
                   gpu_seq_offset + i * num_gpu_threads_per_worker);
      gpu_workers.push_back(w);
    }
#endif

    // CPU workers
    for (size_t i = 0; i < num_cpu_workers_; ++i) {
      auto rng = master_rng;
      rng.split(num_rng_sequences, i);
      cpu_workers.push_back(new cpu_worker_t(G, rng));
    }

    // map workers to OpenMP nums
    if (gpu_mapping.empty()) {
      size_t omp_num = 0;
      // by default, GPU wprkers are mapped after CPU workers
      for (auto &wp : cpu_workers) {
        workers.push_back(wp);
        printf("> mapping: omp=%d\t->\tCPU-worker\n", omp_num++);
      }
#ifndef RIPPLES_DISABLE_CUDA
      for (auto &wp : gpu_workers) {
        workers.push_back(wp);
        printf("> mapping: omp=%d\t->\tGPU-worker\n", omp_num++);
      }
#endif
    } else {
#ifndef RIPPLES_DISABLE_CUDA
        // translate user-mapping string into vector
        auto cw = cpu_workers.begin();
        auto gw = gpu_workers.begin();
        auto m = gpu_mapping.begin();
        for (size_t omp_num = 0; omp_num < num_cpu_workers + num_gpu_workers;
             ++omp_num) {
          if(m != gpu_mapping.end() && omp_num == *m) {
            workers.push_back(*gw++);
            printf("> mapping: omp=%d\t->\tGPU-worker\n", omp_num);
            ++m;
          }
          else {
            workers.push_back(*cw++);
            printf("> mapping: omp=%d\t->\tCPU-worker\n", omp_num);
          }
        }
        // check
        assert(cw == cpu_workers.end());
        assert(gw == gpu_workers.end());
        assert(m == gpu_mapping.end());
#endif
    }
  }

  ~StreamingRRRGenerator() {
#if CUDA_PROFILE
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(prof_bd.d);
    auto console = spdlog::get("console");
    console->info("*** BEGIN Streaming Engine profiling");
    for (size_t i = 0; i < prof_bd.prof_bd.size(); ++i) {
      console->info("+++ BEGIN iter {}", i);
      console->info("--- CPU workers");
      for (auto &wp : cpu_workers)
        wp->print_prof_iter(i);
#ifndef RIPPLES_DISABLE_CUDA
      console->info("--- GPU workers");
      for (auto &wp : gpu_workers)
        wp->print_prof_iter(i);
#endif
      console->info("--- overall");
      auto &p(prof_bd.prof_bd[i]);
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(p.d_);
      console->info("n. sets               = {}", p.n_);
      console->info("elapsed (ns)          = {}", p.d_.count());
      console->info("throughput (sets/sec) = {}",
                    (float)p.n_ * 1e03 / ms.count());
      console->info("+++ END iter {}", i);
    }
    console->info("--- overall");
    console->info("n. sets               = {}", prof_bd.n);
    console->info("n. iters              = {}", prof_bd.prof_bd.size());
    console->info("elapsed (ms)          = {}", ms.count());
    console->info("throughput (sets/sec) = {}",
                  (float)prof_bd.n * 1e03 / ms.count());
    console->info("*** END Streaming Engine profiling");
#endif

    for (auto &w : workers) delete w;

#ifndef RIPPLES_DISABLE_CUDA
    if (num_gpu_workers_) cuda_graph_fini();
#endif
  }

  rrr_sets_t generate(size_t theta) {
#if CUDA_PROFILE
    auto start = std::chrono::high_resolution_clock::now();
    for (auto &w : workers) w->begin_prof_iter();
#endif

    rrr_sets_t res(theta);
    mpmc_head.store(0);

#pragma omp parallel num_threads(num_cpu_workers_ + num_gpu_workers_)
    {
      size_t rank = omp_get_thread_num();
      workers[rank]->svc_loop(mpmc_head, res);
    }

#if CUDA_PROFILE
    auto d = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - start);
    prof_bd.prof_bd.emplace_back(theta, d);
    prof_bd.n += theta;
    prof_bd.d += std::chrono::duration_cast<std::chrono::microseconds>(d);
#endif

    return res;
  }

 private:
  size_t num_cpu_workers_, num_gpu_workers_;
  size_t max_batch_size_;
  std::vector<cpu_worker_t *> cpu_workers;
#ifndef RIPPLES_DISABLE_CUDA
  std::vector<gpu_worker_t *> gpu_workers;
#endif
  std::vector<worker_t *> workers;
  std::atomic<size_t> mpmc_head{0};

#if CUDA_PROFILE
  struct iter_profile_t {
    iter_profile_t(size_t n, std::chrono::nanoseconds d) : n_(n), d_(d) {}

    size_t n_{0};
    std::chrono::nanoseconds d_{0};
  };
  struct profile_t {
    size_t n{0};
    std::chrono::microseconds d{0};
    std::vector<iter_profile_t> prof_bd;
  };
  profile_t prof_bd;
#endif
};
}  // namespace ripples

#endif  // RIPPLES_STREAMING_RRR_GENERATOR_H