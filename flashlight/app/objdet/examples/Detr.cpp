/**
 * Copyright (c) Facebook, Inc. and its affiliates.  * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include <exception>
#include <iomanip>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "flashlight/app/objdet/criterion/SetCriterion.h"
#include "flashlight/app/objdet/dataset/BoxUtils.h"
#include "flashlight/app/objdet/dataset/Coco.h"
#include "flashlight/app/objdet/nn/PositionalEmbeddingSine.h"
#include "flashlight/app/objdet/nn/Transformer.h"
#include "flashlight/app/objdet/nn/Detr.h"

#include "flashlight/ext/common/DistributedUtils.h"
#include "flashlight/ext/image/af/Transforms.h"
//#include "flashlight/ext/image/fl/models/Resnet50Backbone.h"
#include "flashlight/ext/image/fl/models/Resnet50Backbone.h"
#include "flashlight/ext/image/fl/models/Resnet.h"
#include "flashlight/ext/common/Serializer.h"
#include "flashlight/fl/meter/meters.h"
#include "flashlight/fl/optim/optim.h"
#include "flashlight/fl/flashlight.h"
#include "flashlight/lib/common/String.h"
#include "flashlight/lib/common/System.h"

constexpr const char* kTrainMode = "train";
constexpr const char* kContinueMode = "continue";
constexpr const char* kForkMode = "fork";
constexpr const char* kGflags = "gflags";
constexpr const char* kCommandLine = "commandline";
constexpr const char* kProgramName = "programname";
constexpr const char* kTimestamp = "timestamp";
constexpr const char* kUserName = "username";
constexpr const char* kHostName = "hostname";
constexpr const char* kEpoch = "epoch";
constexpr const char* kUpdates = "updates";
constexpr const char* kRunIdx = "runIdx";
constexpr const char* kRunPath = "runPath";

using fl::lib::format;
using fl::lib::pathsConcat;
using fl::lib::fileExists;
using fl::ext::Serializer;
using fl::lib::getCurrentDate;

#define FL_LOG_MASTER(lvl) LOG_IF(lvl, (fl::getWorldRank() == 0))

//TODO move out of ASR
std::string
getRunFile(const std::string& name, int runidx, const std::string& runpath) {
  auto fname = format("%03d_%s", runidx, name.c_str());
  return pathsConcat(runpath, fname);
};

std::string serializeGflags(const std::string& separator = "\n") {
  std::stringstream serialized;
  std::vector<gflags::CommandLineFlagInfo> allFlags;
  gflags::GetAllFlags(&allFlags);
  std::string currVal;
  for (auto itr = allFlags.begin(); itr != allFlags.end(); ++itr) {
    gflags::GetCommandLineOption(itr->name.c_str(), &currVal);
    serialized << "--" << itr->name << "=" << currVal << separator;
  }
  return serialized.str();
}

bool allClose(
    const af::array& a,
    const af::array& b,
    const double precision = 1e-2) {
  if ((a.numdims() != b.numdims()) || (a.dims() != b.dims())) {
    std::cout << " A dims " << a.dims() << std::endl;
    std::cout << " B dims " << b.dims() << std::endl;
    std::cout << "Shape mismatch " << std::endl;
    return false;
  }
  std::cout << " Max " << af::max<double>(af::abs(a - b)) << std::endl;
  return (af::max<double>(af::abs(a - b)) < precision);
}



DEFINE_string(data_dir, "/private/home/padentomasello/data/coco3/", "Directory of imagenet data");
DEFINE_double(lr, 0.0001f, "Learning rate");
DEFINE_double(momentum, 0.9f, "Momentum");
DEFINE_uint64(metric_iters, 5, "Print metric every");

DEFINE_double(wd, 1e-4f, "Weight decay");
DEFINE_uint64(epochs, 300, "Epochs");
DEFINE_uint64(eval_iters, 1, "Epochs");
DEFINE_int64(
    world_rank,
    0,
    "rank of the process (Used if rndv_filepath is not empty)");
DEFINE_int64(
    world_size,
    1,
    "total number of the process (Used if rndv_filepath is not empty)");
DEFINE_string(
    rndv_filepath,
    "",
    "Shared file path used for setting up rendezvous."
    "If empty, uses MPI to initialize.");
DEFINE_bool(enable_distributed, true, "Enable distributed training");
DEFINE_uint64(batch_size, 2, "Total batch size across all gpus");
DEFINE_string(checkpointpath, "/tmp/model", "Checkpointing prefix path");
DEFINE_int64(checkpoint, -1, "Load from checkpoint");

DEFINE_string(eval_dir, "/private/home/padentomasello/data/coco/output/", "Directory to dump images to run evaluation script on");
DEFINE_bool(print_params, false, "Directory to dump images to run evaluation script on");
DEFINE_bool(pretrained, true, "Directory to dump images to run evaluation script on");
DEFINE_string(pytorch_init, "", "Directory to dump images to run evaluation script on");
DEFINE_string(flagsfile, "", "Directory to dump images to run evaluation script on");
DEFINE_string(rundir, "", "Directory to dump images to run evaluation script on");
DEFINE_string(eval_script,"/private/home/padentomasello/code/flashlight/flashlight/app/objdet/scripts/eval_coco.py", "Script to run evaluation on dumped tensors");
DEFINE_string(set_env, "LD_LIBRARY_PATH=/private/home/padentomasello/usr/lib/:$LD_LIBRARY_PATH ", "Set environment");
DEFINE_int64(eval_break, -1, "Break eval after this many iters");
DEFINE_bool(eval_only, false, "Weather to just run eval");
void parseCmdLineFlagsWrapper(int argc, char** argv) {
  LOG(INFO) << "Parsing command line flags";
  gflags::ParseCommandLineFlags(&argc, &argv, false);
  if (!FLAGS_flagsfile.empty()) {
    LOG(INFO) << "Reading flags from file " << FLAGS_flagsfile;
    gflags::ReadFromFlagsFile(FLAGS_flagsfile, argv[0], true);
  }
  gflags::ParseCommandLineFlags(&argc, &argv, false);
  // Only new flags are re-serialized. Copy any values from deprecated flags to
  // new flags when deprecated flags are present and corresponding new flags
  // aren't
  //handleDeprecatedFlags();
}


using namespace fl;
using namespace fl::ext::image;
using namespace fl::app::objdet;

// TODO Refactor
//const int32_t backboneChannels = 512;


void printParamsAndGrads(std::shared_ptr<fl::Module> mod) {
  auto params = mod->params();
  int i = 0;
  for(auto param : params) {
    double paramMean = af::mean<double>(param.array());
    double paramStd = af::stdev<double>(param.array());
    double gradMean = -1.111111111111;
    double gradStd = -1.111111111111;
    if(param.isGradAvailable()) {
      auto grad = param.grad();
      gradMean = af::mean<double>(grad.array());
      gradStd = af::stdev<double>(grad.array());
    }
    std::cout << " i: " << i
      << " mean: " << paramMean
      << " std: " << paramStd
      << " grad mean: " << gradMean
      << " grad std: " << gradStd
      << std::endl;
    i++;
  }
}

int main(int argc, char** argv) {
  std::stringstream ss;
  ss << "PYTHONPATH=/private/home/padentomasello/code/detection-transformer/ "
    << FLAGS_set_env << " "
    << "/private/home/padentomasello/.conda/envs/coco/bin/python3.8 "
    << "-c 'import arrayfire as af'";
  system(ss.str().c_str());


  //gflags::ParseCommandLineFlags(&argc, &argv, false);
  int runIdx = 1; // current #runs in this path
  std::string runPath; // current experiment path
  std::string reloadPath; // path to model to reload
  std::string runStatus = argv[1];
  int64_t startEpoch = 0;
  int64_t startUpdate = 0;
  std::string exec(argv[0]);
  std::vector<std::string> argvs;
  for (int i = 0; i < argc; i++) {
    argvs.emplace_back(argv[i]);
  }
  gflags::SetUsageMessage(
    "Usage: \n " + exec + " train [flags]\n or " + exec +
    " continue [directory] [flags]\n or " + exec +
    " fork [directory/model] [flags]");
  // Saving checkpointing
  if (argc <= 1) {
    LOG(FATAL) << gflags::ProgramUsage();
  }
  if (runStatus == "train") {
    parseCmdLineFlagsWrapper(argc, argv);
    runPath = FLAGS_rundir;
  } else if (runStatus == "continue") {
    runPath = argv[2];
    while (fileExists(getRunFile("model_last.bin", runIdx, runPath))) {
      ++runIdx;
    }
    reloadPath = getRunFile("model_last.bin", runIdx - 1, runPath);
    LOG(INFO) << "reload path is " << reloadPath;
    std::unordered_map<std::string, std::string> cfg;
    std::string version;
    Serializer::load(reloadPath, version, cfg);
    auto flags = cfg.find(kGflags);
    if (flags == cfg.end()) {
      LOG(FATAL) << "Invalid config loaded from " << reloadPath;
    }
    LOG(INFO) << "Reading flags from config file " << reloadPath;
    gflags::ReadFlagsFromString(flags->second, gflags::GetArgv0(), true);
    gflags::ParseCommandLineFlags(&argc, &argv, false);
    parseCmdLineFlagsWrapper(argc, argv);
    auto epoch = cfg.find(kEpoch);
    if (epoch == cfg.end()) {
      LOG(WARNING) << "Did not find epoch to start from, starting from 0.";
    } else {
      startEpoch = std::stoi(epoch->second);
    }
    auto nbupdates = cfg.find(kUpdates);
    if (nbupdates == cfg.end()) {
      LOG(WARNING) << "Did not find #updates to start from, starting from 0.";
    } else {
      startUpdate = std::stoi(nbupdates->second);
    }
  } else {
    LOG(FATAL) << gflags::ProgramUsage();
  }

  if (runPath.empty()) {
    LOG(FATAL) << "'runpath' specified by --rundir, --runname cannot be empty";
  }

  std::cout << serializeGflags() << std::endl;
  //const std::string label_path = FLAGS_data_dir + "labels.txt";
  //const std::string train_list = FLAGS_data_dir + "train";
  //const std::string val_list = FLAGS_data_dir + "val";

  /////////////////////////
  // Setup distributed training
  ////////////////////////
  if (FLAGS_enable_distributed) {
    fl::ext::initDistributed(
        FLAGS_world_rank,
        FLAGS_world_size,
        8,
        FLAGS_rndv_filepath);
  }
  af::info();
  const int worldRank = fl::getWorldRank();
  const int worldSize = fl::getWorldSize();


  std::string cmdLine= fl::lib::join(" ", argvs);
  std::unordered_map<std::string, std::string> config = {
      {kProgramName, exec},
      {kCommandLine, cmdLine},
      {kGflags, serializeGflags()},
      // extra goodies
      {kUserName, fl::lib::getEnvVar("USER")},
      {kHostName, fl::lib::getEnvVar("HOSTNAME")},
      {kTimestamp, getCurrentDate() + ", " + getCurrentDate()},
      {kRunIdx, std::to_string(runIdx)},
      {kRunPath, runPath}};
  //af::setDevice(worldRank);
  //af::setSeed(worldSize);
  std::cout << "World rank: " << worldRank << std::endl;

  auto reducer = std::make_shared<fl::CoalescingReducer>(
      1.0,
      true,
      true);

  /////////////////////////
  // Setup distributed training
  ////////////////////////

  //////////////////////////
  //  Create datasets
  /////////////////////////
  const std::vector<float> mean = {0.485, 0.456, 0.406};
  const std::vector<float> std = {0.229, 0.224, 0.225};
  //std::vector<ImageTransform> train_transforms = {
      //// randomly resize shortest side of image between 256 to 480 for scale 
      //// invariance
      ////randomResizeTransform(256, 480),
      ////randomCropTransform(224, 224), normalizeImage(mean, std),
      //// Randomly flip image with probability of 0.5
      ////horizontalFlipTransform(0.5)
  //};
  std::vector<ImageTransform> val_transforms = {
      // Resize shortest side to 256, then take a center crop
      //resizeTransform(256),
      //centerCropTransform(224),
      normalizeImage(mean, std)
  };

  const int32_t modelDim = 256;
  const int32_t numHeads = 8;
  const int32_t numEncoderLayers = 6;
  const int32_t numDecoderLayers = 6;
  const int32_t mlpDim = 2048;
  // TODO check this is correct
  const int32_t hiddenDim = modelDim;
  const int32_t numClasses = 91;
  const int32_t numQueries = 100;
  const float pDropout = 0.1;
  const bool auxLoss = false;
  std::shared_ptr<Resnet50Backbone> backbone;
  if(FLAGS_pretrained) {
    std::string modelPath = "/checkpoint/padentomasello/models/resnet50/from_pytorch_fbn2";
    fl::load(modelPath, backbone);
  } else {
    backbone = std::make_shared<Resnet50Backbone>();
  }
  auto transformer = std::make_shared<Transformer>(
      modelDim,
      numHeads,
      numEncoderLayers,
      numDecoderLayers,
      mlpDim,
      pDropout);

  auto detr = std::make_shared<Detr>(
      transformer,
      backbone,
      hiddenDim,
      numClasses,
      numQueries,
      auxLoss);

  std::shared_ptr<Detr> detr2;

  auto* curMemMgr =
          fl::MemoryManagerInstaller::currentlyInstalledMemoryManager();
      if (curMemMgr) {
        curMemMgr->printInfo("Memory Manager Stats", 0 /* device id */);
      }

  // Trained
  // untrained but initializaed
  if (!FLAGS_pytorch_init.empty()) {
    std::cout << "Loading from pytorch intiialization path" << FLAGS_pytorch_init << std::endl;
  //std::string modelPath = "/checkpoint/padentomasello/models/detr/from_pytorch";
    //std::string modelPath = "/checkpoint/padentomasello/models/detr/pytorch_initializaition";
    fl::load(FLAGS_pytorch_init, detr);
  }

  detr->train();

  // synchronize parameters of tje model so that the parameters in each process
  // is the same
  fl::allReduceParameters(detr);
  //fl::allReduceParameters(backbone);

  // Add a hook to synchronize gradients of model parameters as they are
  // computed
  fl::distributeModuleGrads(detr, reducer);
  //fl::distributeModuleGrads(backbone, reducer);

  auto saveOutput = [](
      af::array imageSizes,
      af::array imageIds,
      af::array boxes,
      af::array scores,
      std::string outputFile) {
      af::saveArray("imageSizes", imageSizes, outputFile.c_str(), false);
      af::saveArray("imageIds", imageIds, outputFile.c_str(), true);
      af::saveArray("scores", scores, outputFile.c_str(), true);
      af::saveArray("bboxes", boxes, outputFile.c_str(), true);
  };

  const float setCostClass = 1.f;
  const float setCostBBox = 5.f;
  const float setCostGiou = 2.f;
  const float bboxLossCoef = 5.f;
  const float giouLossCoef = 2.f;


  auto matcher = HungarianMatcher(
      setCostClass,
      setCostBBox,
      setCostGiou
      );
  SetCriterion::LossDict losses;


  std::unordered_map<std::string, float> lossWeightsBase = 
        { { "loss_ce" , 1.f} ,
        { "loss_giou", giouLossCoef },
        { "loss_bbox", bboxLossCoef }
  };

  std::unordered_map<std::string, float> lossWeights;
  for(int i = 0; i < numDecoderLayers; i++) {
    for(auto l : lossWeightsBase) {
      std::string key = l.first + "_" + std::to_string(i);
      lossWeights[key] = l.second;
    }
  }
  auto criterion = SetCriterion(
      numClasses,
      matcher,
      lossWeights,
      0.1,
      losses);

  auto eval_loop = [saveOutput](
      std::shared_ptr<Module> backbone,
      std::shared_ptr<Detr> model,
      std::shared_ptr<CocoDataset> dataset) {
    model->eval();
    int idx = 0;
    std::stringstream mkdir_command;
    mkdir_command << "mkdir -p " << FLAGS_eval_dir << fl::getWorldRank();
    system(mkdir_command.str().c_str());
    for(auto& sample : *dataset) {
      std::vector<Variable> input =  { 
        fl::Variable(sample.images, false),  
        fl::Variable(sample.masks, false) 
      };
      //auto features = backbone->forward(images)[0];
      //auto features = input;
      auto output = model->forward(input);
      std::stringstream ss;
      ss << FLAGS_eval_dir << fl::getWorldRank() << "/detection" << idx << ".array";
      auto output_array = ss.str();
      int lastLayerIdx = output[0].dims(3) - 1;
      auto output_first_last = output[0].array()(af::span, af::span, af::span, af::seq(lastLayerIdx, lastLayerIdx));
      auto output_second_last = output[1].array()(af::span, af::span, af::span, af::seq(lastLayerIdx, lastLayerIdx));
      //saveOutput(sample.imageSizes, sample.imageIds, output[1].array(), output[0].array(), ss.str());
      saveOutput(sample.originalImageSizes, sample.imageIds, output_second_last, output_first_last, ss.str());
      idx++;
      if(FLAGS_eval_break > 0 && idx == FLAGS_eval_break) {
        break;
      }
    }
    if(FLAGS_enable_distributed) {
      barrier();
    }
    std::stringstream ss;
    ss << "PYTHONPATH=/private/home/padentomasello/code/detection-transformer/ "
      << FLAGS_set_env << " "
      << "/private/home/padentomasello/.conda/envs/coco/bin/python3.8 "
      << FLAGS_eval_script << " --dir "
      << FLAGS_eval_dir;
    int numAttempts = 10;
    for(int i = 0; i < numAttempts; i++) {
      int rv = system(ss.str().c_str());
      if (rv == 0) {
        break;
      }
      std::cout << "Eval failed, retrying in 5 seconds" << std::endl;
      sleep(5);
    }
    if(FLAGS_enable_distributed) {
      barrier();
    }
    //system(ss.str().c_str());
    std::stringstream ss2;
    ss2 << "rm -rf " << FLAGS_eval_dir << fl::getWorldRank() <<"/detection*";
    std::cout << "Commond: " << ss2.str() << std::endl;
    system(ss2.str().c_str());
    model->train();
  };

  //const int64_t batch_size_per_gpu = FLAGS_batch_size / FLAGS_world_size;
  const int64_t batch_size_per_gpu = FLAGS_batch_size;
  const int64_t prefetch_threads = 10;
  const int64_t prefetch_size = FLAGS_batch_size;
  std::string coco_dir = FLAGS_data_dir;
  //std::string coco_list = "/private/home/padentomasello/data/coco-mini/train.lst";
  //auto coco = cv::dataset::coco(coco_list, val_transforms, FLAGS_batch_size);

  auto train_ds = std::make_shared<CocoDataset>(
      coco_dir + "train.lst",
      val_transforms,
      worldRank,
      worldSize,
      batch_size_per_gpu,
      prefetch_threads,
      batch_size_per_gpu, false);

  auto val_ds = std::make_shared<CocoDataset>(
      coco_dir + "val.lst",
      val_transforms,
      worldRank,
      worldSize,
      batch_size_per_gpu,
      prefetch_threads,
      batch_size_per_gpu,
      true);
  //SGDOptimizer opt(detr.params(), FLAGS_lr, FLAGS_momentum, FLAGS_wd);
  //

  const float beta1 = 0.9;
  const float beta2 = 0.999;
  const float epsilon = 1e-8;
  auto opt = std::make_shared<AdamOptimizer>(
      detr->paramsWithoutBackbone(), FLAGS_lr, beta1, beta2, epsilon, FLAGS_wd);
  auto opt2 = std::make_shared<AdamOptimizer>(
      detr->backboneParams(), FLAGS_lr * 0.1, beta1, beta2, epsilon, FLAGS_wd);
  auto lrScheduler = [&opt, &opt2](int epoch) {
    // Adjust learning rate every 30 epoch after 30
    const float newLr = FLAGS_lr * pow(0.1, epoch / 100);
    LOG(INFO) << "Setting learning rate to: " << newLr;
    opt->setLr(newLr);
    opt2->setLr(newLr * 0.1);
  };

  if (runStatus == "continue") {
    std::unordered_map<std::string, std::string> cfg; // unused
    std::string version;
        Serializer::load(
            reloadPath,
            version,
            cfg,
            detr,
            opt,
            opt2);
  }


  auto weightDict = criterion.getWeightDict();
  if(startEpoch > 0 || FLAGS_eval_only) {
    std::cout << "here" << std::endl;
    detr->eval();
    eval_loop(backbone, detr, val_ds);
    detr->train();
    if(FLAGS_eval_only) { 
      return 0; 
    }
  }


  

  for(int epoch= startEpoch; epoch < FLAGS_epochs; epoch++) {
    lrScheduler(epoch);

    std::map<std::string, AverageValueMeter> meters;
    std::map<std::string, TimeMeter> timers;
    int idx = 0;
    timers["total"].resume();
    train_ds->resample();
    //while(true) {
    for(auto& sample : *train_ds) {
      std::vector<Variable> input =  { 
        fl::Variable(sample.images, false),
        fl::Variable(sample.masks, false) 
      };
      auto output = detr->forward(input);


      timers["forward"].stop();

      /////////////////////////
      // Criterion
      /////////////////////////
      std::vector<Variable> targetBoxes(sample.target_boxes.size());
      std::vector<Variable> targetClasses(sample.target_labels.size());

      std::transform(
          sample.target_boxes.begin(), sample.target_boxes.end(),
          targetBoxes.begin(),
          [](const af::array& in) { return fl::Variable(in, false); });

      std::transform(
          sample.target_labels.begin(), sample.target_labels.end(),
          targetClasses.begin(),
          [](const af::array& in) { return fl::Variable(in, false); });

      timers["criterion"].resume();

      // TODO test
      //std::vector<Variable> outputSecond = { output[1], output[1] };
      //std::vector<Variable> outputFirst = { output[0], output[0] };
      //output[0] = concatenate(outputFirst, 3);
      //output[1] = concatenate(outputSecond, 4);

      auto loss = criterion.forward(
          output[1],
          output[0],
          targetBoxes,
          targetClasses);
      auto accumLoss = fl::Variable(af::constant(0, 1), true);
      for(auto losses : loss) {
        fl::Variable scaled_loss = weightDict[losses.first] * losses.second;
        meters[losses.first].add(losses.second.array());
        meters[losses.first + "_weighted"].add(scaled_loss.array());
        accumLoss = scaled_loss + accumLoss;
      }
      meters["sum"].add(accumLoss.array());
      timers["criterion"].stop();

      /////////////////////////
      // Backward and update gradients
      //////////////////////////
      timers["backward"].resume();
      accumLoss.backward();
      timers["backward"].stop();

      if (FLAGS_enable_distributed) {
        reducer->finalize();
      }

      if(FLAGS_print_params) {
        std::cout << "Print detr params + grads" << std::endl;
        printParamsAndGrads(detr);
        std::cout << "Print backbone params + grads" << std::endl;
        printParamsAndGrads(backbone);
      }
      fl::clipGradNorm(detr->params(), 0.1);

      opt->step();
      opt2->step();

      opt->zeroGrad();
      opt2->zeroGrad();
      //////////////////////////
      // Metrics
      /////////////////////////
      if(++idx % FLAGS_metric_iters == 0) {
        double total_time = timers["total"].value();
        double sample_per_second = (idx * FLAGS_batch_size * worldSize) / total_time;
        double forward_time = timers["forward"].value();
        double backward_time = timers["backward"].value();
        double criterion_time = timers["criterion"].value();
        std::stringstream ss;
        ss << "Epoch: " <<epoch<< std::setprecision(5) << " | Batch: " << idx
            << " | total_time: " << total_time
            << " | idx: " << idx
            << " | sample_per_second: " << sample_per_second
            << " | forward_time_avg: " << forward_time / idx
            << " | backward_time_avg: " << backward_time / idx
            << " | criterion_time_avg: " << criterion_time / idx;
        for(auto meter : meters) {
          fl::ext::syncMeter(meter.second);
          ss << " | " << meter.first << ": " << meter.second.value()[0];
        }
        ss << std::endl;
        FL_LOG_MASTER(INFO) << ss.str();
      }
    }
    for(auto timer : timers) {
      timer.second.reset();
    }
    for(auto meter : meters) {
      meter.second.reset();
    }
    std::string filename = 
      getRunFile(format("model_last.bin", idx), runIdx, runPath);
    config[kEpoch] = std::to_string(epoch);
    Serializer::save(filename, "0.1", config, detr, opt, opt2);
    filename = 
      getRunFile(format("model_iter_%03d.bin", epoch), runIdx, runPath);
    Serializer::save(filename, "0.1", config, detr, opt, opt2);
    if(epoch % FLAGS_eval_iters == 0) {
      eval_loop(backbone, detr, val_ds);
      //eval_loop(detr, val_ds);
      //saveModel(e);
    }
  }

  std::string filename = 
    getRunFile(format("model_test.bin", 1), runIdx, runPath);
  Serializer::save(filename, "0.1", config, detr, opt, opt2);
  std::string version;
  Serializer::load(filename, version, config, detr2, opt, opt2);
  assert(allParamsClose(*detr2, *detr));
  detr->eval();
  detr2->eval();
  for(auto& sample : *train_ds) {
      std::vector<Variable> input =  { 
        fl::Variable(sample.images, false),
        fl::Variable(sample.masks, false) 
      };
      auto output = detr->forward(input);
      auto output2 = detr2->forward(input);
      for(int i = 0; i < output.size(); i++) {
        std::cout << " Checking output " << i << std::endl;
        assert(allClose(output[i], output2[i]));
      }
      std::cout << "Here" << std::endl;
      break;
  }
  return 0;
}
