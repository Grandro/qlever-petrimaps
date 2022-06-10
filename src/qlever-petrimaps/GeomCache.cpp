// Copyright 2022, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Authors: Patrick Brosi <brosi@informatik.uni-freiburg.de>

#include <curl/curl.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <parallel/algorithm>
#include <sstream>

#include "qlever-petrimaps/GeomCache.h"
#include "qlever-petrimaps/Misc.h"
#include "qlever-petrimaps/server/Requestor.h"
#include "util/Misc.h"
#include "util/geo/Geo.h"
#include "util/geo/PolyLine.h"
#include "util/log/Log.h"

using petrimaps::GeomCache;
using util::geo::FPoint;
using util::geo::latLngToWebMerc;

// const static std::string QUERY =
// "SELECT ?geometry WHERE {"
// " ?osm_id <http://www.opengis.net/ont/geosparql#hasGeometry> ?geometry ."
// "   { ?osm_id <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> "
// "<https://www.openstreetmap.org/node> }"
// " UNION"
// "     { ?osm_id <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> "
// "<https://www.openstreetmap.org/relation> }"
// " } ORDER BY ?geometry LIMIT "
// "18446744073709551615";
//
// const static std::string QUERY = "PREFIX osmway:
// <https://www.openstreetmap.org/way/>" " PREFIX geo:
// <http://www.opengis.net/ont/geosparql#> " " PREFIX osmrel:
// <https://www.openstreetmap.org/relation/> " " SELECT ?geom WHERE {
// osmway:108522418 geo:hasGeometry ?geom } ";

// const static std::string QUERY =
// "SELECT ?geometry WHERE {"
// " ?osm_id <http://www.opengis.net/ont/geosparql#hasGeometry> ?geometry ."
// "      ?osm_id <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> "
// "<https://www.openstreetmap.org/way>"
// " } ORDER BY ?geometry LIMIT "
// "18446744073709551615";

const static std::string QUERY =
    "SELECT ?geometry WHERE {"
    " ?osm_id <http://www.opengis.net/ont/geosparql#hasGeometry> ?geometry "
    " } ORDER BY ?geometry";

// const static std::string  QUERY = "PREFIX osmnode:
// <https://www.openstreetmap.org/node/> " " PREFIX osm:
// <https://www.openstreetmap.org/> " " PREFIX osmrel:
// <https://www.openstreetmap.org/relation/> " " PREFIX geo:
// <http://www.opengis.net/ont/geosparql#> " " PREFIX osmkey:
// <https://www.openstreetmap.org/wiki/Key:> " " SELECT ?geometry WHERE { " "
// ?osmid geo:hasGeometry ?geometry . " "   ?osmid
// <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> "
// "<https://www.openstreetmap.org/node>"
// "}";

// _____________________________________________________________________________
size_t GeomCache::writeCb(void* contents, size_t size, size_t nmemb,
                          void* userp) {
  size_t realsize = size * nmemb;
  try {
    static_cast<GeomCache*>(userp)->parse(static_cast<const char*>(contents),
                                          realsize);
  } catch (...) {
    static_cast<GeomCache*>(userp)->_exceptionPtr = std::current_exception();
    return CURLE_WRITE_ERROR;
  }
  return realsize;
}

// _____________________________________________________________________________
size_t GeomCache::writeCbIds(void* contents, size_t size, size_t nmemb,
                             void* userp) {
  size_t realsize = size * nmemb;
  try {
    static_cast<GeomCache*>(userp)->parseIds(static_cast<const char*>(contents),
                                             realsize);
  } catch (...) {
    static_cast<GeomCache*>(userp)->_exceptionPtr = std::current_exception();
    return CURLE_WRITE_ERROR;
  }
  return realsize;
}

