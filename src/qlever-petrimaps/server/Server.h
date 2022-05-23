// Copyright 2022, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Authors: Patrick Brosi <brosi@informatik.uni-freiburg.de>

#ifndef PETRIMAPS_SERVER_SERVER_H_
#define PETRIMAPS_SERVER_SERVER_H_

#include <map>
#include <thread>
#include <mutex>
#include <string>
#include "qlever-petrimaps/server/Requestor.h"
#include "qlever-petrimaps/GeomCache.h"
#include "util/http/Server.h"

namespace petrimaps {

typedef std::map<std::string, std::string> Params;

class Server : public util::http::Handler {
 public:
  explicit Server() {}

  virtual util::http::Answer handle(const util::http::Req& request,
                                    int connection) const;

 private:
  static std::string parseUrl(std::string u, std::string pl, Params* params);

  util::http::Answer handleHeatMapReq(const Params& pars) const;
  util::http::Answer handleQueryReq(const Params& pars) const;
  util::http::Answer handleClearSessReq(const Params& pars) const;
  util::http::Answer handlePosReq(const Params& pars) const;
  util::http::Answer handleLoadReq(const Params& pars) const;

  void clearSession(const std::string& id) const;
  void clearSessions() const;
  void clearOldSessions() const;

  static std::string writePNG(const unsigned char* data, size_t w, size_t h);

  mutable std::mutex _m;

  mutable std::map<std::string, GeomCache*> _caches;
  mutable std::map<std::string, Requestor*> _rs;
  mutable std::map<std::string, std::string> _queryCache;
};
}  // namespace petrimaps

#endif  // PETRIMAPS_SERVER_SERVER_H_