// Copyright 2022, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Authors: Patrick Brosi <brosi@informatik.uni-freiburg.de>

#include <curl/curl.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include "qlever-mapui/GeomCache.h"
#include "qlever-mapui/Misc.h"
#include "qlever-mapui/server/Requestor.h"
#include "util/Misc.h"
#include "util/geo/Geo.h"
#include "util/geo/PolyLine.h"
#include "util/log/Log.h"

using mapui::GeomCache;

const static char* QUERY =
    "SELECT ?geometry WHERE {"
    " ?osm_id <http://www.opengis.net/ont/geosparql#hasGeometry> ?geometry"
    // " . ?osm_id <https://www.openstreetmap.org/wiki/Key:building> ?a"
    // " . ?osm_id <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> "
    // "<https://www.openstreetmap.org/relation> ."
    "} LIMIT "
    "18446744073709551615";
    // "1000000";

// _____________________________________________________________________________
size_t GeomCache::writeCb(void* contents, size_t size, size_t nmemb,
                          void* userp) {
  size_t realsize = size * nmemb;
  static_cast<GeomCache*>(userp)->parse(static_cast<const char*>(contents),
                                        realsize);
  return realsize;
}

// _____________________________________________________________________________
size_t GeomCache::writeCbIds(void* contents, size_t size, size_t nmemb,
                             void* userp) {
  size_t realsize = size * nmemb;
  static_cast<GeomCache*>(userp)->parseIds(static_cast<const char*>(contents),
                                           realsize);
  return realsize;
}

