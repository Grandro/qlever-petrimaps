// Copyright 2022, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Authors: Patrick Brosi <brosi@informatik.uni-freiburg.de>

#include <regex>
#include <chrono>

#include "SQLCache.h"
#include "3rdparty/md5/md5.h"

using petrimaps::SQLCache;

// _____________________________________________________________________________
void SQLCache::load(const std::string& cacheDir) {
  std::lock_guard<std::mutex> guard(_m);

  if (_ready) {
    return;
  }

  if (cacheDir.size()) {
    std::string cacheFilePath = cacheDir + "/" + _queryHash;
    if (access(cacheFilePath.c_str(), F_OK) != -1) {
      LOG(INFO) << "Reading from cache file " << cacheFilePath << "...";
      loadFromFile(cacheFilePath);
      LOG(INFO) << "done ...";
    } else {
      if (access(cacheDir.c_str(), W_OK) != 0) {
        std::stringstream ss;
        ss << "No write access to cache dir " << cacheDir;
        throw std::runtime_error(ss.str());
      }

      loadNew();
      LOG(INFO) << "Serializing to cache file " << cacheFilePath << "...";
      serializeToFile(cacheFilePath);
      LOG(INFO) << "done ...";
    }
  } else {
    loadNew();
  }

  _ready = true;
}

// _____________________________________________________________________________
void SQLCache::loadNew() {
  // We want the user to be able to write *any* valid SQL-query and visualize
  // every geometry in the result table. While doing so, we have to
  // consider two major points:
  // 1) For the loading bar we need to query the total amount of geometry
  //    first.
  // 2) We need to modify the query to call ST_AsText on every column
  //    that contains geometry in order to obtain the WKT.
  //
  // 2) is tricky: Consider the example:
  // SELECT * FROM table;
  // ST_AsText can only be used on a single column, so we cannot do ST_AsText(*)
  // => Execute the query with LIMIT 0 and analyze the result table columns,
  //    map the result table columns to the query select statements.
  // For every column we can obtain:
  // columnName: Name of the column as string
  // columnType: Type of the column as oid
  // columnTable: Origin table of the column as oid
  // (tableColumn: Column number in the origin table as int)
  // In order to build a second query that yields the same result table,
  // the query needs to look like this:
  // SELECT table1_name.column1_name, table1_name.columm2_name, ...
  //        table2_name.column1_name, table2_name.column2_name, ...
  //        FROM table1, table2, ...;
  // Because the result table can contain more columns than we have select statements
  // (e.g. SELECT * FROM table;), we need to execute all * statements individually
  // to be able to map several result table columns to one * select statement.
  // THIS ASSUMES * IS THE ONLY STATEMENT EXPANDING TO SEVERAL RESULT COLUMNS!
  // Notes:
  // a) columnName can be an alias, so we cannot use it directly.
  //    Theoretically we could use tableColumn to query the nth
  //    column of the origin table, but there does not seem to be
  //    a way to do that. Instead, we have to analyze the original query
  //    to match the aliases to their original names. Because several
  //    columns can have the same alias, we have to rely on their order.
  // b) columnType and columnTable are oids, so we have to execute
  //    another query to obtain a useful name.
  // c) columnType can be used for 1) and to know which columns we have
  //    to parse in the result table of the original query.

  // Steps:
  // 1) Get all origin column tables [✓] (For the FROM statements for * select statements)
  //    Also get idxs of columns containing geometry
  // 2) Expand select statements [✓]
  //    => 1:1 mapping between select statement and result table column
  // 3) Build new query with expanded select statements and ST_AsText on geometry columns [✓]
  // 4) Build row count query [✓]

  LOG(INFO) << "[GEOMCACHE] Loading...";

  const auto timeStart = std::chrono::system_clock::now();

  // Get origin column tables
  // Get origin column types
  _resultColumnNames.clear();
  std::vector<pqxx::oid> columnTables;
  std::vector<pqxx::oid> columnTypes;
  pqxx::result result = processQuery(_query, 0);
  int columns = result.columns();
  _resultColumnNames.reserve(columns);
  columnTables.reserve(columns);
  columnTypes.reserve(columns);
  for (int y = 0; y < columns; y++) {
    std::string columnName = result.column_name(y);
    _resultColumnNames.push_back(columnName);

    pqxx::oid columnTable = result.column_table(y);
    if (columnTable == pqxx::oid_none) {
      continue;
    }
    pqxx::oid columnType = result.column_type(y);
    
    columnTables.push_back(columnTable);
    columnTypes.push_back(columnType);
  }
  std::map<pqxx::oid, std::string> originColumnTables = getOriginColumnTables(columnTables);
  std::map<pqxx::oid, std::string> originColumnTypes = getOriginColumnTypes(columnTypes);

  // Expand select statements
  std::string selectStatementsString = getSelectStatementsString(_query);
  std::vector<std::string> selectStatements = getStatementsStrings(selectStatementsString);
  std::string afterSelectStatementsString = getAfterSelectStatementsString(_query);
  std::vector<std::string> expandedSelectStatements = expandSelectStatements(selectStatements, originColumnTables, afterSelectStatementsString);

  // Build final query using ST_AsText
  buildFinalQuery(expandedSelectStatements, columnTypes, originColumnTypes, afterSelectStatementsString);
  processQuery(_createCacheViewQuery, -1, -1, true);
  
  // Build row count query
  _loadStatusStage = _LoadStatusStages::RowCountQuery;
  
  std::string rowCountQuery = "SELECT COUNT(*) FROM (" + _finalQuery + ") AS finalQuery;";
  pqxx::result rowCountQueryResult = processQuery(rowCountQuery);
  const pqxx::field rowCountField = rowCountQueryResult[0][0];
  size_t rowCount = rowCountField.as<size_t>();
  // Count geometries instead of rows
  _totalSize = rowCount * _geomColumnIdxs.size();

  // Process final query and parse
  _curRow = 0;
  _curUniqueGeom = 0;
  _numPointGeoms = 0;
  _numLineGeoms = 0;
  if (_totalSize == 0) {
    throw std::runtime_error("No geometries found.");
  }

  _points.clear();
  _lines.clear();
  _linePoints.clear();

  // Process final query in batches to use less total RAM
  size_t batchSize = 100000;

  // Use cursor to process data in batches
  LOG(INFO) << "[GEOMCACHE] Process Query: " << _finalQuery;
  try {
    pqxx::work w(*_sqlConn);
    pqxx::stateless_cursor<pqxx::cursor_base::read_only, pqxx::cursor_base::owned> cursor(w, _finalQuery, "myCursor", false);
    for (size_t pos = 0; pos < rowCount; pos += batchSize) {
      _loadStatusStage = _LoadStatusStages::FinalQuery;
      pqxx::result result = cursor.retrieve(pos, pos + batchSize);
      
      _loadStatusStage = _LoadStatusStages::Parse;
      parse(result);
    }
  } catch (const std::exception &e) {
    throw std::runtime_error(e.what());
  }

  const auto timeEnd = std::chrono::system_clock::now();
  const auto time = timeEnd - timeStart;
  LOG(INFO) << "[GEOMCACHE] TIME NEEDED: " << time.count();

  LOG(INFO) << "[GEOMCACHE] Done";
  LOG(INFO) << "[GEOMCACHE] Received " << _curUniqueGeom << " unique geoms";
  LOG(INFO) << "[GEOMCACHE] Received " << _points.size() << " points and "
            << _lines.size() << " lines";
}

