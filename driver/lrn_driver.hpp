#ifndef GUARD_MLOPEN_LRN_DRIVER_HPP
#define GUARD_MLOPEN_LRN_DRIVER_HPP

#include <cstdlib>
#include <mlopen.h>
#include <CL/cl.h>
#include "driver.hpp"
#include "mloNormHost.hpp"
#include "InputFlags.hpp"
#include "tensor_driver.hpp"
#include <mlopen/tensor.hpp>
#include <vector>
#include <algorithm>
#include <float.h>
#include <memory>
#include <numeric>

template<typename T>
class LRNDriver : public Driver 
{
	public:
	LRNDriver() : Driver() {
		mlopenCreateTensorDescriptor(&inputTensor);
		mlopenCreateTensorDescriptor(&outputTensor);

		mlopenCreateLRNDescriptor(&lrnDesc);
			
		mlopenCreateTensorDescriptor(&dInputTensor);
		mlopenCreateTensorDescriptor(&dOutputTensor);
	}

	int AddCmdLineArgs();
	int ParseCmdLineArgs(int argc, char *argv[]);
	InputFlags & GetInputFlags() { return inflags; }

	int GetandSetData();
	std::vector<int> GetInputTensorLengthsFromCmdLine();

	int SetLRNDescriptorFromCmdLineArgs();

	int AllocateBuffersAndCopy();
	
	int FindForward(size_t &workspace);
	int RunForwardGPU();
	int RunForwardCPU();
	
	int FindBackward();
	int RunBackwardGPU();
	int RunBackwardCPU();
	
	int VerifyBackward();
	int VerifyForward();
	~LRNDriver() {

		mlopenDestroyTensorDescriptor(outputTensor);
		mlopenDestroyTensorDescriptor(inputTensor);

		mlopenDestroyLRNDescriptor(lrnDesc);
	}
		
	private:
	InputFlags inflags;

	mlopenTensorDescriptor_t inputTensor;
	mlopenTensorDescriptor_t outputTensor;

	std::unique_ptr<GPUMem> in_dev;
	std::unique_ptr<GPUMem> out_dev;
	std::unique_ptr<GPUMem> scale_dev;

	std::vector<T> in;
	std::vector<T> out;
	std::vector<T> outhost;
	std::vector<T> scale;
	std::vector<T> scalehost;

	mlopenLRNDescriptor_t lrnDesc;

	mlopenTensorDescriptor_t dInputTensor;
	mlopenTensorDescriptor_t dOutputTensor;	
	std::unique_ptr<GPUMem> din_dev;
	std::unique_ptr<GPUMem> dout_dev;

	std::vector<T> din;
	std::vector<T> dout;
	std::vector<T> dinhost;

};

template<typename T>
int LRNDriver<T>::ParseCmdLineArgs(int argc, char *argv[]) { 
	inflags.Parse(argc, argv); 

	if(inflags.GetValueInt("time") == 1) {
		mlopenEnableProfiling(GetHandle(), true);
	}
	return 0; 
}

template<typename T>
int LRNDriver<T>::GetandSetData() {
	std::vector<int> in_len = GetInputTensorLengthsFromCmdLine();

	SetTensor4d(inputTensor, in_len);
	
	SetLRNDescriptorFromCmdLineArgs();

	SetTensor4d(outputTensor, in_len);
	
	SetTensor4d(dInputTensor, in_len);
	SetTensor4d(dOutputTensor, in_len);
	return(0);
}

template<typename T>
int LRNDriver<T>::AddCmdLineArgs() {
	inflags.AddInputFlag("forw", 'F', "0", "Run only Forward LRN Normalization (Default=0)", "int");
	inflags.AddInputFlag("batchsize", 'n', "100", "Mini-batch size (Default=100)", "int");
	inflags.AddInputFlag("in_channels", 'c', "3", "Number of Input Channels (Default=3)", "int");
	inflags.AddInputFlag("in_h", 'H', "32", "Input Height (Default=32)", "int");
	inflags.AddInputFlag("in_w", 'W', "32", "Input Width (Default=32)", "int");
	inflags.AddInputFlag("lrnN", 'N', "5", " lrnN (Default=5)", "int");
	inflags.AddInputFlag("alpha", 'A', "0.001", "lrn Alpha (Default=0.001)", "double");
	inflags.AddInputFlag("beta", 'B', "0.75", "lrn Beta (Default=0.75)", "double");
	inflags.AddInputFlag("lrnK", 'K', "1.0", "lrnK (Default=1.0)", "double");
	inflags.AddInputFlag("iter", 'i', "10", "Number of Iterations (Default=10)", "int");
	inflags.AddInputFlag("verify", 'V', "1", "Verify Each Layer (Default=1)", "int");
	inflags.AddInputFlag("time", 't', "0", "Time Each Layer (Default=0)", "int");
	inflags.AddInputFlag("back", 'b', "1", "Do Backward Pooling (Default=1)", "int");
	inflags.AddInputFlag("printconv", 'P', "1", "Print Convolution Dimensions (Default=1)", "int");
	inflags.AddInputFlag("mode", 'm', "within", "Pooling Mode (within_channel or cross_channel) (Default=within)", "str");

	return 0;
}

