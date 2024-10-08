/*
 * Software License Agreement (BSD License)
 *
 *  Point Cloud Library (PCL) - www.pointclouds.org
 *  Copyright (c) 2014-, Open Perception, Inc.
 *
 *	All rights reserved
 *
 *	Redistribution and use in source and binary forms, with or without
 *	modification, are permitted provided that the following conditions are met
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the copyright holder(s) nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef PCL_REGISTRATION_IMPL_IA_KFPCS_H_
#define PCL_REGISTRATION_IMPL_IA_KFPCS_H_

#include <limits>

namespace pcl {

namespace registration {

template <typename PointSource, typename PointTarget, typename NormalT, typename Scalar>
KFPCSInitialAlignment<PointSource, PointTarget, NormalT, Scalar>::
    KFPCSInitialAlignment()
: indices_validation_(new pcl::Indices)
{
  reg_name_ = "pcl::registration::KFPCSInitialAlignment";
}

template <typename PointSource, typename PointTarget, typename NormalT, typename Scalar>
bool
KFPCSInitialAlignment<PointSource, PointTarget, NormalT, Scalar>::initCompute()
{
  // due to sparse keypoint cloud, do not normalize delta with estimated point density
  if (normalize_delta_) {
    PCL_WARN("[%s::initCompute] Delta should be set according to keypoint precision! "
             "Normalization according to point cloud density is ignored.\n",
             reg_name_.c_str());
    normalize_delta_ = false;
  }

  // initialize as in fpcs
  pcl::registration::FPCSInitialAlignment<PointSource, PointTarget, NormalT, Scalar>::
      initCompute();

  // set the threshold values with respect to keypoint characteristics
  max_pair_diff_ = delta_ * 1.414f;      // diff between 2 points of delta_ accuracy
  coincidation_limit_ = delta_ * 2.828f; // diff between diff of 2 points
  max_edge_diff_ =
      delta_ *
      3.f; // diff between 2 points + some inaccuracy due to quadruple orientation
  max_mse_ =
      powf(delta_ * 4.f, 2.f); // diff between 2 points + some registration inaccuracy
  max_inlier_dist_sqr_ =
      powf(delta_ * 8.f,
           2.f); // set rel. high, because MSAC is used (residual based score function)

  // check use of translation costs and calculate upper boundary if not set by user
  if (upper_trl_boundary_ < 0)
    upper_trl_boundary_ = diameter_ * (1.f - approx_overlap_) * 0.5f;

  if (!(lower_trl_boundary_ < 0) && upper_trl_boundary_ > lower_trl_boundary_)
    use_trl_score_ = true;
  else
    lambda_ = 0.f;

  // generate a subset of indices of size ransac_iterations_ on which to evaluate
  // candidates on
  if (indices_->size() <= static_cast<std::size_t>(ransac_iterations_) ||
      ransac_iterations_ <= 0)
    indices_validation_ = indices_;
  else {
    indices_validation_.reset(new pcl::Indices);
    pcl::RandomSample<PointSource> random_sampling;
    random_sampling.setInputCloud(input_);
    random_sampling.setIndices(indices_);
    random_sampling.setSample(ransac_iterations_);
    random_sampling.filter(*indices_validation_);
  }

  PCL_DEBUG("[%s::initCompute] delta_=%g, max_inlier_dist_sqr_=%g, "
            "coincidation_limit_=%g, max_edge_diff_=%g, max_pair_diff_=%g\n",
            reg_name_.c_str(),
            delta_,
            max_inlier_dist_sqr_,
            coincidation_limit_,
            max_edge_diff_,
            max_pair_diff_);
  return (true);
}

template <typename PointSource, typename PointTarget, typename NormalT, typename Scalar>
void
KFPCSInitialAlignment<PointSource, PointTarget, NormalT, Scalar>::handleMatches(
    const pcl::Indices& base_indices,
    std::vector<pcl::Indices>& matches,
    MatchingCandidates& candidates)
{
  candidates.clear();

  // loop over all Candidate matches
  for (auto& match : matches) {
    Eigen::Matrix4f transformation_temp;
    pcl::Correspondences correspondences_temp;
    float fitness_score =
        std::numeric_limits<float>::max(); // reset to std::numeric_limits<float>::max()
                                           // to accept all candidates and not only best

    correspondences_temp.emplace_back(match[0], base_indices[0], 0.0);
    correspondences_temp.emplace_back(match[1], base_indices[1], 0.0);
    correspondences_temp.emplace_back(match[2], base_indices[2], 0.0);
    correspondences_temp.emplace_back(match[3], base_indices[3], 0.0);

    // check match based on residuals of the corresponding points after transformation
    if (validateMatch(base_indices, match, correspondences_temp, transformation_temp) <
        0)
      continue;

    // check resulting transformation using a sub sample of the source point cloud
    // all candidates are stored and later sorted according to their fitness score
    validateTransformation(transformation_temp, fitness_score);

    // store all valid match as well as associated score and transformation
    candidates.emplace_back(fitness_score, correspondences_temp, transformation_temp);
  }
  // make sure that candidate with best fitness score is at the front, for early
  // termination check
  if (!candidates.empty()) {
    auto best_candidate = candidates.begin();
    for (auto iter = candidates.begin(); iter < candidates.end(); ++iter)
      if (iter->fitness_score < best_candidate->fitness_score)
        best_candidate = iter;
    if (best_candidate != candidates.begin())
      std::swap(*best_candidate, *candidates.begin());
  }
}

template <typename PointSource, typename PointTarget, typename NormalT, typename Scalar>
int
KFPCSInitialAlignment<PointSource, PointTarget, NormalT, Scalar>::
    validateTransformation(Eigen::Matrix4f& transformation, float& fitness_score)
{
  // transform sub sampled source cloud
  PointCloudSource source_transformed;
  pcl::transformPointCloud(
      *input_, *indices_validation_, source_transformed, transformation);

  const std::size_t nr_points = source_transformed.size();
  float score_a = 0.f, score_b = 0.f;

  // residual costs based on mse
  pcl::Indices ids(1);
  std::vector<float> dists_sqr(1);
  for (const auto& source : source_transformed) {
    // search for nearest point using kd tree search
    tree_->nearestKSearch(source, 1, ids, dists_sqr);
    score_a += (dists_sqr[0] < max_inlier_dist_sqr_ ? dists_sqr[0]
                                                    : max_inlier_dist_sqr_); // MSAC
  }

  score_a /= (max_inlier_dist_sqr_ * nr_points); // MSAC
  // score_a = 1.f - (1.f - score_a) / (1.f - approx_overlap_); // make score relative
  // to estimated overlap

  // translation score (solutions with small translation are down-voted)
  float scale = 1.f;
  if (use_trl_score_) {
    float trl = transformation.rightCols<1>().head<3>().norm();
    float trl_ratio =
        (trl - lower_trl_boundary_) / (upper_trl_boundary_ - lower_trl_boundary_);

    score_b =
        (trl_ratio < 0.f ? 1.f
                         : (trl_ratio > 1.f ? 0.f
                                            : 0.5f * sin(M_PI * trl_ratio + M_PI_2) +
                                                  0.5f)); // sinusoidal costs
    scale += lambda_;
  }

  // calculate the fitness and return unsuccessful if smaller than previous ones
  float fitness_score_temp = (score_a + lambda_ * score_b) / scale;
  if (fitness_score_temp > fitness_score)
    return (-1);

  fitness_score = fitness_score_temp;
  return (0);
}

template <typename PointSource, typename PointTarget, typename NormalT, typename Scalar>
void
KFPCSInitialAlignment<PointSource, PointTarget, NormalT, Scalar>::finalCompute(
    const std::vector<MatchingCandidates>& candidates)
{
  // reorganize candidates into single vector
  std::size_t total_size = 0;
  for (const auto& candidate : candidates)
    total_size += candidate.size();

  candidates_.clear();
  candidates_.reserve(total_size);

  for (const auto& candidate : candidates)
    for (const auto& match : candidate)
      candidates_.push_back(match);

  // sort according to score value
  std::sort(candidates_.begin(), candidates_.end(), by_score());

  // return here if no score was valid, i.e. all scores are
  // std::numeric_limits<float>::max()
  if (candidates_[0].fitness_score == std::numeric_limits<float>::max()) {
    converged_ = false;
    return;
  }

  // save best candidate as output result
  // note, all other candidates are accessible via getNBestCandidates () and
  // getTBestCandidates ()
  fitness_score_ = candidates_[0].fitness_score;
  final_transformation_ = candidates_[0].transformation;
  *correspondences_ = candidates_[0].correspondences;
  PCL_DEBUG("[%s::finalCompute] best score is %g, out of %zu candidate solutions.\n",
            reg_name_.c_str(),
            fitness_score_,
            candidates_.size());

  // here we define convergence if resulting score is above threshold
  converged_ = fitness_score_ < score_threshold_;
}

template <typename PointSource, typename PointTarget, typename NormalT, typename Scalar>
void
KFPCSInitialAlignment<PointSource, PointTarget, NormalT, Scalar>::getNBestCandidates(
    int n, float min_angle3d, float min_translation3d, MatchingCandidates& candidates)
{
  candidates.clear();

  // loop over all candidates starting from the best one
  for (const auto& candidate : candidates_) {
    // stop if current candidate has no valid score
    if (candidate.fitness_score == std::numeric_limits<float>::max())
      return;

    // check if current candidate is a unique one compared to previous using the
    // min_diff threshold
    bool unique = true;
    for (const auto& c2 : candidates) {
      Eigen::Matrix4f diff =
          candidate.transformation.colPivHouseholderQr().solve(c2.transformation);
      const float angle3d = Eigen::AngleAxisf(diff.topLeftCorner<3, 3>()).angle();
      const float translation3d = diff.block<3, 1>(0, 3).norm();
      unique = angle3d > min_angle3d && translation3d > min_translation3d;
      if (!unique) {
        break;
      }
    }

    // add candidate to best candidates
    if (unique)
      candidates.push_back(candidate);

    // stop if n candidates are reached
    if (candidates.size() == n)
      return;
  }
}

template <typename PointSource, typename PointTarget, typename NormalT, typename Scalar>
void
KFPCSInitialAlignment<PointSource, PointTarget, NormalT, Scalar>::getTBestCandidates(
    float t, float min_angle3d, float min_translation3d, MatchingCandidates& candidates)
{
  candidates.clear();

  // loop over all candidates starting from the best one
  for (const auto& candidate : candidates_) {
    // stop if current candidate has score below threshold
    if (candidate.fitness_score > t)
      return;

    // check if current candidate is a unique one compared to previous using the
    // min_diff threshold
    bool unique = true;
    for (const auto& c2 : candidates) {
      Eigen::Matrix4f diff =
          candidate.transformation.colPivHouseholderQr().solve(c2.transformation);
      const float angle3d = Eigen::AngleAxisf(diff.topLeftCorner<3, 3>()).angle();
      const float translation3d = diff.block<3, 1>(0, 3).norm();
      unique = angle3d > min_angle3d && translation3d > min_translation3d;
      if (!unique) {
        break;
      }
    }

    // add candidate to best candidates
    if (unique)
      candidates.push_back(candidate);
  }
}

} // namespace registration
} // namespace pcl

#endif // PCL_REGISTRATION_IMPL_IA_KFPCS_H_