// _____________________________________________________________________________
void SQLCache::loadFromFile(const std::string& fname) {
  _loadStatusStage = _LoadStatusStages::FromFile;

  _points.clear();
  _linePoints.clear();
  _lines.clear();
  _nonGeomColumnIdxs.clear();
  _resultColumnNames.clear();

  std::ifstream f(fname, std::ios::binary);

  size_t numPoints;
  size_t numLinePoints;
  size_t numLines;
  size_t numRowIdToResultTableRowId;
  size_t numNonGeomColumnIdxs;
  size_t numResultColumnNames;
  std::streampos posPoints;
  std::streampos posLinePoints;
  std::streampos posLines;
  std::streampos posRowIdToResultTableRowId;
  std::streampos posNonGeomColumnIdxs;
  std::streampos posResultColumnNames;

  // Retrieve num and pos
  // _points
  f.read(reinterpret_cast<char*>(&numPoints), sizeof(size_t));
  _points.resize(numPoints);
  posPoints = f.tellg();
  f.seekg(sizeof(std::tuple<util::geo::FPoint, bool>) * numPoints, f.cur);

  // _linePoints
  f.read(reinterpret_cast<char*>(&numLinePoints), sizeof(size_t));
  _linePoints.resize(numLinePoints);
  posLinePoints = f.tellg();
  f.seekg(sizeof(util::geo::Point<int16_t>) * numLinePoints, f.cur);

  // _lines
  f.read(reinterpret_cast<char*>(&numLines), sizeof(size_t));
  _lines.resize(numLines);
  posLines = f.tellg();
  f.seekg(sizeof(std::tuple<size_t, bool>) * numLines, f.cur);

  // _rowIdToResultTableRowId
  f.read(reinterpret_cast<char*>(&numRowIdToResultTableRowId), sizeof(size_t));
  posRowIdToResultTableRowId = f.tellg();
  f.seekg(sizeof(size_t) * 2 * numRowIdToResultTableRowId, f.cur);

  // _nonGeomColumnIdxs
  f.read(reinterpret_cast<char*>(&numNonGeomColumnIdxs), sizeof(size_t));
  _nonGeomColumnIdxs.resize(numNonGeomColumnIdxs);
  posNonGeomColumnIdxs = f.tellg();
  f.seekg(sizeof(size_t) * numNonGeomColumnIdxs, f.cur);

  // _resultColumnNames
  f.read(reinterpret_cast<char*>(&numResultColumnNames), sizeof(size_t));
  _resultColumnNames.resize(numResultColumnNames);
  posResultColumnNames = f.tellg();

  // Get _totalSize
  _totalSize = numPoints + numLinePoints + numLines;
  _curRow = 0;

  // Read data
  // _points
  f.seekg(posPoints);
  for (size_t i = 0; i < numPoints; i++) {
    f.read(reinterpret_cast<char*>(&_points[i]), sizeof(std::tuple<util::geo::FPoint, bool>));
    _curRow += 1;
  }

  // _linePoints
  f.seekg(posLinePoints);
  for (size_t i = 0; i < numLinePoints; i++) {
    f.read(reinterpret_cast<char*>(&_linePoints[i]), sizeof(util::geo::Point<int16_t>));
    _curRow += 1;
  }

  // _lines
  f.seekg(posLines);
  for (size_t i = 0; i < numLines; i++) {
    f.read(reinterpret_cast<char*>(&_lines[i]), sizeof(std::tuple<size_t, bool>));
    _curRow += 1;
  }

  // _rowIdToResultTableRowId
  f.seekg(posRowIdToResultTableRowId);
  for (size_t i = 0; i < numRowIdToResultTableRowId; i++) {
    size_t rowId;
    size_t resultTableRowId;
    f.read(reinterpret_cast<char*>(&rowId), sizeof(size_t));
    f.read(reinterpret_cast<char*>(&resultTableRowId), sizeof(size_t));
    _rowIdToResultTableRowId[rowId] = resultTableRowId;
  }

  // _nonGeomColumnIdxs
  f.seekg(posNonGeomColumnIdxs);
  for (size_t i = 0; i < numNonGeomColumnIdxs; i++) {
    f.read(reinterpret_cast<char*>(&_nonGeomColumnIdxs[i]), sizeof(size_t));
  }

  // _resultColumnNames
  f.seekg(posResultColumnNames);
  for (size_t i = 0; i < numResultColumnNames; i++) {
    size_t resultColumnNameLength;
    std::string resultColumnName;
    
    f.read(reinterpret_cast<char*>(&resultColumnNameLength), sizeof(size_t));
    
    resultColumnName.reserve(resultColumnNameLength);
    auto temp = new char[resultColumnNameLength + 1];
    f.read(temp, resultColumnNameLength);
    temp[resultColumnNameLength] = 0;
    resultColumnName = temp;

    _resultColumnNames[i] = resultColumnName;
  }

  // Set _finalQuery to be able to retrieve row attributes
  _finalQuery = "SELECT * FROM \"CACHE_" + _queryHash + "\"";

  f.close();
}

