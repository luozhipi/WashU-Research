#include <Eigen/Geometry>
#include <eigen3/Eigen/Eigen>
#include <eigen3/Eigen/StdVector>

#include <pcl/features/normal_3d_omp.h>
#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/filters/uniform_sampling.h>
#include <pcl/io/ply_io.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/registration/gicp.h>
#include <pcl/registration/icp.h>
#include <pcl/registration/icp_nl.h>
#include <pcl/search/kdtree.h>
#include <pcl/visualization/keyboard_event.h>
#include <pcl/visualization/pcl_visualizer.h>

#include <etw_utils.hpp>
#include <scan_gflags.h>

#include <fmt/format.h>
#include <fmt/ostream.h>

typedef pcl::PointXYZRGB PointType;
typedef pcl::PointNormal NormalType;

DEFINE_string(transformation_folder, "transformations/",
              "Folder to write transformation matricies");

class MyPointRepresentation : public pcl::PointRepresentation<NormalType> {
  using pcl::PointRepresentation<NormalType>::nr_dimensions_;

public:
  MyPointRepresentation() {
    // Define the number of dimensions
    nr_dimensions_ = 7;
  }

  // Override the copyToFloatArray method to define our feature vector
  virtual void copyToFloatArray(const NormalType &p, float *out) const {
    // < x, y, z, curvature >
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
    out[3] = p.normal_x;
    out[4] = p.normal_y;
    out[5] = p.normal_z;
    out[6] = p.curvature;
  }
};

pcl::visualization::PCLVisualizer::Ptr
rgbVis(pcl::PointCloud<PointType>::ConstPtr cloud) {
  // --------------------------------------------
  // -----Open 3D viewer and add point cloud-----
  // --------------------------------------------

  boost::shared_ptr<pcl::visualization::PCLVisualizer> viewer(
      new pcl::visualization::PCLVisualizer("3D Viewer"));
  viewer->setBackgroundColor(0, 0, 0);
  pcl::visualization::PointCloudColorHandlerRGBField<PointType> rgb(cloud);
  viewer->addPointCloud<PointType>(cloud, rgb, "sample cloud");
  viewer->setPointCloudRenderingProperties(
      pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 2, "sample cloud");
  viewer->addCoordinateSystem(1.0);
  viewer->initCameraParameters();
  viewer->registerKeyboardCallback(
      [&, viewer](const pcl::visualization::KeyboardEvent &kb) {
        pcl::visualization::Camera c;
        viewer->getCameraParameters(c);

        Eigen::Map<Eigen::Vector3d> focal(c.focal);
        Eigen::Map<Eigen::Vector3d> pos(c.pos);
        Eigen::Vector3d view = (focal - pos).normalized();
        view[2] = 0;
        view.normalize();
        const double incMag = (kb.isShiftPressed() ? 1.0 : 0.5);

        Eigen::Vector3d incDir = incMag * view;
        Eigen::Vector3d perpInc =
            incMag * Eigen::Vector3d(-view[1], view[0], view[2]);

        if (kb.getKeySym() == "Up") {
          if (!kb.isCtrlPressed())
            focal += incDir;
          pos += incDir;
        }
        if (kb.getKeySym() == "Down") {
          if (!kb.isCtrlPressed())
            focal -= incDir;
          pos -= incDir;
        }
        if (kb.getKeySym() == "Left") {
          if (!kb.isCtrlPressed())
            focal += perpInc;
          pos += perpInc;
        }
        if (kb.getKeySym() == "Right") {
          if (!kb.isCtrlPressed())
            focal -= perpInc;
          pos -= perpInc;
        }
        viewer->setCameraParameters(c);
      });
  viewer->setCameraPosition(1, 0, 0, -1, 0, 0, 0, 0, 1);
  return (viewer);
}

std::tuple<Eigen::Array3f, Eigen::Array3f, Eigen::Matrix4f>
createPCLPointCloud(std::list<scan::PointXYZRGBA> &points,
                    pcl::PointCloud<PointType>::Ptr &cloud,
                    const Eigen::Matrix3d &rotMat,
                    const Eigen::Vector3d &trans);
pcl::PointCloud<NormalType>::Ptr
subsample_normals(const pcl::PointCloud<PointType>::Ptr &cloud);

