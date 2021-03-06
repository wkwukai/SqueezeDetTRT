#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <sstream>
#include <iostream>
#include <memory>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <err.h>
#include <opencv2/opencv.hpp>
#include <cuda_runtime.h>

// #define _GNU_SOURCE
#include <getopt.h>

#include "common.h"
#include "tensorUtil.h"
#include "trtUtil.h"
#include "sdt_alloc.h"

static Logger gLogger;

static const int INPUT_N = 1;   // one image at a time
static const int INPUT_C = 3;
static const int INPUT_H = 384;
static const int INPUT_W = 1248;

static const int CONVOUT_C = 72;
static const int CONVOUT_H = 24;
static const int CONVOUT_W = 78;

static const int CLASS_SLICE_C = 27;
static const int CONF_SLICE_C = 9;
static const int BBOX_SLICE_C = 36;

static const int OUTPUT_CLS_SIZE = 3;
static const int OUTPUT_BBOX_SIZE = 4;

static const int TOP_N_DETECTION = 64;
static const float NMS_THRESH = 0.4;
// static const float PROB_THRESH = 0.005;
static const float PROB_THRESH = 0.3;
static const float PLOT_PROB_THRESH = 0.4;
// static const float EPSILON = 1e-16;

static const char* INPUT_NAME = "data";
static const char* CONVOUT_NAME = "conv_out";
static const char* CLASS_INPUT_NAME = "class_slice";
static const char* CONF_INPUT_NAME = "confidence_slice";
// static const char* BBOX_INPUT_NAME = "bbox_slice";
static const char* CLASS_OUTPUT_NAME = "pred_class_probs";
static const char* CONF_OUTPUT_NAME = "pred_confidence_score";
// static const char* BBOX_OUTPUT_NAME = "bbox_delta";

static const int ANCHORS_PER_GRID = 9;
static const int ANCHOR_SIZE = 4;
static const float ANCHOR_SHAPE[] = {36, 37, 366, 174, 115, 59, /* w x h, 2 elements one group*/
                              162, 87, 38, 90, 258, 173,
                              224, 108, 78, 170, 72, 43};

static const char *CLASS_NAMES[] = {"car", "pedestrian", "cyclist"};

// pixel mean used by the SqueezeDet's author
static const float PIXEL_MEAN[3]{ 103.939f, 116.779f, 123.68f }; // in BGR order

static const double DEFAULT_FPS = 10;

struct predictions {
     float *klass;
     float *prob;
     float *bbox;
     int *keep;
     int num;
};

static int anchorsNum;
static int inputIndex, convoutIndex, classInputIndex, confInputIndex, classOutputIndex, confOutputIndex;

// device buffers
static void *convBuffers[2], *interpretBuffers[4];
static float *bboxInput; // don't need to go into interpret engine
static int *transAxesDevice;
static Tensor *convoutTensor;
static Tensor *classInputTensor;
static Tensor *confInputTensor;
static Tensor *bboxInputTensor;
static Tensor *classOutputTensor;
static Tensor *confOutputTensor;
static Tensor *bboxOutputTensor;
static Tensor *classTransTensor;
static Tensor *confTransTensor;
static Tensor *bboxTransTensor;
static int *classTransWorkspace[2], *confTransWorkspace[2], *bboxTransWorkspace[2];
static float *anchorsDevice;
static Tensor *reduceMaxResTensor;
static Tensor *reduceArgResTensor;
static Tensor *mulResTensor;
static Tensor *bboxResTensor;
static Tensor *anchorsDeviceTensor;
static int *orderDevice, *orderDeviceTmp; // for top-n-detecion
static Tensor *finalClassTensor;
static Tensor *finalProbsTensor;
static Tensor *finalBboxTensor;
static cudaStream_t stream;
static cudaEvent_t start_imread, stop_imread, start_detect, stop_detect, start_misc, stop_misc;
static float timeDetect, timeImread, timeMisc;

std::string locateFile(const std::string& input)
{
     std::vector<std::string> dirs{"data/"};
     return locateFile(input, dirs);
}

ILayer*
addFireLayer(INetworkDefinition* network, ITensor &input, int ns1x1, int ne1x1, int ne3x3,
             Weights &wks1x1, Weights &wke1x1, Weights &wke3x3,
             Weights &wbs1x1, Weights &wbe1x1, Weights &wbe3x3)
{
     auto sq1x1 = network->addConvolution(input, ns1x1, DimsHW{1, 1}, wks1x1, wbs1x1);
     assert(sq1x1 != nullptr);
     sq1x1->setStride(DimsHW{1, 1});
     auto relu1 = network->addActivation(*sq1x1->getOutput(0), ActivationType::kRELU);
     assert(relu1 != nullptr);

     auto ex1x1 = network->addConvolution(*relu1->getOutput(0) , ne1x1, DimsHW{1, 1}, wke1x1, wbe1x1);
     assert(ex1x1 != nullptr);
     ex1x1->setStride(DimsHW{1, 1});
     auto relu2 = network->addActivation(*ex1x1->getOutput(0), ActivationType::kRELU);
     assert(relu2 != nullptr);

     auto ex3x3 = network->addConvolution(*relu1->getOutput(0), ne3x3, DimsHW{3, 3}, wke3x3, wbe3x3);
     assert(ex3x3 != nullptr);
     ex3x3->setStride(DimsHW{1, 1});
     ex3x3->setPadding(DimsHW{1, 1});
     auto relu3 = network->addActivation(*ex3x3->getOutput(0), ActivationType::kRELU);
     assert(relu3 != nullptr);

     ITensor *concatTensors[] = {relu2->getOutput(0), relu3->getOutput(0)};
     auto concat = network->addConcatenation(concatTensors, 2);
     assert(concat != nullptr);

     return concat;
}