// _____________________________________________________________________________
void SQLCache::serializeToFile(const std::string& fname) const {
  std::ofstream f;
  f.open(fname);

  // _points
  size_t num = _points.size();
  f.write(reinterpret_cast<const char*>(&num), sizeof(size_t));
  f.write(reinterpret_cast<const char*>(&_points[0]),
          sizeof(std::tuple<util::geo::FPoint, bool>) * num);
  
  // _linePoints
  num = _linePoints.size();
  f.write(reinterpret_cast<const char*>(&num), sizeof(size_t));
  f.write(reinterpret_cast<const char*>(&_linePoints[0]),
          sizeof(util::geo::Point<int16_t>) * num);
  
  // _lines
  num = _lines.size();
  f.write(reinterpret_cast<const char*>(&num), sizeof(size_t));
  f.write(reinterpret_cast<const char*>(&_lines[0]), sizeof(std::tuple<size_t, bool>) * num);

  // _rowIdToResultTableRowId
  num = _rowIdToResultTableRowId.size();
  f.write(reinterpret_cast<const char*>(&num), sizeof(size_t));
  for (auto const& keyValue : _rowIdToResultTableRowId) {
    size_t rowId = keyValue.first;
    size_t resultTableRowId = keyValue.second;
    f.write(reinterpret_cast<const char*>(&rowId), sizeof(size_t));
    f.write(reinterpret_cast<const char*>(&resultTableRowId), sizeof(size_t));
  }

  // _nonGeomColumnIdxs
  num = _nonGeomColumnIdxs.size();
  f.write(reinterpret_cast<const char*>(&num), sizeof(size_t));
  f.write(reinterpret_cast<const char*>(&_nonGeomColumnIdxs[0]), sizeof(size_t) * num);

  // _resultColumnNames
  num = _resultColumnNames.size();
  f.write(reinterpret_cast<const char*>(&num), sizeof(size_t));
  for (size_t i = 0; i < num; i++) {
    std::string resultColumnName = _resultColumnNames[i];
    size_t resultColumnNameLength = resultColumnName.length();
    f.write(reinterpret_cast<const char*>(&resultColumnNameLength), sizeof(size_t));
    f.write(resultColumnName.c_str(), resultColumnNameLength);
  }

  f.close();
}

