// Copyright 2022, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Authors: Patrick Brosi <brosi@informatik.uni-freiburg.de>

#ifndef PETRIMAPS_SERVER_SQLREQUESTOR_H_
#define PETRIMAPS_SERVER_SQLREQUESTOR_H_

#include "qlever-petrimaps/server/Requestor.h"
#include "qlever-petrimaps/SQLCache.h"

namespace petrimaps {
class SQLRequestor : public Requestor {
 public:
   SQLRequestor() {
      _maxMemory = -1;
   };
   SQLRequestor(std::shared_ptr<const SQLCache> cache, size_t maxMemory) {
      Requestor::_cache = cache;
      _cache = cache;
      _maxMemory = maxMemory;
      _createdAt = std::chrono::system_clock::now();
   };

   void request(const std::string& query);
   std::vector<std::pair<std::string, std::string>> requestRow(uint64_t row) const;
   void requestRows(std::function<void(std::vector<std::vector<std::pair<std::string, std::string>>>)> cb) const;
 
 private:
   std::shared_ptr<const SQLCache> _cache;
   std::string _query;
};
} // namespace petrimaps

#endif  // PETRIMAPS_SERVER_SQLREQUESTOR_H_