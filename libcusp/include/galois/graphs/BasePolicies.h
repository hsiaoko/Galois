#ifndef _GALOIS_CUSP_PSCAFFOLD_H_
#define _GALOIS_CUSP_PSCAFFOLD_H_

namespace galois {
namespace graphs {

/**
 * Default functions all CuSP partitioners use
 */
class PartitioningScaffold {
 protected:
  uint32_t _hostID;
  uint32_t _numHosts;
  uint64_t _numNodes;
  uint64_t _numEdges;
  std::vector<std::pair<uint64_t, uint64_t>> _gid2host;
 public:
  PartitioningScaffold(uint32_t hostID, uint32_t numHosts, uint64_t numNodes,
                       uint64_t numEdges) :
    _hostID(hostID), _numHosts(numHosts), _numNodes(numNodes),
    _numEdges(numEdges) { }

  void saveGIDToHost(std::vector<std::pair<uint64_t, uint64_t>>& gid2host) {
    _gid2host = gid2host;
  }

  virtual bool noCommunication() { return false; }
  virtual std::pair<unsigned, unsigned> cartesianGrid() {
    return std::make_pair(0u, 0u);
  }
};

/**
 * Policies that use the read assignment of nodes as the masters. Does not
 * need to go through  a master assignment phase, saving overhead.
 */
class ReadMasterAssignment : public PartitioningScaffold {
 public:
  ReadMasterAssignment(uint32_t hostID, uint32_t numHosts, uint64_t numNodes,
                       uint64_t numEdges) :
    PartitioningScaffold(hostID, numHosts, numNodes, numEdges) {}

  uint32_t getMaster(uint32_t gid) const {
    for (auto h = 0U; h < _numHosts; ++h) {
      uint64_t start, end;
      std::tie(start, end) = _gid2host[h];
      if (gid >= start && gid < end) {
        return h;
      }
    }
    assert(false);
    return _numHosts;
  }

  // below all unused if not assigning masters in default manner
  bool masterAssignPhase() const { return false; }
  void enterStage2() {}
  template<typename EdgeTy> uint32_t determineMaster(uint32_t,
      galois::graphs::BufferedGraph<EdgeTy>&,
      const std::vector<uint32_t>&,
      std::unordered_map<uint64_t, uint32_t>&,
      const std::vector<uint64_t>&,
      std::vector<galois::CopyableAtomic<uint64_t>>&,
      const std::vector<uint64_t>&,
      std::vector<galois::CopyableAtomic<uint64_t>>&) {
    return 0;
  }
  void saveGID2HostInfo(std::unordered_map<uint64_t, uint32_t>&,
                        std::vector<uint32_t>&, uint64_t) { }
  bool addMasterMapping(uint32_t, uint32_t) { return false; }

};

/**
 * Policies that use a custom assignment of masters (from the user).
 * Needs to go through  a master assignment phase, which adds overhead
 * to partitioning, but may get better quality partitions.
 */
class CustomMasterAssignment : public PartitioningScaffold {
 protected:
  char _status;
  // metadata for determining where a node's master is
  std::vector<uint32_t> _localNodeToMaster;
  std::unordered_map<uint64_t, uint32_t> _gid2masters;
  uint64_t _nodeOffset;

  unsigned getHostReader(uint64_t gid) const {
    for (auto i = 0U; i < _numHosts; ++i) {
      uint64_t start, end;
      std::tie(start, end) = _gid2host[i];
      if (gid >= start && gid < end) {
        return i;
      }
    }
    return -1;
  }

 public:
  CustomMasterAssignment(uint32_t hostID, uint32_t numHosts, uint64_t numNodes,
                         uint64_t numEdges) :
    PartitioningScaffold(hostID, numHosts, numNodes, numEdges), _status(0) {}

