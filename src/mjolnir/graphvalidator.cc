
#include "mjolnir/graphvalidator.h"
#include "mjolnir/graphtilebuilder.h"
#include "mjolnir/statistics.h"

#include <valhalla/midgard/logging.h>

#include <ostream>
#include <boost/format.hpp>
#include <sstream>
#include <iostream>
#include <string>
#include <vector>
#include <utility>
#include <queue>
#include <list>
#include <thread>
#include <future>
#include <mutex>
#include <numeric>

#include <valhalla/midgard/logging.h>
#include <valhalla/midgard/pointll.h>
#include <valhalla/midgard/distanceapproximator.h>
#include <valhalla/baldr/tilehierarchy.h>
#include <valhalla/baldr/graphid.h>
#include <valhalla/baldr/graphconstants.h>
#include <valhalla/baldr/graphreader.h>

using namespace valhalla::midgard;
using namespace valhalla::baldr;
using namespace valhalla::mjolnir;

namespace {

bool ShapesMatch(const std::vector<PointLL>& shape1,
		         const std::vector<PointLL>& shape2) {
  if (shape1.size() != shape2.size()) {
    return false;
  }

  if (shape1.front() == shape2.front()) {
    // Compare shape in forward direction
    auto iter1 = shape1.begin();
    auto iter2 = shape2.begin();
    for ( ; iter2 != shape2.end(); iter2++, iter1++) {
      if (*iter1 != *iter2) {
        return false;
      }
    }
    return true;
  } else if (shape1.front() == shape2.back()) {
    // Compare shape (reverse direction for shape2)
    auto iter1 = shape1.begin();
    auto iter2 = shape2.rbegin();
    for ( ; iter2 != shape2.rend(); iter2++, iter1++) {
      if (*iter1 != *iter2) {
        return false;
      }
    }
    return true;
  } else {
    LOG_INFO("Neither end of the shape matches");
    return false;
  }
}

// Get the GraphId of the opposing edge.
uint32_t GetOpposingEdgeIndex(const GraphId& startnode, DirectedEdge& edge,
		uint64_t wayid, const GraphTile* tile, const GraphTile* end_tile,
		uint32_t& dupcount, std::string& endnodeiso) {

  // Get the tile at the end node and get the node info
  GraphId endnode = edge.endnode();
  const NodeInfo* nodeinfo = end_tile->node(endnode.id());
  bool sametile = (startnode.tileid() == endnode.tileid());

  // Set the end node iso.  Used for country crossings.
  endnodeiso = end_tile->admin(nodeinfo->admin_index())->country_iso();

  // Get the directed edges and return when the end node matches
  // the specified node and length / transit attributes matches
  // Check for duplicates
  constexpr uint32_t absurd_index = 777777;
  uint32_t opp_index = absurd_index;
  const DirectedEdge* directededge = end_tile->directededge(
                  nodeinfo->edge_index());
  for (uint32_t i = 0; i < nodeinfo->edge_count(); i++, directededge++) {
    if (directededge->endnode() == startnode) {
      // If on the local level the logic for matching needs to include
      // transit edges and wayid matching
      if (startnode.level() == 2) {
        if (edge.IsTransitLine() && directededge->IsTransitLine()) {
          // For a transit edge the line Id must match
          if (edge.lineid() == directededge->lineid()) {
            if (opp_index != absurd_index) {
              LOG_ERROR("Multiple transit edges have the same line Id = " +
                          std::to_string(edge.lineid()));
              dupcount++;
            }
            opp_index = i;
          }
        } else if (!edge.IsTransitLine() && !directededge->IsTransitLine()) {
          // Non transit lines - length, shortcut flag, and wayids must match
          bool match = false;
          uint64_t wayid2 = 0;
          if ((edge.trans_down() && directededge->trans_up()) ||
              (edge.trans_up() && directededge->trans_down())) {
            match = true;
          } else if (edge.is_shortcut() == directededge->is_shortcut()) {
            // Match wayids and length. Also need to match edge info offset
            // or shape (if not in same tile)
            wayid2 = (directededge->trans_down() || directededge->trans_up()) ?
                  0 : end_tile->edgeinfo(directededge->edgeinfo_offset())->wayid();
            if (wayid == wayid2 &&
                edge.length() == directededge->length()) {
              if (sametile) {
                // If in same tile the edge info offsets must match
                if (edge.edgeinfo_offset() == directededge->edgeinfo_offset()) {
                  match = true;
                }
              } else {
                // Get shape for the edges
                auto shape1 = tile->edgeinfo(edge.edgeinfo_offset())->shape();
                auto shape2 = end_tile->edgeinfo(directededge->edgeinfo_offset())->shape();
                if (ShapesMatch(shape1, shape2)) {
                  match = true;
                }
              }
            }
          }
          if (match) {
            // Check if multiple edges match - log any duplicates
            if (opp_index != absurd_index) {
              if (startnode.level() == 2) {
                LOG_INFO("Potential duplicate: wayids " + std::to_string(wayid) +
                         " and " + std::to_string(wayid2) + " level = " +
                         std::to_string(startnode.level()) +
                         " sametile = " + std::to_string(sametile));
              }
              dupcount++;
            }
            opp_index = i;
          }
        }
      } else {
        // If not on the local level the length and shortcut flag must match
        if (edge.is_shortcut() == directededge->is_shortcut() &&
            edge.length() == directededge->length()) {
          if (opp_index != absurd_index) {
            dupcount++;
          }
          opp_index = i;
        }
      }
    }
  }

  if (opp_index == absurd_index) {
    if (edge.use() == Use::kTransitConnection) {
      // Log error - no opposing edge for a transit connection
      LOG_ERROR("No opposing transit connection edge: endstop = " +
                std::to_string(nodeinfo->stop_index()) + " has " +
                std::to_string(nodeinfo->edge_count()));
    } else if (edge.IsTransitLine()) {
      // TODO - add this when opposing transit edges with unique line Ids
      // are present
      ; /*LOG_ERROR("No opposing transit edge: endstop = " +
               std::to_string(nodeinfo->stop_index()) + " has " +
               std::to_string(nodeinfo->edge_count())); */
    } else {
      bool sc = edge.shortcut();
      LOG_ERROR((boost::format("No opposing edge at LL=%1%,%2% Length = %3% Startnode %4% EndNode %5% Shortcut %6%")
          % nodeinfo->latlng().lat() % nodeinfo->latlng().lng() % edge.length()
          % startnode % edge.endnode() % sc).str());

      uint32_t n = 0;
      directededge = end_tile->directededge(nodeinfo->edge_index());
      for (uint32_t i = 0; i < nodeinfo->edge_count(); i++, directededge++) {
        if (sc == directededge->is_shortcut() && directededge->is_shortcut()) {
          LOG_WARN((boost::format("    Length = %1% Endnode: %2%")
          % directededge->length() % directededge->endnode()).str());
          n++;
        }
      }
      if (n == 0) {
        if (sc) {
          LOG_WARN("   No Shortcut edges found from end node");
        } else {
          LOG_WARN("   No regular edges found from end node");
        }
      }
    }
    return kMaxEdgesPerNode;
  }
  return opp_index;
}

void validate(const boost::property_tree::ptree& pt,
              std::queue<GraphId>& tilequeue, std::mutex& lock,
              std::promise<validator_stats>& result) {

    // Our local class for gathering the stats
    validator_stats vStats;
    // Local Graphreader
    GraphReader graph_reader(pt.get_child("mjolnir.hierarchy"));
    // Get some things we need throughout
    const auto& tile_hierarchy = graph_reader.GetTileHierarchy();
    std::vector<Tiles<PointLL> > levels;
    for (const auto& level : tile_hierarchy.levels()) {
      levels.push_back(level.second.tiles);
    }
    Tiles<PointLL> *tiles;

    // Check for more tiles
    while (true) {
      lock.lock();
      if (tilequeue.empty()) {
        lock.unlock();
        break;
      }
      // Get the next tile Id
      GraphId tile_id = tilequeue.front();
      tilequeue.pop();
      lock.unlock();

      // Point tiles to the set we need for current level
      size_t level = tile_id.level();
      tiles = &levels[level];
      auto tileid = tile_id.tileid();

      uint32_t dupcount = 0;

      // Get the tile
      GraphTileBuilder tilebuilder(tile_hierarchy, tile_id, false);

      // Bin the edges keeping a list of ones that need to go to other tiles
      if(tile_id.level() == tile_hierarchy.levels().rbegin()->first)
        auto unbinned = tilebuilder.Bin();

      // Update nodes and directed edges as needed
      std::vector<NodeInfo> nodes;
      std::vector<DirectedEdge> directededges;

      // Get this tile
      lock.lock();
      const GraphTile* tile = graph_reader.GetGraphTile(tile_id);
      lock.unlock();

      // Iterate through the nodes and the directed edges
      float roadlength = 0.0f;
      uint32_t nodecount = tilebuilder.header()->nodecount();
      GraphId node = tile_id;
      for (uint32_t i = 0; i < nodecount; i++, node++) {
        NodeInfo nodeinfo = tilebuilder.node(i);
        auto ni = tile->node(i);
        std::string begin_node_iso = tile->admin(nodeinfo.admin_index())->country_iso();

        // Go through directed edges and validate/update data
        uint32_t idx = ni->edge_index();
        for (uint32_t j = 0, n = nodeinfo.edge_count(); j < n; j++, idx++) {
          auto de = tile->directededge(idx);

          // Validate signs
          if (de->exitsign()) {
            if (tile->GetSigns(idx).size() == 0) {
              LOG_ERROR("Directed edge marked as having signs but none found");
            }
          }

          //for stats
          bool hazmat = false, axle_load = false, height = false, length = false,
               weight = false, width = false;

          // Validate access restrictions. TODO - should check modes as well
          uint32_t ar_modes = de->access_restriction();
          if (ar_modes) {
            //since only truck restrictions exist, we can still get all restrictions
            //later we may only want to get just the truck ones for stats.
            auto res = tile->GetAccessRestrictions(idx, kAllAccess);
            if (res.size() == 0) {
              LOG_ERROR("Directed edge marked as having access restriction but none found ; tile level = " +
                        std::to_string(tile_id.level()));
            } else {
              for (auto r : res) {
                if (r.edgeindex() != idx) {
                  LOG_ERROR("Access restriction edge index does not match idx");
                } else {// log stats.  currently only for truck right now
                  switch (r.type()) {
                    case AccessType::kHazmat:
                      hazmat = true;
                      break;
                    case AccessType::kMaxAxleLoad:
                      axle_load = true;
                      break;
                    case AccessType::kMaxHeight:
                      height = true;
                      break;
                    case AccessType::kMaxLength:
                      length = true;
                      break;
                    case AccessType::kMaxWeight:
                      weight = true;
                      break;
                    case AccessType::kMaxWidth:
                      width = true;
                      break;
                    default:
                      break;
                  }
                }
              }
            }
          }

          DirectedEdge& directededge = tilebuilder.directededge(
                    nodeinfo.edge_index() + j);

          // Road Length and some variables for statistics
          float tempLength;
          bool validLength = false;
          if (!directededge.shortcut() && !directededge.trans_up() &&
              !directededge.trans_down()) {
            tempLength = directededge.length();
            roadlength += tempLength;
            validLength = true;
          }

          // Check if end node is in a different tile
          const GraphTile* endnode_tile = tile;
          if (tile_id != directededge.endnode().Tile_Base()) {
            directededge.set_leaves_tile(true);

            // Get the end node tile
            lock.lock();
            endnode_tile = graph_reader.GetGraphTile(directededge.endnode());
            lock.unlock();
          }

          // Set the opposing edge index and get the country ISO at the
          // end node)
          std::string end_node_iso;
          uint64_t wayid = (directededge.trans_down() || directededge.trans_up()) ?
                 0 : tile->edgeinfo(directededge.edgeinfo_offset())->wayid();
          directededge.set_opp_index(GetOpposingEdgeIndex(node, directededge,
                         wayid, tile, endnode_tile, dupcount, end_node_iso));

          // Mark a country crossing if country ISO codes do not match
          if (!begin_node_iso.empty() && !end_node_iso.empty() &&
              begin_node_iso != end_node_iso) {
            directededge.set_ctry_crossing(true);
          }

          directededges.emplace_back(std::move(directededge));

          auto rclass = directededge.classification();
          // Add truck stats.
          if (validLength) {

            tempLength /= (tileid == directededge.endnode().tileid()) ? 2 : 4;

            if (directededge.truck_route()) {
              vStats.add_tile_truck_route(tileid, rclass, tempLength);
              vStats.add_country_truck_route(begin_node_iso, rclass, tempLength);
            }
            if (hazmat) {
              vStats.add_tile_hazmat(tileid, rclass, tempLength);
              vStats.add_country_hazmat(begin_node_iso, rclass, tempLength);
            }
            if (axle_load) {
              vStats.add_tile_axle_load(tileid, rclass);
              vStats.add_country_axle_load(begin_node_iso, rclass);
            }
            if (height) {
              vStats.add_tile_height(tileid, rclass);
              vStats.add_country_height(begin_node_iso, rclass);
            }
            if (length) {
              vStats.add_tile_length(tileid, rclass);
              vStats.add_country_length(begin_node_iso, rclass);
            }
            if (weight) {
              vStats.add_tile_weight(tileid, rclass);
              vStats.add_country_weight(begin_node_iso, rclass);
            }
            if (width) {
              vStats.add_tile_width(tileid, rclass);
              vStats.add_country_width(begin_node_iso, rclass);
            }
          }

          // Add all other statistics
          // Only consider edge if edge is good and it's not a link
          if (validLength && !directededge.link()) {
            //Determine access for directed edge
            auto fward = ((kAutoAccess & directededge.forwardaccess()) == kAutoAccess);
            auto bward = ((kAutoAccess & directededge.reverseaccess()) == kAutoAccess);
            // Check if one way
            if ((!fward || !bward) && (fward || bward)) {
              vStats.add_tile_one_way(tileid, rclass, tempLength);
              vStats.add_country_one_way(begin_node_iso, rclass, tempLength);
            }
            // Check if this edge is internal
            if (directededge.internal()) {
              vStats.add_tile_int_edge(tileid, rclass);
              vStats.add_country_int_edge(begin_node_iso, rclass);
            }
            // Check if edge has maxspeed tag
            if (directededge.speed_type() == SpeedType::kTagged){
              vStats.add_tile_speed_info(tileid, rclass, tempLength);
              vStats.add_country_speed_info(begin_node_iso, rclass, tempLength);
            }
            // Check if edge has any names
            if (tilebuilder.edgeinfo(directededge.edgeinfo_offset())->name_count() > 0) {
              vStats.add_tile_named(tileid, rclass, tempLength);
              vStats.add_country_named(begin_node_iso, rclass, tempLength);
            }
            // Add road lengths to statistics for current country and tile
            vStats.add_country_road(begin_node_iso, rclass, tempLength);
            vStats.add_tile_road(tileid, rclass, tempLength);
          }
        }
        // Add the node to the list
        nodes.emplace_back(std::move(nodeinfo));
      }

      // Add density to return class. Approximate the tile area square km
      AABB2<PointLL> bb = tiles->TileBounds(tileid);
      float area = ((bb.maxy() - bb.miny()) * kMetersPerDegreeLat * kKmPerMeter) *
                   ((bb.maxx() - bb.minx()) *
                       DistanceApproximator::MetersPerLngDegree(bb.Center().y()) * kKmPerMeter);
      float density = (roadlength * 0.0005f) / area;
      vStats.add_density(density, level);
      vStats.add_tile_area(tileid, area);
      vStats.add_tile_geom(tileid, tiles->TileBounds(tileid));

      // Write the new tile
      lock.lock();
      tilebuilder.Update(nodes, directededges);

      // Check if we need to clear the tile cache
      if (graph_reader.OverCommitted())
        graph_reader.Clear();
      lock.unlock();

      // Add possible duplicates to return class
      vStats.add_dup(dupcount, level);
    }

    // Fill promise with statistics
    result.set_value(vStats);
  }
}