// _____________________________________________________________________________
double SQLCache::getLoadStatusPercentTotal() {
  if (_totalSize == 0) {
    return 0.0;
  }

  double rowCountQueryPercent = 10.0;
  double finalQueryParsePercent = 90.0;
  double totalPercent = 0.0;
  switch (_loadStatusStage) {
    case _LoadStatusStages::RowCountQuery:
      totalPercent += _curUniqueGeom / static_cast<double>(_totalSize) * rowCountQueryPercent;
      break;
    case _LoadStatusStages::FinalQuery:
      totalPercent += rowCountQueryPercent;
      totalPercent += _curUniqueGeom / static_cast<double>(_totalSize) * finalQueryParsePercent;
      break;
    case _LoadStatusStages::Parse:
      totalPercent += rowCountQueryPercent;
      totalPercent += _curUniqueGeom / static_cast<double>(_totalSize) * finalQueryParsePercent;
      break;
    case _LoadStatusStages::FromFile:
      totalPercent += _curRow / static_cast<double>(_totalSize) * 100.0;
      break;
  }

  return totalPercent;
}

// _____________________________________________________________________________
int SQLCache::getLoadStatusStage() {
  return _loadStatusStage;
}

// _____________________________________________________________________________
size_t SQLCache::getCurrentProgress() { return _curUniqueGeom; }

// _____________________________________________________________________________
void SQLCache::setQuery(std::string query) {
  _query = query;
}

// _____________________________________________________________________________
void SQLCache::setQueryHash(std::string queryHash) {
  _queryHash = queryHash;
}

// _____________________________________________________________________________
std::vector<std::pair<ID_TYPE, ID_TYPE>> SQLCache::getRelObjects() const {
  // Returns all objects as vector<pair<geomID, Row>>
  // geomID starts from 0 ascending, Row = geomID
  std::vector<std::pair<ID_TYPE, ID_TYPE>> objects;
  objects.reserve(_points.size() + _lines.size());

  size_t idx = 0;
  for (size_t i = 0; i < _points.size(); i++) {
    bool isFirst = std::get<1>(_points[i]);
    if (isFirst && i > 0) {
      idx++;
    }
    objects.push_back({i, idx});
  }

  idx = 0;
  for (size_t i = 0; i < _lines.size(); i++) {
    bool isFirst = std::get<1>(_lines[i]);
    if (isFirst && i > 0) {
      idx++;
    }
    objects.push_back({i + I_OFFSET, idx + I_OFFSET});
  }

  return objects;
}

// _____________________________________________________________________________
std::map<std::string, std::string> SQLCache::getRowAttr(size_t rowId) const {
  size_t resultTableRowId = _rowIdToResultTableRowId.at(rowId);
  std::string query = _finalQuery + " WHERE \"$rowNumber\"=" + std::to_string(resultTableRowId);
  pqxx::result result = processQuery(query);

  const pqxx::row row = result[0];
  std::map<std::string, std::string> attr;
  for (size_t i = 0; i < _nonGeomColumnIdxs.size(); i++) {
    int y = _nonGeomColumnIdxs[i];
    pqxx::field field = row[y + 1];
    std::string columnName = _resultColumnNames[y];
    LOG(INFO) << "[GEOMCACHE] columnName: " << columnName;
    std::string fieldValue = field.c_str();
    LOG(INFO) << "[GEOMCACHE] fieldValue: " << fieldValue;
    attr[columnName] = fieldValue;
  }

  return attr;
}