// _____________________________________________________________________________
void GeomCache::parse(const char* c, size_t size) {
  const char* start = c;
  while (c < start + size) {
    switch (_state) {
      case IN_HEADER:
        if (*c == '\n') {
          _state = IN_ROW;
          c++;
        } else {
          c++;
          continue;
        }
      case IN_ROW:
        if (*c == '\t' || *c == '\n') {
          // bool isGeom = util::endsWith(
          // _dangling, "^^<http://www.opengis.net/ont/geosparql#wktLiteral>");

          bool isGeom = true;

          auto p = _dangling.rfind("\"POINT(", 0);

          // if the previous was not a multi geometry, and if the strings
          // match exactly, re-use the geometry
          if (isGeom && _prev == _dangling && _qidToId.back().qid == 0) {
            _qidToId.push_back({0, _qidToId.back().id});
          } else if (isGeom && p != std::string::npos) {
            _curUniqueGeom++;
            p += 7;
            auto point = parsePoint(_dangling, p);
            if (pointValid(point)) {
              _points.push_back(point);
              assert(_points.size() - 1 < I_OFFSET);
              _qidToId.push_back({0, _points.size() - 1});
            } else {
              _qidToId.push_back({0, std::numeric_limits<ID_TYPE>::max()});
            }
          } else if (isGeom && (p = _dangling.rfind("\"LINESTRING(", 0)) !=
                                   std::string::npos) {
            _curUniqueGeom++;
            p += 12;
            const auto& line = parseLineString(_dangling, p);
            if (line.size() == 0) {
              _qidToId.push_back({0, std::numeric_limits<ID_TYPE>::max()});
            } else {
              _lines.push_back(_linePoints.size());
              insertLine(line);
              assert(_lines.size() - 1 <
                     std::numeric_limits<ID_TYPE>::max() - I_OFFSET);
              _qidToId.push_back({0, I_OFFSET + _lines.size() - 1});
            }
          } else if (isGeom && (p = _dangling.rfind("\"MULTILINESTRING(", 0)) !=
                                   std::string::npos) {
            _curUniqueGeom++;
            p += 17;
            size_t i = 0;
            while ((p = _dangling.find("(", p + 1)) != std::string::npos) {
              const auto& line = parseLineString(_dangling, p + 1);
              if (line.size() == 0) {
                if (i == 0) {
                  _qidToId.push_back({0, std::numeric_limits<ID_TYPE>::max()});
                }
              } else {
                _lines.push_back(_linePoints.size());
                insertLine(line);
                assert(_lines.size() - 1 <
                       std::numeric_limits<ID_TYPE>::max() - I_OFFSET);
                _qidToId.push_back(
                    {i == 0 ? 0 : 1, I_OFFSET + _lines.size() - 1});
              }
              i++;
            }
            if (i == 0)
              _qidToId.push_back({0, std::numeric_limits<ID_TYPE>::max()});
          } else if (isGeom && (p = _dangling.rfind("\"POLYGON(", 0)) !=
                                   std::string::npos) {
            _curUniqueGeom++;
            p += 9;
            size_t i = 0;
            while ((p = _dangling.find("(", p + 1)) != std::string::npos) {
              const auto& line = parseLineString(_dangling, p + 1);
              if (line.size() == 0) {
                if (i == 0) {
                  _qidToId.push_back({0, std::numeric_limits<ID_TYPE>::max()});
                }
              } else {
                _lines.push_back(_linePoints.size());
                insertLine(line);
                assert(_lines.size() - 1 <
                       std::numeric_limits<ID_TYPE>::max() - I_OFFSET);
                _qidToId.push_back(
                    {i == 0 ? 0 : 1, I_OFFSET + _lines.size() - 1});
              }
              i++;
            }
            if (i == 0)
              _qidToId.push_back({0, std::numeric_limits<ID_TYPE>::max()});
          } else if (isGeom && (p = _dangling.rfind("\"MULTIPOLYGON(", 0)) !=
                                   std::string::npos) {
            _curUniqueGeom++;
            p += 13;
            size_t i = 0;
            while ((p = _dangling.find("(", p + 1)) != std::string::npos) {
              if (_dangling[p + 1] == '(') p++;
              const auto& line = parseLineString(_dangling, p + 1);
              if (line.size() == 0) {
                if (i == 0) {
                  _qidToId.push_back({0, std::numeric_limits<ID_TYPE>::max()});
                }
              } else {
                _lines.push_back(_linePoints.size());
                insertLine(line);
                assert(_lines.size() - 1 <
                       std::numeric_limits<ID_TYPE>::max() - I_OFFSET);
                _qidToId.push_back(
                    {i == 0 ? 0 : 1, I_OFFSET + _lines.size() - 1});
              }
              i++;
            }
            if (i == 0)
              _qidToId.push_back({0, std::numeric_limits<ID_TYPE>::max()});
          } else {
            _qidToId.push_back({0, std::numeric_limits<ID_TYPE>::max()});
          }

          if (*c == '\n') {
            _curRow++;
            if (_curRow % 1000000 == 0) {
              LOG(INFO) << "[GEOMCACHE] "
                        << "@ row " << _curRow << " (" << _points.size()
                        << " points, " << _lines.size() << " (open) polygons)";
            }
            _prev = _dangling;
            _dangling.clear();
            c++;
            continue;
          } else {
            _prev = _dangling;
            _dangling.clear();
            c++;
            continue;
          }
        }

        _dangling += *c;
        c++;

        break;
      default:
        break;
    }
  }
}

