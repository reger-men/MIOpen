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
/**********************************************************************
Copyright (c)2017 Advanced Micro Devices, Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted
provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list of conditions and
the following disclaimer.
Redistributions in binary form must reproduce the above copyright notice, this list of conditions
and the following disclaimer in the documentation and/or
 other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
********************************************************************/

#ifndef MLO_INTERNAL_H_
#define MLO_INTERNAL_H_

// Header Files
#ifndef NOMINMAX
#define NOMINMAX // stupid windows.h confused with min() macros in std namespace
#endif

#include <miopen/config.h>

#if MIOPEN_BACKEND_OPENCL
#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif
#endif

#ifdef __APPLE__
#include <mach/mach_time.h> // for mach_absolute_time() and friends
#endif

#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cmath>
#include <map>
#include <string>
#include <limits>
#include <algorithm> // std::find  and std::min std::maxx

#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <ctime>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <numeric>
#include <cstdint>
#include <tuple>

using mlo_kernel_info = std::tuple<const std::string,
                                   const std::string,
                                   const std::string,
                                   const std::vector<size_t>,
                                   const std::vector<size_t>>;

#if MIOPEN_BACKEND_OPENCL
#include <miopen/oclkernel.hpp>
#include <miopen/clhelper.hpp>
#include <miopen/ocldeviceinfo.hpp>
#endif
#include <miopen/tensor.hpp>
#include <miopen/handle.hpp>
#include <miopen/db_path.hpp>
#include <miopen/db.hpp>

inline int mloLg2(int v)
{
    auto ret = static_cast<int>(std::ceil(std::log(v) / std::log(2)));
    return (ret);
}

inline int AlignUp(int val, unsigned step)
{
    assert(step > 0);
    return ((val + step - 1) / step) * step;
}

enum class rocm_meta_version
{
    Unknown,
    V1,
    V2,
    V3,
    AMDHSA_1_0,   // 1.0, see https://llvm.org/docs/AMDGPUUsage.html#code-object-metadata
    Default = V3, // Assumption for HIP backend. To be updated together with ROCm release.
};

namespace miopen {

template <class TInstance>
class StaticContainer
{
    public:
    inline static TInstance& Instance()
    {
        static TInstance data{};
        return data;
    }
};

struct ProblemDescription
{
    int n_inputs         = 0;
    int in_height        = 0;
    int in_width         = 0;
    int kernel_size1     = 0;
    int kernel_size0     = 0;
    int n_outputs        = 0;
    int out_height       = 0;
    int out_width        = 0;
    int batch_sz         = 0;
    int pad0             = 0;
    int pad1             = 0;
    int kernel_stride0   = 0;
    int kernel_stride1   = 0;
    int kernel_dilation0 = 0;
    int kernel_dilation1 = 0;
    int bias             = 0;
    std::string in_layout;
    std::string in_data_type;
    std::string weights_layout;
    std::string out_data_type;
    std::string out_layout;
    int float_size         = 32;
    size_t bot_sz          = 0;
    size_t top_sz          = 0;
    size_t weights_sz      = 0;
    size_t bias_sz         = 0;
    int deconvolution      = 0;
    int in_stride          = 0;
    int out_stride         = 0;
    int in_channel_stride  = 0;
    int in_batch_stride    = 0;
    int out_channel_stride = 0;
    int out_batch_stride   = 0;
    struct Direction
    {
        enum class Value
        {
            Unknown,
            Forward,
            Backward,
            BackwardWrW,
        };

        private:
        Value v = Value::Unknown;

        public:
        bool IsKnown() const { return v != Value::Unknown; }
        bool IsForward() const { return v == Value::Forward; }
        bool IsBackwardData() const { return v == Value::Backward; } // Syntax glue.
        bool IsBackwardWrW() const { return v == Value::BackwardWrW; }
        void Set(int forward)
        {
            assert(0 <= forward && forward <= 1);
            v = forward != 0 ? Value::Forward : Value::Backward;
        }
        template <typename T>
        void Set(T) = delete;
        void SetBackwardWrW() { v = Value::BackwardWrW; }
    } direction;
    int GetBackwardPad0() const { return kernel_size0 - pad0 - 1; }
    int GetBackwardPad1() const { return kernel_size1 - pad1 - 1; }