// _____________________________________________________________________________
pqxx::result SQLCache::processQuery(std::string query, int limit, int offset, bool commit) const {
  query = setQueryLimitOffset(query, limit, offset);
  LOG(INFO) << "[GEOMCACHE] Process Query: " << query;

  pqxx::result result;
  try {
    // Start a transaction
    pqxx::work w(*_sqlConn);

    // Execute query
    result = w.exec(query);

    // Optional: Commit query
    if (commit) {
      w.commit();
    }

  } catch (const std::exception &e) {
    throw std::runtime_error(e.what());
  }

  return result;
}

// _____________________________________________________________________________
std::vector<std::string> SQLCache::expandSelectStatements(std::vector<std::string> selectStatements, std::map<pqxx::oid, std::string> originColumnTables,
                                                          std::string afterSelectStatementsString) {
  std::vector<std::string> expandedSelectStatements;
  expandedSelectStatements.reserve(selectStatements.size());

  LOG(INFO) << "[GEOMCACHE] afterSelectStatementsString: " << afterSelectStatementsString;
  
  // RegEx to find select statements containing *. Unescaped: (?<!\()[^\s,()]+\.\*(?!\))|(?<!\()\*(?!\))
  // std::regex starPattern("(?<!\\()[^\\s,()]+\\.\\*(?!\\))|(?<!\\()\\*(?!\\))");
  
  // RegEx to find select statements containing *. Unescaped: [^\s,]+\.\*|\*
  std::regex starPattern("[^\\s,]+\\.\\*|\\*");
  for (size_t i = 0; i < selectStatements.size(); i++) {
    std::string selectStatement = selectStatements[i];
    LOG(INFO) << "[GEOMCACHE] selectStatement: " << selectStatement;
    // Is star select statement?
    std::smatch matchResult;
    bool isStar = std::regex_search(selectStatement, matchResult, starPattern);
    if (isStar) {
      // Because std::regex does not support lookaheads and lookbehinds we have to
      // manually ensure that the matched *-select statement is not enclosed in parentheses.
      size_t pos = matchResult.position();
      isStar = !isStatementInParentheses(selectStatement, pos);
    }
    if (!isStar) {
      expandedSelectStatements.push_back(selectStatement);
      continue;
    }

    std::string query;
    size_t pointPos = selectStatement.find(".");
    std::string tableName;
    if (pointPos == std::string::npos) {
      query = "SELECT * " + afterSelectStatementsString;
    } else {
      tableName = selectStatement.substr(0, pointPos);
      query = "SELECT " + tableName + ".* " + afterSelectStatementsString;
    }

    pqxx::result result = processQuery(query, 0);
    int columns = result.columns();
    std::vector<pqxx::oid> columnTables;
    columnTables.reserve(columns);
    for (int y = 0; y < columns; y++) {
      pqxx::oid columnTable = result.column_table(y);
      columnTables.push_back(columnTable);
    }
    
    for (int y = 0; y < columns; y++) {
      pqxx::oid columnTable = columnTables[y];
      std::string columnName = result.column_name(y); // We know this can't be an alias
      std::string newSelectStatement;
      if (pointPos == std::string::npos) {
        std::string originColumnTable = originColumnTables[columnTable];
        newSelectStatement = "\"" + originColumnTable + "\"." + "\"" + columnName + "\"";
      } else {
        newSelectStatement = tableName + "." + "\"" + columnName + "\"";
      }
      expandedSelectStatements.push_back(newSelectStatement);
    }
  }

  return expandedSelectStatements;
}

// _____________________________________________________________________________
bool SQLCache::isStatementInParentheses(std::string statement, size_t pos) {
  size_t firstParanthesisPos = statement.rfind("(", pos);
  if (firstParanthesisPos == std::string::npos) {
    return false;
  }
  size_t secondParanthesisPos = statement.find(")", pos);
  return secondParanthesisPos != std::string::npos;
}

// _____________________________________________________________________________
std::string SQLCache::getSelectStatementsString(std::string query) {
  std::string lowerQuery = util::toLower(query);
  size_t selectPosEnd = lowerQuery.find("select distinct");
  if (selectPosEnd != std::string::npos) {
    selectPosEnd += 15;
  } else {
    selectPosEnd = lowerQuery.find("select") + 6;
  }
  size_t fromPosStart = lowerQuery.find("from");
  std::string selectStatementsString = query.substr(selectPosEnd, fromPosStart - selectPosEnd);

  return selectStatementsString;
}

