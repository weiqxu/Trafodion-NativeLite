// Lite SQLCI-style client for the reduced Trafodion Type 4 protocol.

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <readline/readline.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const uint16_t kSqlConnect = 3001;
const uint16_t kSqlDisconnect = 3002;
const uint16_t kSetConnectionOption = 3003;
const uint16_t kEndTransaction = 3004;
const uint16_t kExecuteDirect = 3012;
const uint16_t kFetch = 3009;
const uint16_t kFreeStatement = 3015;
const uint32_t kSignature = 12345;
const uint32_t kClientHeaderVersion = 101;
const uint32_t kServerHeaderVersion = 201;
const uint32_t kRequestHeaderType = 1;
const uint32_t kResponseHeaderType = 3;
const size_t kHeaderSize = 40;
const uint32_t kMaxMessageSize = 64U * 1024U * 1024U;

void appendU16(std::string *out, uint16_t value)
{
  value = htons(value);
  out->append(reinterpret_cast<const char *>(&value), sizeof(value));
}

void appendU32(std::string *out, uint32_t value)
{
  value = htonl(value);
  out->append(reinterpret_cast<const char *>(&value), sizeof(value));
}

void appendU64(std::string *out, uint64_t value)
{
  appendU32(out, static_cast<uint32_t>(value >> 32));
  appendU32(out, static_cast<uint32_t>(value));
}

void appendT4String(std::string *out, const std::string &value)
{
  if (value.empty())
    {
      appendU32(out, 0);
      return;
    }
  appendU32(out, static_cast<uint32_t>(value.size() + 1));
  out->append(value);
  out->push_back('\0');
}

void appendT4StringWithCharset(std::string *out, const std::string &value)
{
  appendT4String(out, value);
  if (!value.empty())
    appendU32(out, 15); // UTF-8
}

uint16_t getU16(const char *p)
{
  uint16_t value;
  memcpy(&value, p, sizeof(value));
  return ntohs(value);
}

uint32_t getU32(const char *p)
{
  uint32_t value;
  memcpy(&value, p, sizeof(value));
  return ntohl(value);
}

bool writeExact(int fd, const char *data, size_t length)
{
  while (length != 0)
    {
      ssize_t written = send(fd, data, length, MSG_NOSIGNAL);
      if (written < 0 && errno == EINTR)
        continue;
      if (written <= 0)
        return false;
      data += written;
      length -= static_cast<size_t>(written);
    }
  return true;
}

bool readExact(int fd, char *data, size_t length)
{
  while (length != 0)
    {
      ssize_t count = recv(fd, data, length, 0);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        return false;
      data += count;
      length -= static_cast<size_t>(count);
    }
  return true;
}

class Reader
{
public:
  explicit Reader(const std::string &data) : data_(data), offset_(0) {}

  bool u16(uint16_t *value)
  {
    if (!has(2))
      return false;
    *value = getU16(data_.data() + offset_);
    offset_ += 2;
    return true;
  }

  bool u32(uint32_t *value)
  {
    if (!has(4))
      return false;
    *value = getU32(data_.data() + offset_);
    offset_ += 4;
    return true;
  }

  bool u64(uint64_t *value)
  {
    uint32_t high = 0;
    uint32_t low = 0;
    if (!u32(&high) || !u32(&low))
      return false;
    *value = (static_cast<uint64_t>(high) << 32) | low;
    return true;
  }

  bool bytes(size_t length, std::string *value)
  {
    if (!has(length))
      return false;
    value->assign(data_.data() + offset_, length);
    offset_ += length;
    return true;
  }

  bool skip(size_t length)
  {
    if (!has(length))
      return false;
    offset_ += length;
    return true;
  }

  bool string(std::string *value)
  {
    uint32_t length = 0;
    if (!u32(&length))
      return false;
    if (length == 0)
      {
        value->clear();
        return true;
      }
    if (length > data_.size() - offset_ || data_[offset_ + length - 1] != '\0')
      return false;
    value->assign(data_.data() + offset_, length - 1);
    offset_ += length;
    return true;
  }

  bool stringWithCharset(std::string *value)
  {
    if (!string(value))
      return false;
    if (value->empty())
      return true;
    uint32_t charset = 0;
    return u32(&charset);
  }

  size_t remaining() const { return data_.size() - offset_; }

private:
  bool has(size_t length) const
  {
    return length <= data_.size() - offset_;
  }