// _____________________________________________________________________________
void GeomCache::parseIds(const char* c, size_t size) {
  for (size_t i = 0; i < size; i++) {
    _curId.bytes[_curByte] = c[i];
    _curByte = (_curByte + 1) % 8;

    if (_curByte == 0) {
      if (_curRow % 1000000 == 0) {
        LOG(INFO) << "[GEOMCACHE] "
                  << "@ row " << _curRow;
      }

      if (_curRow < _qidToId.size() && _qidToId[_curRow].qid == 0) {
        _qidToId[_curRow].qid = _curId.val;
        if (_curId.val > _maxQid) _maxQid = _curId.val;
      } else {
        LOG(WARN) << "The results for the binary IDs are out of sync.";
        LOG(WARN) << "_curRow: " << _curRow
                  << " _qleverIdInt.size: " << _qidToId.size()
                  << " cur val: " << _qidToId[_curRow].qid;
      }

      // if a qlever entity contained multiple geometries (MULTILINESTRING,
      // MULTIPOLYGON, MULTIPOIN), they appear consecutively in
      // _qidToId; continuation geometries are marked by a
      // preliminary qlever ID of 1, while the first geometry always has a
      // preliminary id of 0
      while (_curRow < _qidToId.size() - 1 && _qidToId[_curRow + 1].qid == 1) {
        _qidToId[++_curRow].qid = _curId.val;
      }

      _curRow++;
    }
  }
}

