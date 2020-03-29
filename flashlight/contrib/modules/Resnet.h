#pragma once

#include "flashlight/nn/nn.h"

namespace fl {

class ConvBnAct : public fl::Sequential {
 public:
  ConvBnAct();
  explicit ConvBnAct(
      const int in_channels,
      const int out_channels,
      const int kw,
      const int kh,
      const int sx = 1,
      const int sy = 1,
      bool bn = true,
      bool act = true);

 private:
  FL_SAVE_LOAD_WITH_BASE(fl::Sequential)
};

class Bottleneck : public fl::Container {
 private:
   std::shared_ptr<ConvBnAct> downsample_;
   FL_SAVE_LOAD_WITH_BASE(fl::Container, downsample_)
 public:
  Bottleneck();
  explicit Bottleneck(
      const int in_channels,
      const int out_channels,
      const int stride=1);

  std::vector<fl::Variable> forward(
      const std::vector<fl::Variable>& inputs) override;

  std::string prettyString() const override;

  int static expansion() {
    return 4;
  }

};

class BasicBlock : public fl::Container {
 private:
   std::shared_ptr<ConvBnAct> downsample_;
   FL_SAVE_LOAD_WITH_BASE(fl::Container, downsample_)
 public:
  BasicBlock();
  explicit BasicBlock(
      const int in_channels,
      const int out_channels,
      const int stride=1);

  std::vector<fl::Variable> forward(
      const std::vector<fl::Variable>& inputs) override;

  std::string prettyString() const override;

  int static expansion() {
    return 1;
  }

};

template <typename T>
class ResNetStage : public fl::Sequential {
 public:
  ResNetStage();
  explicit ResNetStage(
      const int in_channels,
      const int out_channels,
      const int num_blocks,
      const int stride);
  FL_SAVE_LOAD_WITH_BASE(fl::Sequential)
};



Sequential resnet34();
Sequential resnet50();


} // namespace fl
//CEREAL_REGISTER_TYPE(fl::ConvBnAct)
//CEREAL_REGISTER_TYPE(fl::BasicBlock)
//CEREAL_REGISTER_TYPE(fl::ResNetStage<fl::BasicBlock>)
