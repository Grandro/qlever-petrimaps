// Copyright 2022, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Authors: Patrick Brosi <brosi@informatik.uni-freiburg.de>

#include <curl/curl.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include "GeomCache.h"
#include "qlever-petrimaps/Misc.h"
#include "qlever-petrimaps/server/Requestor.h"
#include "util/Misc.h"
#include "util/geo/Geo.h"
#include "util/geo/PolyLine.h"

using petrimaps::GeomCache;


double GeomCache::getLoadStatusPercentCurrent() {
  if (_totalSize == 0) {
    return 0.0;
  }

  double percent = _curRow / static_cast<double>(_totalSize) * 100.0;
  return std::min(100.0, percent);
}

// _____________________________________________________________________________
size_t GeomCache::getTotalProgress() { return _totalSize; }

// _____________________________________________________________________________
size_t GeomCache::getCurrentProgress() { return _curRow; }

// _____________________________________________________________________________
bool GeomCache::pointValid(const FPoint &p) {
  if (p.getY() > std::numeric_limits<float>::max()) return false;
  if (p.getY() < std::numeric_limits<float>::lowest()) return false;
  if (p.getX() > std::numeric_limits<float>::max()) return false;
  if (p.getX() < std::numeric_limits<float>::lowest()) return false;

  return true;
}

// _____________________________________________________________________________
bool GeomCache::pointValid(const DPoint &p) {
  if (p.getY() > std::numeric_limits<double>::max()) return false;
  if (p.getY() < std::numeric_limits<double>::lowest()) return false;
  if (p.getX() > std::numeric_limits<double>::max()) return false;
  if (p.getX() < std::numeric_limits<double>::lowest()) return false;

  return true;
}

// _____________________________________________________________________________
void GeomCache::insertLine(const util::geo::DLine& l, bool isArea) {
  const auto& bbox = util::geo::getBoundingBox(l);
  int16_t mainX = (bbox.getLowerLeft().getX() * 10.0) / M_COORD_GRANULARITY;
  int16_t mainY = (bbox.getLowerLeft().getY() * 10.0) / M_COORD_GRANULARITY;

  if (mainX != 0 || mainY != 0) {
    util::geo::Point<int16_t> p{mCoord(mainX), mCoord(mainY)};
    _linePoints.push_back(p);
  }

  // add bounding box lower left
  int16_t minorXLoc =
      (bbox.getLowerLeft().getX() * 10.0) - mainX * M_COORD_GRANULARITY;
  int16_t minorYLoc =
      (bbox.getLowerLeft().getY() * 10.0) - mainY * M_COORD_GRANULARITY;
  util::geo::Point<int16_t> p{minorXLoc, minorYLoc};
  _linePoints.push_back(p);

  // add bounding box upper left
  int16_t mainXLoc = (bbox.getUpperRight().getX() * 10.0) / M_COORD_GRANULARITY;
  int16_t mainYLoc = (bbox.getUpperRight().getY() * 10.0) / M_COORD_GRANULARITY;
  minorXLoc =
      (bbox.getUpperRight().getX() * 10.0) - mainXLoc * M_COORD_GRANULARITY;
  minorYLoc =
      (bbox.getUpperRight().getY() * 10.0) - mainYLoc * M_COORD_GRANULARITY;
  if (mainXLoc != mainX || mainYLoc != mainY) {
    mainX = mainXLoc;
    mainY = mainYLoc;
    util::geo::Point<int16_t> p{mCoord(mainX), mCoord(mainY)};
    _linePoints.push_back(p);
  }
  p = util::geo::Point<int16_t>{minorXLoc, minorYLoc};
  _linePoints.push_back(p);

  // add line points
  for (const auto& p : l) {
    mainXLoc = (p.getX() * 10.0) / M_COORD_GRANULARITY;
    mainYLoc = (p.getY() * 10.0) / M_COORD_GRANULARITY;

    if (mainXLoc != mainX || mainYLoc != mainY) {
      mainX = mainXLoc;
      mainY = mainYLoc;
      util::geo::Point<int16_t> p{mCoord(mainX), mCoord(mainY)};
      _linePoints.push_back(p);
    }

    int16_t minorXLoc = (p.getX() * 10.0) - mainXLoc * M_COORD_GRANULARITY;
    int16_t minorYLoc = (p.getY() * 10.0) - mainYLoc * M_COORD_GRANULARITY;
    util::geo::Point<int16_t> pp{minorXLoc, minorYLoc};
    _linePoints.push_back(pp);
  }

  // if we have an area, we end in a major coord (which is not possible for
  // other types)
  if (isArea) {
    util::geo::Point<int16_t> p{mCoord(0), mCoord(0)};
    _linePoints.push_back(p);
  }
}

// _____________________________________________________________________________
util::geo::DBox GeomCache::getLineBBox(size_t lid) const {
  util::geo::DBox ret;
  size_t start = getLine(lid);

  bool s = false;

  double mainX = 0;
  double mainY = 0;
  for (size_t i = start; i < start + 4; i++) {
    // extract real geom
    const auto &cur = _linePoints[i];

    if (isMCoord(cur.getX())) {
      mainX = rmCoord(cur.getX());
      mainY = rmCoord(cur.getY());
      continue;
    }

    util::geo::DPoint curP((mainX * M_COORD_GRANULARITY + cur.getX()) / 10.0,
                           (mainY * M_COORD_GRANULARITY + cur.getY()) / 10.0);

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
