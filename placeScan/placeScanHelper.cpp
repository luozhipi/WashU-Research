#include "placeScan_placeScanHelper.h"

#include <algorithm>

#include <dirent.h>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <iostream>
#include <fstream>


DEFINE_bool(visulization, false, 
	"Turns on all visualization options that do not impact performance");
DEFINE_bool(previewIn, false, "Turns on a preview of the scan before it is placed");
DEFINE_bool(previewOut, true, "Shows a preview of the scans placement before saving");
DEFINE_bool(redo, false, "Forces the program to redo the placement of all scans given");
DEFINE_bool(quiteMode, false, "Very minimal status reports");
DEFINE_bool(tinyPreviewIn, false, "Shows the input scan before any processing");
DEFINE_bool(save, true, "Saves the placements to file");
DEFINE_bool(debugMode, false, 
	"Turns save off, turns replace on, and forces the program to display the correct placement according to the placement files specified by the preDone flag");
DEFINE_bool(reshow, true, "Reshows the placement from a previous run");
DEFINE_bool(V1, false, "Only will use V1 placement");
DEFINE_bool(V2, false, "Ony will use V2 placement");
DEFINE_string(floorPlan, "floorPlan.png", 
	"Path to the floor plan that the scan should be placed on.  This will be appended to the dataPath.");
DEFINE_string(dmFolder, "densityMaps/",
	"Path to folder containing densityMaps. This will be appended to the dataPath.");
DEFINE_string(preDone, "placementOptions/V1/",
	"Path to folder containing previous placements of a scan. This will be appended to the dataPath.");
DEFINE_string(preDoneV2, "placementOptions/V2/",
	"Path to folder containing previous placements of a scan. This will be appended to the dataPath.");
DEFINE_string(zerosFolder, "densityMaps/zeros/",
	"Path to folder where the pixel coordinates of (0,0) are. This will be appended to the dataPath.");
DEFINE_string(voxelFolder, "voxelGrids/",
	"Path to the folder where the voxelGrids are saved to. This will be appended to the dataPath.");
DEFINE_string(dataPath, "/home/erik/Projects/3DscanData/DUC/Floor1/",
	"Path to where the program should search for the various folders it needs");
DEFINE_int32(startIndex, 0, "Scan number to start with");
DEFINE_int32(numScans, -1, 
	"Number of scans to place, default or -1 will cause all scans in the folder to placed");
DEFINE_int32(numLevels, 5, "Number of levels in the pyramid");
DEFINE_int32(metricNumber, 3, "Which metric version the algorithm uses for placement");
DEFINE_int32(top, -1, "Only shows the top x placements, -1=ALL");

cv::Mat fpColor, floorPlan;
std::vector<Eigen::Vector3i> truePlacement;


std::ostream & operator<<(std::ostream & os, const place::posInfo * print) {
	os << print->score <<"      " << print->x << "      " <<print->y << std::endl;
	os << print->scanFP << "      " << print->fpScan << std::endl;
	os << print->scanPixels << "    " << print->fpPixels;
	return os;
}

void place::parseFolders(std::vector<std::string> & pointFileNames, 
	std::vector<std::string> & zerosFileNames,
	std::vector<std::string> * freeFileNames){
	
	DIR *dir;
	struct dirent *ent;
	const std::string newDmFolder = FLAGS_dmFolder + "R0/";
	if ((dir = opendir (newDmFolder.data())) != NULL) {
	  while ((ent = readdir (dir)) != NULL) {
	  	std::string fileName = ent->d_name;
	  	if(fileName != ".." && fileName != "." 
	  		&& fileName.find("point") != std::string::npos){
	  		pointFileNames.push_back(fileName);
	  	} else if (freeFileNames && fileName != ".." && fileName != "." 
	  		&& fileName.find("freeSpace") != std::string::npos) {
	  		freeFileNames->push_back(fileName);
	  	}
	  }
	  closedir (dir);
	}  else {
	  /* could not open directory */
	  perror ("");
	  exit(-1);
	}

	if ((dir = opendir (FLAGS_zerosFolder.data())) != NULL) {
	  while ((ent = readdir (dir)) != NULL) {
	  	std::string fileName = ent->d_name;
	  	if(fileName != ".." && fileName != "." ){
	  		zerosFileNames.push_back(fileName);
	  	}
	  }
	  closedir (dir);
	}  else {
	  /* could not open directory */
	  perror ("");
	  exit(-1);
	}

	if(pointFileNames.size() != zerosFileNames.size()){
		perror("Not the same number of scans as rotations!");
		exit(-1);
	}

	sort(pointFileNames.begin(), pointFileNames.end());
	sort(zerosFileNames.begin(), zerosFileNames.end());
	if(freeFileNames)
		sort(freeFileNames->begin(), freeFileNames->end());
}


