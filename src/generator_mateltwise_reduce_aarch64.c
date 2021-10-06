/******************************************************************************
* Copyright (c) Intel Corporation - All rights reserved.                      *
* This file is part of the LIBXSMM library.                                   *
*                                                                             *
* For information on the license, see the LICENSE file.                       *
* Further information: https://github.com/hfp/libxsmm/                        *
* SPDX-License-Identifier: BSD-3-Clause                                       *
******************************************************************************/
/* Evangelos Georganas (Intel Corp.)
******************************************************************************/
#include "generator_common_aarch64.h"
#include "generator_mateltwise_aarch64.h"
#include "generator_mateltwise_reduce_aarch64.h"
#include "generator_aarch64_instructions.h"
#include "generator_common.h"
#include "libxsmm_main.h"

#if !defined(LIBXSMM_GENERATOR_MATELTWISE_REDUCE_AARCH64_JUMP_LABEL_TRACKER_MALLOC)
# define LIBXSMM_GENERATOR_MATELTWISE_REDUCE_AARCH64_JUMP_LABEL_TRACKER_MALLOC
#endif

LIBXSMM_API_INTERN
void libxsmm_generator_reduce_cols_aarch64_microkernel( libxsmm_generated_code*                        io_generated_code,
    libxsmm_loop_label_tracker*                    io_loop_label_tracker,
    libxsmm_mateltwise_gp_reg_mapping*             i_gp_reg_mapping,
    const libxsmm_mateltwise_kernel_config*        i_micro_kernel_config,
    const libxsmm_meltw_descriptor*                i_mateltwise_desc ) {

  unsigned int _in, im, m, n, m_trips, use_m_masking, mask_count = 0, compute_squared_vals_reduce, compute_plain_vals_reduce;
  unsigned int start_vreg_sum = 0;
  unsigned int start_vreg_sum2 = 0;
  unsigned int reduce_instr = 0;
  unsigned int vmove_instruction_in = i_micro_kernel_config->vmove_instruction_in;
  unsigned int vmove_instruction_out = i_micro_kernel_config->vmove_instruction_out;
  unsigned int mask_reg = 0;
  unsigned int vlen = 4;
  unsigned int tmp_vreg = 31;
  unsigned int max_m_unrolling = 30;
  unsigned int m_unroll_factor = 30;
  unsigned int peeled_m_trips = 0;
  unsigned int m_trips_loop = 0;
  unsigned int accs_used = 0;
  unsigned int split_factor = 0;
  unsigned int n_trips = 0;
  unsigned int n_remainder = 0;
  unsigned int peeled_n_trips = 0;
  unsigned int flag_reduce_elts = ((i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_OP_ADD) ||
                                   (i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_X2_OP_ADD) ||
                                   (i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_OP_MAX)) ? 1 : 0;
  unsigned int flag_reduce_elts_sq = ((i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X2_OP_ADD) ||
                                   (i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_X2_OP_ADD)) ? 1 : 0;
  unsigned int flag_reduce_op_add =((i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_OP_ADD) ||
                                   (i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_X2_OP_ADD) ||
                                   (i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X2_OP_ADD)) ? 1 : 0;
  unsigned int flag_reduce_op_max = (i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_OP_MAX) ? 1 : 0;

  LIBXSMM_UNUSED(mask_reg);
  LIBXSMM_UNUSED(mask_count);
  LIBXSMM_UNUSED(vmove_instruction_in);
  LIBXSMM_UNUSED(vmove_instruction_out);
  /* Some rudimentary checking of M, N and LDs*/
  if ( i_mateltwise_desc->m > i_mateltwise_desc->ldi ) {
    LIBXSMM_HANDLE_ERROR( io_generated_code, LIBXSMM_ERR_LDA );
    return;
  }

  if ( flag_reduce_elts > 0 ) {
    compute_plain_vals_reduce= 1;
  } else {
    compute_plain_vals_reduce= 0;
  }

  if ( flag_reduce_elts_sq > 0 ) {
    compute_squared_vals_reduce = 1;
  } else {
    compute_squared_vals_reduce = 0;
  }

  if ( flag_reduce_op_add > 0 ) {
    reduce_instr = LIBXSMM_AARCH64_INSTR_ASIMD_FADD_V;
  } else if ( flag_reduce_op_max > 0 ) {
    reduce_instr = LIBXSMM_AARCH64_INSTR_ASIMD_FMAX_V;
  } else {
    /* This should not happen  */
    printf("Only supported reduction OPs are ADD and MAX for this reduce kernel\n");
    LIBXSMM_HANDLE_ERROR( io_generated_code, LIBXSMM_ERR_GENERAL );
    return;
  }

  if ((compute_squared_vals_reduce > 0) && (reduce_instr != LIBXSMM_AARCH64_INSTR_ASIMD_FADD_V)) {
    /* This should not happen  */
    printf("Support for squares's reduction only when reduction OP is ADD\n");
    LIBXSMM_HANDLE_ERROR( io_generated_code, LIBXSMM_ERR_GENERAL );
    return;
  }

  /* Configure the register mapping for this eltwise kernel */
  i_gp_reg_mapping->gp_reg_in                   = LIBXSMM_AARCH64_GP_REG_X9;
  i_gp_reg_mapping->gp_reg_reduced_elts         = LIBXSMM_AARCH64_GP_REG_X10;
  i_gp_reg_mapping->gp_reg_reduced_elts_squared = LIBXSMM_AARCH64_GP_REG_X11;
  i_gp_reg_mapping->gp_reg_m_loop               = LIBXSMM_AARCH64_GP_REG_X12;
  i_gp_reg_mapping->gp_reg_n_loop               = LIBXSMM_AARCH64_GP_REG_X13;
  i_gp_reg_mapping->gp_reg_scratch_0            = LIBXSMM_AARCH64_GP_REG_X14;

  /* load the input pointer and output pointer */
  libxsmm_aarch64_instruction_alu_move( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_LDR_I_OFF, i_gp_reg_mapping->gp_reg_param_struct, LIBXSMM_AARCH64_GP_REG_UNDEF, 32, i_gp_reg_mapping->gp_reg_in );

  if ( compute_plain_vals_reduce > 0 ) {
    libxsmm_aarch64_instruction_alu_move( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_LDR_I_OFF, i_gp_reg_mapping->gp_reg_param_struct, LIBXSMM_AARCH64_GP_REG_UNDEF, 56, i_gp_reg_mapping->gp_reg_reduced_elts );
    if ( compute_squared_vals_reduce > 0 ) {
      unsigned int result_size = i_mateltwise_desc->ldo * i_micro_kernel_config->datatype_size_out;
      libxsmm_aarch64_instruction_alu_compute_imm64( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_META_ADD, i_gp_reg_mapping->gp_reg_reduced_elts, i_gp_reg_mapping->gp_reg_scratch_0, i_gp_reg_mapping->gp_reg_reduced_elts_squared, result_size );
    }
  } else {
    libxsmm_aarch64_instruction_alu_move( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_LDR_I_OFF, i_gp_reg_mapping->gp_reg_param_struct, LIBXSMM_AARCH64_GP_REG_UNDEF, 56, i_gp_reg_mapping->gp_reg_reduced_elts_squared );
  }

  /* We fully unroll in N dimension, calculate m-mask if there is remainder */
  m                 = i_mateltwise_desc->m;
  n                 = i_mateltwise_desc->n;
  use_m_masking     = ( m % vlen == 0 ) ? 0 : 1;

  /* Calculate input mask in case we see m_masking */
  if (use_m_masking == 1) {
    printf("Support for m %% 4 == 0 cases\n");
    LIBXSMM_HANDLE_ERROR( io_generated_code, LIBXSMM_ERR_GENERAL );
    return;
#if 0
    /* Calculate mask reg 1 for input-reading */
    if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
      mask_count =  vlen - (m % vlen);
      mask_reg = 1;
      libxsmm_generator_mateltwise_initialize_avx512_mask(io_generated_code, LIBXSMM_X86_GP_REG_RAX, mask_reg, mask_count, LIBXSMM_GEMM_PRECISION_F32);
    } else {
      mask_reg = 15;
      tmp_vreg = 14;
      libxsmm_generator_mateltwise_initialize_avx_mask(io_generated_code, mask_reg, m % vlen);
      max_m_unrolling--;
    }
#endif
  }

  m_trips           = (m + vlen - 1) / vlen;
  m_unroll_factor   = m_trips;
  m_trips_loop      = 1;
  peeled_m_trips    = 0;

  if ( (compute_plain_vals_reduce > 0) && (compute_squared_vals_reduce > 0) ) {
    max_m_unrolling = max_m_unrolling/2;
    start_vreg_sum2 = max_m_unrolling;
  }

  /* In this case we have to generate a loop for m */
  if (m_unroll_factor > max_m_unrolling) {
    m_unroll_factor = max_m_unrolling;
    m_trips_loop = m_trips/m_unroll_factor;
    peeled_m_trips = m_trips  - m_unroll_factor * m_trips_loop;
    if ((use_m_masking > 0) && (peeled_m_trips == 0)) {
      m_trips_loop--;
      peeled_m_trips = m_trips  - m_unroll_factor * m_trips_loop;
    }
  } else {
    if ((use_m_masking > 0) && (peeled_m_trips == 0)) {
      m_trips_loop--;
      peeled_m_trips = m_trips  - m_unroll_factor * m_trips_loop;
    }
  }

  accs_used = m_unroll_factor;
  if (max_m_unrolling/accs_used > 1) {
    split_factor = max_m_unrolling/accs_used;
  } else {
    split_factor = 1;
  }

  n_trips = (n+split_factor-1)/split_factor;
  n_remainder = n % split_factor;
  peeled_n_trips = 0;
  if (n_remainder > 0) {
    n_trips--;
    peeled_n_trips = n - n_trips * split_factor;
  }

  if ( m_trips_loop >= 1 ) {
    if ( m_trips_loop > 1 ) {
      libxsmm_generator_loop_header_aarch64(io_generated_code, io_loop_label_tracker, i_gp_reg_mapping->gp_reg_m_loop, m_trips_loop);
    }
    /* Initialize accumulators to zero */
    for (_in = 0; _in < split_factor; _in++) {
      for (im = 0; im < m_unroll_factor; im++) {
        if ( compute_plain_vals_reduce > 0 ) {
          if ( flag_reduce_op_add > 0 ) {
            libxsmm_aarch64_instruction_asimd_compute( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_EOR_V,
                start_vreg_sum + im + _in * m_unroll_factor, start_vreg_sum + im + _in * m_unroll_factor, 0, start_vreg_sum + im + _in * m_unroll_factor, LIBXSMM_AARCH64_ASIMD_TUPLETYPE_16B );
          } else if ( flag_reduce_op_max > 0 ) {
            libxsmm_aarch64_instruction_asimd_move( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_LDR_I_POST,
                                                i_gp_reg_mapping->gp_reg_in, LIBXSMM_AARCH64_GP_REG_UNDEF, 16,
                                                start_vreg_sum + im + _in * m_unroll_factor,
                                                LIBXSMM_AARCH64_ASIMD_WIDTH_Q );
          }
        }

        if ( compute_squared_vals_reduce > 0 ) {
          libxsmm_aarch64_instruction_asimd_compute( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_EOR_V,
              start_vreg_sum2 + im + _in * m_unroll_factor, start_vreg_sum2 + im + _in * m_unroll_factor, 0, start_vreg_sum2 + im + _in * m_unroll_factor, LIBXSMM_AARCH64_ASIMD_TUPLETYPE_16B );
        }
      }
      if ( flag_reduce_op_max > 0 ) {
        libxsmm_aarch64_instruction_alu_compute_imm64( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_META_SUB,
                                                      i_gp_reg_mapping->gp_reg_in, i_gp_reg_mapping->gp_reg_scratch_0, i_gp_reg_mapping->gp_reg_in,
                                                      (unsigned long long)m_unroll_factor * (unsigned long long)16 );
      }
    }

    if (n_trips >= 1) {
      if (n_trips > 1) {
        libxsmm_generator_loop_header_aarch64(io_generated_code, io_loop_label_tracker, i_gp_reg_mapping->gp_reg_n_loop, n_trips);
      }

      for (_in = 0; _in < split_factor; _in++) {
        int  extra_ld_bytes  = i_mateltwise_desc->ldi * i_micro_kernel_config->datatype_size_in - m_unroll_factor * 16;
        for (im = 0; im < m_unroll_factor; im++) {
          libxsmm_aarch64_instruction_asimd_move( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_LDR_I_POST,
                                              i_gp_reg_mapping->gp_reg_in, LIBXSMM_AARCH64_GP_REG_UNDEF, 16,
                                              tmp_vreg,
                                              LIBXSMM_AARCH64_ASIMD_WIDTH_Q );

          if ( compute_plain_vals_reduce > 0 ) {
            libxsmm_aarch64_instruction_asimd_compute( io_generated_code, reduce_instr,
                                                      tmp_vreg, start_vreg_sum + im + _in * m_unroll_factor, 0, start_vreg_sum + im + _in * m_unroll_factor,
                                                      (i_micro_kernel_config->datatype_size_in == 4) ? LIBXSMM_AARCH64_ASIMD_TUPLETYPE_4S : LIBXSMM_AARCH64_ASIMD_TUPLETYPE_2D );
          }

          if ( compute_squared_vals_reduce > 0 ) {
            libxsmm_aarch64_instruction_asimd_compute( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_FMLA_V,
                                                      tmp_vreg, tmp_vreg, 0, start_vreg_sum2 + im + _in * m_unroll_factor,
                                                      (i_micro_kernel_config->datatype_size_in == 4) ? LIBXSMM_AARCH64_ASIMD_TUPLETYPE_4S : LIBXSMM_AARCH64_ASIMD_TUPLETYPE_2D );
          }
        }
        if (extra_ld_bytes > 0) {
          libxsmm_aarch64_instruction_alu_compute_imm64( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_META_ADD,
                                                        i_gp_reg_mapping->gp_reg_in, i_gp_reg_mapping->gp_reg_scratch_0, i_gp_reg_mapping->gp_reg_in, extra_ld_bytes );
        }
      }

      if (n_trips > 1) {
        libxsmm_generator_loop_footer_aarch64(io_generated_code, io_loop_label_tracker, i_gp_reg_mapping->gp_reg_n_loop, 1);
      }
    }

    if (peeled_n_trips > 0) {
      for (_in = 0; _in < peeled_n_trips; _in++) {
        int  extra_ld_bytes  = i_mateltwise_desc->ldi * i_micro_kernel_config->datatype_size_in - m_unroll_factor * 16;
        for (im = 0; im < m_unroll_factor; im++) {
          libxsmm_aarch64_instruction_asimd_move( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_LDR_I_POST,
                                              i_gp_reg_mapping->gp_reg_in, LIBXSMM_AARCH64_GP_REG_UNDEF, 16,
                                              tmp_vreg,
                                              LIBXSMM_AARCH64_ASIMD_WIDTH_Q );

          if ( compute_plain_vals_reduce > 0 ) {
            libxsmm_aarch64_instruction_asimd_compute( io_generated_code, reduce_instr,
                                                      tmp_vreg, start_vreg_sum + im + _in * m_unroll_factor, 0, start_vreg_sum + im + _in * m_unroll_factor,
                                                      (i_micro_kernel_config->datatype_size_in == 4) ? LIBXSMM_AARCH64_ASIMD_TUPLETYPE_4S : LIBXSMM_AARCH64_ASIMD_TUPLETYPE_2D );
          }

          if ( compute_squared_vals_reduce > 0 ) {
            libxsmm_aarch64_instruction_asimd_compute( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_FMLA_V,
                                                      tmp_vreg, tmp_vreg, 0, start_vreg_sum2 + im + _in * m_unroll_factor,
                                                      (i_micro_kernel_config->datatype_size_in == 4) ? LIBXSMM_AARCH64_ASIMD_TUPLETYPE_4S : LIBXSMM_AARCH64_ASIMD_TUPLETYPE_2D );
          }
        }
        if (extra_ld_bytes > 0) {
          libxsmm_aarch64_instruction_alu_compute_imm64( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_META_ADD,
                                                        i_gp_reg_mapping->gp_reg_in, i_gp_reg_mapping->gp_reg_scratch_0, i_gp_reg_mapping->gp_reg_in, extra_ld_bytes );
        }
      }
    }

    libxsmm_aarch64_instruction_alu_compute_imm64( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_META_SUB,
                                                  i_gp_reg_mapping->gp_reg_in, i_gp_reg_mapping->gp_reg_scratch_0, i_gp_reg_mapping->gp_reg_in,
                                                  (unsigned long long)n * (unsigned long long)i_mateltwise_desc->ldi * (unsigned long long)i_micro_kernel_config->datatype_size_in - (unsigned long long)m_unroll_factor * (unsigned long long)vlen *  (unsigned long long)i_micro_kernel_config->datatype_size_in  );

    for (_in = 1; _in < split_factor; _in++) {
      for (im = 0; im < m_unroll_factor; im++) {
        if ( compute_plain_vals_reduce > 0 ) {
          libxsmm_aarch64_instruction_asimd_compute( io_generated_code, reduce_instr,
                                                    start_vreg_sum + im, start_vreg_sum + im + _in * m_unroll_factor, 0, start_vreg_sum + im,
                                                    (i_micro_kernel_config->datatype_size_in == 4) ? LIBXSMM_AARCH64_ASIMD_TUPLETYPE_4S : LIBXSMM_AARCH64_ASIMD_TUPLETYPE_2D );
        }

        if ( compute_squared_vals_reduce > 0 ) {
          libxsmm_aarch64_instruction_asimd_compute( io_generated_code, reduce_instr,
                                                    start_vreg_sum2 + im, start_vreg_sum2 + im + _in * m_unroll_factor, 0, start_vreg_sum2 + im,
                                                    (i_micro_kernel_config->datatype_size_in == 4) ? LIBXSMM_AARCH64_ASIMD_TUPLETYPE_4S : LIBXSMM_AARCH64_ASIMD_TUPLETYPE_2D );
        }
      }
    }

    /* Store computed results */
    for (im = 0; im < m_unroll_factor; im++) {
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_aarch64_instruction_asimd_move( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_STR_I_POST,
                                                i_gp_reg_mapping->gp_reg_reduced_elts, LIBXSMM_AARCH64_GP_REG_UNDEF, 16,
                                                start_vreg_sum + im,
                                                LIBXSMM_AARCH64_ASIMD_WIDTH_Q );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_aarch64_instruction_asimd_move( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_STR_I_POST,
                                                i_gp_reg_mapping->gp_reg_reduced_elts_squared, LIBXSMM_AARCH64_GP_REG_UNDEF, 16,
                                                start_vreg_sum2 + im,
                                                LIBXSMM_AARCH64_ASIMD_WIDTH_Q );
      }
    }

    if (m_trips_loop > 1) {
      libxsmm_generator_loop_footer_aarch64(io_generated_code, io_loop_label_tracker, i_gp_reg_mapping->gp_reg_m_loop, 1);
    }
  }

  if (peeled_m_trips > 0) {
    for (_in = 0; _in < split_factor; _in++) {
      for (im = 0; im < peeled_m_trips; im++) {
        if ( compute_plain_vals_reduce > 0 ) {
          if ( flag_reduce_op_add > 0 ) {
            libxsmm_aarch64_instruction_asimd_compute( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_EOR_V,
                start_vreg_sum + im + _in * m_unroll_factor, start_vreg_sum + im + _in * m_unroll_factor, 0, start_vreg_sum + im + _in * m_unroll_factor, LIBXSMM_AARCH64_ASIMD_TUPLETYPE_16B );
          } else if ( flag_reduce_op_max > 0 ) {
            libxsmm_aarch64_instruction_asimd_move( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_LDR_I_POST,
                                                i_gp_reg_mapping->gp_reg_in, LIBXSMM_AARCH64_GP_REG_UNDEF, 16,
                                                start_vreg_sum + im + _in * m_unroll_factor,
                                                LIBXSMM_AARCH64_ASIMD_WIDTH_Q );
          }
        }

        if ( compute_squared_vals_reduce > 0 ) {
          libxsmm_aarch64_instruction_asimd_compute( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_EOR_V,
              start_vreg_sum2 + im + _in * m_unroll_factor, start_vreg_sum2 + im + _in * m_unroll_factor, 0, start_vreg_sum2 + im + _in * m_unroll_factor, LIBXSMM_AARCH64_ASIMD_TUPLETYPE_16B );
        }
      }
      if ( flag_reduce_op_max > 0 ) {
        libxsmm_aarch64_instruction_alu_compute_imm64( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_META_SUB,
                                                      i_gp_reg_mapping->gp_reg_in, i_gp_reg_mapping->gp_reg_scratch_0, i_gp_reg_mapping->gp_reg_in,
                                                      (unsigned long long)peeled_m_trips * (unsigned long long)16 );
      }
    }


    if (n_trips >= 1) {
      if (n_trips > 1) {
        libxsmm_generator_loop_header_aarch64(io_generated_code, io_loop_label_tracker, i_gp_reg_mapping->gp_reg_n_loop, n_trips);
      }

      for (_in = 0; _in < split_factor; _in++) {
        int extra_ld_bytes  = i_mateltwise_desc->ldi * i_micro_kernel_config->datatype_size_in - peeled_m_trips * 16;
        for (im = 0; im < peeled_m_trips; im++) {
          libxsmm_aarch64_instruction_asimd_move( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_LDR_I_POST,
                                              i_gp_reg_mapping->gp_reg_in, LIBXSMM_AARCH64_GP_REG_UNDEF, 16,
                                              tmp_vreg,
                                              LIBXSMM_AARCH64_ASIMD_WIDTH_Q );

          if ( compute_plain_vals_reduce > 0 ) {
            libxsmm_aarch64_instruction_asimd_compute( io_generated_code, reduce_instr,
                                                      tmp_vreg, start_vreg_sum + im + _in * m_unroll_factor, 0, start_vreg_sum + im + _in * m_unroll_factor,
                                                      (i_micro_kernel_config->datatype_size_in == 4) ? LIBXSMM_AARCH64_ASIMD_TUPLETYPE_4S : LIBXSMM_AARCH64_ASIMD_TUPLETYPE_2D );
          }

          if ( compute_squared_vals_reduce > 0 ) {
            libxsmm_aarch64_instruction_asimd_compute( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_FMLA_V,
                                                      tmp_vreg, tmp_vreg, 0, start_vreg_sum2 + im + _in * m_unroll_factor,
                                                      (i_micro_kernel_config->datatype_size_in == 4) ? LIBXSMM_AARCH64_ASIMD_TUPLETYPE_4S : LIBXSMM_AARCH64_ASIMD_TUPLETYPE_2D );
          }
        }
        if (extra_ld_bytes > 0) {
          libxsmm_aarch64_instruction_alu_compute_imm64( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_META_ADD,
                                                        i_gp_reg_mapping->gp_reg_in, i_gp_reg_mapping->gp_reg_scratch_0, i_gp_reg_mapping->gp_reg_in, extra_ld_bytes );
        }
      }

      if (n_trips > 1) {
        libxsmm_generator_loop_footer_aarch64(io_generated_code, io_loop_label_tracker, i_gp_reg_mapping->gp_reg_n_loop, 1);
      }
    }

    if (peeled_n_trips > 0) {
      for (_in = 0; _in < peeled_n_trips; _in++) {
        int  extra_ld_bytes  = i_mateltwise_desc->ldi * i_micro_kernel_config->datatype_size_in - peeled_m_trips * 16;
        for (im = 0; im < peeled_m_trips; im++) {
          libxsmm_aarch64_instruction_asimd_move( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_LDR_I_POST,
                                              i_gp_reg_mapping->gp_reg_in, LIBXSMM_AARCH64_GP_REG_UNDEF, 16,
                                              tmp_vreg,
                                              LIBXSMM_AARCH64_ASIMD_WIDTH_Q );

          if ( compute_plain_vals_reduce > 0 ) {
            libxsmm_aarch64_instruction_asimd_compute( io_generated_code, reduce_instr,
                                                      tmp_vreg, start_vreg_sum + im + _in * m_unroll_factor, 0, start_vreg_sum + im + _in * m_unroll_factor,
                                                      (i_micro_kernel_config->datatype_size_in == 4) ? LIBXSMM_AARCH64_ASIMD_TUPLETYPE_4S : LIBXSMM_AARCH64_ASIMD_TUPLETYPE_2D );
          }

          if ( compute_squared_vals_reduce > 0 ) {
            libxsmm_aarch64_instruction_asimd_compute( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_FMLA_V,
                                                      tmp_vreg, tmp_vreg, 0, start_vreg_sum2 + im + _in * m_unroll_factor,
                                                      (i_micro_kernel_config->datatype_size_in == 4) ? LIBXSMM_AARCH64_ASIMD_TUPLETYPE_4S : LIBXSMM_AARCH64_ASIMD_TUPLETYPE_2D );
          }
        }
        if (extra_ld_bytes > 0) {
          libxsmm_aarch64_instruction_alu_compute_imm64( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_META_ADD,
                                                        i_gp_reg_mapping->gp_reg_in, i_gp_reg_mapping->gp_reg_scratch_0, i_gp_reg_mapping->gp_reg_in, extra_ld_bytes );
        }
      }
    }

    libxsmm_aarch64_instruction_alu_compute_imm64( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_META_SUB,
                                                  i_gp_reg_mapping->gp_reg_in, i_gp_reg_mapping->gp_reg_scratch_0, i_gp_reg_mapping->gp_reg_in,
                                                  (unsigned long long)n * (unsigned long long)i_mateltwise_desc->ldi * (unsigned long long)i_micro_kernel_config->datatype_size_in - (unsigned long long)peeled_m_trips * (unsigned long long)vlen *  (unsigned long long)i_micro_kernel_config->datatype_size_in  );

    for (_in = 1; _in < split_factor; _in++) {
      for (im = 0; im < peeled_m_trips; im++) {
        if ( compute_plain_vals_reduce > 0 ) {
          libxsmm_aarch64_instruction_asimd_compute( io_generated_code, reduce_instr,
                                                    start_vreg_sum + im, start_vreg_sum + im + _in * m_unroll_factor, 0, start_vreg_sum + im,
                                                    (i_micro_kernel_config->datatype_size_in == 4) ? LIBXSMM_AARCH64_ASIMD_TUPLETYPE_4S : LIBXSMM_AARCH64_ASIMD_TUPLETYPE_2D );
        }

        if ( compute_squared_vals_reduce > 0 ) {
          libxsmm_aarch64_instruction_asimd_compute( io_generated_code, reduce_instr,
                                                    start_vreg_sum2 + im, start_vreg_sum2 + im + _in * m_unroll_factor, 0, start_vreg_sum2 + im,
                                                    (i_micro_kernel_config->datatype_size_in == 4) ? LIBXSMM_AARCH64_ASIMD_TUPLETYPE_4S : LIBXSMM_AARCH64_ASIMD_TUPLETYPE_2D );
        }
      }
    }

    /* Store computed results */
    for (im = 0; im < peeled_m_trips; im++) {
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_aarch64_instruction_asimd_move( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_STR_I_POST,
                                                i_gp_reg_mapping->gp_reg_reduced_elts, LIBXSMM_AARCH64_GP_REG_UNDEF, 16,
                                                start_vreg_sum + im,
                                                LIBXSMM_AARCH64_ASIMD_WIDTH_Q );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_aarch64_instruction_asimd_move( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_STR_I_POST,
                                                i_gp_reg_mapping->gp_reg_reduced_elts_squared, LIBXSMM_AARCH64_GP_REG_UNDEF, 16,
                                                start_vreg_sum2 + im,
                                                LIBXSMM_AARCH64_ASIMD_WIDTH_Q );
      }
    }
  }
}