    void Serialize(std::ostream& stream) const
    {
        if(!direction.IsKnown())
            MIOPEN_THROW("!direction.IsKnown()");
        const auto sep = '-';
        // clang-format off
        // 576-4-4-1x1-192-4-4-8-1x1-2x2-3x3-0-NCHW-FP32-F
        stream
            << n_inputs << sep << in_height << sep << in_width
            << sep << kernel_size1 << 'x' << kernel_size0
            << sep << n_outputs << sep << out_height << sep << out_width
            << sep << batch_sz
            << sep << pad1 << 'x' << pad0
            << sep << kernel_stride1 << 'x' << kernel_stride0
            << sep << kernel_dilation1 << 'x' << kernel_dilation1
            << sep << bias
            << sep << in_layout
            << sep << in_data_type
            << sep << (direction.IsForward() ? "F"
                     : direction.IsBackwardData() ? "B" : "W"); // clang-format on
    }

    friend std::ostream& operator<<(std::ostream& os, const ProblemDescription& obj)
    {
        obj.Serialize(os);
        return os;
    }
};

/// A leftover of the legacy design, houses problem config,
/// environmental context (e.g. HW/SW platform) and solver-specific state.
///
/// TODO: These three entities should be made separate.
struct ConvolutionContext : ProblemDescription
{
    // Solution-specific
    std::string general_compile_options;
    // Operation modes & environment
    bool do_search                         = false;
    bool save_srch_req                     = false;
    bool use_asm_kernels                   = false;
    bool use_binaries                      = true;
    rocm_meta_version rmv                  = rocm_meta_version::Default;
    bool workaround_disable_search_enforce = false;

    inline Handle& GetStream() const { return *_stream; }
    inline void SetStream(Handle* stream) { _stream = stream; }

    std::string GetPerfDbPath() const
    {
        // clang-format off
        return GetDbPath()
             + "/"
             + GetStream().GetDeviceName()
             + "_"
             + std::to_string(GetStream().GetMaxComputeUnits())
             + ".cd.pdb.txt";
        // clang-format on
    }

    std::string GetUserPerfDbPath() const
    {
        // clang-format off
        return GetUserDbPath()
             + "/"
             + GetStream().GetDeviceName()
             + "_"
             + std::to_string(GetStream().GetMaxComputeUnits())
             + ".cd.updb.txt";
        // clang-format on
    }

    private:
    Handle* _stream = nullptr;
};

namespace solver {
struct ConvSolution;

} // namespace solver

} // namespace miopen

template <class T>
auto mloConstruct(T& x) -> decltype(x.mloConstruct(), void())
{
    x.setupFloats();
    x.mloConstruct();
}

template <class T>
auto FindFirstSolution(T& x) -> decltype(x.FindSolution())
{
    x.setupRocm();
    x.setupFloats();
    return x.FindSolution();
}

template <class T>
auto FindAllSolutions(T& x) -> decltype(x.FindAllSolutions())
{
    x.setupRocm();
    x.setupFloats();
    return x.FindAllSolutions();
}

/// \todo Move this into respective Solution objects. --atamazov
struct mlo_construct_activ_lrn_pooling_common
{
    std::string _comp_options;
    std::string _kernel_file;
    std::string _kernel_name;
    std::vector<size_t> _l_wk;
    std::vector<size_t> _g_wk;

    /*
    * returns kernel file name without location
    */
    inline std::string getKernelFile() const { return (_kernel_file); }
    /*
    * retuns kerner/shader name
    */
    inline std::string getKernelName() const { return (_kernel_name); }
    /*
    * return set of compile options
    */
    inline const std::string& getCompilerOptions() const { return (_comp_options); }
    /*
    *  return a local working configuration
    */
    inline const std::vector<size_t>& getLocalWkSize() const { return (_l_wk); }
    /*
    * return a global working configuration
    */
    inline const std::vector<size_t>& getGlobalWkSize() const { return (_g_wk); }

    int _grp_tile0      = 8; // total number ALUs per group
    int _grp_tile1      = 8; // total number ALUs per group
    int _out_pix_tile0  = 2; // # of generated pixels per output per wk-item  (ALU)
    int _out_pix_tile1  = 4; // # of generated pixels per output per wk-item  (ALU)
    size_t _workspce_sz = 0;

