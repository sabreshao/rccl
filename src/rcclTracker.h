/*
Copyright (c) 2017 - Present Advanced Micro Devices, Inc.
All rights reserved.
*/

#pragma once

#include <atomic>
#include <map>
#include "rcclCheck.h"
#include <hip/hip_runtime.h>

#define KNRM "\x1B[0m"
#define KRED "\x1B[31m"
#define KGRN "\x1B[32m"
#define KYEL "\x1B[33m"
#define KBLU "\x1B[34m"
#define KMAG "\x1B[35m"
#define KCYN "\x1B[36m"
#define CWHT "\x1B[37m"

const char* API_COLOR = KGRN;
const char* API_COLOR_END = KNRM;

// 
constexpr int krccl_print_api = 1 << 0;
constexpr int krccl_print_internal = 1 << 1;
constexpr int krccl_print_kernel = 1 << 2;


// we always launch 1024 workitems per workgroup
// in our kernels
constexpr unsigned knum_workitems = 1024;
// we use 1024 vectors per workgroup
// where single workitem does op on single vector
constexpr unsigned knum_vectors_per_workgroup = 1024;

//
// data structure used to track details about peer gpus.
// It is allocated as pinned host memory visible to all the gpus
//
struct DeviceControl_t {
    // point to DeviceControl_t owned by previous gpu in clique
    struct DeviceControl_t *prev_gpu;
    // point to DeviceControl_t owned by next gpu in clique
    struct DeviceControl_t *next_gpu;
    // we use atomic data type to store pointer to buffers
    // on a gpu, as there are multiple readers (all peer gpus)
    // and single writer (current gpu)

    // stores source buffer on current gpu
    std::atomic<void*> src_buffer;
    // stores destination buffer on current gpu
    std::atomic<void*> dst_buffer;
    // stores device index according to hip programming model
    uint32_t hip_current_device_index;
};

struct RcclComm_t;

//
// pool data structure used to store all DeviceControl_t
// data structures and track rcclComm_t accordingly
//
class DevTrackerPool_t{
private:
    // stores an array of device indices user provided
    // we allocate memory, do memcpy from user buffer
    // deleted at destruction
    int *device_indices_;
    // number of devices a pool is created for
    int num_devices_;
public:
    // counter to track how many devices are
    // active in pool. Used to know when we can
    // destroy the pool and all data structures
    int active_devices_;
    // used to track DeviceControl_t structures for each gpu
    std::map<size_t, DeviceControl_t*> pool_;
    DevTrackerPool_t() : device_indices_(nullptr), num_devices_(0) {}
    ~DevTrackerPool_t() {
        delete device_indices_;
    }
    // construction is initialization
    DevTrackerPool_t(const int *device_indices_, int num_devices_);
    // when a new device is added to clique,
    // return corresponding RcclComm_t structure
    RcclComm_t *AddDevice(int device, int rank, int ndev);
    void PrintAll();
    // given a device index, get DeviceControl_t structure
    DeviceControl_t *GetPoolByDeviceIndex(int device_index);
};

// internal representation of rcclComm_t
// for structure is allocated for each gpu,
// tracks,
// 1. the DeviceControl_t pool where it belongs to
// 2. tracker corresponding to gpu
// 3. number of devices in the pool
// 4. device index it is associated to
// 5. rank of gpu in clique
struct RcclComm_t {
public:
    DevTrackerPool_t *pool_;
    DeviceControl_t *track_;
    hipEvent_t event_;
    int num_devices_;
    int device_;
    int rank_;
    ~RcclComm_t() {
        HIPCHECK(hipEventDestroy(event_));
    }
};

DevTrackerPool_t::DevTrackerPool_t(const int* device_indices, int num_devices) :
        num_devices_(num_devices), active_devices_(num_devices) {

    // store users device index
    // restore before exiting function
    int user_device_index;
    HIPCHECK(hipGetDevice(&user_device_index));

    // allocate memory to store device indices provided by user
    device_indices_ = new int[num_devices_];
    memcpy(device_indices_, device_indices, num_devices_*sizeof(int));

    struct DeviceControl_t *pdctl;

    // allocate DeviceControl_t as system pinned memory for gpu
    // and add its hip device index
    for(int i = 0; i < num_devices_;i ++){
        HIPCHECK(hipHostMalloc(&pdctl, sizeof(DeviceControl_t), hipHostMallocCoherent));
        pool_[i] = pdctl;
        pool_[i]->prev_gpu = nullptr;
        pool_[i]->next_gpu = nullptr;
        pool_[i]->hip_current_device_index = device_indices_[i];
        pool_[i]->src_buffer = nullptr;
        pool_[i]->dst_buffer = nullptr;
    }

    // create a ring of trackers
    pdctl = nullptr; // reuse later.
    HIPCHECK(hipHostGetDevicePointer((void**)&pdctl, pool_[0], 0));
    if(num_devices_ != 1) {
        pool_[1]->prev_gpu = pdctl;
    } else {
        pool_[0]->prev_gpu = pdctl;
    }

    pool_[num_devices_-1]->next_gpu = pdctl;

    for(unsigned i = 1; i < num_devices_; i++) {
        HIPCHECK(hipSetDevice(device_indices_[i]));
        HIPCHECK(hipHostGetDevicePointer((void**)&pdctl, pool_[i], 0));
        pool_[(i+1)%num_devices_]->prev_gpu = pdctl;
        pool_[(i-1)%num_devices_]->next_gpu = pdctl;
    }

    // restore users hip device index
    HIPCHECK(hipSetDevice(user_device_index));
}


RcclComm_t* DevTrackerPool_t::AddDevice(int device, int rank, int ndev) {
    RcclComm_t* ret_comm = new RcclComm_t;
    num_devices_ = ndev;
    ret_comm->num_devices_ = ndev;
    ret_comm->device_ = device;
    ret_comm->rank_ = rank;
    struct DeviceControl_t *pdctl;
    HIPCHECK(hipHostMalloc(&pdctl, sizeof(DeviceControl_t), hipHostMallocCoherent));
    pdctl->src_buffer = 0;
    pdctl->dst_buffer = 0;
    pdctl->prev_gpu = nullptr;
    pdctl->next_gpu = nullptr;
    pdctl->hip_current_device_index = device;

    if(pool_.find(rank) != pool_.end()) {
            // clean existing entry
    } else {
        pool_[rank] = pdctl;
    }

    if(pool_.size() == ndev) {
        pool_[1]->prev_gpu = pool_[0];
        pool_[ndev-1]->next_gpu = pool_[0];
        for(int i = 1; i < ndev; i++) {
            pool_[(i+1)%ndev]->prev_gpu = pool_[i];
            pool_[(i-1)%ndev]->next_gpu = pool_[i];
        }
    }
    ret_comm->track_ = pdctl;
    return ret_comm;
}

void DevTrackerPool_t::PrintAll() {
    for(int i = 0; i < num_devices_; i++) {
        std::cout<<"On Device: "<<device_indices_[i]<<std::endl;
        std::cout<<pool_[i]->prev_gpu<<std::endl;
        std::cout<<pool_[i]->next_gpu<<std::endl;
        std::cout<<pool_[i]->dst_buffer<<std::endl;
        std::cout<<pool_[i]->src_buffer<<std::endl;
    }
}

struct DeviceControl_t* DevTrackerPool_t::GetPoolByDeviceIndex(int device_index) {
    for(int i = 0; i < num_devices_; i++) {
        if(device_index == device_indices_[i]) {
            return pool_[i];
        }
    }
    return nullptr;
}
