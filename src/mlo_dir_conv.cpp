/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2017 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/

#define MIOPEN

#include <miopen/config.h>

#include <cmath>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <unordered_map>

#include <miopen/solver.hpp>
#include <miopen/db.hpp>
#include <miopen/env.hpp>
#include <miopen/gcn_asm_utils.hpp>
#include <miopen/mlo_internal.hpp>
#include <miopen/mlo_utils.hpp>

MIOPEN_DECLARE_ENV_VAR(MIOPEN_DEBUG_GCN_ASM_KERNELS)

/************************************************************************************************************************
 **
 **			CONSTRUCT CONVOLUTIONAL LAYER
 **
 ************************************************************************************************************************/

miopen::MultiFileDb mlo_construct_direct2D::GetDb() const
{
    return {db_path(), _search_params.GetUserPerfDbPath()};
}

std::vector<miopen::solver::ConvSolution> mlo_construct_direct2D::FindAllSolutions()
{
    // clang-format off
    return miopen::solver::SearchForAllSolutions<
        miopen::solver::ConvAsm3x3U,
        miopen::solver::ConvAsm1x1U,
        miopen::solver::ConvAsm5x10u2v2f1,
        miopen::solver::ConvAsm7x7c3h224w224k64u2v2p3q3f1,
        miopen::solver::ConvAsm5x10u2v2b1,
        miopen::solver::ConvOclDirectFwd11x11,
        miopen::solver::ConvOclDirectFwdGen,
        miopen::solver::ConvOclDirectFwd3x3,
        miopen::solver::ConvOclDirectFwd1x1,
        miopen::solver::ConvOclDirectFwd
    >(_search_params, this->GetDb());
    // clang-format on
}

miopen::solver::ConvSolution mlo_construct_winograd::FindSolution()
{
    // clang-format off
    return miopen::solver::SearchForSolution<
        miopen::solver::ConvBinWinograd3x3U,
        miopen::solver::ConvBinWinogradRxS
    >(_search_params, this->GetDb());
    // clang-format on
}

std::vector<miopen::solver::ConvSolution> mlo_construct_BwdWrW2D::FindAllSolutions()
{
    // clang-format off
    return miopen::solver::SearchForAllSolutions<
        miopen::solver::ConvAsmBwdWrW1x1,
        miopen::solver::ConvAsmBwdWrW3x3,
        miopen::solver::ConvOclBwdWrW2,
        miopen::solver::ConvOclBwdWrW53,
        miopen::solver::ConvOclBwdWrW1x1
    >(_search_params, this->GetDb());
    // clang-format on
}

#if MIOPEN_BACKEND_OPENCL
static bool IsTokenWithin(const std::string& s, const char* delimiters, const std::string& find_tok)
{
    assert(delimiters);
    std::size_t cursor = 0;
    do
    {
        const std::size_t tok_begin = s.find_first_not_of(delimiters, cursor);
        if(tok_begin == std::string::npos)
        {
            break;
        }
        cursor            = s.find_first_of(delimiters, tok_begin);
        std::string token = (cursor == std::string::npos) ? s.substr(tok_begin)
                                                          : s.substr(tok_begin, cursor - tok_begin);
        if(token == find_tok)
        {
            return true;
        }
    } while(cursor != std::string::npos);
    return false;
}

static bool IsAmdRocmOpencl(const miopen::ConvolutionContext& context)
{
    const auto dev             = miopen::GetDevice(context.GetStream().GetStream());
    const auto platform        = miopen::GetDeviceInfo<CL_DEVICE_PLATFORM>(dev);
    const auto platform_vendor = miopen::GetPlatformInfo<CL_PLATFORM_VENDOR>(platform);
    if(platform_vendor != "Advanced Micro Devices, Inc.")
    {
        return false;
    }
    const auto device_vendor_id = miopen::GetDeviceInfo<CL_DEVICE_VENDOR_ID>(dev);
    if(device_vendor_id != 0x1002) // AMD
    {
        return false;
    }
    const auto driver_version = miopen::GetDeviceInfo<CL_DRIVER_VERSION>(dev);
    const char* delimiters    = " (),*";                    // Specific for ROCm OCL driver version.
    return IsTokenWithin(driver_version, delimiters, "LC"); // Lightning Compiler.
}
#endif // MIOPEN_BACKEND_OPENCL