void place::loadInScans(const std::string & scanName,
	 const std::string & zerosFile, std::vector<cv::Mat> & rotatedScans,
   std::vector<Eigen::Vector2i> * zeroZero) {

	if(zeroZero) {
		zeroZero->resize(NUM_ROTS);
		std::ifstream binaryReader (zerosFile, std::ios::in | std::ios::binary);
		for (int i = 0; i < NUM_ROTS; ++i) {
			binaryReader.read(reinterpret_cast<char *>((*zeroZero)[i].data()),
				sizeof(Eigen::Vector2i));
		}
		binaryReader.close();
	}
	
	
	for(int i = 0; i < NUM_ROTS; ++i) {
		std::string fullScanName = FLAGS_dmFolder + "R" + std::to_string(i) + "/"
			+ scanName;

		rotatedScans.push_back(cv::imread(fullScanName, 0));

		if(!rotatedScans[i].data){
			std::cout << "Error reading scan" << std::endl;
			exit(1);
		}
	}

	
	if(FLAGS_tinyPreviewIn || FLAGS_visulization) {
		cvNamedWindow("Preview", CV_WINDOW_NORMAL);
		cv::imshow("Preview", rotatedScans[0]);
		cv::waitKey(0);
	}
}

void place::loadInScansAndMasks(const std::string & scanName, 
   	const std::string & zerosFile, 
    const std::string & maskName, std::vector<cv::Mat> & rotatedScans,
    std::vector<cv::Mat> & masks, std::vector<Eigen::Vector2i> & zeroZero) {

    place::loadInScans(scanName, zerosFile, rotatedScans, &zeroZero);
    place::loadInScans(maskName, zerosFile, masks, NULL);
}

void place::trimScans(const std::vector<cv::Mat> & toTrim, 
	std::vector<cv::Mat> & trimmedScans, std::vector<Eigen::Vector2i> & zeroZero) {
	int k = 0;
	for(auto & scan : toTrim){
		int minRow = scan.rows;
		int minCol = scan.cols;
		int maxRow = 0;
		int maxCol = 0;

		for (int i = 0; i < scan.rows; ++i) {
			const uchar * src = scan.ptr<uchar>(i);
			for (int j = 0; j < scan.cols; ++j) {
				if(src[j] != 255){
					minRow = std::min(i, minRow);
					minCol = std::min(j, minCol);
					maxRow = std::max(i, maxRow);
					maxCol = std::max(j, maxCol);
				}
			}
		}

		cv::Mat trimmedScan (maxRow - minRow + 1, maxCol - minCol + 1, CV_8UC1);
		for (int i = minRow; i < maxRow + 1; ++i) {
			const uchar * src = scan.ptr<uchar>(i);
			uchar * dst = trimmedScan.ptr<uchar>(i-minRow);
			for (int j = minCol; j < maxCol + 1; ++j)
			{
				dst[j-minCol] = src[j];
			}
		}

		trimmedScans.push_back(trimmedScan);
		zeroZero[k][0] -= minCol;
		zeroZero[k][1] -= minRow;
		++k;
	}
}

static void normalize(const std::vector<const place::posInfo *> & minima,
	std::vector<place::posInfo> & out) {
	double average = 0;
	for(auto & m : minima)
		average += m->score;
	average /= minima.size();

	double sigma = 0;
	for(auto & m : minima)
		sigma += (m->score - average)*(m->score - average);
	sigma /= minima.size() - 1;
	sigma = sqrt(sigma);

	for(auto & m : minima) {
		place::posInfo minScore = *m;
		minScore.score = (minScore.score - average)/sigma;
		out.push_back(minScore);
	}
}