    /*
    * get workspace size
    */
    inline size_t getWorkSpaceSzBytes() const { return (_workspce_sz); }
};

struct mlo_construct_direct2D
{
    mlo_construct_direct2D(int dir, bool do_bias = false)
    {
        _search_params.direction.Set(dir);
        _search_params.bias             = (do_bias) ? 1 : 0;
        _search_params.pad0             = 1;
        _search_params.pad1             = 1;
        _search_params.kernel_size0     = 3;
        _search_params.kernel_size1     = 3;
        _search_params.kernel_stride0   = 1;
        _search_params.kernel_stride1   = 1;
        _search_params.kernel_dilation0 = 1;
        _search_params.kernel_dilation1 = 1;
        _search_params.deconvolution    = 0;
        _search_params.bot_sz           = 0; // bytes
        _search_params.top_sz           = 0; // bytes
        _search_params.weights_sz       = 0; // bytes
        _search_params.bias_sz          = 0; // bytes
    }

    void setupRocm();
    void setupFloats();

    /*
    * major interface
    * it has to be called only after
    * convolutional parmeters, input, output and weight tesnor have been set
    *
    * constructs compiler option
    *
    * selects kernel file and name
    * covers genrinc forward convolution:
    * arbitrary combination of kerenl sizes, strides
    */

    miopen::solver::ConvSolution FindSolution();
    std::vector<miopen::solver::ConvSolution> FindAllSolutions();
    miopen::MultiFileDb GetDb() const;

    /*
    * returns parameter values that are compiled in legacy kernels for kernels using them as
    * arguments.
    */
    inline void getCompiledInParameters(int* const N,
                                        int* const C,
                                        int* const H,
                                        int* const W,
                                        int* const K,
                                        int* const n_groups) const
    {
        assert(N && C && H && W && K && n_groups);
        *N        = _search_params.batch_sz;
        *C        = _search_params.n_inputs;
        *H        = _search_params.in_height;
        *W        = _search_params.in_width;
        *K        = _search_params.n_outputs;
        *n_groups = _search_params.GetStream().GetMaxComputeUnits();
    }

    inline void getCompiledInParameters(int* const N,
                                        int* const C,
                                        int* const H,
                                        int* const W,
                                        int* const K,
                                        int* const n_groups,
                                        int* const out_H,
                                        int* const out_W) const
    {
        getCompiledInParameters(N, C, H, W, K, n_groups);
        assert(out_H && out_W);
        *out_H = _search_params.out_height;
        *out_W = _search_params.out_width;
    }

    inline void getCompiledInParameters(int* const N,
                                        int* const C,
                                        int* const H,
                                        int* const W,
                                        int* const K,
                                        int* const n_groups,
                                        int* const out_H,
                                        int* const out_W,
                                        int* const R,
                                        int* const S,
                                        int* const pad_H,
                                        int* const pad_W) const
    {
        getCompiledInParameters(N, C, H, W, K, n_groups, out_H, out_W);
        assert(R && S && pad_H && pad_W);
        *R     = _search_params.kernel_size1;
        *S     = _search_params.kernel_size0;
        *pad_H = _search_params.direction.IsForward() ? _search_params.pad1
                                                      : _search_params.GetBackwardPad1();
        *pad_W = _search_params.direction.IsForward() ? _search_params.pad0
                                                      : _search_params.GetBackwardPad0();
    }

    /*
    * get common compiler options
    */
    inline const std::string& getGeneralCompOptions() const
    {
        return (_search_params.general_compile_options);
    }

    /*
    * return direction: true - forward, false - backward
    */
    inline bool isForwardDirection() const
    {
        if(!_search_params.direction.IsKnown())
            MIOPEN_THROW("!_search_params.direction.IsKnown()");
        return _search_params.direction.IsForward(); // convolutions: backward data OR wrw otherwise
    }

    /*
    *  is bias incuded
    */
    inline bool doBias() const { return (_search_params.bias == 1); }

    /*
    * set library stream
    */
    inline void setStream(miopen::Handle* stream) { _search_params.SetStream(stream); }

    /*
    * set convolutional parameters
    */
    inline void setConvDescr(int u_padding,
                             int v_padding,
                             int u_stride,
                             int v_stride,
                             int /*u_upstride*/,
                             int /*v_upstride*/
                             )
    {
        _search_params.pad1           = u_padding;
        _search_params.pad0           = v_padding;
        _search_params.kernel_stride0 = u_stride;
        _search_params.kernel_stride1 = v_stride;
    }

