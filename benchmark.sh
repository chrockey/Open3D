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

# Benchmark
echo "Running benchmarks from 1 to ${NPROC} threads."
for (( i = ${NPROC} ; i >= 1 ; i-- ));
do
    export OMP_NUM_THREADS=${i}
    echo "# OMP_NUM_THREADS: ${OMP_NUM_THREADS}"
    echo "######################################" >> ${OUT_FILE}
    echo "# OMP_NUM_THREADS: ${OMP_NUM_THREADS}" >> ${OUT_FILE}
    OMP_NUM_THREADS=${OMP_NUM_THREADS} ./bin/benchmarks --benchmark_filter="Zeros|Reduction|Voxel|Odometry|RegistrationICP" >> ${OUT_FILE} 2>&1
    echo "######################################" >> ${OUT_FILE}
    command
done

popd
