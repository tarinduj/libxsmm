#!/usr/bin/env bash
###############################################################################
# Copyright (c) Intel Corporation - All rights reserved.                      #
# This file is part of the LIBXSMM library.                                   #
#                                                                             #
# For information on the license, see the LICENSE file.                       #
# Further information: https://github.com/libxsmm/libxsmm/                    #
# SPDX-License-Identifier: BSD-3-Clause                                       #
###############################################################################

HERE=$(cd "$(dirname "$0")" && pwd -P)
MATS=${HERE}/mats
#
# Build PyFR sample code with "make OMP=0".
# Consider fixing CPU clock frequency, and
# disabling all kinds of "turbo boost".
#
export OMP_PROC_BIND=${OMP_PROC_BIND:-TRUE}
export FSSPMDM_NBLOCK=${FSSPMDM_NBLOCK:-40}
export PERF_R=${PERF_R:-200000}
export PERF_N=${PERF_N:-40}

PERF_B=1
MATX=$(echo "${MATS}" | sed 's/\//\\\//g')
for MTX in "${MATS}"/p*/{pri,hex}/m{3,6}-sp.mtx; do
  MAT=$(echo "${MTX}" | sed "s/^${MATX}\///" | sed -n 's/\(.*[^.]\)\..*/\1/p')
  RESULT=$("${HERE}/pyfr_driver_asp_reg" "${MTX}" "${PERF_N}" "${PERF_R}" "${PERF_B}")
  SPARSE=$(echo "${RESULT}" | sed -n "s/[[:space:]][[:space:]]*LIBXSMM GFLOPS : \(..*\) (sparse)/\1/p")
  DENSE=$(echo "${RESULT}" | sed -n "s/[[:space:]][[:space:]]*LIBXSMM GFLOPS : \(..*\) (dense)/\1/p")
  BLAS=$(echo "${RESULT}" | sed -n "s/[[:space:]][[:space:]]*BLAS GFLOPS    : \(..*\)/\1/p")
  echo "${MAT} ${PERF_N} ${PERF_R} ${PERF_B} ${SPARSE} ${DENSE} ${BLAS}"
done

PERF_B=0
export FSSPMDM_NTS=0
for MTX in "${MATS}"/p*/{pri,hex}/m{0,132,460}-sp.mtx; do
  MAT=$(echo "${MTX}" | sed "s/^${MATX}\///" | sed -n 's/\(.*[^.]\)\..*/\1/p')
  RESULT=$("${HERE}/pyfr_driver_asp_reg" "${MTX}" "${PERF_N}" "${PERF_R}" "${PERF_B}")
  SPARSE=$(echo "${RESULT}" | sed -n "s/[[:space:]][[:space:]]*LIBXSMM GFLOPS : \(..*\) (sparse)/\1/p")
  DENSE=$(echo "${RESULT}" | sed -n "s/[[:space:]][[:space:]]*LIBXSMM GFLOPS : \(..*\) (dense)/\1/p")
  BLAS=$(echo "${RESULT}" | sed -n "s/[[:space:]][[:space:]]*BLAS GFLOPS    : \(..*\)/\1/p")
  echo "${MAT} ${PERF_N} ${PERF_R} ${PERF_B} ${SPARSE} ${DENSE} ${BLAS}"
done