    /*
    * set weights tensor
    */
    inline void setWeightsDescr(const std::string& layout,
                                const std::string& data_type,
                                int batch,
                                int depth,
                                int height,
                                int width,
                                int batch_stride,
                                int channel_stride,
                                int stride,
                                int w_stride)
    {
        _search_params.kernel_size0 = width;
        _search_params.kernel_size1 = height;
        int data_len                = (data_type == "FP32" ? 4 : 8);
        _search_params.float_size   = (data_type == "FP32" ? 32 : 16);
        size_t size                 = (layout == "NCHW")
                          ? batch * depth * height * width * data_len
                          : batch * batch_stride * channel_stride * stride * w_stride * data_len;
        _search_params.weights_sz = size;
    }

    /*
    * set output tensor
    */
    inline void setOutputDescr(const std::string& layout,
                               const std::string& data_type,
                               int batch,
                               int depth,
                               int height,
                               int width,
                               int batch_stride,
                               int channel_stride,
                               int stride,
                               int w_stride)
    {
        _search_params.batch_sz   = batch;
        int data_len              = (data_type == "FP16") ? 2 : (data_type == "FP32") ? 4 : 8;
        _search_params.float_size = (data_type == "FP32" ? 32 : 16);
        size_t size               = (layout == "NCHW")
                          ? batch * depth * height * width * data_len
                          : batch * batch_stride * channel_stride * stride * w_stride * data_len;
        if(_search_params.direction.IsForward())
        {

            _search_params.out_width          = width;
            _search_params.out_height         = height;
            _search_params.n_outputs          = depth;
            _search_params.out_batch_stride   = batch_stride;
            _search_params.out_channel_stride = channel_stride;
            _search_params.out_stride         = stride;
            _search_params.top_sz             = size;
            _search_params.out_layout         = layout;
            _search_params.out_data_type      = data_type;
        }
        else
        {
            _search_params.in_width          = width;
            _search_params.in_height         = height;
            _search_params.n_inputs          = depth;
            _search_params.in_batch_stride   = batch_stride;
            _search_params.in_channel_stride = channel_stride;
            _search_params.in_stride         = stride;
            _search_params.bot_sz            = size;
            _search_params.in_layout         = layout;
            _search_params.in_data_type      = data_type;
            //			_tens_layout = layout;
            //			_tens_data_format = data_type;
        }
    }

    /*
    *  set input tensor
    */

    inline void setInputDescr(const std::string& layout,
                              const std::string& data_type,
                              int batch,
                              int depth,
                              int height,
                              int width,
                              int batch_stride,
                              int channel_stride,
                              int stride,
                              int w_stride)
    {
        _search_params.batch_sz   = batch;
        int data_len              = (data_type == "FP16") ? 2 : (data_type == "FP32") ? 4 : 8;
        _search_params.float_size = (data_type == "FP32" ? 32 : 16);
        size_t size               = (layout == "NCHW")
                          ? batch * depth * height * width * data_len
                          : batch * batch_stride * channel_stride * stride * w_stride * data_len;
        if(_search_params.direction.IsForward())
        {

            _search_params.in_width          = width;
            _search_params.in_height         = height;
            _search_params.n_inputs          = depth;
            _search_params.in_batch_stride   = batch_stride;
            _search_params.in_channel_stride = channel_stride;
            _search_params.in_stride         = stride;
            _search_params.bot_sz            = size;
            _search_params.in_layout         = layout;
            _search_params.in_data_type      = data_type;
            //			_tens_layout = layout;
            //			_tens_data_format = data_type;
        }
        else
        {
            _search_params.out_width          = width;
            _search_params.out_height         = height;
            _search_params.n_outputs          = depth;
            _search_params.out_batch_stride   = batch_stride;
            _search_params.out_channel_stride = channel_stride;
            _search_params.out_stride         = stride;
            _search_params.top_sz             = size;
            _search_params.out_layout         = layout;
            _search_params.out_data_type      = data_type;
        }

        _search_params.bias_sz =
            (_search_params.bias) != 0 ? _search_params.n_outputs * data_len : 0;
    }

