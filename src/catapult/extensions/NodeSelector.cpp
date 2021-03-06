/**
*** Copyright (c) 2016-present,
*** Jaguar0625, gimre, BloodyRookie, Tech Bureau, Corp. All rights reserved.
***
*** This file is part of Catapult.
***
*** Catapult is free software: you can redistribute it and/or modify
*** it under the terms of the GNU Lesser General Public License as published by
*** the Free Software Foundation, either version 3 of the License, or
*** (at your option) any later version.
***
*** Catapult is distributed in the hope that it will be useful,
*** but WITHOUT ANY WARRANTY; without even the implied warranty of
*** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*** GNU Lesser General Public License for more details.
***
*** You should have received a copy of the GNU Lesser General Public License
*** along with Catapult. If not, see <http://www.gnu.org/licenses/>.
**/

#include "NodeSelector.h"
#include "catapult/ionet/NodeContainer.h"
#include <random>

namespace catapult { namespace extensions {

	namespace {
		CPP14_CONSTEXPR uint32_t GetWeightMultipler(ionet::NodeSource source) {
			switch (source) {
			case ionet::NodeSource::Dynamic:
				return 1;

			case ionet::NodeSource::Static:
				return 2;

			default:
				return 0;
			};
		}

		using NodeScorePairs = std::vector<std::pair<ionet::Node, uint32_t>>;

		struct ServiceNodesInfo {
			NodeScorePairs Actives; // node to age pairs
			WeightedCandidates Candidates; // candidate nodes with weight
			uint64_t TotalCandidateWeight = 0;
		};

		ServiceNodesInfo FindServiceNodes(
				const ionet::NodeContainerView& nodes,
				ionet::ServiceIdentifier serviceId,
				ionet::NodeRoles requiredRole) {
			ServiceNodesInfo nodesInfo;
			nodes.forEach([serviceId, requiredRole, &nodesInfo](const auto& node, const auto& nodeInfo) {
				auto weightMultiplier = GetWeightMultipler(nodeInfo.source());
				const auto* pConnectionState = nodeInfo.getConnectionState(serviceId);
				if (!pConnectionState || 0 == weightMultiplier || !HasFlag(requiredRole, node.metadata().Roles))
					return;

				// if the node is associated with the current service, mark it as either active or candidate
				if (pConnectionState->Age > 0) {
					nodesInfo.Actives.emplace_back(node, pConnectionState->Age);
				} else {
					nodesInfo.Candidates.emplace_back(node, CalculateWeight(*pConnectionState) * weightMultiplier);
					nodesInfo.TotalCandidateWeight += nodesInfo.Candidates.back().Weight;
				}
			});

			return nodesInfo;
		}

		utils::KeySet FindRemoveCandidates(const NodeScorePairs& nodePairs, uint32_t maxConnections, uint32_t maxConnectionAge) {
			utils::KeySet removeCandidates;

			// 1. if fewer than `maxConnections` active connections, no nodes are removed
			// 2. if removal takes place, leave `maxConnections - 1` connections (this prevents all connections from being closed at once)
			// 3. only remove nodes with sufficient age
			auto maxNodesToRemove = nodePairs.size() >= maxConnections ? nodePairs.size() - maxConnections + 1 : 0;
			for (const auto& pair : nodePairs) {
				if (removeCandidates.size() == maxNodesToRemove)
					break;

				if (pair.second >= maxConnectionAge)
					removeCandidates.emplace(pair.first.identityKey());
			}

			return removeCandidates;
		}

		size_t FindCandidateIndex(const WeightedCandidates& candidates, const std::vector<bool>& usedNodeFlags, uint64_t selectedWeight) {
			uint64_t cumulativeWeight = 0;
			auto lastUnusedNodeIndex = 0u;
			for (auto i = 0u; i < candidates.size(); ++i) {
				if (usedNodeFlags[i])
					continue;

				lastUnusedNodeIndex = i;
				cumulativeWeight += candidates[i].Weight;
				if (cumulativeWeight >= selectedWeight)
					return i;
			}

			return lastUnusedNodeIndex;
		}
	}

