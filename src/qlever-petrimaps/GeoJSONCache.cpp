// Copyright 2022, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Authors: Patrick Brosi <brosi@informatik.uni-freiburg.de>

#include <3rdparty/nlohmann/json.hpp>

#include "GeoJSONCache.h"

using petrimaps::GeoJSONCache;
using json = nlohmann::json;

// _____________________________________________________________________________
double GeoJSONCache::getLoadStatusPercentTotal() {
  if (_totalSize == 0) {
    return 0.0;
  }

  double totalPercent = 0.0;
  switch (_loadStatusStage) {
    case _LoadStatusStages::Parse:
      totalPercent += _curRow / static_cast<double>(_totalSize) * 100.0;
      break;
  }

  return totalPercent;
}

// _____________________________________________________________________________
int GeoJSONCache::getLoadStatusStage() {
  return _loadStatusStage;
}

// _____________________________________________________________________________
std::vector<std::pair<ID_TYPE, ID_TYPE>> GeoJSONCache::getRelObjects() const {
  // Used for GeoJSON, returns all objects as vector<pair<geomID, Row>>
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
void GeoJSONCache::load(const std::string& _cacheDir) {
  _loadStatusStage = _LoadStatusStages::Parse;

  json res = json::parse(_content);

  // Parse json
  if (res["type"] != "FeatureCollection") {
    LOG(INFO) << "GeoJSON content is not a FeatureCollection.";
    return;
  }

  // Parse features
  auto features = res["features"];
  _totalSize = features.size();
  _curRow = 0;
  _curUniqueGeom = 0;
  if (_totalSize == 0) {
    throw std::runtime_error("Number of rows was 0");
  }

  _points.clear();
  _lines.clear();
  _linePoints.clear();

  size_t numPoints = 0;
  size_t numLines = 0;
  for (json feature : features) {
    // Parse type
    if (feature["type"] != "Feature") {
      LOG(INFO) << "[GeomCache] Non-Feature detected. Skipping...";
      continue;
    }
    // Parse geometry
    if (!feature.contains("geometry")) {
      LOG(INFO) << "[GeomCache] Feature has no geometry. Skipping...";
      continue;
    }

    json geom = feature["geometry"];
    std::string type = geom["type"];
    auto coords = geom["coordinates"];
    auto properties = feature["properties"];

    // PRIMITIVES
    // Point
    if (type == "Point") {
      FPoint point = latLngToWebMerc(FPoint(coords[0], coords[1]));
      if (!pointValid(point)) {
        LOG(INFO) << "[GeomCache] Invalid point found. Skipping...";
        continue;
      }
      _points.push_back({point, true});

      _curUniqueGeom++;
      _attr[numPoints] = properties;
      numPoints++;
    
    // LineString
    } else if (type == "LineString") {
      util::geo::DLine line;
      line.reserve(coords.size());

      for (std::vector<float> coord : coords) {
        DPoint point = latLngToWebMerc(DPoint(coord[0], coord[1]));
        if (!pointValid(point)) {
          LOG(INFO) << "[GeomCache] Invalid point found. Skipping...";
          continue;
        }
        line.push_back(point);
      }
      std::size_t idx = _linePoints.size();
      _lines.push_back({idx, true});
      line = util::geo::densify(line, 200 * 3);
      insertLine(line, false);

      _curUniqueGeom++;
      _attr[numLines + I_OFFSET] = properties;
      numLines++;
    
    // Polygon
    } else if (type == "Polygon") {
      for (size_t i = 0; i < coords.size(); i++) {
        auto args = coords[i];
        util::geo::DLine line;
        line.reserve(args.size());

        for (std::vector<float> coord : args) {
          DPoint point = latLngToWebMerc(DPoint(coord[0], coord[1]));
          if (!pointValid(point)) {
            LOG(INFO) << "[GeomCache] Invalid point found. Skipping...";
            continue;
          }
          line.push_back(point);
        }

        std::size_t idx = _linePoints.size();
        bool isFirst = i == 0;
        _lines.push_back({idx, isFirst});
        line = util::geo::densify(line, 200 * 3);
        insertLine(line, true);
      }

      _curUniqueGeom++;
      _attr[numLines + I_OFFSET] = properties;
      numLines++;

    // MULTIPART
    // MultiPoint
    } else if (type == "MultiPoint") {
      for (size_t i = 0; i < coords.size(); i++) {
        std::vector<float> coord = coords[i];
        FPoint point = latLngToWebMerc(FPoint(coord[0], coord[1]));
        if (!pointValid(point)) {
          LOG(INFO) << "[GeomCache] Invalid point found. Skipping...";
          continue;
        }
        bool isFirst = i == 0;
        _points.push_back({point, isFirst});
      }

      _curUniqueGeom++;
      _attr[numPoints] = properties;
      numPoints++;

    // MultiLineString
    } else if (type == "MultiLineString") {
      for (size_t i = 0; i < coords.size(); i++) {
        auto args = coords[i];
        util::geo::DLine line;
        line.reserve(args.size());

        for (std::vector<float> coord : args) {
          DPoint point = latLngToWebMerc(DPoint(coord[0], coord[1]));
          if (!pointValid(point)) {
            LOG(INFO) << "[GeomCache] Invalid point found. Skipping...";
            continue;
          }
          line.push_back(point);
        }
        std::size_t idx = _linePoints.size();
        bool isFirst = i == 0;
        _lines.push_back({idx, isFirst});
        line = util::geo::densify(line, 200 * 3);
        insertLine(line, false);
      }

      _curUniqueGeom++;
      _attr[numLines + I_OFFSET] = properties;
      numLines++;
    
    // MultiPolygon
    } else if (type == "MultiPolygon") {
      for (size_t i = 0; i < coords.size(); i++) {
        auto args1 = coords[i];
        for (auto args2 : args1) {
          util::geo::DLine line;
          line.reserve(args2.size());

          for (std::vector<float> coord : args2) {
            DPoint point = latLngToWebMerc(DPoint(coord[0], coord[1]));
            if (!pointValid(point)) {
              LOG(INFO) << "[GeomCache] Invalid point found. Skipping...";
              continue;
            }
            line.push_back(point);
          }
          std::size_t idx = _linePoints.size();
          bool isFirst = i == 0;
          _lines.push_back({idx, isFirst});
          line = util::geo::densify(line, 200 * 3);
          insertLine(line, true);
        }
      }

      _curUniqueGeom++;
      _attr[numLines + I_OFFSET] = properties;
      numLines++;
    }

    _curRow++;
  }

  _ready = true;

  LOG(INFO) << "[GEOMCACHE] Done";
  LOG(INFO) << "[GEOMCACHE] Received " << _curUniqueGeom << " unique geoms";
  LOG(INFO) << "[GEOMCACHE] Received " << _points.size() << " points and "
            << _lines.size() << " lines";
}

void GeoJSONCache::setContent(const std::string& content) {
  _content = content;
}
