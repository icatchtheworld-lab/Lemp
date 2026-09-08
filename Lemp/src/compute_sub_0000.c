/*
 * This file is developed by EdgeCortix Inc. to be used with certain Renesas Electronics Hardware only.
 *
 * Copyright © 2025 EdgeCortix Inc. Licensed to Renesas Electronics Corporation with the
 * right to sublicense under the Apache License, Version 2.0.
 *
 * This file also includes source code originally developed by the Renesas Electronics Corporation.
 * The Renesas disclaimer below applies to any Renesas-originated portions for usage of the code.
 *
 * The Renesas Electronics Corporation
 * DISCLAIMER
 * This software is supplied by Renesas Electronics Corporation and is only intended for use with Renesas products. No
 * other uses are authorized. This software is owned by Renesas Electronics Corporation and is protected under all
 * applicable laws, including copyright laws.
 * THIS SOFTWARE IS PROVIDED 'AS IS' AND RENESAS MAKES NO WARRANTIES REGARDING
 * THIS SOFTWARE, WHETHER EXPRESS, IMPLIED OR STATUTORY, INCLUDING BUT NOT LIMITED TO WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. ALL SUCH WARRANTIES ARE EXPRESSLY DISCLAIMED. TO THE MAXIMUM
 * EXTENT PERMITTED NOT PROHIBITED BY LAW, NEITHER RENESAS ELECTRONICS CORPORATION NOR ANY OF ITS AFFILIATED COMPANIES
 * SHALL BE LIABLE FOR ANY DIRECT, INDIRECT, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES FOR ANY REASON RELATED TO THIS
 * SOFTWARE, EVEN IF RENESAS OR ITS AFFILIATES HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
 * Renesas reserves the right, without notice, to make changes to this software and to discontinue the availability of
 * this software. By using this software, you agree to the additional terms and conditions found by accessing the
 * following link:
 * http://www.renesas.com/disclaimer
 *
 * Changed from original python code to C source code.
 * Copyright (C) 2017 Renesas Electronics Corporation. All rights reserved.
 *
 * This file also includes source codes originally developed by the TensorFlow Authors which were distributed under the following conditions.
 *
 * The TensorFlow Authors
 * Copyright 2023 The Apache Software Foundation
 *
 * This product includes software developed at
 * The Apache Software Foundation (http://www.apache.org/).
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>

#include "compute_sub_0000.h"

#include "arm_nn_types.h"
#include "arm_nnfunctions.h"
#include "kernel_library_utils.h"

#include "kernel_library_int.h" 

 

void compute_sub_0000_book_face_v5(
  // buffer for intermediate results
  uint8_t* main_storage, // should provide at least 196613 bytes of storage

  // inputs
  
  const float images[196608], // 1,3,256,256
  

  // outputs
  
  int8_t images_70592_11134[196608]  // 1,256,256,3
  
) {
  // Buffers allocated on the main storage (note: depends on the execution order)
    
  
  int8_t* images_11132 = (int8_t *) &main_storage[0]; // 1,3,256,256 == 196608
  
  

  // Parameters
  







//
// Quantize
//
// Input  : float - 1,3,256,256
// Output : int8_t - 1,3,256,256
AffineQuantizeFloatToInt8(
  images,   // input data
  images_11132,   // output data
  196608,   // size
  -1,   // output zeropoint
  0.007843137718737125);   // output scale

//
// Transpose
//
// Input images_11132: int8_t - 1,3,256,256
// Output images_70592_11134: int8_t - 1,256,256,3
// Perm: ( 0,  2,  3,  1, )

int32_t strides_images_70592_11134[4] = { 196608, 256, 1, 65536,  };

int32_t next_dim_sizes_images_70592_11134[4] = { 196608, 196608, 768, 3,  };

int32_t dim_sizes_images_70592_11134[4] = { 196608, 768, 3, 1,  };


Transpose(
      images_11132
    , images_70592_11134
    , 196608
    , 4
    , strides_images_70592_11134
    , next_dim_sizes_images_70592_11134
    , dim_sizes_images_70592_11134
);

}