// Creat the Engine using only the API and not any parser.
ICudaEngine *
createConvEngine(unsigned int maxBatchSize, IBuilder *builder, DataType dt)
{
     INetworkDefinition* network = builder->createNetwork();

     auto data = network->addInput(INPUT_NAME, dt, DimsCHW{INPUT_C, INPUT_H, INPUT_W});
     assert(data != nullptr);

     std::map<std::string, Weights> weightMap = loadWeights(locateFile("sqdtrt.wts"));
     auto conv1 = network->addConvolution(*data, 64, DimsHW{3, 3},
                                          weightMap["conv1_kernels"],
                                          weightMap["conv1_bias"]);
     assert(conv1 != nullptr);
     conv1->setStride(DimsHW{2, 2});
     conv1->setPadding(DimsHW{1, 1}); // all kernels of size 3x3 need to set padding 1x1
     auto relu1 = network->addActivation(*conv1->getOutput(0), ActivationType::kRELU);
     assert(relu1 != nullptr);

     auto pool1 = network->addPooling(*relu1->getOutput(0), PoolingType::kMAX, DimsHW{3, 3});
     assert(pool1 != nullptr);
     pool1->setStride(DimsHW{2, 2});
     pool1->setPadding(DimsHW{1, 1});

     auto fire2 = addFireLayer(network, *pool1->getOutput(0), 16, 64, 64,
                               weightMap["fire2_squeeze1x1_kernels"],
                               weightMap["fire2_expand1x1_kernels"],
                               weightMap["fire2_expand3x3_kernels"],
                               weightMap["fire2_squeeze1x1_biases"],
                               weightMap["fire2_expand1x1_biases"],
                               weightMap["fire2_expand3x3_biases"]);
     auto fire3 = addFireLayer(network, *fire2->getOutput(0), 16, 64, 64,
                               weightMap["fire3_squeeze1x1_kernels"],
                               weightMap["fire3_expand1x1_kernels"],
                               weightMap["fire3_expand3x3_kernels"],
                               weightMap["fire3_squeeze1x1_biases"],
                               weightMap["fire3_expand1x1_biases"],
                               weightMap["fire3_expand3x3_biases"]);

     auto pool3 = network->addPooling(*fire3->getOutput(0), PoolingType::kMAX, DimsHW{3, 3});
     assert(pool3 != nullptr);
     pool3->setStride(DimsHW{2, 2});
     pool3->setPadding(DimsHW{1, 1});

     auto fire4 = addFireLayer(network, *pool3->getOutput(0), 32, 128, 128,
                               weightMap["fire4_squeeze1x1_kernels"],
                               weightMap["fire4_expand1x1_kernels"],
                               weightMap["fire4_expand3x3_kernels"],
                               weightMap["fire4_squeeze1x1_biases"],
                               weightMap["fire4_expand1x1_biases"],
                               weightMap["fire4_expand3x3_biases"]);
     auto fire5 = addFireLayer(network, *fire4->getOutput(0), 32, 128, 128,
                               weightMap["fire5_squeeze1x1_kernels"],
                               weightMap["fire5_expand1x1_kernels"],
                               weightMap["fire5_expand3x3_kernels"],
                               weightMap["fire5_squeeze1x1_biases"],
                               weightMap["fire5_expand1x1_biases"],
                               weightMap["fire5_expand3x3_biases"]);

     auto pool5 = network->addPooling(*fire5->getOutput(0), PoolingType::kMAX, DimsHW{3, 3});
     assert(pool5 != nullptr);
     pool5->setStride(DimsHW{2, 2});
     pool5->setPadding(DimsHW{1, 1});

     auto fire6 = addFireLayer(network, *pool5->getOutput(0), 48, 192, 192,
                               weightMap["fire6_squeeze1x1_kernels"],
                               weightMap["fire6_expand1x1_kernels"],
                               weightMap["fire6_expand3x3_kernels"],
                               weightMap["fire6_squeeze1x1_biases"],
                               weightMap["fire6_expand1x1_biases"],
                               weightMap["fire6_expand3x3_biases"]);
     auto fire7 = addFireLayer(network, *fire6->getOutput(0), 48, 192, 192,
                               weightMap["fire7_squeeze1x1_kernels"],
                               weightMap["fire7_expand1x1_kernels"],
                               weightMap["fire7_expand3x3_kernels"],
                               weightMap["fire7_squeeze1x1_biases"],
                               weightMap["fire7_expand1x1_biases"],
                               weightMap["fire7_expand3x3_biases"]);
     auto fire8 = addFireLayer(network, *fire7->getOutput(0), 64, 256, 256,
                               weightMap["fire8_squeeze1x1_kernels"],
                               weightMap["fire8_expand1x1_kernels"],
                               weightMap["fire8_expand3x3_kernels"],
                               weightMap["fire8_squeeze1x1_biases"],
                               weightMap["fire8_expand1x1_biases"],
                               weightMap["fire8_expand3x3_biases"]);
     auto fire9 = addFireLayer(network, *fire8->getOutput(0), 64, 256, 256,
                               weightMap["fire9_squeeze1x1_kernels"],
                               weightMap["fire9_expand1x1_kernels"],
                               weightMap["fire9_expand3x3_kernels"],
                               weightMap["fire9_squeeze1x1_biases"],
                               weightMap["fire9_expand1x1_biases"],
                               weightMap["fire9_expand3x3_biases"]);

     auto fire10 = addFireLayer(network, *fire9->getOutput(0), 96, 384, 384,
                                weightMap["fire10_squeeze1x1_kernels"],
                                weightMap["fire10_expand1x1_kernels"],
                                weightMap["fire10_expand3x3_kernels"],
                                weightMap["fire10_squeeze1x1_biases"],
                                weightMap["fire10_expand1x1_biases"],
                                weightMap["fire10_expand3x3_biases"]);
     auto fire11 = addFireLayer(network, *fire10->getOutput(0), 96, 384, 384,
                                weightMap["fire11_squeeze1x1_kernels"],
                                weightMap["fire11_expand1x1_kernels"],
                                weightMap["fire11_expand3x3_kernels"],
                                weightMap["fire11_squeeze1x1_biases"],
                                weightMap["fire11_expand1x1_biases"],
                                weightMap["fire11_expand3x3_biases"]);

     // TODO: add dropout11
     // not need any more, because dropout probability is 1.0 during evaluation
     // ILayer *dropout11 = fire11;

     auto preds = network->addConvolution(*fire11->getOutput(0), CONVOUT_C, DimsHW{3, 3},
                                          weightMap["conv12_kernels"],
                                          weightMap["conv12_biases"]);
     assert(preds != nullptr);
     preds->setStride(DimsHW{1, 1}); // what is xavier, stddev?
     preds->setPadding(DimsHW{1, 1});

     preds->getOutput(0)->setName(CONVOUT_NAME);
     network->markOutput(*preds->getOutput(0));

     // Build the engine
     builder->setMaxBatchSize(maxBatchSize);
     builder->setMaxWorkspaceSize(1 << 20);

     auto engine = builder->buildCudaEngine(*network);
     // we don't need the network any more
     // network->destroy();	// SIGSEGV, don't know why

     // Once we have built the cuda engine, we can release all of our held memory.
     for (auto &mem : weightMap)
     {
          sdt_free((void*)(mem.second.values));
     }
     return engine;
}