void place::savePlacement(const std::vector<const place::posInfo *> & minima,
	const std::string & outName, const std::vector<Eigen::Vector2i> & zeroZero){
	std::ofstream out (outName, std::ios::out);
	std::ofstream outB (outName.substr(0, outName.find(".")) + ".dat", std::ios::out | std::ios::binary);

	/*std::vector<place::posInfo> outVec;
	normalize(minima, outVec);*/

	out << "Score x y rotation" << std::endl;
	const int num = minima.size() < 20 ? minima.size() : 20;
	outB.write(reinterpret_cast<const char *>(&num), sizeof(num));
	for(auto & min : minima){
		place::posInfo minScore = *min;
		minScore.x += zeroZero[minScore.rotation][0];
		minScore.y += zeroZero[minScore.rotation][1];
		out << minScore.score << " " << minScore.x  << " "
			<< minScore.y << " " << minScore.rotation << std::endl;
		
		outB.write(reinterpret_cast<const char *> (&minScore), sizeof(minScore));
	}
	out.close();
	outB.close();
}

bool place::reshowPlacement(const std::string & scanName,
	const std::string & zerosFile, const std::string & preDone) {
	const std::string placementName = preDone + scanName.substr(scanName.find("_")-3, 3) 
	+ "_placement_" + scanName.substr(scanName.find(".")-3, 3) + ".dat";

	std::ifstream in (placementName, std::ios::in | std::ios::binary);
	if(!in.is_open())
		return false;
	if(!FLAGS_reshow)
		return true;

	if(!FLAGS_quiteMode)
		std::cout << placementName << std::endl;
	
	std::vector<cv::Mat> rotatedScans, toTrim;
  std::vector<Eigen::Vector2i> zeroZero;
	place::loadInScans(scanName, zerosFile, toTrim, &zeroZero);
	place::trimScans(toTrim, rotatedScans, zeroZero);

	int num;
	in.read(reinterpret_cast<char *>(&num), sizeof(num));
	num = FLAGS_top > 0 && num > FLAGS_top ? FLAGS_top : num;

	cvNamedWindow("Preview", CV_WINDOW_NORMAL);


	if(!FLAGS_quiteMode)
		std::cout << "Showing minima: " << num << std::endl;
	std::vector<place::posInfo> scores;
	for (int i = 0; i < num; ++i) {
		place::posInfo minScore;
		in.read(reinterpret_cast<char *>(&minScore), sizeof(minScore));

		const cv::Mat & bestScan = rotatedScans[minScore.rotation];

		const int xOffset = minScore.x - zeroZero[minScore.rotation][0];
		const int yOffset = minScore.y - zeroZero[minScore.rotation][1];

		cv::Mat output (fpColor.rows, fpColor.cols, CV_8UC3);
		fpColor.copyTo(output);
		
		for (int i = 0; i < bestScan.rows; ++i) {
			if(i + yOffset < 0 || i + yOffset >= fpColor.rows)
				continue;

			const uchar * src = bestScan.ptr<uchar>(i);
			uchar * dst = output.ptr<uchar>(i + yOffset);
			for (int j = 0; j < bestScan.cols; ++j) {
				if(j + xOffset < 0 || j + xOffset >= fpColor.cols)
					continue;

				if(src[j]!=255){
					dst[j*3 + xOffset*3] = 0;
					dst[j*3 + xOffset*3 + 1] = 0;
					dst[j*3 + xOffset*3 + 2] = 255 - src[j];
				}
			}
		}

		for(int i = -10; i < 10; ++i) {
			uchar * dst = output.ptr<uchar>(i + minScore.y);
			for(int j = -10; j < 10; ++j) {
				dst[j*3 + minScore.x*3 + 0] = 255;
				dst[j*3 + minScore.x*3 + 1] = 0;
				dst[j*3 + minScore.x*3 + 2] = 0;
			}
		}

		if(!FLAGS_quiteMode) {
			std::cout << &minScore << std::endl;
			std::cout << "% of scan unexplained: " << minScore.scanFP/minScore.scanPixels << std::endl << std::endl;
		}
		cv::imshow("Preview", output);
		cv::waitKey(0);
	}
	return true;
}

