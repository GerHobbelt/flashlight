#include "Hungarian.h"
#include "flashlight/app/objdet/criterion/HungarianLib.h"
#include "flashlight/app/objdet/dataset/BoxUtils.h"

#include "flashlight/fl/autograd/Functions.h"

namespace {

std::pair<af::array, af::array> hungarian(af::array& cost) {
  cost = cost.T();
  // af_print(cost);
  const int M = cost.dims(0);
  const int N = cost.dims(1);
  std::vector<float> costHost(cost.elements());
  std::vector<int> rowIdxs(M);
  std::vector<int> colIdxs(M);
  cost.host(costHost.data());
  fl::cv::hungarian(costHost.data(), rowIdxs.data(), colIdxs.data(), M, N);
  auto rowIdxsArray = af::array(M, rowIdxs.data());
  auto colIdxsArray = af::array(M, colIdxs.data());
  return {rowIdxsArray, colIdxsArray};
}
} // namespace

namespace fl {
namespace app {
namespace objdet {

HungarianMatcher::HungarianMatcher(
    const float costClass,
    const float costBbox,
    const float costGiou)
    : costClass_(costClass), costBbox_(costBbox), costGiou_(costGiou){};

std::pair<af::array, af::array> HungarianMatcher::matchBatch(
    const Variable& predBoxes,
    const Variable& predLogits,
    const Variable& targetBoxes,
    const Variable& targetClasses) const {
  // TODO Kind of a hack...
  if (targetClasses.isempty()) {
    return {af::array(0, 1), af::array(0, 1)};
  }

  // Create an M X N cost matrix where M is the number of targets and N is the
  // number of preds

  // Class cost
  auto outProbs = softmax(predLogits, 0);
  auto costClass = transpose((0 - outProbs(targetClasses.array(), af::span)));
  // auto costClass = (1 - outProbs(targetClasses.array(), af::span));
  //

  // Generalized IOU loss
  auto costGiou =
      0 - generalizedBoxIou(cxcywh2xyxy(predBoxes), cxcywh2xyxy(targetBoxes));

  // Bbox Cost
  Variable costBbox = cartesian(
      predBoxes, targetBoxes, [](const Variable& x, const Variable& y) {
        return sum(abs(x - y), {0});
      });
  costBbox = flatten(costBbox, 0, 1);

  auto cost =
      costBbox_ * costBbox + costClass_ * costClass + costGiou_ * costGiou;
  return ::hungarian(cost.array());
}

std::vector<std::pair<af::array, af::array>> HungarianMatcher::forward(
    const Variable& predBoxes,
    const Variable& predLogits,
    const std::vector<Variable>& targetBoxes,
    const std::vector<Variable>& targetClasses) const {
  std::vector<std::pair<af::array, af::array>> results;
  for (int b = 0; b < predBoxes.dims(2); b++) {
    auto result = matchBatch(
        predBoxes(af::span, af::span, b),
        predLogits(af::span, af::span, b),
        targetBoxes[b],
        targetClasses[b]);
    results.emplace_back(result);
  }
  return results;
};

} // end namespace objdet
} // end namespace app
} // end namespace fl