ICudaEngine *
createInterpretEngine(unsigned int maxBatchSize, IBuilder *builder, DataType dt)
{
     INetworkDefinition* network = builder->createNetwork();

     auto class_tensor = network->addInput(CLASS_INPUT_NAME, dt, DimsNCHW{ANCHORS_PER_GRID, OUTPUT_CLS_SIZE, CONVOUT_H, CONVOUT_W});
     assert(class_tensor != nullptr);
     // auto confidence_tensor = network->addInput(CONF_INPUT_NAME, dt, DimsNCHW{INPUT_N, 1, 1, CONVOUT_W * CONVOUT_H * ANCHORS_PER_GRID});
     auto confidence_tensor = network->addInput(CONF_INPUT_NAME, dt, DimsCHW{1, 1, INPUT_N * CONVOUT_W * CONVOUT_H * ANCHORS_PER_GRID});
     assert(confidence_tensor != nullptr);

     auto class_softmax = network->addSoftMax(*class_tensor);
     assert(class_softmax != nullptr);
     auto pred_conf = network->addActivation(*confidence_tensor, ActivationType::kSIGMOID);
     assert(pred_conf != nullptr);

     class_softmax->getOutput(0)->setName(CLASS_OUTPUT_NAME);
     pred_conf->getOutput(0)->setName(CONF_OUTPUT_NAME);
     network->markOutput(*class_softmax->getOutput(0));
     network->markOutput(*pred_conf->getOutput(0));

     // Build the engine
     builder->setMaxBatchSize(maxBatchSize);
     builder->setMaxWorkspaceSize(1 << 20);

     auto engine = builder->buildCudaEngine(*network);
     // we don't need the network any more
     // network->destroy(); // SIGSEGV, don't know why

     return engine;
}

// maxBatch - NB must be at least as large as the batch we want to run with)
void APIToModel(unsigned int maxBatchSize, IHostMemory **convModelStream, IHostMemory **interpretModelStream)
{
     // create the builder
     IBuilder* builder = createInferBuilder(gLogger);

     // create the model to populate the network, then set the outputs and create an engine
     ICudaEngine* convEngine = createConvEngine(maxBatchSize, builder, DataType::kFLOAT);
     ICudaEngine* interpretEngine = createInterpretEngine(maxBatchSize, builder, DataType::kFLOAT);

     assert(convEngine != nullptr);
     assert(interpretEngine != nullptr);

     // serialize the engine, then close everything down
     (*convModelStream) = convEngine->serialize();
     (*interpretModelStream) = interpretEngine->serialize();
     convEngine->destroy();
     interpretEngine->destroy();
     builder->destroy();
}