    /*
    * set top tensor
    */
    void setTopDescr(const std::string& layout,
                     const std::string& data_type,
                     int batch,
                     int depth,
                     int height,
                     int width,
                     int batch_stride,
                     int channel_stride,
                     int stride,
                     int w_stride)
    {
        _search_params.batch_sz   = batch;
        int data_len              = (data_type == "FP16") ? 2 : (data_type == "FP32") ? 4 : 8;
        _search_params.float_size = (data_type == "FP32" ? 32 : 16);
        size_t size               = (layout == "NCHW")
                          ? batch * depth * height * width * data_len
                          : batch * batch_stride * channel_stride * stride * w_stride * data_len;

        _search_params.out_width          = width;
        _search_params.out_height         = height;
        _search_params.n_outputs          = depth;
        _search_params.out_batch_stride   = batch_stride;
        _search_params.out_channel_stride = channel_stride;
        _search_params.out_stride         = stride;
        _search_params.top_sz             = size;
        _search_params.out_layout         = layout;
        _search_params.out_data_type      = data_type;
        _search_params.bias_sz =
            (_search_params.bias != 0) ? (_search_params.n_outputs * data_len) : 0;
    }

    /*
    *  set bot tensor
    */

    void setBotDescr(const std::string& layout,
                     const std::string& data_type,
                     int batch,
                     int depth,
                     int height,
                     int width,
                     int batch_stride,
                     int channel_stride,
                     int stride,
                     int w_stride)
    {
        _search_params.batch_sz   = batch;
        int data_len              = (data_type == "FP16") ? 2 : (data_type == "FP32") ? 4 : 8;
        _search_params.float_size = (data_type == "FP32" ? 32 : 16);
        size_t size               = (layout == "NCHW")
                          ? batch * depth * height * width * data_len
                          : batch * batch_stride * channel_stride * stride * w_stride * data_len;

        _search_params.in_width          = width;
        _search_params.in_height         = height;
        _search_params.n_inputs          = depth;
        _search_params.in_batch_stride   = batch_stride;
        _search_params.in_channel_stride = channel_stride;
        _search_params.in_stride         = stride;
        _search_params.bot_sz            = size;
        _search_params.in_layout         = layout;
        _search_params.in_data_type      = data_type;
        //			_tens_layout = layout;
        //			_tens_data_format = data_type;
    }
    /*
    * set top df tensor
    */
    void setTopDfDescr(const std::string& layout,
                       const std::string& data_type,
                       int batch,
                       int depth,
                       int height,
                       int width,
                       int batch_stride,
                       int channel_stride,
                       int stride,
                       int w_stride)
    {
        _search_params.batch_sz   = batch;
        int data_len              = (data_type == "FP16") ? 2 : (data_type == "FP32") ? 4 : 8;
        _search_params.float_size = (data_type == "FP32" ? 32 : 16);
        size_t size               = (layout == "NCHW")
                          ? batch * depth * height * width * data_len
                          : batch * batch_stride * channel_stride * stride * w_stride * data_len;

        _out_df_width            = width;
        _out_df_height           = height;
        _search_params.n_outputs = depth;
        _out_df_batch_stride     = batch_stride;
        _out_df_channel_stride   = channel_stride;
        _out_df_stride           = stride;
        _top_df_sz               = size;
        _out_df_layout           = layout;
        _out_df_data_type        = data_type;
    }

    /*
    *  set bot df tensor
    */

    void setBotDfDescr(const std::string& layout,
                       const std::string& data_type,
                       int batch,
                       int depth,
                       int height,
                       int width,
                       int batch_stride,
                       int channel_stride,
                       int stride,
                       int w_stride)
    {
        _search_params.batch_sz   = batch;
        int data_len              = (data_type == "FP16") ? 2 : (data_type == "FP32") ? 4 : 8;
        _search_params.float_size = (data_type == "FP32" ? 32 : 16);
        size_t size               = (layout == "NCHW")
                          ? batch * depth * height * width * data_len
                          : batch * batch_stride * channel_stride * stride * w_stride * data_len;

        _in_df_width            = width;
        _in_df_height           = height;
        _search_params.n_inputs = depth;
        _in_df_batch_stride     = batch_stride;
        _in_df_channel_stride   = channel_stride;
        _in_df_stride           = stride;
        _bot_df_sz              = size;
        _in_df_layout           = layout;
        _in_df_data_type        = data_type;
    }

