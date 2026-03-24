#include "CurlUtils.h"

using namespace iptvsimple;
using namespace iptvsimple::utilities;

namespace iptvsimple
{
namespace utilities
{

CUrl::CUrl(std::string_view url)
{
  if (m_file.CURLCreate(url.data()))
  {
    // Set timeout to 5 seconds
    m_file.CURLAddOption(ADDON_CURL_OPTION_PROTOCOL, "connection-timeout", "5");
    // Default curl options
    m_file.CURLAddOption(ADDON_CURL_OPTION_PROTOCOL, "seekable",
                         "0"); // Indicates that the stream is not seekable.
    m_file.CURLAddOption(
        ADDON_CURL_OPTION_PROTOCOL, "acceptencoding",
        "gzip, deflate"); // Indicates that the response from the server may be compressed with gzip or deflate.
    m_file.CURLAddOption(ADDON_CURL_OPTION_PROTOCOL, "redirect-limit",
                         "5"); // Allows a maximum of 5 redirects to be followed.
  }
}

CUrl::~CUrl()
{
  m_file.Close();
}

int CUrl::Open()
{
  unsigned int flags = ADDON_READ_NO_CACHE | ADDON_READ_CHUNKED;

  if (!m_file.CURLOpen(flags))
  {
    Logger::Log(LEVEL_ERROR, "%s - Failed to open CURL file.", __FUNCTION__);
    return -1;
  }

  // Get the HTTP response status line (e.g., "HTTP/1.1 200 OK")
  std::string statusLine = m_file.GetPropertyValue(ADDON_FILE_PROPERTY_RESPONSE_PROTOCOL, "");

  if (!statusLine.empty())
  {
    int result = -1;
    std::string_view str = statusLine.substr(statusLine.find(' ') + 1);
    std::from_chars(str.data(), str.data() + str.size(), result);
    return result;
  }
  return -1;
}

void CUrl::AddHeaders(const std::map<std::string, std::string>& headers)
{
  for (const auto& header : headers)  
    m_file.CURLAddOption(ADDON_CURL_OPTION_HEADER, header.first.data(), header.second.data());  
}

ReadStatus CUrl::Read(std::string& data, size_t chunkBufferSize)
{
  while (true)
  {
    std::vector<char> bufferData(chunkBufferSize);
    ssize_t ret = m_file.Read(bufferData.data(), chunkBufferSize);

    if (ret == -1)
      return ReadStatus::ERROR;
    else if (ret == 0)
      return ReadStatus::IS_EOF;

    data.append(bufferData.data(), static_cast<size_t>(ret));
    m_bytesRead += static_cast<size_t>(ret);
  }
}

} // namespace utilities
} // namespace iptvsimple
