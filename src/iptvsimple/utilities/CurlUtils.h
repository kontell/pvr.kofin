#pragma once

#include "Logger.h"
#include "kodi/Filesystem.h"

#include <charconv>
#include <map>
#include <string>
#include <vector>

using namespace iptvsimple;
using namespace iptvsimple::utilities;

namespace iptvsimple
{
namespace utilities
{

constexpr size_t BUFFER_SIZE_32 = 32 * 1024; // 32 Kbyte

// HTTPResponse structure represents the HTTP response.
struct HTTPResponse
{
  std::string effectiveUrl; // The last used URL after following redirects
  std::string data; // Response data
  size_t dataSize{0}; // Size of the response data in bytes
  std::map<std::string, std::string> headers; // Headers retrieved from the response
  double downloadSpeed{0}; // Download speed in bytes/s
};

// Enum class for read status
enum class ReadStatus
{
  IS_EOF, // End of file has been reached
  CHUNK_READ, // A chunk of data has been read
  ERROR, // An error occurred
};

// CUrl class manages HTTP requests.
class ATTR_DLL_LOCAL CUrl
{
public:
  // Creates a CUrl object with the specified URL.
  CUrl(std::string_view url);
  ~CUrl();

  // Opens the connection.
  int Open();
  // Adds multiple headers.
  void AddHeaders(const std::map<std::string, std::string>& headers);
  // Reads data.
  ReadStatus Read(std::string& data, size_t chunkBufferSize = BUFFER_SIZE_32);
private:
  kodi::vfs::CFile m_file; // File object
  size_t m_bytesRead{0}; // Number of bytes read
};

} // namespace utilities
} // namespace iptvsimple
