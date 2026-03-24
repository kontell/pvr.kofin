#pragma once

#include "CurlUtils.h"
#include "FileUtils.h"
#include "Logger.h"
#include "WebUtils.h"

#include <regex>
#include <string>

namespace iptvsimple
{
namespace utilities
{

class WebStreamExtractor
{
public:
  static std::string ExtractStreamUrl(const std::string& line,
                                      const std::string& webPattern,
                                      const std::string& webHeaders,
                                      bool isMediaEntry);

private:
  static std::string ExtractByPattern(const std::string& content,
                                      const std::string& customPattern,
                                      bool isMediaEntry);
  static std::string DefaultFindUrl(const std::string& content, bool isMediaEntry);
};

} // namespace utilities
} // namespace iptvsimple