namespace valhalla {
namespace mjolnir {

  void GraphValidator::Validate(const boost::property_tree::ptree& pt) {

    // Graphreader
    TileHierarchy hierarchy(pt.get_child("mjolnir.hierarchy"));
    // Make sure there are at least 2 levels!
    if (hierarchy.levels().size() < 2)
      throw std::runtime_error("Bad tile hierarchy - need 2 levels");

    // Setup threads
    std::vector<std::shared_ptr<std::thread> > threads(
        std::max(static_cast<unsigned int>(1),
                 pt.get<unsigned int>("concurrency",std::thread::hardware_concurrency())));

    // Setup promises
    std::list<std::promise<validator_stats> > results;

    // Create a randomized queue of tiles to work from
    std::deque<GraphId> tempqueue;
    for (auto tier : hierarchy.levels()) {
      auto level = tier.second.level;
      auto tiles = tier.second.tiles;
      for (uint32_t id = 0; id < tiles.TileCount(); id++) {
        // If tile exists add it to the queue
        GraphId tile_id(id, level, 0);
        if (GraphReader::DoesTileExist(hierarchy, tile_id)) {
          tempqueue.push_back(tile_id);
        }
      }
    }
    std::random_shuffle(tempqueue.begin(), tempqueue.end());
    std::queue<GraphId> tilequeue(tempqueue);

    // An mutex we can use to do the synchronization
    std::mutex lock;

    LOG_INFO("Validating signs and connectivity");
    // Spawn the threads
    for (auto& thread : threads) {
      results.emplace_back();
      thread.reset(new std::thread(validate, std::cref(pt), std::ref(tilequeue),
                                   std::ref(lock), std::ref(results.back())));
    }
    // Wait for threads to finish
    for (auto& thread : threads) {
      thread->join();
    }
    // Get the returned data from the promise
    validator_stats stats;
    for (auto& result : results) {
      auto data = result.get_future().get();
      stats.add(data);
    }
    // Add up total dupcount_ and find densities
    LOG_INFO("Finished");
    for (uint8_t level = 0; level <= 2; level++) {
      // Print duplicates info for level
      std::vector<uint32_t> dups = stats.get_dups(level);
      uint32_t dupcount = std::accumulate(dups.begin(), dups.end(), 0);
      LOG_WARN((boost::format("Possible duplicates at level: %1% = %2%")
      % std::to_string(level) % dupcount).str());
      // Get the average density and the max density
      float max_density = 0.0f;
      float sum = 0.0f;
      for (auto density : stats.get_densities(level)) {
        if (density > max_density) {
          max_density = density;
        }
        sum += density;
      }
      float average_density = sum / stats.get_densities(level).size();
      LOG_DEBUG("Average density = " + std::to_string(average_density) +
               " max = " + std::to_string(max_density));
    }
    stats.build_db(pt);
  }
}
}
