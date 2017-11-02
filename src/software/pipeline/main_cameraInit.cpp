// This file is part of the AliceVision project.
// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include <aliceVision/image/image.hpp>
#include <aliceVision/sfm/sfm.hpp>
#include <aliceVision/exif/EasyExifIO.hpp>
#include <aliceVision/exif/sensorWidthDatabase/parseDatabase.hpp>
#include <aliceVision/stl/split.hpp>
#include <aliceVision/system/Logger.hpp>
#include <aliceVision/system/cmdline.hpp>

#include <dependencies/stlplus3/filesystemSimplified/file_system.hpp>

#include <boost/program_options.hpp>

#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <vector>

using namespace aliceVision;
using namespace aliceVision::camera;
using namespace aliceVision::exif;
using namespace aliceVision::image;
using namespace aliceVision::sfm;
namespace po = boost::program_options;

using ResourcePathsPerCamera = std::vector<std::vector<std::string>>;
using Resources = std::vector<ResourcePathsPerCamera>;
using ExifData = std::map<std::string, std::string>;

/**
 * @brief Check that Kmatrix is a string like "f;0;ppx;0;f;ppy;0;0;1"
 * @param[in] Kmatrix
 * @param[out] focal
 * @param[out] ppx
 * @param[out] ppy
 * @return true if the string is correct
 */
bool checkIntrinsicStringValidity(const std::string& Kmatrix,
                                  double& focal,
                                  double& ppx,
                                  double& ppy)
{
  std::vector<std::string> vec_str;
  stl::split(Kmatrix, ";", vec_str);
  if (vec_str.size() != 9)
  {
    ALICEVISION_LOG_ERROR("Error: In K matrix string, missing ';' character");
    return false;
  }

  // Check that all K matrix value are valid numbers
  for (size_t i = 0; i < vec_str.size(); ++i)
  {
    double readvalue = 0.0;
    std::stringstream ss;
    ss.str(vec_str[i]);
    if(!(ss >> readvalue))
    {
      ALICEVISION_LOG_ERROR("Error: In K matrix string, used an invalid not a number character");
      return false;
    }
    if (i==0) focal = readvalue;
    if (i==2) ppx = readvalue;
    if (i==5) ppy = readvalue;
  }
  return true;
}

/**
 * @brief Recursively list all files from a folder with a specific extension
 * @param[in] folderOrFile A file or foder path
 * @param[in] extensions An extensions filter
 * @param[out] outFiles A list of output image paths
 * @return true if folderOrFile have been load successfully
 */
bool listFiles(const std::string& folderOrFile,
               const std::vector<std::string>& extensions,
               std::vector<std::string>& resources)
{
  if(stlplus::is_file(folderOrFile))
  {
    std::string fileExtension = stlplus::extension_part(folderOrFile);
    std::transform(fileExtension.begin(), fileExtension.end(), fileExtension.begin(), ::tolower);
    for(const std::string& extension: extensions)
    {
      if(fileExtension == extension)
      {
        resources.push_back(folderOrFile);
        return true;
      }
    }
  }
  else if(stlplus::is_folder(folderOrFile))
  {
    // list all files of the folder
    const std::vector<std::string> allFiles = stlplus::folder_all(folderOrFile);
    if(allFiles.empty())
    {
      ALICEVISION_LOG_ERROR("Error: Folder '" << stlplus::filename_part(folderOrFile) <<"' is empty.");
      return false;
    }

    for(const std::string& item: allFiles)
    {
      const std::string itemPath = stlplus::create_filespec(folderOrFile, item);
      if(!listFiles(itemPath, extensions, resources))
        return false;
    }
  }
  else
  {
    ALICEVISION_LOG_ERROR("Error: '" << folderOrFile << "' is not a valid folder or file path.");
    return false;
  }
  return true;
}

/**
 * @brief Retrieve resources path from a json file
 * Need a "resource" variable in the json
 * @param jsonFile A JSON filepath
 * @param resourcesPaths list of resources
 * @return true if extraction is complete
 */