static std::ostream& operator<<(std::ostream& os, const rocm_meta_version& rmv)
{
    switch(rmv)
    {
    case rocm_meta_version::Unknown: return os << "Unknown";
    case rocm_meta_version::V1: return os << "V1";
    case rocm_meta_version::V2: return os << "V2";
    case rocm_meta_version::V3: return os << "V3";
    case rocm_meta_version::AMDHSA_1_0: return os << "AMDHSA_1_0";
    }
    return os << "<Error>";
}

static rocm_meta_version DetectAmdRocmMetadataVersion(const miopen::ConvolutionContext& context)
{
#if MIOPEN_BACKEND_OPENCL
    const auto dev                     = miopen::GetDevice(context.GetStream().GetStream());
    const auto platform                = miopen::GetDeviceInfo<CL_DEVICE_PLATFORM>(dev);
    const std::string platform_version = miopen::GetPlatformInfo<CL_PLATFORM_VERSION>(
        platform); // e.g. "OpenCL 2.0 AMD-APP.internal (2334.0)"
    size_t num_begin      = platform_version.find('(');
    rocm_meta_version rmv = rocm_meta_version::Unknown;
    if(num_begin != std::string::npos)
    {
        int num = std::stoi(platform_version.substr(num_begin + 1));
        if(num < 2338) // Switched to V2 somewhere within [2337,2338]
            rmv = rocm_meta_version::V1;
        else if(num < 2389) // Switched to V3 somewhere within [2388,2389]
            rmv = rocm_meta_version::V2;
        else if(num < 2535) // Switched to newer version at 2535 for sure.
            rmv = rocm_meta_version::V3;
        else
            rmv = rocm_meta_version::AMDHSA_1_0;
    }
#else
    /// \todo Rework this using clang-ocl.
    (void)context;
    rocm_meta_version rmv = rocm_meta_version::Default;
    // Assembler is always available for HIP backend.
    // ROCm 1.7, which uses AMDHSA_1_0 metadata, does not have bug 34765 in
    // the assembler. Previous ROCm versions have this bug.
    if(!GcnAssemblerHasBug34765())
    {
        rmv = rocm_meta_version::AMDHSA_1_0;
    }
#endif // MIOPEN_BACKEND_OPENCL
    MIOPEN_LOG_I(rmv);
    return rmv;
}

static bool mloIsAmdRocmOpencl(miopen::ConvolutionContext& context)
{
    static const bool ret_bool =
#if MIOPEN_BACKEND_OPENCL
        IsAmdRocmOpencl(context);
#else
        true;
#endif // MIOPEN_BACKEND_OPENCL
    if(ret_bool)
    {
        static const rocm_meta_version ret_rmv = DetectAmdRocmMetadataVersion(context);
        context.rmv                            = ret_rmv;
    }
    return ret_bool;
}

void mlo_construct_direct2D::setupFloats()
{
    if(_search_params.float_size == 32)
    {
        _search_params.general_compile_options += " -DMIOPEN_USE_FP32=1 -DMIOPEN_USE_FP16=0";
    }
    else if(_search_params.float_size == 16)
    {
        _search_params.general_compile_options += " -DMIOPEN_USE_FP32=0 -DMIOPEN_USE_FP16=1";
    }
}

void mlo_construct_direct2D::setupRocm()
{
    // Detect assembly kernels
    _search_params.use_binaries    = false;
    _search_params.use_asm_kernels = false;
    _search_params.rmv             = rocm_meta_version::Default;
    if(mloIsAmdRocmOpencl(_search_params))
    {
        _search_params.use_asm_kernels =
            !miopen::IsDisabled(MIOPEN_DEBUG_GCN_ASM_KERNELS{}) && ValidateGcnAssembler();
#ifndef HIP_OC_FINALIZER
        _search_params.use_binaries =
            !miopen::IsDisabled(MIOPEN_DEBUG_AMD_ROCM_PRECOMPILED_BINARIES{});
#endif
    }
}