// batch size is 1
void setUpDevice(IExecutionContext *convContext, IExecutionContext *interpretContext, float* anchors, int batchSize)
{
     const ICudaEngine &convEngine = convContext->getEngine();
     const ICudaEngine &interpretEngine = interpretContext->getEngine();

     assert(convEngine.getNbBindings() == 2);
     assert(interpretEngine.getNbBindings() == 4);

     // In order to bind the buffers, we need to know the names of the input and output tensors.
     // note that indices are guaranteed to be less than IEngine::getNbBindings()
     inputIndex = convEngine.getBindingIndex(INPUT_NAME);
     convoutIndex = convEngine.getBindingIndex(CONVOUT_NAME);
     classInputIndex = interpretEngine.getBindingIndex(CLASS_INPUT_NAME);
     confInputIndex = interpretEngine.getBindingIndex(CONF_INPUT_NAME);
     classOutputIndex = interpretEngine.getBindingIndex(CLASS_OUTPUT_NAME);
     confOutputIndex = interpretEngine.getBindingIndex(CONF_OUTPUT_NAME);

     // create GPU buffers and a stream
     anchorsNum = batchSize * CONVOUT_W * CONVOUT_H * ANCHORS_PER_GRID;
     size_t inputSize = batchSize * INPUT_C * INPUT_H * INPUT_W * sizeof(float);
     size_t convoutSize = batchSize * CONVOUT_H * CONVOUT_W * CONVOUT_C * sizeof(float);
     size_t classInputSize = batchSize * CONVOUT_H * CONVOUT_W * CLASS_SLICE_C * sizeof(float);
     size_t confInputSize = batchSize * CONVOUT_H * CONVOUT_W * CONF_SLICE_C * sizeof(float);
     size_t bboxInputSize = batchSize * CONVOUT_H * CONVOUT_W * BBOX_SLICE_C * sizeof(float);
     size_t classOutputSize = batchSize * OUTPUT_CLS_SIZE * anchorsNum * sizeof(float);
     size_t confOutputSize = anchorsNum * sizeof(float);
     CHECK(cudaMalloc(&convBuffers[inputIndex], inputSize));
     CHECK(cudaMalloc(&convBuffers[convoutIndex], convoutSize));
     CHECK(cudaMalloc(&interpretBuffers[classInputIndex], classInputSize));
     CHECK(cudaMalloc(&interpretBuffers[confInputIndex], confInputSize));
     CHECK(cudaMalloc(&bboxInput, bboxInputSize));
     CHECK(cudaMalloc(&interpretBuffers[classOutputIndex], classOutputSize));
     CHECK(cudaMalloc(&interpretBuffers[confOutputIndex], confOutputSize));

     int convout_dims[] = {batchSize, CONVOUT_C, CONVOUT_H, CONVOUT_W};
     int classInputDims[] = {batchSize, CLASS_SLICE_C, CONVOUT_H, CONVOUT_W};
     int confInputDims[] = {batchSize, CONF_SLICE_C, CONVOUT_H, CONVOUT_W};
     int bboxInputDims[] = {batchSize, BBOX_SLICE_C, CONVOUT_H, CONVOUT_W};
     int classOutputDims[] = {batchSize, ANCHORS_PER_GRID, OUTPUT_CLS_SIZE, CONVOUT_H, CONVOUT_W};
     int classTransDims[] = {batchSize, CONVOUT_H, CONVOUT_W, ANCHORS_PER_GRID, OUTPUT_CLS_SIZE};
     int confOutputDims[] = {batchSize, ANCHORS_PER_GRID, 1, CONVOUT_H, CONVOUT_W};
     int confTransDims[] = {batchSize, CONVOUT_H, CONVOUT_W, ANCHORS_PER_GRID, 1};
     int bboxOutputDims[] = {batchSize, ANCHORS_PER_GRID, OUTPUT_BBOX_SIZE, CONVOUT_H, CONVOUT_W};
     int bboxTransDims[] = {batchSize, CONVOUT_H, CONVOUT_W, ANCHORS_PER_GRID, OUTPUT_BBOX_SIZE};
     int transAxes[] = {0, 3, 4, 1, 2};
     transAxesDevice = (int *)cloneMem(transAxes, sizeof(int) * 5, H2D);
     convoutTensor = createTensor((float *)convBuffers[convoutIndex], 4, convout_dims);
     classInputTensor = createTensor((float *)interpretBuffers[classInputIndex], 4, classInputDims);
     confInputTensor = createTensor((float *)interpretBuffers[confInputIndex], 4, confInputDims);
     bboxInputTensor = createTensor(bboxInput, 4, bboxInputDims);
     classOutputTensor = createTensor((float *)interpretBuffers[classOutputIndex], 5, classOutputDims);
     confOutputTensor = createTensor((float *)interpretBuffers[confOutputIndex], 5, confOutputDims);
     bboxOutputTensor = reshapeTensor(bboxInputTensor, 5, bboxOutputDims);
     classTransTensor = mallocTensor(5, classTransDims, DEVICE);
     confTransTensor = mallocTensor(5, confTransDims, DEVICE);
     bboxTransTensor = mallocTensor(5, bboxTransDims, DEVICE);
     CHECK(cudaMalloc(&classTransWorkspace[0], sizeof(int) * classTransTensor->ndim * classTransTensor->len));
     CHECK(cudaMalloc(&classTransWorkspace[1], sizeof(int) * classTransTensor->ndim * classTransTensor->len));
     CHECK(cudaMalloc(&confTransWorkspace[0], sizeof(int) * confTransTensor->ndim * confTransTensor->len));
     CHECK(cudaMalloc(&confTransWorkspace[1], sizeof(int) * confTransTensor->ndim * confTransTensor->len));
     CHECK(cudaMalloc(&bboxTransWorkspace[0], sizeof(int) * bboxTransTensor->ndim * bboxTransTensor->len));
     CHECK(cudaMalloc(&bboxTransWorkspace[1], sizeof(int) * bboxTransTensor->ndim * bboxTransTensor->len));

     size_t anchorsDeviceSize = anchorsNum * ANCHOR_SIZE * sizeof(float);
     anchorsDevice = (float *)cloneMem(anchors, anchorsDeviceSize, H2D);

     int reduceMaxResDims[] = {batchSize, CONVOUT_H, CONVOUT_W, ANCHORS_PER_GRID, 1};
     int reduceArgResDims[] = {batchSize, CONVOUT_H, CONVOUT_W, ANCHORS_PER_GRID, 1};
     int mulResDims[] = {batchSize, CONVOUT_H, CONVOUT_W, ANCHORS_PER_GRID, 1};
     int bboxResDims[] = {batchSize, CONVOUT_H, CONVOUT_W, ANCHORS_PER_GRID, OUTPUT_BBOX_SIZE};
     // int anchorsDeviceDims[] = {batchSize, ANCHORS_PER_GRID, ANCHOR_SIZE, CONVOUT_H, CONVOUT_W};
     int anchorsDeviceDims[] = {batchSize, CONVOUT_H, CONVOUT_W, ANCHORS_PER_GRID, ANCHOR_SIZE};
     reduceMaxResTensor = mallocTensor(5, reduceMaxResDims, DEVICE);
     reduceArgResTensor = mallocTensor(5, reduceArgResDims, DEVICE);
     mulResTensor = mallocTensor(5, mulResDims, DEVICE);
     bboxResTensor = mallocTensor(5, bboxResDims, DEVICE);
     anchorsDeviceTensor = createTensor(anchorsDevice, 5, anchorsDeviceDims);

     int *orderHost = (int *)sdt_alloc(anchorsNum * sizeof(int));
     for (int i = 0; i < anchorsNum; i++)
          orderHost[i] = i;
     orderDevice = (int *)cloneMem(orderHost, anchorsNum * sizeof(int), H2D);
     orderDeviceTmp = (int *)cloneMem(orderHost, anchorsNum * sizeof(int), H2D);
     sdt_free(orderHost);

     int finalProbsDims[] = {batchSize, TOP_N_DETECTION, 1};
     int finalClassDims[] = {batchSize, TOP_N_DETECTION, 1};
     int finalBboxDims[] = {batchSize, TOP_N_DETECTION, OUTPUT_BBOX_SIZE};
     // finalProbsTensor shares data with mulResTensor
     finalProbsTensor = createTensor(mulResTensor->data, 3, finalProbsDims);
     // finalProbsTensor = mallocTensor(3, finalProbsDims, DEVICE);
     finalClassTensor = mallocTensor(3, finalClassDims, DEVICE);
     finalBboxTensor = mallocTensor(3, finalBboxDims, DEVICE);

     CHECK(cudaStreamCreate(&stream));
     CHECK(cudaEventCreate(&start_imread));
     CHECK(cudaEventCreate(&stop_imread));
     CHECK(cudaEventCreate(&start_detect));
     CHECK(cudaEventCreate(&stop_detect));
     CHECK(cudaEventCreate(&start_misc));
     CHECK(cudaEventCreate(&stop_misc));
}