bool retrieveResources(const std::string& jsonFile,
                       const std::vector<std::string>& extensions,
                       Resources& resources)
{
  if(!stlplus::file_exists(jsonFile))
  {
    ALICEVISION_LOG_ERROR("File \"" << jsonFile << "\" does not exists.");
    return false;
  }

  // Read file
  std::ifstream jsonStream(jsonFile, std::ifstream::binary);

  if(!jsonStream.is_open())
    throw std::runtime_error("Error: Unable to open " + jsonFile);

  // get length of file:
  jsonStream.seekg (0, jsonStream.end);
  const int length = jsonStream.tellg();
  jsonStream.seekg (0, jsonStream.beg);
  // read data as a block:
  std::string jsonString;
  jsonString.resize(length);
  jsonStream.read(&jsonString[0], length);
  jsonStream.close();

  // Parse json
  rapidjson::Document document;
  document.Parse<0>(&jsonString[0]);
  if(!document.IsObject())
  {
    ALICEVISION_LOG_ERROR("Error: File '" << jsonFile << "' is not in json format.");
    return false;
  }
  if(!document.HasMember("resources"))
  {
    ALICEVISION_LOG_ERROR("Error: No member 'resources' in json file");
    return false;
  }

  rapidjson::Value& jsonResourcesArray = document["resources"];

  if(!jsonResourcesArray.IsArray())
  {
    ALICEVISION_LOG_ERROR("Error: Member 'resources' in json file isn't an array");
    return false;
  }

  bool canListFiles = true;

  // fill imagePaths
  for(rapidjson::Value::ConstValueIterator itrRig = jsonResourcesArray.Begin(); itrRig != jsonResourcesArray.End(); ++itrRig)
  {
    if(itrRig->IsString()) // single image path
    {
      std::vector<std::string> imagePaths;
      if(!listFiles(itrRig->GetString(), extensions, imagePaths))
        canListFiles = false;

      for(const auto& path : imagePaths)
        resources.push_back({{{path}}});
    }
    else if(itrRig->IsArray()) // rig or intrinsic group
    {
      ResourcePathsPerCamera imagePathsPerCamera;
      std::vector<std::string> intrinsicImagePaths;
      for(rapidjson::Value::ConstValueIterator itrCam = itrRig->Begin(); itrCam != itrRig->End(); ++itrCam)
      {
        if(itrCam->IsString()) // list of image paths with the same intrinsic
        {
          if(!listFiles(itrCam->GetString(), extensions, intrinsicImagePaths))
             canListFiles = false;
        }
        else if(itrCam->IsArray()) // list of image paths of a rig
        {
          std::vector<std::string> rigImagePaths;
          for(rapidjson::Value::ConstValueIterator itrVal = itrCam->Begin(); itrVal != itrCam->End(); ++itrVal)
          {
            if(itrVal->IsString()) // list of image paths of one camera of a rig
            {
              if(!listFiles(itrVal->GetString(), extensions, rigImagePaths))
                 canListFiles = false;
            }
          }
          imagePathsPerCamera.push_back(rigImagePaths);
        }
      }
      if(!intrinsicImagePaths.empty())
      {
        imagePathsPerCamera.push_back(intrinsicImagePaths);
      }
      resources.push_back(imagePathsPerCamera);
    }
  }
  return canListFiles;
}

class ImageMetadata
{  
public:

  /**
   * @brief ImageMetadata
   * @param imageAbsPath
   * @param width
   * @param height
   */
  ImageMetadata(const std::string& imageAbsPath,
            double width,
            double height)
    : _imageAbsPath(imageAbsPath)
    , _width(width)
    , _height(height)
  {
    _ppx = width / 2.0;
    _ppy = height / 2.0;

    EasyExifIO exifReader;
    exifReader.open(imageAbsPath);

    _cameraBrand = exifReader.getBrand();
    _cameraModel = exifReader.getModel();
    _serialNumber = exifReader.getSerialNumber() + exifReader.getLensSerialNumber();
    _mmFocalLength = exifReader.getFocal();

    _haveValidMetadata = (exifReader.doesHaveExifInfo() &&
                        !_cameraBrand.empty() &&
                        !_cameraModel.empty());

    if(_cameraBrand.empty() || _cameraModel.empty())
    {
      _cameraBrand = "Custom";
      _cameraModel = EINTRINSIC_enumToString(EINTRINSIC::PINHOLE_CAMERA_RADIAL3);
      _mmFocalLength = 1.2f;
    }

    if(_haveValidMetadata)
      _exifData = exifReader.getExifData();

    if(!exifReader.doesHaveExifInfo())
      ALICEVISION_LOG_WARNING("Warning: No Exif metadata for image '" << stlplus::filename_part(imageAbsPath) << "'" << std::endl);
    else if(_cameraBrand.empty() || _cameraModel.empty())
      ALICEVISION_LOG_WARNING("Warning: No Brand/Model in Exif metadata for image '" << stlplus::filename_part(imageAbsPath) << "'" << std::endl);

    // find width/height in metadata
    {
      _metadataImageWidth = width;
      _metadataImageHeight = height;

      if(_exifData.count("image_width"))
      {
        const int exifWidth = std::stoi(_exifData.at("image_width"));
        // if the metadata is bad, use the real image size
        if(exifWidth <= 0)
          _metadataImageWidth = exifWidth;
      }

      if(_exifData.count("image_height"))
      {
        const int exifHeight = std::stoi(_exifData.at("image_height"));
        // if the metadata is bad, use the real image size
        if(exifHeight <= 0)
          _metadataImageHeight = exifHeight;
      }

      // if metadata is rotated
      if(_metadataImageWidth == height && _metadataImageHeight == width)
      {
        _metadataImageWidth = width;
        _metadataImageHeight = height;
      }
    }

    // resized image
    _isResized = (_metadataImageWidth != width || _metadataImageHeight != height);

    if(_isResized)
    {
      ALICEVISION_LOG_WARNING("Warning: Resized image detected:" << std::endl
                          << "\t- real image size: " << width << "x" << height << std::endl
                          << "\t- image size from metadata is: " << _metadataImageWidth << "x" << _metadataImageHeight << std::endl);
    }
  }