bool mlo_construct_direct2D::mloIsFastBinaryWinograd3x3U() const
{
    return (_search_params.n_outputs >= 16 && _search_params.n_outputs % 2 == 0);
}

/***********************************************************************************************************

 * Internal implementation of the direct conv configuration search

 ************************************************************************************************************/

/*
   the search db is a text file with the name defined by the device characteristics.
   each line is a key/value pair, separated by a space:
   32x16x16x3x3x64x16x16x100xNCHWxFP32x1 16.16.16.16.1.4.8.4.1
   or
   64x8x8x5x5x32x8x8x100xNCHWxFP32x0 16.16.8.8.2.4.1.1.4

   key format (all values are separted by x):
   n input maps
   input height
   input width
   filter height
   filter width
   n output maps
   output height
   output width
   batch size
   tensors' layout
   tensprs' data type
   direction (1 - forward, 0 - backward)

Note:
for backward direction - input and output are reversed.

value format (all values are separated by .):
vertical group size
horizontal group size
input block vertical size
input block horizontal size
output tile vertical size
output tile horizaontal size
n of output tiles
n of input blocks
n batchs (stacks) processed by the group
*/

int mlo_construct_direct2D::mloBuildConf_Key(std::string& conf_key) const
{

    conf_key =
        std::to_string(static_cast<long long>(_search_params.n_inputs)) + std::string("x") +
        std::to_string(static_cast<long long>(_search_params.in_height)) + std::string("x") +
        std::to_string(static_cast<long long>(_search_params.in_width)) + std::string("x") +
        std::to_string(static_cast<long long>(_search_params.kernel_size1)) + std::string("x") +
        std::to_string(static_cast<long long>(_search_params.kernel_size0)) + std::string("x") +
        std::to_string(static_cast<long long>(_search_params.n_outputs)) + std::string("x") +
        std::to_string(static_cast<long long>(_search_params.out_height)) + std::string("x") +
        std::to_string(static_cast<long long>(_search_params.out_width)) + std::string("x") +
        std::to_string(static_cast<long long>(_search_params.batch_sz)) + std::string("x") +
        _search_params.in_layout + std::string("x") + _search_params.in_data_type +
        std::string("x") + (_search_params.direction.IsForward()
                                ? "1"
                                : "0"); /// \todo Shall we separate keys for WrW convolutions?
    return (0);
}

// Tensor Helper APIs

size_t
mlo_construct_direct2D::setWeightDescFromMLDesc(const miopen::TensorDescriptor& weight_tensor)
{

    int nWei;
    int cWei;
    int hWei;
    int wWei;
    int nWeiStride;
    int cWeiStride;
    int hWeiStride;
    int wWeiStride;

    std::tie(nWei, cWei, hWei, wWei) = miopen::tien<4>(weight_tensor.GetLengths(), 1);
    std::tie(nWeiStride, cWeiStride, hWeiStride, wWeiStride) =
        miopen::tien<4>(weight_tensor.GetStrides(), 0);

    std::string data_type = weight_tensor.GetType() == miopenFloat ? "FP32" : "FP16";

    setWeightsDescr(
        "NCHW", data_type, nWei, cWei, hWei, wWei, nWeiStride, cWeiStride, hWeiStride, wWeiStride);

    return weight_tensor.GetElementSpace();
}

size_t
mlo_construct_direct2D::setOutputDescFromMLDesc(const miopen::TensorDescriptor& output_tensor)
{

    int nOut;
    int cOut;
    int hOut;
    int wOut;
    int nOutStride;
    int cOutStride;
    int hOutStride;
    int wOutStride;

    std::tie(nOut, cOut, hOut, wOut) = miopen::tien<4>(output_tensor.GetLengths(), 1);
    std::tie(nOutStride, cOutStride, hOutStride, wOutStride) =
        miopen::tien<4>(output_tensor.GetStrides(), 0);

    std::string data_type = output_tensor.GetType() == miopenFloat ? "FP32" : "FP16";

    setOutputDescr(
        "NCHW", data_type, nOut, cOut, hOut, wOut, nOutStride, cOutStride, hOutStride, wOutStride);
    return output_tensor.GetElementSpace();
}