// _____________________________________________________________________________
void GeomCache::requestPart(size_t offset) {
  _state = IN_HEADER;
  _dangling.clear();
  _dangling.reserve(10000);

  CURLcode res;
  char errbuf[CURL_ERROR_SIZE];

  if (_curl) {
    auto qUrl = queryUrl(QUERY, offset, 1000000);

    curl_easy_setopt(_curl, CURLOPT_URL, qUrl.c_str());
    curl_easy_setopt(_curl, CURLOPT_WRITEFUNCTION, GeomCache::writeCb);
    curl_easy_setopt(_curl, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(_curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(_curl, CURLOPT_SSL_VERIFYPEER, false);
    curl_easy_setopt(_curl, CURLOPT_SSL_VERIFYHOST, false);

    // set headers
    struct curl_slist* headers = 0;
    headers = curl_slist_append(headers, "Accept: text/tab-separated-values");
    curl_easy_setopt(_curl, CURLOPT_HTTPHEADER, headers);

    // accept any compression supported
    curl_easy_setopt(_curl, CURLOPT_ACCEPT_ENCODING, "");
    res = curl_easy_perform(_curl);

    if (_exceptionPtr) std::rethrow_exception(_exceptionPtr);
  } else {
    LOG(ERROR) << "[GEOMCACHE] Failed to perform curl request.";
    return;
  }

  // check if there was an error
  if (res != CURLE_OK) {
    size_t len = strlen(errbuf);
    if (len > 0) {
      LOG(ERROR) << "[GEOMCACHE] " << errbuf;
    } else {
      LOG(ERROR) << "[GEOMCACHE] " << curl_easy_strerror(res);
    }
  }
}

// _____________________________________________________________________________
void GeomCache::request() {
  _state = IN_HEADER;
  _points.clear();
  _lines.clear();
  _linePoints.clear();
  _qidToId.clear();
  _curRow = 0;
  _curUniqueGeom = 0;

  size_t lastNum = -1;

  LOG(INFO) << "[GEOMCACHE] Allocating memory... ";

  size_t NUM_QIDS = 1600000000;
  size_t NUM_LINE_POINTS = 15000000000;
  size_t NUM_POINTS = 200000000;

  // checkMem(NUM_QIDS * sizeof(IdMapping) + NUM_LINE_POINTS *
  // sizeof(util::geo::Point<int16_t>) + NUM_POINTS * sizeof(util::geo::FPoint),
  // _maxMemory);

  _qidToId.reserve(NUM_QIDS);
  _linePoints.reserve(NUM_LINE_POINTS);
  _points.reserve(NUM_POINTS);

  LOG(INFO) << "[GEOMCACHE] Query is:\n" << QUERY;

  while (lastNum != 0) {
    size_t offset = _curRow;
    requestPart(offset);
    lastNum = _curRow - offset;
  }

  LOG(INFO) << "[GEOMCACHE] Done";
  LOG(INFO) << "[GEOMCACHE] Received " << _curUniqueGeom << " unique geoms";
  LOG(INFO) << "[GEOMCACHE] Received " << _points.size() << " points and "
            << _lines.size() << " lines";
}

// _____________________________________________________________________________
void GeomCache::requestIds() {
  _curByte = 0;
  _curRow = 0;
  _curUniqueGeom = 0;
  _maxQid = 0;
  _exceptionPtr = 0;

  LOG(INFO) << "[GEOMCACHE] Query is " << QUERY;

  if (_curl) {
    auto qUrl = queryUrl(QUERY, 0, MAXROWS);
    LOG(INFO) << "[GEOMCACHE] Binary ID query URL is " << qUrl;
    curl_easy_setopt(_curl, CURLOPT_URL, qUrl.c_str());
    curl_easy_setopt(_curl, CURLOPT_WRITEFUNCTION, GeomCache::writeCbIds);
    curl_easy_setopt(_curl, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(_curl, CURLOPT_SSL_VERIFYPEER, false);
    curl_easy_setopt(_curl, CURLOPT_SSL_VERIFYHOST, false);

    // set headers
    struct curl_slist* headers = 0;
    headers = curl_slist_append(headers, "Accept: application/octet-stream");
    curl_easy_setopt(_curl, CURLOPT_HTTPHEADER, headers);

    // accept any compression supported
    curl_easy_setopt(_curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_perform(_curl);
    if (_exceptionPtr) std::rethrow_exception(_exceptionPtr);
  } else {
    LOG(ERROR) << "[GEOMCACHE] Failed to perform curl request.";
  }

  LOG(INFO) << "[GEOMCACHE] Received " << _curRow << " rows";
  LOG(INFO) << "[GEOMCACHE] Max QLever id was " << _maxQid;
  LOG(INFO) << "[GEOMCACHE] Done";

  // sorting by qlever id
  LOG(INFO) << "[GEOMCACHE] Sorting results by qlever ID...";
  std::sort(_qidToId.begin(), _qidToId.end());
  LOG(INFO) << "[GEOMCACHE] ... done";
}

// _____________________________________________________________________________
std::string GeomCache::queryUrl(std::string query, size_t offset,
                                size_t limit) const {
  std::stringstream ss;

  if (util::toLower(query).find("limit") == std::string::npos) {
    query += " LIMIT " + std::to_string(limit);
  }

  if (util::toLower(query).find("offset") == std::string::npos) {
    query += " OFFSET " + std::to_string(offset);
  }

  auto esc = curl_easy_escape(_curl, query.c_str(), query.size());

  ss << _backendUrl << "/?send=" << std::to_string(MAXROWS) << "&query=" << esc;

  return ss.str();
}

// _____________________________________________________________________________
bool GeomCache::pointValid(const FPoint& p) {
  if (p.getY() > std::numeric_limits<float>::max()) return false;
  if (p.getY() < std::numeric_limits<float>::lowest()) return false;
  if (p.getX() > std::numeric_limits<float>::max()) return false;
  if (p.getX() < std::numeric_limits<float>::lowest()) return false;

  return true;
}

// _____________________________________________________________________________
util::geo::FLine GeomCache::parseLineString(const std::string& a,
                                            size_t p) const {
  util::geo::FLine line;
  line.reserve(2);
  auto end = memchr(a.c_str() + p, ')', a.size() - p);
  assert(end);

  while (true) {
    auto point = latLngToWebMerc(FPoint(
        util::atof(a.c_str() + p, 10),
        util::atof(
            static_cast<const char*>(memchr(a.c_str() + p, ' ', a.size() - p)) +
                1,
            10)));

    if (pointValid(point)) line.push_back(point);

    auto n = memchr(a.c_str() + p, ',', a.size() - p);
    if (!n || n > end) break;
    p = static_cast<const char*>(n) - a.c_str() + 1;
  }

  return util::geo::simplify(line, 3);
}

// _____________________________________________________________________________
util::geo::FPoint GeomCache::parsePoint(const std::string& a, size_t p) const {
  auto point = latLngToWebMerc(FPoint(
      util::atof(a.c_str() + p, 10),
      util::atof(
          static_cast<const char*>(memchr(a.c_str() + p, ' ', a.size() - p)) +
              1,
          10)));

  return point;
}

// _____________________________________________________________________________
std::vector<std::pair<ID_TYPE, ID_TYPE>> GeomCache::getRelObjects(
    const std::vector<IdMapping>& ids) const {
  // (geom id, result row)
  std::vector<std::pair<ID_TYPE, ID_TYPE>> ret;

  // in most cases, the return size will be exactly the size of the ids set
  ret.reserve(ids.size());

  size_t i = 0;
  size_t j = 0;

  while (i < ids.size() && j < _qidToId.size()) {
    if (ids[i].qid == _qidToId[j].qid) {
      ret.push_back({_qidToId[j].id, ids[i].id});
      j++;
    } else if (ids[i].qid < _qidToId[j].qid) {
      i++;
    } else {
      j++;
    }
  }

  return ret;
}

// _____________________________________________________________________________
void GeomCache::insertLine(const util::geo::FLine& l) {
  // we also add the line's bounding box here to also
  // compress that
  const auto& bbox = util::geo::getBoundingBox(l);

  int16_t mainX = bbox.getLowerLeft().getX() / M_COORD_GRANULARITY;
  int16_t mainY = bbox.getLowerLeft().getY() / M_COORD_GRANULARITY;

  if (mainX != 0 || mainY != 0)
    _linePoints.push_back({mCoord(mainX), mCoord(mainY)});

  // add bounding box lower left
  int16_t minorXLoc = bbox.getLowerLeft().getX() - mainX * M_COORD_GRANULARITY;
  int16_t minorYLoc = bbox.getLowerLeft().getY() - mainY * M_COORD_GRANULARITY;
  _linePoints.push_back({minorXLoc, minorYLoc});

  // add bounding box upper left
  int16_t mainXLoc = bbox.getUpperRight().getX() / M_COORD_GRANULARITY;
  int16_t mainYLoc = bbox.getUpperRight().getY() / M_COORD_GRANULARITY;
  minorXLoc = bbox.getUpperRight().getX() - mainXLoc * M_COORD_GRANULARITY;
  minorYLoc = bbox.getUpperRight().getY() - mainYLoc * M_COORD_GRANULARITY;
  if (mainXLoc != mainX || mainYLoc != mainY) {
    mainX = mainXLoc;
    mainY = mainYLoc;

    _linePoints.push_back({mCoord(mainX), mCoord(mainY)});
  }
  _linePoints.push_back({minorXLoc, minorYLoc});

  // add line points
  for (const auto& p : l) {
    mainXLoc = p.getX() / M_COORD_GRANULARITY;
    mainYLoc = p.getY() / M_COORD_GRANULARITY;

    if (mainXLoc != mainX || mainYLoc != mainY) {
      mainX = mainXLoc;
      mainY = mainYLoc;

      _linePoints.push_back({mCoord(mainX), mCoord(mainY)});
    }

    int16_t minorXLoc = p.getX() - mainXLoc * M_COORD_GRANULARITY;
    int16_t minorYLoc = p.getY() - mainYLoc * M_COORD_GRANULARITY;

    _linePoints.push_back({minorXLoc, minorYLoc});
  }
}

// _____________________________________________________________________________
util::geo::FBox GeomCache::getLineBBox(size_t lid) const {
  util::geo::FBox ret;
  size_t start = getLine(lid);

  bool s = false;

  double mainX = 0;
  double mainY = 0;
  for (size_t i = start; i < start + 4; i++) {
    // extract real geom
    const auto& cur = _linePoints[i];

    if (isMCoord(cur.getX())) {
      mainX = rmCoord(cur.getX());
      mainY = rmCoord(cur.getY());
      continue;
    }

    util::geo::FPoint curP(mainX * M_COORD_GRANULARITY + cur.getX(),
                           mainY * M_COORD_GRANULARITY + cur.getY());

    if (!s) {
      ret.setLowerLeft(curP);
      s = true;
    } else {
      ret.setUpperRight(curP);
      return ret;
    }
  }

  return ret;
}

// _____________________________________________________________________________
void GeomCache::fromDisk(const std::string& fname) {
  _points.clear();
  _linePoints.clear();
  _lines.clear();

  std::ifstream f(fname, std::ios::binary);

  size_t numPoints;
  f.read(reinterpret_cast<char*>(&numPoints), sizeof(size_t));
  _points.resize(numPoints);
  f.read(reinterpret_cast<char*>(&_points[0]),
         sizeof(util::geo::FPoint) * numPoints);

  f.read(reinterpret_cast<char*>(&numPoints), sizeof(size_t));
  _linePoints.resize(numPoints);
  f.read(reinterpret_cast<char*>(&_linePoints[0]),
         sizeof(util::geo::Point<int16_t>) * numPoints);

  f.read(reinterpret_cast<char*>(&numPoints), sizeof(size_t));
  _lines.resize(numPoints);
  f.read(reinterpret_cast<char*>(&_lines[0]), sizeof(size_t) * numPoints);

  f.read(reinterpret_cast<char*>(&numPoints), sizeof(size_t));
  _qidToId.resize(numPoints);
  f.read(reinterpret_cast<char*>(&_qidToId[0]), sizeof(IdMapping) * numPoints);

  f.close();
}

// _____________________________________________________________________________
void GeomCache::serializeToDisk(const std::string& fname) const {
  std::ofstream f;
  f.open(fname);

  size_t numPoints = _points.size();
  f.write(reinterpret_cast<const char*>(&numPoints), sizeof(size_t));
  f.write(reinterpret_cast<const char*>(&_points[0]),
          sizeof(util::geo::FPoint) * numPoints);

  numPoints = _linePoints.size();
  f.write(reinterpret_cast<const char*>(&numPoints), sizeof(size_t));
  f.write(reinterpret_cast<const char*>(&_linePoints[0]),
          sizeof(util::geo::Point<int16_t>) * numPoints);

  numPoints = _lines.size();
  f.write(reinterpret_cast<const char*>(&numPoints), sizeof(size_t));
  f.write(reinterpret_cast<const char*>(&_lines[0]),
          sizeof(size_t) * numPoints);

  numPoints = _qidToId.size();
  f.write(reinterpret_cast<const char*>(&numPoints), sizeof(size_t));
  f.write(reinterpret_cast<const char*>(&_qidToId[0]),
          sizeof(IdMapping) * numPoints);

  f.close();
}