constexpr double targetNumPoints = 100e6;
constexpr double startScale = 0.02;

bool sanity_check(const Eigen::Matrix4f &T) {
  for (int i = 0; i < 2; ++i)
    if (std::abs(T(i, 3)) >= 0.2)
      return false;

  if (std::abs(T(2, 3)) >= 0.1)
    return false;

  Eigen::Vector3f x, y, z;

  for (int j = 0; j < 3; ++j) {
    x[j] = T(0, j);
    y[j] = T(1, j);
    z[j] = T(2, j);
  }

  if (std::acos(std::abs(x.dot(Eigen::Vector3f::UnitX()))) >= 0.05)
    return false;

  if (std::acos(std::abs(y.dot(Eigen::Vector3f::UnitY()))) >= 0.05)
    return false;

  if (std::acos(std::abs(z.dot(Eigen::Vector3f::UnitZ()))) >= 0.05)
    return false;

  return true;
}

int main(int argc, char **argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  prependDataPath();

  FLAGS_transformation_folder =
      (fs::path(FLAGS_dataPath) / FLAGS_transformation_folder).string();
  fs::create_directories(FLAGS_transformation_folder);

  std::vector<fs::path> binaryFileNames;
  utils::parse_folder(FLAGS_binaryFolder, binaryFileNames);
  auto[buildName, tmp] = parse_name(binaryFileNames[0]);
  const fs::path cloudName =
      fs::path(FLAGS_outputV2) / "{}_pointCloud.ply"_format(buildName);
  const fs::path all_rots_file =
      fs::path(FLAGS_transformation_folder) / "all_transformations.txt";
  std::ofstream all_out(all_rots_file.string(), std::ios::out);
  fmt::print("{}\n", cloudName);

  pcl::PointCloud<PointType>::Ptr output_cloud(new pcl::PointCloud<PointType>);

  if (FLAGS_redo || !fs::exists(cloudName)) {
    const fs::path fileName = fs::path(FLAGS_outputV2) / "final_0.dat";
    CHECK(fs::exists(fileName)) << "Could not find " << fileName;

    std::ifstream in(fileName.string(), std::ios::in | std::ios::binary);
    int num;
    in.read(reinterpret_cast<char *>(&num), sizeof(num));
    std::cout << num << std::endl;

    std::vector<Eigen::Matrix3d> rotMats(num);
    std::vector<Eigen::Vector3d> translations(num);

    for (int i = 0; i < num; ++i) {
      in.read(reinterpret_cast<char *>(rotMats[i].data()),
              sizeof(Eigen::Matrix3d));
      in.read(reinterpret_cast<char *>(translations[i].data()),
              sizeof(Eigen::Vector3d));
    }
    in.close();
    assert(num <= binaryFileNames.size());
    double subSampleSize = startScale;

    if (FLAGS_numScans != -1)
      num = FLAGS_numScans;

    for (int k = FLAGS_startIndex;
         k < std::min((int)rotMats.size(), FLAGS_startIndex + num); ++k) {
      std::cout << "Enter: " << binaryFileNames[k] << std::endl;

      auto[build_name, scan_number] = parse_name(binaryFileNames[k]);
      fmt::print(all_out, "{}\n\n", scan_number);

      if (rotMats[k] == Eigen::Matrix3d::Zero())
        continue;

      in.open(binaryFileNames[k].string(), std::ios::in | std::ios::binary);
      int rows, cols;
      in.read(reinterpret_cast<char *>(&rows), sizeof(rows));
      in.read(reinterpret_cast<char *>(&cols), sizeof(cols));
      std::list<scan::PointXYZRGBA> points(rows * cols);
      for (auto &p : points)
        p.loadFromFile(in);
      in.close();

      pcl::PointCloud<PointType>::Ptr current_cloud(
          new pcl::PointCloud<PointType>);
      Eigen::Array3f min, max;
      Eigen::Matrix4f transformation;
      std::tie(min, max, transformation) = createPCLPointCloud(
          points, current_cloud, rotMats[k], translations[k]);

      const fs::path out_name =
          fs::path(FLAGS_transformation_folder) /
          "{}_trans_{}.txt"_format(build_name, scan_number);

      if (!FLAGS_quietMode)
        std::cout << out_name << std::endl;

      std::ofstream trans_out(out_name.string(), std::ios::out);
      fmt::print(trans_out, "Before general icp:\n"
                            "{}\n\n",
                 transformation);
      fmt::print(all_out, "Before general icp:\n"
                          "{}\n\n",
                 transformation);

      bool run_icp = output_cloud->size() > 0;

      if (run_icp) {
        pcl::PointCloud<PointType>::Ptr icp_target(
            new pcl::PointCloud<PointType>);

        for (auto &point : *output_cloud) {
          bool in = true;
          for (int i = 0; i < 3; ++i)
            if (point.getVector3fMap()[i] < min[i] ||
                point.getVector3fMap()[i] > max[i])
              in = false;

          if (in)
            icp_target->push_back(point);
        }

        fmt::print("Traget size: {}\t"
                   "Current size: {}\n",
                   icp_target->size(), current_cloud->size());

        if (icp_target->size() > 0.25 * current_cloud->size()) {

          pcl::search::KdTree<NormalType>::Ptr tree_source(
              new pcl::search::KdTree<NormalType>),
              tree_target(new pcl::search::KdTree<NormalType>);

          MyPointRepresentation point_representation;
          // ... and weight the 'curvature' dimension so that it is balanced
          // against x, y, and z
          float alpha[] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
          point_representation.setRescaleValues(alpha);

          pcl::GeneralizedIterativeClosestPoint<NormalType, NormalType> icp;
          icp.setPointRepresentation(
              boost::make_shared<const MyPointRepresentation>(
                  point_representation));
          icp.setSearchMethodTarget(tree_target);
          icp.setSearchMethodSource(tree_source);

          // icp.setMaximumIterations(5);
          // icp.setRANSACIterations(5e3);

          auto source_with_normals = subsample_normals(current_cloud);
          auto target_with_normals = subsample_normals(icp_target);

          icp.setInputSource(source_with_normals);
          icp.setInputTarget(target_with_normals);
          icp.setTransformationEpsilon(1e-6);
          icp.setMaxCorrespondenceDistance(1e-1);

          Eigen::Matrix4f Ti = Eigen::Matrix4f::Identity(), prev,
                          targetToSource;
          auto icp_result = source_with_normals;
          icp.setMaximumIterations(2);
          std::list<Eigen::Matrix4f> prev_4;
          for (int i = 0; i < 30 && run_icp; ++i) {
            // save cloud for visualization purpose
            source_with_normals = icp_result;

            // Estimate
            icp.setInputSource(source_with_normals);
            icp.align(*icp_result);

            // accumulate transformation between each Iteration
            Ti = icp.getFinalTransformation() * Ti;

            if (!sanity_check(Ti) || icp.getFitnessScore() > 20) {
              run_icp = false;
              break;
            }

            // if the difference between this transformation and the previous
            // one
            // is smaller than the threshold, refine the process by reducing
            // the maximal correspondence distance
            if (fabs((icp.getLastIncrementalTransformation() - prev).sum()) <
                icp.getTransformationEpsilon())
              icp.setMaxCorrespondenceDistance(
                  icp.getMaxCorrespondenceDistance() - 0.001);

            prev = icp.getLastIncrementalTransformation();

            if (prev_4.size() == 4) {
              double ave = 0;
              for (auto &m : prev_4)
                ave += (Ti - m).norm();

              ave /= 4;

              if (ave < icp.getTransformationEpsilon())
                break;

              prev_4.pop_front();
            }
            prev_4.emplace_back(Ti);
          }

          fmt::print("ICP worked: {}\n"
                     "has converged: {}\n"
                     "score: {}\n"
                     "transformation:\n{}\n\n",
                     run_icp, icp.hasConverged(), icp.getFitnessScore(), Ti);

          if (run_icp) {
            auto tmp = current_cloud;
            current_cloud =
                pcl::PointCloud<PointType>::Ptr(new pcl::PointCloud<PointType>);
            pcl::transformPointCloud(*tmp, *current_cloud, Ti);

            transformation = Ti * transformation;
          }
        }
      }

      fmt::print("Final transformation:\n"
                 "{}\n\n",
                 transformation);

      fmt::print(trans_out, "After general icp:\n"
                            "{}\n\n",
                 transformation);
      fmt::print(all_out, "After general icp:\n"
                          "{}\n\n",
                 transformation);
      trans_out.close();

      output_cloud->insert(output_cloud->end(), current_cloud->begin(),
                           current_cloud->end());

      current_cloud = nullptr;

      pcl::UniformSampling<PointType> uniform_sampling;
      uniform_sampling.setInputCloud(output_cloud);
      output_cloud =
          pcl::PointCloud<PointType>::Ptr(new pcl::PointCloud<PointType>);
      uniform_sampling.setRadiusSearch(subSampleSize);
      uniform_sampling.filter(*output_cloud);

      if (output_cloud->size() > targetNumPoints) {
        subSampleSize *= std::sqrt(output_cloud->size() / targetNumPoints);

        output_cloud =
            pcl::PointCloud<PointType>::Ptr(new pcl::PointCloud<PointType>);
        uniform_sampling.setRadiusSearch(subSampleSize);
        uniform_sampling.filter(*output_cloud);
      }

#if 0
      pcl::visualization::PCLVisualizer::Ptr viewer = rgbVis(output_cloud);
      while (!viewer->wasStopped()) {
        viewer->spinOnce(100);
        boost::this_thread::sleep(boost::posix_time::microseconds(100000));
      }
#endif

      fmt::print("Leaving: {}\n", output_cloud->size());
    }

    fmt::print("Final sample size: {}\n", subSampleSize);

    pcl::StatisticalOutlierRemoval<PointType> sor;
    sor.setInputCloud(output_cloud);
    sor.setMeanK(50);
    sor.setStddevMulThresh(2.0);
    output_cloud =
        pcl::PointCloud<PointType>::Ptr(new pcl::PointCloud<PointType>);
    sor.filter(*output_cloud);

    std::cout << "Saving" << std::endl;
    pcl::io::savePLYFileBinary(cloudName.string(), *output_cloud);
  } else
    pcl::io::loadPLYFile(cloudName.string(), *output_cloud);

  if (FLAGS_visulization) {
    pcl::UniformSampling<PointType> uniform_sampling;
    uniform_sampling.setInputCloud(output_cloud);
    pcl::PointCloud<PointType>::Ptr ss(new pcl::PointCloud<PointType>);
    uniform_sampling.setRadiusSearch(0.05);
    uniform_sampling.filter(*ss);
    pcl::visualization::PCLVisualizer::Ptr viewer = rgbVis(ss);
    while (!viewer->wasStopped()) {
      viewer->spinOnce(100);
      boost::this_thread::sleep(boost::posix_time::microseconds(100000));
    }
  }
}

