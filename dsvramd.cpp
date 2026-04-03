#include <fcntl.h>
#include <hip/hip_runtime.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <string>

static std::string read_text_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

extern "C" {
#include "dsvram_uapi.h"
}

static void hip_check(hipError_t e, const char* msg) {
    if (e != hipSuccess) {
        std::cerr << msg << ": " << hipGetErrorString(e) << "\n";
        std::exit(1);
    }
}

int main(int argc, char** argv) {
    unsigned target_gb = 98;
    unsigned physical_vram_gb = 48;

    if (argc >= 2) target_gb = std::strtoul(argv[1], nullptr, 10);
    if (argc >= 3) physical_vram_gb = std::strtoul(argv[2], nullptr, 10);

    int fd = open("/dev/dsvram", O_RDWR);
    if (fd < 0) {
        std::perror("open /dev/dsvram");
        return 1;
    }

    dsvram_cfg cfg{};
    cfg.version = 1;
    cfg.target_pool_gb = target_gb;
    cfg.physical_vram_gb = physical_vram_gb;
    cfg.flags = 0x1 | 0x2 | 0x4;
    cfg.compression_ppm = 10200; // 0.0102 retained
    cfg.mip_levels = 9;
    cfg.page_kb = 4;
    cfg.hotset_bytes = 12ULL * 1024ULL * 1024ULL * 1024ULL;
    cfg.coldset_bytes = (uint64_t)target_gb * 1024ULL * 1024ULL * 1024ULL - cfg.hotset_bytes;

    if (ioctl(fd, DSVRAM_IOC_SET_CFG, &cfg) != 0) {
        std::perror("ioctl SET_CFG");
        close(fd);
        return 1;
    }

    int deviceId = 0;
    hip_check(hipGetDevice(&deviceId), "hipGetDevice");

    std::size_t bytes = (std::size_t)target_gb * 1024ULL * 1024ULL * 1024ULL;
    std::cout << "Allocating managed pool: " << target_gb << " GB\n";

    unsigned char* pool = nullptr;
    hip_check(hipMallocManaged(&pool, bytes), "hipMallocManaged");

    hip_check(hipMemAdvise(pool, bytes, hipMemAdviseSetPreferredLocation, deviceId),
              "hipMemAdvise preferred location");
    hip_check(hipMemAdvise(pool, bytes, hipMemAdviseSetAccessedBy, deviceId),
              "hipMemAdvise accessed by device");
    hip_check(hipMemAdvise(pool, bytes, hipMemAdviseSetAccessedBy, hipCpuDeviceId),
              "hipMemAdvise accessed by cpu");

    // Sparse first-touch to avoid eagerly faulting the entire pool.
    constexpr std::size_t step = 2ULL * 1024ULL * 1024ULL;
    for (std::size_t i = 0; i < bytes; i += step) {
        pool[i] = static_cast<unsigned char>((i / step) & 0xff);
    }

    hip_check(hipMemPrefetchAsync(pool, cfg.hotset_bytes, deviceId, 0),
              "hipMemPrefetchAsync hotset to GPU");
    hip_check(hipDeviceSynchronize(), "hipDeviceSynchronize");

    std::cout << "Managed pool online.\n";
    std::cout << "HINT: export HSA_XNACK=1 before running on supported AMD GPUs.\n";
    std::cout << "Press Ctrl+C to exit.\n";

    while (true) {
        dsvram_stats st{};
        if (ioctl(fd, DSVRAM_IOC_GET_STATS, &st) == 0) {
            std::cout << "target=" << (st.target_pool_bytes >> 30)
                      << "GB mapped=" << (st.mapped_bytes >> 20)
                      << "MB compressed=" << (st.compressed_bytes >> 20)
                      << "MB evicted=" << (st.evicted_bytes >> 20)
                      << "MB\n";
        }
        sleep(5);
    }

    hipFree(pool);
    close(fd);
    return 0;
}