// _____________________________________________________________________________
void GeomCache::parse(const char* c, size_t size) {
  const char* start = c;
  const char* tmp;
  while ((c - start) < size) {
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
          bool isGeom = util::endsWith(
              _dangling, "^^<http://www.opengis.net/ont/geosparql#wktLiteral>");

          auto p = _dangling.rfind("\"POINT(", 0);

          if (p != std::string::npos) {
            p += 7;
            auto point = parsePoint(_dangling, p);
            if (point.getY() > std::numeric_limits<float>::max()) {
              continue;
            }

            if (point.getY() < std::numeric_limits<float>::lowest()) {
              continue;
            }

            if (point.getX() > std::numeric_limits<float>::max()) {
              continue;
            }

            if (point.getX() < std::numeric_limits<float>::lowest()) {
              continue;
            }
            _points.push_back(point);
            _qleverIdToInternalId.push_back({-1, _points.size() - 1});
          } else if ((p = _dangling.rfind("\"LINESTRING(", 0)) !=
                     std::string::npos) {
            p += 12;
            const auto& line = parseLineString(_dangling, p);
            _lines.push_back(line);
            _lineBoxes.push_back(util::geo::getBoundingBox(line));
            _qleverIdToInternalId.push_back({-1, I_OFFSET + _lines.size() - 1});
          } else if ((p = _dangling.rfind("\"MULTILINESTRING(", 0)) !=
                     std::string::npos) {
            p += 17;
            while ((p = _dangling.find("(", p + 1)) != std::string::npos) {
              const auto& line = parseLineString(_dangling, p + 1);
              _lines.push_back(line);
              _lineBoxes.push_back(util::geo::getBoundingBox(line));
              _qleverIdToInternalId.push_back({-1, I_OFFSET + _lines.size() - 1});
            }
          } else if ((p = _dangling.rfind("\"POLYGON(", 0)) !=
                     std::string::npos) {
            p += 9;
            while ((p = _dangling.find("(", p + 1)) != std::string::npos) {
              const auto& line = parseLineString(_dangling, p + 1);
              _lines.push_back(line);
              _lineBoxes.push_back(util::geo::getBoundingBox(line));
              _qleverIdToInternalId.push_back({-1, I_OFFSET + _lines.size() - 1});
            }
          } else if ((p = _dangling.rfind("\"MULTIPOLYGON(", 0)) !=
                     std::string::npos) {
            p += 13;
              while ((p = _dangling.find("(", p + 1)) != std::string::npos) {
                if (_dangling[p+1] == '(') p++;
                const auto& line = parseLineString(_dangling, p + 1);
                _lines.push_back(line);
                _lineBoxes.push_back(util::geo::getBoundingBox(line));
                _qleverIdToInternalId.push_back({-1, I_OFFSET + _lines.size() - 1});
              }
          } else {
            _qleverIdToInternalId.push_back({-1, 2 * I_OFFSET});
          }

          if (*c == '\n') {
            _dangling = "";
            c++;
            continue;
          } else {
            _dangling = "";
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
    _curByte = ++_curByte % 8;

    if (_curByte == 0) {
      _qleverIdToInternalId[_curRow].first = _curId.val;
      _curRow++;
    }
  }
}

// _____________________________________________________________________________
void GeomCache::request() {
  _state = IN_HEADER;
  _points.clear();
  _lines.clear();
  _dangling = "";
  CURLcode res;

  if (_curl) {
    auto qUrl = queryUrl(QUERY);
    LOG(INFO) << "Query URL is " << qUrl;
    curl_easy_setopt(_curl, CURLOPT_URL, qUrl.c_str());
    curl_easy_setopt(_curl, CURLOPT_WRITEFUNCTION, GeomCache::writeCb);
    curl_easy_setopt(_curl, CURLOPT_WRITEDATA, this);

    // set headers
    struct curl_slist* headers = 0;
    headers = curl_slist_append(headers, "Accept: text/tab-separated-values");
    curl_easy_setopt(_curl, CURLOPT_HTTPHEADER, headers);

    // accept any compression supported
    curl_easy_setopt(_curl, CURLOPT_ACCEPT_ENCODING, "");
    res = curl_easy_perform(_curl);
  }

  LOG(INFO) << "Done";
  LOG(INFO) << "Received " << _points.size() << " points and " << _lines.size()
            << " lines";
}

// _____________________________________________________________________________
void GeomCache::requestIds() {
  CURLcode res;

  _curByte = 0;
  _curRow = 0;

  if (_curl) {
    auto qUrl = queryUrl(QUERY);
    LOG(INFO) << "Binary ID query URL is " << qUrl;
    curl_easy_setopt(_curl, CURLOPT_URL, qUrl.c_str());
    curl_easy_setopt(_curl, CURLOPT_WRITEFUNCTION, GeomCache::writeCbIds);
    curl_easy_setopt(_curl, CURLOPT_WRITEDATA, this);

    // set headers
    struct curl_slist* headers = 0;
    headers = curl_slist_append(headers, "Accept: application/octet-stream");
    curl_easy_setopt(_curl, CURLOPT_HTTPHEADER, headers);

    // accept any compression supported
    curl_easy_setopt(_curl, CURLOPT_ACCEPT_ENCODING, "");
    res = curl_easy_perform(_curl);
  }

  LOG(INFO) << "Received " << _curRow << " rows";
  LOG(INFO) << "Done";

  // sorting by qlever id
  LOG(INFO) << "Sorting results by qlever ID...";
  std::sort(_qleverIdToInternalId.begin(), _qleverIdToInternalId.end());
  LOG(INFO) << "... done";
}

// _____________________________________________________________________________
std::string GeomCache::queryUrl(std::string query) const {
  std::stringstream ss;

  if (util::toLower(query).find("limit") == std::string::npos) {
    query += " LIMIT 18446744073709551615";
  }

  auto esc = curl_easy_escape(_curl, query.c_str(), query.size());

  ss << _backendUrl << "/?send=18446744073709551615"
     << "&query=" << esc;

  return ss.str();
}

// _____________________________________________________________________________
util::geo::FLine GeomCache::parseLineString(const std::string& a,
                                            size_t p) const {
  util::geo::FLine line;
  auto end = memchr(a.c_str() + p, ')', a.size() - p);
  assert(end);
  while (true) {
    auto point = util::geo::latLngToWebMerc(util::geo::FPoint{
        util::atof(a.c_str() + p, 10),
        util::atof(static_cast<const char*>(
                       memchr(a.c_str() + p, ' ', a.size() - p) + 1),
                   10)});

    if (point.getY() > std::numeric_limits<float>::max()) {
      continue;
    }

    if (point.getY() < std::numeric_limits<float>::lowest()) {
      continue;
    }

    if (point.getX() > std::numeric_limits<float>::max()) {
      continue;
    }

    if (point.getX() < std::numeric_limits<float>::lowest()) {
      continue;
    }

    line.push_back(point);

    auto n = memchr(a.c_str() + p, ',', a.size() - p);
    if (!n || n > end) break;
    p = static_cast<const char*>(n) - a.c_str() + 1;
  }

  return line;
}

// _____________________________________________________________________________
util::geo::FPoint GeomCache::parsePoint(const std::string& a, size_t p) const {
  auto point = util::geo::latLngToWebMerc(util::geo::FPoint{
      util::atof(a.c_str() + p, 10),
      util::atof(
          static_cast<const char*>(
              memchr(a.c_str() + p, ' ', a.size() - p) + 1),
          10)});

  return point;
}

// _____________________________________________________________________________
std::vector<std::pair<size_t, size_t>> GeomCache::getRelObjects(
    const std::vector<std::pair<uint64_t, uint64_t>>& ids) const {
  // (geom id, result row)
  std::vector<std::pair<size_t, size_t>> ret;

  size_t i = 0;
  size_t j = 0;

  while (i < ids.size() && j < _qleverIdToInternalId.size()) {
    if (ids[i].first == _qleverIdToInternalId[j].first) {
      ret.push_back({_qleverIdToInternalId[j].second, ids[i].second});
      i++;
      j++;
    } else if (ids[i].first < _qleverIdToInternalId[j].first) {
      i++;
    } else {
      j++;
    }
  }

  return ret;
}
