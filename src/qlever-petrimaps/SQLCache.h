// Copyright 2022, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Authors: Patrick Brosi <brosi@informatik.uni-freiburg.de>

#ifndef PETRIMAPS_SQLCACHE_H_
#define PETRIMAPS_SQLCACHE_H_

#include <pqxx/pqxx>

#include "qlever-petrimaps/GeomCache.h"

namespace petrimaps {
class SQLCache : public GeomCache {
 public:
  // _____________________________________________________________________________
  SQLCache() {
    _curl = curl_easy_init();
    _sqlConn = new pqxx::connection(_sqlCredentials.c_str());
  }

  SQLCache& operator=(SQLCache&& o) {
    _curl = curl_easy_init();
    return *this;
  };

  const util::geo::FPoint& getPoint(ID_TYPE id) const {
    return std::get<0>(_points[id]);
  }
  size_t getLine(ID_TYPE id) const {
    return std::get<0>(_lines[id]);
  }
  size_t getLineEnd(ID_TYPE id) const {
    return id + 1 < _lines.size() ? getLine(id + 1) : _linePoints.size();
  }

  void load(const std::string& cacheDir);

  std::vector<std::pair<ID_TYPE, ID_TYPE>> getRelObjects() const;
  std::map<std::string, std::string> getRowAttr(size_t rowId) const;
  std::vector<std::map<std::string, std::string>> getAttr() const;
  std::vector<std::map<std::string, std::string>> getAttrIncludeGeom() const;
  void setQuery(std::string query);
  void setQueryHash(std::string queryHash);
 
 private:
  enum _LoadStatusStages {RowCountQuery = 1, FinalQuery, Parse, FromFile};
  _LoadStatusStages _loadStatusStage = RowCountQuery;

  void loadNew();
  void loadFromFile(const std::string& fname);
  void serializeToFile(const std::string& fname) const;

  double getLoadStatusPercentTotal();
  int getLoadStatusStage();
  size_t getCurrentProgress();

  std::string _query;
  std::string _queryHash;
  std::string _createCacheViewQuery;
  std::string _finalQuery;
  size_t _batchSize = 100000;

  std::vector<std::tuple<util::geo::FPoint, bool>> _points;
  std::vector<std::tuple<size_t, bool>> _lines;

  std::vector<std::string> _resultColumnNames;
  std::map<size_t, size_t> _rowIdToResultTableRowId;
  std::vector<size_t> _geomColumnIdxs;
  std::vector<size_t> _nonGeomColumnIdxs;
  size_t _numGeoms;
  size_t _rowCount;

  // PostgreSQL
  std::string _sqlCredentials = "host=localhost port=5432 dbname=test_database user=test_user password=123456";
  pqxx::connection* _sqlConn;

  pqxx::result processQuery(std::string query, int limit = -1, int offset = -1, bool commit = false) const;
  std::vector<std::string> expandSelectStatements(std::vector<std::string> selectStatements, std::map<pqxx::oid, std::string> originColumnTables,
                                                  std::string afterSelectStatementsString);
  bool isStatementInParentheses(std::string statement, size_t pos);
  std::string getSelectStatementsString(std::string query);
  std::string getAfterSelectStatementsString(std::string query);
  std::vector<std::string> getStatementsStrings(std::string statementsString);
  std::map<pqxx::oid, std::string> getOriginColumnTables(std::vector<pqxx::oid> columnTables);
  std::map<pqxx::oid, std::string> getOriginColumnTypes(std::vector<pqxx::oid> columnTypes);
  void buildFinalQuery(std::vector<std::string> expandedSelectStatements, std::vector<pqxx::oid> columnTypes,
                       std::map<pqxx::oid, std::string> originColumnTypes, std::string afterSelectStatementsString);
  std::string setQueryLimitOffset(std::string query, int limit = -1, int offset = -1) const;
  void parse(pqxx::result result);
  void parseWKT(std::string WKT, size_t rowNumber);
  size_t parsePoint(std::string WKT, size_t rowNumber, size_t startPos, bool isFirst);
  size_t parseLineString(std::string WKT, size_t rowNumber, size_t startPos, bool isFirst);
  size_t parsePolygon(std::string WKT, size_t rowNumber, size_t startPos, bool isMulti, bool isMultiFirst);
  size_t parseMultiPoint(std::string WKT, size_t rowNumber, size_t startPos);
  size_t parseMultiLineString(std::string WKT, size_t rowNumber, size_t startPos);
  size_t parseMultiPolygon(std::string WKT, size_t rowNumber, size_t startPos);
  size_t parseParenthesis(std::string WKT, size_t startPos, std::vector<DPoint>& points);
};
} // namespace petrimaps

#endif  // PETRIMAPS_SQLCACHE_H_