#if 0
LIBXSMM_API_INTERN
void libxsmm_generator_reduce_rows_avx512_microkernel( libxsmm_generated_code*                        io_generated_code,
    libxsmm_loop_label_tracker*                    io_loop_label_tracker,
    libxsmm_mateltwise_gp_reg_mapping*             i_gp_reg_mapping,
    const libxsmm_mateltwise_kernel_config*        i_micro_kernel_config,
    const libxsmm_meltw_descriptor*                i_mateltwise_desc ) {

  unsigned int i = 0, im, m, n, m_trips, n_trips, n_full_trips, use_m_masking, use_n_masking, mask_in_count, mask_out_count, n_cols_load = 16, compute_squared_vals_reduce, compute_plain_vals_reduce;
  unsigned int reduce_instr = 0;
  unsigned int aux_vreg = 24;
  char  vname_in = i_micro_kernel_config->vector_name;
  char  vname_out = i_micro_kernel_config->vector_name;
  unsigned int vmove_instruction_in = i_micro_kernel_config->vmove_instruction_in;
  unsigned int vmove_instruction_out = i_micro_kernel_config->vmove_instruction_out;
  unsigned int flag_reduce_elts = ((i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_OP_ADD) ||
                                   (i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_X2_OP_ADD) ||
                                   (i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_OP_MAX)) ? 1 : 0;
  unsigned int flag_reduce_elts_sq = ((i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X2_OP_ADD) ||
                                   (i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_X2_OP_ADD)) ? 1 : 0;
  unsigned int flag_reduce_op_add =((i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_OP_ADD) ||
                                   (i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_X2_OP_ADD) ||
                                   (i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X2_OP_ADD)) ? 1 : 0;
  unsigned int flag_reduce_op_max = (i_mateltwise_desc->param == LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_OP_MAX) ? 1 : 0;

  if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) {
    vname_in = 'y';
    vmove_instruction_in = LIBXSMM_X86_INSTR_VMOVDQU16;
  }

  if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
    vname_out = 'y';
    vmove_instruction_out = LIBXSMM_X86_INSTR_VMOVDQU16;
  }

  /* Some rudimentary checking of M, N and LDs*/
  if ( i_mateltwise_desc->m > i_mateltwise_desc->ldi ) {
    LIBXSMM_HANDLE_ERROR( io_generated_code, LIBXSMM_ERR_LDA );
    return;
  }

  if ( flag_reduce_elts > 0 ) {
    compute_plain_vals_reduce= 1;
  } else {
    compute_plain_vals_reduce= 0;
  }

  if ( flag_reduce_elts_sq > 0 ) {
    compute_squared_vals_reduce = 1;
  } else {
    compute_squared_vals_reduce = 0;
  }

  if ( flag_reduce_op_add > 0 ) {
    reduce_instr = LIBXSMM_X86_INSTR_VADDPS;
  } else if ( flag_reduce_op_max > 0 ) {
    reduce_instr = LIBXSMM_X86_INSTR_VMAXPS;
  } else {
    /* This should not happen  */
    printf("Only supported reduction OPs are ADD and MAX for this reduce kernel\n");
    LIBXSMM_HANDLE_ERROR( io_generated_code, LIBXSMM_ERR_GENERAL );
    return;
  }

  if ((compute_squared_vals_reduce > 0) && (reduce_instr != LIBXSMM_X86_INSTR_VADDPS)) {
    /* This should not happen  */
    printf("Support for squares's reduction only when reduction OP is ADD\n");
    LIBXSMM_HANDLE_ERROR( io_generated_code, LIBXSMM_ERR_GENERAL );
    return;
  }

  /* Configure the register mapping for this eltwise kernel */
  i_gp_reg_mapping->gp_reg_in                   = LIBXSMM_X86_GP_REG_R8;
  i_gp_reg_mapping->gp_reg_reduced_elts         = LIBXSMM_X86_GP_REG_R9;
  i_gp_reg_mapping->gp_reg_reduced_elts_squared = LIBXSMM_X86_GP_REG_R10;
  i_gp_reg_mapping->gp_reg_m_loop               = LIBXSMM_X86_GP_REG_R11;
  i_gp_reg_mapping->gp_reg_n_loop               = LIBXSMM_X86_GP_REG_RAX;

  /* load the input pointer and output pointer */
  libxsmm_x86_instruction_alu_mem( io_generated_code,
      i_micro_kernel_config->alu_mov_instruction,
      i_gp_reg_mapping->gp_reg_param_struct,
      LIBXSMM_X86_GP_REG_UNDEF, 0,
      32,
      i_gp_reg_mapping->gp_reg_in,
      0 );

  if ( compute_plain_vals_reduce > 0 ) {
    libxsmm_x86_instruction_alu_mem( io_generated_code,
        i_micro_kernel_config->alu_mov_instruction,
       i_gp_reg_mapping->gp_reg_param_struct,
       LIBXSMM_X86_GP_REG_UNDEF, 0,
       56,
       i_gp_reg_mapping->gp_reg_reduced_elts,
       0 );
    if ( compute_squared_vals_reduce > 0 ) {
      unsigned int result_size = i_mateltwise_desc->n * i_micro_kernel_config->datatype_size_out;
      libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_mov_instruction, i_gp_reg_mapping->gp_reg_reduced_elts, i_gp_reg_mapping->gp_reg_reduced_elts_squared);
      libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_ADDQ, i_gp_reg_mapping->gp_reg_reduced_elts_squared, result_size);
    }
  } else {
    libxsmm_x86_instruction_alu_mem( io_generated_code,
        i_micro_kernel_config->alu_mov_instruction,
       i_gp_reg_mapping->gp_reg_param_struct,
       LIBXSMM_X86_GP_REG_UNDEF, 0,
       56,
       i_gp_reg_mapping->gp_reg_reduced_elts_squared,
       0 );
  }

  /* In this case we don't support the algorithm with "on the fly transpose" */
  if (io_generated_code->arch < LIBXSMM_X86_AVX512) {
    unsigned int reg_sum = 15, reg_sum_squared = 14;
    unsigned int cur_vreg;
    unsigned int vlen = 8;
    unsigned int mask_out = 13;
    unsigned int available_vregs = 13;
    unsigned int mask_reg = 0;

    m                 = i_mateltwise_desc->m;
    n                 = i_mateltwise_desc->n;
    use_m_masking     = ( m % vlen == 0 ) ? 0 : 1;
    m_trips           = ( m+vlen-1 )/ vlen;
    im = 0;

    if ((use_m_masking > 0) && ( flag_reduce_op_max > 0 )) {
      aux_vreg = available_vregs - 1;
      available_vregs--;
      libxsmm_generator_load_vreg_minus_infinity(io_generated_code, 'y', LIBXSMM_X86_GP_REG_RAX, aux_vreg);
    }

    libxsmm_generator_mateltwise_initialize_avx_mask(io_generated_code, mask_out, 1);
    /* Calculate input mask in case we see m_masking */
    if (use_m_masking == 1) {
      mask_reg = available_vregs-1;
      libxsmm_generator_mateltwise_initialize_avx_mask(io_generated_code, mask_reg, m % vlen);
      available_vregs--;
    }

    if (n > 1) {
      /* open n loop */
      libxsmm_generator_mateltwise_header_n_loop(  io_generated_code, io_loop_label_tracker, i_micro_kernel_config, i_gp_reg_mapping->gp_reg_n_loop );
    }

    /* Initialize accumulators to zero */
    if ( compute_plain_vals_reduce > 0 ) {
      if ( flag_reduce_op_add > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VPXORD,
                                                 i_micro_kernel_config->vector_name,
                                                 reg_sum, reg_sum, reg_sum );
      } else if ( flag_reduce_op_max > 0 ) {
        libxsmm_x86_instruction_unified_vec_move( io_generated_code,
            vmove_instruction_in,
            i_gp_reg_mapping->gp_reg_in,
            LIBXSMM_X86_GP_REG_UNDEF, 0,
            0,
            vname_in,
            reg_sum, ((use_m_masking == 1) && (im == (m_trips-1))) ? 1 : 0, ((use_m_masking == 1) && (im == (m_trips-1))) ? mask_reg : 0, 0);

        /* If we have remainder, then we want to blend in -INF for the zero'ed out entries */
        if ((use_m_masking == 1) && (im == (m_trips-1))) {
          libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8(io_generated_code,
                    LIBXSMM_X86_INSTR_VBLENDVPS,
                    'y',
                    reg_sum,
                    aux_vreg,
                    reg_sum,
                    0, 0, 0, (mask_reg) << 4);
        }
      }
    }

    if ( compute_squared_vals_reduce > 0 ) {
      libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                               LIBXSMM_X86_INSTR_VPXORD,
                                               i_micro_kernel_config->vector_name,
                                               reg_sum_squared, reg_sum_squared, reg_sum_squared );
    }

    for (im = 0; im < m_trips; im++) {
      cur_vreg = im % available_vregs;
      libxsmm_x86_instruction_unified_vec_move( io_generated_code,
          vmove_instruction_in,
          i_gp_reg_mapping->gp_reg_in,
          LIBXSMM_X86_GP_REG_UNDEF, 0,
          im * vlen * i_micro_kernel_config->datatype_size_in,
          vname_in,
          cur_vreg, ((use_m_masking == 1) && (im == (m_trips-1))) ? 1 : 0, ((use_m_masking == 1) && (im == (m_trips-1))) ? mask_reg : 0, 0 );

      /* If we have remainder, then we want to blend in -INF for the zero'ed out entries */
      if ( (flag_reduce_op_max > 0) && (im == m_trips-1) && (use_m_masking > 0)) {
        libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8(io_generated_code,
                  LIBXSMM_X86_INSTR_VBLENDVPS,
                  'y',
                  cur_vreg,
                  aux_vreg,
                  cur_vreg,
                  0, 0, 0, (mask_reg) << 4);
      }

      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             cur_vreg, reg_sum, reg_sum );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             LIBXSMM_X86_INSTR_VFMADD231PS,
                                             i_micro_kernel_config->vector_name,
                                             cur_vreg, cur_vreg, reg_sum_squared );
      }
    }

    /* Now last horizontal reduction and store of the result...  */
    if ( compute_plain_vals_reduce > 0 ) {
      libxsmm_generator_hinstrps_avx( io_generated_code, reduce_instr, reg_sum, 0, 1);
      libxsmm_x86_instruction_unified_vec_move( io_generated_code,
                                        vmove_instruction_out,
                                        i_gp_reg_mapping->gp_reg_reduced_elts,
                                        LIBXSMM_X86_GP_REG_UNDEF, 0,
                                        0,
                                        vname_out,
                                        reg_sum, 1, mask_out, 1 );
    }

    if ( compute_squared_vals_reduce > 0 ) {
      libxsmm_generator_hinstrps_avx( io_generated_code, reduce_instr, reg_sum_squared, 0, 1);
      libxsmm_x86_instruction_unified_vec_move( io_generated_code,
                                        vmove_instruction_out,
                                        i_gp_reg_mapping->gp_reg_reduced_elts_squared,
                                        LIBXSMM_X86_GP_REG_UNDEF, 0,
                                        0,
                                        vname_out,
                                        reg_sum_squared, 1, mask_out, 1 );
    }

    if (n > 1) {
      /* Adjust input and output pointer */
      libxsmm_x86_instruction_alu_imm(  io_generated_code,
          i_micro_kernel_config->alu_add_instruction,
          i_gp_reg_mapping->gp_reg_in,
          i_mateltwise_desc->ldi *  i_micro_kernel_config->datatype_size_in);

      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_alu_imm(  io_generated_code,
          i_micro_kernel_config->alu_add_instruction,
          i_gp_reg_mapping->gp_reg_reduced_elts,
          i_micro_kernel_config->datatype_size_out);
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_alu_imm(  io_generated_code,
          i_micro_kernel_config->alu_add_instruction,
          i_gp_reg_mapping->gp_reg_reduced_elts_squared,
          i_micro_kernel_config->datatype_size_out);
      }
      /* close n loop */
      libxsmm_generator_mateltwise_footer_n_loop(  io_generated_code, io_loop_label_tracker, i_micro_kernel_config, i_gp_reg_mapping->gp_reg_n_loop, n);
    }

    return;
  }

  /* We fully unroll in M dimension, calculate mask if there is remainder */
  m                 = i_mateltwise_desc->m;
  n                 = i_mateltwise_desc->n;
  use_m_masking     = ( m % 16 == 0 ) ? 0 : 1;
  use_n_masking     = ( n % 16 == 0 ) ? 0 : 1;
  m_trips           = (m + 15) / 16;
  n_trips           = (n + 15) / 16;
  n_full_trips      = ( n % 16 == 0 ) ? n_trips : n_trips-1;

  /* Calculate input mask in case we see m_masking */
  if (use_m_masking == 1) {
    /* Calculate mask reg 1 for input-reading */
    mask_in_count =  16 - (m % 16);
    libxsmm_generator_mateltwise_initialize_avx512_mask(io_generated_code, LIBXSMM_X86_GP_REG_RAX, 1, mask_in_count, LIBXSMM_GEMM_PRECISION_F32);
  }

  /* Calculate output mask in case we see n_masking */
  if (use_n_masking == 1) {
    /* Calculate mask reg 2 for output-writing */
    mask_out_count = 16 - (n % 16);
    libxsmm_generator_mateltwise_initialize_avx512_mask(io_generated_code, LIBXSMM_X86_GP_REG_RAX, 2, mask_out_count, LIBXSMM_GEMM_PRECISION_F32);
  }

  if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
    if (io_generated_code->arch < LIBXSMM_X86_AVX512_CPX) {
      libxsmm_generator_vcvtneps2bf16_avx512_prep_stack( io_generated_code, LIBXSMM_X86_GP_REG_RAX );
    }
  }

  /* move blend mask value to GP register and to mask register 7 */
  if (n != 1) {
    libxsmm_x86_instruction_alu_imm( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RAX, 0xff00 );
    libxsmm_x86_instruction_mask_move( io_generated_code, LIBXSMM_X86_INSTR_KMOVW_GPR_LD, LIBXSMM_X86_GP_REG_RAX, 7 );
    if ((use_m_masking > 0) && ( flag_reduce_op_max > 0 )) {
      libxsmm_generator_load_vreg_minus_infinity(io_generated_code, 'z', LIBXSMM_X86_GP_REG_RAX, aux_vreg);
    }
  }

  if (n_full_trips >= 1) {
    if (n_full_trips > 1) {
      /* open n loop */
      libxsmm_generator_mateltwise_header_n_loop(  io_generated_code, io_loop_label_tracker, i_micro_kernel_config, i_gp_reg_mapping->gp_reg_n_loop );
    }

    /* We fully unroll M loop here...  */
    for (im = 0; im < m_trips; im++) {
      /* load 16 columns of input matrix */
      for (i = 0 ; i < 16; i++) {
        libxsmm_x86_instruction_vec_move( io_generated_code,
            i_micro_kernel_config->instruction_set,
            vmove_instruction_in,
            i_gp_reg_mapping->gp_reg_in,
            LIBXSMM_X86_GP_REG_UNDEF, 0,
            (im * 16 + i * i_mateltwise_desc->ldi) * i_micro_kernel_config->datatype_size_in,
            vname_in,
            i, (im == (m_trips-1)) ? use_m_masking : 0, 1, 0 );
        if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) {
          libxsmm_generator_cvtbf16ps_avx512( io_generated_code, 'z', i, i );
        }
        if ((flag_reduce_op_max > 0) && (use_m_masking > 0) && (im == m_trips-1)) {
          libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                       LIBXSMM_X86_INSTR_VBLENDMPS,
                                                       'z',
                                                       i, aux_vreg, i, use_m_masking, 0 );
        }
      }

      /* 1st stage */
      /* zmm0/zmm4; 4444 4444 4444 4444 / 0000 0000 0000 0000 -> zmm0: 4444 4444 0000 0000 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     4, 0, 16, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             4, 0, 17, 0x4e );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              0, 0, 18 );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              4, 4, 19 );


        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     19, 18, 20, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             19, 18, 21, 0x4e );
      }

      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             16, 17, 0 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             20, 21, 24 );
      }

      /* zmm8/zmm12; cccc cccc cccc cccc / 8888 8888 8888 8888 -> zmm8: cccc cccc 8888 8888 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     12, 8, 16, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             12, 8, 17, 0x4e );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              8, 8, 18 );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              12, 12, 19 );


        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     19, 18, 20, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             19, 18, 21, 0x4e );
      }

      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             16, 17, 8 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             20, 21, 28 );
      }

      /* zmm1/zmm5; 5555 5555 5555 5555 / 1111 1111 1111 1111 -> zmm1: 5555 5555 1111 1111 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     5, 1, 16, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             5, 1, 17, 0x4e );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              1, 1, 18 );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              5, 5, 19 );


        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     19, 18, 20, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             19, 18, 21, 0x4e );
      }

      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             16, 17, 1 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             20, 21, 25 );
      }

      /* zmm9/zmm13; dddd dddd dddd dddd / 9999 9999 9999 9999 -> zmm9: dddd dddd 9999 9999 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     13, 9, 16, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             13, 9, 17, 0x4e );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              9, 9, 18 );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              13, 13, 19 );


        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     19, 18, 20, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             19, 18, 21, 0x4e );
      }

      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             16, 17, 9 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             20, 21, 29 );
      }

      /* zmm2/zmm6; 6666 6666 6666 6666 / 2222 2222 2222 2222 -> zmm2: 6666 6666 2222 2222 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     6, 2, 16, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             6, 2, 17, 0x4e );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              2, 2, 18 );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              6, 6, 19 );


        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     19, 18, 20, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             19, 18, 21, 0x4e );
      }

      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             16, 17, 2 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             20, 21, 26 );
      }

      /* zmm10/zmm14; eeee eeee eeee eeee / aaaa aaaa aaaa aaaa -> zmm10: eeee eeee aaaa aaaa */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     14, 10, 16, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             14, 10, 17, 0x4e );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              10, 10, 18 );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              14, 14, 19 );


        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     19, 18, 20, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             19, 18, 21, 0x4e );
      }

      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             16, 17, 10 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             20, 21, 30 );
      }

      /* zmm3/zmm7; 7777 7777 7777 7777 / 3333 3333 3333 3333  -> zmm3: 7777 7777 3333 3333 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     7, 3, 16, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             7, 3, 17, 0x4e );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              3, 3, 18 );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              7, 7, 19 );


        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     19, 18, 20, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             19, 18, 21, 0x4e );
      }

      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             16, 17, 3 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             20, 21, 27 );
      }

      /* zmm11/zmm15; ffff ffff ffff ffff / bbbb bbbb bbbb bbbb  -> zmm11: ffff ffff bbbb bbbb */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     15, 11, 16, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             15, 11, 17, 0x4e );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              11, 11, 18 );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              15, 15, 19 );


        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     19, 18, 20, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             19, 18, 21, 0x4e );
      }

      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             16, 17, 11 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             20, 21, 31 );
      }

      /* 2nd stage */
      /* zmm0/zmm8; 4444 4444 0000 0000 / cccc cccc 8888 8888  -> zmm0: cccc 8888 4444 0000 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 8, 0, 16, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 8, 0, 17, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 16, 17, 0 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 28, 24, 20, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 28, 24, 21, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 20, 21, 24 );
      }

      /* zmm1/zmm9; 5555 5555 1111 1111 / dddd dddd 9999 9999  -> zmm1: dddd 9999 5555 1111 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 9, 1, 16, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 9, 1, 17, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 16, 17, 1 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 29, 25, 20, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 29, 25, 21, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 20, 21, 25 );
      }

      /* zmm2/zmm10; 6666 6666 2222 2222 / eeee eeee aaaa aaaa  -> zmm2: eeee aaaa 6666 2222 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 10, 2, 16, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 10, 2, 17, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 16, 17, 2 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 30, 26, 20, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 30, 26, 21, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 20, 21, 26 );
      }

      /* zmm3/zmm11:  7777 7777 3333 3333 / ffff ffff bbbb bbbb  -> zmm3: ffff bbbb 7777 3333 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 11, 3, 16, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 11, 3, 17, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 16, 17, 3 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 31, 27, 20, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 31, 27, 21, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 20, 21, 27 );
      }

      /* 3rd stage */
      /* zmm0/zmm1; cccc 8888 4444 0000 / dddd 9999 5555 1111  -> zmm0: ddcc 9988 5544 1100 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 1, 0, 16, 0x44 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 1, 0, 17, 0xee );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 16, 17, 0 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 25, 24, 20, 0x44 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 25, 24, 21, 0xee );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 20, 21, 24 );
      }

      /* zmm2/zmm3; eeee aaaa 6666 2222 / ffff bbbb 7777 3333  -> zmm2: ffee bbaa 7766 3322 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 3, 2, 16, 0x44 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 3, 2, 17, 0xee );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 16, 17, 2 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 27, 26, 20, 0x44 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 27, 26, 21, 0xee );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 20, 21, 26 );
      }

      /* 4th stage */
      /* zmm0/zmm2; ddcc 9988 5544 1100 / ffee bbaa 7766 3322  -> zmm0: fedc ba98 7654 3210 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 2, 0, 16, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 2, 0, 17, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 16, 17, 0 );

        /* Update the running reduction result */
        if (im == 0) {
          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            if (io_generated_code->arch < LIBXSMM_X86_AVX512_CPX) {
              libxsmm_generator_vcvtneps2bf16_avx512_preppedstack( io_generated_code, 'z',
                  0, 0,
                  3, 4,
                  3, 4);
            } else {
             libxsmm_x86_instruction_vec_compute_2reg( io_generated_code, LIBXSMM_X86_INSTR_VCVTNEPS2BF16, 'z', 0, 0 );
            }
          }
          libxsmm_x86_instruction_vec_move( io_generated_code,
                                            i_micro_kernel_config->instruction_set,
                                            vmove_instruction_out,
                                            i_gp_reg_mapping->gp_reg_reduced_elts,
                                            LIBXSMM_X86_GP_REG_UNDEF, 0,
                                            0,
                                            vname_out,
                                            0, 0, 0, 1 );
        } else {
          libxsmm_x86_instruction_vec_move( io_generated_code,
              i_micro_kernel_config->instruction_set,
              vmove_instruction_out,
              i_gp_reg_mapping->gp_reg_reduced_elts,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              0,
              vname_out,
              1, 0, 1, 0 );
          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            libxsmm_generator_cvtbf16ps_avx512( io_generated_code, 'z', 1, 1 );
          }

          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                  reduce_instr,
                                                  i_micro_kernel_config->vector_name,
                                                  1, 0, 0 );
          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            if (io_generated_code->arch < LIBXSMM_X86_AVX512_CPX) {
              libxsmm_generator_vcvtneps2bf16_avx512_preppedstack( io_generated_code, 'z',
                  0, 0,
                  3, 4,
                  3, 4);
            } else {
             libxsmm_x86_instruction_vec_compute_2reg( io_generated_code, LIBXSMM_X86_INSTR_VCVTNEPS2BF16, 'z', 0, 0 );
            }
          }

          libxsmm_x86_instruction_vec_move( io_generated_code,
                                            i_micro_kernel_config->instruction_set,
                                            vmove_instruction_out,
                                            i_gp_reg_mapping->gp_reg_reduced_elts,
                                            LIBXSMM_X86_GP_REG_UNDEF, 0,
                                            0,
                                            vname_out,
                                            0, 0, 0, 1 );
        }
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 26, 24, 20, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 26, 24, 21, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 20, 21, 24 );

        /* Update the running reduction result */
        if (im == 0) {
          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            if (io_generated_code->arch < LIBXSMM_X86_AVX512_CPX) {
              libxsmm_generator_vcvtneps2bf16_avx512_preppedstack( io_generated_code, 'z',
                  24, 24,
                  25, 26,
                  3, 4);
            } else {
             libxsmm_x86_instruction_vec_compute_2reg( io_generated_code, LIBXSMM_X86_INSTR_VCVTNEPS2BF16, 'z', 24, 24 );
            }
          }

          libxsmm_x86_instruction_vec_move( io_generated_code,
                                            i_micro_kernel_config->instruction_set,
                                            vmove_instruction_out,
                                            i_gp_reg_mapping->gp_reg_reduced_elts_squared,
                                            LIBXSMM_X86_GP_REG_UNDEF, 0,
                                            0,
                                            vname_out,
                                            24, 0, 0, 1 );
        } else {
          libxsmm_x86_instruction_vec_move( io_generated_code,
              i_micro_kernel_config->instruction_set,
              vmove_instruction_out,
              i_gp_reg_mapping->gp_reg_reduced_elts_squared,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              0,
              vname_out,
              25, 0, 1, 0 );
          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            libxsmm_generator_cvtbf16ps_avx512( io_generated_code, 'z', 25, 25 );
          }

          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                  reduce_instr,
                                                  i_micro_kernel_config->vector_name,
                                                  25, 24, 24 );

          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            if (io_generated_code->arch < LIBXSMM_X86_AVX512_CPX) {
              libxsmm_generator_vcvtneps2bf16_avx512_preppedstack( io_generated_code, 'z',
                  24, 24,
                  25, 26,
                  3, 4);
            } else {
             libxsmm_x86_instruction_vec_compute_2reg( io_generated_code, LIBXSMM_X86_INSTR_VCVTNEPS2BF16, 'z', 24, 24 );
            }
          }
          libxsmm_x86_instruction_vec_move( io_generated_code,
                                            i_micro_kernel_config->instruction_set,
                                            vmove_instruction_out,
                                            i_gp_reg_mapping->gp_reg_reduced_elts_squared,
                                            LIBXSMM_X86_GP_REG_UNDEF, 0,
                                            0,
                                            vname_out,
                                            24, 0, 0, 1 );
        }
      }
    }

    if ((n_full_trips >  1) || (n % 16 != 0)) {
      /* Adjust input and output pointer */
      libxsmm_x86_instruction_alu_imm(  io_generated_code,
          i_micro_kernel_config->alu_add_instruction,
          i_gp_reg_mapping->gp_reg_in,
          16 * i_mateltwise_desc->ldi *  i_micro_kernel_config->datatype_size_in);

      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_alu_imm(  io_generated_code,
          i_micro_kernel_config->alu_add_instruction,
          i_gp_reg_mapping->gp_reg_reduced_elts,
          16 * i_micro_kernel_config->datatype_size_out);
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_alu_imm(  io_generated_code,
          i_micro_kernel_config->alu_add_instruction,
          i_gp_reg_mapping->gp_reg_reduced_elts_squared,
          16 * i_micro_kernel_config->datatype_size_out);
      }
    }

    if (n_full_trips > 1) {
      /* close n loop */
      libxsmm_generator_mateltwise_footer_n_loop(  io_generated_code, io_loop_label_tracker, i_micro_kernel_config, i_gp_reg_mapping->gp_reg_n_loop, n_full_trips);
    }
  }

  /* In this case we load only partial number of columns  */
  n_cols_load = n % 16;
  im = 0;
  /* Special case when we reduce as single column  */
  if (n == 1) {
    unsigned int reg_sum = 2, reg_sum_squared = 3;
    unsigned int cur_vreg;
    aux_vreg = 3;

    /* Initialize accumulators to zero */
    if ( compute_plain_vals_reduce > 0 ) {
      if ( flag_reduce_op_add > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VPXORD,
                                                 i_micro_kernel_config->vector_name,
                                                 reg_sum, reg_sum, reg_sum );
      } else if ( flag_reduce_op_max > 0 ) {
        if ((use_m_masking > 0) && ( flag_reduce_op_max > 0 )) {
          libxsmm_generator_load_vreg_minus_infinity(io_generated_code, 'z', LIBXSMM_X86_GP_REG_RAX, aux_vreg);
        }
        libxsmm_x86_instruction_vec_move( io_generated_code,
            i_micro_kernel_config->instruction_set,
            vmove_instruction_in,
            i_gp_reg_mapping->gp_reg_in,
            LIBXSMM_X86_GP_REG_UNDEF, 0,
            0,
            vname_in,
            reg_sum, (im == (m_trips-1)) ? use_m_masking : 0, 1, 0);
        if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) {
          libxsmm_generator_cvtbf16ps_avx512( io_generated_code, 'z', reg_sum, reg_sum );
        }
        if ((flag_reduce_op_max > 0) && (use_m_masking > 0) && (im == m_trips-1)) {
          libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                       LIBXSMM_X86_INSTR_VBLENDMPS,
                                                       'z',
                                                       reg_sum, aux_vreg, reg_sum, use_m_masking, 0 );
        }
      }
    }

    if ( compute_squared_vals_reduce > 0 ) {
      libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                               LIBXSMM_X86_INSTR_VPXORD,
                                               i_micro_kernel_config->vector_name,
                                               reg_sum_squared, reg_sum_squared, reg_sum_squared );
    }

    for (im = 0; im < m_trips; im++) {
      cur_vreg = im % 28 + 4;
      libxsmm_x86_instruction_vec_move( io_generated_code,
          i_micro_kernel_config->instruction_set,
          vmove_instruction_in,
          i_gp_reg_mapping->gp_reg_in,
          LIBXSMM_X86_GP_REG_UNDEF, 0,
          im * 16 * i_micro_kernel_config->datatype_size_in,
          vname_in,
          cur_vreg, (im == (m_trips-1)) ? use_m_masking : 0, 1, 0 );

      if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) {
        libxsmm_generator_cvtbf16ps_avx512( io_generated_code, 'z', cur_vreg, cur_vreg );
      }

      if ( (flag_reduce_op_max > 0) && (im == m_trips-1) && (use_m_masking > 0)) {
        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     aux_vreg, cur_vreg, cur_vreg, use_m_masking, 0 );
      }

      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             cur_vreg, reg_sum, reg_sum );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             LIBXSMM_X86_INSTR_VFMADD231PS,
                                             i_micro_kernel_config->vector_name,
                                             cur_vreg, cur_vreg, reg_sum_squared );
      }
    }

    /* Now last horizontal reduction and store of the result...  */
    if ( compute_plain_vals_reduce > 0 ) {
      libxsmm_generator_hinstrps_avx512( io_generated_code, reduce_instr, reg_sum, 0, 1);
      if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
        if (io_generated_code->arch < LIBXSMM_X86_AVX512_CPX) {
          libxsmm_generator_vcvtneps2bf16_avx512_preppedstack( io_generated_code, 'z',
              reg_sum, reg_sum,
              0, 1,
              3, 4);
        } else {
         libxsmm_x86_instruction_vec_compute_2reg( io_generated_code, LIBXSMM_X86_INSTR_VCVTNEPS2BF16, 'z', reg_sum, reg_sum );
        }
      }
      libxsmm_x86_instruction_vec_move( io_generated_code,
                                        i_micro_kernel_config->instruction_set,
                                        vmove_instruction_out,
                                        i_gp_reg_mapping->gp_reg_reduced_elts,
                                        LIBXSMM_X86_GP_REG_UNDEF, 0,
                                        0,
                                        vname_out,
                                        reg_sum, 2, 0, 1 );
    }

    if ( compute_squared_vals_reduce > 0 ) {
      libxsmm_generator_hinstrps_avx512( io_generated_code, reduce_instr, reg_sum_squared, 0, 1);
      if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
        if (io_generated_code->arch < LIBXSMM_X86_AVX512_CPX) {
          libxsmm_generator_vcvtneps2bf16_avx512_preppedstack( io_generated_code, 'z',
              reg_sum_squared, reg_sum_squared,
              0, 1,
              3, 4);
        } else {
         libxsmm_x86_instruction_vec_compute_2reg( io_generated_code, LIBXSMM_X86_INSTR_VCVTNEPS2BF16, 'z', reg_sum_squared, reg_sum_squared );
        }
      }
      libxsmm_x86_instruction_vec_move( io_generated_code,
                                        i_micro_kernel_config->instruction_set,
                                        vmove_instruction_out,
                                        i_gp_reg_mapping->gp_reg_reduced_elts_squared,
                                        LIBXSMM_X86_GP_REG_UNDEF, 0,
                                        0,
                                        vname_out,
                                        reg_sum_squared, 2, 0, 1 );
    }
  } else if (n_cols_load != 0) {
    /* We fully unroll M loop here...  */
    for (im = 0; im < m_trips; im++) {
      /* load 16 columns of input matrix */
      for (i = 0 ; i < n_cols_load; i++) {
        libxsmm_x86_instruction_vec_move( io_generated_code,
            i_micro_kernel_config->instruction_set,
            vmove_instruction_in,
            i_gp_reg_mapping->gp_reg_in,
            LIBXSMM_X86_GP_REG_UNDEF, 0,
            (im * 16 + i * i_mateltwise_desc->ldi) * i_micro_kernel_config->datatype_size_in,
            vname_in,
            i, (im == (m_trips-1)) ? use_m_masking : 0, 1, 0 );
        if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) {
          libxsmm_generator_cvtbf16ps_avx512( io_generated_code, 'z', i, i );
        }
        if ((flag_reduce_op_max > 0) && (use_m_masking > 0) && (im == m_trips-1)) {
          libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                       LIBXSMM_X86_INSTR_VBLENDMPS,
                                                       'z',
                                                       i, aux_vreg, i, use_m_masking, 0 );
        }
      }

      for ( i = n_cols_load; i < 16; i++) {
        if ((flag_reduce_op_max > 0) && (use_m_masking > 0) && (im == m_trips-1)) {
          libxsmm_x86_instruction_vec_compute_2reg( io_generated_code, LIBXSMM_X86_INSTR_VMOVUPS, 'z', aux_vreg, i );
        } else {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                   LIBXSMM_X86_INSTR_VPXORD,
                                                   i_micro_kernel_config->vector_name,
                                                   i, i, i );
        }
      }

      /* 1st stage */
      /* zmm0/zmm4; 4444 4444 4444 4444 / 0000 0000 0000 0000 -> zmm0: 4444 4444 0000 0000 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     4, 0, 16, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             4, 0, 17, 0x4e );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              0, 0, 18 );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              4, 4, 19 );


        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     19, 18, 20, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             19, 18, 21, 0x4e );
      }

      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             16, 17, 0 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             20, 21, 24 );
      }

      if (n_cols_load > 7) {
        /* zmm8/zmm12; cccc cccc cccc cccc / 8888 8888 8888 8888 -> zmm8: cccc cccc 8888 8888 */
        if ( compute_plain_vals_reduce > 0 ) {
          libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                       LIBXSMM_X86_INSTR_VBLENDMPS,
                                                       i_micro_kernel_config->vector_name,
                                                       12, 8, 16, 7, 0 );

          libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                               LIBXSMM_X86_INSTR_VSHUFF64X2,
                                               i_micro_kernel_config->vector_name,
                                               12, 8, 17, 0x4e );
        }

        if ( compute_squared_vals_reduce > 0 ) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                LIBXSMM_X86_INSTR_VMULPS,
                                                i_micro_kernel_config->vector_name,
                                                8, 8, 18 );

          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                LIBXSMM_X86_INSTR_VMULPS,
                                                i_micro_kernel_config->vector_name,
                                                12, 12, 19 );


          libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                       LIBXSMM_X86_INSTR_VBLENDMPS,
                                                       i_micro_kernel_config->vector_name,
                                                       19, 18, 20, 7, 0 );

          libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                               LIBXSMM_X86_INSTR_VSHUFF64X2,
                                               i_micro_kernel_config->vector_name,
                                               19, 18, 21, 0x4e );
        }

        if ( compute_plain_vals_reduce > 0 ) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                               reduce_instr,
                                               i_micro_kernel_config->vector_name,
                                               16, 17, 8 );
        }

        if ( compute_squared_vals_reduce > 0 ) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                               reduce_instr,
                                               i_micro_kernel_config->vector_name,
                                               20, 21, 28 );
        }
      }

      /* zmm1/zmm5; 5555 5555 5555 5555 / 1111 1111 1111 1111 -> zmm1: 5555 5555 1111 1111 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     5, 1, 16, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             5, 1, 17, 0x4e );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              1, 1, 18 );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              5, 5, 19 );


        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     19, 18, 20, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             19, 18, 21, 0x4e );
      }

      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             16, 17, 1 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             20, 21, 25 );
      }

      if (n_cols_load > 8) {
        /* zmm9/zmm13; dddd dddd dddd dddd / 9999 9999 9999 9999 -> zmm9: dddd dddd 9999 9999 */
        if ( compute_plain_vals_reduce > 0 ) {
          libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                       LIBXSMM_X86_INSTR_VBLENDMPS,
                                                       i_micro_kernel_config->vector_name,
                                                       13, 9, 16, 7, 0 );

          libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                               LIBXSMM_X86_INSTR_VSHUFF64X2,
                                               i_micro_kernel_config->vector_name,
                                               13, 9, 17, 0x4e );
        }

        if ( compute_squared_vals_reduce > 0 ) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                LIBXSMM_X86_INSTR_VMULPS,
                                                i_micro_kernel_config->vector_name,
                                                9, 9, 18 );

          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                LIBXSMM_X86_INSTR_VMULPS,
                                                i_micro_kernel_config->vector_name,
                                                13, 13, 19 );


          libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                       LIBXSMM_X86_INSTR_VBLENDMPS,
                                                       i_micro_kernel_config->vector_name,
                                                       19, 18, 20, 7, 0 );

          libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                               LIBXSMM_X86_INSTR_VSHUFF64X2,
                                               i_micro_kernel_config->vector_name,
                                               19, 18, 21, 0x4e );
        }

        if ( compute_plain_vals_reduce > 0 ) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                               reduce_instr,
                                               i_micro_kernel_config->vector_name,
                                               16, 17, 9 );
        }

        if ( compute_squared_vals_reduce > 0 ) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                               reduce_instr,
                                               i_micro_kernel_config->vector_name,
                                               20, 21, 29 );
        }
      }

      /* zmm2/zmm6; 6666 6666 6666 6666 / 2222 2222 2222 2222 -> zmm2: 6666 6666 2222 2222 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     6, 2, 16, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             6, 2, 17, 0x4e );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              2, 2, 18 );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              6, 6, 19 );


        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     19, 18, 20, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             19, 18, 21, 0x4e );
      }

      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             16, 17, 2 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             20, 21, 26 );
      }

      if (n_cols_load > 9) {
        /* zmm10/zmm14; eeee eeee eeee eeee / aaaa aaaa aaaa aaaa -> zmm10: eeee eeee aaaa aaaa */
        if ( compute_plain_vals_reduce > 0 ) {
          libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                       LIBXSMM_X86_INSTR_VBLENDMPS,
                                                       i_micro_kernel_config->vector_name,
                                                       14, 10, 16, 7, 0 );

          libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                               LIBXSMM_X86_INSTR_VSHUFF64X2,
                                               i_micro_kernel_config->vector_name,
                                               14, 10, 17, 0x4e );
        }

        if ( compute_squared_vals_reduce > 0 ) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                LIBXSMM_X86_INSTR_VMULPS,
                                                i_micro_kernel_config->vector_name,
                                                10, 10, 18 );

          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                LIBXSMM_X86_INSTR_VMULPS,
                                                i_micro_kernel_config->vector_name,
                                                14, 14, 19 );


          libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                       LIBXSMM_X86_INSTR_VBLENDMPS,
                                                       i_micro_kernel_config->vector_name,
                                                       19, 18, 20, 7, 0 );

          libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                               LIBXSMM_X86_INSTR_VSHUFF64X2,
                                               i_micro_kernel_config->vector_name,
                                               19, 18, 21, 0x4e );
        }

        if ( compute_plain_vals_reduce > 0 ) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                               reduce_instr,
                                               i_micro_kernel_config->vector_name,
                                               16, 17, 10 );
        }

        if ( compute_squared_vals_reduce > 0 ) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                               reduce_instr,
                                               i_micro_kernel_config->vector_name,
                                               20, 21, 30 );
        }
      }

      /* zmm3/zmm7; 7777 7777 7777 7777 / 3333 3333 3333 3333  -> zmm3: 7777 7777 3333 3333 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     7, 3, 16, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             7, 3, 17, 0x4e );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              3, 3, 18 );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                              LIBXSMM_X86_INSTR_VMULPS,
                                              i_micro_kernel_config->vector_name,
                                              7, 7, 19 );


        libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                     LIBXSMM_X86_INSTR_VBLENDMPS,
                                                     i_micro_kernel_config->vector_name,
                                                     19, 18, 20, 7, 0 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                             LIBXSMM_X86_INSTR_VSHUFF64X2,
                                             i_micro_kernel_config->vector_name,
                                             19, 18, 21, 0x4e );
      }

      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             16, 17, 3 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                             reduce_instr,
                                             i_micro_kernel_config->vector_name,
                                             20, 21, 27 );
      }

      if (n_cols_load > 10) {
        /* zmm11/zmm15; ffff ffff ffff ffff / bbbb bbbb bbbb bbbb  -> zmm11: ffff ffff bbbb bbbb */
        if ( compute_plain_vals_reduce > 0 ) {
          libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                       LIBXSMM_X86_INSTR_VBLENDMPS,
                                                       i_micro_kernel_config->vector_name,
                                                       15, 11, 16, 7, 0 );

          libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                               LIBXSMM_X86_INSTR_VSHUFF64X2,
                                               i_micro_kernel_config->vector_name,
                                               15, 11, 17, 0x4e );
        }

        if ( compute_squared_vals_reduce > 0 ) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                LIBXSMM_X86_INSTR_VMULPS,
                                                i_micro_kernel_config->vector_name,
                                                11, 11, 18 );

          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                LIBXSMM_X86_INSTR_VMULPS,
                                                i_micro_kernel_config->vector_name,
                                                15, 15, 19 );


          libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code,
                                                       LIBXSMM_X86_INSTR_VBLENDMPS,
                                                       i_micro_kernel_config->vector_name,
                                                       19, 18, 20, 7, 0 );

          libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                               LIBXSMM_X86_INSTR_VSHUFF64X2,
                                               i_micro_kernel_config->vector_name,
                                               19, 18, 21, 0x4e );
        }

        if ( compute_plain_vals_reduce > 0 ) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                               reduce_instr,
                                               i_micro_kernel_config->vector_name,
                                               16, 17, 11 );
        }

        if ( compute_squared_vals_reduce > 0 ) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                               reduce_instr,
                                               i_micro_kernel_config->vector_name,
                                               20, 21, 31 );
        }
      }

      /* 2nd stage */
      /* zmm0/zmm8; 4444 4444 0000 0000 / cccc cccc 8888 8888  -> zmm0: cccc 8888 4444 0000 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 8, 0, 16, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 8, 0, 17, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 16, 17, 0 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 28, 24, 20, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 28, 24, 21, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 20, 21, 24 );
      }

      /* zmm1/zmm9; 5555 5555 1111 1111 / dddd dddd 9999 9999  -> zmm1: dddd 9999 5555 1111 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 9, 1, 16, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 9, 1, 17, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 16, 17, 1 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 29, 25, 20, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 29, 25, 21, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 20, 21, 25 );
      }

      /* zmm2/zmm10; 6666 6666 2222 2222 / eeee eeee aaaa aaaa  -> zmm2: eeee aaaa 6666 2222 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 10, 2, 16, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 10, 2, 17, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 16, 17, 2 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 30, 26, 20, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 30, 26, 21, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 20, 21, 26 );
      }

      /* zmm3/zmm11:  7777 7777 3333 3333 / ffff ffff bbbb bbbb  -> zmm3: ffff bbbb 7777 3333 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 11, 3, 16, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 11, 3, 17, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 16, 17, 3 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 31, 27, 20, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFF64X2,
                                                 i_micro_kernel_config->vector_name,
                                                 31, 27, 21, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 20, 21, 27 );
      }

      /* 3rd stage */
      /* zmm0/zmm1; cccc 8888 4444 0000 / dddd 9999 5555 1111  -> zmm0: ddcc 9988 5544 1100 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 1, 0, 16, 0x44 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 1, 0, 17, 0xee );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 16, 17, 0 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 25, 24, 20, 0x44 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 25, 24, 21, 0xee );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 20, 21, 24 );
      }

      /* zmm2/zmm3; eeee aaaa 6666 2222 / ffff bbbb 7777 3333  -> zmm2: ffee bbaa 7766 3322 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 3, 2, 16, 0x44 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 3, 2, 17, 0xee );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 16, 17, 2 );
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 27, 26, 20, 0x44 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 27, 26, 21, 0xee );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 20, 21, 26 );
      }

      /* 4th stage */
      /* zmm0/zmm2; ddcc 9988 5544 1100 / ffee bbaa 7766 3322  -> zmm0: fedc ba98 7654 3210 */
      if ( compute_plain_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 2, 0, 16, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 2, 0, 17, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 16, 17, 0 );

        /* Update the running reduction result */
        if (im == 0) {
          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            if (io_generated_code->arch < LIBXSMM_X86_AVX512_CPX) {
              libxsmm_generator_vcvtneps2bf16_avx512_preppedstack( io_generated_code, 'z',
                  0, 0,
                  3, 4,
                  3, 4);
            } else {
             libxsmm_x86_instruction_vec_compute_2reg( io_generated_code, LIBXSMM_X86_INSTR_VCVTNEPS2BF16, 'z', 0, 0 );
            }
          }
          libxsmm_x86_instruction_vec_move( io_generated_code,
                                            i_micro_kernel_config->instruction_set,
                                            vmove_instruction_out,
                                            i_gp_reg_mapping->gp_reg_reduced_elts,
                                            LIBXSMM_X86_GP_REG_UNDEF, 0,
                                            0,
                                            vname_out,
                                            0, 2, 0, 1 );
        } else {
          libxsmm_x86_instruction_vec_move( io_generated_code,
              i_micro_kernel_config->instruction_set,
              vmove_instruction_out,
              i_gp_reg_mapping->gp_reg_reduced_elts,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              0,
              vname_out,
              1, 2, 1, 0 );
          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            libxsmm_generator_cvtbf16ps_avx512( io_generated_code, 'z', 1, 1 );
          }

          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                  reduce_instr,
                                                  i_micro_kernel_config->vector_name,
                                                  1, 0, 0 );
          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            if (io_generated_code->arch < LIBXSMM_X86_AVX512_CPX) {
              libxsmm_generator_vcvtneps2bf16_avx512_preppedstack( io_generated_code, 'z',
                  0, 0,
                  3, 4,
                  3, 4);
            } else {
             libxsmm_x86_instruction_vec_compute_2reg( io_generated_code, LIBXSMM_X86_INSTR_VCVTNEPS2BF16, 'z', 0, 0 );
            }
          }

          libxsmm_x86_instruction_vec_move( io_generated_code,
                                            i_micro_kernel_config->instruction_set,
                                            vmove_instruction_out,
                                            i_gp_reg_mapping->gp_reg_reduced_elts,
                                            LIBXSMM_X86_GP_REG_UNDEF, 0,
                                            0,
                                            vname_out,
                                            0, 2, 0, 1 );
        }
      }

      if ( compute_squared_vals_reduce > 0 ) {
        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 26, 24, 20, 0x88 );

        libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VSHUFPS,
                                                 i_micro_kernel_config->vector_name,
                                                 26, 24, 21, 0xdd );

        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 reduce_instr,
                                                 i_micro_kernel_config->vector_name,
                                                 20, 21, 24 );

        /* Update the running reduction result */
        if (im == 0) {
          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            if (io_generated_code->arch < LIBXSMM_X86_AVX512_CPX) {
              libxsmm_generator_vcvtneps2bf16_avx512_preppedstack( io_generated_code, 'z',
                  24, 24,
                  25, 26,
                  3, 4);
            } else {
             libxsmm_x86_instruction_vec_compute_2reg( io_generated_code, LIBXSMM_X86_INSTR_VCVTNEPS2BF16, 'z', 24, 24 );
            }
          }

          libxsmm_x86_instruction_vec_move( io_generated_code,
                                            i_micro_kernel_config->instruction_set,
                                            vmove_instruction_out,
                                            i_gp_reg_mapping->gp_reg_reduced_elts_squared,
                                            LIBXSMM_X86_GP_REG_UNDEF, 0,
                                            0,
                                            vname_out,
                                            24, 2, 0, 1 );
        } else {

          libxsmm_x86_instruction_vec_move( io_generated_code,
              i_micro_kernel_config->instruction_set,
              vmove_instruction_out,
              i_gp_reg_mapping->gp_reg_reduced_elts_squared,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              0,
              vname_out,
              25, 2, 1, 0 );
          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            libxsmm_generator_cvtbf16ps_avx512( io_generated_code, 'z', 25, 25 );
          }

          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                  reduce_instr,
                                                  i_micro_kernel_config->vector_name,
                                                  25, 24, 24 );

          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            if (io_generated_code->arch < LIBXSMM_X86_AVX512_CPX) {
              libxsmm_generator_vcvtneps2bf16_avx512_preppedstack( io_generated_code, 'z',
                  24, 24,
                  25, 26,
                  3, 4);
            } else {
             libxsmm_x86_instruction_vec_compute_2reg( io_generated_code, LIBXSMM_X86_INSTR_VCVTNEPS2BF16, 'z', 24, 24 );
            }
          }
          libxsmm_x86_instruction_vec_move( io_generated_code,
                                            i_micro_kernel_config->instruction_set,
                                            vmove_instruction_out,
                                            i_gp_reg_mapping->gp_reg_reduced_elts_squared,
                                            LIBXSMM_X86_GP_REG_UNDEF, 0,
                                            0,
                                            vname_out,
                                            24, 2, 0, 1 );
        }
      }
    }
  }

  if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
    if (io_generated_code->arch < LIBXSMM_X86_AVX512_CPX) {
      libxsmm_generator_vcvtneps2bf16_avx512_clean_stack( io_generated_code, LIBXSMM_X86_GP_REG_RAX );
    }
  }
}

