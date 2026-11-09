`nvml.h` is NVIDIA's public NVML header, unmodified, from the CUDA
redistributable package `cuda_nvml_dev` 13.5.37
(`cuda_nvml_dev-linux-x86_64-13.5.37-archive.tar.xz`,
https://developer.download.nvidia.com/compute/cuda/redist/). Its license
notice is at the top of the file. Only the public API comes from it; the
private export table nvidia-smi uses is described in `src/etbl.c`.