template<typename T>
std::vector<int> LRNDriver<T>::GetInputTensorLengthsFromCmdLine() {
	int in_n = inflags.GetValueInt("batchsize");
	int in_c = inflags.GetValueInt("in_channels");
	int in_h = inflags.GetValueInt("in_h");
	int in_w = inflags.GetValueInt("in_w");

	return std::vector<int> ({in_n, in_c, in_h, in_w});
}

template<typename T>
int LRNDriver<T>::SetLRNDescriptorFromCmdLineArgs() {

	mlopenLRNMode_t mode; 
	int lrnN = inflags.GetValueInt("lrnN");
	double lrnAlpha = inflags.GetValueDouble("alpha");
	double lrnBeta = inflags.GetValueDouble("beta");
	double lrnK = inflags.GetValueDouble("lrnK");
	if((inflags.GetValueStr("mode")) == "within") {
		mode = mlopenLRNWithinChannel;
	}
	else if((inflags.GetValueStr("mode")) == "cross") {
		mode = mlopenLRNCrossChannel;
	}

	mlopenSetLRNDescriptor(lrnDesc,
			mode,
			lrnN,
			lrnAlpha,
			lrnBeta,
			lrnK);
	return(0);
}

template<typename T>
int LRNDriver<T>::AllocateBuffersAndCopy() {
	
	size_t workspaceSize = 0;
	FindForward(workspaceSize);

	size_t in_sz = GetTensorSize(inputTensor); 
	size_t out_sz = GetTensorSize(outputTensor); 

	cl_context ctx;

	cl_command_queue q = GetStream();

	clGetCommandQueueInfo(q, CL_QUEUE_CONTEXT, sizeof(cl_context), &ctx, NULL);

	in_dev = std::unique_ptr<GPUMem>( new GPUMem(ctx, in_sz, sizeof(float)));
	out_dev = std::unique_ptr<GPUMem> (new GPUMem(ctx, out_sz, sizeof(float)));
	
	din_dev = std::unique_ptr<GPUMem>(new GPUMem(ctx, in_sz, sizeof(float)));
	dout_dev = std::unique_ptr<GPUMem>(new GPUMem(ctx, out_sz, sizeof(float)));

	if (inflags.GetValueInt("back") == 1) {
		scale_dev = std::unique_ptr<GPUMem>(new GPUMem(ctx, workspaceSize/sizeof(float), sizeof(float)));
	}

	in = std::vector<float>(in_sz);
	out = std::vector<float>(out_sz, 0);
	scale = std::vector<float>(workspaceSize/sizeof(float), 0);
	outhost = std::vector<float>(out_sz, 0);
	scalehost = std::vector<float>(workspaceSize / sizeof(float), 0);

	din = std::vector<float>(in_sz);
	dout = std::vector<float>(out_sz, 0);
	dinhost = std::vector<float>(in_sz, 0);

	for (int i = 0; i < in_sz; i++) {
		in[i] = rand() * (1.0 / RAND_MAX);
	}

	for (int i = 0; i < out_sz; i++) {
		dout[i] = (double)(rand() * (1.0 / RAND_MAX) - 0.5) * 0.001;
	}

	cl_int status;
	status = in_dev->ToGPU(q, in.data());
	status |= scale_dev->ToGPU(q, scale.data());
	status |= out_dev->ToGPU(q, out.data());

	status = din_dev->ToGPU(q, din.data());
	status |= dout_dev->ToGPU(q, dout.data());

	if(status != CL_SUCCESS) 
		printf("Error copying data to GPU\n");

	return mlopenStatusSuccess;
}

