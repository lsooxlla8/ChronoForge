#include "chronoforge/core/node_graph.hpp"

#include <algorithm>
#include <deque>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace chronoforge {

void NodeGraph::add_node(GraphNode node) {
    if (node.id.empty()) {
        throw std::invalid_argument("A graph node requires a stable id");
    }
    if (nodes_.contains(node.id)) {
        throw std::invalid_argument("A graph node with this id already exists");
    }
    inputs_.try_emplace(node.id);
    nodes_.emplace(node.id, std::move(node));
}

void NodeGraph::connect(const std::string& source_id, const std::string& destination_id) {
    require_node(source_id);
    require_node(destination_id);
    if (source_id == destination_id || path_exists(source_id, destination_id)) {
        throw std::invalid_argument("Connection would introduce a cycle in the dependency graph");
    }
    auto& inputs = inputs_.at(destination_id);
    if (std::find(inputs.begin(), inputs.end(), source_id) == inputs.end()) {
        inputs.push_back(source_id);
    }
}

void NodeGraph::disconnect(const std::string& source_id, const std::string& destination_id) {
    require_node(source_id);
    require_node(destination_id);
    auto& inputs = inputs_.at(destination_id);
    inputs.erase(std::remove(inputs.begin(), inputs.end(), source_id), inputs.end());
}

std::vector<std::string> NodeGraph::topological_order() const {
    std::unordered_map<std::string, std::size_t> in_degree;
    std::unordered_map<std::string, std::vector<std::string>> dependents;
    for (const auto& [id, _] : nodes_) {
        in_degree[id] = inputs_.at(id).size();
        for (const auto& input : inputs_.at(id)) {
            dependents[input].push_back(id);
        }
    }

    std::deque<std::string> ready;
    for (const auto& [id, degree] : in_degree) {
        if (degree == 0) {
            ready.push_back(id);
        }
    }
    std::vector<std::string> result;
    result.reserve(nodes_.size());
    while (!ready.empty()) {
        auto id = std::move(ready.front());
        ready.pop_front();
        result.push_back(id);
        for (const auto& dependent : dependents[id]) {
            if (--in_degree[dependent] == 0) {
                ready.push_back(dependent);
            }
        }
    }
    if (result.size() != nodes_.size()) {
        throw std::logic_error("Dependency graph contains a cycle");
    }
    return result;
}

const std::vector<std::string>& NodeGraph::inputs_for(const std::string& node_id) const {
    require_node(node_id);
    return inputs_.at(node_id);
}

bool NodeGraph::path_exists(const std::string& start, const std::string& target) const {
    std::deque<std::string> pending{start};
    std::unordered_set<std::string> seen;
    while (!pending.empty()) {
        auto current = std::move(pending.front());
        pending.pop_front();
        if (!seen.insert(current).second) {
            continue;
        }
        if (current == target) {
            return true;
        }
        for (const auto& input : inputs_.at(current)) {
            pending.push_back(input);
        }
    }
    return false;
}

void NodeGraph::require_node(const std::string& node_id) const {
    if (!nodes_.contains(node_id)) {
        throw std::out_of_range("Graph node does not exist: " + node_id);
    }
}

}  // namespace chronoforge
