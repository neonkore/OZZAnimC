//----------------------------------------------------------------------------//
//                                                                            //
// ozz-animation is hosted at http://github.com/guillaumeblanc/ozz-animation  //
// and distributed under the MIT License (MIT).                               //
//                                                                            //
// Copyright (c) 2017 Guillaume Blanc                                         //
//                                                                            //
// Permission is hereby granted, free of charge, to any person obtaining a    //
// copy of this software and associated documentation files (the "Software"), //
// to deal in the Software without restriction, including without limitation  //
// the rights to use, copy, modify, merge, publish, distribute, sublicense,   //
// and/or sell copies of the Software, and to permit persons to whom the      //
// Software is furnished to do so, subject to the following conditions:       //
//                                                                            //
// The above copyright notice and this permission notice shall be included in //
// all copies or substantial portions of the Software.                        //
//                                                                            //
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR //
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   //
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    //
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER //
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    //
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        //
// DEALINGS IN THE SOFTWARE.                                                  //
//                                                                            //
//----------------------------------------------------------------------------//

#include "ozz/animation/offline/tools/convert2anim.h"

#include <cstdlib>
#include <cstring>

#include <json/json.h>

#include "ozz/animation/offline/additive_animation_builder.h"
#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/animation_optimizer.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"

#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/skeleton.h"

#include "ozz/base/io/archive.h"
#include "ozz/base/io/stream.h"

#include "ozz/base/log.h"

#include "ozz/options/options.h"

// Declares command line options.
OZZ_OPTIONS_DECLARE_STRING(file, "Specifies input file", "", true)
OZZ_OPTIONS_DECLARE_STRING(skeleton,
                           "Specifies ozz skeleton (raw or runtime) input file",
                           "", true)
OZZ_OPTIONS_DECLARE_STRING(config_string,
                           "Specifies input configuration string", "{}", false)

static bool ValidateEndianness(const ozz::options::Option& _option,
                               int /*_argc*/) {
  const ozz::options::StringOption& option =
      static_cast<const ozz::options::StringOption&>(_option);
  bool valid = std::strcmp(option.value(), "native") == 0 ||
               std::strcmp(option.value(), "little") == 0 ||
               std::strcmp(option.value(), "big") == 0;
  if (!valid) {
    ozz::log::Err() << "Invalid endianess option." << std::endl;
  }
  return valid;
}

OZZ_OPTIONS_DECLARE_STRING_FN(
    endian,
    "Selects output endianness mode. Can be \"native\" (same as current "
    "platform), \"little\" or \"big\".",
    "native", false, &ValidateEndianness)

static bool ValidateLogLevel(const ozz::options::Option& _option,
                             int /*_argc*/) {
  const ozz::options::StringOption& option =
      static_cast<const ozz::options::StringOption&>(_option);
  bool valid = std::strcmp(option.value(), "verbose") == 0 ||
               std::strcmp(option.value(), "standard") == 0 ||
               std::strcmp(option.value(), "silent") == 0;
  if (!valid) {
    ozz::log::Err() << "Invalid log level option." << std::endl;
  }
  return valid;
}

OZZ_OPTIONS_DECLARE_STRING_FN(
    log_level,
    "Selects log level. Can be \"silent\", \"standard\" or \"verbose\".",
    "standard", false, &ValidateLogLevel)

static bool ValidateSamplingRate(const ozz::options::Option& _option,
                                 int /*_argc*/) {
  const ozz::options::FloatOption& option =
      static_cast<const ozz::options::FloatOption&>(_option);
  bool valid = option.value() >= 0.f;
  if (!valid) {
    ozz::log::Err() << "Invalid sampling rate option (must be >= 0)."
                    << std::endl;
  }
  return valid;
}

OZZ_OPTIONS_DECLARE_FLOAT_FN(sampling_rate,
                             "Selects animation sampling rate in hertz. Set a "
                             "value = 0 to use imported scene frame rate.",
                             0.f, false, &ValidateSamplingRate)