LIBXSMM_API_INTERN void libxsmm_generator_avx_extract_mask4_from_mask8( libxsmm_generated_code* io_generated_code, unsigned int mask_in, unsigned int mask_out, unsigned int lohi);
LIBXSMM_API_INTERN void libxsmm_generator_avx_extract_mask4_from_mask8( libxsmm_generated_code* io_generated_code, unsigned int mask_in, unsigned int mask_out, unsigned int lohi) {
  if (lohi == 0) {
    libxsmm_x86_instruction_vec_compute_2reg( io_generated_code, LIBXSMM_X86_INSTR_VCVTPS2PD, 'y', mask_in, mask_out );
  } else {
    libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code, LIBXSMM_X86_INSTR_VPERM2F128, 'y', mask_in, mask_in, mask_out, 0x1 );
    libxsmm_x86_instruction_vec_compute_2reg( io_generated_code, LIBXSMM_X86_INSTR_VCVTPS2PD, 'y', mask_out, mask_out );
  }
}

LIBXSMM_API_INTERN
void libxsmm_generator_opreduce_vecs_index_avx512_microkernel( libxsmm_generated_code*                        io_generated_code,
    libxsmm_loop_label_tracker*                    io_loop_label_tracker,
    libxsmm_mateltwise_gp_reg_mapping*             i_gp_reg_mapping,
    const libxsmm_mateltwise_kernel_config*        i_micro_kernel_config,
    const libxsmm_meltw_descriptor*                i_mateltwise_desc ) {

  unsigned int m, im, use_m_masking, m_trips, max_m_unrolling = 4, m_unroll_factor = 1, m_trips_loop = 0, peeled_m_trips = 0, mask_out_count = 0, vecin_offset = 0, vecidxin_offset = 0, vecout_offset = 0, temp_vreg = 31, use_stack_vars = 0;
  unsigned int vlen = 16;
  unsigned int mask_inout = 1;
  unsigned int mask_argidx64 = 2;
  unsigned int use_indexed_vec = 0, use_indexed_vecidx = 1;
  unsigned int use_implicitly_indexed_vec = 0;
  unsigned int use_implicitly_indexed_vecidx = 0;
  unsigned int idx_tsize =  i_mateltwise_desc->n;
  unsigned int in_tsize = 4;
  unsigned int vecidx_ind_base_param_offset = 8;
  unsigned int vecidx_in_base_param_offset = 16;
  unsigned int temp_vreg_argop0 = 30, temp_vreg_argop1 = 29;
  unsigned int record_argop_off_vec0 = 0, record_argop_off_vec1 = 0;
  unsigned int vecargop_vec0_offset = 0, vecargop_vec1_offset = 0;
  unsigned int bcast_idx_instr = ( idx_tsize == 8 ) ? LIBXSMM_X86_INSTR_VPBROADCASTQ : LIBXSMM_X86_INSTR_VPBROADCASTD ;
  unsigned int gpr_bcast_idx_instr = ( idx_tsize == 8 ) ? LIBXSMM_X86_INSTR_VPBROADCASTQ_GPR : LIBXSMM_X86_INSTR_VPBROADCASTD_GPR ;
  const int rbp_offset_idx = -8;
  int rbp_offset_argop_ptr0 = -16;
  int rbp_offset_argop_ptr1 = -24;
  char vname_argop_bcast = 'z';
  unsigned int argop_mask = 3;
  unsigned int argop_mask_aux = 4;
  unsigned int argop_cmp_instr = LIBXSMM_X86_INSTR_VCMPPS;
  unsigned int argop_blend_instr = (idx_tsize == 8) ? LIBXSMM_X86_INSTR_VPBLENDMQ : LIBXSMM_X86_INSTR_VPBLENDMD;
  unsigned int argop_cmp_imm = 0;
  unsigned int temp_gpr = LIBXSMM_X86_GP_REG_RBX;
  unsigned int ind_alu_mov_instr = (idx_tsize == 8) ? LIBXSMM_X86_INSTR_MOVQ : LIBXSMM_X86_INSTR_MOVL;
  unsigned int max_m_unrolling_index = (idx_tsize == 8) ? 2 * max_m_unrolling : max_m_unrolling;
  int pf_dist = 4, use_nts = 0, pf_instr = LIBXSMM_X86_INSTR_PREFETCHT1, pf_type = 1, load_acc = 1;
  unsigned int vstore_instr = 0;
  int apply_op = 0, apply_redop = 0, op_order = -1, op_instr = 0, reduceop_instr = 0, scale_op_result = 0, pushpop_scaleop_reg = 0;
  unsigned int NO_PF_LABEL_START = 0;
  unsigned int NO_PF_LABEL_START_2 = 1;
  unsigned int END_LABEL = 2;
  const int LIBXSMM_X86_INSTR_DOTPS = -1;
  unsigned int op_mask = 0, reduceop_mask = 0, op_mask_cntl = 0, reduceop_mask_cntl = 0, op_imm = 0, reduceop_imm = 0;
  unsigned char op_sae_cntl = 0, reduceop_sae_cntl = 0;
  const char *const env_pf_dist = getenv("PF_DIST_OPREDUCE_VECS_IDX");
  const char *const env_pf_type = getenv("PF_TYPE_OPREDUCE_VECS_IDX");
  const char *const env_nts     = getenv("NTS_OPREDUCE_VECS_IDX");
  const char *const env_load_acc= getenv("LOAD_ACCS_OPREDUCE_VECS_IDX");
#if defined(LIBXSMM_GENERATOR_MATELTWISE_REDUCE_AVX_AVX512_JUMP_LABEL_TRACKER_MALLOC)
  libxsmm_jump_label_tracker* const p_jump_label_tracker = (libxsmm_jump_label_tracker*)malloc(sizeof(libxsmm_jump_label_tracker));
#else
  libxsmm_jump_label_tracker l_jump_label_tracker;
  libxsmm_jump_label_tracker* const p_jump_label_tracker = &l_jump_label_tracker;
#endif
  libxsmm_reset_jump_label_tracker(p_jump_label_tracker);

  if ((i_mateltwise_desc->param & 0x1) > 0) {
    record_argop_off_vec0 = 1;
    use_stack_vars = 1;
  }

  if ((i_mateltwise_desc->param & 0x2) > 0) {
    record_argop_off_vec1 = 1;
    use_stack_vars = 1;
  }

  if (io_generated_code->arch < LIBXSMM_X86_AVX512) {
    vname_argop_bcast = 'y';
    vlen = 8;
    max_m_unrolling = 2;
    mask_inout = 15;
    mask_argidx64 = 14;
    argop_mask = 13;
    temp_vreg = 12;
    temp_vreg_argop0 = 11;
    temp_vreg_argop1 = 10;
    argop_mask_aux = 9;
    if ((idx_tsize == 8) && ((record_argop_off_vec0 == 1) || (record_argop_off_vec1 == 1))) {
      max_m_unrolling = 1;
      max_m_unrolling_index = 2 * max_m_unrolling;
    } else {
      max_m_unrolling_index = max_m_unrolling;
    }
    bcast_idx_instr = ( idx_tsize == 8 ) ? LIBXSMM_X86_INSTR_VBROADCASTSD : LIBXSMM_X86_INSTR_VBROADCASTSS;
    argop_blend_instr = (idx_tsize == 8) ? LIBXSMM_X86_INSTR_VBLENDVPD : LIBXSMM_X86_INSTR_VBLENDVPS;
  }

  if ((i_mateltwise_desc->param & 0x1) > 0) {
    vecargop_vec0_offset = 0;
  }
  if ((i_mateltwise_desc->param & 0x2) > 0) {
    vecargop_vec1_offset = (record_argop_off_vec0 == 1) ? vecargop_vec0_offset + max_m_unrolling_index : 0;
  }
  vecout_offset =  ((record_argop_off_vec0 == 1) || (record_argop_off_vec1 == 1)) ? LIBXSMM_MAX(vecargop_vec0_offset, vecargop_vec1_offset) + max_m_unrolling_index : 0;
  vecin_offset    = vecout_offset + max_m_unrolling;
  vecidxin_offset = vecin_offset + max_m_unrolling;

  if ( 0 == env_pf_dist ) {
  } else {
    pf_dist = atoi(env_pf_dist);
  }
  if ( 0 == env_pf_type ) {
  } else {
    pf_type = atoi(env_pf_type);
  }
  if ( 0 == env_nts ) {
  } else {
    use_nts = atoi(env_nts);
  }
  if ( 0 == env_load_acc ) {
  } else {
    load_acc = atoi(env_load_acc);
  }

  /* TODO: Add validation checks for various JIT params and error out... */

  if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) {
    in_tsize = 2;
  } else if (LIBXSMM_DATATYPE_F32 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) {
    in_tsize = 4;
  } else {
#if defined(LIBXSMM_GENERATOR_MATELTWISE_REDUCE_AVX_AVX512_JUMP_LABEL_TRACKER_MALLOC)
    free(p_jump_label_tracker);
#endif
    /* This should not happen  */
    LIBXSMM_HANDLE_ERROR( io_generated_code, LIBXSMM_ERR_UNSUP_DATATYPE );
    return;
  }

  if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_INDEXED_VEC) > 0) {
    use_indexed_vec = 1;
  }

  if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_IMPLICIT_INDEXED_VEC) > 0) {
    use_indexed_vec = 1;
    use_implicitly_indexed_vec = 1;
  }

  if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_IMPLICIT_INDEXED_VECIDX) > 0) {
    use_implicitly_indexed_vecidx = 1;
  }

  pf_instr      = (pf_type == 2) ? LIBXSMM_X86_INSTR_PREFETCHT1 : LIBXSMM_X86_INSTR_PREFETCHT0 ;
  vstore_instr  = (use_nts == 0) ? i_micro_kernel_config->vmove_instruction_out : LIBXSMM_X86_INSTR_VMOVNTPS;

  i_gp_reg_mapping->gp_reg_n        = LIBXSMM_X86_GP_REG_R8;
  i_gp_reg_mapping->gp_reg_ind_base = LIBXSMM_X86_GP_REG_R9;
  i_gp_reg_mapping->gp_reg_in_base  = LIBXSMM_X86_GP_REG_R10;
  i_gp_reg_mapping->gp_reg_out      = LIBXSMM_X86_GP_REG_R11;
  i_gp_reg_mapping->gp_reg_m_loop   = LIBXSMM_X86_GP_REG_RAX;
  i_gp_reg_mapping->gp_reg_n_loop   = LIBXSMM_X86_GP_REG_RDX;
  i_gp_reg_mapping->gp_reg_in       = LIBXSMM_X86_GP_REG_RSI;
  i_gp_reg_mapping->gp_reg_in_pf    = LIBXSMM_X86_GP_REG_RCX;
  i_gp_reg_mapping->gp_reg_invec    = LIBXSMM_X86_GP_REG_RDI;

  i_gp_reg_mapping->gp_reg_in_base2  = LIBXSMM_X86_GP_REG_RDI;
  i_gp_reg_mapping->gp_reg_ind_base2 = LIBXSMM_X86_GP_REG_R12;
  i_gp_reg_mapping->gp_reg_in2       = LIBXSMM_X86_GP_REG_R13;
  i_gp_reg_mapping->gp_reg_in_pf2    = LIBXSMM_X86_GP_REG_R14;

  libxsmm_x86_instruction_alu_mem( io_generated_code,
      i_micro_kernel_config->alu_mov_instruction,
      i_gp_reg_mapping->gp_reg_param_struct,
      LIBXSMM_X86_GP_REG_UNDEF, 0,
      0,
      i_gp_reg_mapping->gp_reg_n,
      0 );

  libxsmm_x86_instruction_alu_imm(io_generated_code, i_micro_kernel_config->alu_cmp_instruction, i_gp_reg_mapping->gp_reg_n, 0);
  libxsmm_x86_instruction_jump_to_label(io_generated_code, LIBXSMM_X86_INSTR_JLE, END_LABEL, p_jump_label_tracker);

  if (use_stack_vars > 0) {
    libxsmm_x86_instruction_push_reg( io_generated_code, LIBXSMM_X86_GP_REG_RBP );
    libxsmm_x86_instruction_alu_reg( io_generated_code, i_micro_kernel_config->alu_mov_instruction, LIBXSMM_X86_GP_REG_RSP, LIBXSMM_X86_GP_REG_RBP);
    libxsmm_x86_instruction_alu_imm( io_generated_code, i_micro_kernel_config->alu_sub_instruction, LIBXSMM_X86_GP_REG_RSP, 32 );
    libxsmm_x86_instruction_push_reg( io_generated_code, temp_gpr );
  }

  if ( record_argop_off_vec0 == 1 ) {
    libxsmm_x86_instruction_alu_mem( io_generated_code,
        i_micro_kernel_config->alu_mov_instruction,
        i_gp_reg_mapping->gp_reg_param_struct,
        LIBXSMM_X86_GP_REG_UNDEF, 0,
        64,
        temp_gpr,
        0 );
    libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_argop_ptr0, temp_gpr, 1 );
  }

  if ( record_argop_off_vec1 == 1 ) {
    libxsmm_x86_instruction_alu_mem( io_generated_code,
        i_micro_kernel_config->alu_mov_instruction,
        i_gp_reg_mapping->gp_reg_param_struct,
        LIBXSMM_X86_GP_REG_UNDEF, 0,
        72,
        temp_gpr,
        0 );
    libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_argop_ptr1, temp_gpr, 1 );
  }

  if (((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_OP_COPY) > 0) && ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_OPORDER_VECIN_VECIDX) > 0)) {
    vecidx_ind_base_param_offset = 48;
    vecidx_in_base_param_offset = 56;
    if (record_argop_off_vec1 > 0) {
      record_argop_off_vec0 = 1;
      record_argop_off_vec1 = 0;
      rbp_offset_argop_ptr0 = rbp_offset_argop_ptr1;
    }
  }

  if (use_implicitly_indexed_vecidx == 0) {
    libxsmm_x86_instruction_alu_mem( io_generated_code,
        i_micro_kernel_config->alu_mov_instruction,
        i_gp_reg_mapping->gp_reg_param_struct,
        LIBXSMM_X86_GP_REG_UNDEF, 0,
        vecidx_ind_base_param_offset,
        i_gp_reg_mapping->gp_reg_ind_base,
        0 );
  }

  libxsmm_x86_instruction_alu_mem( io_generated_code,
      i_micro_kernel_config->alu_mov_instruction,
      i_gp_reg_mapping->gp_reg_param_struct,
      LIBXSMM_X86_GP_REG_UNDEF, 0,
      vecidx_in_base_param_offset,
      i_gp_reg_mapping->gp_reg_in_base,
      0 );

  libxsmm_x86_instruction_alu_mem( io_generated_code,
      i_micro_kernel_config->alu_mov_instruction,
      i_gp_reg_mapping->gp_reg_param_struct,
      LIBXSMM_X86_GP_REG_UNDEF, 0,
      32,
      i_gp_reg_mapping->gp_reg_out,
      0 );

  if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_SCALE_OP_RESULT) > 0) {
    scale_op_result = 1;
    if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_OP_COPY) > 0) {
      i_gp_reg_mapping->gp_reg_scale_base = LIBXSMM_X86_GP_REG_RDI;
    } else if (pf_dist == 0) {
      i_gp_reg_mapping->gp_reg_scale_base = LIBXSMM_X86_GP_REG_RCX;
    } else {
      i_gp_reg_mapping->gp_reg_scale_base = LIBXSMM_X86_GP_REG_R15;
      pushpop_scaleop_reg = 1;
      libxsmm_x86_instruction_push_reg( io_generated_code, i_gp_reg_mapping->gp_reg_scale_base );
    }

    libxsmm_x86_instruction_alu_mem( io_generated_code,
        i_micro_kernel_config->alu_mov_instruction,
        i_gp_reg_mapping->gp_reg_param_struct,
        LIBXSMM_X86_GP_REG_UNDEF, 0,
        40,
        i_gp_reg_mapping->gp_reg_scale_base,
        0 );
  }

  if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_OP_COPY) == 0) {
    apply_op = 1;
    if (use_indexed_vec > 0) {
      libxsmm_x86_instruction_push_reg( io_generated_code, i_gp_reg_mapping->gp_reg_in2 );
      if (pf_dist > 0) {
        libxsmm_x86_instruction_push_reg( io_generated_code, i_gp_reg_mapping->gp_reg_in_pf2 );
      }
      if (use_implicitly_indexed_vec == 0) {
        libxsmm_x86_instruction_push_reg( io_generated_code, i_gp_reg_mapping->gp_reg_ind_base2 );
        libxsmm_x86_instruction_alu_mem( io_generated_code,
            i_micro_kernel_config->alu_mov_instruction,
            i_gp_reg_mapping->gp_reg_param_struct,
            LIBXSMM_X86_GP_REG_UNDEF, 0,
            48,
            i_gp_reg_mapping->gp_reg_ind_base2,
            0 );
      }
      libxsmm_x86_instruction_alu_mem( io_generated_code,
          i_micro_kernel_config->alu_mov_instruction,
          i_gp_reg_mapping->gp_reg_param_struct,
          LIBXSMM_X86_GP_REG_UNDEF, 0,
          56,
          i_gp_reg_mapping->gp_reg_in_base2,
          0 );
    } else {
      libxsmm_x86_instruction_alu_mem( io_generated_code,
          i_micro_kernel_config->alu_mov_instruction,
          i_gp_reg_mapping->gp_reg_param_struct,
          LIBXSMM_X86_GP_REG_UNDEF, 0,
          24,
          i_gp_reg_mapping->gp_reg_invec,
          0 );
    }
  }

  if (apply_op == 1) {
    if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_OPORDER_VECIN_VECIDX) > 0) {
      op_order = 1;
    } else if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_OPORDER_VECIDX_VECIN) > 0) {
      op_order = 0;
    } else {
#if defined(LIBXSMM_GENERATOR_MATELTWISE_REDUCE_AVX_AVX512_JUMP_LABEL_TRACKER_MALLOC)
      free(p_jump_label_tracker);
#endif
      /* This should not happen  */
      LIBXSMM_HANDLE_ERROR( io_generated_code, LIBXSMM_ERR_GENERAL );
      return;
    }

    if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_OP_ADD) > 0) {
      op_instr = LIBXSMM_X86_INSTR_VADDPS;
    } else if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_OP_SUB) > 0) {
      op_instr = LIBXSMM_X86_INSTR_VSUBPS;
    } else if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_OP_MUL) > 0) {
      op_instr = LIBXSMM_X86_INSTR_VMULPS;
    } else if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_OP_DIV) > 0) {
      op_instr = LIBXSMM_X86_INSTR_VDIVPS;
    } else if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_OP_DOT) > 0) {
      op_instr = LIBXSMM_X86_INSTR_DOTPS;
    } else {
#if defined(LIBXSMM_GENERATOR_MATELTWISE_REDUCE_AVX_AVX512_JUMP_LABEL_TRACKER_MALLOC)
      free(p_jump_label_tracker);
#endif
      /* This should not happen  */
      LIBXSMM_HANDLE_ERROR( io_generated_code, LIBXSMM_ERR_GENERAL );
      return;
    }
  }

  if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_REDOP_NONE) == 0) {
    apply_redop = 1;
    if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_REDOP_SUM) > 0) {
      reduceop_instr = LIBXSMM_X86_INSTR_VADDPS;
    } else if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_REDOP_MAX) > 0) {
      reduceop_instr = LIBXSMM_X86_INSTR_VRANGEPS;
      reduceop_imm = 5;
      argop_cmp_imm = 6;
      if (io_generated_code->arch < LIBXSMM_X86_AVX512_CORE) {
        reduceop_instr = LIBXSMM_X86_INSTR_VMAXPS;
        reduceop_imm = 0;
      }
    } else if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_REDOP_MIN) > 0) {
      reduceop_instr = LIBXSMM_X86_INSTR_VRANGEPS;
      reduceop_imm = 4;
      argop_cmp_imm = 9;
      if (io_generated_code->arch < LIBXSMM_X86_AVX512_CORE) {
        reduceop_instr = LIBXSMM_X86_INSTR_VMINPS;
        reduceop_imm = 0;
      }
    } else {
#if defined(LIBXSMM_GENERATOR_MATELTWISE_REDUCE_AVX_AVX512_JUMP_LABEL_TRACKER_MALLOC)
      free(p_jump_label_tracker);
#endif
      /* This should not happen  */
      LIBXSMM_HANDLE_ERROR( io_generated_code, LIBXSMM_ERR_GENERAL );
      return;
    }
  } else {
    vecin_offset    = vecout_offset;
    if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_OP_COPY) > 0) {
      vecidxin_offset = vecout_offset;
    }
  }

  m                 = i_mateltwise_desc->m;
  use_m_masking     = ( m % vlen == 0 ) ? 0 : 1;
  m_trips           = (m + vlen - 1) / vlen;
  m_unroll_factor   = m_trips;
  m_trips_loop      = 1;
  peeled_m_trips    = 0;

  if (use_m_masking == 1) {
    if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
      /* Calculate mask reg 1 for reading/output-writing */
      mask_out_count = vlen - (m % vlen);
      libxsmm_generator_mateltwise_initialize_avx512_mask(io_generated_code, LIBXSMM_X86_GP_REG_RAX, mask_inout, mask_out_count, LIBXSMM_GEMM_PRECISION_F32);
    } else {
      libxsmm_generator_mateltwise_initialize_avx_mask(io_generated_code, mask_inout, m % vlen);
    }
  }

  if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
    if ((idx_tsize == 8) && (m % 8 != 0) && ((record_argop_off_vec0 > 0) || (record_argop_off_vec1 > 0))) {
      libxsmm_generator_mateltwise_initialize_avx512_mask(io_generated_code, LIBXSMM_X86_GP_REG_RAX, mask_argidx64, 8 - (m % 8), LIBXSMM_GEMM_PRECISION_F64);
    }
  } else {
    if ((idx_tsize == 8) && (m % 4 != 0) && ((record_argop_off_vec0 > 0) || (record_argop_off_vec1 > 0))) {
      unsigned long long  mask_array[4] = {0, 0, 0, 0};
      unsigned int _i_;
      for (_i_ = 0; _i_ < m % 4 ; _i_++) {
        mask_array[_i_] = 0xFFFFFFFFFFFFFFFF;
      }
      libxsmm_x86_instruction_full_vec_load_of_constants ( io_generated_code, (const unsigned char *) mask_array, "mask_array", 'y', mask_argidx64 );
    }
  }

  /* In this case we have to use CPX replacement sequence for downconverts... */
  if ((io_generated_code->arch < LIBXSMM_X86_AVX512_CPX) && (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) ) {
    /* init stack with helper variables for SW-based RNE rounding */
    /* push 0x7f800000 on the stack, naninf masking */
    libxsmm_x86_instruction_push_reg( io_generated_code, LIBXSMM_X86_GP_REG_RAX );

    libxsmm_x86_instruction_alu_imm( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RAX, 0x7f800000);
    libxsmm_x86_instruction_push_reg( io_generated_code, LIBXSMM_X86_GP_REG_RAX );

    /* push 0x00010000 on the stack, fixup masking */
    libxsmm_x86_instruction_alu_imm( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RAX, 0x00010000);
    libxsmm_x86_instruction_push_reg( io_generated_code, LIBXSMM_X86_GP_REG_RAX );

    /* push 0x00007fff on the stack, rneadd */
    libxsmm_x86_instruction_alu_imm( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RAX, 0x00007fff);
    libxsmm_x86_instruction_push_reg( io_generated_code, LIBXSMM_X86_GP_REG_RAX);

    /* push 0x00000001 on the stack, fixup */
    libxsmm_x86_instruction_alu_imm( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RAX, 0x00000001);
    libxsmm_x86_instruction_push_reg( io_generated_code, LIBXSMM_X86_GP_REG_RAX );
  }

  /* In this case we have to generate a loop for m */
  if (m_unroll_factor > max_m_unrolling) {
    m_unroll_factor = max_m_unrolling;
    m_trips_loop = m_trips/m_unroll_factor;
    peeled_m_trips = m_trips  - m_unroll_factor * m_trips_loop;
    if ((use_m_masking > 0) && (peeled_m_trips == 0)) {
      m_trips_loop--;
      peeled_m_trips = m_trips  - m_unroll_factor * m_trips_loop;
    }
  } else {
    if ((use_m_masking > 0) && (peeled_m_trips == 0)) {
      m_trips_loop--;
      peeled_m_trips = m_trips  - m_unroll_factor * m_trips_loop;
    }
  }

  if (m_trips_loop > 1) {
    libxsmm_generator_mateltwise_header_m_loop( io_generated_code, io_loop_label_tracker, i_micro_kernel_config, i_gp_reg_mapping->gp_reg_m_loop );
  }

  if (m_trips_loop >= 1) {
    for (im = 0; im < m_unroll_factor; im++) {
      /* Load output for reduction  */
      if (apply_redop == 1) {
        if (load_acc == 0) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VPXORD,
                                                 i_micro_kernel_config->vector_name,
                                                 im + vecout_offset, im + vecout_offset, im + vecout_offset);
        } else {
          char vname = (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) ? 'y' : 'z';
          vname = (io_generated_code->arch < LIBXSMM_X86_AVX512) ? 'y' : vname;
          libxsmm_x86_instruction_unified_vec_move( io_generated_code,
              i_micro_kernel_config->vmove_instruction_in,
              i_gp_reg_mapping->gp_reg_out,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              im * vlen  * i_micro_kernel_config->datatype_size_out,
              vname,
              im + vecout_offset, 0, 0, 0 );

          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            /* convert 16 bit values into 32 bit (integer convert) */
            libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
                LIBXSMM_X86_INSTR_VPMOVSXWD,
                i_micro_kernel_config->vector_name,
                im + vecout_offset, im + vecout_offset );

            /* shift 16 bits to the left to generate valid FP32 numbers */
            libxsmm_x86_instruction_vec_compute_2reg_imm8( io_generated_code,
                LIBXSMM_X86_INSTR_VPSLLD_I,
                i_micro_kernel_config->vector_name,
                im + vecout_offset,
                im + vecout_offset,
                16);
          }
        }
      }

      /* Initialize argop vectors if need be  */
      if (record_argop_off_vec0 == 1) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                               LIBXSMM_X86_INSTR_VPXORD,
                                               i_micro_kernel_config->vector_name,
                                               im + vecargop_vec0_offset, im + vecargop_vec0_offset, im + vecargop_vec0_offset);
        if (idx_tsize == 8) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VPXORD,
                                                 i_micro_kernel_config->vector_name,
                                                 im + max_m_unrolling + vecargop_vec0_offset, im + max_m_unrolling + vecargop_vec0_offset, im + max_m_unrolling + vecargop_vec0_offset);
        }
      }

      /* Initialize argop vectors if need be  */
      if (record_argop_off_vec1 == 1) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                               LIBXSMM_X86_INSTR_VPXORD,
                                               i_micro_kernel_config->vector_name,
                                               im + vecargop_vec1_offset, im + vecargop_vec1_offset, im + vecargop_vec1_offset);
        if (idx_tsize == 8) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VPXORD,
                                                 i_micro_kernel_config->vector_name,
                                                 im + max_m_unrolling + vecargop_vec1_offset, im + max_m_unrolling + vecargop_vec1_offset, im + max_m_unrolling + vecargop_vec1_offset);
        }
      }

      /* Load input vector in case we have to apply op */
      if (apply_op == 1) {
        if (use_indexed_vec == 0) {
          char vname = (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) ? 'y' : 'z';
          vname = (io_generated_code->arch < LIBXSMM_X86_AVX512) ? 'y' : vname;

          libxsmm_x86_instruction_unified_vec_move( io_generated_code,
              i_micro_kernel_config->vmove_instruction_in,
              i_gp_reg_mapping->gp_reg_invec,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              im * vlen  * i_micro_kernel_config->datatype_size_out,
              vname,
              im + vecin_offset, 0, 0, 0 );

          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            /* convert 16 bit values into 32 bit (integer convert) */
            libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
                LIBXSMM_X86_INSTR_VPMOVSXWD,
                i_micro_kernel_config->vector_name,
                im + vecin_offset, im + vecin_offset );

            /* shift 16 bits to the left to generate valid FP32 numbers */
            libxsmm_x86_instruction_vec_compute_2reg_imm8( io_generated_code,
                LIBXSMM_X86_INSTR_VPSLLD_I,
                i_micro_kernel_config->vector_name,
                im + vecin_offset,
                im + vecin_offset,
                16);
          }
        }
      }
    }

    if (pf_dist > 0) {
      libxsmm_x86_instruction_alu_imm(io_generated_code, i_micro_kernel_config->alu_mov_instruction, i_gp_reg_mapping->gp_reg_n_loop, 0);
      libxsmm_x86_instruction_alu_imm(io_generated_code, i_micro_kernel_config->alu_cmp_instruction, i_gp_reg_mapping->gp_reg_n, pf_dist);
      libxsmm_x86_instruction_jump_to_label(io_generated_code, LIBXSMM_X86_INSTR_JLE, NO_PF_LABEL_START, p_jump_label_tracker);

      libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_SUBQ, i_gp_reg_mapping->gp_reg_n, pf_dist);
      libxsmm_generator_mateltwise_header_n_dyn_loop(io_generated_code, io_loop_label_tracker, i_micro_kernel_config, i_gp_reg_mapping->gp_reg_n_loop, 1);

      if (use_indexed_vecidx > 0) {
        if (use_implicitly_indexed_vecidx == 0) {
          libxsmm_x86_instruction_alu_mem(io_generated_code, ind_alu_mov_instr, i_gp_reg_mapping->gp_reg_ind_base, i_gp_reg_mapping->gp_reg_n_loop, idx_tsize, 0, i_gp_reg_mapping->gp_reg_in, 0);
          libxsmm_x86_instruction_alu_mem(io_generated_code, ind_alu_mov_instr, i_gp_reg_mapping->gp_reg_ind_base, i_gp_reg_mapping->gp_reg_n_loop, idx_tsize, pf_dist * idx_tsize, i_gp_reg_mapping->gp_reg_in_pf, 0);
        } else {
          libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_mov_instruction, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_in);
          libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_mov_instruction, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_in_pf);
          libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_ADDQ, i_gp_reg_mapping->gp_reg_in_pf, pf_dist);
        }
        if (record_argop_off_vec0 == 1) {
          if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
            libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8(io_generated_code, gpr_bcast_idx_instr, vname_argop_bcast, i_gp_reg_mapping->gp_reg_in, LIBXSMM_X86_VEC_REG_UNDEF, temp_vreg_argop0, 0, 0, 0, 0);
          } else {
            libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_idx, i_gp_reg_mapping->gp_reg_in, 1 );
            libxsmm_x86_instruction_vec_move(io_generated_code, i_micro_kernel_config->instruction_set, bcast_idx_instr, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_idx, vname_argop_bcast, temp_vreg_argop0, 0, 0, 0);
          }
        }
        libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_IMUL, i_gp_reg_mapping->gp_reg_in, i_mateltwise_desc->ldi * in_tsize);
        libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_IMUL, i_gp_reg_mapping->gp_reg_in_pf, i_mateltwise_desc->ldi * in_tsize);
        libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_in_base, i_gp_reg_mapping->gp_reg_in);
        libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_in_base, i_gp_reg_mapping->gp_reg_in_pf);
      }

      if (use_indexed_vec > 0) {
        if (use_implicitly_indexed_vec == 0) {
          libxsmm_x86_instruction_alu_mem(io_generated_code, ind_alu_mov_instr, i_gp_reg_mapping->gp_reg_ind_base2, i_gp_reg_mapping->gp_reg_n_loop, idx_tsize, 0, i_gp_reg_mapping->gp_reg_in2, 0);
          libxsmm_x86_instruction_alu_mem(io_generated_code, ind_alu_mov_instr, i_gp_reg_mapping->gp_reg_ind_base2, i_gp_reg_mapping->gp_reg_n_loop, idx_tsize, pf_dist * idx_tsize, i_gp_reg_mapping->gp_reg_in_pf2, 0);
        } else {
          libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_mov_instruction, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_in2);
          libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_mov_instruction, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_in_pf2);
          libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_ADDQ, i_gp_reg_mapping->gp_reg_in_pf2, pf_dist);
        }
        if (record_argop_off_vec1 == 1) {
          if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
            libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8(io_generated_code, gpr_bcast_idx_instr, vname_argop_bcast, i_gp_reg_mapping->gp_reg_in2, LIBXSMM_X86_VEC_REG_UNDEF, temp_vreg_argop1, 0, 0, 0, 0);
          } else {
            libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_idx, i_gp_reg_mapping->gp_reg_in2, 1 );
            libxsmm_x86_instruction_vec_move(io_generated_code, i_micro_kernel_config->instruction_set, bcast_idx_instr, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_idx, vname_argop_bcast, temp_vreg_argop1, 0, 0, 0);
          }
        }
        libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_IMUL, i_gp_reg_mapping->gp_reg_in2, i_mateltwise_desc->ldi * in_tsize);
        libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_IMUL, i_gp_reg_mapping->gp_reg_in_pf2, i_mateltwise_desc->ldi * in_tsize);
        libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_in_base2, i_gp_reg_mapping->gp_reg_in2);
        libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_in_base2, i_gp_reg_mapping->gp_reg_in_pf2);
      }

      for (im = 0; im < m_unroll_factor; im++) {
        /* First load the indexed vector */
        char vname = (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) ? 'y' : 'z';
        vname = (io_generated_code->arch < LIBXSMM_X86_AVX512) ? 'y' : vname;

        libxsmm_x86_instruction_unified_vec_move( io_generated_code,
          i_micro_kernel_config->vmove_instruction_in,
          i_gp_reg_mapping->gp_reg_in,
          LIBXSMM_X86_GP_REG_UNDEF, 0,
          im * vlen * i_micro_kernel_config->datatype_size_in,
          vname,
          im + vecidxin_offset, 0, 0, 0 );

        if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) {
          /* convert 16 bit values into 32 bit (integer convert) */
          libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
              LIBXSMM_X86_INSTR_VPMOVSXWD,
              i_micro_kernel_config->vector_name,
              im + vecidxin_offset, im + vecidxin_offset );

          /* shift 16 bits to the left to generate valid FP32 numbers */
          libxsmm_x86_instruction_vec_compute_2reg_imm8(io_generated_code,
              LIBXSMM_X86_INSTR_VPSLLD_I,
              i_micro_kernel_config->vector_name,
              im + vecidxin_offset,
              im + vecidxin_offset,
              16);
        }

        /* Now apply the OP among the indexed vector and the input vector */
        if (apply_op == 1) {
          if (use_indexed_vec > 0) {
          libxsmm_x86_instruction_unified_vec_move( io_generated_code,
              i_micro_kernel_config->vmove_instruction_in,
              i_gp_reg_mapping->gp_reg_in2,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              im * vlen  * i_micro_kernel_config->datatype_size_in,
              vname,
              im + vecin_offset, 0, 0, 0 );

            if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) {
              /* convert 16 bit values into 32 bit (integer convert) */
              libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
                  LIBXSMM_X86_INSTR_VPMOVSXWD,
                  i_micro_kernel_config->vector_name,
                  im + vecin_offset, im + vecin_offset );

              /* shift 16 bits to the left to generate valid FP32 numbers */
              libxsmm_x86_instruction_vec_compute_2reg_imm8(io_generated_code,
                  LIBXSMM_X86_INSTR_VPSLLD_I,
                  i_micro_kernel_config->vector_name,
                  im + vecin_offset,
                  im + vecin_offset,
                  16);
            }
          }

          if (op_instr == LIBXSMM_X86_INSTR_DOTPS) {
            /* TODO: Add DOT op sequence here  */
          } else {
            libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code,
                op_instr,
                i_micro_kernel_config->vector_name,
                (op_order == 0)    ? im + vecin_offset : im + vecidxin_offset,
                (op_order == 0)    ? im + vecidxin_offset : im + vecin_offset,
                (apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset,
                op_mask,
                op_mask_cntl,
                op_sae_cntl,
                op_imm);
          }
        }

        if (scale_op_result == 1) {
          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            libxsmm_x86_instruction_vec_move(io_generated_code,
                i_micro_kernel_config->instruction_set,
                LIBXSMM_X86_INSTR_VPBROADCASTW,
                i_gp_reg_mapping->gp_reg_scale_base,
                i_gp_reg_mapping->gp_reg_n_loop,
                in_tsize,
                0, 'y',
                temp_vreg,
                0,
                0,
                0);

            libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
                LIBXSMM_X86_INSTR_VPMOVSXWD,
                i_micro_kernel_config->vector_name,
                temp_vreg, temp_vreg );

            libxsmm_x86_instruction_vec_compute_2reg_imm8( io_generated_code,
                LIBXSMM_X86_INSTR_VPSLLD_I,
                i_micro_kernel_config->vector_name,
                temp_vreg,
                temp_vreg,
                16);

            libxsmm_x86_instruction_vec_compute_3reg(io_generated_code,
                LIBXSMM_X86_INSTR_VMULPS,
                i_micro_kernel_config->vector_name,
                temp_vreg,
                (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset,
                (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset);
          } else {
            if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
              libxsmm_x86_instruction_vec_compute_mem_2reg_mask_imm8( io_generated_code,
                  LIBXSMM_X86_INSTR_VMULPS,
                  i_micro_kernel_config->vector_name,
                  i_gp_reg_mapping->gp_reg_scale_base,
                  i_gp_reg_mapping->gp_reg_n_loop,
                  4, 0, 1,
                  (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset,
                  (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset,
                  0, 0, 0);
            } else {
              libxsmm_x86_instruction_vec_move(io_generated_code,
                  i_micro_kernel_config->instruction_set,
                  LIBXSMM_X86_INSTR_VBROADCASTSS,
                  i_gp_reg_mapping->gp_reg_scale_base,
                  i_gp_reg_mapping->gp_reg_n_loop,
                  in_tsize,
                  0, 'y',
                  temp_vreg,
                  0,
                  0,
                  0);

              libxsmm_x86_instruction_vec_compute_3reg(io_generated_code,
                  LIBXSMM_X86_INSTR_VMULPS,
                  i_micro_kernel_config->vector_name,
                  temp_vreg,
                  (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset,
                  (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset);
            }
          }
        }

        /* Now apply the Reduce OP */
        if (apply_redop == 1) {
          if ((record_argop_off_vec0 == 1) ||(record_argop_off_vec1 == 1)) {
            libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code, argop_cmp_instr, i_micro_kernel_config->vector_name, im + vecidxin_offset, im + vecout_offset, argop_mask, argop_cmp_imm );
            if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
              if (record_argop_off_vec0 == 1) {
                libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + vecargop_vec0_offset, temp_vreg_argop0, im + vecargop_vec0_offset, argop_mask, 0 );
              }
              if (record_argop_off_vec1 == 1) {
                libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + vecargop_vec1_offset, temp_vreg_argop1, im + vecargop_vec1_offset, argop_mask, 0 );
              }
              if (idx_tsize == 8) {
                libxsmm_x86_instruction_mask_compute_reg( io_generated_code,LIBXSMM_X86_INSTR_KSHIFTRW, argop_mask, LIBXSMM_X86_VEC_REG_UNDEF, argop_mask, 8);
                if (record_argop_off_vec0 == 1) {
                  libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + max_m_unrolling + vecargop_vec0_offset, temp_vreg_argop0, im + max_m_unrolling + vecargop_vec0_offset, argop_mask, 0 );
                }
                if (record_argop_off_vec1 == 1) {
                  libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + max_m_unrolling + vecargop_vec1_offset, temp_vreg_argop1, im + max_m_unrolling + vecargop_vec1_offset, argop_mask, 0 );
                }
              }
            } else {
              if (idx_tsize == 8) {
                libxsmm_generator_avx_extract_mask4_from_mask8( io_generated_code, argop_mask, argop_mask_aux, 1);
                libxsmm_generator_avx_extract_mask4_from_mask8( io_generated_code, argop_mask, argop_mask, 0);
              }
              if (record_argop_off_vec0 == 1) {
                libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + vecargop_vec0_offset, temp_vreg_argop0, im + vecargop_vec0_offset, 0, 0, 0, argop_mask << 4);
              }
              if (record_argop_off_vec1 == 1) {
                libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + vecargop_vec1_offset, temp_vreg_argop1, im + vecargop_vec1_offset, 0, 0, 0, argop_mask << 4);
              }
              if (idx_tsize == 8) {
                if (record_argop_off_vec0 == 1) {
                  libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + max_m_unrolling + vecargop_vec0_offset, temp_vreg_argop0, im + max_m_unrolling + vecargop_vec0_offset, 0, 0, 0, argop_mask_aux << 4);
                }
                if (record_argop_off_vec1 == 1) {
                  libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + max_m_unrolling + vecargop_vec1_offset, temp_vreg_argop1, im + max_m_unrolling + vecargop_vec1_offset, 0, 0, 0, argop_mask_aux << 4);
                }
              }
            }
          }

          libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code,
              reduceop_instr,
              i_micro_kernel_config->vector_name,
              im + vecidxin_offset,
              im + vecout_offset,
              im + vecout_offset,
              reduceop_mask,
              reduceop_mask_cntl,
              reduceop_sae_cntl,
              reduceop_imm);
        }

        if ((im * vlen * i_micro_kernel_config->datatype_size_in) % 64 == 0 ) {
          libxsmm_x86_instruction_prefetch(io_generated_code,
              pf_instr,
              i_gp_reg_mapping->gp_reg_in_pf,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              im * vlen * i_micro_kernel_config->datatype_size_in);
          if (use_indexed_vec > 0){
            libxsmm_x86_instruction_prefetch(io_generated_code,
                pf_instr,
                i_gp_reg_mapping->gp_reg_in_pf2,
                LIBXSMM_X86_GP_REG_UNDEF, 0,
                im * vlen * i_micro_kernel_config->datatype_size_in);
          }
        }
      }

      libxsmm_generator_mateltwise_footer_n_dyn_loop(io_generated_code, io_loop_label_tracker, i_micro_kernel_config, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_n);
      libxsmm_x86_instruction_alu_imm(io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_n, pf_dist);
      /* NO_PF_LABEL_START */
      libxsmm_x86_instruction_register_jump_label(io_generated_code, NO_PF_LABEL_START, p_jump_label_tracker);
    }

    /* Perform the reductions for all columns */
    libxsmm_generator_mateltwise_header_n_dyn_loop(io_generated_code, io_loop_label_tracker, i_micro_kernel_config, i_gp_reg_mapping->gp_reg_n_loop, (pf_dist > 0) ? 1 : 0);

    if (use_indexed_vecidx > 0) {
      if (use_implicitly_indexed_vecidx == 0) {
        libxsmm_x86_instruction_alu_mem(io_generated_code, ind_alu_mov_instr, i_gp_reg_mapping->gp_reg_ind_base, i_gp_reg_mapping->gp_reg_n_loop, idx_tsize, 0, i_gp_reg_mapping->gp_reg_in, 0);
      } else {
        libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_mov_instruction, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_in);
      }
      if (record_argop_off_vec0 == 1) {
        if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
          libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8(io_generated_code, gpr_bcast_idx_instr, vname_argop_bcast, i_gp_reg_mapping->gp_reg_in, LIBXSMM_X86_VEC_REG_UNDEF, temp_vreg_argop0, 0, 0, 0, 0);
        } else {
          libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_idx, i_gp_reg_mapping->gp_reg_in, 1 );
          libxsmm_x86_instruction_vec_move(io_generated_code, i_micro_kernel_config->instruction_set, bcast_idx_instr, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_idx, vname_argop_bcast, temp_vreg_argop0, 0, 0, 0);
        }
      }
      libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_IMUL, i_gp_reg_mapping->gp_reg_in, i_mateltwise_desc->ldi * in_tsize);
      libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_in_base, i_gp_reg_mapping->gp_reg_in);
    }

    if (use_indexed_vec > 0) {
      if (use_implicitly_indexed_vec == 0) {
        libxsmm_x86_instruction_alu_mem(io_generated_code, ind_alu_mov_instr, i_gp_reg_mapping->gp_reg_ind_base2, i_gp_reg_mapping->gp_reg_n_loop, idx_tsize, 0, i_gp_reg_mapping->gp_reg_in2, 0);
      } else {
        libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_mov_instruction, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_in2);
      }
      if (record_argop_off_vec1 == 1) {
        if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
          libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8(io_generated_code, gpr_bcast_idx_instr, vname_argop_bcast, i_gp_reg_mapping->gp_reg_in2, LIBXSMM_X86_VEC_REG_UNDEF, temp_vreg_argop1, 0, 0, 0, 0);
        } else {
          libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_idx, i_gp_reg_mapping->gp_reg_in2, 1 );
          libxsmm_x86_instruction_vec_move(io_generated_code, i_micro_kernel_config->instruction_set, bcast_idx_instr, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_idx, vname_argop_bcast, temp_vreg_argop1, 0, 0, 0);
        }
      }
      libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_IMUL, i_gp_reg_mapping->gp_reg_in2, i_mateltwise_desc->ldi * in_tsize);
      libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_in_base2, i_gp_reg_mapping->gp_reg_in2);
    }

    for (im = 0; im < m_unroll_factor; im++) {
      /* First load the indexed vector */
      char vname = (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) ? 'y' : 'z';
      vname = (io_generated_code->arch < LIBXSMM_X86_AVX512) ? 'y' : vname;

      libxsmm_x86_instruction_unified_vec_move( io_generated_code,
          i_micro_kernel_config->vmove_instruction_in,
          i_gp_reg_mapping->gp_reg_in,
          LIBXSMM_X86_GP_REG_UNDEF, 0,
          im * vlen  * i_micro_kernel_config->datatype_size_in,
          vname,
          im + vecidxin_offset, 0, 0, 0 );

      if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) {
        /* convert 16 bit values into 32 bit (integer convert) */
        libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
            LIBXSMM_X86_INSTR_VPMOVSXWD,
            i_micro_kernel_config->vector_name,
            im + vecidxin_offset, im + vecidxin_offset );

        /* shift 16 bits to the left to generate valid FP32 numbers */
        libxsmm_x86_instruction_vec_compute_2reg_imm8(io_generated_code,
            LIBXSMM_X86_INSTR_VPSLLD_I,
            i_micro_kernel_config->vector_name,
            im + vecidxin_offset,
            im + vecidxin_offset,
            16);
      }

      /* Now apply the OP among the indexed vector and the input vector */
      if (apply_op == 1) {
        if (use_indexed_vec > 0) {
          libxsmm_x86_instruction_unified_vec_move( io_generated_code,
              i_micro_kernel_config->vmove_instruction_in,
              i_gp_reg_mapping->gp_reg_in2,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              im * vlen  * i_micro_kernel_config->datatype_size_in,
              vname,
              im + vecin_offset, 0, 0, 0 );

          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) {
            /* convert 16 bit values into 32 bit (integer convert) */
            libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
                LIBXSMM_X86_INSTR_VPMOVSXWD,
                i_micro_kernel_config->vector_name,
                im + vecin_offset, im + vecin_offset );

            /* shift 16 bits to the left to generate valid FP32 numbers */
            libxsmm_x86_instruction_vec_compute_2reg_imm8(io_generated_code,
                LIBXSMM_X86_INSTR_VPSLLD_I,
                i_micro_kernel_config->vector_name,
                im + vecin_offset,
                im + vecin_offset,
                16);
          }
        }
        if (op_instr == LIBXSMM_X86_INSTR_DOTPS) {
          /* TODO: Add DOT op sequence here  */
        } else {
          libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code,
              op_instr,
              i_micro_kernel_config->vector_name,
              (op_order == 0)    ? im + vecin_offset : im + vecidxin_offset,
              (op_order == 0)    ? im + vecidxin_offset : im + vecin_offset,
              (apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset,
              op_mask,
              op_mask_cntl,
              op_sae_cntl,
              op_imm);
        }
      }

      if (scale_op_result == 1) {
        if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
          libxsmm_x86_instruction_vec_move(io_generated_code,
              i_micro_kernel_config->instruction_set,
              LIBXSMM_X86_INSTR_VPBROADCASTW,
              i_gp_reg_mapping->gp_reg_scale_base,
              i_gp_reg_mapping->gp_reg_n_loop,
              in_tsize,
              0, 'y',
              temp_vreg,
              0,
              0,
              0);

          libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
              LIBXSMM_X86_INSTR_VPMOVSXWD,
              i_micro_kernel_config->vector_name,
              temp_vreg, temp_vreg );

          libxsmm_x86_instruction_vec_compute_2reg_imm8(io_generated_code,
              LIBXSMM_X86_INSTR_VPSLLD_I,
              i_micro_kernel_config->vector_name,
              temp_vreg,
              temp_vreg,
              16);

          libxsmm_x86_instruction_vec_compute_3reg(io_generated_code,
              LIBXSMM_X86_INSTR_VMULPS,
              i_micro_kernel_config->vector_name,
              temp_vreg,
              (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset,
              (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset);
        } else {
          if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
            libxsmm_x86_instruction_vec_compute_mem_2reg_mask_imm8( io_generated_code,
                LIBXSMM_X86_INSTR_VMULPS,
                i_micro_kernel_config->vector_name,
                i_gp_reg_mapping->gp_reg_scale_base,
                i_gp_reg_mapping->gp_reg_n_loop,
                4, 0, 1,
                (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset,
                (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset,
                0, 0, 0);
          } else {
            libxsmm_x86_instruction_vec_move(io_generated_code,
                i_micro_kernel_config->instruction_set,
                LIBXSMM_X86_INSTR_VBROADCASTSS,
                i_gp_reg_mapping->gp_reg_scale_base,
                i_gp_reg_mapping->gp_reg_n_loop,
                in_tsize,
                0, 'y',
                temp_vreg,
                0,
                0,
                0);

            libxsmm_x86_instruction_vec_compute_3reg(io_generated_code,
                LIBXSMM_X86_INSTR_VMULPS,
                i_micro_kernel_config->vector_name,
                temp_vreg,
                (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset,
                (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset);
          }
        }
      }

      /* Now apply the Reduce OP */
      if (apply_redop == 1) {
        if ((record_argop_off_vec0 == 1) ||(record_argop_off_vec1 == 1)) {
          libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code, argop_cmp_instr, i_micro_kernel_config->vector_name, im + vecidxin_offset, im + vecout_offset, argop_mask, argop_cmp_imm );
          if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
            if (record_argop_off_vec0 == 1) {
              libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + vecargop_vec0_offset, temp_vreg_argop0, im + vecargop_vec0_offset, argop_mask, 0 );
            }
            if (record_argop_off_vec1 == 1) {
              libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + vecargop_vec1_offset, temp_vreg_argop1, im + vecargop_vec1_offset, argop_mask, 0 );
            }
            if (idx_tsize == 8) {
              libxsmm_x86_instruction_mask_compute_reg( io_generated_code,LIBXSMM_X86_INSTR_KSHIFTRW, argop_mask, LIBXSMM_X86_VEC_REG_UNDEF, argop_mask, 8);
              if (record_argop_off_vec0 == 1) {
                libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + max_m_unrolling + vecargop_vec0_offset, temp_vreg_argop0, im + max_m_unrolling + vecargop_vec0_offset, argop_mask, 0 );
              }
              if (record_argop_off_vec1 == 1) {
                libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + max_m_unrolling + vecargop_vec1_offset, temp_vreg_argop1, im + max_m_unrolling + vecargop_vec1_offset, argop_mask, 0 );
              }
            }
          } else {
            if (idx_tsize == 8) {
              libxsmm_generator_avx_extract_mask4_from_mask8( io_generated_code, argop_mask, argop_mask_aux, 1);
              libxsmm_generator_avx_extract_mask4_from_mask8( io_generated_code, argop_mask, argop_mask, 0);
            }
            if (record_argop_off_vec0 == 1) {
              libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + vecargop_vec0_offset, temp_vreg_argop0, im + vecargop_vec0_offset, 0, 0, 0, argop_mask << 4);
            }
            if (record_argop_off_vec1 == 1) {
              libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + vecargop_vec1_offset, temp_vreg_argop1, im + vecargop_vec1_offset, 0, 0, 0, argop_mask << 4);
            }
            if (idx_tsize == 8) {
              if (record_argop_off_vec0 == 1) {
                libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + max_m_unrolling + vecargop_vec0_offset, temp_vreg_argop0, im + max_m_unrolling + vecargop_vec0_offset, 0, 0, 0, argop_mask_aux << 4);
              }
              if (record_argop_off_vec1 == 1) {
                libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + max_m_unrolling + vecargop_vec1_offset, temp_vreg_argop1, im + max_m_unrolling + vecargop_vec1_offset, 0, 0, 0, argop_mask_aux << 4);
              }
            }
          }
        }

        libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code,
            reduceop_instr,
            i_micro_kernel_config->vector_name,
            im + vecidxin_offset,
            im + vecout_offset,
            im + vecout_offset,
            reduceop_mask,
            reduceop_mask_cntl,
            reduceop_sae_cntl,
            reduceop_imm);
      }
    }

    libxsmm_generator_mateltwise_footer_n_dyn_loop(io_generated_code, io_loop_label_tracker, i_micro_kernel_config, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_n);

    /* Now store accumulators  */
    for (im = 0; im < m_unroll_factor; im++) {
      char vname = (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) ? 'y' : 'z';
      vname = (io_generated_code->arch < LIBXSMM_X86_AVX512) ? 'y' : vname;

      if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
        if (io_generated_code->arch >= LIBXSMM_X86_AVX512_CPX) {
          libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
              LIBXSMM_X86_INSTR_VCVTNEPS2BF16,
              i_micro_kernel_config->vector_name,
              im + vecout_offset, im + vecout_offset);
        } else {
          libxsmm_generator_vcvtneps2bf16_avx512_preppedstack( io_generated_code, 'z', im + vecout_offset, im + vecout_offset, 30, 31, 6, 7 );
        }
      }
      libxsmm_x86_instruction_unified_vec_move( io_generated_code,
          vstore_instr,
          i_gp_reg_mapping->gp_reg_out,
          LIBXSMM_X86_GP_REG_UNDEF, 0,
          im * vlen  * i_micro_kernel_config->datatype_size_out,
          vname,
          im + vecout_offset, 0, 0, 1 );
    }

    if ( record_argop_off_vec0 == 1 ) {
      char vname = (io_generated_code->arch < LIBXSMM_X86_AVX512) ? 'y' : 'z';
      libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_argop_ptr0, temp_gpr, 0 );
      for (im = 0; im < m_unroll_factor; im++) {
        unsigned int vreg_id = (idx_tsize == 8) ? ((im % 2 == 0) ? im/2 + vecargop_vec0_offset : im/2 + max_m_unrolling + vecargop_vec0_offset)  : im + vecargop_vec0_offset;
        libxsmm_x86_instruction_unified_vec_move( io_generated_code,
            LIBXSMM_X86_INSTR_VMOVUPS,
            temp_gpr,
            LIBXSMM_X86_GP_REG_UNDEF, 0,
            im * vlen * 4,
            vname,
            vreg_id, 0, 0, 1 );
      }
      if (idx_tsize == 8) {
        for (im = m_unroll_factor; im < 2*m_unroll_factor; im++) {
          unsigned int vreg_id = (im % 2 == 0) ? im/2 + vecargop_vec0_offset : im/2 + max_m_unrolling + vecargop_vec0_offset;
          libxsmm_x86_instruction_unified_vec_move( io_generated_code,
              LIBXSMM_X86_INSTR_VMOVUPS,
              temp_gpr,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              im * vlen * 4,
              vname,
              vreg_id, 0, 0, 1 );
        }
      }
    }

    if ( record_argop_off_vec1 == 1 ) {
      char vname = (io_generated_code->arch < LIBXSMM_X86_AVX512) ? 'y' : 'z';
      libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_argop_ptr1, temp_gpr, 0 );
      for (im = 0; im < m_unroll_factor; im++) {
        unsigned int vreg_id = (idx_tsize == 8) ? ((im % 2 == 0) ? im/2 + vecargop_vec1_offset : im/2 + max_m_unrolling + vecargop_vec1_offset)  : im + vecargop_vec1_offset;
        libxsmm_x86_instruction_unified_vec_move( io_generated_code,
            LIBXSMM_X86_INSTR_VMOVUPS,
            temp_gpr,
            LIBXSMM_X86_GP_REG_UNDEF, 0,
            im * vlen * 4,
            vname,
            vreg_id, 0, 0, 1 );
      }
      if (idx_tsize == 8) {
        for (im = m_unroll_factor; im < 2*m_unroll_factor; im++) {
          unsigned int vreg_id = (im % 2 == 0) ? im/2 + vecargop_vec1_offset : im/2 + max_m_unrolling + vecargop_vec1_offset;
          libxsmm_x86_instruction_unified_vec_move( io_generated_code,
              LIBXSMM_X86_INSTR_VMOVUPS,
              temp_gpr,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              im * vlen * 4,
              vname,
              vreg_id, 0, 0, 1 );
        }
      }
    }
  }

  if (m_trips_loop > 1) {
    libxsmm_x86_instruction_alu_imm( io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_out, m_unroll_factor * vlen * i_micro_kernel_config->datatype_size_out);
    if ( record_argop_off_vec0 == 1 ) {
      libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_argop_ptr0, temp_gpr, 0 );
      libxsmm_x86_instruction_alu_imm( io_generated_code, i_micro_kernel_config->alu_add_instruction, temp_gpr, (idx_tsize == 8) ? m_unroll_factor * vlen * 8 : m_unroll_factor * vlen * 4);
      libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_argop_ptr0, temp_gpr, 1 );
    }
    if ( record_argop_off_vec1== 1 ) {
      libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_argop_ptr1, temp_gpr, 0 );
      libxsmm_x86_instruction_alu_imm( io_generated_code, i_micro_kernel_config->alu_add_instruction, temp_gpr, (idx_tsize == 8) ? m_unroll_factor * vlen * 8 : m_unroll_factor * vlen * 4);
      libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_argop_ptr1, temp_gpr, 1 );
    }
    if (apply_op == 1) {
      libxsmm_x86_instruction_alu_imm( io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_invec, m_unroll_factor * vlen * i_micro_kernel_config->datatype_size_out);
    }
    libxsmm_x86_instruction_alu_imm( io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_in_base, m_unroll_factor * vlen * i_micro_kernel_config->datatype_size_in);
    libxsmm_generator_mateltwise_footer_m_loop( io_generated_code, io_loop_label_tracker, i_micro_kernel_config, i_gp_reg_mapping->gp_reg_m_loop, m_trips_loop);
  }

  if (peeled_m_trips > 0) {
    if (m_trips_loop == 1) {
      libxsmm_x86_instruction_alu_imm( io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_out, m_unroll_factor * vlen * i_micro_kernel_config->datatype_size_out);
      if ( record_argop_off_vec0 == 1 ) {
        libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_argop_ptr0, temp_gpr, 0 );
        libxsmm_x86_instruction_alu_imm( io_generated_code, i_micro_kernel_config->alu_add_instruction, temp_gpr, (idx_tsize == 8) ? m_unroll_factor * vlen * 8 : m_unroll_factor * vlen * 4);
        libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_argop_ptr0, temp_gpr, 1 );
      }
      if ( record_argop_off_vec1== 1 ) {
        libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_argop_ptr1, temp_gpr, 0 );
        libxsmm_x86_instruction_alu_imm( io_generated_code, i_micro_kernel_config->alu_add_instruction, temp_gpr, (idx_tsize == 8) ? m_unroll_factor * vlen * 8 : m_unroll_factor * vlen * 4);
        libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_argop_ptr1, temp_gpr, 1 );
      }
      if (apply_op == 1) {
        libxsmm_x86_instruction_alu_imm( io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_invec, m_unroll_factor * vlen * i_micro_kernel_config->datatype_size_out);
      }
      libxsmm_x86_instruction_alu_imm( io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_in_base, m_unroll_factor * vlen * i_micro_kernel_config->datatype_size_in);
    }

    /* Perform the reductions for all columns */
    for (im = 0; im < peeled_m_trips; im++) {
      /* Load output for reduction  */
      if (apply_redop == 1) {
        if (load_acc == 0) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VPXORD,
                                                 i_micro_kernel_config->vector_name,
                                                 im + vecout_offset, im + vecout_offset, im + vecout_offset);
        } else {
          char vname = (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) ? 'y' : 'z';
          vname = (io_generated_code->arch < LIBXSMM_X86_AVX512) ? 'y' : vname;

          libxsmm_x86_instruction_unified_vec_move( io_generated_code,
              i_micro_kernel_config->vmove_instruction_in,
              i_gp_reg_mapping->gp_reg_out,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              im * vlen  * i_micro_kernel_config->datatype_size_out,
              vname,
              im + vecout_offset, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? 1 : 0, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? mask_inout : 0, 0 );

          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            /* convert 16 bit values into 32 bit (integer convert) */
            libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
                LIBXSMM_X86_INSTR_VPMOVSXWD,
                i_micro_kernel_config->vector_name,
                im + vecout_offset, im + vecout_offset );

            /* shift 16 bits to the left to generate valid FP32 numbers */
            libxsmm_x86_instruction_vec_compute_2reg_imm8(io_generated_code,
                LIBXSMM_X86_INSTR_VPSLLD_I,
                i_micro_kernel_config->vector_name,
                im + vecout_offset,
                im + vecout_offset,
                16);
          }
        }
      }

      /* Initialize argop vectors if need be  */
      if (record_argop_off_vec0 == 1) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                               LIBXSMM_X86_INSTR_VPXORD,
                                               i_micro_kernel_config->vector_name,
                                               im + vecargop_vec0_offset, im + vecargop_vec0_offset, im + vecargop_vec0_offset);
        if (idx_tsize == 8) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VPXORD,
                                                 i_micro_kernel_config->vector_name,
                                                 im + max_m_unrolling + vecargop_vec0_offset, im + max_m_unrolling + vecargop_vec0_offset, im + max_m_unrolling + vecargop_vec0_offset);
        }
      }

      /* Initialize argop vectors if need be  */
      if (record_argop_off_vec1 == 1) {
        libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                               LIBXSMM_X86_INSTR_VPXORD,
                                               i_micro_kernel_config->vector_name,
                                               im + vecargop_vec1_offset, im + vecargop_vec1_offset, im + vecargop_vec1_offset);
        if (idx_tsize == 8) {
          libxsmm_x86_instruction_vec_compute_3reg( io_generated_code,
                                                 LIBXSMM_X86_INSTR_VPXORD,
                                                 i_micro_kernel_config->vector_name,
                                                 im + max_m_unrolling + vecargop_vec1_offset, im + max_m_unrolling + vecargop_vec1_offset, im + max_m_unrolling + vecargop_vec1_offset);
        }
      }

      /* Load input vector in case we have to apply op */
      if (apply_op == 1) {
        if (use_indexed_vec == 0) {
          char vname = (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) ? 'y' : 'z';
          vname = (io_generated_code->arch < LIBXSMM_X86_AVX512) ? 'y' : vname;

          libxsmm_x86_instruction_unified_vec_move( io_generated_code,
              i_micro_kernel_config->vmove_instruction_in,
              i_gp_reg_mapping->gp_reg_invec,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              im * vlen  * i_micro_kernel_config->datatype_size_out,
              vname,
              im + vecin_offset, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? 1 : 0, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? mask_inout : 0, 0 );

          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            /* convert 16 bit values into 32 bit (integer convert) */
            libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
                LIBXSMM_X86_INSTR_VPMOVSXWD,
                i_micro_kernel_config->vector_name,
                im + vecin_offset, im + vecin_offset );

            /* shift 16 bits to the left to generate valid FP32 numbers */
            libxsmm_x86_instruction_vec_compute_2reg_imm8(io_generated_code,
                LIBXSMM_X86_INSTR_VPSLLD_I,
                i_micro_kernel_config->vector_name,
                im + vecin_offset,
                im + vecin_offset,
                16);
          }
        }
      }
    }

    if (pf_dist > 0) {
      libxsmm_x86_instruction_alu_imm(io_generated_code, i_micro_kernel_config->alu_mov_instruction, i_gp_reg_mapping->gp_reg_n_loop, 0);
      libxsmm_x86_instruction_alu_imm(io_generated_code, i_micro_kernel_config->alu_cmp_instruction, i_gp_reg_mapping->gp_reg_n, pf_dist);
      libxsmm_x86_instruction_jump_to_label(io_generated_code, LIBXSMM_X86_INSTR_JLE, NO_PF_LABEL_START_2, p_jump_label_tracker);

      libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_SUBQ, i_gp_reg_mapping->gp_reg_n, pf_dist);
      libxsmm_generator_mateltwise_header_n_dyn_loop(io_generated_code, io_loop_label_tracker, i_micro_kernel_config, i_gp_reg_mapping->gp_reg_n_loop, 1);

      if (use_indexed_vecidx > 0) {
        if (use_implicitly_indexed_vecidx == 0) {
          libxsmm_x86_instruction_alu_mem(io_generated_code, ind_alu_mov_instr, i_gp_reg_mapping->gp_reg_ind_base, i_gp_reg_mapping->gp_reg_n_loop, idx_tsize, 0, i_gp_reg_mapping->gp_reg_in, 0);
          libxsmm_x86_instruction_alu_mem(io_generated_code, ind_alu_mov_instr, i_gp_reg_mapping->gp_reg_ind_base, i_gp_reg_mapping->gp_reg_n_loop, idx_tsize, pf_dist * idx_tsize, i_gp_reg_mapping->gp_reg_in_pf, 0);
        } else {
          libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_mov_instruction, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_in);
          libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_mov_instruction, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_in_pf);
          libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_ADDQ, i_gp_reg_mapping->gp_reg_in_pf, pf_dist);
        }
        if (record_argop_off_vec0 == 1) {
          if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
            libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8(io_generated_code, gpr_bcast_idx_instr, vname_argop_bcast, i_gp_reg_mapping->gp_reg_in, LIBXSMM_X86_VEC_REG_UNDEF, temp_vreg_argop0, 0, 0, 0, 0);
          } else {
            libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_idx, i_gp_reg_mapping->gp_reg_in, 1 );
            libxsmm_x86_instruction_vec_move(io_generated_code, i_micro_kernel_config->instruction_set, bcast_idx_instr, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_idx, vname_argop_bcast, temp_vreg_argop0, 0, 0, 0);
          }
        }
        libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_IMUL, i_gp_reg_mapping->gp_reg_in, i_mateltwise_desc->ldi * in_tsize);
        libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_IMUL, i_gp_reg_mapping->gp_reg_in_pf, i_mateltwise_desc->ldi * in_tsize);
        libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_in_base, i_gp_reg_mapping->gp_reg_in);
        libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_in_base, i_gp_reg_mapping->gp_reg_in_pf);
      }

      if (use_indexed_vec > 0) {
        if (use_implicitly_indexed_vec == 0) {
          libxsmm_x86_instruction_alu_mem(io_generated_code, ind_alu_mov_instr, i_gp_reg_mapping->gp_reg_ind_base2, i_gp_reg_mapping->gp_reg_n_loop, idx_tsize, 0, i_gp_reg_mapping->gp_reg_in2, 0);
          libxsmm_x86_instruction_alu_mem(io_generated_code, ind_alu_mov_instr, i_gp_reg_mapping->gp_reg_ind_base2, i_gp_reg_mapping->gp_reg_n_loop, idx_tsize, pf_dist * idx_tsize, i_gp_reg_mapping->gp_reg_in_pf2, 0);
        } else {
          libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_mov_instruction, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_in2);
          libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_mov_instruction, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_in_pf2);
          libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_ADDQ, i_gp_reg_mapping->gp_reg_in_pf2, pf_dist);
        }
        if (record_argop_off_vec1 == 1) {
          if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
            libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8(io_generated_code, gpr_bcast_idx_instr, vname_argop_bcast, i_gp_reg_mapping->gp_reg_in2, LIBXSMM_X86_VEC_REG_UNDEF, temp_vreg_argop1, 0, 0, 0, 0);
          } else {
            libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_idx, i_gp_reg_mapping->gp_reg_in2, 1 );
            libxsmm_x86_instruction_vec_move(io_generated_code, i_micro_kernel_config->instruction_set, bcast_idx_instr, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_idx, vname_argop_bcast, temp_vreg_argop1, 0, 0, 0);
          }
        }
        libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_IMUL, i_gp_reg_mapping->gp_reg_in2, i_mateltwise_desc->ldi * in_tsize);
        libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_IMUL, i_gp_reg_mapping->gp_reg_in_pf2, i_mateltwise_desc->ldi * in_tsize);
        libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_in_base2, i_gp_reg_mapping->gp_reg_in2);
        libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_in_base2, i_gp_reg_mapping->gp_reg_in_pf2);
      }

      for (im = 0; im < peeled_m_trips; im++) {
        /* First load the indexed vector */
        char vname = (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) ? 'y' : 'z';
        vname = (io_generated_code->arch < LIBXSMM_X86_AVX512) ? 'y' : vname;

        libxsmm_x86_instruction_unified_vec_move( io_generated_code,
            i_micro_kernel_config->vmove_instruction_in,
            i_gp_reg_mapping->gp_reg_in,
            LIBXSMM_X86_GP_REG_UNDEF, 0,
            im * vlen  * i_micro_kernel_config->datatype_size_in,
            vname,
            im + vecidxin_offset, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? 1 : 0, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? mask_inout : 0, 0 );

        if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) {
          /* convert 16 bit values into 32 bit (integer convert) */
          libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
              LIBXSMM_X86_INSTR_VPMOVSXWD,
              i_micro_kernel_config->vector_name,
              im + vecidxin_offset, im + vecidxin_offset );

          /* shift 16 bits to the left to generate valid FP32 numbers */
          libxsmm_x86_instruction_vec_compute_2reg_imm8(io_generated_code,
              LIBXSMM_X86_INSTR_VPSLLD_I,
              i_micro_kernel_config->vector_name,
              im + vecidxin_offset,
              im + vecidxin_offset,
              16);
        }

        /* Now apply the OP among the indexed vector and the input vector */
        if (apply_op == 1) {
          if (use_indexed_vec > 0) {
            libxsmm_x86_instruction_unified_vec_move( io_generated_code,
                i_micro_kernel_config->vmove_instruction_in,
                i_gp_reg_mapping->gp_reg_in2,
                LIBXSMM_X86_GP_REG_UNDEF, 0,
                im * vlen  * i_micro_kernel_config->datatype_size_in,
                vname,
                im + vecin_offset, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? 1 : 0, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? mask_inout : 0, 0 );

            if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) {
              /* convert 16 bit values into 32 bit (integer convert) */
              libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
                  LIBXSMM_X86_INSTR_VPMOVSXWD,
                  i_micro_kernel_config->vector_name,
                  im + vecin_offset, im + vecin_offset );

              /* shift 16 bits to the left to generate valid FP32 numbers */
              libxsmm_x86_instruction_vec_compute_2reg_imm8(io_generated_code,
                  LIBXSMM_X86_INSTR_VPSLLD_I,
                  i_micro_kernel_config->vector_name,
                  im + vecin_offset,
                  im + vecin_offset,
                  16);
            }
          }
          if (op_instr == LIBXSMM_X86_INSTR_DOTPS) {
            /* TODO: Add DOT op sequence here  */
          } else {
            libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code,
                op_instr,
                i_micro_kernel_config->vector_name,
                (op_order == 0)    ? im + vecin_offset : im + vecidxin_offset,
                (op_order == 0)    ? im + vecidxin_offset : im + vecin_offset,
                (apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset,
                op_mask,
                op_mask_cntl,
                op_sae_cntl,
                op_imm);
          }
        }

        if (scale_op_result == 1) {
          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
            libxsmm_x86_instruction_vec_move(io_generated_code,
                i_micro_kernel_config->instruction_set,
                LIBXSMM_X86_INSTR_VPBROADCASTW,
                i_gp_reg_mapping->gp_reg_scale_base,
                i_gp_reg_mapping->gp_reg_n_loop,
                in_tsize,
                0, 'y',
                temp_vreg,
                0,
                0,
                0);

            libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
                LIBXSMM_X86_INSTR_VPMOVSXWD,
                i_micro_kernel_config->vector_name,
                temp_vreg, temp_vreg );

            libxsmm_x86_instruction_vec_compute_2reg_imm8( io_generated_code,
                LIBXSMM_X86_INSTR_VPSLLD_I,
                i_micro_kernel_config->vector_name,
                temp_vreg,
                temp_vreg,
                16);

            libxsmm_x86_instruction_vec_compute_3reg(io_generated_code,
                LIBXSMM_X86_INSTR_VMULPS,
                i_micro_kernel_config->vector_name,
                temp_vreg,
                (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset,
                (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset);
          } else {
            if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
              libxsmm_x86_instruction_vec_compute_mem_2reg_mask_imm8( io_generated_code,
                  LIBXSMM_X86_INSTR_VMULPS,
                  i_micro_kernel_config->vector_name,
                  i_gp_reg_mapping->gp_reg_scale_base,
                  i_gp_reg_mapping->gp_reg_n_loop,
                  4, 0, 1,
                  (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset,
                  (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset,
                  0, 0, 0);
            } else {
              libxsmm_x86_instruction_vec_move(io_generated_code,
                  i_micro_kernel_config->instruction_set,
                  LIBXSMM_X86_INSTR_VBROADCASTSS,
                  i_gp_reg_mapping->gp_reg_scale_base,
                  i_gp_reg_mapping->gp_reg_n_loop,
                  in_tsize,
                  0, 'y',
                  temp_vreg,
                  0,
                  0,
                  0);

              libxsmm_x86_instruction_vec_compute_3reg(io_generated_code,
                  LIBXSMM_X86_INSTR_VMULPS,
                  i_micro_kernel_config->vector_name,
                  temp_vreg,
                  (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset,
                  (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset);
            }
          }
        }

        /* Now apply the Reduce OP */
        if (apply_redop == 1) {
          if ((record_argop_off_vec0 == 1) ||(record_argop_off_vec1 == 1)) {
            libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code, argop_cmp_instr, i_micro_kernel_config->vector_name, im + vecidxin_offset, im + vecout_offset, argop_mask, argop_cmp_imm );
            if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
              if (record_argop_off_vec0 == 1) {
                libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + vecargop_vec0_offset, temp_vreg_argop0, im + vecargop_vec0_offset, argop_mask, 0 );
              }
              if (record_argop_off_vec1 == 1) {
                libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + vecargop_vec1_offset, temp_vreg_argop1, im + vecargop_vec1_offset, argop_mask, 0 );
              }
              if (idx_tsize == 8) {
                libxsmm_x86_instruction_mask_compute_reg( io_generated_code,LIBXSMM_X86_INSTR_KSHIFTRW, argop_mask, LIBXSMM_X86_VEC_REG_UNDEF, argop_mask, 8);
                if (record_argop_off_vec0 == 1) {
                  libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + max_m_unrolling + vecargop_vec0_offset, temp_vreg_argop0, im + max_m_unrolling + vecargop_vec0_offset, argop_mask, 0 );
                }
                if (record_argop_off_vec1 == 1) {
                  libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + max_m_unrolling + vecargop_vec1_offset, temp_vreg_argop1, im + max_m_unrolling + vecargop_vec1_offset, argop_mask, 0 );
                }
              }
            } else {
              if (idx_tsize == 8) {
                libxsmm_generator_avx_extract_mask4_from_mask8( io_generated_code, argop_mask, argop_mask_aux, 1);
                libxsmm_generator_avx_extract_mask4_from_mask8( io_generated_code, argop_mask, argop_mask, 0);
              }
              if (record_argop_off_vec0 == 1) {
                libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + vecargop_vec0_offset, temp_vreg_argop0, im + vecargop_vec0_offset, 0, 0, 0, argop_mask << 4);
              }
              if (record_argop_off_vec1 == 1) {
                libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + vecargop_vec1_offset, temp_vreg_argop1, im + vecargop_vec1_offset, 0, 0, 0, argop_mask << 4);
              }
              if (idx_tsize == 8) {
                if (record_argop_off_vec0 == 1) {
                  libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + max_m_unrolling + vecargop_vec0_offset, temp_vreg_argop0, im + max_m_unrolling + vecargop_vec0_offset, 0, 0, 0, argop_mask_aux << 4);
                }
                if (record_argop_off_vec1 == 1) {
                  libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + max_m_unrolling + vecargop_vec1_offset, temp_vreg_argop1, im + max_m_unrolling + vecargop_vec1_offset, 0, 0, 0, argop_mask_aux << 4);
                }
              }
            }
          }

          libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code,
              reduceop_instr,
              i_micro_kernel_config->vector_name,
              im + vecidxin_offset,
              im + vecout_offset,
              im + vecout_offset,
              reduceop_mask,
              reduceop_mask_cntl,
              reduceop_sae_cntl,
              reduceop_imm);
        }

        if ((im * vlen * i_micro_kernel_config->datatype_size_in) % 64 == 0 ) {
          libxsmm_x86_instruction_prefetch(io_generated_code,
              pf_instr,
              i_gp_reg_mapping->gp_reg_in_pf,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              im * vlen * i_micro_kernel_config->datatype_size_in);
          if (use_indexed_vec > 0){
            libxsmm_x86_instruction_prefetch(io_generated_code,
                pf_instr,
                i_gp_reg_mapping->gp_reg_in_pf2,
                LIBXSMM_X86_GP_REG_UNDEF, 0,
                im * vlen * i_micro_kernel_config->datatype_size_in);
          }
        }
      }

      libxsmm_generator_mateltwise_footer_n_dyn_loop(io_generated_code, io_loop_label_tracker, i_micro_kernel_config, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_n);
      libxsmm_x86_instruction_alu_imm(io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_n, pf_dist);
      /* NO_PF_LABEL_START_2 */
      libxsmm_x86_instruction_register_jump_label(io_generated_code, NO_PF_LABEL_START_2, p_jump_label_tracker);
    }

    libxsmm_generator_mateltwise_header_n_dyn_loop(io_generated_code, io_loop_label_tracker, i_micro_kernel_config, i_gp_reg_mapping->gp_reg_n_loop, (pf_dist > 0) ? 1 : 0);

    if (use_indexed_vecidx > 0) {
      if (use_implicitly_indexed_vecidx == 0) {
        libxsmm_x86_instruction_alu_mem(io_generated_code, ind_alu_mov_instr, i_gp_reg_mapping->gp_reg_ind_base, i_gp_reg_mapping->gp_reg_n_loop, idx_tsize, 0, i_gp_reg_mapping->gp_reg_in, 0);
      } else {
        libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_mov_instruction, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_in);
      }
      if (record_argop_off_vec0 == 1) {
        if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
          libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8(io_generated_code, gpr_bcast_idx_instr, vname_argop_bcast, i_gp_reg_mapping->gp_reg_in, LIBXSMM_X86_VEC_REG_UNDEF, temp_vreg_argop0, 0, 0, 0, 0);
        } else {
          libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_idx, i_gp_reg_mapping->gp_reg_in, 1 );
          libxsmm_x86_instruction_vec_move(io_generated_code, i_micro_kernel_config->instruction_set, bcast_idx_instr, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_idx, vname_argop_bcast, temp_vreg_argop0, 0, 0, 0);
        }
      }
      libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_IMUL, i_gp_reg_mapping->gp_reg_in, i_mateltwise_desc->ldi * in_tsize);
      libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_in_base, i_gp_reg_mapping->gp_reg_in);
    }

    if (use_indexed_vec > 0) {
      if (use_implicitly_indexed_vec == 0) {
        libxsmm_x86_instruction_alu_mem(io_generated_code, ind_alu_mov_instr, i_gp_reg_mapping->gp_reg_ind_base2, i_gp_reg_mapping->gp_reg_n_loop, idx_tsize, 0, i_gp_reg_mapping->gp_reg_in2, 0);
      } else {
        libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_mov_instruction, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_in2);
      }
      if (record_argop_off_vec1 == 1) {
        if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
          libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8(io_generated_code, gpr_bcast_idx_instr, vname_argop_bcast, i_gp_reg_mapping->gp_reg_in2, LIBXSMM_X86_VEC_REG_UNDEF, temp_vreg_argop1, 0, 0, 0, 0);
        } else {
          libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_idx, i_gp_reg_mapping->gp_reg_in2, 1 );
          libxsmm_x86_instruction_vec_move(io_generated_code, i_micro_kernel_config->instruction_set, bcast_idx_instr, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_idx, vname_argop_bcast, temp_vreg_argop1, 0, 0, 0);
        }
      }
      libxsmm_x86_instruction_alu_imm(io_generated_code, LIBXSMM_X86_INSTR_IMUL, i_gp_reg_mapping->gp_reg_in2, i_mateltwise_desc->ldi * in_tsize);
      libxsmm_x86_instruction_alu_reg(io_generated_code, i_micro_kernel_config->alu_add_instruction, i_gp_reg_mapping->gp_reg_in_base2, i_gp_reg_mapping->gp_reg_in2);
    }

    for (im = 0; im < peeled_m_trips; im++) {
      /* First load the indexed vector */
      char vname = (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) ? 'y' : 'z';
      vname = (io_generated_code->arch < LIBXSMM_X86_AVX512) ? 'y' : vname;

      libxsmm_x86_instruction_unified_vec_move( io_generated_code,
          i_micro_kernel_config->vmove_instruction_in,
          i_gp_reg_mapping->gp_reg_in,
          LIBXSMM_X86_GP_REG_UNDEF, 0,
          im * vlen  * i_micro_kernel_config->datatype_size_in,
          vname,
          im + vecidxin_offset, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? 1 : 0, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? mask_inout : 0, 0 );

      if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) {
        /* convert 16 bit values into 32 bit (integer convert) */
        libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
            LIBXSMM_X86_INSTR_VPMOVSXWD,
            i_micro_kernel_config->vector_name,
            im + vecidxin_offset, im + vecidxin_offset );

        /* shift 16 bits to the left to generate valid FP32 numbers */
        libxsmm_x86_instruction_vec_compute_2reg_imm8(io_generated_code,
            LIBXSMM_X86_INSTR_VPSLLD_I,
            i_micro_kernel_config->vector_name,
            im + vecidxin_offset,
            im + vecidxin_offset,
            16);
      }

      /* Now apply the OP among the indexed vector and the input vector */
      if (apply_op == 1) {
        if (use_indexed_vec > 0) {
          libxsmm_x86_instruction_unified_vec_move( io_generated_code,
              i_micro_kernel_config->vmove_instruction_in,
              i_gp_reg_mapping->gp_reg_in2,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              im * vlen  * i_micro_kernel_config->datatype_size_in,
              vname,
              im + vecin_offset, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? 1 : 0, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? mask_inout : 0, 0 );

          if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_INP( i_mateltwise_desc->datatype )) {
            /* convert 16 bit values into 32 bit (integer convert) */
            libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
                LIBXSMM_X86_INSTR_VPMOVSXWD,
                i_micro_kernel_config->vector_name,
                im + vecin_offset, im + vecin_offset );

            /* shift 16 bits to the left to generate valid FP32 numbers */
            libxsmm_x86_instruction_vec_compute_2reg_imm8(io_generated_code,
                LIBXSMM_X86_INSTR_VPSLLD_I,
                i_micro_kernel_config->vector_name,
                im + vecin_offset,
                im + vecin_offset,
                16);
          }
        }
        if (op_instr == LIBXSMM_X86_INSTR_DOTPS) {
          /* TODO: Add DOT op sequence here  */
        } else {
          libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code,
              op_instr,
              i_micro_kernel_config->vector_name,
              (op_order == 0)    ? im + vecin_offset : im + vecidxin_offset,
              (op_order == 0)    ? im + vecidxin_offset : im + vecin_offset,
              (apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset,
              op_mask,
              op_mask_cntl,
              op_sae_cntl,
              op_imm);
        }
      }

      if (scale_op_result == 1) {
        if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
          libxsmm_x86_instruction_vec_move(io_generated_code,
              i_micro_kernel_config->instruction_set,
              LIBXSMM_X86_INSTR_VPBROADCASTW,
              i_gp_reg_mapping->gp_reg_scale_base,
              i_gp_reg_mapping->gp_reg_n_loop,
              in_tsize,
              0, 'y',
              temp_vreg,
              0,
              0,
              0);

          libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
              LIBXSMM_X86_INSTR_VPMOVSXWD,
              i_micro_kernel_config->vector_name,
              temp_vreg, temp_vreg );

          libxsmm_x86_instruction_vec_compute_2reg_imm8(io_generated_code,
              LIBXSMM_X86_INSTR_VPSLLD_I,
              i_micro_kernel_config->vector_name,
              temp_vreg,
              temp_vreg,
              16);

          libxsmm_x86_instruction_vec_compute_3reg(io_generated_code,
              LIBXSMM_X86_INSTR_VMULPS,
              i_micro_kernel_config->vector_name,
              temp_vreg,
              (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset,
              (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset);
        } else {
          if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
            libxsmm_x86_instruction_vec_compute_mem_2reg_mask_imm8( io_generated_code,
                LIBXSMM_X86_INSTR_VMULPS,
                i_micro_kernel_config->vector_name,
                i_gp_reg_mapping->gp_reg_scale_base,
                i_gp_reg_mapping->gp_reg_n_loop,
                4, 0, 1,
                (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset,
                (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset,
                0, 0, 0);
          } else {
            libxsmm_x86_instruction_vec_move(io_generated_code,
                i_micro_kernel_config->instruction_set,
                LIBXSMM_X86_INSTR_VBROADCASTSS,
                i_gp_reg_mapping->gp_reg_scale_base,
                i_gp_reg_mapping->gp_reg_n_loop,
                in_tsize,
                0, 'y',
                temp_vreg,
                0,
                0,
                0);

            libxsmm_x86_instruction_vec_compute_3reg(io_generated_code,
                LIBXSMM_X86_INSTR_VMULPS,
                i_micro_kernel_config->vector_name,
                temp_vreg,
                (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset,
                (apply_op == 1) ? ((apply_redop == 1) ? im + vecidxin_offset : im + vecin_offset) : im + vecidxin_offset);
          }
        }
      }

      /* Now apply the Reduce OP */
      if (apply_redop == 1) {
        if ((record_argop_off_vec0 == 1) ||(record_argop_off_vec1 == 1)) {
          libxsmm_x86_instruction_vec_compute_3reg_imm8( io_generated_code, argop_cmp_instr, i_micro_kernel_config->vector_name, im + vecidxin_offset, im + vecout_offset, argop_mask, argop_cmp_imm );
          if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
            if (record_argop_off_vec0 == 1) {
              libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + vecargop_vec0_offset, temp_vreg_argop0, im + vecargop_vec0_offset, argop_mask, 0 );
            }
            if (record_argop_off_vec1 == 1) {
              libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + vecargop_vec1_offset, temp_vreg_argop1, im + vecargop_vec1_offset, argop_mask, 0 );
            }
            if (idx_tsize == 8) {
              libxsmm_x86_instruction_mask_compute_reg( io_generated_code,LIBXSMM_X86_INSTR_KSHIFTRW, argop_mask, LIBXSMM_X86_VEC_REG_UNDEF, argop_mask, 8);
              if (record_argop_off_vec0 == 1) {
                libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + max_m_unrolling + vecargop_vec0_offset, temp_vreg_argop0, im + max_m_unrolling + vecargop_vec0_offset, argop_mask, 0 );
              }
              if (record_argop_off_vec1 == 1) {
                libxsmm_x86_instruction_vec_compute_3reg_mask( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + max_m_unrolling + vecargop_vec1_offset, temp_vreg_argop1, im + max_m_unrolling + vecargop_vec1_offset, argop_mask, 0 );
              }
            }
          } else {
            if (idx_tsize == 8) {
              libxsmm_generator_avx_extract_mask4_from_mask8( io_generated_code, argop_mask, argop_mask_aux, 1);
              libxsmm_generator_avx_extract_mask4_from_mask8( io_generated_code, argop_mask, argop_mask, 0);
            }
            if (record_argop_off_vec0 == 1) {
              libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + vecargop_vec0_offset, temp_vreg_argop0, im + vecargop_vec0_offset, 0, 0, 0, argop_mask << 4);
            }
            if (record_argop_off_vec1 == 1) {
              libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + vecargop_vec1_offset, temp_vreg_argop1, im + vecargop_vec1_offset, 0, 0, 0, argop_mask << 4);
            }
            if (idx_tsize == 8) {
              if (record_argop_off_vec0 == 1) {
                libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + max_m_unrolling + vecargop_vec0_offset, temp_vreg_argop0, im + max_m_unrolling + vecargop_vec0_offset, 0, 0, 0, argop_mask_aux << 4);
              }
              if (record_argop_off_vec1 == 1) {
                libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code, argop_blend_instr, i_micro_kernel_config->vector_name, im + max_m_unrolling + vecargop_vec1_offset, temp_vreg_argop1, im + max_m_unrolling + vecargop_vec1_offset, 0, 0, 0, argop_mask_aux << 4);
              }
            }
          }
        }

        libxsmm_x86_instruction_vec_compute_3reg_mask_sae_imm8( io_generated_code,
            reduceop_instr,
            i_micro_kernel_config->vector_name,
            im + vecidxin_offset,
            im + vecout_offset,
            im + vecout_offset,
            reduceop_mask,
            reduceop_mask_cntl,
            reduceop_sae_cntl,
            reduceop_imm);
      }
    }

    libxsmm_generator_mateltwise_footer_n_dyn_loop(io_generated_code, io_loop_label_tracker, i_micro_kernel_config, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_n);

    /* Now store accumulators  */
    for (im = 0; im < peeled_m_trips; im++) {
      char vname = (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) ? 'y' : 'z';
      vname = (io_generated_code->arch < LIBXSMM_X86_AVX512) ? 'y' : vname;

      if (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) {
        if (io_generated_code->arch >= LIBXSMM_X86_AVX512_CPX) {
          libxsmm_x86_instruction_vec_compute_2reg( io_generated_code,
              LIBXSMM_X86_INSTR_VCVTNEPS2BF16,
              i_micro_kernel_config->vector_name,
              im + vecout_offset, im + vecout_offset );
        } else {
          libxsmm_generator_vcvtneps2bf16_avx512_preppedstack( io_generated_code, 'z', im + vecout_offset, im + vecout_offset, 30, 31, 6, 7 );
        }
      }
      libxsmm_x86_instruction_unified_vec_move( io_generated_code,
          ((im == peeled_m_trips-1) && (use_m_masking == 1)) ? i_micro_kernel_config->vmove_instruction_out : vstore_instr,
          i_gp_reg_mapping->gp_reg_out,
          LIBXSMM_X86_GP_REG_UNDEF, 0,
          im * vlen  * i_micro_kernel_config->datatype_size_out,
          vname,
          im + vecout_offset, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? 1 : 0, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? mask_inout : 0, 1 );
    }

    if ( record_argop_off_vec0 == 1 ) {
      char vname = (io_generated_code->arch < LIBXSMM_X86_AVX512) ? 'y' : 'z';
      libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_argop_ptr0, temp_gpr, 0 );
      if (idx_tsize == 4) {
        for (im = 0; im < peeled_m_trips; im++) {
          unsigned int vreg_id = im + vecargop_vec0_offset;
          libxsmm_x86_instruction_unified_vec_move( io_generated_code,
              LIBXSMM_X86_INSTR_VMOVUPS,
              temp_gpr,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              im * vlen * 4,
              vname,
              vreg_id, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? 1 : 0, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? mask_inout : 0, 1 );
        }
      } else {
        unsigned int use_idx_masking = 0;
        unsigned int idx_peeled_m_trips = 0;
        if (m % vlen != 0) {
          idx_peeled_m_trips = (peeled_m_trips-1) * vlen + m % vlen;
        } else {
          idx_peeled_m_trips = peeled_m_trips * vlen;
        }
        if (idx_peeled_m_trips % (vlen/2) != 0) {
          use_idx_masking = 1;
        }
        idx_peeled_m_trips = (idx_peeled_m_trips + (vlen/2)-1)/(vlen/2);
        for (im = 0; im < idx_peeled_m_trips; im++) {
          unsigned int vreg_id = (im % 2 == 0) ? im/2 + vecargop_vec0_offset : im/2 + max_m_unrolling + vecargop_vec0_offset;
          libxsmm_x86_instruction_unified_vec_move( io_generated_code,
              LIBXSMM_X86_INSTR_VMOVUPD,
              temp_gpr,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              im * vlen * 4,
              vname,
              vreg_id, ((im == idx_peeled_m_trips-1) && (use_idx_masking > 0)) ? 1 : 0, ((im == idx_peeled_m_trips-1) && (use_idx_masking > 0)) ? mask_argidx64 : 0, 1 );
        }
      }
    }

    if ( record_argop_off_vec1 == 1 ) {
      char vname = (io_generated_code->arch < LIBXSMM_X86_AVX512) ? 'y' : 'z';
      libxsmm_x86_instruction_alu_mem( io_generated_code, LIBXSMM_X86_INSTR_MOVQ, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_UNDEF, 0, rbp_offset_argop_ptr1, temp_gpr, 0 );
      if (idx_tsize == 4) {
        for (im = 0; im < peeled_m_trips; im++) {
          unsigned int vreg_id = im + vecargop_vec1_offset;
          libxsmm_x86_instruction_unified_vec_move( io_generated_code,
              LIBXSMM_X86_INSTR_VMOVUPS,
              temp_gpr,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              im * vlen * 4,
              vname,
              vreg_id, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? 1 : 0, ((im == peeled_m_trips-1) && (use_m_masking > 0)) ? mask_inout : 0, 1 );
        }
      } else {
        unsigned int use_idx_masking = 0;
        unsigned int idx_peeled_m_trips = 0;
        if (m % vlen != 0) {
          idx_peeled_m_trips = (peeled_m_trips-1) * vlen + m % vlen;
        } else {
          idx_peeled_m_trips = peeled_m_trips * vlen;
        }
        if (idx_peeled_m_trips % (vlen/2) != 0) {
          use_idx_masking = 1;
        }
        idx_peeled_m_trips = (idx_peeled_m_trips + (vlen/2)-1)/(vlen/2);
        for (im = 0; im < idx_peeled_m_trips; im++) {
          unsigned int vreg_id = (im % 2 == 0) ? im/2 + vecargop_vec1_offset : im/2 + max_m_unrolling + vecargop_vec1_offset;
          libxsmm_x86_instruction_unified_vec_move( io_generated_code,
              LIBXSMM_X86_INSTR_VMOVUPD,
              temp_gpr,
              LIBXSMM_X86_GP_REG_UNDEF, 0,
              im * vlen * 4,
              vname,
              vreg_id, ((im == idx_peeled_m_trips-1) && (use_idx_masking > 0)) ? 1 : 0, ((im == idx_peeled_m_trips-1) && (use_idx_masking > 0)) ? mask_argidx64 : 0, 1 );
        }
      }
    }
  }

  if ((io_generated_code->arch < LIBXSMM_X86_AVX512_CPX) && (LIBXSMM_DATATYPE_BF16 == LIBXSMM_GETENUM_OUT( i_mateltwise_desc->datatype )) ) {
    libxsmm_x86_instruction_pop_reg( io_generated_code, LIBXSMM_X86_GP_REG_R8 );
    libxsmm_x86_instruction_pop_reg( io_generated_code, LIBXSMM_X86_GP_REG_R8 );
    libxsmm_x86_instruction_pop_reg( io_generated_code, LIBXSMM_X86_GP_REG_R8 );
    libxsmm_x86_instruction_pop_reg( io_generated_code, LIBXSMM_X86_GP_REG_R8 );
    libxsmm_x86_instruction_pop_reg( io_generated_code, LIBXSMM_X86_GP_REG_R8 );
  }

  if ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_OPREDUCE_VECS_OP_COPY) == 0) {
    if (use_indexed_vec > 0) {
      if (use_implicitly_indexed_vec == 0) {
        libxsmm_x86_instruction_pop_reg( io_generated_code, i_gp_reg_mapping->gp_reg_ind_base2 );
      }
      if (pf_dist > 0) {
        libxsmm_x86_instruction_pop_reg( io_generated_code, i_gp_reg_mapping->gp_reg_in_pf2 );
      }
      libxsmm_x86_instruction_pop_reg( io_generated_code, i_gp_reg_mapping->gp_reg_in2 );
    }
  }

  if (pushpop_scaleop_reg == 1) {
    libxsmm_x86_instruction_pop_reg( io_generated_code, i_gp_reg_mapping->gp_reg_scale_base );
  }

  if (use_stack_vars > 0) {
    libxsmm_x86_instruction_pop_reg( io_generated_code, LIBXSMM_X86_GP_REG_RBX );
    libxsmm_x86_instruction_alu_reg( io_generated_code, i_micro_kernel_config->alu_mov_instruction, LIBXSMM_X86_GP_REG_RBP, LIBXSMM_X86_GP_REG_RSP);
    libxsmm_x86_instruction_pop_reg( io_generated_code, LIBXSMM_X86_GP_REG_RBP );
  }

  libxsmm_x86_instruction_register_jump_label(io_generated_code, END_LABEL, p_jump_label_tracker);