// batch size is 1
void doInference(IExecutionContext *convContext, IExecutionContext *interpretContext, float* input, int inputSize, int img_width, int img_height, int x_shift, int y_shift, struct predictions *preds, int batchSize)
{
     CHECK(cudaEventRecord(start_detect, 0));

     // DMA the input to the GPU,  execute the batch asynchronously, and DMA it back:
     CHECK(cudaMemcpyAsync(convBuffers[inputIndex], input, inputSize, cudaMemcpyHostToDevice, stream));

     convContext->enqueue(batchSize, convBuffers, stream, nullptr);
     sliceTensor(convoutTensor, classInputTensor, 1, 0, CLASS_SLICE_C);
     sliceTensor(convoutTensor, confInputTensor, 1, CLASS_SLICE_C, CONF_SLICE_C);
     sliceTensor(convoutTensor, bboxInputTensor, 1, CLASS_SLICE_C + CONF_SLICE_C, BBOX_SLICE_C);
     interpretContext->enqueue(batchSize, interpretBuffers, stream, nullptr);
     transposeTensor(classOutputTensor, classTransTensor, transAxesDevice, classTransWorkspace);
     transposeTensor(confOutputTensor, confTransTensor, transAxesDevice, confTransWorkspace);
     transposeTensor(bboxOutputTensor, bboxTransTensor, transAxesDevice, bboxTransWorkspace);
     reduceArgMax(classTransTensor, reduceMaxResTensor, reduceArgResTensor, 4);
     multiplyElement(reduceMaxResTensor, confTransTensor, mulResTensor);
     transformBboxSQD(bboxTransTensor, anchorsDeviceTensor, bboxResTensor, INPUT_W, INPUT_H, img_width, img_height, x_shift, y_shift);

     CHECK(cudaEventRecord(stop_detect, 0));
     CHECK(cudaEventSynchronize(stop_detect));
     CHECK(cudaEventElapsedTime(&timeDetect, start_detect, stop_detect));

#ifdef DEBUG
     saveDeviceTensor("data/convoutTensor.txt", convoutTensor, "%15.6e");
     saveDeviceTensor("data/classInputTensor.txt", classInputTensor, "%15.6e");
     saveDeviceTensor("data/confInputTensor.txt", confInputTensor, "%15.6e");
     saveDeviceTensor("data/bboxInputTensor.txt", bboxInputTensor, "%15.6e");
     saveDeviceTensor("data/classOutputTensor.txt", classOutputTensor, "%15.6e");
     saveDeviceTensor("data/confOutputTensor.txt", confOutputTensor, "%15.6e");
     saveDeviceTensor("data/bboxOutputTensor.txt", bboxOutputTensor, "%15.6e");
     saveDeviceTensor("data/mulResTensor.txt", mulResTensor, "%15.6e");
     saveDeviceTensor("data/reduceMaxResTensor.txt", reduceMaxResTensor, "%15.6e");
     saveDeviceTensor("data/reduceArgResTensor.txt", reduceArgResTensor, "%15.6e");
     saveDeviceTensor("data/bboxResTensor.txt", bboxResTensor, "%15.6e");
     saveDeviceTensor("data/confTransTensor.txt", confTransTensor, "%15.6e");
     saveDeviceTensor("data/classTransTensor.txt", classTransTensor, "%15.6e");
     saveDeviceTensor("data/bboxTransTensor.txt", bboxTransTensor, "%15.6e");
     saveDeviceTensor("data/anchorsDeviceTensor.txt", anchorsDeviceTensor, "%15.6e");

     int classInputDims2[] = {batchSize, 9, 3, CONVOUT_W*CONVOUT_H};
     int classInputDims3[] = {batchSize, 3, 9, CONVOUT_W*CONVOUT_H};
     Tensor *classInput2 = reshapeTensor(classInputTensor, 4, classInputDims2);
     Tensor *classInput3 = reshapeTensor(classInputTensor, 4, classInputDims3);
     saveDeviceTensor("data/classInputDims2.txt", classInput2, "%15.6e");
     saveDeviceTensor("data/classInputDims3.txt", classInput3, "%15.6e");
#endif
     // filter top-n-detection
     CHECK(cudaEventRecord(start_misc, 0));
     CHECK(cudaMemcpy(orderDeviceTmp, orderDevice, anchorsNum * sizeof(int), cudaMemcpyDeviceToDevice));
     tensorIndexSort(mulResTensor, orderDeviceTmp);
     // already sort mulResTensor (sharing data with finalProbsTensor), so we can skip this
     // pickElements(mulResTensor->data, finalProbsTensor->data, 1, orderDeviceTmp, TOP_N_DETECTION);
     pickElements(reduceArgResTensor->data, finalClassTensor->data, 1, orderDeviceTmp, TOP_N_DETECTION);
     pickElements(bboxResTensor->data, finalBboxTensor->data, OUTPUT_BBOX_SIZE, orderDeviceTmp, TOP_N_DETECTION);

#ifdef DEBUG
     FILE * sort_file = fopen("data/orderDevice.txt", "w");
     int *orderHost2 = (int *)cloneMem(orderDevice, anchorsNum * sizeof(int), D2H);
     for (int i = 0; i < anchorsNum; i++)
          fprintf(sort_file, "%d\n", orderHost2[i]);
     fclose(sort_file);
     sdt_free(orderHost2);
     saveDeviceTensor("data/finalClassTensor.txt", finalClassTensor, "%15.6e");
     saveDeviceTensor("data/finalProbsTensor.txt", finalProbsTensor, "%15.6e");
     saveDeviceTensor("data/finalBboxTensor.txt", finalBboxTensor, "%15.6e");
#endif

     CHECK(cudaMemcpyAsync(preds->prob, finalProbsTensor->data, finalProbsTensor->len*sizeof(float), cudaMemcpyDeviceToHost, stream));
     CHECK(cudaMemcpyAsync(preds->klass, finalClassTensor->data, finalClassTensor->len*sizeof(float), cudaMemcpyDeviceToHost, stream));
     CHECK(cudaMemcpyAsync(preds->bbox, finalBboxTensor->data, finalBboxTensor->len*sizeof(float), cudaMemcpyDeviceToHost, stream));
     CHECK(cudaStreamSynchronize(stream));
}