// _____________________________________________________________________________
std::string SQLCache::getAfterSelectStatementsString(std::string query) {
  std::string lowerQuery = util::toLower(query);
  size_t fromPosStart = lowerQuery.find("from");
  std::string afterSelectStatementsString = query.substr(fromPosStart);

  return afterSelectStatementsString;
}

// _____________________________________________________________________________
std::vector<std::string> SQLCache::getStatementsStrings(std::string statementsString) {
  // Split the statements string into a vector of strings
  std::vector<std::string> statementsStrings;
  std::stringstream ss(statementsString);
  while (ss.good()) {
    std::string statementString;
    std::getline(ss, statementString, ',');
    statementsStrings.push_back(statementString);
  }

  return statementsStrings;
}

// _____________________________________________________________________________
std::map<pqxx::oid, std::string> SQLCache::getOriginColumnTables(std::vector<pqxx::oid> columnTables) {
  std::map<pqxx::oid, std::string> originColumnTables; // Map oid to table name
  
  std::string inStatement = "(";
  for (size_t i = 0; i < columnTables.size(); i++) {
    pqxx::oid columnTable = columnTables[i];
    inStatement += std::to_string(columnTable);
    if (i < columnTables.size() - 1) {
      inStatement += ", ";
    }
  }
  inStatement += ")";
  
  std::string query = "SELECT oid, relname FROM pg_class WHERE oid IN " + inStatement + ";";
  pqxx::result result = processQuery(query);
  for (const auto &row: result) {
    const pqxx::field fieldOid = row[0];
    const pqxx::field fieldRelname = row[1];
    pqxx::oid oid = fieldOid.as<pqxx::oid>();
    std::string relname = fieldRelname.as<std::string>();
    originColumnTables[oid] = relname;
  }
  
  return originColumnTables;
}

// _____________________________________________________________________________
std::map<pqxx::oid, std::string> SQLCache::getOriginColumnTypes(std::vector<pqxx::oid> columnTypes) {
  std::map<pqxx::oid, std::string> originColumnTypes; // Map oid to type name

  std::string inStatement = "(";
  for (size_t i = 0; i < columnTypes.size(); i++) {
    pqxx::oid columnType = columnTypes[i];
    inStatement += std::to_string(columnType);
    if (i < columnTypes.size() - 1) {
      inStatement += ", ";
    }
  }
  inStatement += ")";

  std::string query = "SELECT oid, typname FROM pg_type WHERE oid IN " + inStatement + ";";
  pqxx::result result = processQuery(query);
  for (const auto &row: result) {
    const pqxx::field fieldOid = row[0];
    const pqxx::field fieldTypname = row[1];
    pqxx::oid oid = fieldOid.as<pqxx::oid>();
    std::string typname = fieldTypname.as<std::string>();
    originColumnTypes[oid] = typname;
  }

  return originColumnTypes;
}

// _____________________________________________________________________________
void SQLCache::buildFinalQuery(std::vector<std::string> expandedSelectStatements, std::vector<pqxx::oid> columnTypes,
                               std::map<pqxx::oid, std::string> originColumnTypes, std::string afterSelectStatementsString) {
  _geomColumnIdxs.clear();
  _nonGeomColumnIdxs.clear();

  // When creating a view we cannot select a column twice.
  // Thus we have to give each column a unique alias.
  size_t alias_idx = 0;

  // Add expanded select statements
  std::string expandedSelectStatementsString = "";
  for (size_t i = 0; i < expandedSelectStatements.size(); i++) {
    std::string expandedSelectStatement = expandedSelectStatements[i];
    pqxx::oid columnType = columnTypes[i];
    std::string originColumnType = originColumnTypes[columnType];

    if (originColumnType == "geometry") {
      expandedSelectStatement = "ST_AsText(" + expandedSelectStatement + ")";
      _geomColumnIdxs.push_back(i);
    } else {
      _nonGeomColumnIdxs.push_back(i);
    }

    std::string alias = "\"" + std::to_string(alias_idx) + "\"";
    expandedSelectStatementsString += expandedSelectStatement + " AS " + alias;
    if (i < expandedSelectStatements.size() - 1) {
      expandedSelectStatementsString += ", ";
    }
    alias_idx++;
  }
  
  _finalQuery = "SELECT ROW_NUMBER() OVER () AS \"$rowNumber\", " + expandedSelectStatementsString + " " + afterSelectStatementsString;
  _createCacheViewQuery = "CREATE OR REPLACE VIEW \"CACHE_" + _queryHash + "\" AS " + _finalQuery;
  _finalQuery = "SELECT * FROM \"CACHE_" + _queryHash + "\"";
}