#if defined(LIBXSMM_GENERATOR_MATELTWISE_REDUCE_AVX_AVX512_JUMP_LABEL_TRACKER_MALLOC)
  free(p_jump_label_tracker);
#endif
}
#endif

LIBXSMM_API_INTERN
void libxsmm_generator_reduce_cols_index_aarch64_microkernel( libxsmm_generated_code*                        io_generated_code,
    libxsmm_loop_label_tracker*                    io_loop_label_tracker,
    libxsmm_mateltwise_gp_reg_mapping*             i_gp_reg_mapping,
    const libxsmm_mateltwise_kernel_config*        i_micro_kernel_config,
    const libxsmm_meltw_descriptor*                i_mateltwise_desc ) {

  unsigned int m, im, use_m_masking, m_trips, max_m_unrolling = 4, m_unroll_factor = 1, m_trips_loop = 0, peeled_m_trips = 0, mask_out_count = 0;
  unsigned int aux_vreg_offset = 16;
  unsigned int gp_reg_index_64bit = 0, gp_reg_index = 0, gp_reg_ldi = 0;
  unsigned int idx_tsize = ((i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_UNARY_IDX_SIZE_8BYTES) > 0 ) ? 8 : 4;
  unsigned int load_acc = 1;
  unsigned int END_LABEL = 1;
  unsigned int vlen = 4;
  unsigned int mask_reg = 0;
  const char *const env_max_m_unroll = getenv("MAX_M_UNROLL_REDUCE_COLS_IDX");
  const char *const env_load_acc= getenv("LOAD_ACCS_REDUCE_COLS_IDX");
#if defined(LIBXSMM_GENERATOR_MATELTWISE_REDUCE_AARCH64_JUMP_LABEL_TRACKER_MALLOC)
  libxsmm_jump_label_tracker* const p_jump_label_tracker = (libxsmm_jump_label_tracker*)malloc(sizeof(libxsmm_jump_label_tracker));
#else
  libxsmm_jump_label_tracker l_jump_label_tracker;
  libxsmm_jump_label_tracker* const p_jump_label_tracker = &l_jump_label_tracker;
#endif
  libxsmm_reset_jump_label_tracker(p_jump_label_tracker);

  LIBXSMM_UNUSED( mask_reg );
  LIBXSMM_UNUSED( mask_out_count );
  LIBXSMM_UNUSED( END_LABEL );

  if ( 0 == env_max_m_unroll ) {
  } else {
    max_m_unrolling = atoi(env_max_m_unroll);
  }
  if ( 0 == env_load_acc ) {
  } else {
    load_acc = atoi(env_load_acc);
  }
  if ( (i_mateltwise_desc->flags & LIBXSMM_MELTW_FLAG_UNARY_REDUCE_XOR_ACC) > 0 ) {
    load_acc = 0;
  }

  i_gp_reg_mapping->gp_reg_n        = LIBXSMM_AARCH64_GP_REG_X9;
  i_gp_reg_mapping->gp_reg_ind_base = LIBXSMM_AARCH64_GP_REG_X10;
  i_gp_reg_mapping->gp_reg_in_base  = LIBXSMM_AARCH64_GP_REG_X11;
  i_gp_reg_mapping->gp_reg_out      = LIBXSMM_AARCH64_GP_REG_X12;
  i_gp_reg_mapping->gp_reg_m_loop   = LIBXSMM_AARCH64_GP_REG_X13;
  i_gp_reg_mapping->gp_reg_n_loop   = LIBXSMM_AARCH64_GP_REG_X14;
  i_gp_reg_mapping->gp_reg_in       = LIBXSMM_AARCH64_GP_REG_X15;
  gp_reg_index                        = ( idx_tsize == 8 ) ? LIBXSMM_AARCH64_GP_REG_X16 : LIBXSMM_AARCH64_GP_REG_W16;
  gp_reg_index_64bit                  = LIBXSMM_AARCH64_GP_REG_X16;
  i_gp_reg_mapping->gp_reg_scratch_0  = LIBXSMM_AARCH64_GP_REG_X17;
  gp_reg_ldi                          = LIBXSMM_AARCH64_GP_REG_X1;

  libxsmm_aarch64_instruction_alu_move( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_LDR_I_OFF, i_gp_reg_mapping->gp_reg_param_struct, LIBXSMM_AARCH64_GP_REG_UNDEF, 48, i_gp_reg_mapping->gp_reg_n );

  libxsmm_aarch64_instruction_alu_move( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_LDR_I_OFF, i_gp_reg_mapping->gp_reg_n, LIBXSMM_AARCH64_GP_REG_UNDEF, 0, i_gp_reg_mapping->gp_reg_n );

#if 0
  libxsmm_x86_instruction_alu_imm(io_generated_code, i_micro_kernel_config->alu_cmp_instruction, i_gp_reg_mapping->gp_reg_n, 0);
  libxsmm_x86_instruction_jump_to_label(io_generated_code, LIBXSMM_X86_INSTR_JLE, END_LABEL, p_jump_label_tracker);
#endif

  libxsmm_aarch64_instruction_alu_move( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_LDR_I_OFF, i_gp_reg_mapping->gp_reg_param_struct, LIBXSMM_AARCH64_GP_REG_UNDEF, 40, i_gp_reg_mapping->gp_reg_ind_base );
  libxsmm_aarch64_instruction_alu_move( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_LDR_I_OFF, i_gp_reg_mapping->gp_reg_param_struct, LIBXSMM_AARCH64_GP_REG_UNDEF, 32, i_gp_reg_mapping->gp_reg_in_base );
  libxsmm_aarch64_instruction_alu_move( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_LDR_I_OFF, i_gp_reg_mapping->gp_reg_param_struct, LIBXSMM_AARCH64_GP_REG_UNDEF, 56, i_gp_reg_mapping->gp_reg_out );
  libxsmm_aarch64_instruction_alu_compute_shifted_reg( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_EOR_SR, gp_reg_index_64bit, gp_reg_index_64bit, gp_reg_index_64bit, 0, LIBXSMM_AARCH64_SHIFTMODE_LSL );
  libxsmm_aarch64_instruction_alu_set_imm64( io_generated_code, gp_reg_ldi, (unsigned long long)i_mateltwise_desc->ldi * (unsigned long long)i_micro_kernel_config->datatype_size_in );

  m                 = i_mateltwise_desc->m;
  use_m_masking     = ( m % vlen == 0 ) ? 0 : 1;
  m_trips           = (m + vlen - 1) / vlen;
  m_unroll_factor   = m_trips;
  m_trips_loop      = 1;
  peeled_m_trips    = 0;

  if (use_m_masking == 1) {
    printf("Support for m %% 4 == 0 cases\n");
    LIBXSMM_HANDLE_ERROR( io_generated_code, LIBXSMM_ERR_GENERAL );
    return;
#if 0
    if (io_generated_code->arch >= LIBXSMM_X86_AVX512) {
      /* Calculate mask reg 1 for reading/output-writing */
      mask_out_count = vlen - (m % vlen);
      mask_reg = 1;
      libxsmm_generator_mateltwise_initialize_avx512_mask(io_generated_code, LIBXSMM_X86_GP_REG_RAX, mask_reg, mask_out_count, LIBXSMM_GEMM_PRECISION_F32);
    } else {
      mask_reg = 15;
      aux_vreg_offset = 7;
      if (max_m_unrolling > 7) {
        max_m_unrolling = 7;
      }
      libxsmm_generator_mateltwise_initialize_avx_mask(io_generated_code, mask_reg, m % vlen);
    }
#endif
  }

  /* In this case we have to generate a loop for m */
  if (m_unroll_factor > max_m_unrolling) {
    m_unroll_factor = max_m_unrolling;
    m_trips_loop = m_trips/m_unroll_factor;
    peeled_m_trips = m_trips  - m_unroll_factor * m_trips_loop;
    if ((use_m_masking > 0) && (peeled_m_trips == 0)) {
      m_trips_loop--;
      peeled_m_trips = m_trips  - m_unroll_factor * m_trips_loop;
    }
  } else {
    if ((use_m_masking > 0) && (peeled_m_trips == 0)) {
      m_trips_loop--;
      peeled_m_trips = m_trips  - m_unroll_factor * m_trips_loop;
    }
  }

  if (m_trips_loop > 1) {
    libxsmm_generator_loop_header_aarch64( io_generated_code, io_loop_label_tracker, i_gp_reg_mapping->gp_reg_m_loop, m_trips_loop );
  }

  if (m_trips_loop >= 1) {
    for (im = 0; im < m_unroll_factor; im++) {
      if (load_acc == 0) {
        libxsmm_aarch64_instruction_asimd_compute( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_EOR_V, im, im, 0, im, LIBXSMM_AARCH64_ASIMD_TUPLETYPE_16B );
      } else {
        libxsmm_aarch64_instruction_asimd_move( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_LDR_I_POST, i_gp_reg_mapping->gp_reg_out, LIBXSMM_AARCH64_GP_REG_UNDEF, 16, im, LIBXSMM_AARCH64_ASIMD_WIDTH_Q );
      }
    }
    if (load_acc == 1) {
      libxsmm_aarch64_instruction_alu_compute_imm64( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_META_SUB, i_gp_reg_mapping->gp_reg_out, i_gp_reg_mapping->gp_reg_scratch_0, i_gp_reg_mapping->gp_reg_out, (unsigned long long)m_unroll_factor * (unsigned long long)16 );
    }

    /* Perform the reductions for all columns */
    libxsmm_generator_loop_header_gp_reg_bound_aarch64( io_generated_code, io_loop_label_tracker, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_n );
    libxsmm_aarch64_instruction_alu_move( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_LDR_I_POST, i_gp_reg_mapping->gp_reg_ind_base, LIBXSMM_AARCH64_GP_REG_UNDEF, idx_tsize, gp_reg_index );
    libxsmm_aarch64_instruction_alu_compute_shifted_reg( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_MUL, gp_reg_index_64bit, gp_reg_ldi, i_gp_reg_mapping->gp_reg_in, 0, LIBXSMM_AARCH64_SHIFTMODE_LSL );
    libxsmm_aarch64_instruction_alu_compute_shifted_reg( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_ADD_SR, i_gp_reg_mapping->gp_reg_in, i_gp_reg_mapping->gp_reg_in_base, i_gp_reg_mapping->gp_reg_in, 0, LIBXSMM_AARCH64_SHIFTMODE_LSL );

    for (im = 0; im < m_unroll_factor; im++) {
      libxsmm_aarch64_instruction_asimd_move( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_LDR_I_POST, i_gp_reg_mapping->gp_reg_in, LIBXSMM_AARCH64_GP_REG_UNDEF, 16, aux_vreg_offset+im, LIBXSMM_AARCH64_ASIMD_WIDTH_Q );
      libxsmm_aarch64_instruction_asimd_compute( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_FADD_V, aux_vreg_offset+im, im, 0, im, LIBXSMM_AARCH64_ASIMD_TUPLETYPE_4S);
    }

    libxsmm_generator_loop_footer_aarch64(io_generated_code, io_loop_label_tracker, i_gp_reg_mapping->gp_reg_n_loop, 1);

    /* Now store accumulators  */
    for (im = 0; im < m_unroll_factor; im++) {
      libxsmm_aarch64_instruction_asimd_move( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_STR_I_POST, i_gp_reg_mapping->gp_reg_out, LIBXSMM_AARCH64_GP_REG_UNDEF, 16, im, LIBXSMM_AARCH64_ASIMD_WIDTH_Q );
    }
  }

  if (m_trips_loop > 1) {
    libxsmm_aarch64_instruction_alu_move( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_LDR_I_OFF, i_gp_reg_mapping->gp_reg_param_struct, LIBXSMM_AARCH64_GP_REG_UNDEF, 40, i_gp_reg_mapping->gp_reg_ind_base );
    libxsmm_aarch64_instruction_alu_compute_imm64( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_META_ADD, i_gp_reg_mapping->gp_reg_in_base, i_gp_reg_mapping->gp_reg_scratch_0, i_gp_reg_mapping->gp_reg_in_base, (unsigned long long)16 * (unsigned long long)m_unroll_factor );
    libxsmm_generator_loop_footer_aarch64(io_generated_code, io_loop_label_tracker, i_gp_reg_mapping->gp_reg_m_loop, 1);
  }

  if (peeled_m_trips > 0) {
    if (m_trips_loop >= 1) {
      libxsmm_aarch64_instruction_alu_move( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_LDR_I_OFF, i_gp_reg_mapping->gp_reg_param_struct, LIBXSMM_AARCH64_GP_REG_UNDEF, 40, i_gp_reg_mapping->gp_reg_ind_base );
    }
    if (m_trips_loop == 1) {
      libxsmm_aarch64_instruction_alu_compute_imm64( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_META_ADD, i_gp_reg_mapping->gp_reg_in_base, i_gp_reg_mapping->gp_reg_scratch_0, i_gp_reg_mapping->gp_reg_in_base, (unsigned long long)16 * (unsigned long long)m_unroll_factor );
    }

    /* Perform the reductions for all columns */
    for (im = 0; im < peeled_m_trips; im++) {
      if (load_acc == 0) {
        libxsmm_aarch64_instruction_asimd_compute( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_EOR_V, im, im, 0, im, LIBXSMM_AARCH64_ASIMD_TUPLETYPE_16B );
      } else {
        libxsmm_aarch64_instruction_asimd_move( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_LDR_I_POST, i_gp_reg_mapping->gp_reg_out, LIBXSMM_AARCH64_GP_REG_UNDEF, 16, im, LIBXSMM_AARCH64_ASIMD_WIDTH_Q );
      }
    }
    if (load_acc == 1) {
      libxsmm_aarch64_instruction_alu_compute_imm64( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_META_SUB, i_gp_reg_mapping->gp_reg_out, i_gp_reg_mapping->gp_reg_scratch_0, i_gp_reg_mapping->gp_reg_out, (unsigned long long)peeled_m_trips * (unsigned long long)16 );
    }

    libxsmm_generator_loop_header_gp_reg_bound_aarch64( io_generated_code, io_loop_label_tracker, i_gp_reg_mapping->gp_reg_n_loop, i_gp_reg_mapping->gp_reg_n );
    libxsmm_aarch64_instruction_alu_move( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_LDR_I_POST, i_gp_reg_mapping->gp_reg_ind_base, LIBXSMM_AARCH64_GP_REG_UNDEF, idx_tsize, gp_reg_index );
    libxsmm_aarch64_instruction_alu_compute_shifted_reg( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_MUL, gp_reg_index_64bit, gp_reg_ldi, i_gp_reg_mapping->gp_reg_in, 0, LIBXSMM_AARCH64_SHIFTMODE_LSL );
    libxsmm_aarch64_instruction_alu_compute_shifted_reg( io_generated_code, LIBXSMM_AARCH64_INSTR_GP_ADD_SR, i_gp_reg_mapping->gp_reg_in, i_gp_reg_mapping->gp_reg_in_base, i_gp_reg_mapping->gp_reg_in, 0, LIBXSMM_AARCH64_SHIFTMODE_LSL );

    for (im = 0; im < peeled_m_trips; im++) {
      if ((im == peeled_m_trips -1) && (use_m_masking > 0)) {
        /* TODO: Add code when masking is supported */
      } else {
        libxsmm_aarch64_instruction_asimd_move( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_LDR_I_POST, i_gp_reg_mapping->gp_reg_in, LIBXSMM_AARCH64_GP_REG_UNDEF, 16, aux_vreg_offset+im, LIBXSMM_AARCH64_ASIMD_WIDTH_Q );
        libxsmm_aarch64_instruction_asimd_compute( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_FADD_V, aux_vreg_offset+im, im, 0, im, LIBXSMM_AARCH64_ASIMD_TUPLETYPE_4S);
      }
    }

    libxsmm_generator_loop_footer_aarch64(io_generated_code, io_loop_label_tracker, i_gp_reg_mapping->gp_reg_n_loop, 1);

    /* Now store accumulators  */
    for (im = 0; im < peeled_m_trips; im++) {
      libxsmm_aarch64_instruction_asimd_move( io_generated_code, LIBXSMM_AARCH64_INSTR_ASIMD_STR_I_POST, i_gp_reg_mapping->gp_reg_out, LIBXSMM_AARCH64_GP_REG_UNDEF, 16, im, LIBXSMM_AARCH64_ASIMD_WIDTH_Q );
    }
  }

#if 0
  libxsmm_x86_instruction_register_jump_label(io_generated_code, END_LABEL, p_jump_label_tracker);
#endif

#if defined(LIBXSMM_GENERATOR_MATELTWISE_REDUCE_AARCH64_JUMP_LABEL_TRACKER_MALLOC)
  free(p_jump_label_tracker);
#endif
}