template<typename T>
int LRNDriver<T>::FindForward(size_t &workspaceSize) {

	return mlopenLRNForward(GetHandle(),
			lrnDesc,
			NULL,
			inputTensor,
			NULL,
			NULL,
			outputTensor,
			NULL, 
			inflags.GetValueInt("back"),
			NULL, 
			&workspaceSize);
}

template<typename T>
int LRNDriver<T>::RunForwardGPU() {

	int alpha = 1, beta = 1;

	mlopenLRNForward(GetHandle(), 
			lrnDesc, 
			&alpha,
			inputTensor,
			in_dev->GetMem(),
			&beta,
			outputTensor,
			out_dev->GetMem(),
			inflags.GetValueInt("back"),
			scale_dev->GetMem(),
			NULL);

	if(inflags.GetValueInt("time") == 1) {
		float time = 0.0;
		mlopenGetKernelTime(GetHandle(), &time);
		printf("GPU Kernel Time Forward LRN Elapsed: %f ms\n", time);
	}

	out_dev->FromGPU(GetStream(), out.data());

	if (inflags.GetValueInt("back") == 1) {
		scale_dev->FromGPU(GetStream(), scale.data());
	}

	return mlopenStatusSuccess;
}

template<typename T>
int LRNDriver<T>::RunForwardCPU() {
	int nInStride, cInStride, hInStride, wInStride;
	mlopenGet4dTensorDescriptorStrides(inputTensor, &nInStride, &cInStride, &hInStride, &wInStride);
	int nIn, cIn, hIn, wIn;
	mlopenGet4dTensorDescriptorLengths(inputTensor, &nIn, &cIn, &hIn, &wIn);
	int nOutStride, cOutStride, hOutStride, wOutStride;
	mlopenGet4dTensorDescriptorStrides(outputTensor, &nOutStride, &cOutStride, &hOutStride, &wOutStride);
	int nOut, cOut, hOut, wOut;
	mlopenGet4dTensorDescriptorLengths(outputTensor, &nOut, &cOut, &hOut, &wOut);
	mlopenLRNMode_t	v_mode;
	unsigned int v_lrnN;
	double	v_lrnAlpha;
	double	v_lrnBeta;
	double	v_lrnK;

	mlopenGetLRNDescriptor(lrnDesc,
			&v_mode,
			&v_lrnN,
			&v_lrnAlpha,
			&v_lrnBeta,
			&v_lrnK);

	float alphaoverarea = (v_mode == mlopenLRNCrossChannel) ? v_lrnAlpha / v_lrnN : v_lrnAlpha / (v_lrnN*v_lrnN);

	int pre_pad = (v_lrnN - 1) / 2;
	int pad = v_lrnN - pre_pad - 1;

	int batch_sz = nIn;
	int n_inputs = cIn;
	int bot_height = hIn;
	int bot_width = wIn;
	int bot_stride = hInStride;
	int bot_channel_stride = cInStride;
	int bot_batch_stride = nInStride;

	int n_outputs = cOut;
	int top_height = hOut;
	int top_width = wOut;
	int top_stride = hOutStride;
	int top_channel_stride = cOutStride;
	int	top_batch_stride = nOutStride;

	int top_v_stride = hOutStride;
	int top_v_channel_stride = cOutStride;
	int	top_v_batch_stride = nOutStride;
	int scale_v_stride = top_v_stride;
	int scale_v_channel_stride = top_v_channel_stride;
	int scale_v_batch_stride = top_v_batch_stride;

	mloLRNForwardRunHost<float>(
			inflags.GetValueInt("back"),
			v_mode,
			pad,
			v_lrnN,
			alphaoverarea,
			(float)v_lrnAlpha,
			(float)v_lrnBeta,
			(float)v_lrnK,
			batch_sz,
			n_outputs,
			n_inputs,
			bot_height,
			bot_width,
			bot_stride,
			bot_channel_stride,
			bot_batch_stride,
			top_height,
			top_width,
			top_v_stride,
			top_v_channel_stride,
			top_v_batch_stride,
			scale_v_stride,
			scale_v_channel_stride,
			scale_v_batch_stride,
			in.data(),
			scalehost.data(),
			outhost.data()
				);
	return(0);
}