namespace ozz {
namespace animation {
namespace offline {

namespace {

bool MakeDefaultArray(Json::Value& _parent, const char* _name,
                      const char* _comment) {
  const Json::Value* found = _parent.find(_name, _name + strlen(_name));
  if (!found) {
    Json::Value& member = _parent[_name];
    member.resize(1);
    if (*_comment != 0) {
      member.setComment(std::string("//  ") + _comment, Json::commentBefore);
    }

    // It is not a failure to have a missing member, just use the default value.
    return true;
  }

  if (!found->isArray()) {
    // It's a failure to have a wrong member type.
    return false;
  }

  return true;
}

bool MakeDefaultObject(Json::Value& _parent, const char* _name,
                       const char* _comment) {
  const Json::Value* found = _parent.find(_name, _name + strlen(_name));
  if (!found) {
    Json::Value& member = _parent[_name];
    if (*_comment != 0) {
      member.setComment(std::string("//  ") + _comment, Json::commentBefore);
    }

    // It is not a failure to have a missing member, just use the default value.
    return true;
  }

  if (!found->isObject()) {
    // It's a failure to have a wrong member type.
    return false;
  }

  return true;
}

template <typename _Type>
struct ToJsonType;

template <>
struct ToJsonType<int> {
  static const Json::ValueType kType = Json::intValue;
};
template <>
struct ToJsonType<unsigned int> {
  static const Json::ValueType kType = Json::uintValue;
};
template <>
struct ToJsonType<float> {
  static const Json::ValueType kType = Json::realValue;
};
template <>
struct ToJsonType<const char*> {
  static const Json::ValueType kType = Json::stringValue;
};
template <>
struct ToJsonType<bool> {
  static const Json::ValueType kType = Json::booleanValue;
};

const char* JsonTypeToString(Json::ValueType _type) {
  switch (_type) {
    case Json::nullValue:
      return "null";
    case Json::intValue:
      return "integer";
    case Json::uintValue:
      return "unsigned integer";
    case Json::realValue:
      return "float";
    case Json::stringValue:
      return "UTF-8 string";
    case Json::booleanValue:
      return "boolean";
    case Json::arrayValue:
      return "array";
    case Json::objectValue:
      return "object";
    default:
      assert(false && "unknown json type");
      return "unknown";
  }
}

template <typename _Type>
bool MakeDefault(Json::Value& _parent, const char* _name, _Type _value,
                 const char* _comment) {
  const Json::Value* found = _parent.find(_name, _name + strlen(_name));
  if (!found) {
    Json::Value& member = _parent[_name];
    member = _value;
    if (*_comment != 0) {
      member.setComment(std::string("//  ") + _comment,
                        Json::commentAfterOnSameLine);
    }

    // It is not a failure to have a missing member, just use default value.
    return true;
  }

  if (found->type() != ToJsonType<_Type>::kType) {
    // It's a failure to have a wrong member type.
    ozz::log::Err() << "Invalid type \"" << JsonTypeToString(found->type())
                    << "\" for json member \"" << _name << "\". \""
                    << JsonTypeToString(ToJsonType<_Type>::kType)
                    << "\" expected." << std::endl;
    return false;
  }

  return true;
}

bool SanitizeOptimizationTolerances(Json::Value& _root) {
  bool success = true;

  success &= MakeDefaultObject(_root, "optimization_tolerances",
                               "Optimization tolerances.");

  Json::Value& tolerances = _root["optimization_tolerances"];

  success &= MakeDefault(
      tolerances, "translation",
      ozz::animation::offline::AnimationOptimizer().translation_tolerance,
      "Translation optimization tolerance, defined as the distance between "
      "two translation values in meters.");

  success &= MakeDefault(
      tolerances, "rotation",
      ozz::animation::offline::AnimationOptimizer().rotation_tolerance,
      "Rotation optimization tolerance, ie: the angle between two rotation "
      "values in radian.");

  success &=
      MakeDefault(tolerances, "scale",
                  ozz::animation::offline::AnimationOptimizer().scale_tolerance,
                  "Scale optimization tolerance, ie: the norm of the "
                  "difference of two scales.");

  success &= MakeDefault(
      tolerances, "hierarchical",
      ozz::animation::offline::AnimationOptimizer().hierarchical_tolerance,
      "Hierarchical translation optimization tolerance, ie: the maximum "
      "error "
      "(distance) that an optimization on a joint is allowed to generate on "
      "its whole child hierarchy.");

  return success;
}

bool SanitizeAnimation(Json::Value& _root) {
  bool success = true;

  success &= MakeDefault(_root, "output", "*.ozz",
                         "Specifies ozz animation output file(s). When "
                         "importing multiple animations, use a \'*\' character "
                         "to specify part(s) of the filename that should be "
                         "replaced by the animation name.");

  success &=
      MakeDefault(_root, "optimize", true, "Activates keyframes optimization.");
  success &= SanitizeOptimizationTolerances(_root);
  success &= MakeDefault(_root, "raw", false, "Outputs raw animation.");
  success &= MakeDefault(
      _root, "additive", false,
      "Creates a delta animation that can be used for additive blending.");

  return success;
}

bool Sanitize(Json::Value& _root) {
  bool success = true;

  success &= MakeDefaultArray(_root, "animations", "Animations to extract.");

  Json::Value& animations = _root["animations"];
  for (Json::ArrayIndex i = 0; i < animations.size(); ++i) {
    success &= SanitizeAnimation(animations[0]);
  }

  return success;
}

void DisplaysOptimizationstatistics(const RawAnimation& _non_optimized,
                                    const RawAnimation& _optimized) {
  size_t opt_translations = 0, opt_rotations = 0, opt_scales = 0;
  for (size_t i = 0; i < _optimized.tracks.size(); ++i) {
    const RawAnimation::JointTrack& track = _optimized.tracks[i];
    opt_translations += track.translations.size();
    opt_rotations += track.rotations.size();
    opt_scales += track.scales.size();
  }
  size_t non_opt_translations = 0, non_opt_rotations = 0, non_opt_scales = 0;
  for (size_t i = 0; i < _non_optimized.tracks.size(); ++i) {
    const RawAnimation::JointTrack& track = _non_optimized.tracks[i];
    non_opt_translations += track.translations.size();
    non_opt_rotations += track.rotations.size();
    non_opt_scales += track.scales.size();
  }

  // Computes optimization ratios.
  float translation_ratio =
      non_opt_translations != 0
          ? 100.f * (non_opt_translations - opt_translations) /
                non_opt_translations
          : 0;
  float rotation_ratio =
      non_opt_rotations != 0
          ? 100.f * (non_opt_rotations - opt_rotations) / non_opt_rotations
          : 0;
  float scale_ratio =
      non_opt_scales != 0
          ? 100.f * (non_opt_scales - opt_scales) / non_opt_scales
          : 0;

  ozz::log::Log() << "Optimization stage results:" << std::endl;
  ozz::log::Log() << " - Translations key frames optimization: "
                  << translation_ratio << "%" << std::endl;
  ozz::log::Log() << " - Rotations key frames optimization: " << rotation_ratio
                  << "%" << std::endl;
  ozz::log::Log() << " - Scaling key frames optimization: " << scale_ratio
                  << "%" << std::endl;
}

ozz::animation::Skeleton* ImportSkeleton() {
  // Reads the skeleton from the binary ozz stream.
  ozz::animation::Skeleton* skeleton = NULL;
  {
    ozz::log::Log() << "Opens input skeleton ozz binary file: "
                    << OPTIONS_skeleton << std::endl;
    ozz::io::File file(OPTIONS_skeleton, "rb");
    if (!file.opened()) {
      ozz::log::Err() << "Failed to open input skeleton ozz binary file: \""
                      << OPTIONS_skeleton << "\"" << std::endl;
      return NULL;
    }
    ozz::io::IArchive archive(&file);

    // File could contain a RawSkeleton or a Skeleton.
    if (archive.TestTag<ozz::animation::offline::RawSkeleton>()) {
      ozz::log::Log() << "Reading RawSkeleton from file." << std::endl;

      // Reading the skeleton cannot file.
      ozz::animation::offline::RawSkeleton raw_skeleton;
      archive >> raw_skeleton;

      // Builds runtime skeleton.
      ozz::log::Log() << "Builds runtime skeleton." << std::endl;
      ozz::animation::offline::SkeletonBuilder builder;
      skeleton = builder(raw_skeleton);
      if (!skeleton) {
        ozz::log::Err() << "Failed to build runtime skeleton." << std::endl;
        return NULL;
      }
    } else if (archive.TestTag<ozz::animation::Skeleton>()) {
      // Reads input archive to the runtime skeleton.
      // This operation cannot fail.
      skeleton =
          ozz::memory::default_allocator()->New<ozz::animation::Skeleton>();
      archive >> *skeleton;
    } else {
      ozz::log::Err() << "Failed to read input skeleton from binary file: "
                      << OPTIONS_skeleton << std::endl;
      return NULL;
    }
  }
  return skeleton;
}

bool OutputSingleAnimation(const char* _output) {
  return strchr(_output, '*') == NULL;
}

ozz::String::Std BuildFilename(const char* _filename, const char* _animation) {
  ozz::String::Std output(_filename);

  for (size_t asterisk = output.find('*'); asterisk != std::string::npos;
       asterisk = output.find('*')) {
    output.replace(asterisk, 1, _animation);
  }
  return output;
}

bool Export(const ozz::animation::offline::RawAnimation& _raw_animation,
            const ozz::animation::Skeleton& _skeleton,
            const Json::Value& _config) {
  // Raw animation to build and output.
  ozz::animation::offline::RawAnimation raw_animation;

  // Make delta animation if requested.
  if (_config["additive"].asBool()) {
    ozz::log::Log() << "Makes additive animation." << std::endl;
    ozz::animation::offline::AdditiveAnimationBuilder additive_builder;
    RawAnimation raw_additive;
    if (!additive_builder(_raw_animation, &raw_additive)) {
      ozz::log::Err() << "Failed to make additive animation." << std::endl;
      return false;
    }
    // Copy animation.
    raw_animation = raw_additive;
  } else {
    raw_animation = _raw_animation;
  }

  // Optimizes animation if option is enabled.
  if (_config["optimize"].asBool()) {
    ozz::log::Log() << "Optimizing animation." << std::endl;
    ozz::animation::offline::AnimationOptimizer optimizer;

    // Setup optimizer from config parameters.
    const Json::Value& tolerances = _config["optimization_tolerances"];
    optimizer.translation_tolerance = tolerances["translation"].asFloat();
    optimizer.rotation_tolerance = tolerances["rotation"].asFloat();
    optimizer.scale_tolerance = tolerances["scale"].asFloat();
    optimizer.hierarchical_tolerance = tolerances["hierarchical"].asFloat();

    ozz::animation::offline::RawAnimation raw_optimized_animation;
    if (!optimizer(raw_animation, _skeleton, &raw_optimized_animation)) {
      ozz::log::Err() << "Failed to optimize animation." << std::endl;
      return false;
    }

    // Displays optimization statistics.
    DisplaysOptimizationstatistics(raw_animation, raw_optimized_animation);

    // Brings data back to the raw animation.
    raw_animation = raw_optimized_animation;
  }

  // Builds runtime animation.
  ozz::animation::Animation* animation = NULL;
  if (!_config["raw"].asBool()) {
    ozz::log::Log() << "Builds runtime animation." << std::endl;
    ozz::animation::offline::AnimationBuilder builder;
    animation = builder(raw_animation);
    if (!animation) {
      ozz::log::Err() << "Failed to build runtime animation." << std::endl;
      return false;
    }
  }

  {
    // Prepares output stream. File is a RAII so it will close automatically
    // at the end of this scope. Once the file is opened, nothing should fail
    // as it would leave an invalid file on the disk.

    // Builds output filename.
    ozz::String::Std filename = BuildFilename(_config["output"].asCString(),
                                              _raw_animation.name.c_str());

    ozz::log::Log() << "Opens output file: " << filename << std::endl;
    ozz::io::File file(filename.c_str(), "wb");
    if (!file.opened()) {
      ozz::log::Err() << "Failed to open output file: \"" << filename << "\""
                      << std::endl;
      ozz::memory::default_allocator()->Delete(animation);
      return false;
    }

    // Initializes output endianness from options.
    ozz::Endianness endianness = ozz::GetNativeEndianness();
    if (std::strcmp(OPTIONS_endian, "little")) {
      endianness = ozz::kLittleEndian;
    } else if (std::strcmp(OPTIONS_endian, "big")) {
      endianness = ozz::kBigEndian;
    }
    ozz::log::Log() << (endianness == ozz::kLittleEndian ? "Little" : "Big")
                    << " Endian output binary format selected." << std::endl;

    // Initializes output archive.
    ozz::io::OArchive archive(&file, endianness);

    // Fills output archive with the animation.
    if (_config["raw"].asBool()) {
      ozz::log::Log() << "Outputs RawAnimation to binary archive." << std::endl;
      archive << raw_animation;
    } else {
      ozz::log::Log() << "Outputs Animation to binary archive." << std::endl;
      archive << *animation;
    }
  }

  ozz::log::Log() << "Animation binary archive successfully outputted."
                  << std::endl;

  // Delete local objects.
  ozz::memory::default_allocator()->Delete(animation);

  return true;
}
}  // namespace

int AnimationConverter::operator()(int _argc, const char** _argv) {
  // Parses arguments.
  ozz::options::ParseResult parse_result = ozz::options::ParseCommandLine(
      _argc, _argv, "1.1",
      "Imports a animation from a file and converts it to ozz binary raw or "
      "runtime animation format");
  if (parse_result != ozz::options::kSuccess) {
    return parse_result == ozz::options::kExitSuccess ? EXIT_SUCCESS
                                                      : EXIT_FAILURE;
  }

  Json::Value config;
  Json::Reader json_builder;
  ozz::log::Log() << "Config: " << OPTIONS_config_string.value() << std::endl;
  if (!json_builder.parse(std::string(OPTIONS_config_string.value()), config,
                          true)) {
    ozz::log::Err() << "Error while parsing configuration string: "
                    << json_builder.getFormattedErrorMessages() << std::endl;
    return EXIT_FAILURE;
  }
  if (!Sanitize(config)) {
    ozz::log::Err() << "Invalid configuration." << std::endl;
    return EXIT_FAILURE;
  }

  Json::StreamWriterBuilder builder;
  builder["indentation"] = "  ";
  builder["precision"] = 4;
  std::string document = Json::writeString(builder, config);
  // ozz::log::Log() << document << std::endl;

  // ozz::log::Log() << Json::FastWriter().write(config) << std::endl;

  // Initializes log level from options.
  ozz::log::Level log_level = ozz::log::GetLevel();
  if (std::strcmp(OPTIONS_log_level, "silent") == 0) {
    log_level = ozz::log::Silent;
  } else if (std::strcmp(OPTIONS_log_level, "standard") == 0) {
    log_level = ozz::log::Standard;
  } else if (std::strcmp(OPTIONS_log_level, "verbose") == 0) {
    log_level = ozz::log::Verbose;
  }
  ozz::log::SetLevel(log_level);

  // Ensures file to import actually exist.
  if (!ozz::io::File::Exist(OPTIONS_file)) {
    ozz::log::Err() << "File \"" << OPTIONS_file << "\" doesn't exist."
                    << std::endl;
    return EXIT_FAILURE;
  }

  // Import skeleton instance.
  ozz::animation::Skeleton* skeleton = ImportSkeleton();
  if (!skeleton) {
    return EXIT_FAILURE;
  }

  // Imports animation from the document.
  ozz::log::Log() << "Importing file \"" << OPTIONS_file << "\"" << std::endl;

  bool success = false;
  Animations animations;
  if (Import(OPTIONS_file, *skeleton, OPTIONS_sampling_rate, &animations)) {
    success = true;

    if (OutputSingleAnimation(config["animations"][0]["output"].asCString()) &&
        animations.size() > 1) {
      ozz::log::Log() << animations.size()
                      << " animations found. Only the first one ("
                      << animations[0].name << ") will be exported."
                      << std::endl;

      // Remove all unhandled animations.
      animations.resize(1);
    }

    // Iterate all imported animation, build and output them.
    for (size_t i = 0; i < animations.size(); ++i) {
      success &= Export(animations[i], *skeleton, config["animations"][0]);
    }
  } else {
    ozz::log::Err() << "Failed to import file \"" << OPTIONS_file << "\""
                    << std::endl;
  }

  ozz::memory::default_allocator()->Delete(skeleton);

  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
}  // namespace offline
}  // namespace animation
}  // namespace ozz
