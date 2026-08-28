#include <astra/graph.hpp>

#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <vector>

namespace astra {

namespace {

bool find_cycle_dfs(NodeId u,
                    std::vector<int>& state,
                    std::vector<NodeId>& path,
                    std::vector<NodeId>& witness,
                    const std::vector<std::vector<NodeId>>& adj) {
    state[u.value()] = 1;
    path.push_back(u);

    for (NodeId v : adj[u.value()]) {
        if (state[v.value()] == 1) {
            // 发现环：截取从 v 到当前 u 的路径，并闭合到 v
            auto it = std::find(path.begin(), path.end(), v);
            if (it != path.end()) {
                witness.assign(it, path.end());
                witness.push_back(v);
                return true;
            }
        }
        if (state[v.value()] == 0) {
            if (find_cycle_dfs(v, state, path, witness, adj)) {
                return true;
            }
        }
    }

    path.pop_back();
    state[u.value()] = 2;
    return false;
}

}  // namespace

FrozenTaskGraph TaskGraph::freeze() && {
    const std::size_t n = nodes_.size();

    // 1. ForeignNode 校验（R-069 / D-105）
    for (const auto& edge : edges_) {
        if (!edge.from.valid() || edge.from.value() == 0 || edge.from.value() > n ||
            !edge.to.valid() || edge.to.value() == 0 || edge.to.value() > n) {
            throw graph_validation_error(GraphValidationError::ForeignNode);
        }
    }

    // 2. SelfEdge 校验（R-069 / D-105）
    for (const auto& edge : edges_) {
        if (edge.from == edge.to) {
            throw graph_validation_error(GraphValidationError::SelfEdge);
        }
    }

    // 3. DuplicateEdge 校验（R-069 / D-105）
    std::set<std::pair<std::uint64_t, std::uint64_t>> seen_edges;
    for (const auto& edge : edges_) {
        auto key = std::make_pair(edge.from.value(), edge.to.value());
        if (!seen_edges.insert(key).second) {
            throw graph_validation_error(GraphValidationError::DuplicateEdge);
        }
    }

    // 4. Cycle 校验与确定性 witness 提取（R-069 / D-105）
    if (n > 0 && !edges_.empty()) {
        std::vector<std::vector<NodeId>> adj(n + 1);
        std::vector<std::size_t> in_degree(n + 1, 0);

        for (const auto& edge : edges_) {
            adj[edge.from.value()].push_back(edge.to);
            in_degree[edge.to.value()]++;
        }

        // 邻接表按 NodeId 升序排序以保证确定性
        for (std::size_t i = 1; i <= n; ++i) {
            std::sort(adj[i].begin(), adj[i].end());
        }

        // Kahn 算法检测有向无环图
        std::priority_queue<std::uint64_t, std::vector<std::uint64_t>, std::greater<std::uint64_t>> zero_in_degree;
        for (std::size_t i = 1; i <= n; ++i) {
            if (in_degree[i] == 0) {
                zero_in_degree.push(i);
            }
        }

        std::size_t visited_count = 0;
        std::vector<std::size_t> temp_in_degree = in_degree;
        while (!zero_in_degree.empty()) {
            std::uint64_t u = zero_in_degree.top();
            zero_in_degree.pop();
            visited_count++;

            for (NodeId v : adj[u]) {
                if (--temp_in_degree[v.value()] == 0) {
                    zero_in_degree.push(v.value());
                }
            }
        }

        if (visited_count < n) {
            // 存在环路，执行确定性 DFS 提取 cycle witness
            std::vector<int> state(n + 1, 0);
            std::vector<NodeId> path;
            std::vector<NodeId> witness;

            for (std::size_t i = 1; i <= n; ++i) {
                if (state[i] == 0) {
                    if (find_cycle_dfs(NodeId{i}, state, path, witness, adj)) {
                        break;
                    }
                }
            }

            throw graph_validation_error(GraphValidationError::Cycle, std::move(witness));
        }
    }

    return FrozenTaskGraph(std::move(nodes_), std::move(edges_));
}

}  // namespace astra