void cleanUp()
{
     // timer destroy
     CHECK(cudaEventDestroy(start_imread));
     CHECK(cudaEventDestroy(stop_imread));
     CHECK(cudaEventDestroy(start_detect));
     CHECK(cudaEventDestroy(stop_detect));
     CHECK(cudaEventDestroy(start_misc));
     CHECK(cudaEventDestroy(stop_misc));

     // release the stream and the buffers
     CHECK(cudaStreamDestroy(stream));
     CHECK(cudaFree(convBuffers[inputIndex]));
     CHECK(cudaFree(convBuffers[convoutIndex]));
     CHECK(cudaFree(interpretBuffers[classInputIndex]));
     CHECK(cudaFree(interpretBuffers[confInputIndex]));
     CHECK(cudaFree(bboxInput));
     CHECK(cudaFree(interpretBuffers[classOutputIndex]));
     CHECK(cudaFree(interpretBuffers[confOutputIndex]));
     CHECK(cudaFree(classTransTensor->data));
     CHECK(cudaFree(confTransTensor->data));
     CHECK(cudaFree(bboxTransTensor->data));
     CHECK(cudaFree(classTransWorkspace[0]));
     CHECK(cudaFree(classTransWorkspace[1]));
     CHECK(cudaFree(confTransWorkspace[0]));
     CHECK(cudaFree(confTransWorkspace[1]));
     CHECK(cudaFree(bboxTransWorkspace[0]));
     CHECK(cudaFree(bboxTransWorkspace[1]));
     CHECK(cudaFree(reduceMaxResTensor->data));
     CHECK(cudaFree(reduceArgResTensor->data));
     CHECK(cudaFree(mulResTensor->data));
     CHECK(cudaFree(bboxResTensor->data));
     CHECK(cudaFree(transAxesDevice));
     CHECK(cudaFree(anchorsDevice));
     CHECK(cudaFree(orderDevice));
     CHECK(cudaFree(orderDeviceTmp));
     // already freed mulResTensor data (sharing data with finalProbsTensor), so skip this
     // CHECK(cudaFree(finalProbsTensor->data));
     CHECK(cudaFree(finalClassTensor->data));
     CHECK(cudaFree(finalBboxTensor->data));

     // free remaining tensor structure (already freed their data)
     freeTensor(convoutTensor, 0);
     freeTensor(classInputTensor, 0);
     freeTensor(confInputTensor, 0);
     freeTensor(bboxInputTensor, 0);
     freeTensor(classOutputTensor, 0);
     freeTensor(confOutputTensor, 0);
     freeTensor(bboxOutputTensor, 0);
     freeTensor(reduceMaxResTensor, 0);
     freeTensor(reduceArgResTensor, 0);
     freeTensor(mulResTensor, 0);
     freeTensor(bboxResTensor, 0);
     freeTensor(anchorsDeviceTensor, 0);
     freeTensor(finalClassTensor, 0);
     freeTensor(finalProbsTensor, 0);
     freeTensor(finalBboxTensor, 0);
}

// rearrange image data to [N, C, H, W] order
float *prepareData(float *data, cv::Mat &frame)
{
     assert(data && !frame.empty());
     unsigned int volChl = INPUT_H*INPUT_W;
     for (int c = 0; c < INPUT_C; ++c)
     {
          // the color image to input should be in BGR order
          for (unsigned j = 0; j < volChl; ++j)
               data[c*volChl + j] = float(frame.data[j*INPUT_C+c]) - PIXEL_MEAN[c];
     }

     CHECK(cudaEventRecord(stop_imread, 0));
     CHECK(cudaEventSynchronize(stop_imread));
     CHECK(cudaEventElapsedTime(&timeImread, start_imread, stop_imread));
     return data;
}

float *prepareAnchors(const float *anchor_shape, int width, int height, int N, int H, int W, int B)
{
     assert(anchor_shape);
     float center_x[W], center_y[H];
     float anchors[H * W * B * 4];
     int i, j, k;

     for (i = 1; i <= W; i++)
          center_x[i-1] = i * width / (W + 1.0);
     for (i = 1; i <= H; i++)
          center_y[i-1] = i * height / (H + 1.0);

     int h_vol = W * B * 4;
     int w_vol = B * 4;
     int b_vol = 4;
     for (i = 0; i < H; i++) {
          for (j = 0; j < W; j++) {
               for (k = 0; k < B; k++) {
                    anchors[i*h_vol+j*w_vol+k*b_vol] = center_x[j];
                    anchors[i*h_vol+j*w_vol+k*b_vol+1] = center_y[i];
                    anchors[i*h_vol+j*w_vol+k*b_vol+2] = anchor_shape[k*2];
                    anchors[i*h_vol+j*w_vol+k*b_vol+3] = anchor_shape[k*2+1];
               }
          }
     }

     float *ret = (float *)repeatMem(anchors, sizeof(float)*4*B*H*W, N, H2H);
     assert(ret);
     return ret;
}

void detectionFilter(struct predictions *preds, float nms_thresh, float prob_thresh)
{
     assert(preds->bbox && preds->klass && preds->prob && preds->keep);

     int i, j, num = preds->num;
     int *keep = preds->keep;
     float *klass = preds->klass;
     float *bbox = preds->bbox;
     for (i = 0; i < num; i++)
          keep[i] = 1;
     for (i = 0; i < num; i++) {
          // keep[i] = 1;
          // if (probs[i] < prob_thresh) {
          //      keep[i] = 0;
          //      continue;
          // }
          // for (j = i - 1; j >= 0 ; j--) {
          for (j = i + 1; j < num; j++) {
               if (!keep[j] || klass[i] != klass[j])
                    continue;
               if (computeIou(&bbox[i*OUTPUT_BBOX_SIZE],&bbox[j*OUTPUT_BBOX_SIZE]) > nms_thresh)
                         keep[j] = 0;
          }
     }
     CHECK(cudaEventRecord(stop_misc, 0));
     CHECK(cudaEventSynchronize(stop_misc));
     CHECK(cudaEventElapsedTime(&timeMisc, start_misc, stop_misc));
}