    /*
    *  indicate the need for backward pass
    */
    inline void doBackward(bool do_bwd) { _do_backward = do_bwd; }
    /*
    * need for backward pass?
    */
    inline bool doBackward() const { return (_do_backward); }

    /*
    *  allow the search for the best possible solution
    */
    inline void setDoSearch(const bool do_search) { _search_params.do_search = do_search; }

    /*
    * allow to save the missing configuraion in the search request file for an offline search
    */
    inline void saveSearchRequest(bool save_req) { _search_params.save_srch_req = save_req; }
    /*
    * set common compiler options
    */
    inline void setGeneralCompOptions(const std::string& options)
    {
        _search_params.general_compile_options += options;
    }

    inline void setWorkaroundDisableSearchEnforce(bool v)
    {
        _search_params.workaround_disable_search_enforce = v;
    }

    // MD: Hack to get the key outside of mlo_internal
    int mloBuildConf_Key(std::string& conf_key) const;

    // MD: Where is this being used?
    void getNewInputDescr(std::string& layout,
                          std::string& data_type,
                          int& batch,
                          int& depth,
                          int& height,
                          int& width,
                          int& batch_stride,
                          int& channel_stride,
                          int& stride,
                          int& w_stride);
    // TEMP
    int mloConstructSP2D();

    size_t setInputDescFromMLDesc(const miopen::TensorDescriptor& input_tensor);
    size_t setOutputDescFromMLDesc(const miopen::TensorDescriptor& output_tensor);
    size_t setWeightDescFromMLDesc(const miopen::TensorDescriptor& weight_tensor);
    size_t setTopDescFromMLDesc(const miopen::TensorDescriptor& tensor);
    size_t setBotDescFromMLDesc(const miopen::TensorDescriptor& tensor);
    size_t setTopDfDescFromMLDesc(const miopen::TensorDescriptor& tensor);
    size_t setBotDfDescFromMLDesc(const miopen::TensorDescriptor& tensor);

    bool mloIsFastBinaryWinograd3x3U() const;

    std::string db_path() const
    {
        return _db_path != nullptr ? _db_path : _search_params.GetPerfDbPath();
    }

    int mloConstructBwd() { return (0); }
    int mloConstructFwd() { return (0); }

    protected:
    miopen::ConvolutionContext _search_params;

    /// \todo <begin> Move these into respective Contexts (norm, pooling, neuron...) --atamazov
    int _in_df_width          = 0;
    int _in_df_height         = 0;
    int _in_df_batch_stride   = 0;
    int _in_df_channel_stride = 0;
    int _in_df_stride         = 0;
    std::string _in_df_layout;
    std::string _in_df_data_type;

    int _out_df_width          = 0;
    int _out_df_height         = 0;
    int _out_df_batch_stride   = 0;
    int _out_df_channel_stride = 0;
    int _out_df_stride         = 0;
    std::string _out_df_layout;
    std::string _out_df_data_type;

    bool _do_backward = false;

    // local memory size per group
    size_t _dev_local_mem_sz = 0; /// \todo Written but not read - remove?
    // wave size
    int _hw_wave_sz = 0;
    // cl_queue
    size_t _bot_df_sz = 0; /// \todo Written but not read - remove?
    size_t _top_df_sz = 0; /// \todo Written but not read - remove?
    /// \todo <end>

    const char* _db_path = nullptr;
};

/*
* backward with regard to weights construction
*/

struct mlo_construct_BwdWrW2D : mlo_construct_direct2D
{
    mlo_construct_BwdWrW2D(int dir, bool do_bias = false) : mlo_construct_direct2D(dir, do_bias)
    {
        _search_params.direction.SetBackwardWrW();
    }

    std::vector<miopen::solver::ConvSolution> FindAllSolutions();
};

/*
* winograd algorithm
*/

struct mlo_construct_winograd : mlo_construct_direct2D
{
    mlo_construct_winograd(int dir, bool do_bias = false) : mlo_construct_direct2D(dir, do_bias) {}

    miopen::solver::ConvSolution FindSolution();
};

#define MLO_POOLING_OP_AVE 0
#define MLO_POOLING_OP_MAX 1
#define MLO_POOLING_OP_STC 2