template<typename T>
int LRNDriver<T>::FindBackward() {
}

template<typename T>
int LRNDriver<T>::RunBackwardGPU() {
	float alpha = 1., beta = 1.;

	mlopenLRNBackward(GetHandle(),
			lrnDesc,
			&alpha,
			outputTensor,
			out_dev->GetMem(),
			dOutputTensor,
			dout_dev->GetMem(),
			inputTensor,
			in_dev->GetMem(),
			&beta,
			dInputTensor,
			din_dev->GetMem(),
			scale_dev->GetMem());
	
	if(inflags.GetValueInt("time") == 1) {
		float time = 0.0;
		mlopenGetKernelTime(GetHandle(), &time);
		printf("GPU Kernel Time Backward LRN Elapsed: %f ms\n", time);
	}

	din_dev->FromGPU(GetStream(), din.data());
	return(0);

}

template<typename T>
int LRNDriver<T>::VerifyForward() {

	RunForwardCPU();

	cl_int status;
	bool match = true;

	const double allowedEps = (1 << 2);
	double max_sqr = 1. / 100000000;
	double max_abs_diff = 1. / 100000000;
	bool get_error_pos = true;

	int nIn, cIn, hIn, wIn;
	mlopenGet4dTensorDescriptorLengths(inputTensor, &nIn, &cIn, &hIn, &wIn);
	int nOutStride, cOutStride, hOutStride, wOutStride;
	mlopenGet4dTensorDescriptorStrides(outputTensor, &nOutStride, &cOutStride, &hOutStride, &wOutStride);
	int nOut, cOut, hOut, wOut;
	mlopenGet4dTensorDescriptorLengths(outputTensor, &nOut, &cOut, &hOut, &wOut);

	int batch_sz = nIn;

	int n_outputs = cOut;
	int top_height = hOut;
	int top_width = wOut;
	int top_stride = hOutStride;
	int top_channel_stride = cOutStride;
	int	top_batch_stride = nOutStride;

	int top_v_stride = hOutStride;
	int top_v_channel_stride = cOutStride;
	int	top_v_batch_stride = nOutStride;


	match = mloVerify<float>(
			batch_sz,
			n_outputs,
			top_height,
			top_width,
			top_v_batch_stride,
			top_v_channel_stride,
			top_v_stride,
			top_batch_stride,
			top_channel_stride,
			top_stride,
			outhost.data(),
			out.data(),
			allowedEps,
			max_abs_diff,
			max_sqr,
			get_error_pos
			);

	if(match) printf("Forward LRN Verifies on CPU and GPU\n");
	return 0;
}