void fprintResult(FILE *fp, struct predictions *preds)
{
     assert(fp && preds->bbox && preds->klass && preds->prob && preds->keep);

     int i;
     float *bbox;
     for (i = 0; i < preds->num; i++) {
          if (!preds->keep[i])
               continue;
          bbox = &preds->bbox[i * OUTPUT_BBOX_SIZE];
          fprintf(fp, "%s -1 -1 0.0 %.2f %.2f %.2f %.2f 0.0 0.0 0.0 0.0 0.0 0.0 0.0 %.3f\n",
                  CLASS_NAMES[(int)preds->klass[i]], bbox[0], bbox[1], bbox[2], bbox[3], preds->prob[i]);
     }
}

void drawBbox(cv::Mat &frame, struct predictions *preds)
{
     assert(!frame.empty() && preds->bbox && preds->klass && preds->prob && preds->keep);
     int i;
     char *prob_s = (char *)sdt_alloc(32);
     float *bbox;
     for (i = 0; i < preds->num; i++) {
          if (!preds->keep[i] || preds->prob[i] < PLOT_PROB_THRESH)
               continue;
          bbox = &preds->bbox[i * OUTPUT_BBOX_SIZE];
          cv::rectangle(frame, cv::Point(bbox[0], bbox[1]), cv::Point(bbox[2], bbox[3]), cv::Scalar(0, 255, 0));
          sprintf(prob_s, "%.2f", preds->prob[i]);
          cv::putText(frame, std::string(CLASS_NAMES[(int)preds->klass[i]]) + ": " + std::string(prob_s), cv::Point(bbox[0], bbox[1]), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0));
     }
     sdt_free(prob_s);
}

static const struct option longopts[] = {
     {"eval-list", 1, NULL, 'e'},
     {"video", 1, NULL, 'v'},
     {"bbox-dir", 1, NULL, 'b'},
     {"x-shift", 1, NULL, 'x'},
     {"y-shift", 1, NULL, 'y'},
     {"help", 0, NULL, 'h'},
     {0, 0, 0, 0}
};

static const char *usage = "Usage: sqdtrt [options] IMAGE_DIR RESULT_DIR\n\
Apply SqueezeDet detection algorithm to images in IMAGE_DIR.\n\
Print detection results to one text file per image in RESULT_DIR using KITTI dataset format.\n\
\n\
Options:\n\
       -e, --eval-list=EVAL_LIST_FILE          Provide an evaluation list file which contains\n\
                                               the image names (without extension names)\n\
                                               in IMAGE_DIR to be evaluated.\n\
       -v, --video=VIDEO_FILE                  Detect a video file and play detected video\n\
                                               in a new window. IMAGE_DIR and RESULT_DIR\n\
                                               are not needed.\n\
       -b, --bbox-dir=BBOX_DIR                 Draw bounding boxes in images or video and\n\
                                               save them in BBOX_DIR.\n\
       -x, --x-shift=X_SHIFT                   Shift all bboxes downward X_SHIFT pixels.\n\
       -y, --y-shift=Y_SHIFT                   Shift all bboxes rightward Y_SHIFT pixels.\n\
       -h, --help                              Print this help and exit.\n";