  /**
   * Implementation of get master: does not fail if a GID
   * mapping is not found but instead returns -1 if in stage 1, else
   * fails.
   *
   * @param gid GID to get master of
   * @returns Master of specified GID, -1, unsigned, if not found
   */
  uint32_t getMaster(uint32_t gid) const {
    if (_status != 0) {
      // use map if not a locally read node, else use vector
      if (getHostReader(gid) != _hostID) {
        auto gidMasterIter = _gid2masters.find(gid);
        // found in map
        if (gidMasterIter != _gid2masters.end()) {
          uint32_t mappedMaster = gidMasterIter->second;
          //galois::gDebug("[", _hostID, "] ", gid, " found with master ",
          //               mappedMaster, "!");
          // make sure host is in bounds
          assert(mappedMaster >= 0 && mappedMaster < _numHosts);
          return mappedMaster;
        } else {
          // NOT FOUND (not necessarily a bad thing, and required for
          // some cases)
          galois::gDebug("[", _hostID, "] ", gid, " not found!");
          if (_status == 2) {
            // die if we expect all gids to be mapped already (stage 2)
            GALOIS_DIE("should not fail to find a GID after stage 2 "
                       "partitioning");
          }
          return (uint32_t)-1;
        }
      } else {
        // determine offset
        uint32_t offsetIntoMap = gid - _nodeOffset;
        assert(offsetIntoMap != (uint32_t)-1);
        assert(offsetIntoMap >= 0);
        assert(offsetIntoMap < _localNodeToMaster.size());
        return _localNodeToMaster[offsetIntoMap];
      }
    } else {
      // stage 0 = this function shouldn't be called
      GALOIS_DIE("Master setup incomplete");
      return (uint32_t)-1;
    }
  }

  /**
   * Given gid to master mapping info, save it into a local map.
   *
   * @param gid2offsets Map a GID to an offset into a vector containing master
   * mapping information
   * @param localNodeToMaster Vector that represents the master mapping of
   * local nodes
   * @param nodeOffset First GID of nodes read by this host
   */
  void saveGID2HostInfo(std::unordered_map<uint64_t, uint32_t>& gid2offsets,
                        std::vector<uint32_t>& localNodeToMaster,
                        uint64_t nodeOffset) {
    #ifndef NDEBUG
    size_t originalSize = _gid2masters.size();
    #endif

    for (auto i = gid2offsets.begin(); i != gid2offsets.end(); i++) {
      assert(i->second < localNodeToMaster.size());
      galois::gDebug("Map ", i->first, " to ", localNodeToMaster[i->second]);
      _gid2masters[i->first] = localNodeToMaster[i->second];
    }
    assert(_gid2masters.size() == (originalSize + gid2offsets.size()));
    // get memory back
    gid2offsets.clear();

    size_t myLocalNodes = _gid2host[_hostID].second - _gid2host[_hostID].first;
    assert((myLocalNodes + _gid2masters.size() - originalSize) ==
           localNodeToMaster.size());
    // copy over to this structure
    _localNodeToMaster = std::move(localNodeToMaster);
    assert(myLocalNodes <= _localNodeToMaster.size());

    // resize to fit only this host's read nodes
    _localNodeToMaster.resize(myLocalNodes);
    _nodeOffset = nodeOffset;

    // stage 1 setup complete
    _status = 1;
  }


  // below all unused if not assigning masters in default manner
  bool masterAssignPhase() const { return true; }
  void enterStage2() { _status = 2; }

  template<typename EdgeTy> uint32_t determineMaster(uint32_t,
      galois::graphs::BufferedGraph<EdgeTy>&,
      const std::vector<uint32_t>&,
      std::unordered_map<uint64_t, uint32_t>&,
      const std::vector<uint64_t>&,
      std::vector<galois::CopyableAtomic<uint64_t>>&,
      const std::vector<uint64_t>&,
      std::vector<galois::CopyableAtomic<uint64_t>>&) {
    return 0;
  }

  /**
   * Add a new master mapping to the local map: needs to be in stage 1
   *
   * @param gid GID to map; should not be a GID read by this host (won't
   * cause problems, but would just be a waste of compute resouces)
   * @param mappedMaster master to map a GID to
   * @returns true if new mapping added; false if already existed in map
   */
  bool addMasterMapping(uint32_t gid, uint32_t mappedMaster) {
    assert(mappedMaster >= 0 && mappedMaster < _numHosts);
    if (_status <= 1) {
      auto offsetIntoMapIter = _gid2masters.find(gid);
      if (offsetIntoMapIter == _gid2masters.end()) {
        // NOT FOUND
        galois::gDebug("[", _hostID, "] ", gid, " not found; mapping!");
        _gid2masters[gid] = mappedMaster;
        return true;
      } else {
        // already mapped
        galois::gDebug("[", _hostID, "] ", gid, " already mapped with master ",
                       offsetIntoMapIter->second, "!");
        assert(offsetIntoMapIter->second == mappedMaster);
        return false;
      }
    } else {
      GALOIS_DIE("add master mapping should only be called in stage 0/1");
      return false;
    }
  }
};

} // end namespace graphs
} // end namespace galois

#endif