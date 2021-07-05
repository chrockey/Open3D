#!/usr/bin/env bash
set -eu

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
BUILD_DIR=${SCRIPT_DIR}/build

# # Number of CPU cores, not counting hyper-threading:
# # https://stackoverflow.com/a/6481016/1255535
# #
# # Typically, set max # of threads to the # of physical cores, not logical:
# # https://www.thunderheadeng.com/2014/08/openmp-benchmarks/
# # https://stackoverflow.com/a/36959375/1255535
# NUM_CORES=$(grep ^cpu\\scores /proc/cpuinfo | uniq |  awk '{print $4}')
# export OMP_NUM_THREADS=${NUM_CORES}
# echo "OMP_NUM_THREADS: ${OMP_NUM_THREADS}"

NPROC=$(nproc)

pushd ${BUILD_DIR}

# Build
make benchmarks -j${NPROC}

# Create an empty output file
# Subsequent outputs will be appended
OUT_FILE=${BUILD_DIR}/benchmark.log
rm -rf ${OUT_FILE}
touch ${OUT_FILE}

# Run all tensor-related benchmarks
BENCHMARK_FILTER="FromLegacyPointCloud|ToLegacyPointCloud|VoxelDownSample/core|Odometry|RegistrationICP"

# Benchmark
echo "Running benchmarks from 1 to ${NPROC} threads."
for (( i = ${NPROC} ; i >= 1 ; i-- ));
do
    echo "######################################" >> ${OUT_FILE}

    export OMP_NUM_THREADS=${i}
    echo "# OMP_NUM_THREADS: ${OMP_NUM_THREADS}"

    if [[ ${OMP_NUM_THREADS} = ${NPROC} ]]; then
        # Don't specify OMP_NUM_THREADS if it is the maximum already
        echo "special"
        ./bin/benchmarks --benchmark_filter=${BENCHMARK_FILTER} >> ${OUT_FILE} 2>&1
    else
        echo "regular"
        OMP_NUM_THREADS=${OMP_NUM_THREADS} ./bin/benchmarks --benchmark_filter=${BENCHMARK_FILTER} >> ${OUT_FILE} 2>&1
    fi

    echo "######################################" >> ${OUT_FILE}
done

popd
