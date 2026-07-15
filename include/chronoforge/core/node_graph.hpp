#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace chronoforge {

enum class NodeKind {
    Input,
    SpaceTimeTranspose,
    LumaTimeShift,
    RadialChronoFunnel,
    TemporalPixelSort,
    Tensor3dRotation,
    SpectralFftSwap,
    Output,
};

struct GraphNode {
    std::string id;
    std::string label;
    NodeKind kind;
};

class NodeGraph {
public:
    void add_node(GraphNode node);
    void connect(const std::string& source_id, const std::string& destination_id);
    void disconnect(const std::string& source_id, const std::string& destination_id);

    [[nodiscard]] std::vector<std::string> topological_order() const;
    [[nodiscard]] const std::vector<std::string>& inputs_for(const std::string& node_id) const;

private:
    [[nodiscard]] bool path_exists(const std::string& start, const std::string& target) const;
    void require_node(const std::string& node_id) const;

    std::unordered_map<std::string, GraphNode> nodes_;
    std::unordered_map<std::string, std::vector<std::string>> inputs_;
};

}  // namespace chronoforge