  /**
   * @brief Get camera brand
   * @return camera brand
   */
  std::string getCameraBrand() const
  {
    return _cameraBrand;
  }

  /**
   * @brief Get camera model
   * @return camera model
   */
  std::string getCameraModel() const
  {
    return _cameraModel;
  }

  /**
   * @brief Get focal length (px)
   * @return focal length (px)
   */
  double getFocalLengthPx() const
  {
    return _pxFocalLength;
  }

  /**
   * @brief Get Exif metadata
   * @return Exif metadata
   */
  const ExifData& getExifData() const
  {
    return _exifData;
  }

  /**
   * @brief haveValidMetadata
   * @return
   */
  bool haveValidExifMetadata() const
  {
    return _haveValidMetadata;
  }

  /**
   * @brief setKMatrix
   * @param kMatrix
   * @return
   */
  bool setKMatrix(const std::string& kMatrix)
  {
    if(kMatrix.empty())
      return false;

    if(!checkIntrinsicStringValidity(kMatrix, _pxFocalLength, _ppx, _ppy))
    {
      _ppx = _width / 2.0;
      _ppy = _height / 2.0;
      _pxFocalLength = -1.0;
      return false;
    }
    return true;
  }

  /**
   * @brief setFocalLengthPixel
   * @param pxFocalLengthPixel
   */
  void setFocalLengthPixel(double pxFocalLengthPixel)
  {
    _pxFocalLength = pxFocalLengthPixel;
  }

  /**
   * @brief setSensorWidth
   * @param sensorWidth
   */
  void setSensorWidth(double sensorWidth)
  {
    _ccdw = sensorWidth;
    _exifData.emplace("sensor_width", std::to_string(_ccdw));
  }

  /**
   * @brief setIntrinsicType
   * @param intrinsicType
   */
  void setIntrinsicType(EINTRINSIC intrinsicType)
  {
    _intrinsicType = intrinsicType;
  }

  /**
   * @brief compute sensor width
   * @param database
   */
  bool computeSensorWidth(std::vector<sensordb::Datasheet>& database)
  {
    if(!_haveValidMetadata)
    {
      ALICEVISION_LOG_WARNING("Warning: No metadata in image '" << stlplus::filename_part(_imageAbsPath) << "'." << std::endl
                          << "Use default sensor width." << std::endl);
    }

    aliceVision::exif::sensordb::Datasheet datasheet;
    if(!getInfo(_cameraBrand, _cameraModel, database, datasheet))
    {
      return false;
    }
    // the camera model was found in the database so we can compute it's approximated focal length
    setSensorWidth(datasheet._sensorSize);
    return true;
  }