struct mlo_construct_pooling2D : mlo_construct_direct2D, mlo_construct_activ_lrn_pooling_common
{
    mlo_construct_pooling2D(int dir) : mlo_construct_direct2D(dir)
    {
        _pooling_method = MLO_POOLING_OP_MAX;
        _NAN_option     = 0;
    }

    inline void setPoolingDescr(int pooling_method = MLO_POOLING_OP_MAX,
                                int windowHeight   = 3,
                                int windowWidth    = 3,
                                int padding_v      = 0,
                                int padding_h      = 0,
                                int stride_v       = 2,
                                int stride_h       = 2,
                                int NAN_opt        = 0)
    {
        _pooling_method               = pooling_method;
        _search_params.pad1           = padding_v;
        _search_params.pad0           = padding_h;
        _search_params.kernel_size1   = windowHeight;
        _search_params.kernel_size0   = windowWidth;
        _search_params.kernel_stride1 = stride_v;
        _search_params.kernel_stride0 = stride_h;
        _NAN_option                   = NAN_opt;
    }

    inline void getPoolingDescr(int& /*pooling_method*/,
                                int& windowHeight,
                                int& windowWidth,
                                int& padding_v,
                                int& padding_h,
                                int& stride_v,
                                int& stride_h,
                                int& NAN_opt) const
    {
        padding_v    = _search_params.pad1;
        padding_h    = _search_params.pad0;
        windowHeight = _search_params.kernel_size1;
        windowWidth  = _search_params.kernel_size0;
        stride_v     = _search_params.kernel_stride1;
        stride_h     = _search_params.kernel_stride0;
        NAN_opt      = _NAN_option;
    }

    inline int getPoolingMethod() const { return (_pooling_method); }
    int mloConstruct();

    protected:
    int _pooling_method;
    int _NAN_option;
    int mloConstructFwd();
    int mloConstructBwd();
};

#define MLO_LRN_WITHIN_CHANNEL 0
#define MLO_LRN_ACROSS_CHANNELS 1

struct mlo_construct_norm : mlo_construct_direct2D, mlo_construct_activ_lrn_pooling_common
{
    mlo_construct_norm(int dir) : mlo_construct_direct2D(dir) {}

    inline void setNormDescr(
        int norm_region, int norm_area, double normAlpha, double normBeta, double normK = 1.)
    {
        _norm_region = norm_region;
        _norm_area   = norm_area;
        _normAlpha   = normAlpha;
        _normBeta    = normBeta;
        _normK       = normK;
    }

    inline void getNormDescr(int& norm_region,
                             int& norm_area,
                             double& normAlpha,
                             double& normBeta,
                             double& normK,
                             double& alphaoverarea) const
    {
        norm_region   = _norm_region;
        norm_area     = _norm_area;
        normAlpha     = _normAlpha;
        normBeta      = _normBeta;
        normK         = _normK;
        alphaoverarea = (_norm_region == MLO_LRN_ACROSS_CHANNELS)
                            ? _normAlpha / _norm_area
                            : _normAlpha / (_norm_area * _norm_area);
    }

    void mloConstruct();

    protected:
    int mloConstructFwd();
    int mloConstructBwd();
    int _norm_region  = 0;
    int _norm_area    = 0;
    double _normAlpha = 0.0;
    double _normBeta  = 0.0;
    double _normK     = 0.0;
};

struct mlo_construct_neuron : mlo_construct_direct2D, mlo_construct_activ_lrn_pooling_common
{
    mlo_construct_neuron(int dir) : mlo_construct_direct2D(dir)
    {
        _neuron_type = 0;
        _gamma       = 0;
        _beta        = 1;
        _alpha       = 0;
    }

    inline void setNeuronDescr(int neuron_type, double gamma, double beta, double alpha)
    {
        _neuron_type = neuron_type;
        _gamma       = gamma;
        _beta        = beta;
        _alpha       = alpha;
    }

    inline void getNeuronDescr(int& neuron_type, double& gamma, double& beta, double& alpha) const
    {
        neuron_type = _neuron_type;
        gamma       = _gamma;
        beta        = _beta;
        alpha       = _alpha;
    }

    void mloConstruct();

    protected:
    int mloConstructFwd();
    int mloConstructBwd();
    int _neuron_type;
    double _gamma;
    double _beta;
    double _alpha;
};

#endif
