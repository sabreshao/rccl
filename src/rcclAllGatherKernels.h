/*
Copyright (c) 2017-Present Advanced Micro Devices, Inc. 
All rights reserved.
*/

#pragma once

//
// This file declares kernels for allgather
//

// 
// This kernel does data copy from all peer gpu buffers to current gpu buffer
//
template<typename DataType_t, typename VectorType_t>
__global__ void RcclKernelAllGather(DeviceControl_t* pcurr_track, const void* send_buff, void* recv_buff, int rank, int count, unsigned num_vector_workgroups, unsigned num_scalars) {
    unsigned tx = threadIdx.x;
    unsigned bx = blockIdx.x;
    unsigned tid = tx + bx * knum_vectors_per_workgroup;

    const int knum_elements_per_vector = sizeof(VectorType_t) / sizeof(DataType_t);
    const int knum_elements_per_workgroup = knum_elements_per_vector * knum_workitems;
    int total_elements = knum_elements_per_workgroup * num_vector_workgroups;

    // add recv_buff to tracker on current gpu
    if(tid == 0) {
        std::atomic_store_explicit(&(pcurr_track->dst_buffer), (void*)recv_buff, std::memory_order_seq_cst);
    }
    __syncthreads();

    if(bx < num_vector_workgroups) {

        // typecast source buffer
        VectorType_t* curr_buff = reinterpret_cast<VectorType_t*>((void*)send_buff);
        // get destination buffer with offset at rank * count
        VectorType_t* dest_buff = reinterpret_cast<VectorType_t*>(reinterpret_cast<DataType_t*>(recv_buff) + rank * count);

        dest_buff[tid] = curr_buff[tid];

        // get peer gpu tracker
        DeviceControl_t* pnext_track = pcurr_track->next_gpu;

        // start traveling along the ring (clique) until current track is reached
        // 1. Get destination buffer from next gpu
        // 2. Copy data to peer gpus destination buffer
        while(pnext_track != pcurr_track) {

            // wait until peer gpus destination buffer is set
            while(std::atomic_load_explicit(&(pnext_track->dst_buffer), std::memory_order_seq_cst) == 0) {}
            __syncthreads();

            // get destination buffer from peer gpu
            VectorType_t* next_buff = reinterpret_cast<VectorType_t*>(reinterpret_cast<DataType_t*>(std::atomic_load_explicit(&(pnext_track->dst_buffer), std::memory_order_seq_cst)) + rank * count);

            next_buff[tid] = curr_buff[tid];
            __syncthreads();

            // move on to next gpu
            pnext_track = pnext_track->next_gpu;
        }

    // operate on scalar data. Algorithm is same as VectorType_t
    } else {

        DataType_t* curr_buff = reinterpret_cast<DataType_t*>((void*)send_buff) + total_elements;
        DataType_t* dest_buff = reinterpret_cast<DataType_t*>(recv_buff) + total_elements;

        DeviceControl_t* pnext_track = pcurr_track->next_gpu;
        while(pnext_track != pcurr_track) {

            // wait until peer gpus destination buffer is set
            while(std::atomic_load_explicit(&(pnext_track->dst_buffer), std::memory_order_seq_cst) == 0) {}
            __syncthreads();

            // get destination buffer from peer gpu
            DataType_t* next_buff = reinterpret_cast<DataType_t*>(std::atomic_load_explicit(&(pnext_track->dst_buffer), std::memory_order_seq_cst)) + rank * count + total_elements;

            for(unsigned id = tx; id < num_scalars; id = id + knum_workitems) {
                next_buff[id] = curr_buff[id];
            }
            __syncthreads();

            // move on to next gpu
            pnext_track = pnext_track->next_gpu;
        }
    }
}

#if 0
// This is read based approach
template<typename DataType_t, typename VectorType_t>
__global__ void RcclKernelAllGather(DeviceControl_t* pcurr_track, const void* send_buff, void* recv_buff, int rank, int count, int num_gpus) {
    unsigned tx = threadIdx.x;
    unsigned bx = blockIdx.x;
    unsigned tid = tx + bx * knum_vectors_per_workgroup;

    if(tid == 0) {
        std::atomic_store_explicit(&(pcurr_track->src_buffer), (void*)send_buff, std::memory_order_seq_cst);
    }
    __syncthreads();

    VectorType_t* curr_buff = reinterpret_cast<VectorType_t*>((void*)send_buff);
    VectorType_t* dest_buff = reinterpret_cast<VectorType_t*>(reinterpret_cast<DataType_t*>(recv_buff) + (rank % num_gpus) * count);

    dest_buff[tid] = curr_buff[tid];

    DeviceControl_t* pnext_track = pcurr_track->next_gpu;

    rank++;

    while(pnext_track != pcurr_track) {
        while(std::atomic_load_explicit(&(pnext_track->src_buffer), std::memory_order_seq_cst) == 0) {}
        __syncthreads();

        curr_buff = reinterpret_cast<VectorType_t*>(std::atomic_load_explicit(&(pnext_track->src_buffer), std::memory_order_seq_cst));
        dest_buff = reinterpret_cast<VectorType_t*>(reinterpret_cast<DataType_t*>(recv_buff) + (rank % num_gpus) * count);

        dest_buff[tid] = curr_buff[tid];
        __syncthreads();
        rank++;
        pnext_track = pnext_track->next_gpu;
    }
}

#endif
