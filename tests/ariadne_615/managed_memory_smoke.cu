// Runtime correctness test for a Linux host running the ported UVM module.
// nvcc -O2 -o /tmp/ariadne-managed-smoke managed_memory_smoke.cu
// /tmp/ariadne-managed-smoke [allocation_MiB] [rounds] [pause_ms]
// Choose an allocation larger than GPU memory to exercise eviction/zero-copy.
#include <cuda_runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define CUDA_CHECK(call) do {                                             \
    cudaError_t error = (call);                                           \
    if (error != cudaSuccess) {                                           \
        fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, #call,      \
                cudaGetErrorString(error));                               \
        exit(1);                                                         \
    }                                                                    \
} while (0)

__global__ void update(uint32_t *data, size_t count, uint32_t increment, bool interleaved)
{
    size_t thread = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t threads = (size_t)gridDim.x * blockDim.x;
    // Both paths cover each element exactly once; interleaving changes which
    // SMs touch a VA block without introducing atomic races in the test.
    if (interleaved) {
        for (size_t index = thread; index < count; index += threads)
            data[index] += increment;
    }
    else {
        size_t per_thread = (count + threads - 1) / threads;
        size_t first = thread * per_thread;
        size_t end = first + per_thread;
        if (end > count)
            end = count;
        for (size_t index = first; index < end; ++index)
            data[index] += increment;
    }
}

static void check(const uint32_t *data, size_t count, uint32_t expected)
{
    for (size_t index = 0; index < count; ++index) {
        uint32_t value = (uint32_t)index ^ 0xa5a5a5a5u;
        if (data[index] != value + expected) {
            fprintf(stderr, "Mismatch at %zu: %u != %u\n", index, data[index], value + expected);
            exit(1);
        }
    }
}

int main(int argc, char **argv)
{
    char *end = NULL;
    unsigned long long mib = argc > 1 ? strtoull(argv[1], &end, 10) : 256;
    if ((argc > 1 && (!argv[1][0] || *end || argv[1][0] == '-')) ||
        mib == 0 || mib > SIZE_MAX / (1024 * 1024)) {
        fprintf(stderr, "Invalid allocation size in MiB\n");
        return 1;
    }
    unsigned long rounds = argc > 2 ? strtoul(argv[2], &end, 10) : 4;
    if ((argc > 2 && (!argv[2][0] || *end || argv[2][0] == '-')) || rounds == 0 || rounds > 10000) {
        fprintf(stderr, "Rounds must be between 1 and 10000\n");
        return 1;
    }
    unsigned long pause_ms = argc > 3 ? strtoul(argv[3], &end, 10) : 300;
    if ((argc > 3 && (!argv[3][0] || *end || argv[3][0] == '-')) || pause_ms > 3600000) {
        fprintf(stderr, "Pause must be between 0 and 3600000 milliseconds\n");
        return 1;
    }

    cudaDeviceProp device;
    CUDA_CHECK(cudaGetDeviceProperties(&device, 0));
    if (!device.managedMemory || !device.concurrentManagedAccess) {
        fprintf(stderr, "Test requires concurrent managed memory support\n");
        return 1;
    }
    size_t bytes = (size_t)mib * 1024 * 1024;
    size_t count = bytes / sizeof(uint32_t);
    uint32_t *data;
    CUDA_CHECK(cudaMallocManaged(&data, bytes));
    for (size_t index = 0; index < count; ++index)
        data[index] = (uint32_t)index ^ 0xa5a5a5a5u;

    uint32_t expected = 0;
    for (unsigned long round = 0; round < rounds; ++round) {
        update<<<device.multiProcessorCount * 4, 256>>>(data, count, 1, round & 1);
        CUDA_CHECK(cudaGetLastError());
        CUDA_CHECK(cudaDeviceSynchronize());
        ++expected;
        // Use a pause longer than the adaptive pin duration to test expiry.
        sleep(pause_ms / 1000);
        usleep((pause_ms % 1000) * 1000);
    }
    check(data, count, expected);

    // A small interior policy change splits a VA block. Issue it immediately
    // after GPU access so live synthetic mappings must be handled by split.
    update<<<device.multiProcessorCount * 4, 256>>>(data, count, 3, false);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());
    expected += 3;
    const size_t advice_bytes = 64 * 1024;
    uint32_t *interior = data + advice_bytes / sizeof(*data);
    CUDA_CHECK(cudaMemAdvise(interior, advice_bytes, cudaMemAdviseSetPreferredLocation, cudaCpuDeviceId));
    update<<<device.multiProcessorCount * 4, 256>>>(data, count, 2, true);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());
    expected += 2;
    check(data, count, expected);
    CUDA_CHECK(cudaMemAdvise(interior, advice_bytes, cudaMemAdviseUnsetPreferredLocation, cudaCpuDeviceId));

    // Explicit application placement must continue to work with ARIADNE on.
    CUDA_CHECK(cudaMemAdvise(data, bytes, cudaMemAdviseSetPreferredLocation, cudaCpuDeviceId));
    CUDA_CHECK(cudaMemAdvise(data, bytes, cudaMemAdviseSetAccessedBy, 0));
    update<<<device.multiProcessorCount * 4, 256>>>(data, count, 7, true);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());
    expected += 7;
    check(data, count, expected);
    CUDA_CHECK(cudaMemAdvise(data, bytes, cudaMemAdviseUnsetAccessedBy, 0));
    CUDA_CHECK(cudaMemAdvise(data, bytes, cudaMemAdviseUnsetPreferredLocation, cudaCpuDeviceId));
    CUDA_CHECK(cudaFree(data));
    printf("PASS: %llu MiB, %lu rounds, %lu ms pause, contiguous/interleaved access, range split and CPU placement\n",
           mib, rounds, pause_ms);
    return 0;
}