  const std::string &data_;
  size_t offset_;
};

struct Response
{
  std::string body;
  uint16_t error;
  uint16_t errorDetail;
};

struct Column
{
  std::string name;
  int32_t sqlType;
  uint32_t capacity;
  uint32_t datetimeCode;
  bool variable;

  Column() : sqlType(0), capacity(0), datetimeCode(0), variable(false) {}
};

struct Cell
{
  bool isNull;
  std::string value;
  Cell() : isNull(true) {}
};

struct Result
{
  std::vector<Column> columns;
  std::vector<std::vector<Cell> > rows;
  uint32_t rowsAffected;
  std::string sqlstate;
  std::string error;
  Result() : rowsAffected(0) {}
};

std::string trim(const std::string &value)
{
  size_t begin = 0;
  while (begin < value.size() &&
         isspace(static_cast<unsigned char>(value[begin])))
    begin++;
  size_t end = value.size();
  while (end > begin && isspace(static_cast<unsigned char>(value[end - 1])))
    end--;
  return value.substr(begin, end - begin);
}

std::string cellText(const Cell &cell)
{
  return cell.isNull ? "NULL" : cell.value;
}

class Client
{
public:
  Client(const std::string &host, uint16_t port, const std::string &user)
      : host_(host), port_(port), user_(user), fd_(-1), dialogue_(0), sequence_(0)
  {
  }

  ~Client() { close(); }

  void connectToServer()
  {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    std::ostringstream port;
    port << port_;
    struct addrinfo *addresses = NULL;
    int rc = getaddrinfo(host_.c_str(), port.str().c_str(), &hints, &addresses);
    if (rc != 0)
      throw std::runtime_error(std::string("resolve server: ") + gai_strerror(rc));

    for (struct addrinfo *address = addresses; address != NULL;
         address = address->ai_next)
      {
        fd_ = socket(address->ai_family, address->ai_socktype,
                     address->ai_protocol);
        if (fd_ < 0)
          continue;
        if (::connect(fd_, address->ai_addr, address->ai_addrlen) == 0)
          break;
        ::close(fd_);
        fd_ = -1;
      }
    freeaddrinfo(addresses);
    if (fd_ < 0)
      throw std::runtime_error("connect to " + host_ + ":" + port.str() +
                               ": " + strerror(errno));

    dialogue_ = static_cast<uint32_t>(getpid());
    if (dialogue_ == 0)
      dialogue_ = 1;
    std::string body;
    appendU32(&body, 0); // descriptor type
    appendT4String(&body, std::string()); // user SID
    appendT4String(&body, std::string()); // domain
    appendT4String(&body, user_);
    appendT4String(&body, std::string()); // password
    Response response = request(kSqlConnect, body);
    Reader reader(response.body);
    uint32_t exception = 0;
    uint32_t detail = 0;
    if (response.error != 0 || !reader.u32(&exception) || !reader.u32(&detail) ||
        exception != 0)
      throw std::runtime_error("server rejected SQLCONNECT");
  }

  Result execute(const std::string &sql)
  {
    Result result;
    std::string label = "SQLCI_" + toString(++sequence_);
    std::string body;
    body.append(32, '\0');
    appendT4StringWithCharset(&body, sql);
    appendT4StringWithCharset(&body, std::string()); // cursor
    appendT4StringWithCharset(&body, label);
    appendT4String(&body, std::string()); // explain
    Response response = request(kExecuteDirect, body);
    Reader reader(response.body);

    uint32_t returnCode = 0;
    uint32_t errorLength = 0;
    if (response.error != 0 || !reader.u32(&returnCode) ||
        !reader.u32(&errorLength))
      {
        result.error = "invalid execute response";
        return result;
      }
    if (returnCode != 0)
      readError(&reader, errorLength, &result);

    uint32_t descriptorLength = 0;
    if (!reader.u32(&descriptorLength))
      {
        result.error = "invalid execute descriptor response";
        return result;
      }
    if (descriptorLength != 0)
      {
        uint32_t rowLength = 0;
        uint32_t columnCount = 0;
        if (!reader.u32(&rowLength) || !reader.u32(&columnCount) ||
            columnCount > 4096)
          {
            result.error = "invalid execute column response";
            return result;
          }
        for (uint32_t i = 0; i < columnCount; i++)
          {
            Column column;
            if (!readDescriptor(&reader, &column))
              {
                result.error = "invalid execute column descriptor";
                return result;
              }
            result.columns.push_back(column);
          }
      }

    uint32_t queryType = 0;
    uint32_t estimatedCost = 0;
    uint32_t outputValues = 0;
    uint32_t resultSets = 0;
    std::string proxySyntax;
    if (!reader.u32(&result.rowsAffected) || !reader.u32(&queryType) ||
        !reader.u32(&estimatedCost) || !reader.u32(&outputValues) ||
        !reader.u32(&resultSets) || !reader.string(&proxySyntax))
      {
        result.error = "invalid execute trailer";
        return result;
      }

    if (result.error.empty() && !result.columns.empty())
      fetchAll(label, &result);
    freeStatement(label);
    return result;
  }

