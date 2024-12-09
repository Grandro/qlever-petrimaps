// Copyright 2022, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Authors: Patrick Brosi <brosi@informatik.uni-freiburg.de>

#include "SQLRequestor.h"

using petrimaps::SQLRequestor;

void SQLRequestor::request() {
  std::lock_guard<std::mutex> guard(_m);

  if (_ready) {
    // nothing to do
    return;
  }
  if (!_cache->ready()) {
    throw std::runtime_error("Geom cache not ready");
  }

  _ready = false;
  _objects.clear();
  _clusterObjects.clear();
  _rowIdToObjectId.clear();

  _objects = _cache->getRelObjects();
  _numObjects = _objects.size();

  LOG(INFO) << "[REQUESTOR] ... done, got " << _objects.size() << " objects.";

  // Create mapping rowId to objectId for multigeometries
  for (size_t objectId = 0; objectId < _objects.size(); objectId++) {
    std::pair<ID_TYPE, ID_TYPE> object = _objects[objectId];
    ID_TYPE rowId = object.second;
    _rowIdToObjectId[rowId] = objectId;
  }

  LOG(INFO) << "[REQUESTOR] Calculating bounding box of result...";

  util::geo::FBox pointBbox;
  util::geo::DBox lineBbox;
  createBboxes(pointBbox, lineBbox);

  LOG(INFO) << "[REQUESTOR] ... done";
  LOG(INFO) << "[REQUESTOR] Point BBox: " << util::geo::getWKT(pointBbox);
  LOG(INFO) << "[REQUESTOR] Line BBox: " << util::geo::getWKT(lineBbox);
  LOG(INFO) << "[REQUESTOR] Building grid...";

  createGrid(pointBbox, lineBbox);

  _ready = true;

  LOG(INFO) << "[REQUESTOR] ...done";
}

// _____________________________________________________________________________
std::vector<std::pair<std::string, std::string>> SQLRequestor::requestRow(uint64_t row) const {
  if (!_cache->ready()) {
    throw std::runtime_error("Geom cache not ready");
  }

  std::map<std::string, std::string> rowAttr = _cache->getRowAttr(row);
  std::vector<std::pair<std::string, std::string>> pairs = getRowAttrPairs(rowAttr);

  return pairs;
}

void SQLRequestor::requestRows(std::function<void(std::vector<std::vector<std::pair<std::string, std::string>>>)> cb) const {
  if (!_cache->ready()) {
    throw std::runtime_error("Geom cache not ready");
  }

  std::vector<std::map<std::string, std::string>> attr = _cache->getAttr();
  std::vector<std::vector<std::pair<std::string, std::string>>> res;
  res.reserve(attr.size());
  for (size_t i = 0; i < attr.size(); i++) {
    std::map<std::string, std::string> rowAttr = attr[i];
    std::vector<std::pair<std::string, std::string>> pairs = getRowAttrPairs(rowAttr);
    res.push_back(pairs);
  }
  
  cb(res);
}

void SQLRequestor::requestRowsIncludeGeom(std::function<void(std::vector<std::vector<std::pair<std::string, std::string>>>)> cb) const {
  if (!_cache->ready()) {
    throw std::runtime_error("Geom cache not ready");
  }

  std::vector<std::map<std::string, std::string>> attr = _cache->getAttrIncludeGeom();
  std::vector<std::vector<std::pair<std::string, std::string>>> res;
  res.reserve(attr.size());
  for (size_t i = 0; i < attr.size(); i++) {
    std::map<std::string, std::string> rowAttr = attr[i];
    std::vector<std::pair<std::string, std::string>> pairs = getRowAttrPairs(rowAttr);
    res.push_back(pairs);
  }
  
  cb(res);
}

std::vector<std::pair<std::string, std::string>> SQLRequestor::getRowAttrPairs(std::map<std::string, std::string> rowAttr) const {
  std::vector<std::pair<std::string, std::string>> pairs;
  pairs.reserve(rowAttr.size());
  for (auto const& keyVal : rowAttr) {
    std::string key = keyVal.first;
    std::string val = keyVal.second;
    std::pair<std::string, std::string> pair{key, val};
    pairs.push_back(pair);
  }

  return pairs;
}