void place::displayOutput(const std::vector<Eigen::SparseMatrix<double> > & rSSparseTrimmed, 
	const std::vector<const place::posInfo *> & minima,
  const std::vector<Eigen::Vector2i> & zeroZero) {
	const int num = minima.size() < 20 ? minima.size() : 20;
	if(!FLAGS_quiteMode) {
		std::cout << "Num minima: " << num << std::endl;
		std::cout << "Press a key to begin displaying placement options" << std::endl;
	}
	
	cvNamedWindow("Preview", CV_WINDOW_NORMAL);
	cv::imshow("Preview", fpColor);
	cv::waitKey(0);
	const int cutOff = FLAGS_top > 0 ? FLAGS_top : 20;

	int currentCount = 0;
	for(auto & min : minima){
		const int xOffset = min->x;
		const int yOffset = min->y;
		const Eigen::SparseMatrix<double> & currentScan = 
			rSSparseTrimmed[min->rotation]; 
		cv::Mat output (fpColor.rows, fpColor.cols, CV_8UC3, cv::Scalar::all(255));
		fpColor.copyTo(output);

		cv::Mat_<cv::Vec3b> _output = output;

		for (int i = 0; i < currentScan.outerSize(); ++i) {
			for(Eigen::SparseMatrix<double>::InnerIterator it(currentScan, i); it; ++it){
				if(it.row() + yOffset < 0 || it.row() + yOffset >= output.rows)
					continue;
				if(it.col() + xOffset < 0 || it.col() + xOffset >= output.cols)
					continue;
				
				_output(it.row() + yOffset, it.col() + xOffset)[0]=0;
				_output(it.row() + yOffset, it.col() + xOffset)[1]=0;
				_output(it.row() + yOffset, it.col() + xOffset)[2]=255;

			}
		}

		cv::imshow("Preview", output);
		if(!FLAGS_quiteMode) {
			std::cout << min << std::endl << std::endl;
    }
		cv::waitKey(0);
		~output;
		if(++currentCount == cutOff) break;
	}
}

void place::displayOutput(const Eigen::SparseMatrix<double> & fp,
	const std::vector<Eigen::SparseMatrix<double> > & rSSparseTrimmed, 
	const std::vector<const place::posInfo *> & minima) {
	const int num = minima.size() < 20 ? minima.size() : 20;
	if(!FLAGS_quiteMode) {
		std::cout << "Num minima: " << num << std::endl;
		std::cout << "Press a key to begin displaying placement options" << std::endl;
	}
	cv::Mat fpImg = place::sparseToImage(fp);
	cv::Mat tmpColor (fpImg.rows, fpImg.cols, CV_8UC3, cv::Scalar::all(255));

	for (int i = 0; i < tmpColor.rows; ++i) {
    uchar * dst = tmpColor.ptr<uchar>(i);
    const uchar * src = fpImg.ptr<uchar>(i);
    for (int j = 0; j < tmpColor.cols; ++j) {
      if(src[j]!=255) {
        dst[j*3] = 128;
        dst[j*3+1] = 128;
        dst[j*3+2] = 128;
      }
    }
  }
	
	cvNamedWindow("Preview", CV_WINDOW_NORMAL);
	cv::imshow("Preview", tmpColor);
	cv::waitKey(0);
	const int cutOff = FLAGS_top > 0 ? FLAGS_top : 20;

	int currentCount = 0;
	for(auto & min : minima){
		const int xOffset = min->x;
		const int yOffset = min->y;
		const Eigen::SparseMatrix<double> & currentScan = 
			rSSparseTrimmed[min->rotation]; 
		cv::Mat output (tmpColor.rows, tmpColor.cols, CV_8UC3, cv::Scalar::all(255));
		tmpColor.copyTo(output);
		cv::Mat_<cv::Vec3b> _output = output;

		for (int i = 0; i < currentScan.outerSize(); ++i) {
			for(Eigen::SparseMatrix<double>::InnerIterator it(currentScan, i); it; ++it){
				if(it.row() + yOffset < 0 || it.row() + yOffset >= output.rows)
					continue;
				if(it.col() + xOffset < 0 || it.col() + xOffset >= output.cols)
					continue;
				
				_output(it.row() + yOffset, it.col() + xOffset)[0]=0;
				_output(it.row() + yOffset, it.col() + xOffset)[1]=0;
				_output(it.row() + yOffset, it.col() + xOffset)[2]=255;

			}
		}

		cv::imshow("Preview", output);
		if(!FLAGS_quiteMode) {
			std::cout << min << std::endl << std::endl;
    }
		cv::waitKey(0);
		~output;
		if(++currentCount == cutOff) break;
	}
}

