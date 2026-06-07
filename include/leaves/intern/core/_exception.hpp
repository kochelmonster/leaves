#ifndef _LEAVES__EXCEPTIONS_HPP
#define _LEAVES__EXCEPTIONS_HPP

#include <exception>
#include <string>

#ifndef _MSC_VER
#define NOEXCEPT noexcept
#else
#if _MSC_VER >= 1600
#define NOEXCEPT noexcept
#else
#define NOEXCEPT
#endif
#endif

namespace leaves {

class LeavesException : public std::runtime_error {
 public:
  LeavesException(const char* msg = "leaves exception") : std::runtime_error(msg) {}
  LeavesException(const std::string& msg) : std::runtime_error(msg) {}
};

class TransactionActive : public LeavesException {};

class NoProcess : public LeavesException {};

class NoValidPosition : public LeavesException {};

class NotImplemented : public LeavesException {
 public:
  NotImplemented(const char* msg = "not implemented") : LeavesException(msg) {}
};

class KeyTooBig : public LeavesException {};

class TypeMismatch : public LeavesException {
 public:
  TypeMismatch(const char* msg = "db type mismatch") : LeavesException(msg) {}
};

class StorageFull : public LeavesException {
 public:
  StorageFull(
      const char* msg = "storage full: file size would exceed mapped region")
      : LeavesException(msg) {}
};

class WrongValue : public LeavesException {
 public:
  WrongValue(const char* msg) : LeavesException(msg) {}
};

class WalError : public LeavesException {
 public:
  WalError(const char* msg) : LeavesException(msg) {}
};

class FileError : public LeavesException {
 public:
  FileError(const std::string& msg, int err)
      : LeavesException(msg), _errno(err) {}
  int code() const { return _errno; }

 private:
  int _errno;
};


}  // namespace leaves

#endif  // _LEAVES__EXCEPTIONS_HPP