	uint32_t CalculateWeight(const ionet::ConnectionState& connectionState) {
		// return a weight in range of 1..10'000
		if (0 == connectionState.NumAttempts)
			return 5'000;

		if (0 == connectionState.NumFailures)
			return 10'000;

		auto weight = connectionState.NumSuccesses * 10'000 / connectionState.NumAttempts;
		return std::max<uint32_t>({ 1, weight, 1'000 / connectionState.NumFailures });
	}

	ionet::NodeSet SelectCandidatesBasedOnWeight(
			const WeightedCandidates& candidates,
			uint64_t totalCandidateWeight,
			size_t maxCandidates) {
		ionet::NodeSet addCandidates;

		// if the number of nodes does not exceed `maxCandidates`, select all
		if (candidates.size() <= maxCandidates) {
			for (const auto& candidate : candidates)
				addCandidates.emplace(candidate.Node);

			return addCandidates;
		}

		std::mt19937 generator((std::random_device()()));
		auto generatorRange = generator.max() - generator.min();
		std::vector<bool> usedNodeFlags(candidates.size(), false);
		for (auto i = 0u; i < maxCandidates; ++i) {
			// cast value to uint64_t to prevent multiplcation overflow below
			auto randomValue = static_cast<uint64_t>(generator());
			auto randomWeight = static_cast<uint32_t>(randomValue * totalCandidateWeight / generatorRange);
			auto index = FindCandidateIndex(candidates, usedNodeFlags, randomWeight);
			auto& candidate = candidates[index];

			addCandidates.emplace(candidate.Node);
			totalCandidateWeight -= candidate.Weight;
			usedNodeFlags[index] = true;
		}

		return addCandidates;
	}

	NodeSelectionResult SelectNodes(const ionet::NodeContainer& nodes, const NodeSelectionConfiguration& config) {
		// 1. find compatible (service and role) nodes
		NodeSelectionResult result;
		auto nodesInfo = FindServiceNodes(nodes.view(), config.ServiceId, config.RequiredRole);

		// 2. find removal candidates
		auto numActiveNodes = nodesInfo.Actives.size();
		result.RemoveCandidates = FindRemoveCandidates(nodesInfo.Actives, config.MaxConnections, config.MaxConnectionAge);
		numActiveNodes -= result.RemoveCandidates.size();

		// 3. find add candidates
		if (numActiveNodes < config.MaxConnections) {
			auto maxAddCandidates = config.MaxConnections - numActiveNodes;
			result.AddCandidates = SelectCandidatesBasedOnWeight(nodesInfo.Candidates, nodesInfo.TotalCandidateWeight, maxAddCandidates);
			numActiveNodes += result.AddCandidates.size();
		}

		// 4. preserve max connections if possible (removal assumes that at least one inactive node can be activated)
		if (!result.RemoveCandidates.empty() && numActiveNodes < config.MaxConnections)
			result.RemoveCandidates.erase(result.RemoveCandidates.begin());

		return result;
	}

	utils::KeySet SelectNodesForRemoval(const ionet::NodeContainer& nodes, const NodeAgingConfiguration& config) {
		// 1. find compatible (service) nodes; always match all roles
		NodeSelectionResult result;
		auto nodesInfo = FindServiceNodes(nodes.view(), config.ServiceId, ionet::NodeRoles::None);

		// 2. find removal candidates
		// a. allow at most 1/4 of active nodes to be disconnected
		// b. add one to adjust for FindRemoveCandidates behavior of assuming that at least one inactive node can be activated
		// c. always retain at least one connection
		auto adjustedMaxConnections = std::max<uint32_t>(1, config.MaxConnections * 3 / 4) + 1;
		return FindRemoveCandidates(nodesInfo.Actives, adjustedMaxConnections, config.MaxConnectionAge);
	}
}}