void place::loadInTruePlacement(const std::string & scanName, 
  const std::vector<Eigen::Vector2i> & zeroZero){
	const std::string placementName = FLAGS_preDone + scanName.substr(scanName.find("_")-3, 3) 
	+ "_placement_" + scanName.substr(scanName.find(".")-3, 3) + ".dat";
	std::ifstream in (placementName, std::ios::in | std::ios::binary);

	int num;
	in.read(reinterpret_cast<char *>(&num), sizeof(num));

	std::vector<place::posInfo> tmp (num);
	for (int i = 0; i < num; ++i) {
		in.read(reinterpret_cast<char *>(&tmp[i]), sizeof(place::posInfo));
	}

	truePlacement.clear();
	for(auto & s : tmp){
		Eigen::Vector3i tmp2 (s.x - zeroZero[s.rotation][0], 
			s.y - zeroZero[s.rotation][1], s.rotation);
		truePlacement.push_back(tmp2);
	}
}

void place::displayTruePlacement(const std::vector<Eigen::SparseMatrix<double> > & rSSparseTrimmed,
	const std::vector<place::posInfo> & scores,
  const std::vector<Eigen::Vector2i> & zeroZero){

	std::vector<const place::posInfo *> tmp;
	for (int i = 0; i < scores.size(); ++i) {
		tmp.push_back(&scores[i]);
	}

	std::cout << "displaying true placement" << std::endl;
	place::displayOutput(rSSparseTrimmed, tmp, zeroZero);
}

void place::sparseToImage(const Eigen::SparseMatrix<double> & toImage,
	cv::Mat & imageOut){

	imageOut = cv::Mat(toImage.rows(), toImage.cols(), CV_8UC1, cv::Scalar::all(255));

	double maxV = 0;
	for (int i = 0; i < toImage.outerSize(); ++i) {
		for (Eigen::SparseMatrix<double>::InnerIterator it(toImage, i); it; ++it) {
			maxV = std::max(maxV, it.value());
		}
	}

	for (int i = 0; i < toImage.outerSize(); ++i) {
		for (Eigen::SparseMatrix<double>::InnerIterator it(toImage, i); it; ++it) {
			imageOut.at<uchar>(it.row(), it.col()) = 255 - 255*it.value()/maxV;
		}
	}
}

cv::Mat place::sparseToImage(const Eigen::SparseMatrix<double> & toImage){

	cv::Mat image (toImage.rows(), toImage.cols(), CV_8UC1, cv::Scalar::all(255));
	double maxV = 0;
	for (int i = 0; i < toImage.outerSize(); ++i) {
		for (Eigen::SparseMatrix<double>::InnerIterator it(toImage, i); it; ++it) {
			maxV = std::max(maxV, it.value());
		}
	}

	for (int i = 0; i < toImage.outerSize(); ++i) {
		for (Eigen::SparseMatrix<double>::InnerIterator it(toImage, i); it; ++it) {
			image.at<uchar>(it.row(), it.col()) = 255 - 255*it.value()/maxV;
		}
	}
	return image;
}

void place::scanToSparse(const cv::Mat & scan, 
	Eigen::SparseMatrix<double> & sparse) {
	std::vector<Eigen::Triplet<double> > tripletList;

	for (int i = 0; i < scan.rows; ++i) {
		const uchar * src = scan.ptr<uchar>(i);
		for (int j = 0; j < scan.cols; ++j) {
			if(src[j] == 255)
				continue;
			double confidence = 1.0 -(double)src[j]/255.0;
			tripletList.push_back(Eigen::Triplet<double> (i,j,confidence));
		}
	}
	sparse = Eigen::SparseMatrix<double>(scan.rows, scan.cols);
	sparse.setFromTriplets(tripletList.begin(), tripletList.end());
	sparse.makeCompressed();
	sparse.prune(1.0);
}

