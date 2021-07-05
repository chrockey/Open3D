#!/usr/bin/env bash
set -eu

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
BUILD_DIR=${SCRIPT_DIR}/build
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
# If there are multiple sets of benchmarks of the same code, only run one of them
BENCHMARK_FILTER="FromLegacyPointCloud|ToLegacyPointCloud|VoxelDownSample/core::HashmapBackend::TBB_0_01|Odometry|BenchmarkRegistrationICP/"

# Benchmark
echo "Running benchmarks from 1 to ${NPROC} threads."
for (( i = ${NPROC} ; i >= 1 ; i-- ));
do
    echo "######################################" >> ${OUT_FILE}

    export OMP_NUM_THREADS=${i}
    echo "# OMP_NUM_THREADS: ${OMP_NUM_THREADS}"
    echo "# OMP_NUM_THREADS: ${OMP_NUM_THREADS}" >> ${OUT_FILE}

    if [[ ${OMP_NUM_THREADS} = ${NPROC} ]]; then
        # Don't specify OMP_NUM_THREADS if it is the maximum already
        ./bin/benchmarks --benchmark_filter=${BENCHMARK_FILTER} >> ${OUT_FILE} 2>&1
    else
        OMP_NUM_THREADS=${OMP_NUM_THREADS} ./bin/benchmarks --benchmark_filter=${BENCHMARK_FILTER} >> ${OUT_FILE} 2>&1
    fi

    echo "######################################" >> ${OUT_FILE}
done

popd