  void disconnect()
  {
    if (fd_ < 0)
      return;
    try
      {
        Response response = request(kSqlDisconnect, std::string());
        (void)response;
      }
    catch (...)
      {
      }
    close();
  }

private:
  static std::string toString(uint32_t value)
  {
    std::ostringstream out;
    out << value;
    return out.str();
  }

  void close()
  {
    if (fd_ >= 0)
      {
        ::close(fd_);
        fd_ = -1;
      }
  }

  Response request(uint16_t operation, const std::string &body)
  {
    if (body.size() > kMaxMessageSize)
      throw std::runtime_error("T4 request is too large");
    std::string header;
    appendU16(&header, operation);
    appendU16(&header, 0);
    appendU32(&header, dialogue_);
    appendU32(&header, static_cast<uint32_t>(body.size()));
    appendU32(&header, 0);
    header.push_back('N');
    header.push_back('\0');
    appendU16(&header, 0);
    appendU32(&header, kRequestHeaderType);
    appendU32(&header, kSignature);
    appendU32(&header, kClientHeaderVersion);
    header.append("NTN\0", 4);
    appendU16(&header, 0);
    appendU16(&header, 0);
    if (header.size() != kHeaderSize ||
        !writeExact(fd_, header.data(), header.size()) ||
        !writeExact(fd_, body.data(), body.size()))
      throw std::runtime_error("write T4 request failed");

    char responseHeader[kHeaderSize];
    if (!readExact(fd_, responseHeader, sizeof(responseHeader)))
      throw std::runtime_error("read T4 response header failed");
    if (getU32(responseHeader + 24) != kSignature ||
        getU32(responseHeader + 28) != kServerHeaderVersion ||
        getU32(responseHeader + 20) != kResponseHeaderType ||
        getU32(responseHeader + 4) != dialogue_)
      throw std::runtime_error("invalid T4 response header");
    uint32_t length = getU32(responseHeader + 8);
    if (length > kMaxMessageSize)
      throw std::runtime_error("T4 response is too large");
    Response response;
    response.error = getU16(responseHeader + 36);
    response.errorDetail = getU16(responseHeader + 38);
    response.body.assign(length, '\0');
    if (length != 0 && !readExact(fd_, &response.body[0], length))
      throw std::runtime_error("read T4 response body failed");
    return response;
  }

  static bool readDescriptor(Reader *reader, Column *column)
  {
    uint32_t valueOffset = 0;
    uint32_t nullOffset = 0;
    uint32_t descriptorVersion = 0;
    uint32_t sqlType = 0;
    uint32_t capacity = 0;
    uint32_t precision = 0;
    uint32_t scale = 0;
    uint32_t nullable = 0;
    uint32_t signedType = 0;
    uint32_t odbcType = 0;
    uint32_t odbcPrecision = 0;
    uint32_t sqlCharset = 0;
    uint32_t odbcCharset = 0;
    if (!reader->u32(&valueOffset) || !reader->u32(&nullOffset) ||
        !reader->u32(&descriptorVersion) || !reader->u32(&sqlType) ||
        !reader->u32(&column->datetimeCode) || !reader->u32(&capacity) ||
        !reader->u32(&precision) || !reader->u32(&scale) ||
        !reader->u32(&nullable) || !reader->u32(&signedType) ||
        !reader->u32(&odbcType) || !reader->u32(&odbcPrecision) ||
        !reader->u32(&sqlCharset) || !reader->u32(&odbcCharset))
      return false;
    std::string table;
    std::string catalog;
    std::string schema;
    std::string heading;
    if (!reader->string(&column->name) || !reader->string(&table) ||
        !reader->string(&catalog) || !reader->string(&schema) ||
        !reader->string(&heading) || !reader->u32(&valueOffset) ||
        !reader->u32(&nullOffset))
      return false;
    column->sqlType = static_cast<int32_t>(sqlType);
    column->capacity = capacity;
    column->variable = !(column->sqlType == -701 || column->sqlType == -403 ||
                         column->sqlType == -404 || column->sqlType == 5 ||
                         column->sqlType == -502 || column->sqlType == 4 ||
                         column->sqlType == -401 || column->sqlType == -402 ||
                         column->sqlType == 7 || column->sqlType == 8 ||
                         column->sqlType == 9);
    return true;
  }