static void print_usage_and_exit()
{
     fputs(usage, stderr);
     exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
     int opt, optindex;
     char *img_dir = NULL, *result_dir = NULL, *eval_list = NULL, *video = NULL, *bbox_dir = NULL;
     int x_shift = 0, y_shift = 0;
     while ((opt = getopt_long(argc, argv, ":e:v:b:x:y:h", longopts, &optindex)) != -1) {
          switch (opt) {
          case 'e':
               eval_list = optarg;
               break;
          case 'v':
               video = optarg;
               break;
          case 'b':
               bbox_dir = optarg;
               break;
          case 'x':
               x_shift = atoi(optarg);
               break;
          case 'y':
               y_shift = atoi(optarg);
               break;
          case 'h':
               print_usage_and_exit();
               break;
          case ':':
               fprintf(stderr, "option --%s needs a value\n", longopts[optindex].name);
               break;
          case '?':
               fprintf(stderr, "unknown option %c\n", optopt);
               break;
          }
     }
     if (video == NULL && optind >= argc)
          print_usage_and_exit();
     if (video == NULL) {
          img_dir = argv[optind++];
          result_dir = argv[optind];
          validateDir(img_dir, 0);
          validateDir(result_dir, 1);
     }
     if (bbox_dir != NULL)
          validateDir(bbox_dir, 1);

     // maloc host memory
     size_t inputSize = sizeof(float) * INPUT_C * INPUT_H * INPUT_W;
     float *data = (float *)sdt_alloc(inputSize);
     float *anchors = prepareAnchors(ANCHOR_SHAPE, INPUT_W, INPUT_H, INPUT_N, CONVOUT_H, CONVOUT_W, ANCHORS_PER_GRID);
     struct predictions preds;
     preds.prob = (float *)sdt_alloc(sizeof(float) * TOP_N_DETECTION);
     preds.klass = (float *)sdt_alloc(sizeof(float) * TOP_N_DETECTION);
     preds.bbox = (float *)sdt_alloc(sizeof(float) * TOP_N_DETECTION * OUTPUT_BBOX_SIZE);
     preds.keep = (int *)sdt_alloc(sizeof(int) * TOP_N_DETECTION);
     preds.num = TOP_N_DETECTION;

     // create engines
     IHostMemory *convModelStream{ nullptr };
     IHostMemory *interpretModelStream{ nullptr };
     APIToModel(INPUT_N, &convModelStream, &interpretModelStream);

     // deserialize engines
     IRuntime* runtime = createInferRuntime(gLogger);
     ICudaEngine* convEngine = runtime->deserializeCudaEngine(convModelStream->data(), convModelStream->size(), nullptr);
     ICudaEngine* interpretEngine = runtime->deserializeCudaEngine(interpretModelStream->data(), interpretModelStream->size(), nullptr);
     IExecutionContext *convContext = convEngine->createExecutionContext();
     IExecutionContext *interpretContext = interpretEngine->createExecutionContext();

     // malloc device memory
     setUpDevice(convContext, interpretContext, anchors, INPUT_N);

     // read image or video, alloc path buffer
     FILE *result_fp;
     char *result_file_path = NULL;
     char *img_name_buf = NULL;
     char *bbox_file_path = NULL;
     std::vector<std::string> imageList;
     int img_list_size;
     // double write_fps;
     cv::VideoCapture cap;
     cv::VideoWriter writer;
     cv::Mat frame, frame_origin;
     if (video == NULL) {
          result_file_path = sdt_path_alloc(NULL);
          img_name_buf = sdt_path_alloc(NULL);
          imageList = getImageList(img_dir, eval_list);
          img_list_size = imageList.size();
          printf("number of images: %d\n", img_list_size);
     } else {
          cap = cv::VideoCapture(video);
          if (!cap.isOpened()) {
               fprintf(stderr, "error reading video file: %s\n", video);
               exit(EXIT_FAILURE);
          }
          if (bbox_dir != NULL) {
               // if ((write_fps = cap.get(CV_CAP_PROP_FPS)) <= 0)
               //      write_fps = DEFAULT_FPS;
               // if (cap.read(frame) == false) {
               //      fprintf(stderr, "error reading first frame from file: %s\n", video);
               //      exit(EXIT_FAILURE);
               // }
               bbox_file_path = sdt_path_alloc(NULL);
               assemblePath(bbox_file_path, bbox_dir, video, "_bbox.avi");
               // int codec = CV_FOURCC('M', 'J', 'P', 'G');
               if (!writer.open(bbox_file_path,
                                CV_FOURCC('M', 'J', 'P', 'G'),
                                cap.get(CV_CAP_PROP_FPS),
                                cv::Size(cap.get(CV_CAP_PROP_FRAME_WIDTH),
                                         cap.get(CV_CAP_PROP_FRAME_HEIGHT)), true)) {
               // if (!writer.open(bbox_file_path, codec, write_fps, frame.size(), true)) {
                    fprintf(stderr, "error open cv::VideoWriter for file: %s\n", bbox_file_path);
                    exit(EXIT_FAILURE);
               }
               sdt_free(bbox_file_path);
          }
     }

     // do inference
     int frame_idx = 0;
     char key;
     double start_fps, end_fps;
     double fps;
     double imread_time_sum = 0, detect_time_sum = 0, misc_time_sum = 0, fps_sum = 0;
     float img_width, img_height;
     for (;; frame_idx++) {
          start_fps = getUnixTime();
          if (video == NULL) {
               if (frame_idx >= img_list_size) { // end of images
                    frame_idx--;
                    break;
               }
               getFileName(img_name_buf, imageList[frame_idx].c_str());
               printf("(%d/%d) image: %s ", frame_idx+1, img_list_size, img_name_buf);
               CHECK(cudaEventRecord(start_imread, 0));
               frame_origin = cv::imread(imageList[frame_idx]);
               if (frame_origin.empty()) {
                    fprintf(stderr, "error reading image\n");
                    continue;
               }
          } else {
               CHECK(cudaEventRecord(start_imread, 0));
               if (cap.read(frame_origin) == false) { // end of video
                    frame_idx--;
                    break;
               }
               if (frame_origin.empty()) {
                    fprintf(stderr, "error reading frame %d\n", frame_idx);
                    continue;
               }
          }
          preprocessFrame(frame, frame_origin, INPUT_W, INPUT_H, &img_width, &img_height);
          prepareData(data, frame);

          doInference(convContext, interpretContext, data, inputSize, img_width, img_height, x_shift, y_shift, &preds, INPUT_N);
          detectionFilter(&preds, NMS_THRESH, PROB_THRESH);

          if (video == NULL) {
               assemblePath(result_file_path, result_dir, imageList[frame_idx].c_str(), ".txt");
               result_fp = fopen(result_file_path, "w");
               fprintResult(result_fp, &preds);
               fclose(result_fp);
          } else {
               drawBbox(frame_origin, &preds);
               if (bbox_dir != NULL) {
                    writer.write(frame_origin);
               }
               cv::imshow("detection", frame_origin);
               key = cv::waitKey(1);
               if (key == ' ') {
                    cv::waitKey(0);
               } else if (key == 'q' || key == 27) { // 27 is the ASCII code of ESC
                    frame_idx--;
                    break;
               }
          }

          end_fps = getUnixTime();
          fps = 1 / (end_fps - start_fps);
          printf("imread: %.2fms detect: %.2fms misc: %.2fms fps: %.2fHz\n", timeImread, timeDetect, timeMisc, fps);
          imread_time_sum += timeImread;
          detect_time_sum += timeDetect;
          misc_time_sum += timeMisc;
          fps_sum += fps;
     }
     cap.release();
     writer.release();
     // clean up device memory
     cleanUp();

     // compute timing result
     double avg_imread, avg_detect, avg_misc, avg_fps;
     avg_imread = imread_time_sum / (frame_idx + 1);
     avg_detect = detect_time_sum / (frame_idx + 1);
     avg_misc = misc_time_sum / (frame_idx + 1);
     avg_fps = fps_sum / (frame_idx + 1);
     printf("Average timing: imread: %.2fms detect: %.2fms misc: %.2fms fps: %.2fHz\n", avg_imread, avg_detect, avg_misc, avg_fps);

     // destroy the engine
     convContext->destroy();
     interpretContext->destroy();
     convEngine->destroy();
     interpretEngine->destroy();
     runtime->destroy();

     // clean up host memory
     sdt_free(img_name_buf);
     sdt_free(result_file_path);
     sdt_free(data);
     sdt_free(anchors);
     sdt_free(preds.prob);
     sdt_free(preds.klass);
     sdt_free(preds.bbox);
     sdt_free(preds.keep);

     return 0;
}
