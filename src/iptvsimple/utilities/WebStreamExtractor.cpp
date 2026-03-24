#include "WebStreamExtractor.h"

using namespace iptvsimple::utilities;

std::string WebStreamExtractor::ExtractStreamUrl(const std::string& webUrl,
                                                 const std::string& webPattern,
                                                 const std::string& webHeaders,
                                                 bool isMediaEntry)
{
  try
  {
    if (!WebUtils::IsHttpUrl(webUrl))
    {
      Logger::Log(LEVEL_DEBUG, "%s - Invalid URL format: %s", __FUNCTION__, webUrl.c_str());
      return "";
    }

    std::string content;
    CUrl curl{webUrl};
    if (!webHeaders.empty())
    {
      std::map<std::string, std::string> headersMap = WebUtils::ConvertStringToHeaders(webHeaders);
      curl.AddHeaders(headersMap);
    }

    int statusCode = curl.Open();
    if (statusCode != 200)
    {
      Logger::Log(LEVEL_ERROR, "%s - Unexpected HTTP status code: %d", __FUNCTION__, statusCode);
      return "";
    }

    ReadStatus readStatus = curl.Read(content);
    if (readStatus != ReadStatus::IS_EOF)
    {
      Logger::Log(LEVEL_ERROR, "%s - Failed to read response content from: %s", __FUNCTION__,
                  webUrl.c_str());
      return "";
    }

    if (content.empty())
    {
      Logger::Log(LEVEL_ERROR, "%s - Failed to get web content from: %s", __FUNCTION__,
                  webUrl.c_str());
      return "";
    }

    std::string streamUrl = ExtractByPattern(content, webPattern, isMediaEntry);
    if (!streamUrl.empty())
    {
      if (streamUrl[0] == '/')
      {
        size_t schemePos = webUrl.find("://");
        if (schemePos != std::string::npos)
        {
          size_t domainEnd = webUrl.find('/', schemePos + 3);
          std::string baseUrl = webUrl.substr(0, domainEnd);
          streamUrl = baseUrl + streamUrl;
          return streamUrl;
        }
      }

      return streamUrl;
    }
        
    return "";
  }
  catch (const std::exception& e)
  {
    Logger::Log(LEVEL_ERROR, "%s - Exception while extracting stream URL: %s", __FUNCTION__,
                e.what());
    return "";
  }
}

std::string WebStreamExtractor::ExtractByPattern(const std::string& content,
                                                 const std::string& customPattern,
                                                 bool isMediaEntry)
{
  if (!customPattern.empty())
  {
    try
    {
      std::regex pattern(customPattern);
      std::smatch match;
      if (std::regex_search(content, match, pattern) && match.size() > 0)
        return match[match.size() - 1].str();
    }
    catch (const std::regex_error& e)
    {
      Logger::Log(LEVEL_ERROR, "%s - Invalid custom pattern: %s, error: %s", __FUNCTION__,
                  customPattern.c_str(), e.what());
      return "";
    }
  }

  // Default URL finding process
  return DefaultFindUrl(content, isMediaEntry);
}

std::string WebStreamExtractor::DefaultFindUrl(const std::string& content, bool isMediaEntry)
{
  Logger::Log(LEVEL_DEBUG, "%s - Default URL finding process started for media entry: %s",
              __FUNCTION__, isMediaEntry ? "true" : "false");
  // Default patterns
  std::vector<std::regex> patterns;

  if (isMediaEntry)
  {
    patterns = {
        std::regex(R"((['"])(https?://[^\s<>"']+\.(m3u8|m3u|ts|mp4|mkv|avi|mov|flv|webm))\1)")};
  }
  else
  {
    patterns = {std::regex(R"((['"])(https?://[^\s<>"']+\.(m3u8|m3u))\1)")};
  }
  
  for (const auto& pattern : patterns)
  {
    std::smatch match;
    if (std::regex_search(content, match, pattern))
    {
      return match[2].str();
    }
  }

  return "";
}