  static void readError(Reader *reader, uint32_t errorLength, Result *result)
  {
    uint32_t count = 0;
    if (errorLength < 4 || !reader->u32(&count) || count == 0)
      {
        result->sqlstate = "HY000";
        result->error = "server returned an SQL error";
        return;
      }
    uint32_t rowId = 0;
    uint32_t sqlCode = 0;
    std::string state;
    if (!reader->u32(&rowId) || !reader->u32(&sqlCode) ||
        !reader->string(&result->error) || !reader->bytes(6, &state))
      {
        result->sqlstate = "HY000";
        result->error = "malformed SQL error response";
        return;
      }
    result->sqlstate = state.substr(0, 5);
  }

  static std::string decodeFixed(Reader *reader, const Column &column)
  {
    std::string raw;
    if (!reader->bytes(column.capacity, &raw))
      throw std::runtime_error("malformed T4 row");
    if (column.sqlType == -701)
      return raw[0] == 0 ? "FALSE" : "TRUE";
    if (column.sqlType == -403)
      return toSigned(raw[0]);
    if (column.sqlType == -404)
      return toUnsigned(raw[0]);
    if (column.capacity == 2)
      {
        uint16_t value = getU16(raw.data());
        return column.sqlType == -502 ? toUnsigned(value) : toSigned(static_cast<int16_t>(value));
      }
    if (column.capacity == 4 && column.sqlType == 7)
      {
        uint32_t bits = getU32(raw.data());
        float value = 0;
        memcpy(&value, &bits, sizeof(value));
        std::ostringstream out;
        out << value;
        return out.str();
      }
    if (column.capacity == 4)
      {
        uint32_t value = getU32(raw.data());
        return column.sqlType == 4 ? toSigned(static_cast<int32_t>(value)) : toUnsigned(value);
      }
    if (column.capacity == 8 && column.sqlType == -402)
      {
        uint64_t value = (static_cast<uint64_t>(getU32(raw.data())) << 32) |
                         getU32(raw.data() + 4);
        return toSigned(static_cast<int64_t>(value));
      }
    if (column.capacity == 8 && column.sqlType == 8)
      {
        uint64_t bits = (static_cast<uint64_t>(getU32(raw.data())) << 32) |
                        getU32(raw.data() + 4);
        double value = 0;
        memcpy(&value, &bits, sizeof(value));
        std::ostringstream out;
        out << value;
        return out.str();
      }
    while (!raw.empty() && raw[raw.size() - 1] == '\0')
      raw.erase(raw.size() - 1);
    return raw;
  }

  static std::string toSigned(int64_t value)
  {
    std::ostringstream out;
    out << value;
    return out.str();
  }

  static std::string toUnsigned(uint64_t value)
  {
    std::ostringstream out;
    out << value;
    return out.str();
  }

