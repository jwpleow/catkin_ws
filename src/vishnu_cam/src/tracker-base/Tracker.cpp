#include "Tracker.h"

#include <iostream>
#include <iomanip>

#include <opencv2/aruco.hpp>
#include <opencv2/calib3d.hpp>
#include "SMA.hpp"

using namespace std;
using namespace cv;

const int SMAL = 5;
SMA ctSMA[3] {SMA(SMAL), SMA(SMAL), SMA(SMAL)};

Tracker::Tracker(CVCalibration &cvl, bool _showFrame) {
  cameraMatrix = cvl.cameraMatrix;
  distCoeffs = cvl.distCoeffs;
  frameWidth = cvl.frameWidth;
  frameHeight = cvl.frameHeight;
  showFrame = _showFrame;
  fovx = 2 * atan(frameWidth / (2 * cameraMatrix.at<double>(0, 0))) * (180.0 / pi);
  fovy = 2 * atan(frameHeight / (2 * cameraMatrix.at<double>(1, 1))) * (180.0 / pi);
  cout << "FOVx: " << fovx << "\tFOVy: " << fovy << "\tFrame Width: "
       << frameWidth << "\tFrame Height: " << frameHeight << endl;
}

double _avgdur = 0;
int _fpsstart = 0;
double _avgfps = 0;
double _fps1sec = 0;

int CLOCK() {
  struct timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return static_cast<int>((t.tv_sec * 1000) + (t.tv_nsec * 1e-6));
}

double avgDur(double newdur) {
  _avgdur = 0.98 * _avgdur + 0.02 * newdur;
  return _avgdur;
}

double avgFPS() {
  if (CLOCK() - _fpsstart > 1000) {
    _fpsstart = CLOCK();
    _avgfps = 0.7 * _avgfps + 0.3 * _fps1sec;
    _fps1sec = 0;
  }
  
  _fps1sec++;
  return _avgfps;
}

// Checks if a matrix is a valid rotation matrix.
bool isRotationMatrix(Mat &R) {
  Mat Rt;
  transpose(R, Rt);
  Mat shouldBeIdentity = Rt * R;
  Mat I = Mat::eye(3, 3, shouldBeIdentity.type());
  return norm(I, shouldBeIdentity) < 1e-6;
}

Vec3d rotationMatrixToEulerAngles(Mat &R) {
  assert(isRotationMatrix(R));
  double sy = sqrt(R.at<double>(0, 0) * R.at<double>(0, 0) + R.at<double>(1, 0) * R.at<double>(1, 0));
  bool singular = sy < 1e-6; // If
  double x, y, z;
  if (!singular) {
    x = atan2(R.at<double>(2, 1), R.at<double>(2, 2));
    y = atan2(-R.at<double>(2, 0), sy);
    z = atan2(R.at<double>(1, 0), R.at<double>(0, 0));
  } else {
    x = atan2(-R.at<double>(1, 2), R.at<double>(1, 1));
    y = atan2(-R.at<double>(2, 0), sy);
    z = 0;
  }
  return Vec3d(x, y, z);
}

void Tracker::loopedTracking(VideoCapture vid) {
  Mat frame;
  Vec3d rVec, tVec, ctVec, sctVec;
  int frameno = 0;
  
  cout << "FrameNo\t\tTimestamp\t\t\tRunningTime\tFPS\t\tDist1\t\tDist2\t\tDist3\n";
  if (showFrame) namedWindow("Camera Feed", WINDOW_AUTOSIZE);
  while (true) {
    if (!vid.read(frame)) {
      cerr << "Unable to read next frame. Ending tracking.\n";
      break;
    };
    
    auto start = static_cast<clock_t>(CLOCK());
    if (getPose(frame, tVec, rVec) > 0) {
      correctedPose(rVec, tVec, ctVec);
      smaPose(ctVec, sctVec);
      
      double dur = CLOCK() - start;
      auto t = time(nullptr);
      auto tm = *localtime(&t);
      frameno++;
      
      cout << frameno << "\t"
           << put_time(&tm, "%H:%M:%S") << "\t"
           << avgDur(dur) << "\t"
           << avgFPS() << "\t"
           << ctVec[0] << "\t"
           << ctVec[1] << "\t"
           << ctVec[2] << "\t"
           << sctVec[0] << "\t"
           << sctVec[1] << "\t"
           << sctVec[2] << "\t"
           << endl;
    }
    if (showFrame) imshow("Camera Feed", frame);
    if (waitKey(60) >= 0) break;
  }
}

void Tracker::correctedPose(const Vec3d &rVec, const Vec3d &tVec, Vec3d &ctVec) const {
  Mat R_flip = Mat::zeros(3, 3, CV_64F);
  R_flip.at<double>(0, 0) = 1.0;
  R_flip.at<double>(1, 1) = -1.0;
  R_flip.at<double>(2, 2) = -1.0;
  
  Mat R_ct = Mat::eye(3, 3, CV_64F);
  Rodrigues(rVec, R_ct);
  Mat R_tc = R_ct.t();
  Mat rMat = R_flip * R_tc;
  Vec3d euler = rotationMatrixToEulerAngles(rMat);
  Matx<double, 1, 3> tVect = tVec.t();
  Mat tVecC = -1 * (R_tc * tVec).t();
  
  ctVec[0] = -1 * (tVecC.at<double>(0, 0) - 0); // TODO: Abstract this into the board logic
  ctVec[1] = tVecC.at<double>(0, 1) - 0;
  ctVec[2] = tVecC.at<double>(0, 2);
}

void Tracker::smaPose(const Vec3d &ctVec, Vec3d &sctVec) {
  for(int i = 0; i < 3; ++i) {
    ctSMA[i].add(ctVec[i]);
    sctVec[i] = ctSMA[i].avg();
  }
}

bool Tracker::startStreamingTrack(int port) {
  VideoCapture vid(port);
  vid.set(CAP_PROP_FRAME_WIDTH, frameWidth);
  vid.set(CAP_PROP_FRAME_HEIGHT, frameHeight);
  if (!vid.isOpened()) {
    cerr << "Unable to read video stream. Is the camera mount path correct?\n";
    return false;
  }
  loopedTracking(vid);
  return true;
}

bool Tracker::startVideoTrack(const string &fname) {
  VideoCapture vid(fname);
  if (!vid.isOpened()) {
    cerr << "Unable to read video file. Is the filepath correct?\n";
    return false;
  }
  loopedTracking(vid);
  return true;
}