  /**
   * @brief compute intrinsic
   * @return
   */
  std::shared_ptr<IntrinsicBase> computeIntrinsic()
  {
    if(_pxFocalLength == -1.0) // focal length (px) unset
    {
      // handle case where focal length (mm) is equal to 0
      if(_mmFocalLength <= .0f)
      {
        ALICEVISION_LOG_WARNING("Warning: image '" << stlplus::filename_part(_imageAbsPath) << "' focal length (in mm) metadata is missing." << std::endl
                            << "Can't compute focal length (in px)." << std::endl);
      }
      else if(_ccdw != -1.0)
      {
        // Retrieve the focal from the metadata in mm and convert to pixel.
        _pxFocalLength = std::max(_metadataImageWidth, _metadataImageHeight) * _mmFocalLength / _ccdw;
      }
    }

    // if no user input choose a default camera model
    if(_intrinsicType == PINHOLE_CAMERA_START)
    {
      // use standard lens with radial distortion by default
      _intrinsicType = PINHOLE_CAMERA_RADIAL3;

      if(_cameraBrand == "Custom")
      {
        _intrinsicType = EINTRINSIC_stringToEnum(_cameraModel);
      }
      else if(_isResized)
      {
        // if the image has been resized, we assume that it has been undistorted
        // and we use a camera without lens distortion.
        _intrinsicType = PINHOLE_CAMERA;
      }
      else if(_mmFocalLength > 0.0 && _mmFocalLength < 15)
      {

        // if the focal lens is short, the fisheye model should fit better.
        _intrinsicType = PINHOLE_CAMERA_FISHEYE;
      }
    }

    // create the desired intrinsic
    std::shared_ptr<IntrinsicBase> intrinsic = createPinholeIntrinsic(_intrinsicType, _width, _height, _pxFocalLength, _ppx, _ppy);
    intrinsic->setInitialFocalLengthPix(_pxFocalLength);

    // initialize distortion parameters
    switch(_intrinsicType)
    {
      case PINHOLE_CAMERA_FISHEYE:
      {
        if(_cameraBrand == "GoPro")
          intrinsic->updateFromParams({_pxFocalLength, _ppx, _ppy, 0.0524, 0.0094, -0.0037, -0.0004});
        break;
      }
      case PINHOLE_CAMERA_FISHEYE1:
      {
        if(_cameraBrand == "GoPro")
          intrinsic->updateFromParams({_pxFocalLength, _ppx, _ppy, 1.04});
        break;
      }
      default: break;
    }

    // not enough information to find intrinsics
    if(_pxFocalLength <= 0 || _ppx <= 0 || _ppy <= 0)
    {
      ALICEVISION_LOG_WARNING("Warning: No instrinsics for '" << stlplus::filename_part(_imageAbsPath) << "':" << std::endl
                           << "\t- width: " << _width << std::endl
                           << "\t- height: " << _height << std::endl
                           << "\t- camera brand: " << ((_cameraBrand.empty()) ? "unknown" : _cameraBrand) << std::endl
                           << "\t- camera model: " << ((_cameraModel.empty()) ? "unknown" : _cameraModel) << std::endl
                           << "\t- sensor width: " << ((_ccdw <= 0) ? "unknown" : std::to_string(_ccdw)) << std::endl
                           << "\t- focal length (mm): " << ((_mmFocalLength <= 0) ? "unknown" : std::to_string(_mmFocalLength)) << std::endl
                           << "\t- focal length (px): " << ((_pxFocalLength <= 0) ? "unknown" : std::to_string(_pxFocalLength)) << std::endl
                           << "\t- ppx: " << ((_ppx <= 0) ? "unknown" : std::to_string(_ppx)) << std::endl
                           << "\t- ppy: " << ((_ppy <= 0) ? "unknown" : std::to_string(_ppy)) << std::endl);
    }

    if(_haveValidMetadata)
    {
      intrinsic->setSerialNumber(_serialNumber);
    }

    return intrinsic;
  }

private:
  const std::string _imageAbsPath;
  const double _width;
  const double _height;

  std::string _cameraBrand;
  std::string _cameraModel;
  std::string _serialNumber;
  int _metadataImageWidth;
  int _metadataImageHeight;
  double _ppx = -1.0;
  double _ppy = -1.0;
  double _ccdw = -1.0;
  double _pxFocalLength = -1.0;
  float _mmFocalLength = -1.0f;
  bool _haveValidMetadata;
  bool _isResized;
  EINTRINSIC _intrinsicType = PINHOLE_CAMERA_START;
  ExifData _exifData;
};

/**
 * @brief Create the description of an input image dataset for AliceVision toolsuite
 * - Export a SfMData file with View & Intrinsic data
 */