void boundingBox(const pcl::PointCloud<PointType>::Ptr &cloud,
                 Eigen::Array3f &pointMin, Eigen::Array3f &pointMax,
                 const Eigen::Array3f &range) {
  Eigen::Array3f average = Eigen::Array3f::Zero();
  Eigen::Array3f sigma = Eigen::Array3f::Zero();

  for (auto &point : *cloud)
    average += point.getVector3fMap().array();

  average /= cloud->size();

  for (auto &point : *cloud)
    sigma += (average - point.getVector3fMap().array()).square();

  sigma /= cloud->size() - 1;
  sigma = sigma.sqrt();

  auto delta = 1.1 * range * sigma;

  pointMin = average - delta / 2.0;
  pointMax = average + delta / 2.0;
}

void boundingBox(const pcl::PointCloud<PointType>::Ptr &cloud,
                 Eigen::Array3f &pointMin, Eigen::Array3f &pointMax) {
  boundingBox(cloud, pointMin, pointMax, Eigen::Array3f(3.5, 3.5, 4.0));
}

std::tuple<Eigen::Array3f, Eigen::Array3f, Eigen::Matrix4f>
createPCLPointCloud(std::list<scan::PointXYZRGBA> &points,
                    pcl::PointCloud<PointType>::Ptr &cloud,
                    const Eigen::Matrix3d &rotMat,
                    const Eigen::Vector3d &trans) {

  const Eigen::Vector3d corrected_trans(trans[0], trans[1], 0);

  Eigen::Vector3d x, y, z;
  Eigen::Matrix3d corrected_rot;

  for (int j = 0; j < 3; ++j) {
    x[j] = rotMat(0, j);
    y[j] = rotMat(1, j);
    z[j] = rotMat(2, j);
  }

  x[2] = 0;
  x.normalize();
  y[2] = 0;
  y.normalize();
  z = Eigen::Vector3d::UnitZ();

  for (int j = 0; j < 3; ++j) {
    corrected_rot(0, j) = x[j];
    corrected_rot(1, j) = y[j];
    corrected_rot(2, j) = z[j];
  }

  corrected_rot = corrected_rot.inverse().eval();

  Eigen::Matrix4d T = Eigen::Matrix4d::Identity();

  for (int j = 0; j < 3; ++j) {
    for (int i = 0; i < 3; ++i) {
      T(j, i) = corrected_rot(j, i);
    }

    T(j, 3) = trans[j];
  }

  /* clang-format off */
  Eigen::Matrix4d correction;
  correction << 1,  0, 0, 0,
                0, -1, 0, 0,
                0,  0, 1, 0,
                0,  0, 0, 1;
  /* clang-format on */

  T = correction * T * correction;
  fmt::print("Transformation before ICP:\n{}\n\n", T);

  for (auto it = points.begin(); it != points.end();) {
    auto &p = *it;

    Eigen::Vector3d point =
        (T * p.point.cast<double>().homogeneous()).eval().hnormalized();

    auto &rgb = p.rgb;
    PointType tmp;
    tmp.getVector3fMap() = point.cast<float>();
    tmp.r = cv::saturate_cast<uint8_t>(rgb[0]);
    tmp.g = cv::saturate_cast<uint8_t>(rgb[1]);
    tmp.b = cv::saturate_cast<uint8_t>(rgb[2]);
    cloud->push_back(tmp);

    it = points.erase(it);
  }

  pcl::UniformSampling<PointType> uniform_sampling;
  uniform_sampling.setInputCloud(cloud);
  cloud = pcl::PointCloud<PointType>::Ptr(new pcl::PointCloud<PointType>);
  uniform_sampling.setRadiusSearch(0.85 * startScale);
  uniform_sampling.filter(*cloud);

  Eigen::Array3f min, max;
  boundingBox(cloud, min, max);

  std::cout << min << std::endl << std::endl << max << std::endl << std::endl;

  cloud->erase(std::remove_if(*cloud,
                              [&](auto &point) {
                                bool in = true;
                                for (int i = 0; i < 3; ++i)
                                  if (point.getVector3fMap()[i] < min[i] ||
                                      point.getVector3fMap()[i] > max[i])
                                    in = false;

                                return !in;
                              }),
               cloud->end());

  /*pcl::visualization::PCLVisualizer::Ptr viewer = rgbVis(cloud);
  while (!viewer->wasStopped()) {
    viewer->spinOnce(100);
    boost::this_thread::sleep(boost::posix_time::microseconds(100000));
  }*/

  uniform_sampling.setInputCloud(cloud);
  cloud = pcl::PointCloud<PointType>::Ptr(new pcl::PointCloud<PointType>);
  uniform_sampling.setRadiusSearch(startScale);
  uniform_sampling.filter(*cloud);

  boundingBox(cloud, min, max, Eigen::Array3f(3., 3., 3.));

  return std::make_tuple(min, max, T.cast<float>());
}

pcl::PointCloud<NormalType>::Ptr
subsample_normals(const pcl::PointCloud<PointType>::Ptr &cloud) {
  pcl::UniformSampling<PointType> uniform_sampling;
  uniform_sampling.setInputCloud(cloud);
  pcl::PointCloud<PointType>::Ptr ss(new pcl::PointCloud<PointType>);
  uniform_sampling.setRadiusSearch(0.04);
  uniform_sampling.filter(*ss);

  pcl::NormalEstimationOMP<PointType, NormalType> norm_est;
  pcl::search::KdTree<PointType>::Ptr tree(
      new pcl::search::KdTree<PointType>());

  norm_est.setSearchSurface(cloud);
  norm_est.setSearchMethod(tree);
  norm_est.setKSearch(30);

  pcl::PointCloud<NormalType>::Ptr output(new pcl::PointCloud<NormalType>);
  norm_est.setInputCloud(ss);
  norm_est.compute(*output);
  pcl::copyPointCloud(*ss, *output);

  return output;
}