// _____________________________________________________________________________
std::string SQLCache::setQueryLimitOffset(std::string query, int limit, int offset) const {
  // Set LIMIT and OFFSET of a query.
  // If limit or offset == -1, it is left unchanged.
  std::map<std::string, int> argNames;
  argNames["limit"] = limit;
  argNames["offset"] = offset;
  for (auto const& keyValue : argNames) {
    int argValue = keyValue.second;
    if (argValue == -1) {
      continue;
    }
    std::string lowerQuery = util::toLower(query);
    std::string argName = keyValue.first;
    std::string argNameUpper = util::toUpper(argName);
    std::string argValueString = std::to_string(argValue);
    if (lowerQuery.find(argName) == std::string::npos) {
      // Add new limit / offset
      size_t semicolonPos = lowerQuery.find(";");
      std::string front = query;
      if (semicolonPos != std::string::npos) {
        front = query.substr(0, semicolonPos);
      }
      query = front + " " + argNameUpper + " " + argValueString + ";";
    } else {
      // Modify existing limit / offset
      std::string patternString = argNameUpper + "\\s*\\d+";
      std::string replacement = argNameUpper + " " + argValueString;
      std::regex pattern(patternString, std::regex_constants::icase);
      query = std::regex_replace(query, pattern, replacement);
    }
  }

  return query;
}

// POINT
// LINESTRING
// POLYGON
// MULTIPOINT
// MULTILINESTRING
// MULTIPOLYGON
// _____________________________________________________________________________
void SQLCache::parse(pqxx::result result) {
  // Parse geometry
  // Store attributes
  // Row by Row
  for (size_t x = 0; x < result.size(); x++) {
    const pqxx::row row = result[x];
    pqxx::field rowNumberField = row[0];
    size_t rowNumber = rowNumberField.as<size_t>();
    for (size_t i = 0; i < _geomColumnIdxs.size(); i++) {
      int y = _geomColumnIdxs[i];
      pqxx::field field = row[y + 1];
      std::string WKT = field.c_str();
      parseWKT(WKT, rowNumber);
    }

    _curRow++;
  }
}

// _____________________________________________________________________________
void SQLCache::parseWKT(std::string WKT, size_t rowNumber) {
  if (WKT.find("EMPTY") != std::string::npos) {
    LOG(INFO) << "[GeomCache] Empty geometry found. Skipping...";
    return;
  }

  if (WKT.rfind("POINT(", 0) != std::string::npos) {
    parsePoint(WKT, rowNumber, 5, true);
  } else if (WKT.rfind("LINESTRING(", 0) != std::string::npos) {
    parseLineString(WKT, rowNumber, 10, true);
  } else if (WKT.rfind("POLYGON(", 0) != std::string::npos) {
    parsePolygon(WKT, rowNumber, 7, false, false);
  } else if (WKT.rfind("MULTIPOINT(", 0) != std::string::npos) {
    parseMultiPoint(WKT, rowNumber, 10);
  } else if (WKT.rfind("MULTILINESTRING(", 0) != std::string::npos) {
    parseMultiLineString(WKT, rowNumber, 15);
  } else if (WKT.rfind("MULTIPOLYGON(", 0) != std::string::npos) {
    parseMultiPolygon(WKT, rowNumber, 12);
  }
}

// _____________________________________________________________________________
size_t SQLCache::parsePoint(std::string WKT, size_t rowNumber, size_t startPos, bool isFirst) {
  std::vector<DPoint> points;
  size_t pos = parseParenthesis(WKT, startPos, points);
  // Do points have to be FPoints?
  FPoint point = latLngToWebMerc(FPoint(points[0].getX(), points[0].getY()));
  if (!pointValid(point)) {
    LOG(INFO) << "[GeomCache] Invalid point found. Skipping...";
    return pos;
  }

  _points.push_back({point, isFirst});
  _curUniqueGeom++;

  if (isFirst) {
    _rowIdToResultTableRowId[_numPointGeoms] = rowNumber;
    _numPointGeoms++;
  }

  return pos;
}