  void fetchAll(const std::string &label, Result *result)
  {
    for (;;)
      {
        std::string body;
        body.append(16, '\0');
        appendT4StringWithCharset(&body, label);
        appendU64(&body, 256);
        appendU64(&body, 0);
        appendT4StringWithCharset(&body, std::string());
        appendT4String(&body, std::string());
        Response response = request(kFetch, body);
        Reader reader(response.body);
        uint32_t returnCode = 0;
        uint32_t rowCount = 0;
        uint32_t rowFormat = 0;
        uint32_t rowBytes = 0;
        std::string rows;
        if (response.error != 0 || !reader.u32(&returnCode) ||
            !reader.u32(&rowCount) || !reader.u32(&rowFormat) ||
            !reader.u32(&rowBytes))
          throw std::runtime_error("invalid fetch response");
        if (returnCode == 100)
          return;
        if (returnCode != 0 || rowCount == 0)
          throw std::runtime_error("fetch failed");
        if (!reader.bytes(rowBytes, &rows))
          throw std::runtime_error("truncated fetch rows");
        Reader rowReader(rows);
        for (uint32_t row = 0; row < rowCount; row++)
          {
            std::vector<Cell> values;
            for (size_t column = 0; column < result->columns.size(); column++)
              {
                uint16_t indicator = 0;
                if (!rowReader.u16(&indicator))
                  throw std::runtime_error("malformed T4 row indicator");
                Cell cell;
                cell.isNull = indicator == 0xffffU;
                const Column &description = result->columns[column];
                if (description.variable)
                  {
                    uint16_t length = 0;
                    std::string value;
                    if (!rowReader.u16(&length) ||
                        !rowReader.bytes(description.capacity, &value) ||
                        length > description.capacity)
                      throw std::runtime_error("malformed T4 variable row");
                    if (!cell.isNull)
                      cell.value.assign(value.data(), length);
                  }
                else if (!cell.isNull)
                  cell.value = decodeFixed(&rowReader, description);
                else if (!rowReader.skip(description.capacity))
                  throw std::runtime_error("malformed T4 null row");
                values.push_back(cell);
              }
            result->rows.push_back(values);
          }
      }
  }

  void freeStatement(const std::string &label)
  {
    std::string body;
    appendU32(&body, dialogue_);
    appendT4String(&body, label);
    appendU16(&body, 1); // drop statement
    Response response = request(kFreeStatement, body);
    (void)response;
  }

  std::string host_;
  uint16_t port_;
  std::string user_;
  int fd_;
  uint32_t dialogue_;
  uint32_t sequence_;
};

void printResult(const Result &result)
{
  if (!result.error.empty())
    {
      std::cerr << "*** ERROR[" << (result.sqlstate.empty() ? "HY000" : result.sqlstate)
                << "] " << result.error << std::endl;
      return;
    }
  if (result.columns.empty())
    {
      if (result.rowsAffected != 0)
        std::cout << "--- " << result.rowsAffected << " row(s) affected." << std::endl;
      else
        std::cout << "--- SQL operation complete." << std::endl;
      return;
    }

  std::vector<size_t> widths(result.columns.size(), 0);
  for (size_t i = 0; i < result.columns.size(); i++)
    widths[i] = std::min<size_t>(std::max<size_t>(result.columns[i].name.size(), 1), 80);
  for (size_t row = 0; row < result.rows.size(); row++)
    for (size_t column = 0; column < result.columns.size(); column++)
      widths[column] = std::min<size_t>(
          std::max(widths[column], cellText(result.rows[row][column]).size()), 80);

  for (size_t row = 0; row <= result.rows.size(); row++)
    {
      std::cout << "|";
      for (size_t column = 0; column < result.columns.size(); column++)
        {
          std::string value = row == 0 ? result.columns[column].name : cellText(result.rows[row - 1][column]);
          if (value.size() > widths[column])
            value = value.substr(0, widths[column]);
          std::cout << " " << std::left << std::setw(static_cast<int>(widths[column]))
                    << value << " |";
        }
      std::cout << std::endl;
      if (row == 0)
        {
          std::cout << "+";
          for (size_t column = 0; column < result.columns.size(); column++)
            std::cout << std::string(widths[column] + 2, '-') << "+";
          std::cout << std::endl;
        }
    }
  std::cout << "--- " << result.rows.size() << " row(s) selected." << std::endl;
}

bool isExitCommand(const std::string &sql)
{
  std::string command = trim(sql);
  if (!command.empty() && command[command.size() - 1] == ';')
    command = trim(command.substr(0, command.size() - 1));
  return command == "exit" || command == "quit" || command == "\\q";
}

bool hasStatementTerminator(const std::string &line, std::string *statement)
{
  bool singleQuote = false;
  bool doubleQuote = false;
  for (size_t i = 0; i < line.size(); i++)
    {
      char c = line[i];
      if (c == '\'' && !doubleQuote)
        {
          if (singleQuote && i + 1 < line.size() && line[i + 1] == '\'')
            i++;
          else
            singleQuote = !singleQuote;
        }
      else if (c == '"' && !singleQuote)
        doubleQuote = !doubleQuote;
      else if (c == ';' && !singleQuote && !doubleQuote)
        {
          *statement = line.substr(0, i + 1);
          return true;
        }
    }
  return false;
}