template<typename T>
int LRNDriver<T>::RunBackwardCPU() {
	
	int nInStride, cInStride, hInStride, wInStride;
	mlopenGet4dTensorDescriptorStrides(inputTensor, &nInStride, &cInStride, &hInStride, &wInStride);
	int nIn, cIn, hIn, wIn;
	mlopenGet4dTensorDescriptorLengths(inputTensor, &nIn, &cIn, &hIn, &wIn);
	int nOutStride, cOutStride, hOutStride, wOutStride;
	mlopenGet4dTensorDescriptorStrides(outputTensor, &nOutStride, &cOutStride, &hOutStride, &wOutStride);
	int nOut, cOut, hOut, wOut;
	mlopenGet4dTensorDescriptorLengths(outputTensor, &nOut, &cOut, &hOut, &wOut);

	int ndInStride, cdInStride, hdInStride, wdInStride;
	mlopenGet4dTensorDescriptorStrides(dInputTensor, &ndInStride, &cdInStride, &hdInStride, &wdInStride);
	int ndIn, cdIn, hdIn, wdIn;
	mlopenGet4dTensorDescriptorLengths(dInputTensor, &ndIn, &cdIn, &hdIn, &wdIn);
	int ndOutStride, cdOutStride, hdOutStride, wdOutStride;
	mlopenGet4dTensorDescriptorStrides(dOutputTensor, &ndOutStride, &cdOutStride, &hdOutStride, &wdOutStride);
	int ndOut, cdOut, hdOut, wdOut;
	mlopenGet4dTensorDescriptorLengths(dOutputTensor, &ndOut, &cdOut, &hdOut, &wdOut);

	mlopenLRNMode_t	v_mode;
	unsigned int v_lrnN;
	double	v_lrnAlpha;
	double	v_lrnBeta;
	double	v_lrnK;

	mlopenGetLRNDescriptor(lrnDesc,
			&v_mode,
			&v_lrnN,
			&v_lrnAlpha,
			&v_lrnBeta,
			&v_lrnK);

	float alphaoverarea = (v_mode == mlopenLRNCrossChannel) ? v_lrnAlpha / v_lrnN : v_lrnAlpha / (v_lrnN*v_lrnN);

	int pre_pad = (v_lrnN - 1) / 2;
	int pad = v_lrnN - pre_pad - 1;

	int batch_sz = nIn;
	int n_inputs = cIn;
	int bot_height = hIn;
	int bot_width = wIn;
	int bot_stride = hInStride;
	int bot_channel_stride = cInStride;
	int bot_batch_stride = nInStride;

	int bot_df_stride = hdInStride;
	int bot_df_channel_stride = cdInStride;
	int bot_df_batch_stride = ndInStride;

	int bot_df_v_stride = hdInStride;
	int bot_df_v_channel_stride = cdInStride;
	int bot_df_v_batch_stride = ndInStride;

	int n_outputs = cOut;
	int top_height = hOut;
	int top_width = wOut;
	int top_stride = hOutStride;
	int top_channel_stride = cOutStride;
	int	top_batch_stride = nOutStride;

	int top_df_stride = hdOutStride;
	int top_df_channel_stride = cdOutStride;
	int	top_df_batch_stride = ndOutStride;

	int scale_stride = top_df_stride;
	int scale_channel_stride = top_df_channel_stride;
	int scale_batch_stride = top_df_batch_stride;

	cl_int status;
	status = mloLRNBackwardRunHost<float>(
			(int)v_mode,
			pad,
			v_lrnN,
			alphaoverarea,
			(float)v_lrnAlpha,
			(float)v_lrnBeta,
			(float)v_lrnK,
			batch_sz,
			n_outputs,
			n_inputs,
			bot_height,
			bot_width,
			bot_stride,
			bot_channel_stride,
			bot_batch_stride,
			bot_df_v_stride,
			bot_df_v_channel_stride,
			bot_df_v_batch_stride,
			top_height,
			top_width,
			top_stride,
			top_channel_stride,
			top_batch_stride,
			top_df_stride,
			top_df_channel_stride,
			top_df_batch_stride,
			scale_stride,
			scale_channel_stride,
			scale_batch_stride,
			out.data(),
			dout.data(),
			scale.data(),
			in.data(),
			dinhost.data()
				);

	return 0;
}

template<typename T>
int LRNDriver<T>::VerifyBackward() {

	RunBackwardCPU();
	
	bool match = true;

	double sqr_accum = 0;
	double max_err = -std::numeric_limits<double>::min();
	double allowedEps = 4;
	double max_abs_diff = 0.00000001;
	double max_sqr = 0.000000001;
	bool get_error_pos = true;

	int nIn, cIn, hIn, wIn;
	mlopenGet4dTensorDescriptorLengths(inputTensor, &nIn, &cIn, &hIn, &wIn);

	int ndInStride, cdInStride, hdInStride, wdInStride;
	mlopenGet4dTensorDescriptorStrides(dInputTensor, &ndInStride, &cdInStride, &hdInStride, &wdInStride);

	int batch_sz = nIn;
	int n_inputs = cIn;
	int bot_height = hIn;
	int bot_width = wIn;

	int bot_df_stride = hdInStride;
	int bot_df_channel_stride = cdInStride;
	int bot_df_batch_stride = ndInStride;

	int bot_df_v_stride = hdInStride;
	int bot_df_v_channel_stride = cdInStride;
	int bot_df_v_batch_stride = ndInStride;

	match = mloVerify<float>(
			batch_sz,
			n_inputs,
			bot_height,
			bot_width,
			bot_df_v_batch_stride,
			bot_df_v_channel_stride,
			bot_df_v_stride,
			bot_df_batch_stride,
			bot_df_channel_stride,
			bot_df_stride,
			dinhost.data(),
			din.data(),
			allowedEps,
			max_abs_diff,
			max_sqr,
			get_error_pos
			);

	if(match) printf("Backward LRN Verifies on CPU and GPU\n");
	return 0;
}

#endif // GUARD_MLOPEN_CONV_DRIVER_HPP