// _____________________________________________________________________________
size_t SQLCache::parseLineString(std::string WKT, size_t rowNumber, size_t startPos, bool isFirst) {
  std::vector<DPoint> points;
  size_t pos = parseParenthesis(WKT, startPos, points);
  util::geo::DLine line;
  line.reserve(points.size());
  
  for (DPoint point : points) {
    DPoint projectedPoint = latLngToWebMerc(point);
    if (!pointValid(projectedPoint)) {
      LOG(INFO) << "[GeomCache] Invalid point found. Skipping...";
      return pos;
    }
    line.push_back(projectedPoint);
  }
  line = util::geo::densify(line, 200 * 3);
  
  size_t idx = _linePoints.size();
  _lines.push_back({idx, isFirst});
  insertLine(line, false);
  _curUniqueGeom++;

  if (isFirst) {
    _rowIdToResultTableRowId[_numLineGeoms + I_OFFSET] = rowNumber;
    _numLineGeoms++;
  }

  return pos;
}

// _____________________________________________________________________________
size_t SQLCache::parsePolygon(std::string WKT, size_t rowNumber, size_t startPos, bool isMulti, bool isMultiFirst) {
  bool isFirst = true;
  size_t pos = startPos;
  while (WKT[pos] != ')') {
    pos++;
    std::vector<DPoint> points;
    pos = parseParenthesis(WKT, pos, points);

    util::geo::DLine line;
    line.reserve(points.size());

    for (DPoint point : points) {
      DPoint projectedPoint = latLngToWebMerc(point);
      if (!pointValid(projectedPoint)) {
        LOG(INFO) << "[GeomCache] Invalid point found. Skipping...";
        continue;
      }
      line.push_back(projectedPoint);
    }
    line = util::geo::densify(line, 200 * 3);

    size_t idx = _linePoints.size();
    if (isMulti && isFirst) {
      _lines.push_back({idx, isMultiFirst});
    } else {
      _lines.push_back({idx, isFirst});
    }
    insertLine(line, true);

    isFirst = false;
  }
  _curUniqueGeom++;

  if (!isMulti || isMultiFirst) {
    _rowIdToResultTableRowId[_numLineGeoms + I_OFFSET] = rowNumber;
    _numLineGeoms++;
  }

  return pos + 1;
}

// _____________________________________________________________________________
size_t SQLCache::parseMultiPoint(std::string WKT, size_t rowNumber, size_t startPos) {
  bool isFirst = true;
  size_t pos = startPos;
  while (WKT[pos] != ')') {
    pos++;
    pos = parsePoint(WKT, rowNumber, pos, isFirst);

    isFirst = false;
  }

  return pos + 1;
}

// _____________________________________________________________________________
size_t SQLCache::parseMultiLineString(std::string WKT, size_t rowNumber, size_t startPos) {
  bool isFirst = true;
  size_t pos = startPos;
  while (WKT[pos] != ')') {
    pos++;
    pos = parseLineString(WKT, rowNumber, pos, isFirst);

    isFirst = false;
  }

  return pos + 1;
}

// _____________________________________________________________________________
size_t SQLCache::parseMultiPolygon(std::string WKT, size_t rowNumber, size_t startPos) {
  bool isFirst = true;
  size_t pos = startPos;
  while (WKT[pos] != ')') {
    pos++;
    pos = parsePolygon(WKT, rowNumber, pos, true, isFirst);

    isFirst = false;
  }

  return pos + 1;
}

// _____________________________________________________________________________
size_t SQLCache::parseParenthesis(std::string WKT, size_t startPos, std::vector<DPoint>& points) {
  // Parse the coordinates enclosed in Parenthesis as DPoints
  // startPos has to point to '(' and we stop at the next ')'
  std::vector<double> coords;
  size_t leftPos = startPos + 1;
  size_t rightPos = startPos + 1;

  char currChar = WKT[rightPos];
  while (currChar != ')') {
    currChar = WKT[rightPos];
    if (currChar == ' ' || currChar == ',' || currChar == ')') {
      std::string coordString = WKT.substr(leftPos, rightPos);
      float coord = std::stof(coordString);
      coords.push_back(coord);
          
      leftPos = rightPos + 1;
    }
    if (currChar == ',' || currChar == ')') {
      if (coords.size() == 2) {
        DPoint point = DPoint(coords[0], coords[1]);
        points.push_back(point);
        coords.clear();
      } else {
        // Capture non-2D points
        LOG(INFO) << "[GeomCache] Point with dimension" << coords.size() << " found. Skipping...";
      }
    }

    rightPos++;
  }

  return rightPos;
}