void printUsage(const char *program)
{
  std::cout << "Usage: " << program << " [options]\n"
            << "  --host HOST       server host (default: 127.0.0.1)\n"
            << "  --port PORT       server port (default: 23400)\n"
            << "  --user USER       database user (default: DB__ROOT)\n"
            << "  -f, --file FILE   execute SQL from FILE\n"
            << "  -h, --help        show this help\n"
            << "\nTerminate SQL with ';'. Commands: exit, quit, \\q.\n";
}

int runInput(Client *client, std::istream *input, bool interactive)
{
  std::string pending;
  bool hadSqlError = false;
  std::string line;
  while (true)
    {
      if (interactive)
        {
          const char *prompt = pending.empty() ? "SQL> " : " ... ";
          char *input = readline(prompt);
          if (!input)
            break;
          line.assign(input);
          free(input);
        }
      else if (!std::getline(*input, line))
        break;
      if (pending.empty() &&
          (trim(line) == "exit" || trim(line) == "quit" || trim(line) == "\\q"))
        return hadSqlError ? 1 : 0;
      std::string remaining = line;
      while (true)
        {
          pending += remaining;
          pending.push_back('\n');
          std::string statement;
          if (!hasStatementTerminator(pending, &statement))
            break;
          pending.erase(0, statement.size());
          // Each input line contributes a newline to the pending buffer.
          // Remove that separator before deciding whether another statement
          // is pending; otherwise the prompt changes to "..." after every
          // completed single-line statement.
          size_t firstPending = pending.find_first_not_of(" \t\r\n");
          if (firstPending == std::string::npos)
            pending.clear();
          else if (firstPending != 0)
            pending.erase(0, firstPending);
          if (isExitCommand(statement))
            return hadSqlError ? 1 : 0;
          try
            {
              Result result = client->execute(statement);
              printResult(result);
              hadSqlError = hadSqlError || !result.error.empty();
            }
          catch (const std::exception &error)
            {
              std::cerr << "*** CLIENT ERROR: " << error.what() << std::endl;
              return 1;
            }
          remaining.clear();
          if (pending.empty())
            break;
          remaining = pending;
          pending.clear();
        }
    }
  if (!trim(pending).empty())
    {
      try
        {
          Result result = client->execute(pending);
          printResult(result);
          hadSqlError = hadSqlError || !result.error.empty();
        }
      catch (const std::exception &error)
        {
          std::cerr << "*** CLIENT ERROR: " << error.what() << std::endl;
          return 1;
        }
    }
  return hadSqlError ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
  std::string host = "127.0.0.1";
  uint16_t port = 23400;
  std::string user = "DB__ROOT";
  std::string file;
  for (int i = 1; i < argc; i++)
    {
      std::string argument(argv[i]);
      if (argument == "-h" || argument == "--help")
        {
          printUsage(argv[0]);
          return 0;
        }
      if (argument == "--host" && i + 1 < argc)
        host = argv[++i];
      else if (argument == "--port" && i + 1 < argc)
        {
          long value = strtol(argv[++i], NULL, 10);
          if (value < 1 || value > 65535)
            {
              std::cerr << "invalid port" << std::endl;
              return 2;
            }
          port = static_cast<uint16_t>(value);
        }
      else if (argument == "--user" && i + 1 < argc)
        user = argv[++i];
      else if ((argument == "-f" || argument == "--file") && i + 1 < argc)
        file = argv[++i];
      else
        {
          std::cerr << "unknown or incomplete option: " << argument << std::endl;
          printUsage(argv[0]);
          return 2;
        }
    }

  std::ifstream script;
  std::istream *input = &std::cin;
  if (!file.empty())
    {
      script.open(file.c_str());
      if (!script)
        {
          std::cerr << "cannot open SQL file: " << file << std::endl;
          return 2;
        }
      input = &script;
    }
  bool interactive = file.empty() && isatty(STDIN_FILENO) != 0;
  try
    {
      Client client(host, port, user);
      client.connectToServer();
      int status = runInput(&client, input, interactive);
      client.disconnect();
      return status;
    }
  catch (const std::exception &error)
    {
      std::cerr << "*** CLIENT ERROR: " << error.what() << std::endl;
      return 1;
    }
}
