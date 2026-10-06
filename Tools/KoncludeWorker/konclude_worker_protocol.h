// Wire format between KoncludeWorkerClient and konclude_worker (see
// README.md in this directory). Header-only; used by both sides.
//
// A frame is: uint32 kind, uint32 itemCount, then itemCount items, each a
// uint32 length followed by that many bytes (length kNullItem = null string,
// no bytes). All integers are little-endian (host order; both ends run on the
// same machine).

#ifndef KONCLUDE_WORKER_PROTOCOL_H
#define KONCLUDE_WORKER_PROTOCOL_H

#include <sys/socket.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace konclude_worker {

// Request kinds (client -> worker).
enum RequestKind : uint32_t {
  kLoadOntology = 1,     // [path]
  kTellAndRetract = 2,   // [nTellC, nRetractC, nTellP, nRetractP, then
                         //  2 strings per class assertion (individual, class)
                         //  and 3 per property assertion (s, p, o), in that order]
  kQuery = 3,            // [sparql]
  kCheckConsistency = 4, // []
  kCheckSatisfiable = 5, // [classIRI]
  kQuit = 6,             // [] (worker exits without a response)
};

// Response kinds (worker -> client).
enum ResponseKind : uint32_t {
  kOk = 0,    // query: [varCount, rowCount, varNames..., bindings row-major]
              // checks: ["0"|"1"]; others: []
  kError = 1, // [message]
};

const uint32_t kNullItem = 0xFFFFFFFFu;

// One decoded item: a string or null.
struct Item {
  bool isNull;
  std::string value;
};

struct Frame {
  uint32_t kind;
  std::vector<Item> items;
};

inline void addItem(Frame &frame, const std::string &value) {
  frame.items.push_back(Item{false, value});
}

inline void addItem(Frame &frame, const char *value) {
  if (value) {
    frame.items.push_back(Item{false, std::string(value)});
  } else {
    frame.items.push_back(Item{true, std::string()});
  }
}

// Sends all bytes; MSG_NOSIGNAL so a closed peer gives EPIPE instead of
// raising SIGPIPE in the sender.
inline bool sendAll(int fd, const char *data, size_t size) {
  while (size > 0) {
    ssize_t sent = ::send(fd, data, size, MSG_NOSIGNAL);
    if (sent < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    data += sent;
    size -= static_cast<size_t>(sent);
  }
  return true;
}

inline bool recvAll(int fd, char *data, size_t size) {
  while (size > 0) {
    ssize_t received = ::recv(fd, data, size, 0);
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (received == 0) {
      return false; // peer closed
    }
    data += received;
    size -= static_cast<size_t>(received);
  }
  return true;
}

inline void appendU32(std::string &buffer, uint32_t value) {
  buffer.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

inline bool writeFrame(int fd, const Frame &frame) {
  std::string buffer;
  appendU32(buffer, frame.kind);
  appendU32(buffer, static_cast<uint32_t>(frame.items.size()));
  for (const Item &item : frame.items) {
    if (item.isNull) {
      appendU32(buffer, kNullItem);
    } else {
      appendU32(buffer, static_cast<uint32_t>(item.value.size()));
      buffer.append(item.value);
    }
  }
  return sendAll(fd, buffer.data(), buffer.size());
}

inline bool readU32(int fd, uint32_t *value) {
  return recvAll(fd, reinterpret_cast<char *>(value), sizeof(*value));
}

inline bool readFrame(int fd, Frame *frame) {
  uint32_t count = 0;
  if (!readU32(fd, &frame->kind) || !readU32(fd, &count)) {
    return false;
  }
  frame->items.clear();
  frame->items.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t length = 0;
    if (!readU32(fd, &length)) {
      return false;
    }
    Item item{length == kNullItem, std::string()};
    if (!item.isNull) {
      item.value.resize(length);
      if (length > 0 && !recvAll(fd, &item.value[0], length)) {
        return false;
      }
    }
    frame->items.push_back(item);
  }
  return true;
}

} // namespace konclude_worker

#endif // KONCLUDE_WORKER_PROTOCOL_H
