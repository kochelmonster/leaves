#ifndef _LEAVES__EXCEPTIONS_HPP
#define _LEAVES__EXCEPTIONS_HPP

#include <exception>

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

class LeavesException : public std::exception {};

class TransactionActive : public LeavesException {};

class NoProcess : public LeavesException {};

class NoValidPosition : public LeavesException {};

class NotImplemented : public LeavesException {};

class KeyToBig : public LeavesException {};

class WrongValue : public LeavesException {
 private:
  const char* _msg;

 public:
  WrongValue(const char* msg) : _msg(msg) {}

  const char* what() const NOEXCEPT { return _msg; }
};

}  // namespace leaves

#endif  // _LEAVES__EXCEPTIONS_HPP