size_t mlo_construct_direct2D::setInputDescFromMLDesc(const miopen::TensorDescriptor& input_tensor)
{

    int nIn;
    int cIn;
    int hIn;
    int wIn;
    int nInStride;
    int cInStride;
    int hInStride;
    int wInStride;

    std::tie(nIn, cIn, hIn, wIn) = miopen::tien<4>(input_tensor.GetLengths(), 1);
    std::tie(nInStride, cInStride, hInStride, wInStride) =
        miopen::tien<4>(input_tensor.GetStrides(), 0);

    std::string data_type = input_tensor.GetType() == miopenFloat ? "FP32" : "FP16";

    setInputDescr(
        "NCHW", data_type, nIn, cIn, hIn, wIn, nInStride, cInStride, hInStride, wInStride);

    return input_tensor.GetElementSpace();
}

size_t mlo_construct_direct2D::setTopDescFromMLDesc(const miopen::TensorDescriptor& tensor)
{
    int nIn;
    int cIn;
    int hIn;
    int wIn;
    int nInStride;
    int cInStride;
    int hInStride;
    int wInStride;

    std::tie(nIn, cIn, hIn, wIn)                         = miopen::tien<4>(tensor.GetLengths(), 1);
    std::tie(nInStride, cInStride, hInStride, wInStride) = miopen::tien<4>(tensor.GetStrides(), 0);

    std::string data_type = tensor.GetType() == miopenFloat ? "FP32" : "FP16";

    setTopDescr("NCHW", data_type, nIn, cIn, hIn, wIn, nInStride, cInStride, hInStride, wInStride);

    return tensor.GetElementSpace();
}
size_t mlo_construct_direct2D::setBotDescFromMLDesc(const miopen::TensorDescriptor& tensor)
{
    int nIn;
    int cIn;
    int hIn;
    int wIn;
    int nInStride;
    int cInStride;
    int hInStride;
    int wInStride;

    std::tie(nIn, cIn, hIn, wIn)                         = miopen::tien<4>(tensor.GetLengths(), 1);
    std::tie(nInStride, cInStride, hInStride, wInStride) = miopen::tien<4>(tensor.GetStrides(), 0);

    std::string data_type = tensor.GetType() == miopenFloat ? "FP32" : "FP16";

    setBotDescr("NCHW", data_type, nIn, cIn, hIn, wIn, nInStride, cInStride, hInStride, wInStride);

    return tensor.GetElementSpace();
}

size_t mlo_construct_direct2D::setTopDfDescFromMLDesc(const miopen::TensorDescriptor& tensor)
{
    int nIn;
    int cIn;
    int hIn;
    int wIn;
    int nInStride;
    int cInStride;
    int hInStride;
    int wInStride;

    std::tie(nIn, cIn, hIn, wIn)                         = miopen::tien<4>(tensor.GetLengths(), 1);
    std::tie(nInStride, cInStride, hInStride, wInStride) = miopen::tien<4>(tensor.GetStrides(), 0);

    std::string data_type = tensor.GetType() == miopenFloat ? "FP32" : "FP16";

    setTopDfDescr(
        "NCHW", data_type, nIn, cIn, hIn, wIn, nInStride, cInStride, hInStride, wInStride);

    return tensor.GetElementSpace();
}
size_t mlo_construct_direct2D::setBotDfDescFromMLDesc(const miopen::TensorDescriptor& tensor)
{
    int nIn;
    int cIn;
    int hIn;
    int wIn;
    int nInStride;
    int cInStride;
    int hInStride;
    int wInStride;

    std::tie(nIn, cIn, hIn, wIn)                         = miopen::tien<4>(tensor.GetLengths(), 1);
    std::tie(nInStride, cInStride, hInStride, wInStride) = miopen::tien<4>(tensor.GetStrides(), 0);

    std::string data_type = tensor.GetType() == miopenFloat ? "FP32" : "FP16";

    setBotDfDescr(
        "NCHW", data_type, nIn, cIn, hIn, wIn, nInStride, cInStride, hInStride, wInStride);

    return tensor.GetElementSpace();
}