Eigen::SparseMatrix<double> place::scanToSparse(const cv::Mat & scan) {
	std::vector<Eigen::Triplet<double> > tripletList;

	for (int i = 0; i < scan.rows; ++i) {
		const uchar * src = scan.ptr<uchar>(i);
		for (int j = 0; j < scan.cols; ++j) {
			if(src[j] == 255)
				continue;
			double confidence = 1.0 -(double)src[j]/255.0;
			tripletList.push_back(Eigen::Triplet<double> (i,j,confidence));
		}
	}
	Eigen::SparseMatrix<double> sparseTmp (scan.rows, scan.cols);
	sparseTmp.setFromTriplets(tripletList.begin(), tripletList.end());
	sparseTmp.makeCompressed();
	sparseTmp.prune(1.0);

	return sparseTmp;
}

void place::displayScanAndMask(const std::vector<std::vector<Eigen::SparseMatrix<double> > > & rSSparsePyramidTrimmed,
	const std::vector<std::vector<Eigen::MatrixXb> > & eMaskPyramidTrimmedNS) {

	for(int i = 0; i < rSSparsePyramidTrimmed.size(); ++i) {
		for(int j = 0; j < rSSparsePyramidTrimmed[i].size(); ++j) {
			const Eigen::SparseMatrix<double> & currentScan = rSSparsePyramidTrimmed[i][j];
			const Eigen::MatrixXb & currentMask = eMaskPyramidTrimmedNS[i][j];
			cv::Mat out (currentScan.rows(), currentScan.cols(), CV_8UC3, cv::Scalar::all(255));

			for(int i = 0; i < out.rows; ++i) {
				uchar * dst = out.ptr<uchar>(i);
				for(int j = 0; j < out.cols; ++j) {
					if(currentMask(i,j) != 0) {
						dst[3*j] = 0;
						dst[3*j+1] = 0;
						dst[3*j+2] = 0;
					}
				}
			}

			cv::Mat_<cv::Vec3b> _out = out;
			for(int i = 0; i < currentScan.outerSize(); ++i) {
				for(Eigen::SparseMatrix<double>::InnerIterator it (currentScan, i); it; ++it) {
					if(it.value() > 0 && _out(it.row(), it.col())[0] == 0) {
						_out(it.row(), it.col())[0] = 0;
						_out(it.row(), it.col())[1] = 255;
						_out(it.row(), it.col())[2] = 0;
					} else if( it.value() > 0) {
						_out(it.row(), it.col())[0] = 0;
						_out(it.row(), it.col())[1] = 0;
						_out(it.row(), it.col())[2] = 255;
					}
				}
			}
			out = _out;
			cvNamedWindow("Preview", CV_WINDOW_NORMAL);
			cv::imshow("Preview", out);
			cv::waitKey(0);
		}
	}
}

void place::erodeSparse(const Eigen::SparseMatrix<double> & src,
	Eigen::SparseMatrix<double> & dst) {
	dst = Eigen::SparseMatrix<double>(src.rows(), src.cols());
	std::vector<Eigen::Triplet<double> > tripletList;
	Eigen::MatrixXd srcNS = Eigen::MatrixXd(src);

	for(int i = 0; i < srcNS.cols(); ++i) {
		for(int j = 0; j < srcNS.rows(); ++j) {
			double V = 0.0;
			for(int k = -1; k < 1; ++k) {
				for(int l = -1; l < 1; ++l) {
					if(i + k < 0 || i + k >=srcNS.cols() ||
						j+l < 0 || j + l >=srcNS.rows())
						continue;
					else
						V = std::max(V, srcNS(j + l, i + k));
				}
			}
			
			if(V != 0)
				tripletList.push_back(Eigen::Triplet<double> (j, i, V));
		}
	}
	dst.setFromTriplets(tripletList.begin(), tripletList.end());
}