int main(int argc, char **argv)
{
  // command-line parameters

  std::string verboseLevel = system::EVerboseLevel_enumToString(system::Logger::getDefaultVerboseLevel());
  std::string imageDirectory;
  std::string jsonFile;
  std::string sensorDatabasePath;
  std::string outputDirectory;

  // user optional parameters

  std::string userKMatrix;
  std::string userCameraModelName;
  double userFocalLengthPixel = -1.0;
  double userSensorWidth = -1.0;
  int userGroupCameraModel = 1;

  po::options_description allParams("AliceVision cameraInit");

  po::options_description requiredParams("Required parameters");
  requiredParams.add_options()
    ("imageDirectory,i", po::value<std::string>(&imageDirectory)->default_value(imageDirectory),
      "Input images folder.")
    ("jsonFile,j", po::value<std::string>(&jsonFile)->default_value(jsonFile),
      "Input file with all the user options. It can be used to provide a list of images instead of a directory.")
    ("sensorDatabase,s", po::value<std::string>(&sensorDatabasePath)->required(),
      "Camera sensor width database path.")
    ("output,o", po::value<std::string>(&outputDirectory)->required(),
      "Output directory for the new SfMData file");

  po::options_description optionalParams("Optional parameters");
  optionalParams.add_options()
    ("defaultFocalLengthPix", po::value<double>(&userFocalLengthPixel)->default_value(userFocalLengthPixel),
      "Focal length in pixels.")
    ("defaultSensorWidth", po::value<double>(&userSensorWidth)->default_value(userSensorWidth),
      "Sensor width in mm.")
    ("defaultIntrinsics", po::value<std::string>(&userKMatrix)->default_value(userKMatrix),
      "Intrinsics Kmatrix \"f;0;ppx;0;f;ppy;0;0;1\".")
    ("defaultCameraModel", po::value<std::string>(&userCameraModelName)->default_value(userCameraModelName),
      "Camera model type (pinhole, radial1, radial3, brown, fisheye4).")
    ("groupCameraModel", po::value<int>(&userGroupCameraModel)->default_value(userGroupCameraModel),
      "* 0: each view have its own camera intrinsic parameters\n"
      "* 1: view share camera intrinsic parameters based on metadata, if no metadata each view has its own camera intrinsic parameters\n"
      "* 2: view share camera intrinsic parameters based on metadata, if no metadata they are grouped by folder\n");

  po::options_description logParams("Log parameters");
  logParams.add_options()
    ("verboseLevel,v", po::value<std::string>(&verboseLevel)->default_value(verboseLevel),
      "verbosity level (fatal, error, warning, info, debug, trace).");

  allParams.add(requiredParams).add(optionalParams).add(logParams);

  po::variables_map vm;
  try
  {
    po::store(po::parse_command_line(argc, argv, allParams), vm);

    if(vm.count("help") || (argc == 1))
    {
      ALICEVISION_COUT(allParams);
      return EXIT_SUCCESS;
    }
    po::notify(vm);
  }
  catch(boost::program_options::required_option& e)
  {
    ALICEVISION_CERR("ERROR: " << e.what());
    ALICEVISION_COUT("Usage:\n\n" << allParams);
    return EXIT_FAILURE;
  }
  catch(boost::program_options::error& e)
  {
    ALICEVISION_CERR("ERROR: " << e.what());
    ALICEVISION_COUT("Usage:\n\n" << allParams);
    return EXIT_FAILURE;
  }

  ALICEVISION_COUT("Program called with the following parameters:");
  ALICEVISION_COUT(vm);

  // set verbose level
  system::Logger::get()->setLogLevel(verboseLevel);

  // set user camera model
  EINTRINSIC userCameraModel = PINHOLE_CAMERA_START;

  if(!userCameraModelName.empty())
  {
    userCameraModel = EINTRINSIC_stringToEnum(userCameraModelName);
  }

  // check user don't choose both input options
  if(!imageDirectory.empty() && !jsonFile.empty())
  {
    ALICEVISION_LOG_ERROR("Error: Cannot combine -i and -j options");
    return EXIT_FAILURE;
  }

  // check input directory
  if(!imageDirectory.empty() && !stlplus::folder_exists(imageDirectory))
  {
    ALICEVISION_LOG_ERROR("Error: The input directory doesn't exist");
    return EXIT_FAILURE;
  }

  // check output directory string
  if(outputDirectory.empty())
  {
    ALICEVISION_LOG_ERROR("Error: Invalid output directory");
    return EXIT_FAILURE;
  }

  // check if output directory exists, if no create it
  if(!stlplus::folder_exists(outputDirectory))
  {
    if(!stlplus::folder_create(outputDirectory))
    {
      ALICEVISION_LOG_ERROR("Error: Cannot create output directory");
      return EXIT_FAILURE;
    }
  }

  // check user don't combine focal and K matrix
  if(userKMatrix.size() > 0 && userFocalLengthPixel != -1.0)
  {
    ALICEVISION_LOG_ERROR("Error: Cannot combine -f and -k options");
    return EXIT_FAILURE;
  }

  // check if K matrix is valid
  {
    double ppx = -1.0;
    double ppy = -1.0;
    if(userKMatrix.size() > 0 && !checkIntrinsicStringValidity(userKMatrix, userFocalLengthPixel, ppx, ppy))
    {
      ALICEVISION_LOG_ERROR("Error: Invalid K matrix input");
      return EXIT_FAILURE;
    }
  }

  // check sensor database
  std::vector<sensordb::Datasheet> database;
  if(!sensorDatabasePath.empty())
  {
    if(!sensordb::parseDatabase(sensorDatabasePath, database))
    {
      ALICEVISION_LOG_ERROR("Error: Invalid input database '" << sensorDatabasePath << "', please specify a valid file.");
      return EXIT_FAILURE;
    }
  }

  // retrieve image paths
  Resources allImagePaths;
  const std::vector<std::string> supportedExtensions{"jpg", "jpeg"};

  // retrieve resources from json file
  if(imageDirectory.empty())
  {
    if(!retrieveResources(jsonFile, supportedExtensions, allImagePaths))
    {
      ALICEVISION_LOG_ERROR("Error: Can't retrieve image paths in '" << jsonFile << "'");
      return EXIT_FAILURE;
    } 
  }
  else
  {
    std::vector<std::string> imagePaths = stlplus::folder_files(imageDirectory);
    if(!imagePaths.empty())
    {
      std::sort(imagePaths.begin(), imagePaths.end());
      for(const std::string& imagePath : imagePaths)
      {
        allImagePaths.push_back({{imagePath}});
      }
    }
    else
    {
      ALICEVISION_LOG_ERROR("Error: Can't find image paths in '" << imageDirectory << "'");
      return EXIT_FAILURE;
    }
  }

  // check the number of groups
  if(allImagePaths.empty())
  {
    ALICEVISION_LOG_ERROR("Error: No image paths given");
    return EXIT_FAILURE;
  }


  // check rigs and display retrieve informations
  std::size_t nbTotalImages = 0;
  {
    std::size_t nbSingleImages = 0;
    std::size_t nbInstrinsicGroup = 0;
    std::size_t nbRigs = 0;

    for(const auto& groupImagePaths : allImagePaths)
    {
      const std::size_t nbCameras = groupImagePaths.size();
      const std::size_t nbCamImages = groupImagePaths.front().size();

      if(nbCameras > 1) //is a rig
      {
        nbTotalImages += nbCameras * nbCamImages;
        ++nbRigs;

        for(const auto& cameraImagePaths : groupImagePaths)
        {
          if(cameraImagePaths.size() != nbCamImages)
          {
            ALICEVISION_LOG_ERROR("Error: Each camera of a rig must have the same number of images.");
            return EXIT_FAILURE;
          }
        }
      }
      else
      {
        if(nbCamImages > 1) // is an intrinsic group
        {
          nbTotalImages += nbCamImages;
          ++nbInstrinsicGroup;
        }
        else
        {
          ++nbTotalImages;
          ++nbSingleImages;
        }
      }
    }

    ALICEVISION_LOG_INFO("Retrive: " << std::endl
                      << "\t- # single image(s): " << nbSingleImages << std::endl
                      << "\t- # intrinsic group(s): " << nbInstrinsicGroup << std::endl
                      << "\t- # rig(s): " << nbRigs << std::endl);
  }


  // configure an empty scene with Views and their corresponding cameras
  SfMData sfm_data;

  // setup main image root_path
  if(jsonFile.empty())
  {
    sfm_data.s_root_path = imageDirectory;
  }
  else
  {
    sfm_data.s_root_path = "";
  }

  Views& views = sfm_data.views;
  Intrinsics& intrinsics = sfm_data.intrinsics;
  Rigs& rigs = sfm_data.getRigs();

  std::size_t rigId = 0;
  std::size_t poseId = 0;
  std::size_t intrinsicId = 0;
  std::size_t nbCurrImages = 0;

  struct sensorInfo
  {
    std::string filePath;
    std::string brand;
    std::string model;

    bool operator==(sensorInfo& other) const
    {
      return (brand == other.brand &&
              model == other.model);
    }
  };

  std::vector<sensorInfo> unknownSensorImages;
  std::vector<std::string> noMetadataImages;

  ALICEVISION_LOG_TRACE("Start image listing :" << std::endl);

  for(std::size_t groupId = 0; groupId < allImagePaths.size(); ++groupId) // intrinsic group or rig
  {
    const auto& groupImagePaths = allImagePaths.at(groupId);
    const std::size_t nbCameras = groupImagePaths.size();
    const bool isRig = (nbCameras > 1);

    for(std::size_t cameraId = 0; cameraId < nbCameras; ++cameraId) // camera in the group (cameraId always 0 if single image)
    {
      bool isCameraFirstImage = true;
      double cameraWidth = .0;
      double cameraHeight = .0;
      IndexT cameraIntrincicId = intrinsicId; // we assume intrinsic doesn't change over time
      ExifData cameraExifData; // we assume exifData doesn't change over time

      ++intrinsicId;

      const auto& cameraImagePaths = groupImagePaths.at(cameraId);
      const std::size_t nbImages = cameraImagePaths.size();
      const bool isGroup = (nbImages > 1);

      if(isRig)
      {
        rigs[rigId] = Rig(nbCameras);
      }

      for(std::size_t frameId = 0; frameId < nbImages; ++frameId) //view in the group (always 0 if single image)
      {
        const std::string& imagePath = cameraImagePaths.at(frameId);

        if(isRig)
        {
          ALICEVISION_LOG_TRACE("[" << (1 + nbCurrImages) << "/" << nbTotalImages << "] "
                            << "rig [" << std::to_string(1 + cameraId) << "/" << nbCameras << "]"
                            << " file: '" << stlplus::filename_part(imagePath) << "'");
        }
        else
        {
          ALICEVISION_LOG_TRACE("[" << (1 + nbCurrImages) << "/" << nbTotalImages
                            << "] image file: '" << stlplus::filename_part(imagePath) << "'");
        }

        const std::string imageAbsPath = (imageDirectory.empty()) ? imagePath : stlplus::create_filespec(imageDirectory, imagePath);
        const std::string imageFolder = stlplus::folder_part(imageAbsPath);

        // test if the image format is supported
        if(aliceVision::image::GetFormat(imageAbsPath.c_str()) == aliceVision::image::Unknown)
        {
          ALICEVISION_LOG_WARNING("Warning: Unknown image file format '" << stlplus::filename_part(imageAbsPath) << "'." << std::endl
                              << "Skip image." << std::endl);
          continue; // image cannot be opened
        }

        // read image header
        ImageHeader imgHeader;
        if(!aliceVision::image::ReadImageHeader(imageAbsPath.c_str(), &imgHeader))
        {
          ALICEVISION_LOG_WARNING("Warning: Can't read image header '" << stlplus::filename_part(imageAbsPath) << "'." << std::endl
                              << "Skip image." << std::endl);
          continue; // image cannot be read
        }

        const double width = imgHeader.width;
        const double height = imgHeader.height;

        //check dimensions
        if(width <= 0 || height <= 0)
        {
          ALICEVISION_LOG_WARNING("Error: Image size is invalid '" << imagePath << "'." << std::endl
                              << "\t- width: " << width << std::endl
                              << "\t- height: " << height << std::endl
                              << "Skip image." << std::endl);
          continue;
        }

        if(isCameraFirstImage) // get intrinsic and metadata from first view of the group
        {
          // set camera dimensions
          cameraWidth = width;
          cameraHeight = height;

          ImageMetadata imageMetadata(imageAbsPath, width, height);

          // add user custom metadata
          if(!userCameraModelName.empty())
            imageMetadata.setIntrinsicType(userCameraModel);

          if(!userKMatrix.empty())
            imageMetadata.setKMatrix(userKMatrix);

          if(userFocalLengthPixel != -1.0)
            imageMetadata.setFocalLengthPixel(userFocalLengthPixel);

          // find image sensor width
          if(userSensorWidth != -1.0)
          {
            imageMetadata.setSensorWidth(userSensorWidth);
          }
          else
          {
            if(!imageMetadata.computeSensorWidth(database) &&
                imageMetadata.haveValidExifMetadata() &&
               (imageMetadata.getFocalLengthPx() == -1))
            {
              unknownSensorImages.push_back({imagePath, imageMetadata.getCameraBrand(), imageMetadata.getCameraModel()});
            }
          }

          if(!imageMetadata.haveValidExifMetadata())
          {
            noMetadataImages.push_back(imagePath);
          }

          // retrieve intrinsic
          std::shared_ptr<IntrinsicBase> intrinsic = imageMetadata.computeIntrinsic();

          if(!imageMetadata.haveValidExifMetadata())
          {

            if(userGroupCameraModel == 2)
            {
              // when we have no metadata at all, we create one intrinsic group per folder.
              // the use case is images extracted from a video without metadata and assumes fixed intrinsics in the video.
              intrinsic->setSerialNumber(imageFolder);
            }
            else if(isRig)
            {
              // when we have no metadata for rig images, we create an intrinsic per camera.
              intrinsic->setSerialNumber("no_metadata_rig_" + std::to_string(groupId) + "_" + std::to_string(cameraId));
            }
            else if(isGroup)
            {
              intrinsic->setSerialNumber("no_metadata_intrincic_group_" + std::to_string(groupId));
            }
          }

          cameraExifData = imageMetadata.getExifData();

          // add the intrinsic to the sfm_container
          intrinsics[cameraIntrincicId] = intrinsic;

          isCameraFirstImage = false;
        }
        else
        {
          if((width != cameraWidth) && (height != cameraHeight))
          {
            // if not the first image check dimensions
            ALICEVISION_LOG_ERROR("Error: rig camera images don't have the same dimensions" << std::endl);
            return EXIT_FAILURE;
          }
        }

        // Init viewId from metadata
        IndexT viewId = views.size();
        {
          EasyExifIO exifReader;
          exifReader.open(imageAbsPath);
        
          viewId = (IndexT)computeUID(exifReader, imagePath);
        }

        // check duplicated view identifier
        if(views.count(viewId))
        {
          ALICEVISION_LOG_WARNING("Warning: view identifier already use, duplicated image in input (" << imageAbsPath << ")." << std::endl
                              << "Skip image." << std::endl);
          continue;
        }

        // build the view corresponding to the image and add to the sfm_container
        const std::size_t cameraPoseId = (isRig) ? poseId + frameId : poseId;
        auto& currView = views[viewId];

        currView = std::make_shared<View>(imagePath, viewId, cameraIntrincicId, cameraPoseId, width, height);
        currView->setMetadata(cameraExifData);

        if(isRig)
        {
          currView->setRigAndSubPoseId(rigId, cameraId);
        }
        else
        {
          ++poseId; // one pose per view
        }

        ++nbCurrImages;
      }
    }

    if(isRig)
    {
      ++rigId;
      poseId += groupImagePaths.front().size(); // one pose for all camera for a given time
    }
  }

  if(!noMetadataImages.empty())
  {
    ALICEVISION_LOG_WARNING("Warning: No metadata in image(s) :");
    for(const auto& imagePath : noMetadataImages)
    {
      ALICEVISION_LOG_WARNING("\t- '" << imagePath << "'");
    }
    ALICEVISION_LOG_WARNING(std::endl);
  }

  if(!unknownSensorImages.empty())
  {
    unknownSensorImages.erase(unique(unknownSensorImages.begin(), unknownSensorImages.end()), unknownSensorImages.end());
    ALICEVISION_LOG_ERROR("Error: Sensor width doesn't exist in the database for image(s) :");

    for(const auto& unknownSensor : unknownSensorImages)
    {
      ALICEVISION_LOG_ERROR("image: '" << stlplus::filename_part(unknownSensor.filePath) << "'" << std::endl
                        << "\t- camera brand: " << unknownSensor.brand <<  std::endl
                        << "\t- camera model: " << unknownSensor.model <<  std::endl);
    }
    ALICEVISION_LOG_ERROR("Please add camera model(s) and sensor width(s) in the database." << std::endl);
    return EXIT_FAILURE;
  }


  // group camera that share common properties if desired (leads to more faster & stable BA).
  if(userGroupCameraModel)
  {
    GroupSharedIntrinsics(sfm_data);
  }

  // store SfMData views & intrinsic data
  if (!Save(sfm_data, stlplus::create_filespec( outputDirectory, "sfm_data.json" ).c_str(), ESfMData(VIEWS|INTRINSICS|EXTRINSICS)))
  {
    return EXIT_FAILURE;
  }

  // count view without intrinsic
  std::size_t viewsWithoutIntrinsic = 0;

  for(const auto& viewValue: sfm_data.GetViews())
  {
    if(viewValue.second->getIntrinsicId() == UndefinedIndexT)
      ++viewsWithoutIntrinsic;
  }

  // print report
  ALICEVISION_LOG_INFO("SfMInit_ImageListing report:" << std::endl
                   << "\t- # input image path(s): " << nbTotalImages << std::endl
                   << "\t- # view(s) listed in sfm_data: " << sfm_data.GetViews().size() << std::endl
                   << "\t- # view(s) listed in sfm_data without intrinsic: " << viewsWithoutIntrinsic << std::endl
                   << "\t- # intrinsic(s) listed in sfm_data: " << sfm_data.GetIntrinsics().size());

  if(viewsWithoutIntrinsic == sfm_data.GetViews().size())
  {
    ALICEVISION_LOG_ERROR("Error: No metadata in all images." << std::endl);
    return EXIT_FAILURE;
  }
  else if(viewsWithoutIntrinsic > 0)
  {
    ALICEVISION_LOG_WARNING("Warning: " << viewsWithoutIntrinsic << " views without metadata. It may fail the reconstruction." << std::endl);
  }

  return EXIT_SUCCESS;
}